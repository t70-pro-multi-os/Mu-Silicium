// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_LINUX_LID_H
#define T70_LINUX_LID_H
#include <Uefi.h>
#define T70_LID_TREE_SLACK 1024U
#define T70_LID_TREE_MAX 0x200000U
EFI_STATUS T70PrepareLinuxLidTree(CONST VOID *Original,UINTN Bytes,VOID *Output,UINTN Capacity);
#endif
