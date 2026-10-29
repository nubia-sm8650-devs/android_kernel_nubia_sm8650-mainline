// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm MSM Camera Subsystem - CSIPHY Module 3phase v1.0, C-PHY mode
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/time64.h>

#include "phy-qcom-mipi-csi2.h"

#define CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(offset, n)	((offset) + 0x4 * (n))
#define CSIPHY_3PH_CMN_CSI_COMMON_CTRL6_COMMON_PWRDN_B	BIT(0)

/*
 * 3 phase CSI has 19 common status regs with only 0-10 being used
 * and 11-18 being reserved.
 */
#define CSI_COMMON_STATUS_NUM				11
/*
 * There are a number of common control registers. The offset to clear the
 * CSIPHY IRQ status starts @ 22, so CSI_COMMON_STATUS0 is cleared via
 * CSI_COMMON_CONTROL22, STATUS1 via CONTROL23 and so on.
 */
#define CSI_CTRL_STATUS_INDEX				22

/*
 * C-PHY common control values, per the CSIPHY v1.3.0 3-phase C-PHY
 * programming sequence (differ from the D-PHY values for the same registers).
 */
#define CSIPHY_3PH_CMN_CSI_COMMON_CTRL7_CPHY		0x5a
#define CSIPHY_3PH_CMN_CSI_COMMON_CTRL0_CPHY		0x0e

#define CSIPHY_DEFAULT_PARAMS				0
#define CSIPHY_SETTLE_CNT_LOWER_BYTE			2
#define CSIPHY_SETTLE_CNT_HIGHER_BYTE			3
#define CSIPHY_SHORT_CHANNEL_PARAMS			5
#define CSIPHY_STANDARD_CHANNEL_PARAMS			6

static inline const struct mipi_csi2phy_device_regs *
csi2phy_dev_to_cphy_regs(struct mipi_csi2phy_device *csi2phy)
{
	return csi2phy->soc_cfg->reg_info_cphy;
}

/*
 * phy_qcom_mipi_csi2_cphy_settle_cnt_calc - Calculate settle count value
 *
 * Helper function to calculate settle count value. This is based on the
 * CSI2 T_hs_settle parameter which in turn is calculated based on the CSI2
 * transmitter link frequency. The calculation is identical to the D-PHY
 * one; the CSIPHY settle-count timer is not phase-mode specific.
 *
 * Return settle count value or 0 if the CSI2 link frequency is not available.
 */
static u8 phy_qcom_mipi_csi2_cphy_settle_cnt_calc(s64 link_freq, u32 timer_clk_rate)
{
	u32 t_hs_prepare_max_ps;
	u32 timer_period_ps;
	u32 t_hs_settle_ps;
	u8 settle_cnt;
	u32 ui_ps;

	if (link_freq <= 0)
		return 0;

	ui_ps = div_u64(PSEC_PER_SEC, link_freq);
	ui_ps /= 2;
	t_hs_prepare_max_ps = 85000 + 6 * ui_ps;
	t_hs_settle_ps = t_hs_prepare_max_ps;

	timer_period_ps = div_u64(PSEC_PER_SEC, timer_clk_rate);
	settle_cnt = t_hs_settle_ps / timer_period_ps - 6;

	return settle_cnt;
}

/*
 * phy_qcom_mipi_csi2_cphy_lane_mask - Compute the C-PHY lane enable mask
 *
 * Unlike D-PHY (which sets the CTRL5 clock-lane enable bit and uses even
 * bit positions), C-PHY has no separate clock lane and enables each active
 * trio at an odd bit position (data lane logical position * 2 + 1).
 */
static u8 phy_qcom_mipi_csi2_cphy_lane_mask(struct mipi_csi2phy_stream_cfg *cfg)
{
	struct mipi_csi2phy_lanes_cfg *lane_cfg = &cfg->lane_cfg;
	u8 lane_mask = 0;
	int i;

	for (i = 0; i < cfg->num_data_lanes; i++)
		lane_mask |= BIT((lane_cfg->data[i].pos * 2) + 1);

	return lane_mask;
}

