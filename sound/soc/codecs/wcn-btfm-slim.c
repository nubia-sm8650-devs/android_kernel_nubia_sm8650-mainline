// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm WCN Bluetooth SLIMbus audio interface
 *
 * The Bluetooth part of Qualcomm WCN chips exchanges SCO and offloaded A2DP
 * audio with the audio DSP over SLIMbus. The chip enumerates as a ported
 * generic device (PGD) carrying the data ports and an interface device (IFD)
 * holding the port configuration registers.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slimbus.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define BTFM_SLIM_MANF_ID_QCOM		0x217
#define BTFM_SLIM_PROD_CODE_HMT		0x220

#define BTFM_REG_OFFSET			0x800
#define BTFM_REG_HW_REV_MSB		0x01
#define BTFM_REG_HW_REV_LSB		0x02
#define BTFM_REG_RX_CFG(n)		(0x40 + (n))
#define BTFM_REG_TX_CFG(n)		(0x50 + (n))
#define BTFM_REG_TX_MULTI_CHNL_0(n)	(0x100 + 0x4 * (n))
#define BTFM_REG_RX_MULTI_CHNL_0(n)	(0x180 + 0x4 * (n))
#define BTFM_REG_TX_OR_UR_CFG(n)	(0x1f0 + (n))

#define BTFM_PORT_ENABLE		BIT(0)
#define BTFM_PORT_WM_L1			(0x1 << 1)
#define BTFM_PORT_WM_LB			(0xb << 1)
#define BTFM_TX_OVERRUN_RECOVERY	BIT(1)
#define BTFM_TX_UNDERRUN_RECOVERY	BIT(0)

/* PGD port numbers, receive ports start at 16 */
#define BTFM_PORT_TX_SCO		0
#define BTFM_PORT_RX_BASE		16
#define BTFM_PORT_RX_SCO		16

#define BTFM_REG_RETRIES		3
#define BTFM_PRESENT_TIMEOUT_MS		100
#define BTFM_PORT_RESET_DELAY_MS	200

enum {
	BTFM_DAI_SCO_A2DP_RX,
	BTFM_DAI_SCO_TX,
	BTFM_NUM_DAIS,
};

struct btfm_slim_dai {
	struct slim_stream_runtime *rt;
	unsigned int rate;
	unsigned int bps;
	unsigned int ch;
	bool enabled;
};

struct btfm_slim {
	struct device *dev;
	struct slim_device *pgd;
	struct slim_device *ifd;
	struct btfm_slim_dai dais[BTFM_NUM_DAIS];
	/* Serializes port setup on the device */
	struct mutex lock;
	unsigned int ports_open;
	int feedback;
};

static int btfm_slim_write(struct btfm_slim *btfm, u16 reg, u8 val)
{
	int i, ret = -EIO;

	for (i = 0; i < BTFM_REG_RETRIES; i++) {
		ret = slim_writeb(btfm->ifd, BTFM_REG_OFFSET + reg, val);
		if (!ret)
			return 0;
		usleep_range(5000, 5100);
	}

	dev_err(btfm->dev, "failed to write 0x%02x to 0x%03x: %d\n",
		val, reg, ret);
	return ret;
}

static int btfm_slim_read(struct btfm_slim *btfm, u16 reg)
{
	int i, ret = -EIO;

	for (i = 0; i < BTFM_REG_RETRIES; i++) {
		ret = slim_readb(btfm->ifd, BTFM_REG_OFFSET + reg);
		if (ret >= 0)
			return ret;
		usleep_range(5000, 5100);
	}

	return ret;
}

