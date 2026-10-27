// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Qualcomm Camera NOC (CAMNOC) interconnect provider.
 *
 * CAMNOC is the internal AXI interconnect within the Qualcomm camera
 * subsystem. Multiple camera sub-devices (IFE, JPEG, BPS, etc.) share
 * CAM_CC_CAMNOC_AXI_CLK. This driver acts as an ICC provider so that
 * each sub-device can independently vote for bandwidth; the ICC core
 * aggregates the votes (max of peak_bw across all consumers) and this
 * driver translates the result into a clk_set_rate() call, avoiding
 * the last-writer-wins race that occurs with direct clk_set_rate().
 *
 * Consumers express their required clock rate directly as peak_bw in
 * kBps (e.g. 400000 for 400 MHz).  The driver converts kBps → Hz:
 *   rate_hz = peak_bw_kBps * 1000
 */

#include <linux/clk.h>
#include <linux/interconnect-provider.h>
#include <linux/interconnect.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <dt-bindings/interconnect/qcom,camnoc.h>

#define to_camnoc_provider(_p) \
	container_of(_p, struct qcom_camnoc_icc_provider, provider)

struct qcom_camnoc_icc_provider {
	struct clk *clk;
	struct icc_provider provider;
};

struct qcom_camnoc_node {
	const char *name;
	u16 buswidth;
};

/* IFE */
static const struct qcom_camnoc_node camnoc_master_ife_0 = {
	.name = "master-camnoc-ife-0", .buswidth = 16
};

static const struct qcom_camnoc_node camnoc_master_ife_0_rdi = {
	.name = "master-camnoc-ife-0-rdi", .buswidth = 16
};

static const struct qcom_camnoc_node camnoc_master_ife_1 = {
	.name = "master-camnoc-ife-1", .buswidth = 16
};

static const struct qcom_camnoc_node camnoc_master_ife_1_rdi = {
	.name = "master-camnoc-ife-1-rdi", .buswidth = 16
};

static const struct qcom_camnoc_node camnoc_master_ife_lite_0 = {
	.name = "master-camnoc-ife-lite-0", .buswidth = 8
};

static const struct qcom_camnoc_node camnoc_master_ife_lite_1 = {
	.name = "master-camnoc-ife-lite-1", .buswidth = 8
};

static const struct qcom_camnoc_node camnoc_master_ife_lite_2 = {
	.name = "master-camnoc-ife-lite-2", .buswidth = 8
};

static const struct qcom_camnoc_node camnoc_master_ife_lite_3 = {
	.name = "master-camnoc-ife-lite-3", .buswidth = 8
};

static const struct qcom_camnoc_node camnoc_master_ife_lite_4 = {
	.name = "master-camnoc-ife-lite-4", .buswidth = 8
};

/* BPS */
static const struct qcom_camnoc_node camnoc_master_bps = {
	.name = "master-camnoc-bps", .buswidth = 16
};

/* IPE */
static const struct qcom_camnoc_node camnoc_master_ipe_0 = {
	.name = "master-camnoc-ipe-0", .buswidth = 16
};

static const struct qcom_camnoc_node camnoc_master_ipe_1 = {
	.name = "master-camnoc-ipe-1", .buswidth = 16
};

/* JPEG */
static const struct qcom_camnoc_node camnoc_master_jpeg = {
	.name = "master-camnoc-jpeg", .buswidth = 16
};

static const struct qcom_camnoc_node camnoc_master_jpeg_dma = {
	.name = "master-camnoc-jpeg-dma", .buswidth = 16
};

/* CDM */
static const struct qcom_camnoc_node camnoc_master_cdm = {
	.name = "master-camnoc-cdm", .buswidth = 4
};

/* FD */
static const struct qcom_camnoc_node camnoc_master_fd = {
	.name = "master-camnoc-fd", .buswidth = 16
};

/* SBI */
static const struct qcom_camnoc_node camnoc_master_sbi = {
	.name = "master-camnoc-sbi", .buswidth = 16
};

/* ICP */
static const struct qcom_camnoc_node camnoc_master_icp = {
	.name = "master-camnoc-icp", .buswidth = 8
};

/* Slave */
static const struct qcom_camnoc_node camnoc_slave_axi = {
	.name = "slave-camnoc-axi", .buswidth = 32
};

