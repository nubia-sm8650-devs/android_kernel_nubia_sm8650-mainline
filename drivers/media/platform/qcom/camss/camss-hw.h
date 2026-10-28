/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Qualcomm MSM Camera Subsystem - hardware access for drivers outside V4L2
 */
#ifndef QC_MSM_CAMSS_HW_H
#define QC_MSM_CAMSS_HW_H

#include <linux/bits.h>
#include <linux/types.h>

struct camss;
struct camss_hw_pipe;
struct device;
struct device_node;

enum camss_hw_path {
	CAMSS_HW_PATH_IPP,
	CAMSS_HW_PATH_RDI0,
	CAMSS_HW_PATH_RDI1,
	CAMSS_HW_PATH_RDI2,
	CAMSS_HW_PATH_RDI3,
	CAMSS_HW_PATH_RDI4,
	CAMSS_HW_PATH_PPP,
	CAMSS_HW_PATH_NUM,
};

#define CAMSS_HW_COMP_NUM	16
#define CAMSS_HW_SFE_DONE_NUM	11
#define CAMSS_HW_WM_NUM		28
#define CAMSS_HW_WM_MAX_REGS	24
#define CAMSS_HW_WM_BW_LIMIT_MAX	0xff

#define CAMSS_HW_WM_UPDATE_STRIDE	BIT(0)
#define CAMSS_HW_WM_UPDATE_PACKER	BIT(1)
#define CAMSS_HW_WM_UPDATE_UBWC		BIT(2)

enum camss_hw_block {
	CAMSS_HW_CSID,
	CAMSS_HW_IFE,
	CAMSS_HW_CSID_WRAPPER,
};

enum camss_hw_err_src {
	CAMSS_HW_ERR_TOP,
	CAMSS_HW_ERR_RX,
	CAMSS_HW_ERR_PATH,
};

#define CAMSS_HW_EV_SOF		BIT(0)
#define CAMSS_HW_EV_EOF		BIT(1)
#define CAMSS_HW_EV_EPOCH	BIT(2)
#define CAMSS_HW_EV_RUP		BIT(3)
/* data reached the path, raised while the path is halted too */
#define CAMSS_HW_EV_INPUT_SOF	BIT(4)

#define CAMSS_HW_VFE_ERR_CAMIF	BIT(0)
#define CAMSS_HW_VFE_ERR_BUS	BIT(1)

struct camss_hw_reg {
	u32 offset;
	u32 value;
};

/**
 * struct camss_hw_vfe_err - VFE error status
 * @top: pixel pipe errors in top status 0, cam_vfe880.h error_irq_mask bits
 * @bus: bus status 0 errors, 31 image size, 30 CCIF and 28 constraint
 * @image_size_wms: write masters with an image size violation
 * @ccif_wms: write masters with a CCIF violation
 * @constraint_wms: write masters with a constraint violation, read only
 *                  when @image_size_wms is empty
 */
struct camss_hw_vfe_err {
	u32 top;
	u32 bus;
	u32 image_size_wms;
	u32 ccif_wms;
	u32 constraint_wms;
};

struct camss_hw_ops {
	void (*sof)(void *priv, enum camss_hw_path path, u64 qtimer_ns,
		    u64 boot_ns);
	void (*input_sof)(void *priv, enum camss_hw_path path, u64 boot_ns);
	void (*epoch)(void *priv, enum camss_hw_path path, u64 boot_ns);
	void (*eof)(void *priv, enum camss_hw_path path, u64 boot_ns);
	void (*rup_done)(void *priv, enum camss_hw_path path, u64 boot_ns);
	void (*buf_done)(void *priv, unsigned int comp, unsigned int wm,
			 u32 last_addr, u64 boot_ns);
	void (*sfe_done)(void *priv, u32 lines, u64 boot_ns);
	void (*error)(void *priv, enum camss_hw_err_src src,
		      enum camss_hw_path path, u32 status, bool fatal);
	void (*vfe_error)(void *priv, const struct camss_hw_vfe_err *err);
	void (*vfe_frame)(void *priv, u32 events, u64 sof_ns, u64 boot_ns);
};

struct camss_hw_rx_cfg {
	u8 phy;
	u8 lanes;
	u16 lane_cfg;
	bool cphy;
	bool epd;
	bool vc_full_width;
	bool dyn_switch;	/* MUP switches the sensor mode, no RUP latch */
};

enum camss_hw_sync {
	CAMSS_HW_SYNC_NONE,
	CAMSS_HW_SYNC_MASTER,
	CAMSS_HW_SYNC_SLAVE,
};

/**
 * struct camss_hw_csid_route - where a full CSID sends its paths
 * @sfe: SFE whose output feeds the VFE, -1 for a VFE fed by the CSID
 * @vfe_out: the CSID drives the VFE, false when only the SFE consumes it
 * @sfe_offline: the SFE reads its input back from memory
 * @lcr_rdis: RDI paths sent to the PDAF LCR through the SFE, bit n for
 *            CAMSS_HW_PATH_RDI0 + n
 */
