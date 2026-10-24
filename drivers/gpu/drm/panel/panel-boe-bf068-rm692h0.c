// SPDX-License-Identifier: GPL-2.0-only
/*
 * BOE BF068 6.8" AMOLED panel on a Raydium RM692H0 controller, as found in
 * the ZTE nubia Z60 Ultra.
 *
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
 *   Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define RM692H0_MAX_BRIGHTNESS	3515

struct rm692h0 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_connector *connector;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
};

static const struct regulator_bulk_data rm692h0_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vddd" },
	{ .supply = "vdd", .init_load_uA = 299000 },
	{ .supply = "vci", .init_load_uA = 20000 },
};

static const unsigned int rm692h0_post_on_ms[] = { 1, 5, 5, 5 };
static const unsigned int rm692h0_pre_off_ms[] = { 0, 0, 0, 5 };
static const unsigned int rm692h0_post_off_ms[] = { 0, 0, 0, 5 };

#define RM692H0_CLOCK	((1116 + 26 + 8 + 76) * (2480 + 56 + 8 + 8) * 120 / 1000)

#define RM692H0_MODE(vfp) {						\
	.clock = RM692H0_CLOCK,						\
	.hdisplay = 1116,						\
	.hsync_start = 1116 + 26,					\
	.hsync_end = 1116 + 26 + 8,					\
	.htotal = 1116 + 26 + 8 + 76,					\
	.vdisplay = 2480,						\
	.vsync_start = 2480 + (vfp),					\
	.vsync_end = 2480 + (vfp) + 8,					\
	.vtotal = 2480 + (vfp) + 8 + 8,					\
	.width_mm = 71,							\
	.height_mm = 157,						\
	.type = DRM_MODE_TYPE_DRIVER,					\
}

/*
 * Both modes share the pixel clock and horizontal timings so that the host
 * keeps its link configuration and the panel switches in place.
 */
static const struct drm_display_mode rm692h0_modes[] = {
	RM692H0_MODE(56 + 2552),
	RM692H0_MODE(56),
};

static inline struct rm692h0 *to_rm692h0(struct drm_panel *panel)
{
	return container_of(panel, struct rm692h0, panel);
}

static int rm692h0_power_on(struct rm692h0 *ctx)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(rm692h0_supplies); i++) {
		ret = regulator_enable(ctx->supplies[i].consumer);
		if (ret) {
			while (--i >= 0)
				regulator_disable(ctx->supplies[i].consumer);
			return ret;
		}

		fsleep(rm692h0_post_on_ms[i] * USEC_PER_MSEC);
	}

	return 0;
}

static void rm692h0_power_off(struct rm692h0 *ctx)
{
	int i;

	for (i = ARRAY_SIZE(rm692h0_supplies) - 1; i >= 0; i--) {
		fsleep(rm692h0_pre_off_ms[i] * USEC_PER_MSEC);
		regulator_disable(ctx->supplies[i].consumer);
		fsleep(rm692h0_post_off_ms[i] * USEC_PER_MSEC);
	}
}

static void rm692h0_reset(struct rm692h0 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(40);
}

static bool rm692h0_mode_is_120hz(const struct drm_display_mode *mode)
{
	return drm_mode_vrefresh(mode) == 120;
}

static bool rm692h0_is_120hz(struct rm692h0 *ctx)
{
	struct drm_connector *connector = ctx->connector;

	if (!connector || !connector->state || !connector->state->crtc)
		return false;

	return rm692h0_mode_is_120hz(&connector->state->crtc->state->mode);
}

static void rm692h0_60hz_timing(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x49, 0x5e, 0xff, 0xf0, 0xff, 0xff,
				     0xff, 0xf0, 0xff, 0xff, 0x00, 0xfd, 0xfd);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x58, 0x16, 0xfa);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5e, 0x0f, 0x0f, 0x0f, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x66, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6e, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7e, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x92, 0x4e, 0x22, 0x1f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x99, 0x2d, 0x24, 0x0c, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9e, 0x20, 0x20, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa2, 0x10, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa7, 0x10);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa9, 0x10, 0x0d);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xac, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x08, 0x08, 0x05, 0x05, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb8, 0x09, 0x09, 0x05, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc0, 0x08, 0x08, 0x03, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc8, 0x06, 0x06, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xea, 0x16);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x5b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2b, 0x4b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2d, 0x24);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2f, 0x1f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x30, 0x1f, 0x3f, 0x3f, 0x38, 0x32,
				     0x24, 0x0c, 0x00, 0x00, 0x25, 0x25, 0x1d);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3d, 0x11);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3f, 0x07);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x40, 0x04, 0x00, 0x00, 0x1a, 0x1a,
				     0x0e, 0x08, 0x03, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4b, 0x08, 0x08, 0x08, 0x08);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x50, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x53, 0x09, 0x09, 0x05, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5c, 0x08, 0x08, 0x04, 0x01, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x64, 0x07, 0x07, 0x03, 0x01);
}