static const struct qcom_camnoc_node * const camnoc_nodes[] = {
	[MASTER_CAMNOC_IFE_0]      = &camnoc_master_ife_0,
	[MASTER_CAMNOC_IFE_0_RDI]  = &camnoc_master_ife_0_rdi,
	[MASTER_CAMNOC_IFE_1]      = &camnoc_master_ife_1,
	[MASTER_CAMNOC_IFE_1_RDI]  = &camnoc_master_ife_1_rdi,
	[MASTER_CAMNOC_IFE_LITE_0] = &camnoc_master_ife_lite_0,
	[MASTER_CAMNOC_IFE_LITE_1] = &camnoc_master_ife_lite_1,
	[MASTER_CAMNOC_IFE_LITE_2] = &camnoc_master_ife_lite_2,
	[MASTER_CAMNOC_IFE_LITE_3] = &camnoc_master_ife_lite_3,
	[MASTER_CAMNOC_IFE_LITE_4] = &camnoc_master_ife_lite_4,
	[MASTER_CAMNOC_BPS]        = &camnoc_master_bps,
	[MASTER_CAMNOC_IPE_0]      = &camnoc_master_ipe_0,
	[MASTER_CAMNOC_IPE_1]      = &camnoc_master_ipe_1,
	[MASTER_CAMNOC_JPEG]       = &camnoc_master_jpeg,
	[MASTER_CAMNOC_JPEG_DMA]   = &camnoc_master_jpeg_dma,
	[MASTER_CAMNOC_CDM]        = &camnoc_master_cdm,
	[MASTER_CAMNOC_FD]         = &camnoc_master_fd,
	[MASTER_CAMNOC_SBI]        = &camnoc_master_sbi,
	[MASTER_CAMNOC_ICP_RD_WR] = &camnoc_master_icp,
	[SLAVE_CAMNOC_AXI]         = &camnoc_slave_axi,
};

#define CAMNOC_NUM_NODES	ARRAY_SIZE(camnoc_nodes)

static int qcom_camnoc_get_bw(struct icc_node *node, u32 *avg, u32 *peak)
{
	*avg = 0;
	*peak = 0;

	return 0;
}

static int qcom_camnoc_set(struct icc_node *src, struct icc_node *dst)
{
	struct qcom_camnoc_icc_provider *cp =
		to_camnoc_provider(src->provider);
	unsigned long rate;

	/*
	 * peak_bw is the aggregated max across all consumers (kBps).
	 * Consumers encode the required clock frequency directly as kBps,
	 * so the conversion is simply: rate_hz = peak_bw * 1000.
	 * A vote of 0 means no requirement; leave the clock at its minimum.
	 */
	rate = icc_units_to_bps(dst->peak_bw);

	return clk_set_rate(cp->clk, rate);
}

static void qcom_camnoc_remove(struct platform_device *pdev)
{
	struct qcom_camnoc_icc_provider *cp = platform_get_drvdata(pdev);

	icc_provider_deregister(&cp->provider);
	icc_nodes_remove(&cp->provider);
}

static int qcom_camnoc_probe(struct platform_device *pdev)
{
	struct qcom_camnoc_icc_provider *cp;
	struct icc_onecell_data *data;
	struct icc_provider *provider;
	struct icc_node *node;
	unsigned int i;
	int ret;

	cp = devm_kzalloc(&pdev->dev, sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->clk = devm_clk_get(&pdev->dev, "camnoc_axi");
	if (IS_ERR(cp->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(cp->clk),
				     "failed to get camnoc_axi clock\n");

	data = devm_kzalloc(&pdev->dev,
			    struct_size(data, nodes, CAMNOC_NUM_NODES),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->num_nodes = CAMNOC_NUM_NODES;

	provider = &cp->provider;
	provider->dev = &pdev->dev;
	provider->get_bw = qcom_camnoc_get_bw;
	provider->set = qcom_camnoc_set;
	provider->aggregate = icc_std_aggregate;
	provider->xlate = of_icc_xlate_onecell;
	provider->data = data;

	icc_provider_init(provider);

	for (i = 0; i < CAMNOC_NUM_NODES; i++) {
		node = icc_node_create_dyn();
		if (IS_ERR(node)) {
			ret = PTR_ERR(node);
			goto err_remove;
		}

		ret = icc_node_set_name(node, provider, camnoc_nodes[i]->name);
		if (ret) {
			icc_node_destroy(node->id);
			goto err_remove;
		}

		node->data = (void *)camnoc_nodes[i];
		icc_node_add(node, provider);
		data->nodes[i] = node;
	}

	/* Link every master to the single CAMNOC AXI slave. */
	for (i = 0; i < CAMNOC_NUM_NODES - 1; i++) {
		if (camnoc_nodes[i] == &camnoc_slave_axi)
			continue;
		ret = icc_link_nodes(data->nodes[i],
				     &data->nodes[SLAVE_CAMNOC_AXI]);
		if (ret)
			goto err_remove;
	}

	ret = icc_provider_register(provider);
	if (ret)
		goto err_remove;

	platform_set_drvdata(pdev, cp);

	return 0;

err_remove:
	icc_nodes_remove(provider);
	return ret;
}

static const struct of_device_id qcom_camnoc_of_match[] = {
	{ .compatible = "qcom,sm8250-cam-virt" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_camnoc_of_match);

static struct platform_driver qcom_camnoc_driver = {
	.probe  = qcom_camnoc_probe,
	.remove = qcom_camnoc_remove,
	.driver = {
		.name           = "qcom-camnoc-icc",
		.of_match_table = qcom_camnoc_of_match,
		.sync_state     = icc_sync_state,
	},
};
module_platform_driver(qcom_camnoc_driver);

MODULE_DESCRIPTION("Qualcomm CAMNOC interconnect driver");
MODULE_LICENSE("GPL");
