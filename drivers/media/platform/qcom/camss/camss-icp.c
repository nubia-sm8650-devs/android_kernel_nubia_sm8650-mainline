// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm ICP (Image Control Processor) driver for X1E80100
 *
 */
#define DEBUG
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/interconnect.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/soc/qcom/mdt_loader.h>

#include "camss-hw.h"
#include "camss-icp.h"
#include "camss-icp-hfi.h"

/* CSR register offsets */
#define ICP_CSR_HW_VERSION		0x00
#define ICP_CSR_TCM_SIZE		0x08
#define ICP_CSR_GP			0x20
#define ICP_CSR_DBG_STATUS		0x44
#define ICP_CSR_DBG_CTRL		0x48

#define ICP_WD_CTRL			0x08
#define ICP_WD_INTCLR			0x0c

#define ICP_TZ_STATE_SUSPEND		0
#define ICP_TZ_STATE_RESUME		1

#define ICP_EV_WDOG			0

#define ICP_CLK_MAX			32

#define POLLING_SLEEP_US		1000
#define POLLING_TIMEOUT_US		20000

/* HFI polling - match CAMX: 100us interval, 2 second timeout */
#define HFI_POLL_DELAY_US		100
#define HFI_POLL_TIMEOUT_US		2000000

struct camss_icp_irq_regs {
	u32 mask;
	u32 status;
	u32 clear;
	u32 cmd;
	u32 host2icp;
	u32 host_irq;
	u32 wdog_bite;
	u32 clear_cmd;
};

/* ICC paths */
#define HFI_MAX_ICC_PATHS		4
struct camss_icp_resources {
	int pas_id;
	const char * const *clk_names;
	int clk_num;
	const char * const *noc_names;
	int noc_num;
	const unsigned long *rates;
	unsigned int rate_num;
	const struct camss_icp_irq_regs *irq_regs;
	bool irq_regs_in_csr;
	bool hfi_boot;
	struct hfi_resources hfi_res;
};

struct camss_icp {
	struct device *dev;
	void __iomem *csr_base;
	void __iomem *irq_base;
	void __iomem *wd_base;
	int irq;

	struct dev_pm_domain_list *pd_list;

	struct clk_bulk_data clks[ICP_CLK_MAX];
	struct clk *core_clk;
	struct icc_path *icc_path[HFI_MAX_ICC_PATHS];

	struct icp_hfi hfi;

	phys_addr_t fw_phys;
	size_t fw_size;

	const struct camss_icp_resources *icp_res;

	/* serializes the claim, power and firmware state below */
	struct mutex lock;
	const struct camss_hw_icp_ops *ops;
	void *priv;
	bool claimed;
	bool powered;
	bool booted;
	bool hfi_running;
	unsigned long events;
};

struct icp_hfi *icp_hfi_get(struct device *dev)
{
	struct camss_icp *icp = dev_get_drvdata(dev);

	if (!icp)
		return ERR_PTR(-ENODEV);

	get_device(icp->dev);
	return &icp->hfi;
}
EXPORT_SYMBOL(icp_hfi_get);

void icp_hfi_put(struct icp_hfi *phfi)
{
	struct camss_icp *icp = container_of(phfi, struct camss_icp, hfi);

	put_device(icp->dev);
}
EXPORT_SYMBOL(icp_hfi_put);

/*
 * Raise interrupt to firmware - called by HFI layer after CMD_Q write
 */
static void icp_raise_irq(void *priv)
{
	struct camss_icp *icp = priv;
	const struct camss_icp_irq_regs *regs = icp->icp_res->irq_regs;

	dev_dbg(icp->dev, "Raising HOST2ICPINT (CIRQ STATUS before: 0x%x)\n",
		readl(icp->irq_base + regs->status));

	writel(1, icp->irq_base + regs->host2icp);

	/* Small delay then check if anything changed */
	udelay(100);
	dev_dbg(icp->dev, "CIRQ STATUS after HOST2ICPINT: 0x%x\n",
		readl(icp->irq_base + regs->status));
}

