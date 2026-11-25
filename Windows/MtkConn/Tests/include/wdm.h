// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef CONN_TEST_WDM_H
#define CONN_TEST_WDM_H

#include "../../../MtkBtif/Tests/include/ntddk.h"

typedef UCHAR *PUCHAR;
typedef ULONG *PULONG;
typedef ULONGLONG *PULONGLONG;
typedef PHYSICAL_ADDRESS LARGE_INTEGER;
typedef struct { ULONG A; USHORT B, C; UCHAR D[8]; } GUID;
typedef struct {
    USHORT Size, Version;
    PVOID Context;
    void (*InterfaceReference)(PVOID);
    void (*InterfaceDereference)(PVOID);
} INTERFACE;
typedef enum { MmNonCached } MEMORY_CACHING_TYPE;

#define _IRQL_requires_(Level)
#define NTAPI
#define PASSIVE_LEVEL 0
#define KernelMode 0
#define MAXLONGLONG INT64_MAX
#define MAXULONG UINT32_MAX
#define RTL_NUMBER_OF(Elements) (sizeof(Elements) / sizeof((Elements)[0]))
#define CM_RESOURCE_MEMORY_READ_ONLY 1
#define CM_RESOURCE_MEMORY_WRITE_ONLY 2
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xc00000b5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)

PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T, PHYSICAL_ADDRESS, PHYSICAL_ADDRESS,
                                            PHYSICAL_ADDRESS, MEMORY_CACHING_TYPE);
VOID MmFreeContiguousMemory(PVOID);
PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID);
UCHAR KeGetCurrentIrql(VOID);
VOID KeStallExecutionProcessor(ULONG);
NTSTATUS KeDelayExecutionThread(int, BOOLEAN, const LARGE_INTEGER *);
VOID KeMemoryBarrier(VOID);
ULONG READ_REGISTER_ULONG(volatile ULONG *);
VOID WRITE_REGISTER_ULONG(volatile ULONG *, ULONG);
VOID RtlCopyMemory(PVOID, const void *, SIZE_T);
PVOID RtlSecureZeroMemory(PVOID, SIZE_T);

#endif
