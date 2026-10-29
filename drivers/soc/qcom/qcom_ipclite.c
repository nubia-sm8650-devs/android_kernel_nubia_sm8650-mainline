// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm IPCLite shared memory protocol
 */

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/hwspinlock.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/soc/qcom/ipclite.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define IPCLITE_MAJOR_VERSION		1
#define IPCLITE_MINOR_VERSION		0
#define IPCLITE_FEATURE_GLOBAL_ATOMIC	BIT_ULL(0)
#define IPCLITE_FEATURE_TEST_SUITE	BIT_ULL(1)

#define IPCMEM_INIT_COMPLETED		1
#define IPCMEM_TOC_SIZE			SZ_4K
#define IPCMEM_TOC_DATA_OFFSET		0x100

#define IPCMEM_FLAGS_RW_PROTECTION	0x3

#define GLOBAL_PARTITION_TYPE		0xff
#define GLOBAL_PARTITION_SIZE		SZ_128K
#define GLOBAL_REGION_OFFSET		SZ_4K
#define GLOBAL_REGION_SIZE		(SZ_128K - SZ_4K)

#define PARTITION_TYPE_DEFAULT		0
#define PARTITION_TYPE_LOOPBACK		1
#define PARTITION_SIZE			SZ_32K
#define PARTITION_DESC_OFFSET		SZ_1K
#define PARTITION_DESC_SIZE		(3 * SZ_1K)
#define PARTITION_FIFO0_OFFSET		SZ_4K
#define PARTITION_FIFO1_OFFSET		(12 * SZ_1K)
#define PARTITION_FIFO_SIZE		SZ_8K

#define FIFO_ALIGNMENT			8
#define FIFO_FULL_RESERVE		8

enum ipcmem_channel_status {
	IPCMEM_CHANNEL_INACTIVE,
	IPCMEM_CHANNEL_IN_PROGRESS,
	IPCMEM_CHANNEL_ACTIVE,
};

struct ipcmem_toc_header {
	u32 magic_number;
	u32 init_done;
	u32 major_version;
	u32 minor_version;
	u64 feature_mask;
	u32 reserved[6];
};

struct ipcmem_offsets {
	u32 host_info;
	u32 global_entry;
	u32 partition_info;
	u32 partition_entry;
	u32 debug;
	u32 reserved;
};

struct ipcmem_toc {
	struct ipcmem_toc_header hdr;
	struct ipcmem_offsets offsets;
};

struct ipcmem_host_info {
	u32 hwlock_owner;
	u32 configured_host;
};

struct ipcmem_partition_entry {
	u32 base_offset;
	u32 size;
	u32 flags;
	u32 host0;
	u32 host1;
	u32 reserved;
};

struct ipcmem_partition_info {
	u32 num_entries;
	u32 entry_size;
};

struct ipcmem_global_partition_header {
	u32 partition_type;
	u32 region_offset;
	u32 region_size;
};

struct ipcmem_partition_header {
	u32 type;
	u32 desc_offset;
	u32 desc_size;
	u32 fifo0_offset;
	u32 fifo0_size;
	u32 fifo1_offset;
	u32 fifo1_size;
	u32 status;
};

struct qcom_ipclite_host {
	unsigned int id;
	const char *name;
};

struct qcom_ipclite_data {
	const struct qcom_ipclite_host *hosts;
	unsigned int num_hosts;
	u64 feature_mask;
};

struct ipclite_fifo {
	void *data;
	u32 length;
	__le32 *head;
	__le32 *tail;
};

struct qcom_ipclite;

struct ipclite_channel {
	struct qcom_ipclite *ipc;
	unsigned int remote;
	struct ipclite_fifo tx;
	struct ipclite_fifo rx;
	u32 *gstatus;
	u32 status;
	/* Protects tx and status */
	spinlock_t tx_lock;
	/* Protects rx */
	spinlock_t rx_lock;
	struct mbox_chan *msg_chan;
	struct mbox_chan *ssr_chan;
};

struct qcom_ipclite {
	struct device *dev;
	void *base;
	const struct qcom_ipclite_data *data;
	unsigned long hosts;
	struct ipcmem_host_info *host_info;
	struct ipcmem_partition_entry *entries;
	u32 num_entries;
	struct ipcmem_global_partition_header *global_hdr;
	struct ipclite_channel channels[IPCMEM_NUM_HOSTS];
	struct mbox_client mbox_client;
	struct hwspinlock *hwlock;
	/* Serializes recoveries */
	struct mutex ssr_lock;
	IPCLite_Client client_cb;
	void *client_priv;
};