struct camss_hw_csid_route {
	int sfe;
	bool vfe_out;
	bool sfe_offline;
	u8 lcr_rdis;
};

struct camss_hw_path_cfg {
	u8 vc;
	u8 dt;
	u8 cid;
	u8 decode_format;
	u8 plain_format;
	bool packing_mipi;
	bool plain_align;
	bool pix_store;
	bool crop;
	bool drop;
	bool vfr;
	bool frame_id_dec;
	bool bin_h;
	bool bin_v;
	bool bin_qcfa;
	bool lcr;	/* RDI0 feeds the VFE PDAF LCR input, the PPP follows it */
	enum camss_hw_sync sync;
	u32 epoch_line;
	u32 events;
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

struct camss_hw_wm_hfr {
	u32 framedrop_pattern;
	u32 framedrop_period;
	u32 subsample_pattern;
	u32 subsample_period;
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

struct camss *camss_hw_get(struct device *consumer, struct device_node *np);

struct camss_hw_pipe *camss_hw_claim(struct camss *camss, unsigned int index,
				     const struct camss_hw_ops *ops,
				     void *priv);
void camss_hw_release(struct camss_hw_pipe *pipe);

int camss_hw_csid_rx(struct camss_hw_pipe *pipe,
		     const struct camss_hw_rx_cfg *cfg);
int camss_hw_path_config(struct camss_hw_pipe *pipe, enum camss_hw_path path,
			 const struct camss_hw_path_cfg *cfg);
int camss_hw_csid_dual(struct camss_hw_pipe *pipe, enum camss_hw_sync sync,
		       unsigned int peer);
int camss_hw_csid_route(struct camss_hw_pipe *pipe,
			const struct camss_hw_csid_route *route);
int camss_hw_path_resume(struct camss_hw_pipe *pipe, enum camss_hw_path path);
int camss_hw_path_resume_global(struct camss_hw_pipe *pipe,
				enum camss_hw_path path);
void camss_hw_csid_global_start(struct camss_hw_pipe *pipe);
int camss_hw_csid_stop(struct camss_hw_pipe *pipe);
int camss_hw_path_sof_ts(struct camss_hw_pipe *pipe, enum camss_hw_path path,
			 bool prev, u64 *ns);
int camss_hw_set_clk_rate(struct camss_hw_pipe *pipe, enum camss_hw_block block,
			  unsigned long rate, unsigned long *applied);
struct camss_hw_reg camss_hw_rup_reg(struct camss_hw_pipe *pipe,
				     unsigned long paths, bool mup);
void camss_hw_rup(struct camss_hw_pipe *pipe, unsigned long paths, bool mup);

int camss_hw_vfe_in_start(struct camss_hw_pipe *pipe, bool camif,
			  u32 epoch_line);
void camss_hw_vfe_in_stop(struct camss_hw_pipe *pipe);
void camss_hw_vfe_err_enable(struct camss_hw_pipe *pipe, unsigned int errs,
			     bool enable);
void camss_hw_vfe_sync(struct camss_hw_pipe *pipe);
int camss_hw_vfe_frame_enable(struct camss_hw_pipe *pipe, u32 events,
			      enum camss_hw_path ts_path);
void camss_hw_bus_status(struct camss_hw_pipe *pipe, u32 *overflow,
			 u32 *violation);
int camss_hw_vfe_pdaf_lcr_regs(struct camss_hw_pipe *pipe, unsigned int rdi,
			       bool init, struct camss_hw_reg regs[2]);
int camss_hw_vfe_fcg_phase_regs(struct camss_hw_pipe *pipe, u32 r, u32 g,
				u32 b, struct camss_hw_reg regs[2]);

int camss_hw_comp_start(struct camss_hw_pipe *pipe, unsigned int comp,
			unsigned int wm);
void camss_hw_comp_stop(struct camss_hw_pipe *pipe, unsigned int comp,
			unsigned int wm);
int camss_hw_sfe_done_enable(struct camss_hw_pipe *pipe, u32 lines);

int camss_hw_wm_start_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			   const struct camss_hw_wm_cfg *cfg,
			   struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS]);
int camss_hw_wm_update_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			    const struct camss_hw_wm_cfg *cfg, u64 iova,
			    unsigned int flags,
			    struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS]);
int camss_hw_wm_frame_header_regs(struct camss_hw_pipe *pipe, unsigned int wm,
				  u64 iova, u32 id,
				  struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS]);
int camss_hw_wm_hfr_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			 const struct camss_hw_wm_hfr *hfr,
			 struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS]);
int camss_hw_wm_bw_limit_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			      u32 counter,
			      struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS]);
int camss_hw_wm_stop_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			  struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS]);
u32 camss_hw_wm_last_addr(struct camss_hw_pipe *pipe, unsigned int wm);
int camss_hw_read_reg(struct camss_hw_pipe *pipe, enum camss_hw_block block,
		      u32 offset, u32 *val);
int camss_hw_write_regs(struct camss_hw_pipe *pipe, enum camss_hw_block block,
			const struct camss_hw_reg *regs, unsigned int n);

#endif /* QC_MSM_CAMSS_HW_H */
