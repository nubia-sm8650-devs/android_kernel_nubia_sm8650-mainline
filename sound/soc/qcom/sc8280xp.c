// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/soc-dpcm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include "qdsp6/q6afe.h"
#include "qdsp6/q6apm.h"
#include "qdsp6/q6prm.h"
#include "qdsp6/q6dsp-common.h"
#include "common.h"
#include "sdw.h"

#define I2S_MCLKFS 256
#define SLIM_MAX_PORTS 16

#define I2S_MCLK_RATE(rate) \
	((rate) * (I2S_MCLKFS))
#define I2S_BIT_RATE(rate, channels, format) \
	((rate) * (channels) * (format))

static struct snd_soc_dapm_widget sc8280xp_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_SPK("DP0 Jack", NULL),
	SND_SOC_DAPM_SPK("DP1 Jack", NULL),
	SND_SOC_DAPM_SPK("DP2 Jack", NULL),
	SND_SOC_DAPM_SPK("DP3 Jack", NULL),
	SND_SOC_DAPM_SPK("DP4 Jack", NULL),
	SND_SOC_DAPM_SPK("DP5 Jack", NULL),
	SND_SOC_DAPM_SPK("DP6 Jack", NULL),
	SND_SOC_DAPM_SPK("DP7 Jack", NULL),
};

static const struct snd_kcontrol_new max98090_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headset Mic12"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic56"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Receiver"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
};

static const struct snd_soc_dapm_widget max98090_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic12", NULL),
	SND_SOC_DAPM_MIC("Headset Mic56", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_SPK("Receiver", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

struct qcom_snd_soc_common {
	const char *driver_name;
	const struct snd_soc_dapm_widget *dapm_widgets;
	int num_dapm_widgets;
	const struct snd_soc_dapm_route *dapm_routes;
	int num_dapm_routes;
	const struct snd_kcontrol_new *controls;
	int num_controls;
	unsigned int codec_dai_fmt;
	bool codec_sysclk_set;
	bool mi2s_mclk_enable;
	bool mi2s_bclk_enable;
	bool wcd_jack;
	int (*snd_prepare)(struct snd_pcm_substream *substream);
};

struct sc8280xp_be_config {
	unsigned int rate;
	unsigned int channels_min;
	unsigned int channels_max;
	snd_pcm_format_t format;
};

struct sc8280xp_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	const struct qcom_snd_soc_common *priv;
	bool jack_setup;
	struct sc8280xp_be_config be_config[AFE_PORT_MAX];
};

enum {
	SC8280XP_BE_RATE,
	SC8280XP_BE_CHANNELS,
	SC8280XP_BE_FORMAT,
};

#define SC8280XP_BE_CONTROL(id, kind)	(((id) << 2) | (kind))
#define SC8280XP_BE_CONTROL_ID(value)	((value) >> 2)
#define SC8280XP_BE_CONTROL_KIND(value)	((value) & 0x3)

static const char * const sc8280xp_be_rate_text[] = {
	"8000", "11025", "12000", "16000", "22050", "24000", "32000", "44100",
	"48000", "88200", "96000", "176400", "192000", "352800", "384000",
};

static const unsigned int sc8280xp_be_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100,
	48000, 88200, 96000, 176400, 192000, 352800, 384000,
};

static const char * const sc8280xp_be_channels_text[] = {
	"1", "2", "3", "4", "5", "6", "7", "8",
};

static const char * const sc8280xp_be_format_text[] = {
	"S16_LE", "S24_LE", "S24_3LE", "S32_LE",
};

static const snd_pcm_format_t sc8280xp_be_formats[] = {
	SNDRV_PCM_FORMAT_S16_LE, SNDRV_PCM_FORMAT_S24_LE,
	SNDRV_PCM_FORMAT_S24_3LE, SNDRV_PCM_FORMAT_S32_LE,
};

static inline int sc8280xp_get_mclk_freq(struct snd_pcm_hw_params *params)
{
	int rate = params_rate(params);

	switch (rate) {
	case 11025:
	case 44100:
	case 88200:
		return I2S_MCLK_RATE(44100);
	default:
		break;
	}

	return I2S_MCLK_RATE(rate);
}

