// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm MSM Camera Subsystem - hardware access for drivers outside V4L2
 */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "camss.h"
#include "camss-csid.h"
#include "camss-csid-gen3.h"
#include "camss-hw.h"
#include "camss-vfe.h"

struct camss *camss_hw_get(struct device *consumer, struct device_node *np)
{
	struct platform_device *pdev;
	struct device_link *link;
	struct camss *camss;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	camss = camss_from_device(&pdev->dev);
	if (!camss) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	link = device_link_add(consumer, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&pdev->dev);
	if (!link)
		return ERR_PTR(-EINVAL);

	return camss;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_get, "QCOM_CAMSS");

bool camss_hw_csid_claimed(struct csid_device *csid)
{
	return csid->claim;
}

bool camss_hw_vfe_claimed(struct vfe_device *vfe)
{
	struct camss *camss = vfe->camss;

	return vfe->id < camss->res->csid_num && camss->csid[vfe->id].claim;
}

static bool camss_hw_vfe_busy(struct vfe_device *vfe)
{
	unsigned int i;
	bool busy;

	for (i = 0; i < vfe->res->line_num; i++)
		if (vfe->line[i].subdev.entity.use_count)
			return true;

	mutex_lock(&vfe->power_lock);
	busy = vfe->power_count;
	mutex_unlock(&vfe->power_lock);

	return busy;
}

static void camss_hw_wm_disable_all(struct camss_hw_pipe *pipe)
{
	struct camss_hw_reg reg;
	unsigned int wm;

	for (wm = 0; wm < pipe->wm_num; wm++)
		if (vfe_780_wm_stop_regs(pipe->vfe, wm, &reg) == 1)
			vfe_780_write_regs(pipe->vfe, &reg, 1);
}

/**
 * camss_hw_claim - take a CSID and its VFE from their V4L2 subdevices
 * @camss: CAMSS device
 * @index: index of the CSID and the VFE
 * @ops: event callbacks
 * @priv: argument passed to @ops
 *
 * The ops run from the hard interrupt handler of the CSID once the claim
 * returns, and must not sleep.
 *
 * Return: the claimed pipe, an ERR_PTR() otherwise
 */
struct camss_hw_pipe *camss_hw_claim(struct camss *camss, unsigned int index,
				     const struct camss_hw_ops *ops,
				     void *priv)
{
	struct platform_device *pdev = to_platform_device(camss->dev);
	struct camss_hw_pipe *pipe;
	struct csid_device *csid;
	struct vfe_device *vfe;
	struct resource *res;
	int ret;

	if (index >= camss->res->csid_num || index >= camss->res->vfe_num)
		return ERR_PTR(-EINVAL);

	csid = &camss->csid[index];
	vfe = &camss->vfe[index];
	if (csid->res->hw_ops != &csid_ops_gen3 || vfe->res->hw_ops != &vfe_ops_780 ||
	    csid_is_lite(csid) != vfe_is_lite(vfe))
		return ERR_PTR(-EOPNOTSUPP);

	pipe = kzalloc(sizeof(*pipe), GFP_KERNEL);
	if (!pipe)
		return ERR_PTR(-ENOMEM);

	pipe->camss = camss;
	pipe->csid = csid;
	pipe->vfe = vfe;
	pipe->ops = ops;
	pipe->priv = priv;
	pipe->path_num = csid_gen3_hw_path_num(csid);
	pipe->comp_num = vfe_780_comp_num(vfe);
	pipe->wm_num = vfe_780_wm_num(vfe);
	spin_lock_init(&pipe->lock);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   camss->res->csid_res[index].reg[0]);
	pipe->csid_size = res ? resource_size(res) : 0;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   camss->res->vfe_res[index].reg[0]);
	pipe->vfe_size = res ? resource_size(res) : 0;
	if (camss->res->csid_wrapper_res && !csid_is_lite(csid)) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   camss->res->csid_wrapper_res->reg);
		pipe->wrapper_size = res ? resource_size(res) : 0;
	}

	mutex_lock(&camss->media_dev.graph_mutex);

	if (csid->claim || csid->subdev.entity.use_count ||
	    camss_hw_vfe_busy(vfe)) {
		ret = -EBUSY;
		goto err_unlock;
	}

	csid->claim = pipe;

	ret = csid_power(csid, true);
	if (ret < 0) {
		csid->claim = NULL;
		goto err_unlock;
	}

	csid_gen3_hw_init(pipe);
	camss_hw_wm_disable_all(pipe);
	if (!vfe_is_lite(vfe))
		vfe_780_ubwc_static(vfe);
	smp_store_release(&vfe->claim, pipe);

	mutex_unlock(&camss->media_dev.graph_mutex);

	return pipe;

