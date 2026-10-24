// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Goodix Berlin Touchscreen Driver
 *
 * Copyright (C) 2020 - 2021 Goodix, Inc.
 * Copyright (C) 2023 Linaro Ltd.
 *
 * Based on goodix_ts_berlin driver.
 */
#include <linux/unaligned.h>
#include <linux/cache.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/input.h>

#include "goodix_berlin.h"

#define GOODIX_BERLIN_SPI_TRANS_PREFIX_LEN	1
#define GOODIX_BERLIN_REGISTER_WIDTH		4
#define GOODIX_BERLIN_SPI_READ_DUMMY_LEN_A	4
#define GOODIX_BERLIN_SPI_READ_DUMMY_LEN_D	3
#define GOODIX_BERLIN_SPI_READ_PREFIX_LEN_A	(GOODIX_BERLIN_SPI_TRANS_PREFIX_LEN + \
						 GOODIX_BERLIN_REGISTER_WIDTH + \
						 GOODIX_BERLIN_SPI_READ_DUMMY_LEN_A)
#define GOODIX_BERLIN_SPI_READ_PREFIX_LEN_D	(GOODIX_BERLIN_SPI_TRANS_PREFIX_LEN + \
						 GOODIX_BERLIN_REGISTER_WIDTH + \
						 GOODIX_BERLIN_SPI_READ_DUMMY_LEN_D)
#define GOODIX_BERLIN_SPI_WRITE_PREFIX_LEN	(GOODIX_BERLIN_SPI_TRANS_PREFIX_LEN + \
						 GOODIX_BERLIN_REGISTER_WIDTH)

#define GOODIX_BERLIN_SPI_WRITE_FLAG		0xF0
#define GOODIX_BERLIN_SPI_READ_FLAG		0xF1

#define GOODIX_BERLIN_SPI_BUF_LEN		SZ_2K

/*
 * The regmap lock serializes all transfers, so one pair of buffers serves the
 * device. Each sits on its own DMA cache lines.
 */
struct goodix_berlin_spi {
	struct spi_device *spi;
	const struct goodix_berlin_ic_data *ic_data;
	u8 tx_buf[GOODIX_BERLIN_SPI_BUF_LEN] __aligned(ARCH_DMA_MINALIGN);
	u8 rx_buf[GOODIX_BERLIN_SPI_BUF_LEN] __aligned(ARCH_DMA_MINALIGN);
};

static int goodix_berlin_spi_read(void *context, const void *reg_buf,
				  size_t reg_size, void *val_buf,
				  size_t val_size)
{
	struct goodix_berlin_spi *bs = context;
	unsigned int prefix_len = bs->ic_data->read_prefix_len;
	const u32 *reg = reg_buf; /* reg is stored as native u32 at start of buffer */
	struct spi_transfer xfer = {
		.tx_buf = bs->tx_buf,
		.rx_buf = bs->rx_buf,
		.len = prefix_len + val_size,
	};
	int error;

	if (reg_size != GOODIX_BERLIN_REGISTER_WIDTH ||
	    xfer.len > GOODIX_BERLIN_SPI_BUF_LEN)
		return -EINVAL;

	/* buffer format: 0xF1 + addr(4bytes) + dummy(3/4bytes) + data */
	bs->tx_buf[0] = GOODIX_BERLIN_SPI_READ_FLAG;
	put_unaligned_be32(*reg, bs->tx_buf + GOODIX_BERLIN_SPI_TRANS_PREFIX_LEN);
	memset(bs->tx_buf + GOODIX_BERLIN_SPI_TRANS_PREFIX_LEN + GOODIX_BERLIN_REGISTER_WIDTH,
	       0xff, bs->ic_data->read_dummy_len);
	memset(bs->tx_buf + prefix_len, 0, val_size);

	error = spi_sync_transfer(bs->spi, &xfer, 1);
	if (error < 0) {
		dev_err(&bs->spi->dev, "spi transfer error, %d", error);
		return error;
	}

	memcpy(val_buf, bs->rx_buf + prefix_len, val_size);
	return 0;
}

