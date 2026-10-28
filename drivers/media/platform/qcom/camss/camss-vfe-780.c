// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm MSM Camera Subsystem - VFE (Video Front End) Module 780
 *
 * Copyright (c) 2024 Qualcomm Technologies, Inc.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>

#include <linux/qcom-camss-config.h>

#include "camss.h"
#include "camss-csid-gen3.h"
#include "camss-vfe.h"
#include "camss-vfe-880-pp.h"

typedef enum {
	VFE_WM_VIDEO_FULL_Y = 0,
	VFE_WM_VIDEO_FULL_C,
	VFE_WM_VIDEO_DS_4,
	VFE_WM_VIDEO_DS_16,
	VFE_WM_DISPLAY_FULL_Y,
	VFE_WM_DISPLAY_FULL_C,
	VFE_WM_DISPLAY_DS_4,
	VFE_WM_DISPLAY_DS_16,
	VFE_WM_FD_Y,
	VFE_WM_FD_C,
	VFE_WM_PIXEL_RAW,
	VFE_WM_STATS_AEC_BE,
	VFE_WM_STATS_AEC_BHIST,
	VFE_WM_STATS_TINTLESS_BG,
	VFE_WM_STATS_AWB_BG,
	VFE_WM_STATS_AWB_BFW,
	VFE_WM_STATS_BF,
	VFE_WM_STATS_GTM_BHIST,
	VFE_WM_STATS_RS,
	VFE_WM_STATS_IHIST,
	VFE_WM_PDAF_2PD,
	VFE_WM_PDAF_PREPROCESS_2PD,
	VFE_WM_PDAF_PARSED,
	VFE_WM_RDI0,
	VFE_WM_RDI1,
	VFE_WM_RDI2,
	VFE_WM_LTM,
	VFE_WM_STATS_ALSC,
}vfe_wm_t;

typedef enum {
	VFE_LITE_WM_RDI0 = 0,
	VFE_LITE_WM_RDI1,
	VFE_LITE_WM_RDI2,
	VFE_LITE_WM_RDI3,
	VFE_LITE_WM_PREPROCESS_RAW,
	VFE_LITE_WM_STATS_BG,
} vfe_lite_wm_t;

#define VFE_TOP_IRQ_CMD			(vfe_is_lite(vfe) ? 0x38 : 0x30)
#define		TOP_IRQ_CMD_CLEAR		BIT(0)
#define VFE_TOP_IRQ_MASK(n)		((vfe_is_lite(vfe) ? 0x24 : 0x34) + 4 * (n))
#define VFE_TOP_IRQ_CLEAR(n)		((vfe_is_lite(vfe) ? 0x2C : 0x3C) + 4 * (n))
#define VFE_TOP_IRQ_STATUS_0		(vfe_is_lite(vfe) ? 0x1C : 0x44)
#define		TOP_IRQ_BUS			BIT(0)
#define		TOP_IRQ_CAMIF_ERR		0x7F1D0

#define VFE_TOP_EPOCH_HEIGHT_CFG	0x9C
#define VFE_TOP_DEBUG_CFG		(vfe_is_lite(vfe) ? 0x74 : 0xFC)
#define		TOP_DEBUG_CFG_EN		3
#define VFE_TOP_PDAF_INPUT_CFG_0	0x130
#define VFE_TOP_PDAF_INPUT_CFG_1	0x134
#define		PDAF_INPUT_CFG_1_DEFAULT	1
#define		PDAF_INPUT_CFG_1_LCR		0

#define VFE_FCG_PHASE_INDEX_CFG_0	0xC870
#define		FCG_PHASE_INDEX_G		16
#define VFE_FCG_PHASE_INDEX_CFG_1	0xC874

#define BUS_REG_BASE (vfe_is_lite(vfe) ? 0x200 : 0xC00)

#define VFE_BUS_IRQ_MASK(n)		(BUS_REG_BASE + 0x18 + 4 * (n))
#define VFE_BUS_IRQ_CLEAR(n)		(BUS_REG_BASE + 0x20 + 4 * (n))
#define VFE_BUS_IRQ_STATUS_0		(BUS_REG_BASE + 0x28)
#define VFE_BUS_IRQ_CMD			(BUS_REG_BASE + 0x30)
#define		BUS_IRQ_CMD_CLEAR		BIT(0)
#define		BUS_IRQ_CONSTRAINT		BIT(28)
#define		BUS_IRQ_CCIF			BIT(30)
#define		BUS_IRQ_IMAGE_SIZE		BIT(31)
#define		BUS_IRQ_ERR			(BUS_IRQ_CONSTRAINT | BUS_IRQ_CCIF | \
						 BUS_IRQ_IMAGE_SIZE)
#define VFE_BUS_IRQ_REGS		(vfe_is_lite(vfe) ? 1 : 2)
#define VFE_BUS_VIOLATION_STATUS	(BUS_REG_BASE + 0x64)
#define VFE_BUS_OVERFLOW_STATUS		(BUS_REG_BASE + 0x68)
#define VFE_BUS_IMAGE_SIZE_VIOLATION	(BUS_REG_BASE + 0x70)