/*
 * phy_qcom_mipi_csi2_cphy_pick_datarate - select the bandwidth-tier override
 *
 * Selects the first entry whose bandwidth is >= link_freq, falling back to
 * the highest entry if none qualifies, or the first entry if link_freq is
 * not yet known (0). Returns NULL if @regs has no datarate_regs table.
 */
static const struct mipi_csi2phy_datarate_regs *
phy_qcom_mipi_csi2_cphy_pick_datarate(const struct mipi_csi2phy_device_regs *regs,
				      s64 link_freq)
{
	size_t idx;

	if (!regs->datarate_regs || !regs->num_datarate_regs)
		return NULL;

	if (!link_freq)
		return &regs->datarate_regs[0];

	for (idx = 0; idx < regs->num_datarate_regs; idx++) {
		if (regs->datarate_regs[idx].bandwidth >= link_freq)
			return &regs->datarate_regs[idx];
	}

	/* No tier reaches link_freq: fall back to the highest one. */
	return &regs->datarate_regs[regs->num_datarate_regs - 1];
}

static void
phy_qcom_mipi_csi2_cphy_write_regs(struct mipi_csi2phy_device *csi2phy,
				   const struct mipi_csi2phy_lane_regs *r,
				   size_t count, u16 settle_cnt)
{
	u32 val;
	size_t i;

	for (i = 0; i < count; i++, r++) {
		switch (r->param_type) {
		case CSIPHY_SETTLE_CNT_LOWER_BYTE:
			val = settle_cnt & 0xff;
			break;
		case CSIPHY_SETTLE_CNT_HIGHER_BYTE:
			val = (settle_cnt >> 8) & 0xff;
			break;
		case CSIPHY_SHORT_CHANNEL_PARAMS:
			if (csi2phy->standard_channel)
				continue;
			val = r->reg_data;
			break;
		case CSIPHY_STANDARD_CHANNEL_PARAMS:
			if (!csi2phy->standard_channel)
				continue;
			val = r->reg_data;
			break;
		default:
			val = r->reg_data;
			break;
		}
		writel(val, csi2phy->base + r->reg_addr);
		if (r->delay_us)
			udelay(r->delay_us);
	}
}

static int phy_qcom_mipi_csi2_cphy_lanes_enable(struct mipi_csi2phy_device *csi2phy,
						struct mipi_csi2phy_stream_cfg *cfg)
{
	const struct mipi_csi2phy_device_regs *regs = csi2phy_dev_to_cphy_regs(csi2phy);
	const struct mipi_csi2phy_datarate_regs *tier;
	u8 settle_cnt;
	u8 val;
	int i;

	settle_cnt = phy_qcom_mipi_csi2_cphy_settle_cnt_calc(cfg->link_freq,
							     csi2phy->timer_clk_rate);

	/* Enable the active C-PHY trios (odd bit positions, no clock lane). */
	val = phy_qcom_mipi_csi2_cphy_lane_mask(cfg);
	writel(val, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 5));

	val = CSIPHY_3PH_CMN_CSI_COMMON_CTRL6_COMMON_PWRDN_B;
	writel(val, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 6));

	val = CSIPHY_3PH_CMN_CSI_COMMON_CTRL7_CPHY;
	writel(val, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 7));

	val = CSIPHY_3PH_CMN_CSI_COMMON_CTRL0_CPHY;
	writel(val, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 0));

	/* Base C-PHY lane_regs init sequence. */
	phy_qcom_mipi_csi2_cphy_write_regs(csi2phy, regs->init_seq,
					   regs->lane_array_size, settle_cnt);

	/* Bandwidth-tier overrides on top, if this soc_cfg has any. */
	tier = phy_qcom_mipi_csi2_cphy_pick_datarate(regs, cfg->link_freq);
	if (tier)
		phy_qcom_mipi_csi2_cphy_write_regs(csi2phy, tier->reg_array,
						   tier->reg_array_size, settle_cnt);

	/* IRQ_MASK registers - disable all interrupts */
	for (i = CSI_COMMON_STATUS_NUM; i < CSI_CTRL_STATUS_INDEX; i++) {
		writel(0, csi2phy->base +
		       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, i));
	}

	return 0;
}