static inline int sc8280xp_get_bclk_freq(struct snd_pcm_hw_params *params)
{
	return I2S_BIT_RATE(params_rate(params),
			    params_channels(params),
			    snd_pcm_format_width(params_format(params)));
}

static int sc8280xp_tdm_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	struct qcom_snd_tdm_slot_cfg cpu_cfg;
	struct qcom_snd_tdm_slot_cfg codec_cfg;
	int bclk_freq;
	int ret;
	int i;

	ret = qcom_snd_get_dai_tdm_slots(rtd, &cpu_cfg, &codec_cfg);
	if (ret)
		return ret == -ENOENT ? 0 : ret;

	if (!cpu_cfg.slots)
		return 0;

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
	if (ret && ret != -ENOTSUPP)
		return ret;

	if (data->priv->codec_dai_fmt) {
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			ret = snd_soc_dai_set_fmt(codec_dai,
						  data->priv->codec_dai_fmt);
			if (ret && ret != -ENOTSUPP)
				return ret;
		}
	}

	ret = qcom_snd_apply_dai_tdm_slots_cfg(rtd, &cpu_cfg, &codec_cfg);
	if (ret)
		return ret;

	bclk_freq = snd_soc_tdm_params_to_bclk(params, cpu_cfg.slot_width, cpu_cfg.slots, 1);
	if (bclk_freq <= 0)
		return -EINVAL;

	if (data->priv->mi2s_bclk_enable) {
		ret = snd_soc_dai_set_sysclk(cpu_dai, LPAIF_MI2S_BCLK, bclk_freq,
					     SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "%s: failed to set cpu sysclk: %d\n",
				__func__, ret);
			return ret;
		}
	}

	if (data->priv->codec_sysclk_set) {
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			ret = snd_soc_dai_set_sysclk(codec_dai, 0, bclk_freq,
						     SND_SOC_CLOCK_IN);
			if (ret && ret != -ENOTSUPP) {
				dev_err(rtd->dev, "%s: failed to set codec sysclk on %s: %d\n",
					__func__, codec_dai->name, ret);
				return ret;
			}
		}
	}

	return 0;
}

static void sc8280xp_init_be_config(struct snd_soc_pcm_runtime *rtd)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_be_config *cfg = &data->be_config[cpu_dai->id];

	cfg->rate = 48000;
	cfg->format = SNDRV_PCM_FORMAT_S16_LE;
	cfg->channels_max = 2;

	switch (cpu_dai->id) {
	case TX_CODEC_DMA_TX_0 ... TX_CODEC_DMA_TX_3:
		cfg->channels_min = 1;
		break;
	default:
		cfg->channels_min = 2;
		break;
	}
}

static int sc8280xp_be_config_info(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_info *uinfo)
{
	switch (SC8280XP_BE_CONTROL_KIND(kcontrol->private_value)) {
	case SC8280XP_BE_RATE:
		return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(sc8280xp_be_rate_text),
					 sc8280xp_be_rate_text);
	case SC8280XP_BE_CHANNELS:
		return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(sc8280xp_be_channels_text),
					 sc8280xp_be_channels_text);
	default:
		return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(sc8280xp_be_format_text),
					 sc8280xp_be_format_text);
	}
}

static struct sc8280xp_be_config *
sc8280xp_be_config_of(struct snd_kcontrol *kcontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(card);

	return &data->be_config[SC8280XP_BE_CONTROL_ID(kcontrol->private_value)];
}

static int sc8280xp_be_config_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct sc8280xp_be_config *cfg = sc8280xp_be_config_of(kcontrol);
	unsigned int *item = &ucontrol->value.enumerated.item[0];
	int i;

	switch (SC8280XP_BE_CONTROL_KIND(kcontrol->private_value)) {
	case SC8280XP_BE_RATE:
		for (i = 0; i < ARRAY_SIZE(sc8280xp_be_rates); i++) {
			if (sc8280xp_be_rates[i] == READ_ONCE(cfg->rate))
				*item = i;
		}

		break;
	case SC8280XP_BE_CHANNELS:
		*item = READ_ONCE(cfg->channels_max) - 1;
		break;
	default:
		for (i = 0; i < ARRAY_SIZE(sc8280xp_be_formats); i++) {
			if (sc8280xp_be_formats[i] == READ_ONCE(cfg->format))
				*item = i;
		}

		break;
	}

	return 0;
}