#define VFE_BUS_WM_TEST_BUS_CTRL (BUS_REG_BASE + 0xDC)

#define VFE_BUS_WM_CGC_OVERRIDE		(BUS_REG_BASE + 0x08)
#define		WM_CGC_OVERRIDE_ALL		(0x7FFFFFF)

#define VFE_BUS_WM_CFG(n)		(BUS_REG_BASE + 0x200 + (n) * 0x100)
#define		WM_CFG_EN			BIT(0)
#define		WM_VIR_FRM_EN			BIT(1)
#define		WM_FRAME_HEADER_EN		BIT(2)
#define		WM_CFG_MODE			BIT(16)
#define VFE_BUS_WM_IMAGE_ADDR(n)	(BUS_REG_BASE + 0x204 + (n) * 0x100)
#define VFE_BUS_WM_FRAME_INCR(n)	(BUS_REG_BASE + 0x208 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_CFG_0(n)	(BUS_REG_BASE + 0x20c + (n) * 0x100)
#define		WM_IMAGE_CFG_0_DEFAULT_WIDTH	(0xFFFF)
#define VFE_BUS_WM_IMAGE_CFG_1(n)	(BUS_REG_BASE + 0x210 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_CFG_2(n)	(BUS_REG_BASE + 0x214 + (n) * 0x100)
#define		WM_IMAGE_CFG_2_DEFAULT_STRIDE	(0xFFFF)
#define VFE_BUS_WM_PACKER_CFG(n)	(BUS_REG_BASE + 0x218 + (n) * 0x100)
#define VFE_BUS_WM_FRAME_HEADER_ADDR(n)	(BUS_REG_BASE + 0x220 + (n) * 0x100)
#define VFE_BUS_WM_FRAME_HEADER_CFG(n)	(BUS_REG_BASE + 0x228 + (n) * 0x100)

#define VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(n)	(BUS_REG_BASE + 0x230 + (n) * 0x100)
#define VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(n)	(BUS_REG_BASE + 0x234 + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PERIOD(n)		(BUS_REG_BASE + 0x238 + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PATTERN(n)		(BUS_REG_BASE + 0x23c + (n) * 0x100)

#define VFE_BUS_WM_MMU_PREFETCH_CFG(n)		(BUS_REG_BASE + 0x260 + (n) * 0x100)
#define VFE_BUS_WM_MMU_PREFETCH_MAX_OFFSET(n)	(BUS_REG_BASE + 0x264 + (n) * 0x100)
#define VFE_BUS_WM_ADDR_CFG(n)			(BUS_REG_BASE + 0x270 + (n) * 0x100)
#define VFE_BUS_WM_ADDR_STATUS_0(n)		(BUS_REG_BASE + 0x278 + (n) * 0x100)
#define VFE_BUS_WM_DEBUG_STATUS_CFG(n)		(BUS_REG_BASE + 0x288 + (n) * 0x100)
#define		WM_DEBUG_STATUS_1_EN		(11 << 8)
#define VFE_BUS_WM_DEBUG_STATUS_1(n)		(BUS_REG_BASE + 0x290 + (n) * 0x100)

#define VFE_BUS_WM_BW_LIMIT(n)			(BUS_REG_BASE + 0x21c + (n) * 0x100)
#define VFE_BUS_WM_UBWC_META_ADDR(n)		(BUS_REG_BASE + 0x240 + (n) * 0x100)
#define VFE_BUS_WM_UBWC_META_CFG(n)		(BUS_REG_BASE + 0x244 + (n) * 0x100)
#define VFE_BUS_WM_UBWC_MODE_CFG(n)		(BUS_REG_BASE + 0x248 + (n) * 0x100)
#define		WM_UBWC_MODE_EN			BIT(0)
#define VFE_BUS_WM_UBWC_CTRL_2(n)		(BUS_REG_BASE + 0x250 + (n) * 0x100)
#define VFE_BUS_WM_UBWC_LOSSY_THRESH0(n)	(BUS_REG_BASE + 0x254 + (n) * 0x100)
#define VFE_BUS_WM_UBWC_LOSSY_THRESH1(n)	(BUS_REG_BASE + 0x258 + (n) * 0x100)
#define VFE_BUS_WM_UBWC_OFF_LOSSY_VAR(n)	(BUS_REG_BASE + 0x25c + (n) * 0x100)
#define		WM_UBWC_BW_LIMIT_BLOB		0xFFFF

#define VFE_BUS_UBWC_STATIC_CTRL		(BUS_REG_BASE + 0x58)
#define		UBWC_STATIC_CTRL_LPDDR5		0x1036

#define VFE_WM_REG(r, o, v)	((r)[n].offset = (o), (r)[n++].value = (v))