err_unlock:
	mutex_unlock(&camss->media_dev.graph_mutex);
	kfree(pipe);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_claim, "QCOM_CAMSS");

static struct camss_clock *camss_hw_clk(struct camss_hw_pipe *pipe,
					enum camss_hw_block block)
{
	struct camss_clock *clocks;
	char name[16];
	int i, nclocks;

	switch (block) {
	case CAMSS_HW_CSID:
		clocks = pipe->csid->clock;
		nclocks = pipe->csid->nclocks;
		strscpy(name, csid_is_lite(pipe->csid) ? "vfe_lite_csid" : "csid",
			sizeof(name));
		break;
	case CAMSS_HW_IFE:
		clocks = pipe->vfe->clock;
		nclocks = pipe->vfe->nclocks;
		if (vfe_is_lite(pipe->vfe))
			strscpy(name, "vfe_lite", sizeof(name));
		else
			snprintf(name, sizeof(name), "vfe%u", pipe->vfe->id);
		break;
	default:
		return NULL;
	}

	for (i = 0; i < nclocks; i++)
		if (!strcmp(clocks[i].name, name))
			return &clocks[i];

	return NULL;
}

static void camss_hw_clk_drop(struct camss_hw_pipe *pipe,
			      enum camss_hw_block block)
{
	struct camss_clock *clock = camss_hw_clk(pipe, block);

	if (clock)
		clk_set_min_rate(clock->clk, 0);
}

void camss_hw_release(struct camss_hw_pipe *pipe)
{
	struct camss *camss = pipe->camss;
	struct csid_device *csid = pipe->csid;

	mutex_lock(&camss->media_dev.graph_mutex);

	if (pipe->paths)
		csid_gen3_hw_stop(pipe);
	camss_hw_wm_disable_all(pipe);
	WRITE_ONCE(pipe->vfe->claim, NULL);
	csid_power(csid, false);
	csid->claim = NULL;
	camss_hw_clk_drop(pipe, CAMSS_HW_CSID);
	camss_hw_clk_drop(pipe, CAMSS_HW_IFE);

	mutex_unlock(&camss->media_dev.graph_mutex);

	kfree(pipe);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_release, "QCOM_CAMSS");

int camss_hw_csid_rx(struct camss_hw_pipe *pipe,
		     const struct camss_hw_rx_cfg *cfg)
{
	if (!cfg->lanes || cfg->lanes > 4 ||
	    cfg->phy >= pipe->camss->res->csiphy_num)
		return -EINVAL;

	csid_gen3_hw_rx(pipe, cfg);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_csid_rx, "QCOM_CAMSS");

int camss_hw_path_config(struct camss_hw_pipe *pipe, enum camss_hw_path path,
			 const struct camss_hw_path_cfg *cfg)
{
	if (path >= pipe->path_num || cfg->vc > 0x1f || cfg->dt > 0x3f ||
	    cfg->cid > 3 || cfg->decode_format > 0xf || cfg->plain_format > 3 ||
	    cfg->epoch_line > 0xffff)
		return -EINVAL;

	if (cfg->pix_store && path > CAMSS_HW_PATH_RDI0)
		return -EINVAL;

	if (path != CAMSS_HW_PATH_IPP &&
	    (cfg->sync || cfg->bin_h || cfg->bin_v || cfg->bin_qcfa))
		return -EINVAL;

	if (cfg->sync > CAMSS_HW_SYNC_SLAVE ||
	    (cfg->sync && csid_is_lite(pipe->csid)))
		return -EINVAL;

	csid_gen3_hw_path_config(pipe, path, cfg);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_path_config, "QCOM_CAMSS");

int camss_hw_path_resume(struct camss_hw_pipe *pipe, enum camss_hw_path path)
{
	if (path >= pipe->path_num || !(pipe->paths & BIT(path)))
		return -EINVAL;

	csid_gen3_hw_path_resume(pipe, path);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_path_resume, "QCOM_CAMSS");

