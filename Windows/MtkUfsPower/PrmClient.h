/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_UFS_POWER_PRM_CLIENT_H_
#define MTK_UFS_POWER_PRM_CLIENT_H_

#include <ntddk.h>
#include <Library/MtkUfsPower.h>

/* Pins the UFS PRM module until OS restart; runtime module updates are unsupported. */
NTSTATUS MtkUfsPrmInvoke(MTK_UFS_POWER_REQUEST *Request);

#endif