static struct qcom_ipclite *ipclite_instance;

static struct qcom_ipclite *ipclite_get(void)
{
	/* Pairs with the release at the end of qcom_ipclite_probe() */
	return smp_load_acquire(&ipclite_instance);
}

static void qcom_ipclite_setup(struct qcom_ipclite *ipc)
{
	struct ipcmem_toc *toc = ipc->base;
	struct ipcmem_partition_entry *global_entry, *entry;
	struct ipcmem_partition_info *partition_info;
	struct ipcmem_partition_header *hdr;
	u32 offset = IPCMEM_TOC_DATA_OFFSET;
	u32 base_offset, num_entries = 0;
	unsigned int host0, host1;
	u32 *words = ipc->base;
	u32 magic = 0;
	int i;

	toc->offsets.host_info = offset;
	offset += sizeof(*ipc->host_info);
	toc->offsets.global_entry = offset;
	offset += sizeof(*global_entry);
	toc->offsets.partition_info = offset;
	offset += sizeof(*partition_info);
	toc->offsets.partition_entry = offset;

	ipc->host_info = ipc->base + toc->offsets.host_info;
	global_entry = ipc->base + toc->offsets.global_entry;
	partition_info = ipc->base + toc->offsets.partition_info;
	ipc->entries = ipc->base + toc->offsets.partition_entry;

	ipc->host_info->hwlock_owner = IPCMEM_INVALID_HOST;
	ipc->host_info->configured_host = ipc->hosts;

	global_entry->base_offset = IPCMEM_TOC_SIZE;
	global_entry->size = GLOBAL_PARTITION_SIZE;
	global_entry->flags = IPCMEM_FLAGS_RW_PROTECTION;
	global_entry->host0 = IPCMEM_GLOBAL_HOST;
	global_entry->host1 = IPCMEM_GLOBAL_HOST;

	ipc->global_hdr = ipc->base + IPCMEM_TOC_SIZE;
	ipc->global_hdr->partition_type = GLOBAL_PARTITION_TYPE;
	ipc->global_hdr->region_offset = GLOBAL_REGION_OFFSET;
	ipc->global_hdr->region_size = GLOBAL_REGION_SIZE;

	base_offset = IPCMEM_TOC_SIZE + GLOBAL_PARTITION_SIZE;
	entry = ipc->entries;
	for_each_set_bit(host0, &ipc->hosts, IPCMEM_NUM_HOSTS) {
		host1 = host0;
		for_each_set_bit_from(host1, &ipc->hosts, IPCMEM_NUM_HOSTS) {
			if (host0 == host1 && host0 != IPCMEM_APPS)
				continue;

			entry->base_offset = base_offset;
			entry->size = PARTITION_SIZE;
			entry->flags = IPCMEM_FLAGS_RW_PROTECTION;
			entry->host0 = host0;
			entry->host1 = host1;

			hdr = ipc->base + base_offset;
			hdr->desc_offset = PARTITION_DESC_OFFSET;
			hdr->desc_size = PARTITION_DESC_SIZE;
			hdr->fifo0_offset = PARTITION_FIFO0_OFFSET;
			hdr->fifo0_size = PARTITION_FIFO_SIZE;
			if (host0 == host1) {
				hdr->type = PARTITION_TYPE_LOOPBACK;
				hdr->fifo1_offset = PARTITION_FIFO0_OFFSET;
				hdr->status = IPCMEM_CHANNEL_ACTIVE;
			} else {
				hdr->type = PARTITION_TYPE_DEFAULT;
				hdr->fifo1_offset = PARTITION_FIFO1_OFFSET;
				if (host0 == IPCMEM_APPS)
					hdr->status = IPCMEM_CHANNEL_IN_PROGRESS;
			}
			hdr->fifo1_size = PARTITION_FIFO_SIZE;

			base_offset += PARTITION_SIZE;
			entry++;
			num_entries++;
		}
	}

	ipc->num_entries = num_entries;
	partition_info->num_entries = num_entries;
	partition_info->entry_size = sizeof(*entry);

	toc->hdr.major_version = IPCLITE_MAJOR_VERSION;
	toc->hdr.minor_version = IPCLITE_MINOR_VERSION;
	toc->hdr.feature_mask = ipc->data->feature_mask;

	/* A core that sees init_done reads the layout behind it */
	wmb();
	toc->hdr.init_done = IPCMEM_INIT_COMPLETED;

	/* The cores accept the TOC when magic_number is the inverted XOR of the other header words */
	for (i = 1; i < sizeof(toc->hdr) / sizeof(u32); i++)
		magic ^= words[i];
	toc->hdr.magic_number = ~magic;
}