static int sc8280xp_be_config_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct sc8280xp_be_config *cfg = sc8280xp_be_config_of(kcontrol);
	unsigned int item = ucontrol->value.enumerated.item[0];

	switch (SC8280XP_BE_CONTROL_KIND(kcontrol->private_value)) {
	case SC8280XP_BE_RATE:
		if (item >= ARRAY_SIZE(sc8280xp_be_rates))
			return -EINVAL;

		if (READ_ONCE(cfg->rate) == sc8280xp_be_rates[item])
			return 0;

		WRITE_ONCE(cfg->rate, sc8280xp_be_rates[item]);
		break;
	case SC8280XP_BE_CHANNELS:
		if (item >= ARRAY_SIZE(sc8280xp_be_channels_text))
			return -EINVAL;

		if (READ_ONCE(cfg->channels_min) == item + 1 &&
		    READ_ONCE(cfg->channels_max) == item + 1)
			return 0;

		WRITE_ONCE(cfg->channels_min, item + 1);
		WRITE_ONCE(cfg->channels_max, item + 1);
		break;
	default:
		if (item >= ARRAY_SIZE(sc8280xp_be_formats))
			return -EINVAL;

		if (READ_ONCE(cfg->format) == sc8280xp_be_formats[item])
			return 0;

		WRITE_ONCE(cfg->format, sc8280xp_be_formats[item]);
		break;
	}

	return 1;
}

static int sc8280xp_add_be_config_control(struct snd_soc_pcm_runtime *rtd,
					  unsigned int kind, const char *suffix)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_kcontrol_new control = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.info = sc8280xp_be_config_info,
		.get = sc8280xp_be_config_get,
		.put = sc8280xp_be_config_put,
		.private_value = SC8280XP_BE_CONTROL(cpu_dai->id, kind),
	};

	control.name = devm_kasprintf(card->dev, GFP_KERNEL, "%s %s",
				      rtd->dai_link->name, suffix);
	if (!control.name)
		return -ENOMEM;

	return snd_soc_add_card_controls(card, &control, 1);
}

static int sc8280xp_add_slim_map_control(struct snd_soc_pcm_runtime *rtd);

static int sc8280xp_add_be_config(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret;

	ret = sc8280xp_add_be_config_control(rtd, SC8280XP_BE_RATE, "Rate");
	if (ret)
		return ret;

	ret = sc8280xp_add_be_config_control(rtd, SC8280XP_BE_FORMAT, "Format");
	if (ret)
		return ret;

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX ... SLIMBUS_6_TX:
		return sc8280xp_add_slim_map_control(rtd);
	default:
		return sc8280xp_add_be_config_control(rtd, SC8280XP_BE_CHANNELS,
						      "Channels");
	}
}

static int sc8280xp_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack  = NULL;
	int dp_pcm_id = 0;
	int ret;

	sc8280xp_init_be_config(rtd);

	ret = sc8280xp_add_be_config(rtd);
	if (ret)
		return ret;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 17);
		break;
	case DISPLAY_PORT_RX_0:
		/* DISPLAY_PORT dai ids are not contiguous */
		dp_pcm_id = 0;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		dp_pcm_id = cpu_dai->id - DISPLAY_PORT_RX_1 + 1;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	default:
		break;
	}

	if (dp_jack)
		return qcom_snd_dp_jack_setup(rtd, dp_jack, dp_pcm_id);

	if (data->priv->wcd_jack)
		return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);

	return 0;
}

static int sc8280xp_get_slim_channels(struct snd_soc_pcm_runtime *rtd,
				      unsigned int *ch, unsigned int *num)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int rx_ch[SLIM_MAX_PORTS], tx_ch[SLIM_MAX_PORTS];
	unsigned int rx_num = 0, tx_num = 0;
	struct snd_soc_dai *codec_dai;
	int i, ret;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_get_channel_map(codec_dai, &tx_num, tx_ch,
						  &rx_num, rx_ch);
		if (ret == -ENOTSUPP)
			continue;

		if (ret)
			return ret;

		if (cpu_dai->id & 0x1) {
			*num = tx_num;
			memcpy(ch, tx_ch, tx_num * sizeof(*ch));
		} else {
			*num = rx_num;
			memcpy(ch, rx_ch, rx_num * sizeof(*ch));
		}

		return 0;
	}

	return -ENOTSUPP;
}