static int btfm_slim_rx_port_en(struct btfm_slim *btfm, unsigned int port,
				unsigned int rate, bool enable)
{
	unsigned int n = port - BTFM_PORT_RX_BASE;
	int ret;

	/* The 44.1 kHz rates leave the channel map unset */
	if (enable && rate != 44100 && rate != 88200) {
		ret = btfm_slim_write(btfm, BTFM_REG_RX_MULTI_CHNL_0(n), BIT(n));
		if (ret)
			return ret;
	}

	return btfm_slim_write(btfm, BTFM_REG_RX_CFG(n),
			       enable ? BTFM_PORT_ENABLE | BTFM_PORT_WM_LB : 0);
}

static int btfm_slim_tx_port_en(struct btfm_slim *btfm, unsigned int port,
				bool enable)
{
	int ret;

	if (enable) {
		ret = btfm_slim_write(btfm, BTFM_REG_TX_MULTI_CHNL_0(port),
				      BIT(port));
		if (ret)
			return ret;

		ret = btfm_slim_write(btfm, BTFM_REG_TX_OR_UR_CFG(port),
				      BTFM_TX_OVERRUN_RECOVERY |
				      BTFM_TX_UNDERRUN_RECOVERY);
		if (ret)
			return ret;
	}

	return btfm_slim_write(btfm, BTFM_REG_TX_CFG(port),
			       enable ? BTFM_PORT_ENABLE | BTFM_PORT_WM_L1 : 0);
}

static int btfm_slim_port_en(struct btfm_slim *btfm, int id, bool enable)
{
	if (id == BTFM_DAI_SCO_A2DP_RX)
		return btfm_slim_rx_port_en(btfm, BTFM_PORT_RX_SCO,
					    btfm->dais[id].rate, enable);

	return btfm_slim_tx_port_en(btfm, BTFM_PORT_TX_SCO, enable);
}

/*
 * Bluetooth enumerates the chip again whenever it powers it up, so ask the
 * manager for the current logical addresses on every start.
 */
static int btfm_slim_query_laddr(struct slim_device *sdev)
{
	unsigned long timeout = jiffies +
				msecs_to_jiffies(BTFM_PRESENT_TIMEOUT_MS);
	int ret;

	sdev->is_laddr_valid = false;

	do {
		ret = slim_get_logical_addr(sdev);
		if (!ret)
			return 0;
		usleep_range(1000, 1100);
	} while (time_before(jiffies, timeout));

	return ret;
}

static int btfm_slim_get_laddr(struct btfm_slim *btfm)
{
	int ret;

	ret = btfm_slim_query_laddr(btfm->pgd);
	if (ret)
		return ret;

	if (!btfm->ifd)
		return -ENODEV;

	return btfm_slim_query_laddr(btfm->ifd);
}

static void btfm_slim_stream_stop(struct btfm_slim *btfm, int id)
{
	struct btfm_slim_dai *bdai = &btfm->dais[id];

	if (!bdai->enabled)
		return;

	slim_stream_disable(bdai->rt);
	slim_stream_unprepare(bdai->rt);
	btfm_slim_port_en(btfm, id, false);
	bdai->enabled = false;

	/* The chip resets its SLIMbus block once the last port closes */
	if (!--btfm->ports_open)
		msleep(BTFM_PORT_RESET_DELAY_MS);
}

static int btfm_slim_dai_startup(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);
	struct btfm_slim_dai *bdai = &btfm->dais[dai->id];
	int ret;

	/* Not under the lock, the status callback runs from here */
	ret = btfm_slim_get_laddr(btfm);
	if (ret) {
		dev_err(btfm->dev, "Bluetooth SLIMbus device not present: %d\n",
			ret);
		return ret;
	}

	guard(mutex)(&btfm->lock);

	bdai->rt = slim_stream_allocate(btfm->pgd, dai->name);
	if (IS_ERR(bdai->rt)) {
		ret = PTR_ERR(bdai->rt);
		bdai->rt = NULL;
		return ret;
	}

	return 0;
}

static void btfm_slim_dai_shutdown(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);
	struct btfm_slim_dai *bdai = &btfm->dais[dai->id];

	guard(mutex)(&btfm->lock);

	btfm_slim_stream_stop(btfm, dai->id);

	if (bdai->rt) {
		slim_stream_free(bdai->rt);
		bdai->rt = NULL;
	}
}

