// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2021, Linaro Limited

#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/q6usboffload.h>
#include "q6dsp-lpass-ports.h"
#include "q6dsp-common.h"
#include "audioreach.h"
#include "q6apm.h"
#include "q6prm.h"

#define AUDIOREACH_BE_PCM_BASE	16

struct q6apm_dai_priv_data {
	struct clk *mclk;
	struct clk *bclk;
	bool mclk_enabled, bclk_enabled;
};

struct q6apm_lpass_dai_data {
	struct q6apm_graph *graph[APM_PORT_MAX];
	bool is_port_started[APM_PORT_MAX];
	struct audioreach_module_config module_config[APM_PORT_MAX];
	u16 data_format[APM_PORT_MAX];
	struct q6apm_dai_priv_data priv[APM_PORT_MAX];
};

static const char * const q6apm_data_format_text[] = {
	"Topology", "Fixed Point", "IEC61937", "IEC60958", "DSD over PCM",
	"Generic Compressed", "Raw Compressed", "Compressed over PCM",
	"IEC60958 Non-Linear",
};

static int q6apm_data_format_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(q6apm_data_format_text),
				 q6apm_data_format_text);
}

static u16 *q6apm_data_format_of(struct snd_kcontrol *kcontrol)
{
	struct snd_soc_dai *dai = snd_kcontrol_chip(kcontrol);
	struct q6apm_lpass_dai_data *data = dev_get_drvdata(dai->dev);

	return &data->data_format[dai->id];
}

static int q6apm_data_format_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.enumerated.item[0] = READ_ONCE(*q6apm_data_format_of(kcontrol));

	return 0;
}

static int q6apm_data_format_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	unsigned int format = ucontrol->value.enumerated.item[0];
	u16 *value = q6apm_data_format_of(kcontrol);

	if (format >= ARRAY_SIZE(q6apm_data_format_text))
		return -EINVAL;

	if (READ_ONCE(*value) == format)
		return 0;

	WRITE_ONCE(*value, format);

	return 1;
}

static int q6apm_data_format_probe(struct snd_soc_dai *dai)
{
	struct snd_kcontrol_new control = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.info = q6apm_data_format_info,
		.get = q6apm_data_format_get,
		.put = q6apm_data_format_put,
	};

	control.name = devm_kasprintf(dai->dev, GFP_KERNEL, "%s Data Format", dai->name);
	if (!control.name)
		return -ENOMEM;

	return snd_soc_add_dai_controls(dai, &control, 1);
}

static void q6apm_lpass_dai_disable_clocks(struct q6apm_lpass_dai_data *dai_data, int id)
{
	if (dai_data->priv[id].mclk_enabled) {
		clk_disable_unprepare(dai_data->priv[id].mclk);
		dai_data->priv[id].mclk_enabled = false;
	}

	if (dai_data->priv[id].bclk_enabled) {
		clk_disable_unprepare(dai_data->priv[id].bclk);
		dai_data->priv[id].bclk_enabled = false;
	}
}

static void q6apm_lpass_dai_put_clocks(struct q6apm_lpass_dai_data *dai_data)
{
	int i;

	for (i = 0; i < APM_PORT_MAX; i++) {
		q6apm_lpass_dai_disable_clocks(dai_data, i);

		if (dai_data->priv[i].mclk) {
			clk_put(dai_data->priv[i].mclk);
			dai_data->priv[i].mclk = NULL;
		}
		if (dai_data->priv[i].bclk) {
			clk_put(dai_data->priv[i].bclk);
			dai_data->priv[i].bclk = NULL;
		}
	}
}

