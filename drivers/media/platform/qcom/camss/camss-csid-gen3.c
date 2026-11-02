// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm MSM Camera Subsystem - CSID (CSI Decoder) Module
 *
 * Copyright (c) 2024 Qualcomm Technologies, Inc.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/timekeeping.h>

#include "camss.h"
#include "camss-csid.h"
#include "camss-csid-gen3.h"
#include "camss-vfe.h"

#define CSID_IO_PATH_CFG0(csid)		(0x4 * (csid))
#define		OUTPUT_IFE_EN			0x100
#define		INTERNAL_CSID			1

#define CSID_DUAL_CFG0(csid)		(0xC + 0x4 * (csid))
#define		DUAL_CFG0_EN			BIT(0)
#define		DUAL_CFG0_SLAVE			BIT(1)
#define		DUAL_CFG0_SYNC_SEL		8

#define CSID_GLOBAL_CMD			0x8
#define		GLOBAL_CMD_START		BIT(0)

#define CSID_SHDR_MASTER_SLAVE_CFG	0x20
#define		SHDR_EN				BIT(0)
#define		SHDR_MASTER_RDI0		BIT(5)
#define		SHDR_SLAVE_PPP			BIT(20)

#define CSID_RST_CFG			0xC
#define		RST_MODE			BIT(0)
#define		RST_LOCATION			BIT(4)

#define CSID_RST_CMD			0x10
#define		SELECT_HW_RST			BIT(0)
#define		SELECT_IRQ_RST			BIT(2)

#define CSID_IRQ_CMD			0x14
#define		IRQ_CMD_CLEAR			BIT(0)

#define CSID_RUP_AUP_CMD		0x18
#define		CSID_RUP_AUP_RDI(rdi)		((BIT(4) | BIT(20)) << (rdi))

#define CSID_TOP_IRQ_STATUS		0x7C
#define		 TOP_IRQ_STATUS_RESET_DONE	BIT(0)

#define CSID_TOP_IRQ_MASK		0x80
#define CSID_TOP_IRQ_CLEAR		0x84
#define CSID_TOP_IRQ_SET		0x88

#define CSID_CSI2_RX_IRQ_STATUS		0x9C
#define CSID_CSI2_RX_IRQ_MASK		0xA0
#define CSID_CSI2_RX_IRQ_CLEAR		0xA4
#define CSID_CSI2_RX_IRQ_SET		0xA8
#define CSID_CSI2_IPP_IRQ_STATUS	0xAC
#define CSID_CSI2_IPP_IRQ_CLEAR		0xB4
#define		CSID_RUP_AUP_IPP	(BIT(0) | BIT(16))
#define		CSID_RUP_AUP_MUP	BIT(28)

#define IS_CSID_690(csid)	((csid->camss->res->version == CAMSS_8775P) \
				 || (csid->camss->res->version == CAMSS_8300))
#define CSID_BUF_DONE_IRQ_STATUS	0x8C
#define BUF_DONE_IRQ_STATUS_RDI_OFFSET  (csid_is_lite(csid) ? \
						((IS_CSID_690(csid) ? 0 : 1)) : \
						((IS_CSID_690(csid) ? 13 : 14)))
#define CSID_BUF_DONE_IRQ_MASK		0x90
#define CSID_BUF_DONE_IRQ_CLEAR		0x94
#define CSID_BUF_DONE_IRQ_SET		0x98

#define CSID_CSI2_RDIN_IRQ_STATUS(rdi)	(0xEC + 0x10 * (rdi))
#define		RUP_DONE_IRQ_STATUS		BIT(23)

#define CSID_CSI2_RDIN_IRQ_CLEAR(rdi)	(0xF4 + 0x10 * (rdi))
#define CSID_CSI2_RDIN_IRQ_SET(rdi)	(0xF8 + 0x10 * (rdi))

#define CSID_CSI2_RX_CFG0		0x200
#define		CSI2_RX_CFG0_NUM_ACTIVE_LANES	0
#define		CSI2_RX_CFG0_VC_MODE		3
#define		CSI2_RX_CFG0_DL0_INPUT_SEL	4
#define		CSI2_RX_CFG0_PHY_NUM_SEL	20
#define		CSI2_RX_CFG0_PHY_TYPE_SEL	24
#define		CSI2_RX_CFG0_TPG_MUX_EN		BIT(27)
#define		CSI2_RX_CFG0_TPG_MUX_SEL	GENMASK(29, 28)

#define CSID_CSI2_RX_CFG1		0x204
#define		CSI2_RX_CFG1_ECC_CORRECTION_EN	BIT(0)
#define		CSI2_RX_CFG1_VC_MODE		BIT(2)

#define CSID_IPP_CFG0			0x300
#define		IPP_CFG0_TIMESTAMP_EN		BIT(6)
#define		IPP_CFG0_DECODE_FORMAT		12
#define		IPP_CFG0_DT			16
#define		IPP_CFG0_VC			22
#define		IPP_CFG0_DT_ID			27
#define		IPP_CFG0_EN			BIT(31)
#define CSID_IPP_CTRL			0x304
#define		IPP_CTRL_START_CMD		BIT(0)
#define CSID_IPP_CAMIF_FRAME_CFG	0x330
#define		IPP_CAMIF_PIXEL_PATTERN		24

#define CSID_RDI_CFG0(rdi)	(csid_is_lite(csid) && IS_CSID_690(csid) ?\
					(0x300 + 0x100 * (rdi)) :\
					(0x500 + 0x100 * (rdi)))
#define		RDI_CFG0_TIMESTAMP_EN		BIT(6)
#define		RDI_CFG0_TIMESTAMP_STB_SEL	BIT(8)
#define		RDI_CFG0_DECODE_FORMAT		12
#define		RDI_CFG0_DT			16
#define		RDI_CFG0_VC			22
#define		RDI_CFG0_DT_ID			27
#define		RDI_CFG0_EN			BIT(31)

