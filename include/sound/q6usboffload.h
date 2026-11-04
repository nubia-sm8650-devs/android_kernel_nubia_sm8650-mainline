/* SPDX-License-Identifier: GPL-2.0
 *
 * sound/q6usboffload.h -- QDSP6 USB offload
 *
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/types.h>

/**
 * struct q6usb_offload - USB backend DAI link offload parameters
 * @dev: dev handle to usb be
 * @domain: allocated iommu domain
 * @intr_num: usb interrupter number
 * @sid: streamID for iommu
 **/
struct q6usb_offload {
	struct device *dev;
	struct iommu_domain *domain;
	u16 intr_num;
	u8 sid;
};

struct snd_soc_pcm_runtime;

#if IS_REACHABLE(CONFIG_SND_SOC_QDSP6_USB)
int q6usb_get_offload_token(struct snd_soc_pcm_runtime *rtd, int direction,
			    u32 *token);
#else
static inline int q6usb_get_offload_token(struct snd_soc_pcm_runtime *rtd,
					  int direction, u32 *token)
{
	return -ENODEV;
}
#endif