/**
 * camss_hw_csid_stop - reset every configured path
 * @pipe: claimed pipe
 *
 * No ops callback runs once this returns, so it must not be called from
 * one.
 *
 * Return: 0 on success, -EIO if the reset timed out
 */
int camss_hw_csid_stop(struct camss_hw_pipe *pipe)
{
	return csid_gen3_hw_stop(pipe);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_csid_stop, "QCOM_CAMSS");

/**
 * camss_hw_path_sof_ts - read the SOF timestamp latched by a path
 * @pipe: claimed pipe
 * @path: configured path
 * @prev: read the previous frame's SOF instead of the current one
 * @ns: QTimer time in nanoseconds
 *
 * Return: 0 on success, -EINVAL for a path that is not configured
 */
int camss_hw_path_sof_ts(struct camss_hw_pipe *pipe, enum camss_hw_path path,
			 bool prev, u64 *ns)
{
	if (path >= pipe->path_num || !(pipe->paths & BIT(path)))
		return -EINVAL;

	*ns = csid_gen3_hw_path_sof_ts(pipe, path, prev);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_path_sof_ts, "QCOM_CAMSS");

/**
 * camss_hw_set_clk_rate - set the core clock of a claimed CSID or VFE
 * @pipe: claimed pipe
 * @block: CAMSS_HW_CSID or CAMSS_HW_IFE
 * @rate: minimum rate in Hz, rounded by the clock first
 * @applied: lowest supported rate not below the rounded @rate
 *
 * The CSIDs and the lite VFEs share their core clocks. The rate holds as a
 * minimum until the claim is released, so the clock runs at the highest rate
 * any claimed pipe asks for. @applied is the supported rate itself, the PLL
 * behind it can settle a few Hz off.
 *
 * Return: 0 on success, -ERANGE if the rounded @rate is above every supported
 * rate
 */
int camss_hw_set_clk_rate(struct camss_hw_pipe *pipe, enum camss_hw_block block,
			  unsigned long rate, unsigned long *applied)
{
	struct camss_clock *clock;
	long round;
	int i, ret;

	if (block != CAMSS_HW_CSID && block != CAMSS_HW_IFE)
		return -EINVAL;

	clock = camss_hw_clk(pipe, block);
	if (!clock)
		return -ENOENT;

	round = clk_round_rate(clock->clk, rate);
	if (round < 0)
		return round;

	for (i = 0; i < clock->nfreqs; i++)
		if (clock->freq[i] >= round)
			break;

	if (i == clock->nfreqs)
		return -ERANGE;

	round = clk_round_rate(clock->clk, clock->freq[i]);
	if (round < 0)
		return round;

	ret = clk_set_min_rate(clock->clk, round);
	if (ret)
		return ret;

	ret = clk_set_rate(clock->clk, round);
	if (ret)
		return ret;

	*applied = clock->freq[i];

	return 0;
}

EXPORT_SYMBOL_NS_GPL(camss_hw_set_clk_rate, "QCOM_CAMSS");

struct camss_hw_reg camss_hw_rup_reg(struct camss_hw_pipe *pipe,
				     unsigned long paths, bool mup)
{
	return csid_gen3_hw_rup_reg(paths & GENMASK(pipe->path_num - 1, 0), mup);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_rup_reg, "QCOM_CAMSS");

void camss_hw_rup(struct camss_hw_pipe *pipe, unsigned long paths, bool mup)
{
	struct camss_hw_reg reg = camss_hw_rup_reg(pipe, paths, mup);

	writel(reg.value, pipe->csid->base + reg.offset);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_rup, "QCOM_CAMSS");

int camss_hw_comp_start(struct camss_hw_pipe *pipe, unsigned int comp,
			unsigned int wm)
{
	if (comp >= pipe->comp_num || wm >= pipe->wm_num)
		return -EINVAL;

	csid_gen3_hw_buf_done(pipe, comp, wm, true);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_comp_start, "QCOM_CAMSS");

void camss_hw_comp_stop(struct camss_hw_pipe *pipe, unsigned int comp,
			unsigned int wm)
{
	if (comp >= pipe->comp_num || wm >= pipe->wm_num)
		return;

	csid_gen3_hw_buf_done(pipe, comp, wm, false);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_comp_stop, "QCOM_CAMSS");

int camss_hw_wm_start_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			   const struct camss_hw_wm_cfg *cfg,
			   struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	if (wm >= pipe->wm_num)
		return -EINVAL;

	return vfe_780_wm_start_regs(pipe->vfe, wm, cfg, regs);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_wm_start_regs, "QCOM_CAMSS");