#define CSID_RDI_CTRL(rdi)	(csid_is_lite(csid) && IS_CSID_690(csid) ?\
					(0x304 + 0x100 * (rdi)) :\
					(0x504 + 0x100 * (rdi)))
#define		RDI_CTRL_START_CMD		BIT(0)

#define CSID_RDI_CFG1(rdi)	(csid_is_lite(csid) && IS_CSID_690(csid) ?\
					(0x310 + 0x100 * (rdi)) :\
					(0x510 + 0x100 * (rdi)))
#define		RDI_CFG1_DROP_H_EN		BIT(5)
#define		RDI_CFG1_DROP_V_EN		BIT(6)
#define		RDI_CFG1_CROP_H_EN		BIT(7)
#define		RDI_CFG1_CROP_V_EN		BIT(8)
#define		RDI_CFG1_PIX_STORE		BIT(10)
#define		RDI_CFG1_PACKING_FORMAT_MIPI	BIT(15)

#define CSID_RDI_IRQ_SUBSAMPLE_PATTERN(rdi)	(csid_is_lite(csid) && IS_CSID_690(csid) ?\
							(0x348 + 0x100 * (rdi)) :\
							(0x548 + 0x100 * (rdi)))
#define CSID_RDI_IRQ_SUBSAMPLE_PERIOD(rdi)	(csid_is_lite(csid) && IS_CSID_690(csid) ?\
							(0x34C + 0x100 * (rdi)) :\
							(0x54C + 0x100 * (rdi)))
#define CSI2_RX_CFG0_PHY_SEL_BASE_IDX	1

#define CSID_TOP_IRQ_RX			BIT(2)
#define CSID_TOP_IRQ_BUF_DONE		BIT(13)
#define CSID_TOP_IRQ_ERR		0x180002

#define CSID_RX_IRQ_FATAL		0x19FA800
#define CSID_RX_IRQ_PART_FATAL		0x1000
#define CSID_RX_IRQ_NON_FATAL		0x200000
#define CSID_RX_IRQ_CPHY_EOT		BIT(11)

#define CSI2_RX_CFG1_MISR_EN		BIT(6)
#define CSI2_RX_CFG1_EPD_EN		8
#define CSI2_RX_CFG1_DYN_SWITCH_EN	BIT(10)
#define CSI2_RX_CFG1_RUP_AUP_LATCH_DIS	BIT(13)

#define CSID_PATH_IRQ_EOF		BIT(3)
#define CSID_PATH_IRQ_SOF		BIT(4)
#define CSID_PATH_IRQ_INPUT_SOF		BIT(12)
#define CSID_PATH_IRQ_EPOCH0		BIT(21)
#define CSID_PATH_IRQ_RUP_DONE		BIT(23)
#define CSID_PATH_IRQ_FATAL		0x20186001
#define CSID_PATH_IRQ_NON_FATAL		0x12000004

#define CSID_PATH_CFG0_FRAME_ID_DEC	1
#define CSID_PATH_CFG0_TS_STB_SEL	(2 << 8)
#define CSID_PATH_CTRL_RESUME		BIT(0)
#define CSID_PATH_ERR_REC_DEFAULT	0x9

#define CSID_IPP_CFG1_DROP_H_EN		BIT(10)
#define CSID_IPP_CFG1_DROP_V_EN		BIT(11)
#define CSID_IPP_CFG1_CROP_H_EN		BIT(12)
#define CSID_IPP_CFG1_CROP_V_EN		BIT(13)
#define CSID_IPP_CFG1_PIX_STORE		BIT(14)
#define CSID_IPP_CFG1_EARLY_EOF		BIT(16)
#define CSID_IPP_CFG1_BIN_EN		BIT(18)
#define CSID_IPP_CFG1_BIN_QCFA_EN	BIT(19)
#define CSID_IPP_CFG1_BIN_H_EN		BIT(20)
#define CSID_IPP_CFG1_BIN_V_EN		BIT(21)
#define CSID_PPP_CFG1_BIN_H_EN		BIT(18)

#define CSID_PATH_CFG0_SOF_RETIME_DIS	BIT(5)

#define CSID_PATH_CTRL_START_MODE	GENMASK(3, 2)
#define		START_MODE_GLOBAL		1
#define		START_MODE_MASTER		2
#define		START_MODE_SLAVE		3

#define CSID_RDI_CFG1_PLAIN_ALIGN	11
#define CSID_RDI_CFG1_PLAIN_FMT		12
#define CSID_RDI_CFG1_PACKING		15

#define CSID_PATH_RESET_CFG		RST_MODE
#define CSID_PATH_RESET_CMD		SELECT_HW_RST

#define CSID_QTIMER_NS_NUM		10000
#define CSID_QTIMER_NS_DEN		192

struct csid_gen3_path {
	u32 cfg0;
	u32 ctrl;
	u32 cfg1;
	u32 err_rec;
	u32 epoch;
	u32 ts_lo;
	u32 ts_hi;
	u32 ts_prev_lo;
	u32 ts_prev_hi;
	u32 irq_status;
	u32 irq_mask;
	u32 irq_clear;
	u32 irq_clear_all;
	u32 top_bit;
	u32 rup;
};

#define CSID_GEN3_RDI(n) {					\
	.cfg0 = 0x500 + 0x100 * (n),				\
	.ctrl = 0x504 + 0x100 * (n),				\
	.cfg1 = 0x510 + 0x100 * (n),				\
	.err_rec = 0x514 + 0x100 * (n),				\
	.epoch = 0x52C + 0x100 * (n),				\
	.ts_lo = 0x594 + 0x100 * (n),				\
	.ts_hi = 0x598 + 0x100 * (n),				\
	.ts_prev_lo = 0x59C + 0x100 * (n),			\
	.ts_prev_hi = 0x5A0 + 0x100 * (n),			\
	.irq_status = 0xEC + 0x10 * (n),			\
	.irq_mask = 0xF0 + 0x10 * (n),				\
	.irq_clear = 0xF4 + 0x10 * (n),				\
	.irq_clear_all = 0x7FFF,				\
	.top_bit = BIT(8) << (n),				\
	.rup = 0x100010 << (n),					\
}

