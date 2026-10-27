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
 * Consumers express their required bandwidth in bytes per second; the
 * driver divides the aggregated peak by the slave port bus width to
 * obtain the clock rate.
 */

#include <linux/clk.h>
#include <linux/interconnect-provider.h>
#include <linux/interconnect.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <dt-bindings/interconnect/qcom,camnoc.h>

#define to_camnoc_provider(_p) \
	container_of(_p, struct qcom_camnoc_icc_provider, provider)

struct qcom_camnoc_icc_provider {
	struct clk *clk;
	void __iomem *base;
	const struct qcom_camnoc_data *data;
	bool active;
	struct icc_provider provider;
};

struct qcom_camnoc_node {
	const char *name;
	u16 buswidth;
};

struct qcom_camnoc_reg {
	u32 offset;
	u32 value;
};

struct qcom_camnoc_qos_port {
	const char *name;
	const struct qcom_camnoc_reg *regs;
	unsigned int num_regs;
};

struct qcom_camnoc_data {
	u16 slave_buswidth;
	const struct qcom_camnoc_qos_port *qos_ports;
	unsigned int num_qos_ports;
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

/* CPAS aggregate vote, not tied to one hardware port */
static const struct qcom_camnoc_node camnoc_master_cpas = {
	.name = "master-camnoc-cpas", .buswidth = 32
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
	[MASTER_CAMNOC_CPAS]      = &camnoc_master_cpas,
	[SLAVE_CAMNOC_AXI]         = &camnoc_slave_axi,
};

#define CAMNOC_NUM_NODES	ARRAY_SIZE(camnoc_nodes)

static void qcom_camnoc_qos_program(struct qcom_camnoc_icc_provider *cp)
{
	const struct qcom_camnoc_data *data = cp->data;
	unsigned int i, j;

	if (!cp->base || !data->qos_ports)
		return;

	for (i = 0; i < data->num_qos_ports; i++) {
		const struct qcom_camnoc_qos_port *port = &data->qos_ports[i];

		for (j = 0; j < port->num_regs; j++)
			writel(port->regs[j].value, cp->base + port->regs[j].offset);
	}
}

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
	unsigned long rate = icc_units_to_bps(dst->peak_bw) /
		cp->data->slave_buswidth;
	int ret = 0;

	/*
	 * peak_bw is the aggregated max across all consumers, in bytes per
	 * second. The slave port moves slave_buswidth bytes per cycle, so
	 * the clock rate is the bandwidth divided by the bus width.
	 */
	if (dst->peak_bw) {
		if (!cp->active) {
			ret = pm_runtime_resume_and_get(cp->provider.dev);
			if (ret < 0)
				return ret;

			qcom_camnoc_qos_program(cp);
			cp->active = true;
		}

		ret = clk_set_rate(cp->clk, rate);
	} else if (cp->active) {
		cp->active = false;
		pm_runtime_put(cp->provider.dev);
	}

	return ret;
}

static int qcom_camnoc_runtime_resume(struct device *dev)
{
	struct qcom_camnoc_icc_provider *cp = dev_get_drvdata(dev);

	return clk_prepare_enable(cp->clk);
}

static int qcom_camnoc_runtime_suspend(struct device *dev)
{
	struct qcom_camnoc_icc_provider *cp = dev_get_drvdata(dev);

	clk_disable_unprepare(cp->clk);

	return 0;
}

static const struct dev_pm_ops qcom_camnoc_pm_ops = {
	SET_RUNTIME_PM_OPS(qcom_camnoc_runtime_suspend,
			   qcom_camnoc_runtime_resume, NULL)
};

static void qcom_camnoc_remove(struct platform_device *pdev)
{
	struct qcom_camnoc_icc_provider *cp = platform_get_drvdata(pdev);

	icc_provider_deregister(&cp->provider);
	icc_nodes_remove(&cp->provider);

	if (cp->active)
		pm_runtime_put(&pdev->dev);
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

	cp->data = device_get_match_data(&pdev->dev);
	if (!cp->data)
		return -EINVAL;

	if (cp->data->qos_ports) {
		cp->base = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(cp->base))
			return PTR_ERR(cp->base);
	}

	platform_set_drvdata(pdev, cp);

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		return ret;

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
	for (i = 0; i < CAMNOC_NUM_NODES; i++) {
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

	return 0;

err_remove:
	icc_nodes_remove(provider);
	return ret;
}