static int icp_power_on(struct camss_icp *icp)
{
	const struct camss_icp_resources *icp_res = icp->icp_res;
	int i, ret;

	ret = pm_runtime_resume_and_get(icp->dev);
	if (ret) {
		dev_err(icp->dev, "Failed to power on: %d\n", ret);
		return ret;
	}

	if (icp_res->rate_num) {
		ret = clk_set_rate(icp->core_clk, icp_res->rates[0]);
		if (ret) {
			dev_err(icp->dev, "Failed to set the ICP rate: %d\n", ret);
			goto err_pm;
		}
	}

	ret = clk_bulk_prepare_enable(icp_res->clk_num, icp->clks);
	if (ret) {
		dev_err(icp->dev, "Failed to enable clocks: %d\n", ret);
		goto err_pm;
	}

	for (i = 0; i < icp_res->noc_num; i++) {
		if (!icp->icc_path[i])
			continue;

		dev_dbg(icp->dev, "Voting for BW now %s\n", icp_res->noc_names[i]);
		ret = icc_set_bw(icp->icc_path[i], 100000000, 1000000000);
		if (ret) {
			dev_err(icp->dev, "Voting for %s failed\n", icp_res->noc_names[i]);
			goto err_icc;
		}
	}

	icp->powered = true;
	enable_irq(icp->irq);

	return 0;

err_icc:
	while (i--)
		if (icp->icc_path[i])
			icc_set_bw(icp->icc_path[i], 0, 0);
	clk_bulk_disable_unprepare(icp_res->clk_num, icp->clks);
err_pm:
	pm_runtime_put_sync(icp->dev);
	return ret;
}

static void icp_power_off(struct camss_icp *icp)
{
	const struct camss_icp_resources *icp_res = icp->icp_res;
	int i;

	disable_irq(icp->irq);
	icp->powered = false;

	for (i = 0; i < icp_res->noc_num; i++)
		if (icp->icc_path[i])
			icc_set_bw(icp->icc_path[i], 0, 0);

	clk_bulk_disable_unprepare(icp_res->clk_num, icp->clks);

	pm_runtime_put_sync(icp->dev);
}

/* Load firmware */
static int icp_load_firmware(struct camss_icp *icp)
{
	const struct camss_icp_resources *icp_res = icp->icp_res;
	const struct firmware *fw;
	struct device_node *node;
	struct resource res;
	const char *fw_name;
	void *vaddr;
	ssize_t fw_size;
	int ret;

	ret = of_property_read_string(icp->dev->of_node, "firmware-name", &fw_name);
	if (ret)
		return ret;

	node = of_parse_phandle(icp->dev->of_node, "memory-region", 0);
	if (!node)
		return -ENODEV;

	ret = of_address_to_resource(node, 0, &res);
	of_node_put(node);
	if (ret)
		return ret;

	dev_dbg(icp->dev, "FW memory: phys=0x%pa size=%llu\n",
		&res.start, resource_size(&res));

	ret = firmware_request_nowarn(&fw, fw_name, icp->dev);
	if (ret) {
		dev_err(icp->dev, "Failed to request %s: %d\n", fw_name, ret);
		return ret;
	}

	fw_size = qcom_mdt_get_size(fw);
	if (fw_size < 0 || (size_t)fw_size > resource_size(&res)) {
		release_firmware(fw);
		return -EINVAL;
	}

	vaddr = ioremap_wc(res.start, resource_size(&res));
	if (!vaddr) {
		release_firmware(fw);
		return -ENOMEM;
	}

	ret = qcom_mdt_load(icp->dev, fw, fw_name, icp_res->pas_id, vaddr,
			    res.start, resource_size(&res), NULL);
	iounmap(vaddr);
	release_firmware(fw);

	if (ret == 0) {
		dev_dbg(icp->dev, "Firmware loaded: %s (%zd bytes)\n", fw_name, fw_size);
		icp->fw_phys = res.start;
		icp->fw_size = fw_size;
	}

	return ret;
}

static irqreturn_t camss_icp_isr_thread(int irq, void *data)
{
	struct camss_icp *icp = data;

	if (!IS_ENABLED(CONFIG_VIDEO_QCOM_CAMSS_HARD_ISP))
		return IRQ_HANDLED;

	if (test_and_clear_bit(ICP_EV_WDOG, &icp->events)) {
		dev_err(icp->dev, "ICP watchdog bite!\n");
		icp_hfi_dump_sfr(&icp->hfi);
	}

	/* Process message queue - signals waiters if data present */
	icp_hfi_process_msg_queue(&icp->hfi);

	/* Flush debug queue - async debug messages */
	icp_hfi_flush_debug_queue(&icp->hfi);

	return IRQ_HANDLED;
}

static irqreturn_t camss_icp_isr(int irq, void *data)
{
	struct camss_icp *icp = data;
	const struct camss_icp_irq_regs *regs = icp->icp_res->irq_regs;
	const struct camss_hw_icp_ops *ops;
	bool wdog;
	u32 status;

	status = readl(icp->irq_base + regs->status);
	writel(status, icp->irq_base + regs->clear);
	if (regs->clear_cmd)
		writel(regs->clear_cmd, icp->irq_base + regs->cmd);

	wdog = status & regs->wdog_bite;
	if (wdog) {
		writel(0, icp->wd_base + ICP_WD_CTRL);
		writel(1, icp->wd_base + ICP_WD_INTCLR);
	}

	ops = smp_load_acquire(&icp->ops);
	if (ops) {
		ops->irq(icp->priv, wdog);
		return IRQ_HANDLED;
	}

	if (!status)
		return IRQ_NONE;

	if (wdog)
		set_bit(ICP_EV_WDOG, &icp->events);

	return IRQ_WAKE_THREAD;
}