static const struct csid_gen3_path csid_gen3_paths[CAMSS_HW_PATH_NUM] = {
	[CAMSS_HW_PATH_IPP] = {
		.cfg0 = 0x300,
		.ctrl = 0x304,
		.cfg1 = 0x310,
		.err_rec = 0x318,
		.epoch = 0x334,
		.ts_lo = 0x398,
		.ts_hi = 0x39C,
		.ts_prev_lo = 0x3A0,
		.ts_prev_hi = 0x3A4,
		.irq_status = 0xAC,
		.irq_mask = 0xB0,
		.irq_clear = 0xB4,
		.irq_clear_all = 0x7FFF,
		.top_bit = BIT(4),
		.rup = 0x10001,
	},
	[CAMSS_HW_PATH_RDI0] = CSID_GEN3_RDI(0),
	[CAMSS_HW_PATH_RDI1] = CSID_GEN3_RDI(1),
	[CAMSS_HW_PATH_RDI2] = CSID_GEN3_RDI(2),
	[CAMSS_HW_PATH_RDI3] = CSID_GEN3_RDI(3),
	[CAMSS_HW_PATH_RDI4] = CSID_GEN3_RDI(4),
	[CAMSS_HW_PATH_PPP] = {
		.cfg0 = 0xB00,
		.ctrl = 0xB04,
		.cfg1 = 0xB10,
		.err_rec = 0xB18,
		.epoch = 0xB34,
		.ts_lo = 0xB98,
		.ts_hi = 0xB9C,
		.ts_prev_lo = 0xBA0,
		.ts_prev_hi = 0xBA4,
		.irq_status = 0xCC,
		.irq_mask = 0xD0,
		.irq_clear = 0xD4,
		.irq_clear_all = 0xFFFFFFFF,
		.top_bit = BIT(6),
		.rup = 0x40004,
	},
};

static const u8 csid_gen3_buf_done_bit[CAMSS_HW_COMP_NUM] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 14, 15, 16, 11,
};

/* The lite CSID reports each VFE lite completion group at its own bit */
static u32 csid_gen3_buf_done_mask(struct camss_hw_pipe *pipe,
				   unsigned int comp)
{
	return BIT(csid_is_lite(pipe->csid) ? comp : csid_gen3_buf_done_bit[comp]);
}

unsigned int csid_gen3_hw_path_num(struct csid_device *csid)
{
	return csid_is_lite(csid) ? CAMSS_HW_PATH_RDI4 : CAMSS_HW_PATH_NUM;
}

static void __csid_configure_rx(struct csid_device *csid,
				struct csid_phy_config *phy, int vc)
{
	int val;
	struct camss *camss;

	camss = csid->camss;
	val = (phy->lane_cnt - 1) << CSI2_RX_CFG0_NUM_ACTIVE_LANES;
	val |= phy->cphy << CSI2_RX_CFG0_PHY_TYPE_SEL;
	val |= phy->lane_assign << CSI2_RX_CFG0_DL0_INPUT_SEL;

	if (camss->tpg && csid->tpg_linked &&
	    camss->tpg[phy->csiphy_id].testgen.mode != TPG_PAYLOAD_MODE_DISABLED) {
		val |= FIELD_PREP(CSI2_RX_CFG0_TPG_MUX_SEL, phy->csiphy_id + 1);
		val |= CSI2_RX_CFG0_TPG_MUX_EN;
	} else {
		val |= (phy->csiphy_id + CSI2_RX_CFG0_PHY_SEL_BASE_IDX)
			<< CSI2_RX_CFG0_PHY_NUM_SEL;
	}

	writel(val, csid->base + CSID_CSI2_RX_CFG0);

	val = CSI2_RX_CFG1_ECC_CORRECTION_EN;
	if (vc > 3)
		val |= CSI2_RX_CFG1_VC_MODE;

	writel(val, csid->base + CSID_CSI2_RX_CFG1);
}

static void __csid_ctrl_rdi(struct csid_device *csid, int enable, u8 rdi)
{
	int val = 0;

	if (enable)
		val = RDI_CTRL_START_CMD;

	writel(val, csid->base + CSID_RDI_CTRL(rdi));
}

static void __csid_configure_wrapper(struct csid_device *csid)
{
	u32 val;

	/* csid lite doesn't need to configure top register */
	if (csid->res->is_lite)
		return;

	val = OUTPUT_IFE_EN | INTERNAL_CSID;
	writel(val, csid->camss->csid_wrapper_base + CSID_IO_PATH_CFG0(csid->id));
}

static bool csid_port_is_ipp(struct csid_device *csid, u8 port)
{
	if (csid_is_lite(csid))
		return false;

	return port == VFE_LINE_PIX;
}

static void __csid_configure_ipp_stream(struct csid_device *csid, u8 enable, u8 port, u8 vc)
{
	struct v4l2_mbus_framefmt *input_format = &csid->fmt[MSM_CSID_PAD_FIRST_SRC + port];
	const struct csid_format_info *format = csid_get_fmt_entry(csid->res->formats->formats,
								   csid->res->formats->nformats,
								   input_format->code);
	u32 val;

	writel(0, csid->base + CSID_IPP_CTRL);

	/* The IPP decodes pixels for the pixel pipe: real decode format */
	val = IPP_CFG0_TIMESTAMP_EN;
	val |= format->decode_format << IPP_CFG0_DECODE_FORMAT;
	val |= format->data_type << IPP_CFG0_DT;
	val |= vc << IPP_CFG0_VC;
	val |= 0 << IPP_CFG0_DT_ID;
	if (enable)
		val |= IPP_CFG0_EN;
	writel(val, csid->base + CSID_IPP_CFG0);

	writel(csid_ipp_bayer_pattern(input_format->code) << IPP_CAMIF_PIXEL_PATTERN,
	       csid->base + CSID_IPP_CAMIF_FRAME_CFG);
}