static struct ipcmem_partition_header *
qcom_ipclite_partition(struct qcom_ipclite *ipc, unsigned int host0, unsigned int host1)
{
	u32 i;

	for (i = 0; i < ipc->num_entries; i++)
		if (ipc->entries[i].host0 == host0 && ipc->entries[i].host1 == host1)
			return ipc->base + ipc->entries[i].base_offset;

	return NULL;
}

static void qcom_ipclite_channel_init(struct qcom_ipclite *ipc, unsigned int remote)
{
	struct ipclite_channel *ch = &ipc->channels[remote];
	struct ipcmem_partition_header *hdr;
	__le32 *descs;

	hdr = qcom_ipclite_partition(ipc, IPCMEM_APPS, remote);
	descs = (void *)hdr + hdr->desc_offset;

	ch->ipc = ipc;
	ch->remote = remote;
	spin_lock_init(&ch->tx_lock);
	spin_lock_init(&ch->rx_lock);

	if (remote == IPCMEM_APPS) {
		/* The loopback partition shares FIFO0 and the second index pair */
		ch->tx.data = (void *)hdr + hdr->fifo1_offset;
		ch->tx.length = hdr->fifo1_size;
		ch->rx.data = (void *)hdr + hdr->fifo0_offset;
		ch->rx.length = hdr->fifo0_size;
		ch->tx.tail = &descs[2];
		ch->tx.head = &descs[3];
		ch->rx.tail = &descs[2];
		ch->rx.head = &descs[3];
	} else {
		ch->tx.data = (void *)hdr + hdr->fifo0_offset;
		ch->tx.length = hdr->fifo0_size;
		ch->rx.data = (void *)hdr + hdr->fifo1_offset;
		ch->rx.length = hdr->fifo1_size;
		ch->tx.tail = &descs[0];
		ch->tx.head = &descs[1];
		ch->rx.tail = &descs[2];
		ch->rx.head = &descs[3];
	}

	ch->gstatus = &hdr->status;
	ch->status = hdr->status;
}

static u32 ipclite_index(__le32 *index)
{
	return le32_to_cpu(READ_ONCE(*index));
}

static size_t ipclite_rx_avail(struct ipclite_fifo *fifo)
{
	u32 head = ipclite_index(fifo->head);
	u32 tail = ipclite_index(fifo->tail);
	size_t len;

	if (head < tail)
		len = fifo->length - tail + head;
	else
		len = head - tail;

	return len > fifo->length ? 0 : len;
}

static void ipclite_rx_read(struct ipclite_fifo *fifo, void *buf, size_t count)
{
	u32 tail = ipclite_index(fifo->tail);
	size_t len;

	if (tail > fifo->length)
		return;
	if (tail == fifo->length)
		tail = 0;

	len = min_t(size_t, count, fifo->length - tail);
	memcpy(buf, fifo->data + tail, len);
	if (len != count)
		memcpy(buf + len, fifo->data, count - len);
}

static void ipclite_rx_advance(struct ipclite_fifo *fifo, size_t count)
{
	u32 tail = ipclite_index(fifo->tail) + count;

	if (tail >= fifo->length)
		tail %= fifo->length;

	WRITE_ONCE(*fifo->tail, cpu_to_le32(tail));
}

static size_t ipclite_tx_avail(struct ipclite_fifo *fifo)
{
	u32 head = ipclite_index(fifo->head);
	u32 tail = ipclite_index(fifo->tail);
	size_t avail;

	if (tail <= head)
		avail = fifo->length - head + tail;
	else
		avail = tail - head;

	if (avail < FIFO_FULL_RESERVE)
		return 0;
	avail -= FIFO_FULL_RESERVE;

	return avail > fifo->length ? 0 : avail;
}

static void ipclite_tx_write(struct ipclite_fifo *fifo, const void *buf, size_t count)
{
	u32 head = ipclite_index(fifo->head);
	size_t len;

	if (head > fifo->length)
		return;

	len = min_t(size_t, count, fifo->length - head);
	memcpy(fifo->data + head, buf, len);
	if (len != count)
		memcpy(fifo->data, buf + len, count - len);

	head += count;
	if (head >= fifo->length)
		head -= fifo->length;
	head = ALIGN(head, FIFO_ALIGNMENT);
	if (head >= fifo->length)
		head -= fifo->length;

	/* The remote core reads the message once the head covers it */
	wmb();
	WRITE_ONCE(*fifo->head, cpu_to_le32(head));
}