/*
 * SM8650 CAMNOC QoS values, ported from the camera-kernel
 * cpastop_v880_100.h camnoc specific table.
 */
static const struct qcom_camnoc_reg qos_rt0_sfe_rd[] = {
	{ .offset = 0x9038, .value = 0x00000004 },
};

static const struct qcom_camnoc_reg qos_rt1_ife_ubwc[] = {
	{ .offset = 0x9230, .value = 0x65555544 },
	{ .offset = 0x9234, .value = 0x66666666 },
	{ .offset = 0x9238, .value = 0x00001e40 },
	{ .offset = 0x9240, .value = 0xffffff00 },
	{ .offset = 0x9248, .value = 0x0000000f },
};

static const struct qcom_camnoc_reg qos_rt2_ife_stats[] = {
	{ .offset = 0x9430, .value = 0x65555544 },
	{ .offset = 0x9434, .value = 0x66666666 },
	{ .offset = 0x9438, .value = 0x00001c40 },
	{ .offset = 0x9440, .value = 0xffffff00 },
	{ .offset = 0x9448, .value = 0x0000000f },
};

static const struct qcom_camnoc_reg qos_rt3_ife_pdaf_linear_ifelite[] = {
	{ .offset = 0x9630, .value = 0x65555544 },
	{ .offset = 0x9634, .value = 0x66666666 },
	{ .offset = 0x9638, .value = 0x00001c40 },
	{ .offset = 0x9640, .value = 0xffffff00 },
	{ .offset = 0x9648, .value = 0x0000000f },
};

static const struct qcom_camnoc_reg qos_rt4_ife_rdi_sfe[] = {
	{ .offset = 0x9830, .value = 0x65555544 },
	{ .offset = 0x9834, .value = 0x66666666 },
	{ .offset = 0x9838, .value = 0x00001e40 },
	{ .offset = 0x9840, .value = 0xffffff00 },
	{ .offset = 0x9848, .value = 0x0000000f },
};

static const struct qcom_camnoc_reg qos_nrt0_cdm[] = {
	{ .offset = 0x8238, .value = 0x00000003 },
};

static const struct qcom_camnoc_reg qos_nrt1_jpeg_rd_wr[] = {
	{ .offset = 0x8430, .value = 0x22222222 },
	{ .offset = 0x8434, .value = 0x22222222 },
	{ .offset = 0x8438, .value = 0x00000022 },
	{ .offset = 0x7088, .value = 0x00000002 },
	{ .offset = 0x70a0, .value = 0x10101010 },
	{ .offset = 0x70a4, .value = 0x10101010 },
};

static const struct qcom_camnoc_reg qos_nrt2_bps_cre_wr[] = {
	{ .offset = 0x8630, .value = 0x33333333 },
	{ .offset = 0x8634, .value = 0x33333333 },
	{ .offset = 0x8638, .value = 0x00000030 },
	{ .offset = 0x7108, .value = 0x00000002 },
	{ .offset = 0x7120, .value = 0x14141414 },
	{ .offset = 0x7124, .value = 0x14141414 },
};

static const struct qcom_camnoc_reg qos_nrt3_bps_cre_rd[] = {
	{ .offset = 0x8838, .value = 0x00000003 },
	{ .offset = 0x7188, .value = 0x00000002 },
	{ .offset = 0x71a0, .value = 0x14141414 },
	{ .offset = 0x71a4, .value = 0x14141414 },
};

static const struct qcom_camnoc_reg qos_nrt4_ipe_0_rd[] = {
	{ .offset = 0x8a38, .value = 0x00000003 },
};

static const struct qcom_camnoc_reg qos_nrt5_ipe_1_rd[] = {
	{ .offset = 0x8c38, .value = 0x00000003 },
};

static const struct qcom_camnoc_reg qos_nrt6_ipe_wr_0[] = {
	{ .offset = 0x8e30, .value = 0x33333333 },
	{ .offset = 0x8e34, .value = 0x33333333 },
	{ .offset = 0x8e38, .value = 0x00000030 },
};