static void __csid_ctrl_ipp(struct csid_device *csid, int resume)
{
	writel(resume ? IPP_CTRL_START_CMD : 0, csid->base + CSID_IPP_CTRL);
}

static void __csid_configure_rdi_stream(struct csid_device *csid, u8 enable, u8 port, u8 vc)
{
	u32 val;
	u8 lane_cnt = csid->phy.lane_cnt;
	/* Source pads matching RDI channels on hardware. Pad 1 -> RDI0, Pad 2 -> RDI1, etc. */
	struct v4l2_mbus_framefmt *input_format = &csid->fmt[MSM_CSID_PAD_FIRST_SRC + port];
	const struct csid_format_info *format = csid_get_fmt_entry(csid->res->formats->formats,
								   csid->res->formats->nformats,
								   input_format->code);

	if (!lane_cnt)
		lane_cnt = 4;

	/*
	 * DT_ID is a two bit bitfield that is concatenated with
	 * the four least significant bits of the five bit VC
	 * bitfield to generate an internal CID value.
	 *
	 * CSID_RDI_CFG0(port)
	 * DT_ID : 28:27
	 * VC    : 26:22
	 * DT    : 21:16
	 *
	 * CID   : VC 3:0 << 2 | DT_ID 1:0
	 */
	u8 dt_id = port & 0x03;

	val = RDI_CFG0_TIMESTAMP_EN;
	val |= RDI_CFG0_TIMESTAMP_STB_SEL;
	/* note: for non-RDI path, this should be format->decode_format */
	val |= DECODE_FORMAT_PAYLOAD_ONLY << RDI_CFG0_DECODE_FORMAT;
	val |= vc << RDI_CFG0_VC;
	val |= format->data_type << RDI_CFG0_DT;
	val |= dt_id << RDI_CFG0_DT_ID;

	writel(val, csid->base + CSID_RDI_CFG0(port));

	val = RDI_CFG1_PACKING_FORMAT_MIPI;
	val |= RDI_CFG1_PIX_STORE;
	val |= RDI_CFG1_DROP_H_EN;
	val |= RDI_CFG1_DROP_V_EN;
	val |= RDI_CFG1_CROP_H_EN;
	val |= RDI_CFG1_CROP_V_EN;

	writel(val, csid->base + CSID_RDI_CFG1(port));

	val = 0;
	writel(val, csid->base + CSID_RDI_IRQ_SUBSAMPLE_PERIOD(port));

	val = 1;
	writel(val, csid->base + CSID_RDI_IRQ_SUBSAMPLE_PATTERN(port));

	val = 0;
	writel(val, csid->base + CSID_RDI_CTRL(port));

	val = readl(csid->base + CSID_RDI_CFG0(port));

	if (enable)
		val |= RDI_CFG0_EN;
	writel(val, csid->base + CSID_RDI_CFG0(port));
}

static int csid_reset(struct csid_device *csid);

static void csid_configure_stream(struct csid_device *csid, u8 enable)
{
	u8 i;

	__csid_configure_wrapper(csid);

	/* Loop through all enabled ports and configure a stream for each */
	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS; i++)
		if (csid->phy.en_vc & BIT(i)) {
			if (csid_port_is_ipp(csid, i)) {
				/* the VFE has programmed the pixel pipe in its s_stream */
				__csid_configure_ipp_stream(csid, enable, i, 0);
				__csid_configure_rx(csid, &csid->phy, 0);
				__csid_ctrl_ipp(csid, enable);
			} else {
				__csid_configure_rdi_stream(csid, enable, i, 0);
				__csid_configure_rx(csid, &csid->phy, 0);
				__csid_ctrl_rdi(csid, enable, i);
			}
		}
}

static int csid_configure_testgen_pattern(struct csid_device *csid, s32 val)
{
	return 0;
}


static void csid_subdev_reg_update(struct csid_device *csid, int port_id, bool clear)
{
	u32 val;

	if (port_id == CSID_780_CUSTOM_PORT_ID_IPP0)
		val = BIT(0) | BIT(16) | BIT(28);	/* RUP_IPP0 | AUP_IPP0 | MUP */
	else
		val = CSID_RUP_AUP_RDI(port_id);

	if (clear) {
		csid->reg_update &= ~val;
	} else {
		csid->reg_update |= val;
		writel(csid->reg_update, csid->base + CSID_RUP_AUP_CMD);
	}
}

void csid_gen3_hw_init(struct camss_hw_pipe *pipe)
{
	struct csid_device *csid = pipe->csid;
	unsigned long flags;
	unsigned int i;

	__csid_configure_wrapper(csid);

	writel(0, csid->base + CSID_CSI2_RX_IRQ_MASK);
	writel(0, csid->base + CSID_BUF_DONE_IRQ_MASK);
	for (i = 0; i < pipe->path_num; i++)
		writel(0, csid->base + csid_gen3_paths[i].irq_mask);

	writel(TOP_IRQ_STATUS_RESET_DONE, csid->base + CSID_TOP_IRQ_CLEAR);
	writel(0xFFFFFFF, csid->base + CSID_CSI2_RX_IRQ_CLEAR);
	for (i = 0; i < pipe->path_num; i++)
		writel(csid_gen3_paths[i].irq_clear_all,
		       csid->base + csid_gen3_paths[i].irq_clear);
	writel(IRQ_CMD_CLEAR, csid->base + CSID_IRQ_CMD);

	spin_lock_irqsave(&pipe->lock, flags);
	pipe->top_mask = TOP_IRQ_STATUS_RESET_DONE | CSID_TOP_IRQ_BUF_DONE |
			 CSID_TOP_IRQ_ERR;
	pipe->buf_done_mask = 0;
	memset(pipe->comp_wms, 0, sizeof(pipe->comp_wms));
	writel(pipe->top_mask, csid->base + CSID_TOP_IRQ_MASK);
	spin_unlock_irqrestore(&pipe->lock, flags);
}