/* Dump GP registers for debugging */
static void icp_dump_gp_regs(struct camss_icp *icp, const char *label)
{
	u32 regs[20];
	int i;

	dev_info(icp->dev, "=== %s ===\n", label);
	dev_info(icp->dev, "Raw CSR offsets 0x20-0x6C (GP registers):\n");

	for (i = 0; i < 20; i++)
		regs[i] = readl(icp->csr_base + 0x20 + i * 4);

	for (i = 0; i < 20; i += 4)
		dev_info(icp->dev, "  [0x%02x]: %08x %08x %08x %08x\n",
			 0x20 + i * 4, regs[i], regs[i+1], regs[i+2], regs[i+3]);
}

static void icp_hfi_latch_regs(struct camss_icp *icp)
{
	struct icp_hfi *hfi = &icp->hfi;

	/* Shared memory */
	writel(hfi->hfi_mem.shmem.dma_addr, icp->csr_base + HFI_REG_SHARED_MEM_PTR);
	writel(hfi->hfi_mem.shmem.size, icp->csr_base + HFI_REG_SHARED_MEM_SIZE);

	/* QTBL - header describing HFI queues */
	writel(hfi->hfi_mem.q_tbl.dma_addr, icp->csr_base + HFI_REG_QTBL_PTR);

	/* QDSS */
	writel(hfi->hfi_mem.qdss.dma_addr, icp->csr_base + HFI_REG_QDSS_IOVA);
	writel(HFI_QDSS_SIZE, icp->csr_base + HFI_REG_QDSS_IOVA_SIZE);

	/* FW Uncached */
	writel(hfi->hfi_mem.fwuncached.dma_addr, icp->csr_base + HFI_REG_FWUNCACHED_IOVA);
	writel(HFI_FWUNCACHED_SIZE, icp->csr_base + HFI_REG_FWUNCACHED_SIZE);

	/* Sec heap */
	writel(hfi->hfi_mem.secheap.dma_addr, icp->csr_base + HFI_REG_SECONDARY_HEAP_PTR);
	writel(HFI_SECHEAP_SIZE, icp->csr_base + HFI_REG_SECONDARY_HEAP_SIZE);

	/* SFR buffer */
	writel(hfi->hfi_mem.sfr.dma_addr, icp->csr_base + HFI_REG_SFR_PTR);

	/* IO regions - use standard CAMX values TODO: fix this */
	writel(0x10c00000, icp->csr_base + HFI_REG_IO_REGION_1_IOVA);
	writel(0xcf400000, icp->csr_base + HFI_REG_IO_REGION_1_SIZE);
	writel(0xe0800000, icp->csr_base + HFI_REG_IO_REGION_2_IOVA);
	writel(0x1e700000, icp->csr_base + HFI_REG_IO_REGION_2_SIZE);

}

/* HFI operations */
static const struct icp_hfi_ops hfi_ops = {
	.raise_irq = icp_raise_irq,
};

