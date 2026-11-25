/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef AW_TEST_SPB_H
#define AW_TEST_SPB_H
#include <ntddk.h>
#define IOCTL_SPB_EXECUTE_SEQUENCE 0x4242u
#define SpbTransferDirectionToDevice 0
#define SpbTransferDirectionFromDevice 1
typedef struct { unsigned Direction,Delay; UCHAR *Buffer; size_t Length; } SPB_TRANSFER_LIST_ENTRY;
typedef struct { unsigned Count; SPB_TRANSFER_LIST_ENTRY Transfers[2]; } SPB_TRANSFER_LIST;
#define SPB_TRANSFER_LIST_AND_ENTRIES(N) struct { SPB_TRANSFER_LIST List; }
#define SPB_TRANSFER_LIST_INIT(L,N) ((L)->Count=(N))
#define SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(D,T,B,N) ((SPB_TRANSFER_LIST_ENTRY){D,T,B,N})
#endif