#define VFE_780_COMP_NUM	16
#define VFE_LITE_780_COMP_NUM	5

unsigned int vfe_780_wm_num(struct vfe_device *vfe)
{
	return vfe_is_lite(vfe) ? VFE_LITE_WM_STATS_BG + 1 :
				  VFE_WM_STATS_ALSC + 1;
}

unsigned int vfe_780_comp_num(struct vfe_device *vfe)
{
	return vfe_is_lite(vfe) ? VFE_LITE_780_COMP_NUM : VFE_780_COMP_NUM;
}

static bool vfe_780_wm_has_ubwc(struct vfe_device *vfe, unsigned int wm)
{
	if (vfe_is_lite(vfe))
		return false;

	return wm == VFE_WM_VIDEO_FULL_Y || wm == VFE_WM_VIDEO_FULL_C ||
	       wm == VFE_WM_DISPLAY_FULL_Y || wm == VFE_WM_DISPLAY_FULL_C;
}

static bool vfe_780_wm_has_frame_header(struct vfe_device *vfe, unsigned int wm)
{
	if (vfe_is_lite(vfe))
		return true;

	return wm != VFE_WM_VIDEO_DS_4 && wm != VFE_WM_VIDEO_DS_16 &&
	       wm != VFE_WM_DISPLAY_DS_4 && wm != VFE_WM_DISPLAY_DS_16 &&
	       wm != VFE_WM_FD_C;
}

static u32 vfe_780_wm_en_cfg(const struct camss_hw_wm_cfg *cfg)
{
	u32 val = cfg->mode << 16 | cfg->virtual_frame << 1 | WM_CFG_EN;

	if (cfg->frame_header)
		val |= WM_FRAME_HEADER_EN;

	return val;
}

static unsigned int vfe_780_wm_ubwc_regs(struct vfe_device *vfe, unsigned int wm,
					 const struct camss_hw_wm_cfg *cfg,
					 u32 mode_cfg, struct camss_hw_reg *regs,
					 unsigned int n)
{
	const struct camss_hw_wm_ubwc *u = &cfg->ubwc_cfg;

	VFE_WM_REG(regs, VFE_BUS_WM_PACKER_CFG(wm), cfg->packer);
	VFE_WM_REG(regs, VFE_BUS_WM_UBWC_META_CFG(wm), u->meta_cfg);
	VFE_WM_REG(regs, VFE_BUS_WM_UBWC_MODE_CFG(wm), mode_cfg);
	VFE_WM_REG(regs, VFE_BUS_WM_UBWC_CTRL_2(wm), u->ctrl_2);
	VFE_WM_REG(regs, VFE_BUS_WM_UBWC_LOSSY_THRESH0(wm), u->lossy_thresh0);
	VFE_WM_REG(regs, VFE_BUS_WM_UBWC_LOSSY_THRESH1(wm), u->lossy_thresh1);
	VFE_WM_REG(regs, VFE_BUS_WM_UBWC_OFF_LOSSY_VAR(wm), u->off_lossy_var);
	if (u->bw_limit < WM_UBWC_BW_LIMIT_BLOB)
		VFE_WM_REG(regs, VFE_BUS_WM_BW_LIMIT(wm), u->bw_limit);

	return n;
}

int vfe_780_wm_start_regs(struct vfe_device *vfe, unsigned int wm,
			  const struct camss_hw_wm_cfg *cfg,
			  struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	unsigned int n = 0;

	if (cfg->ubwc && !vfe_780_wm_has_ubwc(vfe, wm))
		return -EINVAL;

	VFE_WM_REG(regs, VFE_BUS_WM_IMAGE_CFG_0(wm), cfg->height << 16 | cfg->width);
	if (cfg->mode == CAMSS_HW_WM_LINE)
		VFE_WM_REG(regs, VFE_BUS_WM_IMAGE_CFG_1(wm), cfg->h_init);
	VFE_WM_REG(regs, VFE_BUS_WM_IMAGE_CFG_2(wm), cfg->stride);
	VFE_WM_REG(regs, VFE_BUS_WM_PACKER_CFG(wm), cfg->packer);
	if (cfg->ubwc)
		n = vfe_780_wm_ubwc_regs(vfe, wm, cfg,
					 cfg->ubwc_cfg.mode_cfg | WM_UBWC_MODE_EN,
					 regs, n);

	if (!vfe_is_lite(vfe)) {
		VFE_WM_REG(regs, VFE_BUS_WM_FRAMEDROP_PERIOD(wm), 0);
		VFE_WM_REG(regs, VFE_BUS_WM_FRAMEDROP_PATTERN(wm), 1);
	}
	VFE_WM_REG(regs, VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(wm), 0);
	VFE_WM_REG(regs, VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(wm), 1);
	VFE_WM_REG(regs, VFE_BUS_WM_MMU_PREFETCH_CFG(wm), 1);
	VFE_WM_REG(regs, VFE_BUS_WM_MMU_PREFETCH_MAX_OFFSET(wm), 0xFFFFFFFF);
	VFE_WM_REG(regs, VFE_BUS_WM_CFG(wm), vfe_780_wm_en_cfg(cfg));
	VFE_WM_REG(regs, VFE_BUS_WM_DEBUG_STATUS_CFG(wm), WM_DEBUG_STATUS_1_EN);

	return n;
}