static void rm692h0_120hz_timing(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x49, 0x0f, 0x08, 0x04, 0x00,
				     0x06, 0x06, 0x06, 0x06, 0x06, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x55, 0x10, 0x10);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x58, 0x05, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5e, 0x0f, 0x0f, 0x0f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x66, 0x10, 0x10, 0x10);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6e, 0x05, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x76, 0x05, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7e, 0x05, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x92, 0x72, 0x45, 0x25);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x99, 0x7f, 0x40, 0x0f, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9e, 0x30, 0x10, 0x3d);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa2, 0x1d, 0x09);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa7, 0x27);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa9, 0x27, 0x1a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xac, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x20, 0x10, 0x0a, 0x0d, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb8, 0x12, 0x12, 0x0d, 0x03, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc0, 0x0e, 0x0e, 0x09, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc8, 0x0b, 0x0b, 0x07);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xea, 0x1f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x5b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2b, 0x7a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2d, 0x45);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2f, 0x32);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x31, 0x60, 0x1f, 0x60, 0x60,
				     0x60, 0x37, 0x18, 0x0f, 0x0a, 0x30,
				     0x30, 0x2b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3d, 0x1f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3f, 0x0f, 0x07, 0x02, 0x01,
				     0x1b, 0x1b, 0x1b, 0x0e, 0x06, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4b, 0x13, 0x13, 0x0d, 0x09);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x50, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x53, 0x12, 0x12, 0x0b, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x58, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5c, 0x0b, 0x0d, 0x08, 0x04, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5c, 0x0b, 0x00, 0x08, 0x04, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x64, 0x09, 0x09, 0x06, 0x02);
}

static void rm692h0_on(struct mipi_dsi_multi_context *dsi_ctx, bool hz120)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x98);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x20, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x42);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x68, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0a, 0xc9);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0b, 0x17);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0c, 0x35);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbe, 0xc9);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbf, 0x11);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc0, 0xc9);
	if (hz120)
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x05);
	else
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0xa1);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb3, 0x7f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfa, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x57);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc2, 0x48);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x00, 0x80);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x1a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0d, 0x64);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xdc, 0xb8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x42);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xda, 0x82);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x81, 0x80);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x34);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x47, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x48, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9f, 0x5c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa0, 0xc4);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x1c, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x1d, 0x10);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x49, 0x55);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x34);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x1e, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x1f, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x20, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x22, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x21, 0x70);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x23, 0xf8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x24, 0x77);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x25, 0xf8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x26, 0xf8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2f, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x30, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x31, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x32, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x33, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x34, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x36, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x37, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x38, 0x7f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x39, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3a, 0xb6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3b, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3d, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3f, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x40, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x41, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x42, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0xd4);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x40, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0xfd);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x80, 0x06);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x83, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0xa0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x06, 0x36);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6e, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7c, 0x15);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0xd0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x11, 0x75);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x92, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0xa1);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc3, 0x87);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc4, 0xff);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc5, 0x7f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfa, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0xd2);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4f, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x50, 0x11);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x51, 0xab);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x52, 0x30);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x53, 0x09);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x54, 0xb0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x55, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x56, 0x5c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x59, 0x14);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5a, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5b, 0x2e);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5c, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5d, 0x9a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5e, 0x19);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5f, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x60, 0xbe);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x61, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x62, 0x0a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x63, 0x0c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x64, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x65, 0x0e);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x66, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x67, 0xfd);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x68, 0x16);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x69, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6a, 0x10);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6b, 0xdc);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6c, 0x07);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6d, 0x10);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6e, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6f, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x70, 0x06);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x71, 0x0f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x72, 0x0f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x73, 0x33);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x74, 0x0e);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x75, 0x1c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x76, 0x2a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x77, 0x38);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x78, 0x46);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x79, 0x54);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7a, 0x62);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7b, 0x69);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7c, 0x70);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7d, 0x77);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7e, 0x79);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x7f, 0x7b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x80, 0x7d);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x81, 0x7e);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x82, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x83, 0xc2);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x84, 0x22);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x85, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x86, 0x2a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x87, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x88, 0x32);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x89, 0xbe);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x8a, 0x3a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x8b, 0xfc);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x8c, 0x3a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x8d, 0xfa);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x8e, 0x3a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x8f, 0xf8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x90, 0x3b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x91, 0x38);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x92, 0x3b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x93, 0x78);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x94, 0x3b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x95, 0x76);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x96, 0x4b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x97, 0xb6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x98, 0x4b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x99, 0xf6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9a, 0x4c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9b, 0x34);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9c, 0x5c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9d, 0x74);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9e, 0x8c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9f, 0xf4);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa2, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa3, 0xba);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa4, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa5, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa6, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa7, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa9, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xaa, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa0, 0xa0);
	if (hz120)
		rm692h0_120hz_timing(dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x24, 0x54);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa9, 0x14);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xad, 0xc8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa5, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xac, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xae, 0x70);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xaf, 0x4b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x4b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0x4b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb3, 0x4f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x26);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x1d, 0x26);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xa4, 0x1f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3a, 0xe8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3b, 0xe8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x42);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x81, 0xe8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x82, 0xe8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc2, 0x08);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_SET_ADDRESS_MODE, 0x00);
	mipi_dsi_dcs_set_tear_on_multi(dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x60);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xae, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb3, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb8, 0xff);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xba, 0xff);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc7, 0xcb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc8, 0xcb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xdb, 0xcb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xdc, 0xcb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 100);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 20);
}

