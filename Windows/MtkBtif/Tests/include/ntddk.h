// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef BTIF_TEST_NTDDK_H
#define BTIF_TEST_NTDDK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef void VOID,*PVOID,*PDRIVER_OBJECT,*PUNICODE_STRING;
typedef uint8_t UCHAR,BOOLEAN;
typedef uint16_t USHORT;
typedef uint32_t ULONG;
typedef int32_t LONG;
LONG InterlockedExchange(volatile LONG *, LONG);
LONG InterlockedCompareExchange(volatile LONG *, LONG, LONG);
typedef uint64_t ULONGLONG,KAFFINITY;
typedef int64_t LONGLONG;
typedef size_t SIZE_T;
typedef int32_t NTSTATUS;
typedef union {LONGLONG QuadPart;} PHYSICAL_ADDRESS;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xc0000182)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3)
#define STATUS_DEVICE_NOT_CONNECTED ((NTSTATUS)0xc000009d)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xc000009c)
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001a)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_CANCELLED ((NTSTATUS)0xc0000120)
#define NT_SUCCESS(S) ((S)>=0)
#define UNREFERENCED_PARAMETER(P) (void)(P)
#define RtlZeroMemory(P,N) memset(P,0,N)
#define PAGE_READWRITE 4
#define PAGE_NOCACHE 0x200
#define CmResourceTypeMemory 3
#define CmResourceTypeInterrupt 2
#define CmResourceShareDeviceExclusive 1
#define CmResourceShareDriverExclusive 2
#define CmResourceShareShared 3
#define CM_RESOURCE_MEMORY_READ_WRITE 0
#define CM_RESOURCE_INTERRUPT_LATCHED 1
#define CM_RESOURCE_INTERRUPT_MESSAGE 2
#define CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT 0x10
typedef struct {
 UCHAR Type,ShareDisposition;USHORT Flags;
 union {struct {PHYSICAL_ADDRESS Start;ULONG Length;} Memory;
        struct {ULONG Level,Vector;KAFFINITY Affinity;} Interrupt;} u;
} CM_PARTIAL_RESOURCE_DESCRIPTOR,*PCM_PARTIAL_RESOURCE_DESCRIPTOR;
typedef NTSTATUS DRIVER_INITIALIZE(PDRIVER_OBJECT,PUNICODE_STRING);
PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS,SIZE_T,ULONG);
VOID MmUnmapIoSpace(PVOID,SIZE_T);
ULONGLONG KeQueryInterruptTimePrecise(ULONGLONG *);
#endif