int vfe_780_wm_update_regs(struct vfe_device *vfe, unsigned int wm,
			   const struct camss_hw_wm_cfg *cfg, u64 iova,
			   unsigned int flags,
			   struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	u32 en_cfg = vfe_780_wm_en_cfg(cfg);
	u64 frame_incr, addr;
	unsigned int n = 0;
	u32 offset;

	if (cfg->ubwc && !vfe_780_wm_has_ubwc(vfe, wm))
		return -EINVAL;

	if (cfg->frame_header && !vfe_780_wm_has_frame_header(vfe, wm))
		return -EINVAL;

	VFE_WM_REG(regs, VFE_BUS_WM_CFG(wm), en_cfg);
	VFE_WM_REG(regs, VFE_BUS_WM_IMAGE_CFG_0(wm), cfg->height << 16 | cfg->width);
	if (flags & CAMSS_HW_WM_UPDATE_STRIDE)
		VFE_WM_REG(regs, VFE_BUS_WM_IMAGE_CFG_2(wm), cfg->stride);
	if (flags & CAMSS_HW_WM_UPDATE_PACKER)
		VFE_WM_REG(regs, VFE_BUS_WM_PACKER_CFG(wm), cfg->packer);
	if (cfg->ubwc) {
		if (flags & CAMSS_HW_WM_UPDATE_UBWC)
			n = vfe_780_wm_ubwc_regs(vfe, wm, cfg,
						 cfg->ubwc_cfg.mode_cfg | WM_UBWC_MODE_EN,
						 regs, n);
		VFE_WM_REG(regs, VFE_BUS_WM_UBWC_META_ADDR(wm), iova >> 8);
	}

	frame_incr = (u64)cfg->stride * cfg->slice_height;
	if (cfg->ubwc)
		frame_incr = ALIGN(frame_incr, SZ_4K) + cfg->ubwc_cfg.meta_size;

	if (cfg->mode == CAMSS_HW_WM_LINE)
		VFE_WM_REG(regs, VFE_BUS_WM_IMAGE_CFG_1(wm), cfg->h_init);

	if (cfg->ubwc)
		offset = cfg->ubwc_cfg.meta_size;
	else if (cfg->mode != CAMSS_HW_WM_LINE)
		offset = cfg->offset;
	else
		offset = 0;

	addr = iova + offset;
	VFE_WM_REG(regs, VFE_BUS_WM_IMAGE_ADDR(wm), addr >> 8);
	VFE_WM_REG(regs, VFE_BUS_WM_ADDR_CFG(wm), addr & 0xff);
	VFE_WM_REG(regs, VFE_BUS_WM_FRAME_INCR(wm), frame_incr >> 8);
	VFE_WM_REG(regs, VFE_BUS_WM_CFG(wm), en_cfg);

	return n;
}

int vfe_780_wm_frame_header_regs(struct vfe_device *vfe, unsigned int wm,
				 u64 iova, u32 id,
				 struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	unsigned int n = 0;

	if (!vfe_780_wm_has_frame_header(vfe, wm) || iova & 0xff)
		return -EINVAL;

	VFE_WM_REG(regs, VFE_BUS_WM_FRAME_HEADER_ADDR(wm), iova >> 8);
	VFE_WM_REG(regs, VFE_BUS_WM_FRAME_HEADER_CFG(wm), id);

	return n;
}

int vfe_780_wm_hfr_regs(struct vfe_device *vfe, unsigned int wm,
			const struct camss_hw_wm_hfr *hfr,
			struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	unsigned int n = 0;

	if (!vfe_is_lite(vfe)) {
		VFE_WM_REG(regs, VFE_BUS_WM_FRAMEDROP_PATTERN(wm), hfr->framedrop_pattern);
		VFE_WM_REG(regs, VFE_BUS_WM_FRAMEDROP_PERIOD(wm), hfr->framedrop_period);
	}
	VFE_WM_REG(regs, VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(wm), hfr->subsample_period);
	VFE_WM_REG(regs, VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(wm), hfr->subsample_pattern);

	return n;
}

int vfe_780_wm_bw_limit_regs(struct vfe_device *vfe, unsigned int wm,
			     u32 counter,
			     struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	unsigned int n = 0;

	if (vfe_is_lite(vfe) || counter > CAMSS_HW_WM_BW_LIMIT_MAX)
		return -EINVAL;

	VFE_WM_REG(regs, VFE_BUS_WM_BW_LIMIT(wm), counter ? counter << 1 | 1 : 0);

	return n;
}

