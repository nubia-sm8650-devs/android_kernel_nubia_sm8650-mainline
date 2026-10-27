/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __DT_BINDINGS_INTERCONNECT_QCOM_CAMNOC_H
#define __DT_BINDINGS_INTERCONNECT_QCOM_CAMNOC_H

/*
 * CAMNOC master port IDs for SM8250 (Kona).
 * Each ID identifies a camera sub-device that can vote for CAMNOC AXI
 * bandwidth via icc_set_bw().  Only ports wired on this SoC are listed.
 */

/* Image Front End */
#define MASTER_CAMNOC_IFE_0		0
#define MASTER_CAMNOC_IFE_0_RDI		1
#define MASTER_CAMNOC_IFE_1		2
#define MASTER_CAMNOC_IFE_1_RDI		3
#define MASTER_CAMNOC_IFE_LITE_0	4
#define MASTER_CAMNOC_IFE_LITE_1	5
#define MASTER_CAMNOC_IFE_LITE_2	6
#define MASTER_CAMNOC_IFE_LITE_3	7
#define MASTER_CAMNOC_IFE_LITE_4	8

/* Bayer Processing Segment */
#define MASTER_CAMNOC_BPS		9

/* Image Processing Engine */
#define MASTER_CAMNOC_IPE_0		10
#define MASTER_CAMNOC_IPE_1		11

/* JPEG encoder and DMA */
#define MASTER_CAMNOC_JPEG		12
#define MASTER_CAMNOC_JPEG_DMA		13

/* Camera Data Mover */
#define MASTER_CAMNOC_CDM		14

/* Face Detection */
#define MASTER_CAMNOC_FD		15

/* Sensor Bayer Interface */
#define MASTER_CAMNOC_SBI		16

/* Image Control Processor */
#define MASTER_CAMNOC_ICP_RD_WR	17

/* CAMNOC AXI slave (single exit point to external interconnect) */
#define SLAVE_CAMNOC_AXI		18

#endif /* __DT_BINDINGS_INTERCONNECT_QCOM_CAMNOC_H */