static int icp_boot(struct camss_icp *icp)
{
	const struct camss_icp_resources *icp_res = icp->icp_res;
	const struct camss_icp_irq_regs *regs = icp_res->irq_regs;
	u32 hw_version, data;
	int ret;

	ret = icp_power_on(icp);
	if (ret)
		return ret;

	/* Verify HW version */
	hw_version = readl(icp->csr_base + ICP_CSR_HW_VERSION);
	dev_info(icp->dev, "HW version: 0x%08x\n", hw_version);

	if (hw_version == 0 || hw_version == 0xffffffff) {
		dev_err(icp->dev, "Invalid HW version\n");
		ret = -EIO;
		goto err_power;
	}

	/* Configure interrupts */
	writel(0x7f, icp->irq_base + regs->clear);
	writel(regs->host_irq | regs->wdog_bite, icp->irq_base + regs->mask);

	/* Load firmware */
	ret = icp_load_firmware(icp);
	if (ret)
		goto err_power;

	/* Allocate HFI queues */
	ret = icp_hfi_init_queues(&icp->hfi);
	if (ret)
		goto err_power;

	/* Start firmware via TrustZone */
	ret = qcom_scm_pas_auth_and_reset(icp_res->pas_id);
	if (ret) {
		dev_err(icp->dev, "TZ auth_and_reset failed: %d\n", ret);
		goto err_hfi;
	}

	/* Wait for firmware to boot */
	usleep_range(5000, 51000);

	/* Program HFI pointers after bootup */
	icp_hfi_latch_regs(icp);

	/*
	 * Boot handshake with ICP firmware.
	 *
	 * After TZ starts the ICP, firmware waits for host to program
	 * GP registers with memory addresses and signal readiness.
	 *
	 * Register assignments:
	 *   GP1 (0x24) = FW_VERSION        - firmware writes its version
	 *   GP2 (0x28) = HOST_ICP_INIT_REQ - host writes 1 ("addresses ready")
	 *   GP3 (0x2C) = ICP_HOST_INIT_RESP - firmware writes 1 ("ready")
	 *   GP4-GP18   = memory addresses   - host programs with IOVAs
	 *
	 * Sequence:
	 *   1. Program GP4-GP18 with memory addresses  [done above]
	 *   2. Write GP2 = 1 (HOST_ICP_INIT_REQUEST)
	 *   3. Poll GP3 until ICP_INIT_RESP_SUCCESS (1)
	 *   4. Read GP1 for firmware version
	 *
	 */

	/* Signal firmware */
	writel(ICP_INIT_REQUEST_SET, icp->csr_base + HFI_REG_HOST_ICP_INIT_REQ);

	wmb();

	/* Wait for firmware to signal ready via GP3 */
	ret = readl_poll_timeout(icp->csr_base + HFI_REG_ICP_HOST_INIT_RESP,
				 data, data == ICP_INIT_RESP_SUCCESS,
				 HFI_POLL_DELAY_US, HFI_POLL_TIMEOUT_US);

	if (ret < 0) {
		dev_err(icp->dev, "Firmware ready timeout (GP3=0x%08x)\n",
			readl(icp->csr_base + HFI_REG_ICP_HOST_INIT_RESP));
		dev_err(icp->dev, "  GP1 (FW_VERSION)=0x%08x GP2 (INIT_REQ)=0x%08x\n",
			readl(icp->csr_base + HFI_REG_FW_VERSION),
			readl(icp->csr_base + HFI_REG_HOST_ICP_INIT_REQ));
		icp_dump_gp_regs(icp, "FW READY TIMEOUT");
		icp_hfi_dump_sfr(&icp->hfi);
		ret = -ETIMEDOUT;
		goto err_pas;
	}

	/* Read firmware version from GP1 */
	icp->hfi.fw_version = readl(icp->csr_base + HFI_REG_FW_VERSION);
	dev_dbg(icp->dev, "Firmware ready! version=0x%08x\n", icp->hfi.fw_version);

	/* Dump CIRQ state after FW init */
	dev_info(icp->dev, "CIRQ after FW init: MASK=0x%x STATUS=0x%x\n",
		 readl(icp->irq_base + regs->mask),
		 readl(icp->irq_base + regs->status));

	/* Dump GP registers to verify what firmware sees */
	dev_info(icp->dev, "GP registers after FW init:\n");
	dev_info(icp->dev, "  GP0 (unused):        0x%08x\n", readl(icp->csr_base + 0x20));
	dev_info(icp->dev, "  GP1 (FW_VERSION):    0x%08x\n", readl(icp->csr_base + HFI_REG_FW_VERSION));
	dev_info(icp->dev, "  GP2 (INIT_REQ):      0x%08x\n", readl(icp->csr_base + HFI_REG_HOST_ICP_INIT_REQ));
	dev_info(icp->dev, "  GP3 (INIT_RESP):     0x%08x\n", readl(icp->csr_base + HFI_REG_ICP_HOST_INIT_RESP));
	dev_info(icp->dev, "  GP4 (SHMEM_PTR):     0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_SHARED_MEM_PTR), (u32)icp->hfi.hfi_mem.shmem.dma_addr);
	dev_info(icp->dev, "  GP5 (SHMEM_SIZE):    0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_SHARED_MEM_SIZE), (u32)icp->hfi.hfi_mem.shmem.size);
	dev_info(icp->dev, "  GP6 (QTBL_PTR):      0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_QTBL_PTR), (u32)icp->hfi.hfi_mem.q_tbl.dma_addr);
	dev_info(icp->dev, "  GP7 (SECHEAP_PTR):   0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_SECONDARY_HEAP_PTR), (u32)icp->hfi.hfi_mem.secheap.dma_addr);
	dev_info(icp->dev, "  GP8 (SECHEAP_SIZE):  0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_SECONDARY_HEAP_SIZE), HFI_SECHEAP_SIZE);
	dev_info(icp->dev, "  GP9 (STATUS):        0x%08x\n", readl(icp->csr_base + HFI_REG_RESERVED));
	dev_info(icp->dev, "  GP10 (SFR_PTR):      0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_SFR_PTR), (u32)icp->hfi.hfi_mem.sfr.dma_addr);
	dev_info(icp->dev, "  GP11 (QDSS_IOVA):    0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_QDSS_IOVA), (u32)icp->hfi.hfi_mem.qdss.dma_addr);
	dev_info(icp->dev, "  GP12 (QDSS_SIZE):    0x%08x\n", readl(icp->csr_base + HFI_REG_QDSS_IOVA_SIZE));
	dev_info(icp->dev, "  GP17 (FWUNCACHED):   0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_FWUNCACHED_IOVA), (u32)icp->hfi.hfi_mem.fwuncached.dma_addr);
	dev_info(icp->dev, "  GP18 (FWUNC_SIZE):   0x%08x (we wrote: 0x%08x)\n",
		 readl(icp->csr_base + HFI_REG_FWUNCACHED_SIZE), HFI_FWUNCACHED_SIZE);

	icp->booted = true;
	icp->hfi_running = true;

	return 0;
err_pas:
	qcom_scm_pas_shutdown(icp_res->pas_id);
err_hfi:
	icp_hfi_deinit_queues(&icp->hfi);
err_power:
	icp_power_off(icp);
	return ret;
}

/**
 * camss_hw_icp_get - look up the ICP of a camera subsystem
 * @consumer: device that will claim the ICP
 * @np: ICP node
 *
 * The consumer is unbound before the ICP goes away.
 *
 * Return: the ICP, or -EPROBE_DEFER until the ICP has probed
 */
struct camss_icp *camss_hw_icp_get(struct device *consumer,
				   struct device_node *np)
{
	struct platform_device *pdev;
	struct device_link *link;
	struct camss_icp *icp;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	icp = platform_get_drvdata(pdev);
	if (!icp) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	link = device_link_add(consumer, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&pdev->dev);
	if (!link)
		return ERR_PTR(-EINVAL);

	return icp;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_get, "QCOM_CAMSS");

/**
 * camss_hw_icp_claim - take the ICP for a client that runs its own HFI
 * @icp: ICP from camss_hw_icp_get()
 * @ops: callbacks
 * @priv: argument of the callbacks
 *
 * The client brings up the HFI queues through the GP registers from
 * camss_hw_icp_gp_regs() and gets every ICP interrupt through @ops.
 *
 * Return: 0, or -EBUSY if another client holds the ICP
 */
int camss_hw_icp_claim(struct camss_icp *icp,
		       const struct camss_hw_icp_ops *ops, void *priv)
{
	int ret = 0;

	mutex_lock(&icp->lock);
	if (icp->claimed || icp->hfi_running) {
		ret = -EBUSY;
	} else {
		icp->claimed = true;
		icp->priv = priv;
		smp_store_release(&icp->ops, ops);
	}
	mutex_unlock(&icp->lock);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_claim, "QCOM_CAMSS");

/**
 * camss_hw_icp_release - give up an ICP claim
 * @icp: claimed ICP
 *
 * Shuts the firmware down and powers the ICP off if the client left them
 * up. No callback runs once this returns.
 */
void camss_hw_icp_release(struct camss_icp *icp)
{
	mutex_lock(&icp->lock);

	if (icp->booted) {
		qcom_scm_pas_shutdown(icp->icp_res->pas_id);
		icp->booted = false;
	}

	if (icp->powered)
		icp_power_off(icp);

	WRITE_ONCE(icp->ops, NULL);
	synchronize_irq(icp->irq);
	icp->priv = NULL;
	icp->claimed = false;

	mutex_unlock(&icp->lock);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_release, "QCOM_CAMSS");

/**
 * camss_hw_icp_power - power a claimed ICP on or off
 * @icp: claimed ICP
 * @on: power state
 *
 * Power on enables the ICP clocks at the lowest rate and the ICP
 * interrupt. Powering to the current state does nothing.
 */
int camss_hw_icp_power(struct camss_icp *icp, bool on)
{
	int ret = 0;

	mutex_lock(&icp->lock);

	if (!icp->claimed)
		ret = -EPERM;
	else if (on && !icp->powered)
		ret = icp_power_on(icp);
	else if (!on && icp->powered)
		icp_power_off(icp);

	mutex_unlock(&icp->lock);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_power, "QCOM_CAMSS");

/**
 * camss_hw_icp_set_clk_rate - set the ICP core clock
 * @icp: claimed ICP
 * @rate: minimum rate in Hz
 * @applied: the lowest supported rate not below @rate
 *
 * A powered off ICP comes back at the lowest rate, so the request only
 * applies while the ICP is powered.
 *
 * Return: 0 on success, -ERANGE if @rate is above every supported rate
 */
int camss_hw_icp_set_clk_rate(struct camss_icp *icp, unsigned long rate,
			      unsigned long *applied)
{
	const struct camss_icp_resources *icp_res = icp->icp_res;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < icp_res->rate_num; i++)
		if (icp_res->rates[i] >= rate)
			break;

	if (i == icp_res->rate_num)
		return -ERANGE;

	mutex_lock(&icp->lock);

	if (!icp->claimed)
		ret = -EPERM;
	else if (icp->powered)
		ret = clk_set_rate(icp->core_clk, icp_res->rates[i]);

	mutex_unlock(&icp->lock);

	if (!ret)
		*applied = icp_res->rates[i];

	return ret;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_set_clk_rate, "QCOM_CAMSS");

/**
 * camss_hw_icp_boot - load the ICP firmware and take the ICP out of reset
 * @icp: claimed and powered ICP
 *
 * Loads firmware-name into memory-region and has TrustZone authenticate
 * and start it. The HFI handshake is up to the client.
 */
int camss_hw_icp_boot(struct camss_icp *icp)
{
	int ret;

	mutex_lock(&icp->lock);

	if (!icp->claimed || !icp->powered) {
		ret = -EPERM;
		goto out;
	}

	if (icp->booted) {
		ret = -EBUSY;
		goto out;
	}

	ret = icp_load_firmware(icp);
	if (ret) {
		dev_err(icp->dev, "Failed to load the firmware: %d\n", ret);
		goto out;
	}

	ret = qcom_scm_pas_auth_and_reset(icp->icp_res->pas_id);
	if (ret) {
		dev_err(icp->dev, "TZ auth_and_reset failed: %d\n", ret);
		goto out;
	}

	icp->booted = true;

out:
	mutex_unlock(&icp->lock);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_boot, "QCOM_CAMSS");

/**
 * camss_hw_icp_shutdown - stop the ICP firmware through TrustZone
 * @icp: claimed ICP
 */
int camss_hw_icp_shutdown(struct camss_icp *icp)
{
	int ret = 0;

	mutex_lock(&icp->lock);

	if (!icp->claimed) {
		ret = -EPERM;
	} else if (icp->booted) {
		ret = qcom_scm_pas_shutdown(icp->icp_res->pas_id);
		icp->booted = false;
	}

	mutex_unlock(&icp->lock);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_shutdown, "QCOM_CAMSS");

/**
 * camss_hw_icp_set_state - suspend or resume the ICP core through TrustZone
 * @icp: claimed ICP with its firmware running
 * @resume: resume when set, suspend otherwise
 *
 * Suspend follows the firmware's acknowledgement of HFI PC_PREP, resume
 * comes before the client rewrites the HFI registers.
 */
int camss_hw_icp_set_state(struct camss_icp *icp, bool resume)
{
	int ret;

	mutex_lock(&icp->lock);

	if (!icp->claimed || !icp->booted)
		ret = -EPERM;
	else
		ret = qcom_scm_set_remote_state(resume ? ICP_TZ_STATE_RESUME :
						ICP_TZ_STATE_SUSPEND,
						icp->icp_res->pas_id);

	mutex_unlock(&icp->lock);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_set_state, "QCOM_CAMSS");

/**
 * camss_hw_icp_irq_enable - unmask the host and watchdog bite interrupts
 * @icp: claimed and powered ICP
 */
void camss_hw_icp_irq_enable(struct camss_icp *icp)
{
	const struct camss_icp_irq_regs *regs = icp->icp_res->irq_regs;

	if (WARN_ON_ONCE(!icp->powered))
		return;

	writel(regs->host_irq | regs->wdog_bite, icp->irq_base + regs->mask);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_irq_enable, "QCOM_CAMSS");

/**
 * camss_hw_icp_raise_irq - interrupt the ICP firmware
 * @icp: claimed and powered ICP
 */
void camss_hw_icp_raise_irq(struct camss_icp *icp)
{
	if (WARN_ON_ONCE(!icp->powered))
		return;

	writel(1, icp->irq_base + icp->icp_res->irq_regs->host2icp);
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_raise_irq, "QCOM_CAMSS");

/**
 * camss_hw_icp_gp_regs - the general purpose registers of the ICP CSR
 * @icp: claimed ICP
 *
 * The HFI handshake runs through these. They are only accessible while
 * the ICP is powered.
 */
void __iomem *camss_hw_icp_gp_regs(struct camss_icp *icp)
{
	return icp->csr_base + ICP_CSR_GP;
}
EXPORT_SYMBOL_NS_GPL(camss_hw_icp_gp_regs, "QCOM_CAMSS");

static int camss_icp_probe(struct platform_device *pdev)
{
	struct dev_pm_domain_attach_data pd_data = { .pd_flags = PD_FLAG_DEV_LINK_ON };
	const struct camss_icp_resources *icp_res;
	struct camss_icp *icp;
	int ret, i;

	icp_res = of_device_get_match_data(&pdev->dev);
	if (!icp_res)
		return -EINVAL;

	icp = devm_kzalloc(&pdev->dev, sizeof(*icp), GFP_KERNEL);
	if (!icp)
		return -ENOMEM;

	icp->dev = &pdev->dev;
	icp->icp_res = icp_res;
	icp->hfi.res = &icp_res->hfi_res;
	icp->hfi.ops = &hfi_ops;
	icp->hfi.dev = icp->dev;
	icp->hfi.priv = icp;

	ret = devm_mutex_init(&pdev->dev, &icp->lock);
	if (ret)
		return ret;

	icp->csr_base = devm_platform_ioremap_resource_byname(pdev, "csr");
	if (IS_ERR(icp->csr_base))
		return PTR_ERR(icp->csr_base);

	if (icp_res->irq_regs_in_csr) {
		icp->irq_base = icp->csr_base;
	} else {
		icp->irq_base = devm_platform_ioremap_resource_byname(pdev, "cirq");
		if (IS_ERR(icp->irq_base))
			return PTR_ERR(icp->irq_base);
	}

	icp->wd_base = devm_platform_ioremap_resource_byname(pdev, "wd");
	if (IS_ERR(icp->wd_base))
		return PTR_ERR(icp->wd_base);

	icp->irq = platform_get_irq(pdev, 0);
	if (icp->irq < 0)
		return icp->irq;

	/* Reserved memory for HFI */
	ret = of_reserved_mem_device_init_by_idx(&pdev->dev, pdev->dev.of_node, 1);
	if (ret && ret != -ENODEV) {
		dev_err(&pdev->dev, "Failed to init reserved memory: %d\n", ret);
		return ret;
	}

	ret = dev_pm_domain_attach_list(&pdev->dev, &pd_data, &icp->pd_list);
	if (ret < 0 && ret != -EEXIST) {
		dev_err(&pdev->dev, "Failed to attach power domains: %d\n", ret);
		return ret;
	}

	for (i = 0; i < icp_res->clk_num; i++) {
		dev_info(&pdev->dev, "clk=%s\n", icp_res->clk_names[i]);
		icp->clks[i].id = icp_res->clk_names[i];
	}

	ret = devm_clk_bulk_get(&pdev->dev, icp_res->clk_num, icp->clks);
	if (ret) {
		dev_err(&pdev->dev, "Failed to get clocks: %d\n", ret);
		goto err_pd;
	}

	for (i = 0; i < icp_res->clk_num; i++)
		if (!strcmp(icp->clks[i].id, "core"))
			icp->core_clk = icp->clks[i].clk;

	if (icp_res->rate_num && !icp->core_clk) {
		ret = -EINVAL;
		goto err_pd;
	}

	for (i = 0; i < icp_res->noc_num; i++) {
		icp->icc_path[i] = devm_of_icc_get(&pdev->dev, icp_res->noc_names[i]);
		if (IS_ERR(icp->icc_path[i])) {
			if (PTR_ERR(icp->icc_path[i]) != -ENODATA) {
				ret = PTR_ERR(icp->icc_path[i]);
				goto err_pd;
			}
			icp->icc_path[i] = NULL;
		}
	};

	ret = devm_request_threaded_irq(&pdev->dev, icp->irq, camss_icp_isr,
					camss_icp_isr_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"camss-icp", icp);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ: %d\n", ret);
		goto err_pd;
	}

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		goto err_pd;

	if (IS_ENABLED(CONFIG_VIDEO_QCOM_CAMSS_HARD_ISP) && icp_res->hfi_boot) {
		ret = icp_boot(icp);
		if (ret)
			goto err_pd;

		ret = icp_hfi_core_init(&icp->hfi);
		if (ret)
			goto err_pd;
	}

	platform_set_drvdata(pdev, icp);

	return 0;

err_pd:
	if (icp->pd_list)
		dev_pm_domain_detach_list(icp->pd_list);

	return ret;
}

static void camss_icp_remove(struct platform_device *pdev)
{
	struct camss_icp *icp = platform_get_drvdata(pdev);

	if (IS_ENABLED(CONFIG_VIDEO_QCOM_CAMSS_HARD_ISP) && icp->hfi_running) {
		qcom_scm_pas_shutdown(icp->icp_res->pas_id);
		icp_hfi_deinit_queues(&icp->hfi);
		icp_power_off(icp);
	}

	if (icp->pd_list)
		dev_pm_domain_detach_list(icp->pd_list);

}

static const char * const x1e80100_clk_names [] = {
	"ahb", "core", "debug_xo",
	"gcc_hf_axi", "gcc_sf_axi",
	"cpas_ahb", "core_ahb", "cpas_fast_ahb",
	"camnoc_axi_rt", "camnoc_axi_nrt",
	"bps_ahb", "bps_fast_ahb", "bps", "cpas_bps",
	"ipe_ahb", "ipe_nps_fast_ahb", "ipe_pps_fast_ahb",
	"ipe_nps", "ipe_pps", "cpas_ipe",
};

static const char * const x1e80100_noc_names [] = {
	"ahb",
	"hf_0",
	"sf_0",
	"sf_icp"
};

static const struct camss_icp_irq_regs x1e80100_icp_irq_regs = {
	.mask = 0x00,
	.status = 0x04,
	.clear = 0x08,
	.host2icp = 0x124,
	.host_irq = BIT(0),
	.wdog_bite = BIT(6),
};

static const struct camss_icp_resources x1e80100_icp_res = {
	.pas_id = 33,
	.clk_names = x1e80100_clk_names,
	.clk_num = ARRAY_SIZE(x1e80100_clk_names),
	.noc_names = x1e80100_noc_names,
	.noc_num = ARRAY_SIZE(x1e80100_noc_names),
	.irq_regs = &x1e80100_icp_irq_regs,
	.hfi_boot = true,
	.hfi_res = {
		.shmem_size = SZ_1M,	 // change to 0x0FC00000 per downstream ?
		.qdss_size = SZ_1M,
		.fwuncached_size = 7 * SZ_1M,
		/*
		 * Carve sub-regions from FwUncached:
		 * Over-allocate as CamX does for now.
		 *   +0x000000: SecHeap (1MB)
		 *   +0x100000: QTBL (1MB)
		 *   +0x200000: CMD_Q (1MB)
		 *   +0x300000: MSG_Q (1MB)
		 *   +0x400000: DBG_Q (1MB)
		 *   +0x500000: SFR (1MB alloc 4KB used)
		 */
		.secheap_size = SZ_1M,
		.q_tbl_size = SZ_1M,
		.qdata_size[HFI_Q_CMD_TYPE] = SZ_1M,
		.qdata_size[HFI_Q_MSG_TYPE] = SZ_1M,
		.qdata_size[HFI_Q_DBG_TYPE] = SZ_1M,
		.sfr_size = SZ_1M,
	},
};

static const char * const sm8650_icp_clk_names[] = {
	"ahb", "core", "debug_xo",
};

static const unsigned long sm8650_icp_rates[] = {
	400000000, 480000000, 600000000,
};

static const struct camss_icp_irq_regs icp_v2_1_irq_regs = {
	.mask = 0x200,
	.clear = 0x204,
	.status = 0x20c,
	.cmd = 0x210,
	.host2icp = 0x300,
	.host_irq = BIT(2),
	.wdog_bite = BIT(4),
	.clear_cmd = BIT(1),
};

static const struct camss_icp_resources sm8650_icp_res = {
	.pas_id = 33,
	.clk_names = sm8650_icp_clk_names,
	.clk_num = ARRAY_SIZE(sm8650_icp_clk_names),
	.rates = sm8650_icp_rates,
	.rate_num = ARRAY_SIZE(sm8650_icp_rates),
	.irq_regs = &icp_v2_1_irq_regs,
	.irq_regs_in_csr = true,
};

static const struct of_device_id camss_icp_of_match[] = {
	{ .compatible = "qcom,sm8650-camss-icp", .data = &sm8650_icp_res },
	{ .compatible = "qcom,x1e80100-camss-icp", .data = &x1e80100_icp_res},
	{ }
};
MODULE_DEVICE_TABLE(of, camss_icp_of_match);

static struct platform_driver camss_icp_driver = {
	.probe = camss_icp_probe,
	.remove = camss_icp_remove,
	.driver = {
		.name = "camss-icp",
		.of_match_table = camss_icp_of_match,
	},
};

module_platform_driver(camss_icp_driver);

MODULE_DESCRIPTION("Qualcomm ICP driver");
MODULE_LICENSE("GPL");