int vfe_780_wm_stop_regs(struct vfe_device *vfe, unsigned int wm,
			 struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	unsigned int n = 0;

	VFE_WM_REG(regs, VFE_BUS_WM_CFG(wm), 0);

	return n;
}

void vfe_780_write_regs(struct vfe_device *vfe, const struct camss_hw_reg *regs,
			unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		writel(regs[i].value, vfe->base + regs[i].offset);
}

u32 vfe_780_wm_last_addr(struct vfe_device *vfe, unsigned int wm)
{
	return readl(vfe->base + VFE_BUS_WM_ADDR_STATUS_0(wm));
}

void vfe_780_in_start(struct vfe_device *vfe, bool camif, u32 epoch_line)
{
	writel(TOP_DEBUG_CFG_EN, vfe->base + VFE_TOP_DEBUG_CFG);
	if (camif)
		writel(epoch_line, vfe->base + VFE_TOP_EPOCH_HEIGHT_CFG);
}

void vfe_780_in_stop(struct vfe_device *vfe)
{
	if (!vfe_is_lite(vfe))
		writel(PDAF_INPUT_CFG_1_DEFAULT, vfe->base + VFE_TOP_PDAF_INPUT_CFG_1);
}

int vfe_780_pdaf_lcr_regs(struct vfe_device *vfe, unsigned int rdi, bool init,
			  struct camss_hw_reg regs[2])
{
	unsigned int n = 0;

	if (init) {
		regs[n].offset = VFE_TOP_PDAF_INPUT_CFG_1;
		regs[n++].value = PDAF_INPUT_CFG_1_LCR;
	}
	regs[n].offset = VFE_TOP_PDAF_INPUT_CFG_0;
	regs[n++].value = rdi;

	return n;
}

int vfe_780_fcg_phase_regs(struct vfe_device *vfe, u16 r, u16 g, u32 b,
			   struct camss_hw_reg regs[2])
{
	regs[0].offset = VFE_FCG_PHASE_INDEX_CFG_0;
	regs[0].value = (u32)g << FCG_PHASE_INDEX_G | r;
	regs[1].offset = VFE_FCG_PHASE_INDEX_CFG_1;
	regs[1].value = b;

	return 2;
}

void vfe_780_bus_status(struct vfe_device *vfe, u32 *overflow, u32 *violation)
{
	*overflow = readl(vfe->base + VFE_BUS_OVERFLOW_STATUS);
	*violation = readl(vfe->base + VFE_BUS_VIOLATION_STATUS);
}

void vfe_780_err_irq_mask(struct vfe_device *vfe, unsigned int errs)
{
	u32 top = 0;

	if (errs & CAMSS_HW_VFE_ERR_CAMIF)
		top |= TOP_IRQ_CAMIF_ERR;
	if (errs & CAMSS_HW_VFE_ERR_BUS)
		top |= TOP_IRQ_BUS;

	writel(errs & CAMSS_HW_VFE_ERR_BUS ? BUS_IRQ_ERR : 0,
	       vfe->base + VFE_BUS_IRQ_MASK(0));
	writel(top, vfe->base + VFE_TOP_IRQ_MASK(0));
}

void vfe_780_err_irq_init(struct vfe_device *vfe)
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		writel(0, vfe->base + VFE_TOP_IRQ_MASK(i));
		writel(0xFFFFFFFF, vfe->base + VFE_TOP_IRQ_CLEAR(i));
	}

	for (i = 0; i < VFE_BUS_IRQ_REGS; i++) {
		writel(0, vfe->base + VFE_BUS_IRQ_MASK(i));
		writel(0xFFFFFFFF, vfe->base + VFE_BUS_IRQ_CLEAR(i));
	}

	writel(BUS_IRQ_CMD_CLEAR, vfe->base + VFE_BUS_IRQ_CMD);
	writel(TOP_IRQ_CMD_CLEAR, vfe->base + VFE_TOP_IRQ_CMD);
}

/*
 * Mask each reported source until the consumer enables it again, as
 * cam_vfe_top_ver4 and cam_vfe_bus_ver3 drop their error IRQs until the
 * pipe restarts. A pixel pipe error masks the bus errors too, as
 * cam_vfe_top_ver4 disables the whole top controller.
 */
static irqreturn_t vfe_780_isr_claimed(struct vfe_device *vfe,
				       struct camss_hw_pipe *pipe)
{
	struct camss_hw_vfe_err err = {};
	unsigned int wm;
	u32 top, bus = 0;

	top = readl(vfe->base + VFE_TOP_IRQ_STATUS_0);
	if (top & TOP_IRQ_BUS) {
		bus = readl(vfe->base + VFE_BUS_IRQ_STATUS_0);
		writel(bus, vfe->base + VFE_BUS_IRQ_CLEAR(0));
		writel(BUS_IRQ_CMD_CLEAR, vfe->base + VFE_BUS_IRQ_CMD);
	}
	writel(top, vfe->base + VFE_TOP_IRQ_CLEAR(0));
	writel(TOP_IRQ_CMD_CLEAR, vfe->base + VFE_TOP_IRQ_CMD);