void csid_gen3_hw_dual(struct camss_hw_pipe *pipe, enum camss_hw_sync sync,
		       unsigned int peer)
{
	struct csid_device *csid = pipe->csid;
	u32 val = 0;

	if (sync != CAMSS_HW_SYNC_NONE) {
		val = (peer + 1) << DUAL_CFG0_SYNC_SEL | DUAL_CFG0_EN;
		if (sync == CAMSS_HW_SYNC_SLAVE)
			val |= DUAL_CFG0_SLAVE;
	}

	writel(val, csid->camss->csid_wrapper_base + CSID_DUAL_CFG0(csid->id));
}

static void csid_gen3_top_mask_set(struct camss_hw_pipe *pipe, u32 bits)
{
	unsigned long flags;

	spin_lock_irqsave(&pipe->lock, flags);
	pipe->top_mask |= bits;
	writel(pipe->top_mask, pipe->csid->base + CSID_TOP_IRQ_MASK);
	spin_unlock_irqrestore(&pipe->lock, flags);
}

void csid_gen3_hw_rx(struct camss_hw_pipe *pipe,
		     const struct camss_hw_rx_cfg *cfg)
{
	struct csid_device *csid = pipe->csid;
	u32 val;

	val = (cfg->lanes - 1) << CSI2_RX_CFG0_NUM_ACTIVE_LANES;
	val |= cfg->lane_cfg << CSI2_RX_CFG0_DL0_INPUT_SEL;
	val |= (cfg->phy + CSI2_RX_CFG0_PHY_SEL_BASE_IDX) << CSI2_RX_CFG0_PHY_NUM_SEL;
	val |= cfg->cphy << CSI2_RX_CFG0_PHY_TYPE_SEL;
	writel(val, csid->base + CSID_CSI2_RX_CFG0);

	val = CSI2_RX_CFG1_MISR_EN | CSI2_RX_CFG1_ECC_CORRECTION_EN;
	val |= cfg->epd << CSI2_RX_CFG1_EPD_EN;
	if (cfg->dyn_switch)
		val |= CSI2_RX_CFG1_DYN_SWITCH_EN | CSI2_RX_CFG1_RUP_AUP_LATCH_DIS;
	if (cfg->vc_full_width)
		val |= CSI2_RX_CFG1_VC_MODE;
	writel(val, csid->base + CSID_CSI2_RX_CFG1);

	val = CSID_RX_IRQ_FATAL | CSID_RX_IRQ_PART_FATAL | CSID_RX_IRQ_NON_FATAL;
	if (cfg->epd && !cfg->cphy)
		val &= ~CSID_RX_IRQ_CPHY_EOT;
	writel(val, csid->base + CSID_CSI2_RX_IRQ_MASK);

	csid_gen3_top_mask_set(pipe, CSID_TOP_IRQ_RX);
}