static void
phy_qcom_mipi_csi2_cphy_lanes_disable(struct mipi_csi2phy_device *csi2phy,
				      struct mipi_csi2phy_stream_cfg *cfg)
{
	const struct mipi_csi2phy_device_regs *regs = csi2phy_dev_to_cphy_regs(csi2phy);

	writel(0, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 5));

	writel(0, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 6));
}

const struct mipi_csi2phy_hw_ops phy_qcom_mipi_csi2_ops_3ph_cphy = {
	.hw_version_read = phy_qcom_mipi_csi2_hw_version_read,
	.reset = phy_qcom_mipi_csi2_reset,
	.lanes_enable = phy_qcom_mipi_csi2_cphy_lanes_enable,
	.lanes_disable = phy_qcom_mipi_csi2_cphy_lanes_disable,
};

/*
 * sa8775p C-PHY (3-phase) register tables.
 *
 * Ported verbatim from the legacy in-tree CAMSS C-PHY driver
 * (drivers/media/platform/qcom/camss/camss-csiphy-3ph-1-0.c:
 * lane_regs_sa8775p_3ph[] and data_rate_settings_sa8775p_3ph[]). The
 * per-lane-block macro addresses (CSIPHY_LNx_CSI_3PH_CTRLn_ADDR(n) =
 * base + n * 4, base 0x0100/0x0300/0x0500/0x0900/0x0a00/0x0b00 for
 * LN1/3/5/9/10/11) are expanded to the literal flat offsets used by
 * this driver's mipi_csi2phy_lane_regs format.
 */