	if (!vfe_is_lite(vfe))
		err.top = top & TOP_IRQ_CAMIF_ERR;
	err.bus = bus & BUS_IRQ_ERR;
	if (!err.top && !err.bus)
		return IRQ_HANDLED;

	if (err.bus & (BUS_IRQ_IMAGE_SIZE | BUS_IRQ_CONSTRAINT)) {
		err.image_size_wms = readl(vfe->base + VFE_BUS_IMAGE_SIZE_VIOLATION);
		for (wm = 0; !err.image_size_wms && wm < pipe->wm_num; wm++)
			if (readl(vfe->base + VFE_BUS_WM_DEBUG_STATUS_1(wm)))
				err.constraint_wms |= BIT(wm);
	}
	if (err.bus & BUS_IRQ_CCIF)
		err.ccif_wms = readl(vfe->base + VFE_BUS_VIOLATION_STATUS);

	spin_lock(&pipe->lock);
	if (err.top)
		pipe->vfe_errs = 0;
	else if (err.bus)
		pipe->vfe_errs &= ~CAMSS_HW_VFE_ERR_BUS;
	vfe_780_err_irq_mask(vfe, pipe->vfe_errs);
	spin_unlock(&pipe->lock);

	if (pipe->ops->vfe_error)
		pipe->ops->vfe_error(pipe->priv, &err);

	return IRQ_HANDLED;
}

void vfe_780_ubwc_static(struct vfe_device *vfe)
{
	writel(readl(vfe->base + VFE_BUS_UBWC_STATIC_CTRL) | UBWC_STATIC_CTRL_LPDDR5,
	       vfe->base + VFE_BUS_UBWC_STATIC_CTRL);
}

static void vfe_780_wm_start(struct vfe_device *vfe, unsigned int wm,
			     const struct camss_hw_wm_cfg *cfg)
{
	struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS];
	int n;

	n = vfe_780_wm_start_regs(vfe, wm, cfg, regs);
	if (n > 0)
		vfe_780_write_regs(vfe, regs, n);
}

static void vfe_780_output_wm_cfg(struct vfe_output *output, unsigned int i,
				  struct camss_hw_wm_cfg *cfg)
{
	struct v4l2_pix_format_mplane *pix = &output->video_out.active_fmt.fmt.pix_mp;
	u8 plane = output->wm[i].plane;

	memset(cfg, 0, sizeof(*cfg));

	if (output->pad == MSM_VFE_PAD_SRC_STATS) {
		cfg->mode = CAMSS_HW_WM_FRAME;
		cfg->stride = 1;
		cfg->packer = VFE_BUS_WM_PACKER_FMT_V3_PLAIN_64;
	} else if (output->line->is_pix) {
		cfg->mode = CAMSS_HW_WM_LINE;
		cfg->width = pix->width;
		cfg->height = pix->height >> plane;
		cfg->stride = pix->plane_fmt[0].bytesperline;
		cfg->slice_height = pix->height >> plane;
		cfg->packer = VFE_BUS_WM_PACKER_FMT_V3_PLAIN_8_LSB_MSB_10;
	} else {
		cfg->mode = CAMSS_HW_WM_FRAME;
		cfg->width = WM_IMAGE_CFG_0_DEFAULT_WIDTH;
		cfg->stride = ALIGN(pix->plane_fmt[0].bytesperline, 16);
		cfg->slice_height = pix->height;
	}
}

static void vfe_output_start(struct vfe_device *vfe, struct vfe_output *output)
{
	struct camss_hw_wm_cfg cfg;
	unsigned int i;

	/* no clock gating at bus input */
	writel(WM_CGC_OVERRIDE_ALL, vfe->base + VFE_BUS_WM_CGC_OVERRIDE);

	writel(0x0, vfe->base + VFE_BUS_WM_TEST_BUS_CTRL);

	for (i = 0; i < output->wm_num; i++) {
		vfe_780_output_wm_cfg(output, i, &cfg);
		vfe_780_wm_start(vfe, output->wm[i].bus_client, &cfg);
	}
}

static void vfe_output_stop(struct vfe_device *vfe, struct vfe_output *output)
{
	struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS];
	unsigned int i;
	int n;

	for (i = 0; i < output->wm_num; i++) {
		n = vfe_780_wm_stop_regs(vfe, output->wm[i].bus_client, regs);
		vfe_780_write_regs(vfe, regs, n);
	}
}