static int sc8280xp_slim_map_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = SLIM_MAX_PORTS + 1;
	info->value.integer.min = 0;
	info->value.integer.max = 255;

	return 0;
}

static int sc8280xp_slim_map_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *value)
{
	struct snd_soc_pcm_runtime *rtd = (void *)kcontrol->private_value;
	unsigned int ch[SLIM_MAX_PORTS] = {}, num = 0;
	int i, ret;

	snd_soc_dpcm_mutex_lock(rtd->card);
	ret = sc8280xp_get_slim_channels(rtd, ch, &num);
	snd_soc_dpcm_mutex_unlock(rtd->card);

	if (ret)
		return ret;

	if (!num)
		return -ENODATA;

	value->value.integer.value[0] = num;

	for (i = 0; i < SLIM_MAX_PORTS; i++)
		value->value.integer.value[i + 1] = ch[i];

	return 0;
}

static int sc8280xp_add_slim_map_control(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_kcontrol_new control = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.info = sc8280xp_slim_map_info,
		.get = sc8280xp_slim_map_get,
		.private_value = (unsigned long)rtd,
	};

	control.name = devm_kasprintf(rtd->card->dev, GFP_KERNEL, "%s Channel Map",
				      rtd->dai_link->name);
	if (!control.name)
		return -ENOMEM;

	return snd_soc_add_card_controls(rtd->card, &control, 1);
}

static int sc8280xp_slim_hw_params(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int ch[SLIM_MAX_PORTS];
	unsigned int num;
	int ret;

	ret = sc8280xp_get_slim_channels(rtd, ch, &num);
	if (ret == -ENOTSUPP)
		return 0;

	if (ret)
		return ret;

	if (cpu_dai->id & 0x1)
		ret = snd_soc_dai_set_channel_map(cpu_dai, num, ch, 0, NULL);
	else
		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL, num, ch);

	return ret == -ENOTSUPP ? 0 : ret;
}

static int sc8280xp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	struct sc8280xp_be_config *cfg = &data->be_config[cpu_dai->id];

	rate->min = rate->max = READ_ONCE(cfg->rate);
	snd_mask_none(fmt);
	snd_mask_set_format(fmt, READ_ONCE(cfg->format));
	channels->min = READ_ONCE(cfg->channels_min);
	channels->max = READ_ONCE(cfg->channels_max);

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX ... SLIMBUS_6_TX: {
		unsigned int ch[SLIM_MAX_PORTS];
		unsigned int num;

		if (!sc8280xp_get_slim_channels(rtd, ch, &num) && num)
			channels->min = channels->max = num;

		break;
	}
	default:
		break;
	}


	return 0;
}

static int sc8280xp_snd_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	int mclk_freq = sc8280xp_get_mclk_freq(params);
	int bclk_freq = sc8280xp_get_bclk_freq(params);
	int ret;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX ... QUATERNARY_MI2S_TX:
	case QUINARY_MI2S_RX ... QUINARY_MI2S_TX:
	case SENARY_MI2S_RX ... SENARY_MI2S_TX:
	case LPI_MI2S_RX_0 ... LPI_MI2S_TX_4:
		ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
		if (ret && ret != -ENOTSUPP)
			return ret;

		if (data->priv->codec_dai_fmt) {
			ret = snd_soc_dai_set_fmt(codec_dai,
						  data->priv->codec_dai_fmt);
			if (ret && ret != -ENOTSUPP)
				return ret;
		}

		if (data->priv->mi2s_mclk_enable) {
			ret = snd_soc_dai_set_sysclk(cpu_dai,
						     LPAIF_MI2S_MCLK, mclk_freq,
						     SND_SOC_CLOCK_OUT);
			if (ret)
				return ret;
		}

		if (data->priv->mi2s_bclk_enable) {
			ret = snd_soc_dai_set_sysclk(cpu_dai,
						     LPAIF_MI2S_BCLK, bclk_freq,
						     SND_SOC_CLOCK_OUT);
			if (ret)
				return ret;
		}

		if (data->priv->codec_sysclk_set) {
			ret = snd_soc_dai_set_sysclk(codec_dai,
						     0, mclk_freq,
						     SND_SOC_CLOCK_IN);
			if (ret && ret != -ENOTSUPP)
				return ret;
		}
		break;
	case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7:
		return sc8280xp_tdm_hw_params(substream, params);
	case SLIMBUS_0_RX ... SLIMBUS_6_TX:
		return sc8280xp_slim_hw_params(rtd);
	default:
		break;
	}

	return 0;
}