static bool ipclite_channel_active(struct ipclite_channel *ch)
{
	if (ch->status == IPCMEM_CHANNEL_ACTIVE)
		return true;

	if (ch->status == IPCMEM_CHANNEL_IN_PROGRESS &&
	    READ_ONCE(*ch->gstatus) == IPCMEM_CHANNEL_ACTIVE) {
		ch->status = IPCMEM_CHANNEL_ACTIVE;
		return true;
	}

	return false;
}

static int ipclite_signal(struct mbox_chan *chan)
{
	int ret;

	ret = mbox_send_message(chan, NULL);
	if (ret < 0)
		return ret;

	mbox_client_txdone(chan, 0);

	return 0;
}

static struct ipclite_channel *ipclite_channel(struct qcom_ipclite *ipc, int host)
{
	if (host < 0 || host >= IPCMEM_NUM_HOSTS || !test_bit(host, &ipc->hosts))
		return NULL;

	return &ipc->channels[host];
}

int ipclite_msg_send(int32_t proc_id, uint64_t data)
{
	struct qcom_ipclite *ipc = ipclite_get();
	struct ipclite_channel *ch;
	unsigned long flags;
	int ret;

	if (!ipc)
		return -ENODEV;

	ch = ipclite_channel(ipc, proc_id);
	if (!ch)
		return -EINVAL;

	spin_lock_irqsave(&ch->tx_lock, flags);

	if (!ipclite_channel_active(ch)) {
		ret = -EOPNOTSUPP;
	} else if (ipclite_tx_avail(&ch->tx) < sizeof(data)) {
		ret = -EAGAIN;
	} else {
		ipclite_tx_write(&ch->tx, &data, sizeof(data));
		ret = ipclite_signal(ch->msg_chan);
	}

	spin_unlock_irqrestore(&ch->tx_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(ipclite_msg_send);

int ipclite_register_client(IPCLite_Client cb_func_ptr, void *priv)
{
	struct qcom_ipclite *ipc = ipclite_get();

	if (!ipc)
		return -ENODEV;

	if (!cb_func_ptr)
		return -EINVAL;

	WRITE_ONCE(ipc->client_priv, priv);
	/* The interrupt handler reads priv only after it sees the callback */
	smp_store_release(&ipc->client_cb, cb_func_ptr);

	return 0;
}
EXPORT_SYMBOL_GPL(ipclite_register_client);

int get_global_partition_info(struct global_region_info *global_ipcmem)
{
	struct qcom_ipclite *ipc = ipclite_get();

	if (!ipc)
		return -ENODEV;

	if (!global_ipcmem)
		return -EINVAL;

	global_ipcmem->virt_base = (void *)ipc->global_hdr + ipc->global_hdr->region_offset;
	global_ipcmem->size = ipc->global_hdr->region_size;

	return 0;
}
EXPORT_SYMBOL_GPL(get_global_partition_info);

/*
 * The TOC advertises IPCLITE_FEATURE_GLOBAL_ATOMIC, under which every core
 * updates the global partition with atomic instructions instead of the
 * IPCLite hardware mutex.
 */
uint32_t ipclite_global_test_and_set_bit(uint32_t nr, ipclite_atomic_uint32_t *addr)
{
	return atomic_fetch_or(BIT(nr), addr);
}
EXPORT_SYMBOL_GPL(ipclite_global_test_and_set_bit);

uint32_t ipclite_global_test_and_clear_bit(uint32_t nr, ipclite_atomic_uint32_t *addr)
{
	return atomic_fetch_andnot(BIT(nr), addr);
}
EXPORT_SYMBOL_GPL(ipclite_global_test_and_clear_bit);

void ipclite_recover(enum ipcmem_host_type core_id)
{
	struct qcom_ipclite *ipc = ipclite_get();
	struct ipcmem_partition_header *hdr;
	struct ipclite_channel *ch, *peer;
	unsigned long flags;
	unsigned int host;
	u32 i;

	if (!ipc)
		return;

	ch = ipclite_channel(ipc, core_id);
	if (!ch || core_id == IPCMEM_APPS)
		return;

	if (READ_ONCE(ipc->host_info->hwlock_owner) == core_id) {
		WRITE_ONCE(ipc->host_info->hwlock_owner, IPCMEM_INVALID_HOST);
		hwspin_unlock_raw(ipc->hwlock);
	}

	mutex_lock(&ipc->ssr_lock);

	for (i = 0; i < ipc->num_entries; i++) {
		if (ipc->entries[i].host0 != core_id && ipc->entries[i].host1 != core_id)
			continue;

		hdr = ipc->base + ipc->entries[i].base_offset;
		atomic_set((atomic_t *)&hdr->status, IPCMEM_CHANNEL_INACTIVE);
	}

	spin_lock_irqsave(&ch->rx_lock, flags);
	WRITE_ONCE(*ch->rx.tail, 0);
	spin_unlock_irqrestore(&ch->rx_lock, flags);

	spin_lock_irqsave(&ch->tx_lock, flags);
	WRITE_ONCE(*ch->tx.head, 0);
	ch->status = atomic_inc_return((atomic_t *)ch->gstatus);
	spin_unlock_irqrestore(&ch->tx_lock, flags);

	for_each_set_bit(host, &ipc->hosts, IPCMEM_NUM_HOSTS) {
		if (host == IPCMEM_APPS || host == core_id)
			continue;

		peer = &ipc->channels[host];
		spin_lock_irqsave(&peer->tx_lock, flags);
		if (ipclite_channel_active(peer) && ipclite_signal(peer->ssr_chan))
			dev_err(ipc->dev, "failed to notify host %u of the recovery of host %u\n",
				host, core_id);
		spin_unlock_irqrestore(&peer->tx_lock, flags);
	}

	mutex_unlock(&ipc->ssr_lock);
}
EXPORT_SYMBOL_GPL(ipclite_recover);

static irqreturn_t qcom_ipclite_msg_irq(int irq, void *dev_id)
{
	struct ipclite_channel *ch = dev_id;
	struct qcom_ipclite *ipc = ch->ipc;
	IPCLite_Client cb;
	u64 msg;

	/* Pairs with the release in ipclite_register_client() */
	cb = smp_load_acquire(&ipc->client_cb);

	spin_lock(&ch->rx_lock);

	while (ipclite_rx_avail(&ch->rx) >= sizeof(msg)) {
		/* Read the message only after the head that covers it */
		rmb();
		ipclite_rx_read(&ch->rx, &msg, sizeof(msg));
		ipclite_rx_advance(&ch->rx, ALIGN(sizeof(msg), FIFO_ALIGNMENT));

		if (cb)
			cb(ch->remote, msg, READ_ONCE(ipc->client_priv));
	}

	spin_unlock(&ch->rx_lock);

	return IRQ_HANDLED;
}

static void qcom_ipclite_free_chan(void *chan)
{
	mbox_free_channel(chan);
}

static struct mbox_chan *qcom_ipclite_request_chan(struct qcom_ipclite *ipc, const char *name)
{
	struct mbox_chan *chan;
	int ret;

	chan = mbox_request_channel_byname(&ipc->mbox_client, name);
	if (IS_ERR(chan))
		return ERR_PTR(dev_err_probe(ipc->dev, PTR_ERR(chan),
					     "failed to get the %s channel\n", name));

	ret = devm_add_action_or_reset(ipc->dev, qcom_ipclite_free_chan, chan);
	if (ret)
		return ERR_PTR(ret);

	return chan;
}

static int qcom_ipclite_get_resources(struct qcom_ipclite *ipc)
{
	struct device *dev = ipc->dev;
	const struct qcom_ipclite_host *host;
	struct ipclite_channel *ch;
	const char *name;
	unsigned int i;

	for (i = 0; i < ipc->data->num_hosts; i++) {
		host = &ipc->data->hosts[i];
		ch = &ipc->channels[host->id];

		ch->msg_chan = qcom_ipclite_request_chan(ipc, host->name);
		if (IS_ERR(ch->msg_chan))
			return PTR_ERR(ch->msg_chan);

		if (host->id == IPCMEM_APPS)
			continue;

		name = devm_kasprintf(dev, GFP_KERNEL, "%s-ssr", host->name);
		if (!name)
			return -ENOMEM;

		ch->ssr_chan = qcom_ipclite_request_chan(ipc, name);
		if (IS_ERR(ch->ssr_chan))
			return PTR_ERR(ch->ssr_chan);
	}

	return 0;
}

static int qcom_ipclite_request_irqs(struct qcom_ipclite *ipc)
{
	struct device *dev = ipc->dev;
	const struct qcom_ipclite_host *host;
	const char *name;
	unsigned int i;
	int irq, ret;

	for (i = 0; i < ipc->data->num_hosts; i++) {
		host = &ipc->data->hosts[i];

		irq = of_irq_get_byname(dev->of_node, host->name);
		if (irq < 0)
			return dev_err_probe(dev, irq, "failed to get the %s interrupt\n",
					     host->name);

		name = devm_kasprintf(dev, GFP_KERNEL, "ipclite-%s", host->name);
		if (!name)
			return -ENOMEM;

		ret = devm_request_irq(dev, irq, qcom_ipclite_msg_irq, IRQF_NO_SUSPEND, name,
				       &ipc->channels[host->id]);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request the %s interrupt\n",
					     host->name);
	}

	return 0;
}

static int qcom_ipclite_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mbox_chan *mem_init_chan;
	struct qcom_ipclite *ipc;
	struct resource res;
	unsigned int i;
	int ret;

	ipc = devm_kzalloc(dev, sizeof(*ipc), GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;

	ipc->dev = dev;
	ipc->data = device_get_match_data(dev);
	if (!ipc->data)
		return -EINVAL;

	for (i = 0; i < ipc->data->num_hosts; i++)
		__set_bit(ipc->data->hosts[i].id, &ipc->hosts);

	ret = devm_mutex_init(dev, &ipc->ssr_lock);
	if (ret)
		return ret;

	ret = of_reserved_mem_region_to_resource(dev->of_node, 0, &res);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get the IPCLite memory\n");

	ret = of_hwspin_lock_get_id(dev->of_node, 0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get the hwlock\n");

	ipc->hwlock = devm_hwspin_lock_request_specific(dev, ret);
	if (!ipc->hwlock)
		return dev_err_probe(dev, -EBUSY, "failed to request the hwlock\n");

	ipc->mbox_client.dev = dev;
	ipc->mbox_client.knows_txdone = true;

	mem_init_chan = qcom_ipclite_request_chan(ipc, "mem-init");
	if (IS_ERR(mem_init_chan))
		return PTR_ERR(mem_init_chan);

	ret = qcom_ipclite_get_resources(ipc);
	if (ret)
		return ret;

	ipc->base = devm_memremap(dev, res.start, resource_size(&res), MEMREMAP_WC);
	if (IS_ERR(ipc->base))
		return PTR_ERR(ipc->base);

	memset(ipc->base, 0, resource_size(&res));
	qcom_ipclite_setup(ipc);

	for (i = 0; i < ipc->data->num_hosts; i++)
		qcom_ipclite_channel_init(ipc, ipc->data->hosts[i].id);

	ret = qcom_ipclite_request_irqs(ipc);
	if (ret)
		return ret;

	ret = ipclite_signal(mem_init_chan);
	if (ret)
		return dev_err_probe(dev, ret, "failed to signal mem-init\n");

	/* The exported calls see the instance only once it is complete */
	smp_store_release(&ipclite_instance, ipc);

	dev_info(dev, "IPCLite %d.%d memory ready, hosts 0x%lx\n",
		 IPCLITE_MAJOR_VERSION, IPCLITE_MINOR_VERSION, ipc->hosts);

	return 0;
}

static const struct qcom_ipclite_host sm8650_ipclite_hosts[] = {
	{ IPCMEM_APPS, "apss" },
	{ IPCMEM_CDSP, "cdsp" },
	{ IPCMEM_CVP, "cvp" },
	{ IPCMEM_CAM, "cam" },
};

static const struct qcom_ipclite_data sm8650_ipclite_data = {
	.hosts = sm8650_ipclite_hosts,
	.num_hosts = ARRAY_SIZE(sm8650_ipclite_hosts),
	.feature_mask = IPCLITE_FEATURE_GLOBAL_ATOMIC | IPCLITE_FEATURE_TEST_SUITE,
};

static const struct of_device_id qcom_ipclite_of_match[] = {
	{ .compatible = "qcom,sm8650-ipclite", .data = &sm8650_ipclite_data },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_ipclite_of_match);

static struct platform_driver qcom_ipclite_driver = {
	.probe = qcom_ipclite_probe,
	.driver = {
		.name = "qcom-ipclite",
		.of_match_table = qcom_ipclite_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(qcom_ipclite_driver);

MODULE_DESCRIPTION("Qualcomm IPCLite shared memory protocol");
MODULE_LICENSE("GPL");