static void vfe_output_update(struct vfe_device *vfe, struct vfe_output *output,
			      struct camss_buffer *buf)
{
	struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS];
	struct camss_hw_wm_cfg cfg;
	unsigned int i;

	for (i = 0; i < output->wm_num; i++) {
		u8 wm = output->wm[i].bus_client;
		dma_addr_t addr;
		int n;

		/*
		 * Statistics land in the output's kernel buffer, handed over
		 * once per queued buffer. camss-video sets addr[1] = addr[0] +
		 * bytesperline * height for NV12.
		 */
		if (output->pad == MSM_VFE_PAD_SRC_STATS)
			addr = output->dma_addr;
		else
			addr = buf->addr[output->wm[i].plane];

		vfe_780_output_wm_cfg(output, i, &cfg);
		n = vfe_780_wm_update_regs(vfe, wm, &cfg, addr, 0, regs);
		if (n > 0)
			vfe_780_write_regs(vfe, regs, n);

		dev_dbg(vfe->camss->dev, "wm:%d, image buf addr:%pad\n",
			wm, &addr);
	}
}

/* The PIX line is latched through the CSID's IPP path, RDIs through their own */
static int vfe_reg_update_port(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	return vfe->line[line_id].is_pix ? CSID_780_CUSTOM_PORT_ID_IPP0 : line_id;
}

static void vfe_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	camss_reg_update(vfe->camss, vfe->id, vfe_reg_update_port(vfe, line_id), false);
}

static inline void vfe_reg_update_clear(struct vfe_device *vfe,
					enum vfe_line_id line_id)
{
	camss_reg_update(vfe->camss, vfe->id, vfe_reg_update_port(vfe, line_id), true);
}

/*
 * Program the pixel pipe outside output_lock, before the write masters and
 * the IPP reg update that latches both.
 */
static void vfe_line_prepare(struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix = &line->output[0].video_out.active_fmt.fmt.pix_mp;

	if (line->is_pix && pix->pixelformat == V4L2_PIX_FMT_NV12)
		vfe_880_setup_pixel_pipe(to_vfe(line), pix->width, pix->height);
}

static const struct camss_video_ops vfe_video_ops_780 = {
	.queue_buffer = vfe_queue_buffer_v2,
	.flush_buffers = vfe_flush_buffers,
	.prepare_buffer = vfe_params_prepare,
};

/* Blocks the IFE does not implement, the OPE's among them */
static int vfe_params_block_unsupported(struct device *dev,
					const struct v4l2_isp_block_header *block)
{
	dev_dbg(dev, "Unsupported parameters block type %u\n", block->type);

	return -EINVAL;
}

static int vfe_params_aec_bhist_validate(struct device *dev,
					 const struct v4l2_isp_block_header *block)
{
	const struct camss_params_aec_bhist *bhist =
		container_of(block, struct camss_params_aec_bhist, header);

	if ((bhist->left | bhist->top | bhist->width | bhist->height) & 1 ||
	    bhist->width < 2 || bhist->height < 2) {
		dev_dbg(dev, "Invalid AEC BHIST region %ux%u@%u,%u\n",
			bhist->width, bhist->height, bhist->left, bhist->top);
		return -EINVAL;
	}

	return 0;
}

/*
 * Every type up to the last one the IFE accepts has an entry, so that no
 * block passes validation with a size of zero.
 */
static const struct v4l2_isp_params_block_type_info vfe_params_info[] = {
	[0] = {
		.size = sizeof(struct v4l2_isp_params_block_header),
		.block_validate = vfe_params_block_unsupported,
	},
	[CAMSS_PARAMS_WB_GAIN] = {
		.size = sizeof(struct camss_params_wb_gain),
		.block_validate = vfe_params_block_unsupported,
	},
	[CAMSS_PARAMS_CHROMA_ENHAN] = {
		.size = sizeof(struct camss_params_chroma_enhan),
		.block_validate = vfe_params_block_unsupported,
	},
	[CAMSS_PARAMS_COLOR_CORRECT] = {
		.size = sizeof(struct camss_params_color_correct),
		.block_validate = vfe_params_block_unsupported,
	},
	[CAMSS_PARAMS_GAMMA] = {
		.size = sizeof(struct camss_params_gamma),
		.block_validate = vfe_params_block_unsupported,
	},
	[CAMSS_PARAMS_AEC_BHIST] = {
		.size = sizeof(struct camss_params_aec_bhist),
		.block_validate = vfe_params_aec_bhist_validate,
	},
};

/* Clip the region to the Bayer frame entering the line, then count quads */
static void vfe_params_aec_bhist(struct vfe_device *vfe, struct vfe_line *line,
				 const struct camss_params_aec_bhist *bhist)
{
	u32 frame_w = line->fmt[MSM_VFE_PAD_SINK].width & ~1u;
	u32 frame_h = line->fmt[MSM_VFE_PAD_SINK].height & ~1u;
	u32 left, top, width, height;

	if (frame_w < 2 || frame_h < 2)
		return;

	left = min_t(u32, bhist->left, frame_w - 2);
	top = min_t(u32, bhist->top, frame_h - 2);
	width = min_t(u32, bhist->width, frame_w - left);
	height = min_t(u32, bhist->height, frame_h - top);

	vfe_880_set_aec_bhist_region(vfe, left / 2, top / 2,
				     width / 2 - 1, height / 2 - 1);
}

