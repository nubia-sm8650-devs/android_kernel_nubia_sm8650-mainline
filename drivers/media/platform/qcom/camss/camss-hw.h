/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Qualcomm MSM Camera Subsystem - hardware access for drivers outside V4L2
 */
#ifndef QC_MSM_CAMSS_HW_H
#define QC_MSM_CAMSS_HW_H

#include <linux/bits.h>
#include <linux/types.h>

#define CAMSS_HW_WM_MAX_REGS	24

#define CAMSS_HW_WM_UPDATE_STRIDE	BIT(0)
#define CAMSS_HW_WM_UPDATE_PACKER	BIT(1)
#define CAMSS_HW_WM_UPDATE_UBWC		BIT(2)

struct camss_hw_reg {
	u32 offset;
	u32 value;
};

enum camss_hw_wm_mode {
	CAMSS_HW_WM_LINE,
	CAMSS_HW_WM_FRAME,
	CAMSS_HW_WM_INDEX,
};

struct camss_hw_wm_ubwc {
	u32 meta_cfg;
	u32 mode_cfg;
	u32 ctrl_2;
	u32 lossy_thresh0;
	u32 lossy_thresh1;
	u32 off_lossy_var;
	u32 bw_limit;
	u32 meta_size;
};

struct camss_hw_wm_cfg {
	enum camss_hw_wm_mode mode;
	bool virtual_frame;
	bool frame_header;
	u32 width;
	u32 height;
	u32 stride;
	u32 slice_height;
	u32 h_init;
	u32 offset;
	u32 packer;
	bool ubwc;
	struct camss_hw_wm_ubwc ubwc_cfg;
};

#endif /* QC_MSM_CAMSS_HW_H */