void csid_gen3_hw_path_config(struct camss_hw_pipe *pipe,
			      enum camss_hw_path path,
			      const struct camss_hw_path_cfg *cfg)
{
	const struct csid_gen3_path *p = &csid_gen3_paths[path];
	struct csid_device *csid = pipe->csid;
	u32 cfg0, cfg1, mask, ctrl = 0;

	cfg0 = cfg->cid << RDI_CFG0_DT_ID;
	cfg0 |= cfg->vc << RDI_CFG0_VC;
	cfg0 |= cfg->dt << RDI_CFG0_DT;
	cfg0 |= cfg->decode_format << RDI_CFG0_DECODE_FORMAT;
	cfg0 |= CSID_PATH_CFG0_TS_STB_SEL | RDI_CFG0_TIMESTAMP_EN;
	cfg0 |= cfg->frame_id_dec << CSID_PATH_CFG0_FRAME_ID_DEC;
	cfg0 |= cfg->vfr;
	if (cfg->lcr)
		cfg0 |= CSID_PATH_CFG0_SOF_RETIME_DIS;

	if (path == CAMSS_HW_PATH_IPP || path == CAMSS_HW_PATH_PPP) {
		cfg1 = CSID_IPP_CFG1_PIX_STORE;
		if (cfg->crop)
			cfg1 |= CSID_IPP_CFG1_CROP_H_EN | CSID_IPP_CFG1_CROP_V_EN;
		if (cfg->crop && !cfg->lcr)
			cfg1 |= CSID_IPP_CFG1_EARLY_EOF;
		if (cfg->drop)
			cfg1 |= CSID_IPP_CFG1_DROP_H_EN | CSID_IPP_CFG1_DROP_V_EN;
		if (path == CAMSS_HW_PATH_PPP) {
			if (cfg->bin_h)
				cfg1 |= CSID_PPP_CFG1_BIN_H_EN;
		} else {
			if (cfg->bin_h)
				cfg1 |= CSID_IPP_CFG1_BIN_H_EN;
			if (cfg->bin_v)
				cfg1 |= CSID_IPP_CFG1_BIN_V_EN;
			if (cfg->bin_qcfa)
				cfg1 |= CSID_IPP_CFG1_BIN_QCFA_EN;
			if (cfg->bin_h || cfg->bin_v || cfg->bin_qcfa)
				cfg1 |= CSID_IPP_CFG1_BIN_EN;
		}

		if (cfg->sync == CAMSS_HW_SYNC_MASTER)
			ctrl = FIELD_PREP(CSID_PATH_CTRL_START_MODE, START_MODE_MASTER);
		else if (cfg->sync == CAMSS_HW_SYNC_SLAVE)
			ctrl = FIELD_PREP(CSID_PATH_CTRL_START_MODE, START_MODE_SLAVE);
	} else {
		cfg1 = cfg->packing_mipi << CSID_RDI_CFG1_PACKING;
		cfg1 |= cfg->plain_format << CSID_RDI_CFG1_PLAIN_FMT;
		cfg1 |= cfg->plain_align << CSID_RDI_CFG1_PLAIN_ALIGN;
		if (cfg->pix_store)
			cfg1 |= RDI_CFG1_PIX_STORE;
		if (cfg->crop)
			cfg1 |= RDI_CFG1_CROP_H_EN | RDI_CFG1_CROP_V_EN;
		if (cfg->drop)
			cfg1 |= RDI_CFG1_DROP_H_EN | RDI_CFG1_DROP_V_EN;
	}

	writel(0, csid->base + p->ctrl);
	writel(cfg0, csid->base + p->cfg0);
	writel(cfg1, csid->base + p->cfg1);
	writel(cfg0 | RDI_CFG0_EN, csid->base + p->cfg0);
	writel(CSID_PATH_ERR_REC_DEFAULT, csid->base + p->err_rec);
	writel(cfg->epoch_line << 16, csid->base + p->epoch);

	if (cfg->lcr) {
		u32 shdr = readl(csid->base + CSID_SHDR_MASTER_SLAVE_CFG);

		shdr |= SHDR_EN;
		shdr |= path == CAMSS_HW_PATH_PPP ? SHDR_SLAVE_PPP : SHDR_MASTER_RDI0;
		writel(shdr, csid->base + CSID_SHDR_MASTER_SLAVE_CFG);
		pipe->shdr = true;
	}

	mask = CSID_PATH_IRQ_FATAL | CSID_PATH_IRQ_NON_FATAL;
	if (cfg->events & CAMSS_HW_EV_SOF)
		mask |= CSID_PATH_IRQ_SOF;
	if (cfg->events & CAMSS_HW_EV_EOF)
		mask |= CSID_PATH_IRQ_EOF;
	if (cfg->events & CAMSS_HW_EV_EPOCH)
		mask |= CSID_PATH_IRQ_EPOCH0;
	if (cfg->events & CAMSS_HW_EV_RUP)
		mask |= CSID_PATH_IRQ_RUP_DONE;
	if (cfg->events & CAMSS_HW_EV_INPUT_SOF)
		mask |= CSID_PATH_IRQ_INPUT_SOF;
	writel(mask, csid->base + p->irq_mask);
	writel(ctrl, csid->base + p->ctrl);

	pipe->paths |= BIT(path);
	csid_gen3_top_mask_set(pipe, p->top_bit | CSID_TOP_IRQ_BUF_DONE |
			       CSID_TOP_IRQ_ERR);
}

void csid_gen3_hw_path_resume(struct camss_hw_pipe *pipe,
			      enum camss_hw_path path)
{
	const struct csid_gen3_path *p = &csid_gen3_paths[path];
	struct csid_device *csid = pipe->csid;
	unsigned long flags;

	spin_lock_irqsave(&pipe->lock, flags);
	writel(readl(csid->base + p->irq_mask) & ~CSID_PATH_IRQ_INPUT_SOF,
	       csid->base + p->irq_mask);
	spin_unlock_irqrestore(&pipe->lock, flags);

	writel(readl(csid->base + p->ctrl) | CSID_PATH_CTRL_RESUME,
	       csid->base + p->ctrl);
}

void csid_gen3_hw_path_resume_global(struct camss_hw_pipe *pipe,
				     enum camss_hw_path path)
{
	writel(FIELD_PREP(CSID_PATH_CTRL_START_MODE, START_MODE_GLOBAL) |
	       CSID_PATH_CTRL_RESUME, pipe->csid->base + csid_gen3_paths[path].ctrl);
}

void csid_gen3_hw_global_start(struct camss_hw_pipe *pipe)
{
	writel(GLOBAL_CMD_START, pipe->csid->base + CSID_GLOBAL_CMD);
}

int csid_gen3_hw_stop(struct camss_hw_pipe *pipe)
{
	struct csid_device *csid = pipe->csid;
	unsigned long time, flags;
	unsigned int i;

	reinit_completion(&csid->reset_complete);
	writel(CSID_PATH_RESET_CFG, csid->base + CSID_RST_CFG);
	writel(CSID_PATH_RESET_CMD, csid->base + CSID_RST_CMD);
	time = wait_for_completion_timeout(&csid->reset_complete,
					   msecs_to_jiffies(CSID_RESET_TIMEOUT_MS));

	writel(0, csid->base + CSID_CSI2_RX_IRQ_MASK);
	for_each_set_bit(i, &pipe->paths, CAMSS_HW_PATH_NUM)
		writel(0, csid->base + csid_gen3_paths[i].irq_mask);
	if (pipe->shdr) {
		writel(0, csid->base + CSID_SHDR_MASTER_SLAVE_CFG);
		pipe->shdr = false;
	}

	spin_lock_irqsave(&pipe->lock, flags);
	pipe->buf_done_mask = 0;
	memset(pipe->comp_wms, 0, sizeof(pipe->comp_wms));
	pipe->top_mask = TOP_IRQ_STATUS_RESET_DONE;
	pipe->gen++;
	writel(0, csid->base + CSID_BUF_DONE_IRQ_MASK);
	writel(pipe->top_mask, csid->base + CSID_TOP_IRQ_MASK);
	spin_unlock_irqrestore(&pipe->lock, flags);

	synchronize_irq(csid->irq);
	pipe->paths = 0;

	if (!time) {
		dev_err(csid->camss->dev, "CSID%u path reset timeout\n", csid->id);
		return -EIO;
	}

	return 0;
}

