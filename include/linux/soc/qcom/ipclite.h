/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Qualcomm IPCLite shared memory protocol
 */

#ifndef __QCOM_IPCLITE_H__
#define __QCOM_IPCLITE_H__

#include <linux/atomic.h>
#include <linux/types.h>

typedef atomic_t ipclite_atomic_uint32_t;
typedef atomic_t ipclite_atomic_int32_t;

enum ipcmem_host_type {
	IPCMEM_APPS = 0,
	IPCMEM_MODEM = 1,
	IPCMEM_LPASS = 2,
	IPCMEM_SLPI = 3,
	IPCMEM_GPU = 4,
	IPCMEM_CDSP = 5,
	IPCMEM_CVP = 6,
	IPCMEM_CAM = 7,
	IPCMEM_VPU = 8,
	IPCMEM_NUM_HOSTS = 9,

	IPCMEM_GLOBAL_HOST = 0xfe,
	IPCMEM_INVALID_HOST = 0xff,
};

struct global_region_info {
	void *virt_base;
	uint32_t size;
};

typedef int (*IPCLite_Client)(uint32_t proc_id, int64_t data, void *priv);

int ipclite_msg_send(int32_t proc_id, uint64_t data);
int ipclite_register_client(IPCLite_Client cb_func_ptr, void *priv);
int get_global_partition_info(struct global_region_info *global_ipcmem);
void ipclite_recover(enum ipcmem_host_type core_id);
uint32_t ipclite_global_test_and_set_bit(uint32_t nr, ipclite_atomic_uint32_t *addr);
uint32_t ipclite_global_test_and_clear_bit(uint32_t nr, ipclite_atomic_uint32_t *addr);

#endif