/*
 * WSA and WSA2 are handled as a single interface with the
 * following channels mask:
 *  __________________________________________________
 *  | Bits  |     3    |     2    |   1     |     0   |
 *  ---------------------------------------------------
 *  | Line  | WSA2 Ch2 | WSA2 Ch1 | WSA Ch2 | WSA Ch1 |
 *  ---------------------------------------------------
 *
 * The Ayaneo Pocket S2 speakers are connected only to
 * the WSA2 interface and the WSA interface is not enabled.
 *
 * Set the channel mapping on the WSA2 channels only.
 */
static const unsigned int ayaneo_ps2_channels_mapping[] = {
	0,			/* WSA Ch1 */
	0,			/* WSA Ch2 */
	PCM_CHANNEL_FL,		/* WSA2 Ch1 */
	PCM_CHANNEL_FR		/* WSA2 Ch2 */
};

static int ayaneo_ps2_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int channels = substream->runtime->channels;

	if (cpu_dai->id != WSA_CODEC_DMA_RX_0)
		return 0;

	if (channels != 2)
		return -EINVAL;

	return snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
					   ARRAY_SIZE(ayaneo_ps2_channels_mapping),
					   ayaneo_ps2_channels_mapping);
}

static int sc8280xp_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	if (data->priv->snd_prepare) {
		int ret;

		ret = data->priv->snd_prepare(substream);
		if (ret)
			return ret;
	}

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sc8280xp_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sc8280xp_be_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_params = sc8280xp_snd_hw_params,
	.hw_free = sc8280xp_snd_hw_free,
	.prepare = sc8280xp_snd_prepare,
};

static void sc8280xp_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->id == DUMMY_RX) {
			link->dynamic = 0;
			link->no_pcm = 1;
			link->ignore_pmdown_time = 1;
		}

		if (link->no_pcm == 1) {
			link->init = sc8280xp_snd_init;
			link->be_hw_params_fixup = sc8280xp_be_hw_params_fixup;
			link->ops = &sc8280xp_be_ops;
		}
	}
}