u64 csid_gen3_hw_path_sof_ts(struct camss_hw_pipe *pipe,
			     enum camss_hw_path path, bool prev)
{
	const struct csid_gen3_path *p = &csid_gen3_paths[path];
	struct csid_device *csid = pipe->csid;
	u64 hi, lo;

	hi = readl(csid->base + (prev ? p->ts_prev_hi : p->ts_hi));
	lo = readl(csid->base + (prev ? p->ts_prev_lo : p->ts_lo));

	return mul_u64_u32_div(hi << 32 | lo, CSID_QTIMER_NS_NUM,
			       CSID_QTIMER_NS_DEN);
}

struct camss_hw_reg csid_gen3_hw_rup_reg(unsigned long paths, bool mup)
{
	struct camss_hw_reg reg = { .offset = CSID_RUP_AUP_CMD };
	unsigned int i;

	for_each_set_bit(i, &paths, CAMSS_HW_PATH_NUM)
		reg.value |= csid_gen3_paths[i].rup;
	if (mup)
		reg.value |= CSID_RUP_AUP_MUP;

	return reg;
}

void csid_gen3_hw_buf_done(struct camss_hw_pipe *pipe, unsigned int comp,
			   unsigned int wm, bool enable)
{
	u32 bit = csid_gen3_buf_done_mask(pipe, comp);
	unsigned long flags;

	spin_lock_irqsave(&pipe->lock, flags);

	if (enable)
		pipe->comp_wms[comp] |= BIT(wm);
	else
		pipe->comp_wms[comp] &= ~BIT(wm);

	if (pipe->comp_wms[comp])
		pipe->buf_done_mask |= bit;
	else
		pipe->buf_done_mask &= ~bit;

	writel(pipe->buf_done_mask, pipe->csid->base + CSID_BUF_DONE_IRQ_MASK);

	spin_unlock_irqrestore(&pipe->lock, flags);
}

static void csid_gen3_dispatch(struct camss_hw_pipe *pipe,
			       const struct camss_hw_irq *ev)
{
	const struct camss_hw_ops *ops = pipe->ops;
	unsigned int i, wm;
	unsigned long wms;
	u32 err;

	if (ev->gen != READ_ONCE(pipe->gen))
		return;

	err = ev->top & CSID_TOP_IRQ_ERR;
	if (err && ops->error)
		ops->error(pipe->priv, CAMSS_HW_ERR_TOP, 0, err, true);

	for (i = 0; i < CAMSS_HW_PATH_NUM; i++) {
		u32 st = ev->path[i];

		if (!st)
			continue;

		err = st & (CSID_PATH_IRQ_FATAL | CSID_PATH_IRQ_NON_FATAL);
		if (err && ops->error)
			ops->error(pipe->priv, CAMSS_HW_ERR_PATH, i, err,
				   err & CSID_PATH_IRQ_FATAL);

		if ((st & CSID_PATH_IRQ_INPUT_SOF) && ops->input_sof)
			ops->input_sof(pipe->priv, i, ev->boot_ns);
		if ((st & CSID_PATH_IRQ_EOF) && ops->eof)
			ops->eof(pipe->priv, i, ev->boot_ns);
		if ((st & CSID_PATH_IRQ_SOF) && ops->sof)
			ops->sof(pipe->priv, i,
				 mul_u64_u32_div(ev->sof_ts[i], CSID_QTIMER_NS_NUM,
						 CSID_QTIMER_NS_DEN),
				 ev->boot_ns);
		if ((st & CSID_PATH_IRQ_RUP_DONE) && ops->rup_done)
			ops->rup_done(pipe->priv, i, ev->boot_ns);
		if ((st & CSID_PATH_IRQ_EPOCH0) && ops->epoch)
			ops->epoch(pipe->priv, i, ev->boot_ns);
	}

	if (ev->rx && ops->error)
		ops->error(pipe->priv, CAMSS_HW_ERR_RX, 0, ev->rx,
			   ev->rx & CSID_RX_IRQ_FATAL);

	for (i = 0; ops->buf_done && i < CAMSS_HW_COMP_NUM; i++) {
		wms = ev->comp_wms[i];
		for_each_set_bit(wm, &wms, CAMSS_HW_WM_NUM)
			ops->buf_done(pipe->priv, i, wm, ev->addr[wm],
				      ev->boot_ns);
	}
}

static irqreturn_t csid_isr_claimed(struct csid_device *csid,
				    struct camss_hw_pipe *pipe)
{
	struct camss_hw_irq ev = {};
	unsigned long wms;
	unsigned int i, wm;
	u32 hi;

	spin_lock(&pipe->lock);
	ev.gen = pipe->gen;
	spin_unlock(&pipe->lock);

	ev.top = readl(csid->base + CSID_TOP_IRQ_STATUS);
	writel(ev.top, csid->base + CSID_TOP_IRQ_CLEAR);

	if (ev.top & CSID_TOP_IRQ_RX) {
		ev.rx = readl(csid->base + CSID_CSI2_RX_IRQ_STATUS);
		writel(ev.rx, csid->base + CSID_CSI2_RX_IRQ_CLEAR);
	}

	for_each_set_bit(i, &pipe->paths, CAMSS_HW_PATH_NUM) {
		const struct csid_gen3_path *p = &csid_gen3_paths[i];

		if (!(ev.top & p->top_bit))
			continue;

		ev.path[i] = readl(csid->base + p->irq_status);
		writel(ev.path[i], csid->base + p->irq_clear);

		if (ev.path[i] & CSID_PATH_IRQ_SOF) {
			hi = readl(csid->base + p->ts_hi);
			ev.sof_ts[i] = (u64)hi << 32 |
				       readl(csid->base + p->ts_lo);
		}
	}

	spin_lock(&pipe->lock);

