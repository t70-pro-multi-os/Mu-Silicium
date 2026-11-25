// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_PMIC_PRM_HOST_SHIM_H
#define T70_PMIC_PRM_HOST_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t BOOLEAN;
typedef uint8_t KIRQL;
typedef uint16_t USHORT;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint64_t ULONGLONG;
typedef uint64_t ULONG64;
typedef int64_t LONGLONG;
typedef uint8_t KPROCESSOR_MODE;
typedef struct _LARGE_INTEGER { int64_t QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef int32_t NTSTATUS;
typedef void VOID;
typedef void *PVOID;
typedef ULONG *PULONG;
typedef ULONG64 *PULONG64;
typedef BOOLEAN *PBOOLEAN;
typedef struct _GUID {
  uint32_t Data1;
  uint16_t Data2;
  uint16_t Data3;
  uint8_t Data4[8];
} GUID, *LPGUID;

typedef NTSTATUS (*PPRM_UNLOCK_MODULE)(LPGUID);
typedef NTSTATUS (*PPRM_LOCK_MODULE)(LPGUID);
typedef NTSTATUS (*PPRM_INVOKE_HANDLER)(LPGUID, PVOID, ULONG, PULONG64);
typedef NTSTATUS (*PPRM_QUERY_HANDLER)(LPGUID, PBOOLEAN);
typedef struct _PRM_INTERFACE {
  ULONG Version;
  PPRM_UNLOCK_MODULE UnlockModule;
  PPRM_LOCK_MODULE LockModule;
  PPRM_INVOKE_HANDLER InvokeHandler;
  PPRM_QUERY_HANDLER QueryHandler;
} PRM_INTERFACE, *PPRM_INTERFACE;

#define PRM_INTERFACE_V1 1U
#define PASSIVE_LEVEL 0U
#define DISPATCH_LEVEL 2U
#define KernelMode 0U
#define TRUE 1U
#define FALSE 0U
#define RTL_NUMBER_OF(Array) (sizeof(Array)/sizeof((Array)[0]))
#define MAXULONGLONG UINT64_MAX
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011L)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3L)
#define STATUS_DELETE_PENDING ((NTSTATUS)0xC0000056L)
#define STATUS_RETRY ((NTSTATUS)0xC000022DL)
#define STATUS_DATA_ERROR ((NTSTATUS)0xC000003EL)
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define RtlZeroMemory(Destination,Length) memset((Destination),0,(Length))

#endif
