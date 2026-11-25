/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_DEVICE_TREE_H_
#define T70_UFS_DEVICE_TREE_H_
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 1: exact enabled T70 topology, 0: disabled ancestor/node, -1: invalid. */
int T70UfsValidateDeviceTree(const void *Fdt, size_t Bytes);
#ifdef __cplusplus
}
#endif
#endif