static int goodix_berlin_spi_write(void *context, const void *data,
				   size_t count)
{
	unsigned int len = count - GOODIX_BERLIN_REGISTER_WIDTH;
	struct goodix_berlin_spi *bs = context;
	const u32 *reg = data; /* reg is stored as native u32 at start of buffer */
	struct spi_transfer xfer = {
		.tx_buf = bs->tx_buf,
		.len = GOODIX_BERLIN_SPI_WRITE_PREFIX_LEN + len,
	};
	int error;

	if (xfer.len > GOODIX_BERLIN_SPI_BUF_LEN)
		return -EINVAL;

	bs->tx_buf[0] = GOODIX_BERLIN_SPI_WRITE_FLAG;
	put_unaligned_be32(*reg, bs->tx_buf + GOODIX_BERLIN_SPI_TRANS_PREFIX_LEN);
	memcpy(bs->tx_buf + GOODIX_BERLIN_SPI_WRITE_PREFIX_LEN,
	       data + GOODIX_BERLIN_REGISTER_WIDTH, len);

	error = spi_sync_transfer(bs->spi, &xfer, 1);
	if (error < 0) {
		dev_err(&bs->spi->dev, "spi transfer error, %d", error);
		return error;
	}

	return 0;
}

static const struct regmap_config goodix_berlin_spi_regmap_conf = {
	.reg_bits = 32,
	.val_bits = 8,
	.read = goodix_berlin_spi_read,
	.write = goodix_berlin_spi_write,
};

/* vendor & product left unassigned here, should probably be updated from fw info */
static const struct input_id goodix_berlin_spi_input_id = {
	.bustype = BUS_SPI,
};

static int goodix_berlin_spi_probe(struct spi_device *spi)
{
	const struct goodix_berlin_ic_data *ic_data = spi_get_device_match_data(spi);
	struct regmap_config regmap_config;
	struct goodix_berlin_spi *bs;
	struct regmap *regmap;
	size_t max_size;
	int error = 0;

	bs = devm_kzalloc(&spi->dev, sizeof(*bs), GFP_KERNEL);
	if (!bs)
		return -ENOMEM;

	bs->spi = spi;
	bs->ic_data = ic_data;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	error = spi_setup(spi);
	if (error)
		return error;

	max_size = min(spi_max_transfer_size(spi), GOODIX_BERLIN_SPI_BUF_LEN);

	regmap_config = goodix_berlin_spi_regmap_conf;
	regmap_config.max_raw_read = max_size - ic_data->read_prefix_len;
	regmap_config.max_raw_write = max_size - GOODIX_BERLIN_SPI_WRITE_PREFIX_LEN;

	regmap = devm_regmap_init(&spi->dev, NULL, bs, &regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	error = goodix_berlin_probe(&spi->dev, spi->irq,
				    &goodix_berlin_spi_input_id, regmap,
				    ic_data);
	if (error)
		return error;

	return 0;
}

static const struct goodix_berlin_ic_data gt9897_data = {
	.fw_version_info_addr = GOODIX_BERLIN_FW_VERSION_INFO_ADDR_A,
	.ic_info_addr = GOODIX_BERLIN_IC_INFO_ADDR_A,
	.read_dummy_len = GOODIX_BERLIN_SPI_READ_DUMMY_LEN_A,
	.read_prefix_len = GOODIX_BERLIN_SPI_READ_PREFIX_LEN_A,
};

static const struct goodix_berlin_ic_data gt9916_data = {
	.fw_version_info_addr = GOODIX_BERLIN_FW_VERSION_INFO_ADDR_D,
	.ic_info_addr = GOODIX_BERLIN_IC_INFO_ADDR_D,
	.read_dummy_len = GOODIX_BERLIN_SPI_READ_DUMMY_LEN_D,
	.read_prefix_len = GOODIX_BERLIN_SPI_READ_PREFIX_LEN_D,
};

static const struct spi_device_id goodix_berlin_spi_ids[] = {
	{ .name = "gt9897", .driver_data = (long)&gt9897_data },
	{ .name = "gt9916", .driver_data = (long)&gt9916_data },
	{ },
};
MODULE_DEVICE_TABLE(spi, goodix_berlin_spi_ids);

static const struct of_device_id goodix_berlin_spi_of_match[] = {
	{ .compatible = "goodix,gt9897", .data = &gt9897_data },
	{ .compatible = "goodix,gt9916", .data = &gt9916_data },
	{ }
};
MODULE_DEVICE_TABLE(of, goodix_berlin_spi_of_match);

static struct spi_driver goodix_berlin_spi_driver = {
	.driver = {
		.name = "goodix-berlin-spi",
		.of_match_table = goodix_berlin_spi_of_match,
		.pm = pm_sleep_ptr(&goodix_berlin_pm_ops),
		.dev_groups = goodix_berlin_groups,
	},
	.probe = goodix_berlin_spi_probe,
	.id_table = goodix_berlin_spi_ids,
};
module_spi_driver(goodix_berlin_spi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Goodix Berlin SPI Touchscreen driver");
MODULE_AUTHOR("Neil Armstrong <neil.armstrong@linaro.org>");