static const struct mipi_csi2phy_lane_regs lane_regs_sa8775p_cphy[] = {
	{.reg_addr = 0x0168, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x016c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x015c, .reg_data = 0xc8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0104, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x010c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0108, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0114, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0150, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0188, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x018c, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0190, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0118, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x011c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0120, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0124, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0128, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x012c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0144, .reg_data = 0xb2, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0160, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x01cc, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0164, .reg_data = 0x33, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x01dc, .reg_data = 0x50, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0368, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x036c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x035c, .reg_data = 0xc8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0304, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x030c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0308, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0314, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0350, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0388, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x038c, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0390, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0318, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x031c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0320, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0324, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0328, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x032c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0344, .reg_data = 0xb2, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0360, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x03cc, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0364, .reg_data = 0x33, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x03dc, .reg_data = 0x50, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0568, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x056c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x055c, .reg_data = 0xc8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0504, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x050c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0508, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0514, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0550, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0588, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x058c, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0590, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0518, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x051c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0520, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0524, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0528, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x052c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0544, .reg_data = 0xb2, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0560, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x05cc, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0564, .reg_data = 0x33, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x05dc, .reg_data = 0x50, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0984, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0988, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0980, .reg_data = 0x61, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x09b0, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x09b4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a84, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a80, .reg_data = 0x61, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab0, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b84, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b80, .reg_data = 0x61, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb0, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_lane_regs datarate_sa8775p_cphy_1p5gsps[] = {
	{.reg_addr = 0x015c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0168, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x016c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x010c, .reg_data = 0x24, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x035c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0368, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x036c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x030c, .reg_data = 0x24, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x055c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0568, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x056c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x050c, .reg_data = 0x24, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x09b4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_lane_regs datarate_sa8775p_cphy_1p7gsps[] = {
	{.reg_addr = 0x015c, .reg_data = 0x56, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0168, .reg_data = 0xae, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x016c, .reg_data = 0x65, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x010c, .reg_data = 0x12, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x035c, .reg_data = 0x56, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0368, .reg_data = 0xae, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x036c, .reg_data = 0x65, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x030c, .reg_data = 0x12, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x055c, .reg_data = 0x56, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0568, .reg_data = 0xae, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x056c, .reg_data = 0x65, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x050c, .reg_data = 0x12, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x09b4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_lane_regs datarate_sa8775p_cphy_2p5gsps[] = {
	{.reg_addr = 0x0168, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x015c, .reg_data = 0xc8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x016c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x010c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0368, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x035c, .reg_data = 0xc8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x036c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x030c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0568, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x055c, .reg_data = 0xc8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x056c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x050c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x09b4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0988, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb4, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_lane_regs datarate_sa8775p_cphy_3p5gsps[] = {
	{.reg_addr = 0x0168, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x015c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x016c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x010c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0368, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x035c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x036c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x030c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0568, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x055c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x056c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x050c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x09b4, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0988, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab4, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb4, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_lane_regs datarate_sa8775p_cphy_4p5gsps[] = {
	{.reg_addr = 0x0168, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x015c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x016c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x010c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0368, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x035c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x036c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x030c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0568, .reg_data = 0x80, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x055c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x056c, .reg_data = 0x25, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x050c, .reg_data = 0x08, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x09b4, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0988, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab4, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb4, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_datarate_regs datarate_regs_sa8775p_cphy[] = {
	{
		.bandwidth = 1500000000,
		.reg_array = datarate_sa8775p_cphy_1p5gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sa8775p_cphy_1p5gsps),
	},
	{
		.bandwidth = 1700000000,
		.reg_array = datarate_sa8775p_cphy_1p7gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sa8775p_cphy_1p7gsps),
	},
	{
		.bandwidth = 2500000000,
		.reg_array = datarate_sa8775p_cphy_2p5gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sa8775p_cphy_2p5gsps),
	},
	{
		.bandwidth = 3500000000,
		.reg_array = datarate_sa8775p_cphy_3p5gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sa8775p_cphy_3p5gsps),
	},
	{
		.bandwidth = 4500000000,
		.reg_array = datarate_sa8775p_cphy_4p5gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sa8775p_cphy_4p5gsps),
	},
};

const struct mipi_csi2phy_device_regs mipi_csi2_3ph_cphy_regs_sa8775p = {
	.init_seq = lane_regs_sa8775p_cphy,
	.lane_array_size = ARRAY_SIZE(lane_regs_sa8775p_cphy),
	.common_regs_offset = 0x800,
	.datarate_regs = datarate_regs_sa8775p_cphy,
	.num_datarate_regs = ARRAY_SIZE(datarate_regs_sa8775p_cphy),
};

static const struct mipi_csi2phy_lane_regs lane_regs_sc7280_cphy[] = {
	{.reg_addr = 0x0990, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0994, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0998, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0990, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0994, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0998, .reg_data = 0x1a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x098c, .reg_data = 0xaf, .delay_us = 0x01,
	 .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x015c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0168, .reg_data = 0xa0, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0104, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x010c, .reg_data = 0x12, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0108, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0114, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0150, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0188, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x018c, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0190, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0118, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x011c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0120, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0124, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0128, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x012c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0160, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x01cc, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x01dc, .reg_data = 0x50, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0984, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0988, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0980, .reg_data = 0x60, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x09ac, .reg_data = 0x35, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x09b0, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0800, .reg_data = 0x0e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0884, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},

	{.reg_addr = 0x0a90, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a98, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a90, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a98, .reg_data = 0x1a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0xaf, .delay_us = 0x01,
	 .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x035c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0368, .reg_data = 0xa0, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0304, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x030c, .reg_data = 0x12, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0308, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0314, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0350, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0388, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x038c, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0390, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0318, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x031c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0320, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0324, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0328, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x032c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0360, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x03cc, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x03dc, .reg_data = 0x50, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a84, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a80, .reg_data = 0x60, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0aac, .reg_data = 0x35, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ab0, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0800, .reg_data = 0x0e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0884, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},

	{.reg_addr = 0x0b90, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b94, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b98, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b90, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b94, .reg_data = 0x08, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b98, .reg_data = 0x1a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b8c, .reg_data = 0xaf, .delay_us = 0x01,
	 .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x055c, .reg_data = 0x46, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0568, .reg_data = 0xa0, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0504, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x050c, .reg_data = 0x12, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0508, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0514, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0550, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0588, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x058c, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0590, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0518, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x051c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0520, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0524, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0528, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x052c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0560, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x05cc, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x05dc, .reg_data = 0x50, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b84, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b88, .reg_data = 0x05, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b80, .reg_data = 0x60, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bac, .reg_data = 0x35, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0bb0, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0800, .reg_data = 0x0e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0884, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
};

const struct mipi_csi2phy_device_regs mipi_csi2_3ph_cphy_regs_sc7280 = {
	.init_seq = lane_regs_sc7280_cphy,
	.lane_array_size = ARRAY_SIZE(lane_regs_sc7280_cphy),
	.common_regs_offset = 0x800,
};

/*
 * CSIPHY v2.2.0 C-PHY mode (SM8650).
 *
 * Register tables are ported from the downstream camera-kernel
 * cam_csiphy_2_2_0_hwreg.h data_rate_settings_2_2_0. Each tier array carries
 * both the short and standard channel variants of ctrl27 and ctrl35; the
 * entries whose channel does not match qcom,channel-type are skipped. The
 * 10 us delay between the AFE and the settle write groups rides on the
 * ctrl29 entry of the last trio.
 */
#define CSIPHY_2_2_0_CTRL7_CPHY				0x7a
#define CSIPHY_2_2_0_CTRL0_CPHY				0x0e

static u16 phy_qcom_mipi_csi2_cphy_2_2_0_settle_cnt_calc(s64 link_freq,
							 u32 timer_clk_rate)
{
	u64 symbol_rate = link_freq * 2;
	u64 t_settle;
	u64 timer_period;

	if (!symbol_rate || !timer_clk_rate)
		return 0;

	/* T_settle = 45 ns + 105 UI */
	t_settle = 45000000 + div64_u64(105000000000000000ULL, symbol_rate);
	timer_period = div_u64(1000000000000000ULL, timer_clk_rate);

	return div64_u64(t_settle, timer_period);
}

static int
phy_qcom_mipi_csi2_cphy_2_2_0_lanes_enable(struct mipi_csi2phy_device *csi2phy,
					   struct mipi_csi2phy_stream_cfg *cfg)
{
	const struct mipi_csi2phy_device_regs *regs = csi2phy_dev_to_cphy_regs(csi2phy);
	const struct mipi_csi2phy_datarate_regs *tier;
	u16 settle_cnt;
	u8 val;
	int i;

	if (cfg->link_freq <= 0)
		return -EINVAL;

	settle_cnt = phy_qcom_mipi_csi2_cphy_2_2_0_settle_cnt_calc(cfg->link_freq,
								  csi2phy->timer_clk_rate);
	if (!settle_cnt)
		return -ENODEV;

	val = phy_qcom_mipi_csi2_cphy_lane_mask(cfg);
	writel(val, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 5));

	val = CSIPHY_3PH_CMN_CSI_COMMON_CTRL6_COMMON_PWRDN_B;
	writel(val, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 6));

	writel(0x00, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 33));
	writel(0x00, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 35));
	udelay(1);
	writel(CSIPHY_2_2_0_CTRL7_CPHY, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 7));

	tier = phy_qcom_mipi_csi2_cphy_pick_datarate(regs, cfg->link_freq);
	if (!tier)
		return -EINVAL;
	phy_qcom_mipi_csi2_cphy_write_regs(csi2phy, tier->reg_array,
					   tier->reg_array_size, settle_cnt);

	phy_qcom_mipi_csi2_cphy_write_regs(csi2phy, regs->init_seq,
					   regs->lane_array_size, settle_cnt);

	writel(CSIPHY_2_2_0_CTRL0_CPHY, csi2phy->base +
	       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, 0));
	fsleep(3048);

	for (i = CSI_COMMON_STATUS_NUM; i < CSI_CTRL_STATUS_INDEX; i++) {
		writel(0, csi2phy->base +
		       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->common_regs_offset, i));
	}

	return 0;
}

const struct mipi_csi2phy_hw_ops phy_qcom_mipi_csi2_ops_3ph_cphy_2_2_0 = {
	.hw_version_read = phy_qcom_mipi_csi2_hw_version_read,
	.reset = phy_qcom_mipi_csi2_reset,
	.lanes_enable = phy_qcom_mipi_csi2_cphy_2_2_0_lanes_enable,
	.lanes_disable = phy_qcom_mipi_csi2_cphy_lanes_disable,
};

static const struct mipi_csi2phy_lane_regs lane_regs_sm8650_cphy[] = {
	{.reg_addr = 0x02f4, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x02f8, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x02fc, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x02f0, .reg_data = 0xef, .delay_us = 0xd3, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06f4, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06f8, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06fc, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06f0, .reg_data = 0xef, .delay_us = 0xd3, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0af4, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0af8, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0afc, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0af0, .reg_data = 0xef, .delay_us = 0xd3, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0204, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x02e4, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x02e8, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x02ec, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0218, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x021c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0220, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0224, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0228, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x022c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0264, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0244, .reg_data = 0xb2, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0310, .reg_data = 0x35, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x02bc, .reg_data = 0xd0, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0254, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0240, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0260, .reg_data = 0xa8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0284, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0290, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0604, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06e4, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06e8, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06ec, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0618, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x061c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0620, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0624, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0628, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x062c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0664, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0644, .reg_data = 0xb2, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0710, .reg_data = 0x35, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x06bc, .reg_data = 0xd0, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0654, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0640, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0660, .reg_data = 0xa8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0684, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0690, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a04, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ae4, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0ae8, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0aec, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a18, .reg_data = 0x3e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a1c, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a20, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a24, .reg_data = 0x7f, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a28, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a2c, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a64, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a44, .reg_data = 0xb2, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0b10, .reg_data = 0x35, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0abc, .reg_data = 0xd0, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a54, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a40, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a60, .reg_data = 0xa8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a84, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a90, .reg_data = 0x02, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_80Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x1f, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x02, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x6b, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x1f, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x02, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x6b, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x1f, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x02, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x6b, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_100Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0xb6, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x01, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x6b, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0xb6, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x01, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x6b, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0xb6, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x01, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x6b, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_200Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0xe4, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x33, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0xe4, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x33, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0xe4, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x33, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_300Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x9e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x9e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x9e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_350Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x8a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x8a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x8a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_400Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x7b, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x7b, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x7b, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_500Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x66, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x66, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x66, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_600Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x58, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x58, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x58, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_700Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x4e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x4e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x4e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_800Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x46, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x46, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x46, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_900Msps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x40, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x40, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x40, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_1p0Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x58, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x3c, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x3c, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x3c, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_1p2Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x45, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x45, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x45, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x35, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x35, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x35, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_1p5Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x45, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x45, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x45, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x2e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x2e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x2e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_1p7Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x2e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x2e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x2e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x2a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x2a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x2a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x09, .param_type = CSIPHY_DEFAULT_PARAMS},
};

static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_2p0Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x2e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x2e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x2e, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x27, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x27, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x27, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_2p1Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x26, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x26, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x26, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_2p35Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x23, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x23, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x23, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_2p5Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x05, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x22, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x22, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x22, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_2p6Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x22, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x22, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x22, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_2p8Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x21, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x21, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x21, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_3p0Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x17, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x20, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x20, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x20, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_3p3Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x10, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x10, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x10, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x1e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x1e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x1e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_3p5Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x10, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x10, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x10, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x1e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x1e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x1e, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_4p0Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x0c, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x0c, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x0c, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x0c, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x0c, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x0c, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x1c, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x1c, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x1c, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_4p5Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x06, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x2d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x30, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x1b, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x1b, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x1b, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_5p0Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x82, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x82, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x82, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x82, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x82, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x82, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x1a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x1a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x1a, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_5p5Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x80, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x81, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x80, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x81, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x80, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x81, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x19, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x19, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x19, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_6p0Gsps[] = {
	{.reg_addr = 0x0268, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x80, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0x81, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x80, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0x81, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0x41, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x80, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0x81, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x03, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3f, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x33, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x37, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x00, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x19, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x19, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x19, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_lane_regs datarate_sm8650_cphy_2p0Gsps_z60u_tele[] = {
	{.reg_addr = 0x0268, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0294, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0278, .reg_data = 0xa8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0288, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x026c, .reg_data = 0x3d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x28, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x028c, .reg_data = 0x28, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0270, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0274, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0668, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0694, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0678, .reg_data = 0xa8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0688, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x066c, .reg_data = 0x3d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x28, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x068c, .reg_data = 0x28, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0670, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0674, .reg_data = 0x03, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a68, .reg_data = 0xf1, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a94, .reg_data = 0x01, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a78, .reg_data = 0xa8, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a88, .reg_data = 0x20, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a6c, .reg_data = 0x3d, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x28, .param_type = CSIPHY_SHORT_CHANNEL_PARAMS},
	{.reg_addr = 0x0a8c, .reg_data = 0x28, .param_type = CSIPHY_STANDARD_CHANNEL_PARAMS},
	{.reg_addr = 0x0a70, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a74, .reg_data = 0x03, .delay_us = 0x0a, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x020c, .reg_data = 0x27, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0208, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0210, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0214, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x060c, .reg_data = 0x27, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0608, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0610, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0614, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a0c, .reg_data = 0x27, .param_type = CSIPHY_SETTLE_CNT_LOWER_BYTE},
	{.reg_addr = 0x0a08, .reg_data = 0x00, .param_type = CSIPHY_SETTLE_CNT_HIGHER_BYTE},
	{.reg_addr = 0x0a10, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
	{.reg_addr = 0x0a14, .reg_data = 0x00, .param_type = CSIPHY_DEFAULT_PARAMS},
};


static const struct mipi_csi2phy_datarate_regs datarate_regs_sm8650_cphy[] = {
	{
		.bandwidth = 40000000,
		.reg_array = datarate_sm8650_cphy_80Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_80Msps),
	},
	{
		.bandwidth = 50000000,
		.reg_array = datarate_sm8650_cphy_100Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_100Msps),
	},
	{
		.bandwidth = 100000000,
		.reg_array = datarate_sm8650_cphy_200Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_200Msps),
	},
	{
		.bandwidth = 150000000,
		.reg_array = datarate_sm8650_cphy_300Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_300Msps),
	},
	{
		.bandwidth = 175000000,
		.reg_array = datarate_sm8650_cphy_350Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_350Msps),
	},
	{
		.bandwidth = 200000000,
		.reg_array = datarate_sm8650_cphy_400Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_400Msps),
	},
	{
		.bandwidth = 250000000,
		.reg_array = datarate_sm8650_cphy_500Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_500Msps),
	},
	{
		.bandwidth = 300000000,
		.reg_array = datarate_sm8650_cphy_600Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_600Msps),
	},
	{
		.bandwidth = 350000000,
		.reg_array = datarate_sm8650_cphy_700Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_700Msps),
	},
	{
		.bandwidth = 400000000,
		.reg_array = datarate_sm8650_cphy_800Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_800Msps),
	},
	{
		.bandwidth = 450000000,
		.reg_array = datarate_sm8650_cphy_900Msps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_900Msps),
	},
	{
		.bandwidth = 500000000,
		.reg_array = datarate_sm8650_cphy_1p0Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_1p0Gsps),
	},
	{
		.bandwidth = 600000000,
		.reg_array = datarate_sm8650_cphy_1p2Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_1p2Gsps),
	},
	{
		.bandwidth = 750000000,
		.reg_array = datarate_sm8650_cphy_1p5Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_1p5Gsps),
	},
	{
		.bandwidth = 850000000,
		.reg_array = datarate_sm8650_cphy_1p7Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_1p7Gsps),
	},
	{
		.bandwidth = 1000000000,
		.reg_array = datarate_sm8650_cphy_2p0Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_2p0Gsps),
	},
	{
		.bandwidth = 1050000000,
		.reg_array = datarate_sm8650_cphy_2p1Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_2p1Gsps),
	},
	{
		.bandwidth = 1175000000,
		.reg_array = datarate_sm8650_cphy_2p35Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_2p35Gsps),
	},
	{
		.bandwidth = 1250000000,
		.reg_array = datarate_sm8650_cphy_2p5Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_2p5Gsps),
	},
	{
		.bandwidth = 1300000000,
		.reg_array = datarate_sm8650_cphy_2p6Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_2p6Gsps),
	},
	{
		.bandwidth = 1400000000,
		.reg_array = datarate_sm8650_cphy_2p8Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_2p8Gsps),
	},
	{
		.bandwidth = 1500000000,
		.reg_array = datarate_sm8650_cphy_3p0Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_3p0Gsps),
	},
	{
		.bandwidth = 1650000000,
		.reg_array = datarate_sm8650_cphy_3p3Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_3p3Gsps),
	},
	{
		.bandwidth = 1750000000,
		.reg_array = datarate_sm8650_cphy_3p5Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_3p5Gsps),
	},
	{
		.bandwidth = 2000000000,
		.reg_array = datarate_sm8650_cphy_4p0Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_4p0Gsps),
	},
	{
		.bandwidth = 2250000000,
		.reg_array = datarate_sm8650_cphy_4p5Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_4p5Gsps),
	},
	{
		.bandwidth = 2500000000,
		.reg_array = datarate_sm8650_cphy_5p0Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_5p0Gsps),
	},
	{
		.bandwidth = 2750000000,
		.reg_array = datarate_sm8650_cphy_5p5Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_5p5Gsps),
	},
	{
		.bandwidth = 3000000000,
		.reg_array = datarate_sm8650_cphy_6p0Gsps,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_6p0Gsps),
	},
};