static int btfm_slim_dai_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);
	struct btfm_slim_dai *bdai = &btfm->dais[dai->id];

	guard(mutex)(&btfm->lock);

	bdai->rate = params_rate(params);
	bdai->bps = params_width(params);

	return 0;
}

static int btfm_slim_dai_hw_free(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);

	guard(mutex)(&btfm->lock);

	btfm_slim_stream_stop(btfm, dai->id);

	return 0;
}

static int btfm_slim_dai_prepare(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);
	struct btfm_slim_dai *bdai = &btfm->dais[dai->id];
	struct slim_stream_config cfg = {};
	unsigned int ch = bdai->ch;
	int ret;

	guard(mutex)(&btfm->lock);

	if (bdai->enabled)
		return 0;

	ret = btfm_slim_port_en(btfm, dai->id, true);
	if (ret)
		return ret;

	cfg.rate = bdai->rate;
	cfg.bps = bdai->bps;
	cfg.ch_count = 1;
	cfg.chs = &ch;
	cfg.direction = substream->stream;
	cfg.port_mask = BIT(dai->id == BTFM_DAI_SCO_A2DP_RX ?
			    BTFM_PORT_RX_SCO : BTFM_PORT_TX_SCO);

	ret = slim_stream_prepare(bdai->rt, &cfg);
	if (ret)
		goto err_port;

	ret = slim_stream_enable(bdai->rt);
	if (ret)
		goto err_unprepare;

	bdai->enabled = true;
	btfm->ports_open++;

	return 0;

err_unprepare:
	slim_stream_unprepare(bdai->rt);
err_port:
	btfm_slim_port_en(btfm, dai->id, false);
	return ret;
}

static int btfm_slim_dai_get_channel_map(const struct snd_soc_dai *dai,
					 unsigned int *tx_num,
					 unsigned int *tx_slot,
					 unsigned int *rx_num,
					 unsigned int *rx_slot)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);
	unsigned int ch = btfm->dais[dai->id].ch;

	*tx_num = 0;
	*rx_num = 0;

	if (dai->id == BTFM_DAI_SCO_A2DP_RX) {
		*rx_num = 1;
		rx_slot[0] = ch;
	} else {
		*tx_num = 1;
		tx_slot[0] = ch;
	}

	return 0;
}

static const struct snd_soc_dai_ops btfm_slim_dai_ops = {
	.startup = btfm_slim_dai_startup,
	.shutdown = btfm_slim_dai_shutdown,
	.hw_params = btfm_slim_dai_hw_params,
	.hw_free = btfm_slim_dai_hw_free,
	.prepare = btfm_slim_dai_prepare,
	.get_channel_map = btfm_slim_dai_get_channel_map,
};

#define BTFM_SLIM_RATES	(SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 | \
			 SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
			 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | \
			 SNDRV_PCM_RATE_192000)

static struct snd_soc_dai_driver btfm_slim_dais[] = {
	[BTFM_DAI_SCO_A2DP_RX] = {
		.name = "btfm_bt_sco_a2dp_slim_rx",
		.id = BTFM_DAI_SCO_A2DP_RX,
		.playback = {
			.stream_name = "SCO A2DP RX Playback",
			.rates = BTFM_SLIM_RATES,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min = 8000,
			.rate_max = 192000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.ops = &btfm_slim_dai_ops,
	},
	[BTFM_DAI_SCO_TX] = {
		.name = "btfm_bt_sco_slim_tx",
		.id = BTFM_DAI_SCO_TX,
		.capture = {
			.stream_name = "SCO TX Capture",
			.rates = BTFM_SLIM_RATES,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min = 8000,
			.rate_max = 192000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.ops = &btfm_slim_dai_ops,
	},
};

static int btfm_slim_feedback_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = btfm->feedback;

	return 0;
}

static int btfm_slim_feedback_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(component);
	int val = ucontrol->value.integer.value[0];