static int q6dma_set_channel_map(struct snd_soc_dai *dai,
				 unsigned int tx_num,
				 const unsigned int *tx_ch_mask,
				 unsigned int rx_num,
				 const unsigned int *rx_ch_mask)
{

	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int i;

	switch (dai->id) {
	case WSA_CODEC_DMA_TX_0:
	case WSA_CODEC_DMA_TX_1:
	case WSA_CODEC_DMA_TX_2:
	case VA_CODEC_DMA_TX_0:
	case VA_CODEC_DMA_TX_1:
	case VA_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
	case TX_CODEC_DMA_TX_4:
	case TX_CODEC_DMA_TX_5:
		if (!tx_ch_mask) {
			dev_err(dai->dev, "tx slot not found\n");
			return -EINVAL;
		}

		if (tx_num > PCM_MAX_NUM_CHANNEL) {
			dev_err(dai->dev, "invalid tx num %d\n",
				tx_num);
			return -EINVAL;
		}
		for (i = 0; i < tx_num; i++)
			cfg->channel_map[i] = tx_ch_mask[i];

		break;
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
	case RX_CODEC_DMA_RX_0:
	case RX_CODEC_DMA_RX_1:
	case RX_CODEC_DMA_RX_2:
	case RX_CODEC_DMA_RX_3:
	case RX_CODEC_DMA_RX_4:
	case RX_CODEC_DMA_RX_5:
	case RX_CODEC_DMA_RX_6:
	case RX_CODEC_DMA_RX_7:
		/* rx */
		if (!rx_ch_mask) {
			dev_err(dai->dev, "rx slot not found\n");
			return -EINVAL;
		}
		if (rx_num > APM_PORT_MAX_AUDIO_CHAN_CNT) {
			dev_err(dai->dev, "invalid rx num %d\n",
				rx_num);
			return -EINVAL;
		}
		for (i = 0; i < rx_num; i++)
			cfg->channel_map[i] = rx_ch_mask[i];

		break;
	default:
		dev_err(dai->dev, "%s: invalid dai id 0x%x\n",
			__func__, dai->id);
		return -EINVAL;
	}

	return 0;
}

static int q6hdmi_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int channels = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS)->max;
	int ret;

	cfg->bit_width = params_width(params);
	cfg->sample_rate = params_rate(params);
	cfg->num_channels = channels;
	ret = q6dsp_map_channels(cfg->channel_map, channels);
	if (ret)
		return ret;

	switch (dai->id) {
	case DISPLAY_PORT_RX_0:
		cfg->dp_idx = 0;
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		cfg->dp_idx = dai->id - DISPLAY_PORT_RX_1 + 1;
		break;
	}

	ret = q6dsp_get_channel_allocation(channels);
	if (ret < 0)
		return ret;

	cfg->channel_allocation = ret;

	return 0;
}

static int q6dma_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int channels = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS)->max;
	int ret;

	cfg->bit_width = params_width(params);
	cfg->sample_rate = params_rate(params);
	cfg->num_channels = channels;
	ret = q6dsp_map_channels(cfg->channel_map, channels);
	if (ret)
		return ret;

	return 0;
}

static int q6usb_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int ret;

	ret = q6usb_get_offload_token(snd_soc_substream_to_rtd(substream),
				      substream->stream, &cfg->usb_token);
	if (ret)
		return ret;

	return q6dma_hw_params(substream, params, dai);
}

static int q6slim_set_channel_map(struct snd_soc_dai *dai,
				  unsigned int tx_num, const unsigned int *tx_slot,
				  unsigned int rx_num, const unsigned int *rx_slot)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	const unsigned int *slot = rx_slot;
	unsigned int num = rx_num;
	int i;

	if (dai->id & 0x1) {
		slot = tx_slot;
		num = tx_num;
	}

	if (!slot || num > AR_SLIMBUS_MAX_CHANNELS)
		return -EINVAL;

	for (i = 0; i < num; i++) {
		if (slot[i] > U8_MAX)
			return -EINVAL;
	}

	memset(cfg->slim_channels, 0, sizeof(cfg->slim_channels));

	for (i = 0; i < num; i++)
		cfg->slim_channels[i] = slot[i];

	return 0;
}