static const struct qcom_camnoc_reg qos_nrt7_ipe_wr_1[] = {
	{ .offset = 0xa430, .value = 0x33333333 },
	{ .offset = 0xa434, .value = 0x33333333 },
	{ .offset = 0xa438, .value = 0x00000030 },
};

static const struct qcom_camnoc_qos_port sm8650_camnoc_qos_ports[] = {
	{ .name = "RT0-SFE_RD", .regs = qos_rt0_sfe_rd, .num_regs = ARRAY_SIZE(qos_rt0_sfe_rd) },
	{ .name = "RT1-IFE_UBWC", .regs = qos_rt1_ife_ubwc, .num_regs = ARRAY_SIZE(qos_rt1_ife_ubwc) },
	{ .name = "RT2-IFE_STATS", .regs = qos_rt2_ife_stats, .num_regs = ARRAY_SIZE(qos_rt2_ife_stats) },
	{ .name = "RT3-IFE_PDAF_LINEAR_IFELITE", .regs = qos_rt3_ife_pdaf_linear_ifelite, .num_regs = ARRAY_SIZE(qos_rt3_ife_pdaf_linear_ifelite) },
	{ .name = "RT4-IFE_RDI_SFE", .regs = qos_rt4_ife_rdi_sfe, .num_regs = ARRAY_SIZE(qos_rt4_ife_rdi_sfe) },
	{ .name = "NRT0-CDM", .regs = qos_nrt0_cdm, .num_regs = ARRAY_SIZE(qos_nrt0_cdm) },
	{ .name = "NRT1-JPEG_RD_WR", .regs = qos_nrt1_jpeg_rd_wr, .num_regs = ARRAY_SIZE(qos_nrt1_jpeg_rd_wr) },
	{ .name = "NRT2-BPS_CRE_WR", .regs = qos_nrt2_bps_cre_wr, .num_regs = ARRAY_SIZE(qos_nrt2_bps_cre_wr) },
	{ .name = "NRT3-BPS_CRE_RD", .regs = qos_nrt3_bps_cre_rd, .num_regs = ARRAY_SIZE(qos_nrt3_bps_cre_rd) },
	{ .name = "NRT4-IPE_0_RD", .regs = qos_nrt4_ipe_0_rd, .num_regs = ARRAY_SIZE(qos_nrt4_ipe_0_rd) },
	{ .name = "NRT5-IPE_1_RD", .regs = qos_nrt5_ipe_1_rd, .num_regs = ARRAY_SIZE(qos_nrt5_ipe_1_rd) },
	{ .name = "NRT6-IPE_WR_0", .regs = qos_nrt6_ipe_wr_0, .num_regs = ARRAY_SIZE(qos_nrt6_ipe_wr_0) },
	{ .name = "NRT7-IPE_WR_1", .regs = qos_nrt7_ipe_wr_1, .num_regs = ARRAY_SIZE(qos_nrt7_ipe_wr_1) },
};

static const struct qcom_camnoc_data sm8250_camnoc_data = {
	.slave_buswidth = 32,
};

static const struct qcom_camnoc_data sm8650_camnoc_data = {
	.slave_buswidth = 32,
	.qos_ports = sm8650_camnoc_qos_ports,
	.num_qos_ports = ARRAY_SIZE(sm8650_camnoc_qos_ports),
};

static const struct of_device_id qcom_camnoc_of_match[] = {
	{ .compatible = "qcom,sm8250-cam-virt", .data = &sm8250_camnoc_data },
	{ .compatible = "qcom,sm8650-camnoc", .data = &sm8650_camnoc_data },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_camnoc_of_match);

static struct platform_driver qcom_camnoc_driver = {
	.probe  = qcom_camnoc_probe,
	.remove = qcom_camnoc_remove,
	.driver = {
		.name           = "qcom-camnoc-icc",
		.of_match_table = qcom_camnoc_of_match,
		.pm             = &qcom_camnoc_pm_ops,
		.sync_state     = icc_sync_state,
	},
};
module_platform_driver(qcom_camnoc_driver);

MODULE_DESCRIPTION("Qualcomm CAMNOC interconnect driver");
MODULE_LICENSE("GPL");
