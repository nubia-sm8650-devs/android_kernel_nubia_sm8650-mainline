/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-csid-gen3.h
 *
 * Qualcomm MSM Camera Subsystem - CSID (CSI Decoder) Module Generation 3
 *
 * Copyright (c) 2024 Qualcomm Technologies, Inc.
 */
#ifndef __QC_MSM_CAMSS_CSID_GEN3_H__
#define __QC_MSM_CAMSS_CSID_GEN3_H__

#include "camss-hw.h"

#define DECODE_FORMAT_UNCOMPRESSED_8_BIT	0x1
#define DECODE_FORMAT_UNCOMPRESSED_10_BIT	0x2
#define DECODE_FORMAT_UNCOMPRESSED_12_BIT	0x3
#define DECODE_FORMAT_UNCOMPRESSED_14_BIT	0x4
#define DECODE_FORMAT_UNCOMPRESSED_16_BIT	0x5
#define DECODE_FORMAT_UNCOMPRESSED_20_BIT	0x6
#define DECODE_FORMAT_UNCOMPRESSED_24_BIT	0x7
#define DECODE_FORMAT_PAYLOAD_ONLY		0xf

#define PLAIN_FORMAT_PLAIN8	0x0 /* supports DPCM, UNCOMPRESSED_6/8_BIT */
#define PLAIN_FORMAT_PLAIN16	0x1 /* supports DPCM, UNCOMPRESSED_10/16_BIT */
#define PLAIN_FORMAT_PLAIN32	0x2 /* supports UNCOMPRESSED_20_BIT */

/* port_id passed to camss_reg_update() for the IPP, outside the RDI range */
#define CSID_780_CUSTOM_PORT_ID_IPP0	0x100

struct camss_hw_pipe;
struct csid_device;

unsigned int csid_gen3_hw_path_num(struct csid_device *csid);
void csid_gen3_hw_init(struct camss_hw_pipe *pipe);
void csid_gen3_hw_dual(struct camss_hw_pipe *pipe, enum camss_hw_sync sync,
		       unsigned int peer);
void csid_gen3_hw_rx(struct camss_hw_pipe *pipe,
		     const struct camss_hw_rx_cfg *cfg);
void csid_gen3_hw_path_config(struct camss_hw_pipe *pipe,
			      enum camss_hw_path path,
			      const struct camss_hw_path_cfg *cfg);
void csid_gen3_hw_path_resume(struct camss_hw_pipe *pipe,
			      enum camss_hw_path path);
void csid_gen3_hw_path_resume_global(struct camss_hw_pipe *pipe,
				     enum camss_hw_path path);
void csid_gen3_hw_global_start(struct camss_hw_pipe *pipe);
int csid_gen3_hw_stop(struct camss_hw_pipe *pipe);
struct camss_hw_reg csid_gen3_hw_rup_reg(unsigned long paths, bool mup);
u64 csid_gen3_hw_path_sof_ts(struct camss_hw_pipe *pipe,
			     enum camss_hw_path path, bool prev);
void csid_gen3_hw_buf_done(struct camss_hw_pipe *pipe, unsigned int comp,
			   unsigned int wm, bool enable);

#endif /* __QC_MSM_CAMSS_CSID_GEN3_H__ */