int camss_hw_wm_update_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			    const struct camss_hw_wm_cfg *cfg, u64 iova,
			    unsigned int flags,
			    struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	if (wm >= pipe->wm_num)
		return -EINVAL;

	return vfe_780_wm_update_regs(pipe->vfe, wm, cfg, iova, flags, regs);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_wm_update_regs, "QCOM_CAMSS");

int camss_hw_wm_frame_header_regs(struct camss_hw_pipe *pipe, unsigned int wm,
				  u64 iova, u32 id,
				  struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	if (wm >= pipe->wm_num)
		return -EINVAL;

	return vfe_780_wm_frame_header_regs(pipe->vfe, wm, iova, id, regs);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_wm_frame_header_regs, "QCOM_CAMSS");

int camss_hw_wm_hfr_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			 const struct camss_hw_wm_hfr *hfr,
			 struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	if (wm >= pipe->wm_num)
		return -EINVAL;

	return vfe_780_wm_hfr_regs(pipe->vfe, wm, hfr, regs);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_wm_hfr_regs, "QCOM_CAMSS");

int camss_hw_wm_bw_limit_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			      u32 counter,
			      struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	if (wm >= pipe->wm_num)
		return -EINVAL;

	return vfe_780_wm_bw_limit_regs(pipe->vfe, wm, counter, regs);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_wm_bw_limit_regs, "QCOM_CAMSS");

u32 camss_hw_wm_last_addr(struct camss_hw_pipe *pipe, unsigned int wm)
{
	if (wm >= pipe->wm_num)
		return 0;

	return vfe_780_wm_last_addr(pipe->vfe, wm);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_wm_last_addr, "QCOM_CAMSS");

int camss_hw_wm_stop_regs(struct camss_hw_pipe *pipe, unsigned int wm,
			  struct camss_hw_reg regs[CAMSS_HW_WM_MAX_REGS])
{
	if (wm >= pipe->wm_num)
		return -EINVAL;

	return vfe_780_wm_stop_regs(pipe->vfe, wm, regs);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_wm_stop_regs, "QCOM_CAMSS");

/**
 * camss_hw_read_reg - read a register of a claimed CSID or VFE
 * @pipe: claimed pipe
 * @block: CAMSS_HW_CSID, CAMSS_HW_IFE, or CAMSS_HW_CSID_WRAPPER for the
 *         wrapper shared by the full CSIDs
 * @offset: register offset in the block
 * @val: value read
 *
 * Return: 0 on success, -EINVAL for an offset outside the block or for the
 * wrapper on a lite pipe
 */
int camss_hw_read_reg(struct camss_hw_pipe *pipe, enum camss_hw_block block,
		      u32 offset, u32 *val)
{
	switch (block) {
	case CAMSS_HW_CSID:
		if (offset & 3 || offset >= pipe->csid_size)
			return -EINVAL;
		*val = readl(pipe->csid->base + offset);
		return 0;
	case CAMSS_HW_IFE:
		if (offset & 3 || offset >= pipe->vfe_size)
			return -EINVAL;
		*val = readl(pipe->vfe->base + offset);
		return 0;
	case CAMSS_HW_CSID_WRAPPER:
		if (offset & 3 || offset >= pipe->wrapper_size)
			return -EINVAL;
		*val = readl(pipe->camss->csid_wrapper_base + offset);
		return 0;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_NS_GPL(camss_hw_read_reg, "QCOM_CAMSS");

int camss_hw_write_regs(struct camss_hw_pipe *pipe, enum camss_hw_block block,
			const struct camss_hw_reg *regs, unsigned int n)
{
	resource_size_t size;
	unsigned int i;

	switch (block) {
	case CAMSS_HW_CSID:
		size = pipe->csid_size;
		break;
	case CAMSS_HW_IFE:
		size = pipe->vfe_size;
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < n; i++)
		if (regs[i].offset & 3 || regs[i].offset >= size)
			return -EINVAL;

	if (block == CAMSS_HW_CSID)
		for (i = 0; i < n; i++)
			writel(regs[i].value, pipe->csid->base + regs[i].offset);
	else
		vfe_780_write_regs(pipe->vfe, regs, n);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_write_regs, "QCOM_CAMSS");