static void q6apm_lpass_dai_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	int rc;

	if (dai_data->is_port_started[dai->id]) {
		rc = q6apm_graph_stop(dai_data->graph[dai->id]);
		dai_data->is_port_started[dai->id] = false;
		if (rc < 0)
			dev_err(dai->dev, "failed to stop APM port (%d)\n", rc);
	}

	if (dai_data->graph[dai->id]) {
		snd_soc_dai_set_dma_data(dai, substream, NULL);
		q6apm_graph_close(dai_data->graph[dai->id]);
		dai_data->graph[dai->id] = NULL;
	}
}

static int q6apm_lpass_dai_trigger(struct snd_pcm_substream *substream, int cmd,
				   struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!dai_data->is_port_started[dai->id]) {
			ret = q6apm_graph_start(dai_data->graph[dai->id]);
			if (ret < 0)
				dev_err(dai->dev, "Failed to start APM port %d\n", dai->id);
			else
				dai_data->is_port_started[dai->id] = true;
		}
		break;
	default:
		break;
	}

	return ret;
}

static int q6apm_lpass_dai_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	struct q6apm_graph *graph;
	int graph_id, rc;

	graph_id = audioreach_get_dai_graph_id(snd_soc_substream_to_rtd(substream), dai,
					       substream->stream);
	if (graph_id < 0)
		return graph_id;

	graph = dai_data->graph[dai->id];
	if (graph && graph->id != graph_id)
		return -EBUSY;

	if (graph)
		graph->path_prepared = false;

	if (dai_data->is_port_started[dai->id]) {
		rc = q6apm_graph_stop(dai_data->graph[dai->id]);
		if (rc)
			return rc;
		dai_data->is_port_started[dai->id] = false;

	}

	/**
	 * It is recommend to load DSP with source graph first and then sink
	 * graph, so sequence for playback and capture will be different
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && dai_data->graph[dai->id] == NULL) {
		graph = q6apm_graph_open(dai->dev, NULL, dai->dev, graph_id, substream->stream);
		if (IS_ERR(graph)) {
			dev_err(dai->dev, "Failed to open graph (%d)\n", graph_id);
			rc = PTR_ERR(graph);
			return rc;
		}
		dai_data->graph[dai->id] = graph;
		snd_soc_dai_set_dma_data(dai, substream, graph);
	}

	cfg->direction = substream->stream;
	cfg->data_format = READ_ONCE(dai_data->data_format[dai->id]);
	rc = q6apm_graph_media_format_pcm(dai_data->graph[dai->id], cfg);
	if (rc) {
		dev_err(dai->dev, "Failed to set media format %d\n", rc);
		goto err;
	}

	rc = q6apm_graph_prepare(dai_data->graph[dai->id]);
	if (rc) {
		dev_err(dai->dev, "Failed to prepare Graph %d\n", rc);
		goto err;
	}
	dai_data->graph[dai->id]->path_prepared = true;

	return 0;
err:
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		snd_soc_dai_set_dma_data(dai, substream, NULL);
		q6apm_graph_close(dai_data->graph[dai->id]);
		dai_data->graph[dai->id] = NULL;
	}
	return rc;
}

static int q6apm_lpass_dai_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct q6apm_graph *graph;
	int graph_id;

	graph_id = audioreach_get_dai_graph_id(snd_soc_substream_to_rtd(substream), dai,
					       substream->stream);
	if (graph_id < 0)
		return graph_id;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		graph = q6apm_graph_open(dai->dev, NULL, dai->dev, graph_id, substream->stream);
		if (IS_ERR(graph)) {
			dev_err(dai->dev, "Failed to open graph (%d)\n", graph_id);
			return PTR_ERR(graph);
		}
		dai_data->graph[dai->id] = graph;
		snd_soc_dai_set_dma_data(dai, substream, graph);
	}

	return 0;
}

static int q6i2s_dai_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	return q6apm_lpass_dai_startup(substream, dai);
}

static void q6i2s_lpass_dai_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);

	q6apm_lpass_dai_shutdown(substream, dai);
	q6apm_lpass_dai_disable_clocks(dai_data, dai->id);
}

static int q6i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id, unsigned int freq, int dir)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct clk *sysclk = NULL;
	bool *enabled = NULL;
	int ret = 0;

	switch (clk_id) {
	case LPAIF_MI2S_MCLK:
		sysclk = dai_data->priv[dai->id].mclk;
		enabled = &dai_data->priv[dai->id].mclk_enabled;
		break;
	case LPAIF_MI2S_BCLK:
		sysclk = dai_data->priv[dai->id].bclk;
		enabled = &dai_data->priv[dai->id].bclk_enabled;
		break;
	default:
		return -EINVAL;
	}

	if (sysclk) {
		ret = clk_set_rate(sysclk, freq);
		if (ret) {
			dev_err(dai->dev, "Error, Unable to set rate (%d) for sysclk %d\n",
				freq, clk_id);
			return ret;
		}

		if (*enabled)
			return 0;

		ret = clk_prepare_enable(sysclk);
		if (ret) {
			dev_err(dai->dev, "Error, Unable to prepare (%d) sysclk\n", clk_id);
			return ret;
		}

		*enabled = true;
	}

	return ret;
}

static int q6i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];

	cfg->fmt = fmt;

	return 0;
}

static int q6tdm_set_tdm_slot(struct snd_soc_dai *dai,
			      unsigned int tx_mask,
			      unsigned int rx_mask,
			      int slots, int slot_width)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	unsigned int cap_mask, slot_mask;

	if (slot_width != 16 && slot_width != 32) {
		dev_err(dai->dev, "%s: invalid slot_width %d\n", __func__, slot_width);
		return -EINVAL;
	}

	switch (slots) {
	case 2:
	case 4:
	case 8:
	case 16:
		cap_mask = GENMASK(slots - 1, 0);
		break;
	default:
		dev_err(dai->dev, "%s: invalid slots %d\n", __func__, slots);
		return -EINVAL;
	}

	switch (dai->id) {
	case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7:
		slot_mask = (dai->id & 0x1) ? tx_mask : rx_mask;
		if (slot_mask & ~cap_mask) {
			dev_err(dai->dev, "%s: invalid slot mask 0x%x for %d slots\n",
				__func__, slot_mask, slots);
			return -EINVAL;
		}

		cfg->nslots_per_frame = slots;
		cfg->slot_width = slot_width;
		cfg->slot_mask = slot_mask;
		break;
	default:
		dev_err(dai->dev, "%s: invalid dai id 0x%x\n", __func__, dai->id);
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dai_ops q6dma_ops = {
	.probe		= q6apm_data_format_probe,
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.set_channel_map  = q6dma_set_channel_map,
	.hw_params        = q6dma_hw_params,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6i2s_ops = {
	.probe		= q6apm_data_format_probe,
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6i2s_dai_startup,
	.shutdown	= q6i2s_lpass_dai_shutdown,
	.set_channel_map  = q6dma_set_channel_map,
	.hw_params        = q6dma_hw_params,
	.set_fmt	= q6i2s_set_fmt,
	.set_sysclk	= q6i2s_set_sysclk,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6hdmi_ops = {
	.probe		= q6apm_data_format_probe,
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.hw_params	= q6hdmi_hw_params,
	.set_fmt	= q6i2s_set_fmt,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6tdm_ops = {
	.probe		= q6apm_data_format_probe,
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6i2s_lpass_dai_shutdown,
	.set_tdm_slot	= q6tdm_set_tdm_slot,
	.hw_params	= q6dma_hw_params,
	.set_fmt	= q6i2s_set_fmt,
	.set_sysclk	= q6i2s_set_sysclk,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6slim_ops = {
	.probe		= q6apm_data_format_probe,
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.set_channel_map  = q6slim_set_channel_map,
	.hw_params	= q6dma_hw_params,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6usb_ops = {
	.probe		= q6apm_data_format_probe,
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.hw_params	= q6usb_hw_params,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_component_driver q6apm_lpass_dai_component = {
	.name = "q6apm-be-dai-component",
	.of_xlate_dai_name = q6dsp_audio_ports_of_xlate_dai_name,
	.be_pcm_base = AUDIOREACH_BE_PCM_BASE,
	.use_dai_pcm_id = true,
	.remove_order   = SND_SOC_COMP_ORDER_FIRST,
};

static int of_q6apm_parse_dai_data(struct device *dev,
				   struct q6apm_lpass_dai_data *data)
{
	int ret;

	for_each_child_of_node_scoped(dev->of_node, node) {
		struct q6apm_dai_priv_data *priv;
		int id;

		ret = of_property_read_u32(node, "reg", &id);
		if (ret || id < 0 || id >= APM_PORT_MAX) {
			dev_err(dev, "valid dai id not found:%d\n", ret);
			continue;
		}

		switch (id) {
		/* MI2S specific properties */
		case PRIMARY_MI2S_RX ... QUATERNARY_MI2S_TX:
		case QUINARY_MI2S_RX ... QUINARY_MI2S_TX:
		case SENARY_MI2S_RX ... SENARY_MI2S_TX:
		case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7:
			priv = &data->priv[id];
			priv->mclk = of_clk_get_by_name(node, "mclk");
			if (IS_ERR(priv->mclk)) {
				int err = PTR_ERR(priv->mclk);

				priv->mclk = NULL;
				if (err == -EPROBE_DEFER) {
					q6apm_lpass_dai_put_clocks(data);
					return dev_err_probe(dev, err,
							     "unable to get mi2s mclk\n");
				}
			}

			priv->bclk = of_clk_get_by_name(node, "bclk");
			if (IS_ERR(priv->bclk)) {
				int err = PTR_ERR(priv->bclk);

				priv->bclk = NULL;
				if (err == -EPROBE_DEFER) {
					q6apm_lpass_dai_put_clocks(data);
					return dev_err_probe(dev, err,
							     "unable to get mi2s bclk\n");
				}
			}
			break;
		default:
			break;
		}
	}

	return 0;
}