static int sc8280xp_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sc8280xp_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->priv = of_device_get_match_data(dev);
	if (!data->priv)
		return -ENODEV;

	card->owner = THIS_MODULE;
	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	card->dapm_widgets = data->priv->dapm_widgets;
	card->num_dapm_widgets = data->priv->num_dapm_widgets;
	card->dapm_routes = data->priv->dapm_routes;
	card->num_dapm_routes = data->priv->num_dapm_routes;
	card->controls = data->priv->controls;
	card->num_controls = data->priv->num_controls;

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = data->priv->driver_name;
	sc8280xp_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static struct qcom_snd_soc_common ayaneo_ps2_priv_data = {
	.driver_name = "ayaneo-ps2",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.snd_prepare = ayaneo_ps2_snd_prepare,
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common eliza_priv_data = {
	.driver_name = "eliza",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common hawi_priv_data = {
	.driver_name = "hawi",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.codec_sysclk_set = true,
	.mi2s_bclk_enable = true,
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common kaanapali_priv_data = {
	.driver_name = "kaanapali",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common qcs9100_priv_data = {
	.driver_name = "sa8775p",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
};

static const struct qcom_snd_soc_common qcs615_priv_data = {
	.driver_name = "qcs615",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.codec_sysclk_set = true,
};

static const struct qcom_snd_soc_common qcm6490_priv_data = {
	.driver_name = "qcm6490",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common qcs6490_priv_data = {
	.driver_name = "qcs6490",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common qcs8275_priv_data = {
	.driver_name = "qcs8300",
	.dapm_widgets = max98090_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(max98090_dapm_widgets),
	.controls = max98090_controls,
	.num_controls = ARRAY_SIZE(max98090_controls),
	.codec_sysclk_set = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_BC_FC,
};

static const struct qcom_snd_soc_common sc8280xp_priv_data = {
	.driver_name = "sc8280xp",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common sm8450_priv_data = {
	.driver_name = "sm8450",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
	/* I2S Connected to HDMI */
	.mi2s_mclk_enable = true,
	.mi2s_bclk_enable = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_BC_FC |
			 SND_SOC_DAIFMT_NB_NF |
			 SND_SOC_DAIFMT_I2S,
};

static const struct qcom_snd_soc_common sm8475_priv_data = {
	.driver_name = "sm8475",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct qcom_snd_soc_common sm8550_priv_data = {
	.driver_name = "sm8550",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
	/* I2S Connected to HDMI */
	.mi2s_mclk_enable = true,
	.mi2s_bclk_enable = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_BC_FC |
			 SND_SOC_DAIFMT_NB_NF |
			 SND_SOC_DAIFMT_I2S,
};

static const struct qcom_snd_soc_common sm8650_priv_data = {
	.driver_name = "sm8650",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
	/* I2S Connected to HDMI */
	.mi2s_mclk_enable = true,
	.mi2s_bclk_enable = true,
	.codec_dai_fmt = SND_SOC_DAIFMT_BC_FC |
			 SND_SOC_DAIFMT_NB_NF |
			 SND_SOC_DAIFMT_I2S,
};

static const struct qcom_snd_soc_common sm8750_priv_data = {
	.driver_name = "sm8750",
	.dapm_widgets = sc8280xp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc8280xp_dapm_widgets),
	.wcd_jack = true,
};

static const struct of_device_id snd_sc8280xp_dt_match[] = {
	{ .compatible = "ayaneo,pocket-s2-sndcard", .data = &ayaneo_ps2_priv_data },
	{ .compatible = "qcom,eliza-sndcard", .data = &eliza_priv_data },
	{ .compatible = "qcom,hawi-sndcard", .data = &hawi_priv_data },
	{ .compatible = "qcom,kaanapali-sndcard", .data = &kaanapali_priv_data },
	{ .compatible = "qcom,maili-sndcard", .data = &hawi_priv_data },
	{ .compatible = "qcom,qcm6490-idp-sndcard", .data = &qcm6490_priv_data },
	{ .compatible = "qcom,qcs615-sndcard", .data = &qcs615_priv_data },
	{ .compatible = "qcom,qcs6490-rb3gen2-sndcard", .data = &qcs6490_priv_data },
	{ .compatible = "qcom,qcs8275-sndcard", .data = &qcs8275_priv_data },
	{ .compatible = "qcom,qcs9075-sndcard", .data = &qcs9100_priv_data },
	{ .compatible = "qcom,qcs9100-sndcard", .data = &qcs9100_priv_data },
	{ .compatible = "qcom,sc8280xp-sndcard", .data = &sc8280xp_priv_data },
	{ .compatible = "qcom,sm8450-sndcard", .data = &sm8450_priv_data },
	{ .compatible = "qcom,sm8475-sndcard", .data = &sm8475_priv_data },
	{ .compatible = "qcom,sm8550-sndcard", .data = &sm8550_priv_data },
	{ .compatible = "qcom,sm8650-sndcard", .data = &sm8650_priv_data },
	{ .compatible = "qcom,sm8750-sndcard", .data = &sm8750_priv_data },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sc8280xp_dt_match);

static struct platform_driver snd_sc8280xp_driver = {
	.probe  = sc8280xp_platform_probe,
	.driver = {
		.name = "snd-sc8280xp",
		.of_match_table = snd_sc8280xp_dt_match,
	},
};
module_platform_driver(snd_sc8280xp_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SC8280XP ASoC Machine Driver");
MODULE_LICENSE("GPL");