static void vfe_params_apply(struct vfe_device *vfe, struct vfe_line *line,
			     const struct v4l2_isp_buffer *params)
{
	size_t offset = 0;

	/* validated at prepare: every block fits and has a non-zero size */
	while (offset < params->data_size) {
		const struct v4l2_isp_block_header *block =
			(const struct v4l2_isp_block_header *)&params->data[offset];

		if (block->type == CAMSS_PARAMS_AEC_BHIST &&
		    !(block->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE))
			vfe_params_aec_bhist(vfe, line,
					     container_of(block,
							  struct camss_params_aec_bhist,
							  header));

		offset += block->size;
	}
}
static void vfe_subdev_init(struct device *dev, struct vfe_device *vfe)
{
	struct vfe_line *pix = &vfe->line[VFE_LINE_PIX];

	vfe->video_ops = vfe_video_ops_780;

	/* RDI lite */
	if (vfe_is_lite(vfe)) {
		vfe_add_rdi(&vfe->line[VFE_LINE_RDI0].output[0], VFE_LITE_WM_RDI0, VFE_V3_COMP_GRP_1);
		vfe_add_rdi(&vfe->line[VFE_LINE_RDI1].output[0], VFE_LITE_WM_RDI1, VFE_V3_COMP_GRP_2);
		vfe_add_rdi(&vfe->line[VFE_LINE_RDI2].output[0], VFE_LITE_WM_RDI2, VFE_V3_COMP_GRP_3);
		vfe_add_rdi(&pix->output[0], VFE_LITE_WM_RDI3, VFE_V3_COMP_GRP_4);
		return;
	}

	/* RDI */
	vfe_add_rdi(&vfe->line[VFE_LINE_RDI0].output[0], VFE_WM_RDI0, VFE_V3_COMP_GRP_14);
	vfe_add_rdi(&vfe->line[VFE_LINE_RDI1].output[0], VFE_WM_RDI1, VFE_V3_COMP_GRP_15);
	vfe_add_rdi(&vfe->line[VFE_LINE_RDI2].output[0], VFE_WM_RDI2, VFE_V3_COMP_GRP_16);

	/* Add YUV, DS4, DS16, stats and params descriptors */
	vfe_add_yuv(&pix->output[0], VFE_WM_VIDEO_FULL_Y, VFE_WM_VIDEO_FULL_C, VFE_V3_COMP_GRP_0);
	vfe_add_ds(&pix->output[1], MSM_VFE_PAD_SRC_DS4, VFE_WM_VIDEO_DS_4, VFE_V3_COMP_GRP_0);
	vfe_add_ds(&pix->output[2], MSM_VFE_PAD_SRC_DS16, VFE_WM_VIDEO_DS_16, VFE_V3_COMP_GRP_0);
	vfe_add_stats(&pix->output[3], VFE_WM_STATS_AEC_BHIST, VFE_V3_COMP_GRP_4);
	if (vfe_alloc_stats(vfe, &pix->output[3]))
		pix->output[3].wm_num = 0;	/* no statistics without their buffer */
	vfe_add_params(&pix->output[4], VFE_V3_COMP_GRP_0);

	/* PIX mode */
	vfe_set_pix(pix, 5);
}

static void vfe_global_reset(struct vfe_device *vfe)
{
	vfe_isr_reset_ack(vfe);
}

static irqreturn_t vfe_isr(int irq, void *dev)
{
	struct vfe_device *vfe = dev;
	struct camss_hw_pipe *pipe = camss_hw_vfe_pipe(vfe);

	if (pipe)
		return vfe_780_isr_claimed(vfe, pipe);

	return IRQ_HANDLED;
}

static int vfe_halt(struct vfe_device *vfe)
{
	/* rely on vfe_disable_output() to stop the VFE */
	return 0;
}

const struct vfe_hw_ops vfe_ops_780 = {
	.global_reset = vfe_global_reset,
	.hw_version = vfe_hw_version,
	.isr = vfe_isr,
	.pm_domain_off = vfe_pm_domain_off,
	.pm_domain_on = vfe_pm_domain_on,
	.reg_update = vfe_reg_update,
	.reg_update_clear = vfe_reg_update_clear,
	.subdev_init = vfe_subdev_init,
	.vfe_disable = vfe_disable,
	.vfe_enable = vfe_enable_v2,
	.vfe_line_prepare = vfe_line_prepare,
	.params_info = vfe_params_info,
	.params_num_types = ARRAY_SIZE(vfe_params_info),
	.vfe_params_apply = vfe_params_apply,
	.vfe_halt = vfe_halt,
	.vfe_buf_done = vfe_buf_done,
	.vfe_output_start = vfe_output_start,
	.vfe_output_stop = vfe_output_stop,
	.vfe_output_update = vfe_output_update,
};