static void q6apm_lpass_dai_clocks_action(void *data)
{
	q6apm_lpass_dai_put_clocks(data);
}

static int q6apm_lpass_dai_dev_probe(struct platform_device *pdev)
{
	struct q6dsp_audio_port_dai_driver_config cfg;
	struct q6apm_lpass_dai_data *dai_data;
	struct snd_soc_dai_driver *dais;
	struct device *dev = &pdev->dev;
	int num_dais;
	int ret;

	dai_data = devm_kzalloc(dev, sizeof(*dai_data), GFP_KERNEL);
	if (!dai_data)
		return -ENOMEM;

	dev_set_drvdata(dev, dai_data);
	ret = of_q6apm_parse_dai_data(dev, dai_data);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, q6apm_lpass_dai_clocks_action, dai_data);
	if (ret)
		return ret;

	memset(&cfg, 0, sizeof(cfg));
	cfg.usb_capture = true;
	cfg.q6i2s_ops = &q6i2s_ops;
	cfg.q6dma_ops = &q6dma_ops;
	cfg.q6hdmi_ops = &q6hdmi_ops;
	cfg.q6slim_ops = &q6slim_ops;
	cfg.q6usb_ops = &q6usb_ops;
	cfg.q6tdm_ops = &q6tdm_ops;
	dais = q6dsp_audio_ports_set_config(dev, &cfg, &num_dais);

	return devm_snd_soc_register_component(dev, &q6apm_lpass_dai_component, dais, num_dais);
}

#ifdef CONFIG_OF
static const struct of_device_id q6apm_lpass_dai_device_id[] = {
	{ .compatible = "qcom,q6apm-lpass-dais" },
	{},
};
MODULE_DEVICE_TABLE(of, q6apm_lpass_dai_device_id);
#endif

static struct platform_driver q6apm_lpass_dai_platform_driver = {
	.driver = {
		.name = "q6apm-lpass-dais",
		.of_match_table = of_match_ptr(q6apm_lpass_dai_device_id),
	},
	.probe = q6apm_lpass_dai_dev_probe,
};
module_platform_driver(q6apm_lpass_dai_platform_driver);

MODULE_DESCRIPTION("AUDIOREACH APM LPASS dai driver");
MODULE_LICENSE("GPL");