static void rm692h0_off(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x76);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x00, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x09, 0x28);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0a, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0b, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0c, 0x49);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0d, 0x76);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x0e, 0x17);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x49, 0xff);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4a, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4b, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4c, 0xfe);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4d, 0x60);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4e, 0xae);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x4f, 0x28);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x50, 0xb3);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x51, 0x28);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x52, 0xb8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x53, 0xf9);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x54, 0xba);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x55, 0x9f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x56, 0xc7);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x58, 0xcf);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x59, 0xc8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5a, 0xcf);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5b, 0xdb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5c, 0xcf);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5d, 0xdc);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5e, 0xcf);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x5f, 0xfe);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x60, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfe, 0x00);
	mipi_dsi_msleep(dsi_ctx, 20);
	mipi_dsi_dcs_set_display_off_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 147);
	mipi_dsi_dcs_enter_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 20);
}

static int rm692h0_prepare(struct drm_panel *panel)
{
	struct rm692h0 *ctx = to_rm692h0(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = rm692h0_power_on(ctx);
	if (ret)
		return ret;

	rm692h0_reset(ctx);

	rm692h0_on(&dsi_ctx, rm692h0_is_120hz(ctx));

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);

	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		rm692h0_power_off(ctx);
	}

	return dsi_ctx.accum_err;
}

static int rm692h0_unprepare(struct drm_panel *panel)
{
	struct rm692h0 *ctx = to_rm692h0(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	rm692h0_off(&dsi_ctx);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	rm692h0_power_off(ctx);

	return 0;
}

static int rm692h0_switch_mode(struct drm_panel *panel,
			       const struct drm_display_mode *mode)
{
	struct rm692h0 *ctx = to_rm692h0(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	bool hz120 = rm692h0_mode_is_120hz(mode);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x40);
	if (hz120) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x05);
		rm692h0_120hz_timing(&dsi_ctx);
	} else {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
		rm692h0_60hz_timing(&dsi_ctx);
	}
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x00);

	return dsi_ctx.accum_err;
}

static int rm692h0_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct rm692h0 *ctx = to_rm692h0(panel);
	struct drm_display_mode *mode;
	int i;

	ctx->connector = connector;

	for (i = 0; i < ARRAY_SIZE(rm692h0_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &rm692h0_modes[i]);
		if (!mode)
			return -ENOMEM;

		drm_mode_set_name(mode);
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = rm692h0_modes[0].width_mm;
	connector->display_info.height_mm = rm692h0_modes[0].height_mm;

	return ARRAY_SIZE(rm692h0_modes);
}

static const struct drm_panel_funcs rm692h0_panel_funcs = {
	.prepare = rm692h0_prepare,
	.unprepare = rm692h0_unprepare,
	.get_modes = rm692h0_get_modes,
	.switch_mode = rm692h0_switch_mode,
};

static int rm692h0_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);

	return mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
}

static const struct backlight_ops rm692h0_bl_ops = {
	.update_status = rm692h0_bl_update_status,
};

static struct backlight_device *
rm692h0_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = RM692H0_MAX_BRIGHTNESS / 2,
		.max_brightness = RM692H0_MAX_BRIGHTNESS,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &rm692h0_bl_ops, &props);
}

static int rm692h0_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rm692h0 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct rm692h0, panel,
				   &rm692h0_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(rm692h0_supplies),
					    rm692h0_supplies, &ctx->supplies);
	if (ret)
		return ret;

	/* Leave the panel the bootloader lit out of reset until prepare(). */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = rm692h0_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	ret = devm_drm_panel_add(dev, &ctx->panel);
	if (ret)
		return ret;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 20;
	ctx->dsc.slice_width = 558;
	ctx->dsc.slice_count = rm692h0_modes[0].hdisplay / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 10 << 4;
	ctx->dsc.block_pred_enable = true;
	dsi->dsc = &ctx->dsc;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");

	return 0;
}

static const struct of_device_id rm692h0_of_match[] = {
	{ .compatible = "boe,bf068-rm692h0" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rm692h0_of_match);

static struct mipi_dsi_driver rm692h0_driver = {
	.probe = rm692h0_probe,
	.driver = {
		.name = "panel-boe-bf068-rm692h0",
		.of_match_table = rm692h0_of_match,
	},
};
module_mipi_dsi_driver(rm692h0_driver);

MODULE_AUTHOR("Cosmin Tanislav <demonsingur@gmail.com>");
MODULE_DESCRIPTION("DRM driver for the BOE BF068 RM692H0 DSC command mode DSI panel");
MODULE_LICENSE("GPL");