	if (btfm->feedback == val)
		return 0;

	btfm->feedback = val;

	return 1;
}

static const struct snd_kcontrol_new btfm_slim_controls[] = {
	/* PAL marks the A2DP feedback path with this before it opens SCO TX */
	SOC_SINGLE_EXT("BT set feedback channel", SND_SOC_NOPM, 0, 1, 0,
		       btfm_slim_feedback_get, btfm_slim_feedback_put),
};

static const struct snd_soc_component_driver btfm_slim_component = {
	.name = "btfm-slim",
	.controls = btfm_slim_controls,
	.num_controls = ARRAY_SIZE(btfm_slim_controls),
};

static int btfm_slim_status(struct slim_device *sdev,
			    enum slim_device_status status)
{
	struct btfm_slim *btfm = dev_get_drvdata(&sdev->dev);
	struct device_node *ifd_np;
	int msb, lsb;

	if (status != SLIM_DEVICE_STATUS_UP)
		return 0;

	if (!btfm->ifd) {
		ifd_np = of_parse_phandle(sdev->dev.of_node, "slim-ifc-dev", 0);
		if (!ifd_np)
			return -EINVAL;

		btfm->ifd = of_slim_get_device(sdev->ctrl, ifd_np);
		of_node_put(ifd_np);
		if (!btfm->ifd)
			return -EINVAL;
	}

	if (slim_get_logical_addr(btfm->ifd))
		return 0;

	msb = btfm_slim_read(btfm, BTFM_REG_HW_REV_MSB);
	lsb = btfm_slim_read(btfm, BTFM_REG_HW_REV_LSB);
	if (msb >= 0 && lsb >= 0)
		dev_info(btfm->dev, "SLIMbus slave hardware revision %d.%d.%d\n",
			 (msb >> 4) & 0xf, msb & 0xf, lsb);

	return 0;
}

static int btfm_slim_probe(struct slim_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct btfm_slim *btfm;
	u32 ch;
	int ret;

	btfm = devm_kzalloc(dev, sizeof(*btfm), GFP_KERNEL);
	if (!btfm)
		return -ENOMEM;

	btfm->dev = dev;
	btfm->pgd = sdev;

	ret = devm_mutex_init(dev, &btfm->lock);
	if (ret)
		return ret;

	ret = of_property_read_u32_index(dev->of_node, "qcom,rx-channels", 0,
					 &ch);
	if (ret)
		return dev_err_probe(dev, ret, "missing qcom,rx-channels\n");
	btfm->dais[BTFM_DAI_SCO_A2DP_RX].ch = ch;

	ret = of_property_read_u32_index(dev->of_node, "qcom,tx-channels", 0,
					 &ch);
	if (ret)
		return dev_err_probe(dev, ret, "missing qcom,tx-channels\n");
	btfm->dais[BTFM_DAI_SCO_TX].ch = ch;

	dev_set_drvdata(dev, btfm);

	return devm_snd_soc_register_component(dev, &btfm_slim_component,
					       btfm_slim_dais,
					       ARRAY_SIZE(btfm_slim_dais));
}

static const struct slim_device_id btfm_slim_id[] = {
	{ BTFM_SLIM_MANF_ID_QCOM, BTFM_SLIM_PROD_CODE_HMT, 0x1, 0x0 },
	{}
};
MODULE_DEVICE_TABLE(slim, btfm_slim_id);

static struct slim_driver btfm_slim_driver = {
	.driver = {
		.name = "wcn-btfm-slim",
	},
	.probe = btfm_slim_probe,
	.device_status = btfm_slim_status,
	.id_table = btfm_slim_id,
};
module_slim_driver(btfm_slim_driver);

MODULE_DESCRIPTION("Qualcomm WCN Bluetooth SLIMbus audio interface");
MODULE_LICENSE("GPL");