	if (ev.top & CSID_TOP_IRQ_BUF_DONE) {
		ev.buf_done = readl(csid->base + CSID_BUF_DONE_IRQ_STATUS);
		writel(ev.buf_done, csid->base + CSID_BUF_DONE_IRQ_CLEAR);

		for (i = 0; i < pipe->comp_num; i++) {
			if (!(ev.buf_done & csid_gen3_buf_done_mask(pipe, i)))
				continue;

			ev.comp_wms[i] = pipe->comp_wms[i];
			wms = ev.comp_wms[i];
			for_each_set_bit(wm, &wms, pipe->wm_num)
				ev.addr[wm] = vfe_780_wm_last_addr(pipe->vfe, wm);
		}
	}

	spin_unlock(&pipe->lock);

	writel(IRQ_CMD_CLEAR, csid->base + CSID_IRQ_CMD);

	if (ev.top & TOP_IRQ_STATUS_RESET_DONE)
		complete(&csid->reset_complete);

	if (!(ev.top & ~TOP_IRQ_STATUS_RESET_DONE))
		return IRQ_HANDLED;

	if (smp_load_acquire(&pipe->vfe->claim) != pipe)
		return IRQ_HANDLED;

	ev.boot_ns = ktime_get_boottime_ns();
	csid_gen3_dispatch(pipe, &ev);

	return IRQ_HANDLED;
}

/*
 * csid_isr - CSID module interrupt service routine
 * @irq: Interrupt line
 * @dev: CSID device
 *
 * Return IRQ_HANDLED on success
 */
static irqreturn_t csid_isr(int irq, void *dev)
{
	struct csid_device *csid = dev;
	u32 val, buf_done_val;
	u8 reset_done;
	int i;

	if (csid->claim)
		return csid_isr_claimed(csid, csid->claim);

	val = readl(csid->base + CSID_TOP_IRQ_STATUS);
	writel(val, csid->base + CSID_TOP_IRQ_CLEAR);
	reset_done = val & TOP_IRQ_STATUS_RESET_DONE;

	val = readl(csid->base + CSID_CSI2_RX_IRQ_STATUS);
	writel(val, csid->base + CSID_CSI2_RX_IRQ_CLEAR);

	buf_done_val = readl(csid->base + CSID_BUF_DONE_IRQ_STATUS);
	writel(buf_done_val, csid->base + CSID_BUF_DONE_IRQ_CLEAR);

	/* Read and clear IRQ status for each enabled port */
	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS; i++) {
		u32 done;

		if (!(csid->phy.en_vc & BIT(i)))
			continue;

		if (csid_port_is_ipp(csid, i)) {
			val = readl(csid->base + CSID_CSI2_IPP_IRQ_STATUS);
			writel(val, csid->base + CSID_CSI2_IPP_IRQ_CLEAR);
			/* the pixel and statistics completion groups sit below the first RDI bit */
			done = buf_done_val & (BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET) - 1);
		} else {
			val = readl(csid->base + CSID_CSI2_RDIN_IRQ_STATUS(i));
			writel(val, csid->base + CSID_CSI2_RDIN_IRQ_CLEAR(i));
			done = buf_done_val & BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET + i);
		}

		if (val & RUP_DONE_IRQ_STATUS)
			/* clear the reg update bit */
			csid_subdev_reg_update(csid, i, true);

		if (done) {
			struct camss_buf_done_info info = {
				.hw_id = csid->id,
				.port_id = i,
				.status = done,
			};

			/*
			 * For Titan Gen3, bus done and RUP IRQ have been moved to
			 * CSID from VFE. Once CSID received bus done, need notify
			 * VFE of this event. Trigger VFE to handle bus done process.
			 */
			camss_buf_done(csid->camss, &info);
		}
	}

	val = IRQ_CMD_CLEAR;
	writel(val, csid->base + CSID_IRQ_CMD);

	if (reset_done)
		complete(&csid->reset_complete);

	return IRQ_HANDLED;
}

/*
 * csid_reset - Trigger reset on CSID module and wait to complete
 * @csid: CSID device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_reset(struct csid_device *csid)
{
	unsigned long time;
	u32 val;
	int i;

	reinit_completion(&csid->reset_complete);

	writel(1, csid->base + CSID_TOP_IRQ_CLEAR);
	writel(1, csid->base + CSID_IRQ_CMD);
	writel(1, csid->base + CSID_TOP_IRQ_MASK);

	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS; i++)
		if (csid->phy.en_vc & BIT(i)) {
			writel(BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET + i),
			       csid->base + CSID_BUF_DONE_IRQ_CLEAR);
			writel(IRQ_CMD_CLEAR, csid->base + CSID_IRQ_CMD);
			writel(BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET + i),
			       csid->base + CSID_BUF_DONE_IRQ_MASK);
		}

	/* preserve registers */
	val = RST_LOCATION | RST_MODE;
	writel(val, csid->base + CSID_RST_CFG);

	val = SELECT_HW_RST | SELECT_IRQ_RST;
	writel(val, csid->base + CSID_RST_CMD);

	time = wait_for_completion_timeout(&csid->reset_complete,
					   msecs_to_jiffies(CSID_RESET_TIMEOUT_MS));
	if (!time) {
		dev_err(csid->camss->dev, "CSID reset timeout\n");
		return -EIO;
	}

	writel_relaxed(~0u, csid->base + CSID_TOP_IRQ_MASK);
	writel_relaxed(~0u, csid->base + CSID_BUF_DONE_IRQ_MASK);

	return 0;
}

static void csid_subdev_init(struct csid_device *csid)
{
	csid->testgen.nmodes = CSID_PAYLOAD_MODE_DISABLED;
}

const struct csid_hw_ops csid_ops_gen3 = {
	.configure_stream = csid_configure_stream,
	.configure_testgen_pattern = csid_configure_testgen_pattern,
	.hw_version = csid_hw_version,
	.isr = csid_isr,
	.reset = csid_reset,
	.src_pad_code = csid_src_pad_code,
	.subdev_init = csid_subdev_init,
	.reg_update = csid_subdev_reg_update,
};