const struct mipi_csi2phy_device_regs mipi_csi2_3ph_cphy_regs_sm8650 = {
	.init_seq = lane_regs_sm8650_cphy,
	.lane_array_size = ARRAY_SIZE(lane_regs_sm8650_cphy),
	.common_regs_offset = 0x1000,
	.datarate_regs = datarate_regs_sm8650_cphy,
	.num_datarate_regs = ARRAY_SIZE(datarate_regs_sm8650_cphy),
};

static const struct mipi_csi2phy_datarate_regs datarate_regs_sm8650_cphy_z60u_tele[] = {
	{
		.bandwidth = 1000000000,
		.reg_array = datarate_sm8650_cphy_2p0Gsps_z60u_tele,
		.reg_array_size = ARRAY_SIZE(datarate_sm8650_cphy_2p0Gsps_z60u_tele),
	},
};

const struct mipi_csi2phy_device_regs mipi_csi2_3ph_cphy_regs_sm8650_tele = {
	.init_seq = lane_regs_sm8650_cphy,
	.lane_array_size = ARRAY_SIZE(lane_regs_sm8650_cphy),
	.common_regs_offset = 0x1000,
	.datarate_regs = datarate_regs_sm8650_cphy_z60u_tele,
	.num_datarate_regs = ARRAY_SIZE(datarate_regs_sm8650_cphy_z60u_tele),
};
