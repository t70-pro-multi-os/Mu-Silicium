/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef AW_TEST_WDF_H
#define AW_TEST_WDF_H
#include <ntddk.h>
typedef void *WDFIOTARGET;
typedef struct { void *Buffer; size_t Length; } WDF_MEMORY_DESCRIPTOR;
typedef struct { unsigned Flags; LONGLONG Timeout; } WDF_REQUEST_SEND_OPTIONS;
#define WDF_REQUEST_SEND_OPTION_TIMEOUT 1
#define WDF_REL_TIMEOUT_IN_MS(Ms) (-((LONGLONG)(Ms)*10000))
#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(D,B,L) (*(D)=(WDF_MEMORY_DESCRIPTOR){B,L})
#define WDF_REQUEST_SEND_OPTIONS_INIT(O,F) (*(O)=(WDF_REQUEST_SEND_OPTIONS){F,0})
#define WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(O,T) ((O)->Timeout=(T))
NTSTATUS WdfIoTargetSendWriteSynchronously(WDFIOTARGET,void *,WDF_MEMORY_DESCRIPTOR *,LONGLONG *,WDF_REQUEST_SEND_OPTIONS *,ULONG_PTR *);
NTSTATUS WdfIoTargetSendIoctlSynchronously(WDFIOTARGET,void *,unsigned,WDF_MEMORY_DESCRIPTOR *,WDF_MEMORY_DESCRIPTOR *,WDF_REQUEST_SEND_OPTIONS *,ULONG_PTR *);
typedef void *WDFDEVICE;
typedef struct {unsigned Unused;} WDF_OBJECT_ATTRIBUTES;
#define WDF_NO_OBJECT_ATTRIBUTES NULL
typedef uint16_t WCHAR;
typedef struct {USHORT Length,MaximumLength;WCHAR *Buffer;} UNICODE_STRING;
typedef struct {UNICODE_STRING *Name;ULONG DesiredAccess,ShareAccess,CreateDisposition,FileAttributes;} WDF_IO_TARGET_OPEN_PARAMS;
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_OPEN 1u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(P,N,A) (*(P)=(WDF_IO_TARGET_OPEN_PARAMS){N,A,0,0,0})
NTSTATUS WdfIoTargetCreate(WDFDEVICE,WDF_OBJECT_ATTRIBUTES *,WDFIOTARGET *);
NTSTATUS WdfIoTargetOpen(WDFIOTARGET,WDF_IO_TARGET_OPEN_PARAMS *);
void WdfIoTargetClose(WDFIOTARGET);
void WdfObjectDelete(void *);
typedef UNICODE_STRING *PUNICODE_STRING;
typedef void *WDFDRIVER;
typedef struct {void *EvtDriverDeviceAdd;unsigned DriverInitFlags;} WDF_DRIVER_CONFIG;
#define WDF_NO_EVENT_CALLBACK NULL
#define WdfDriverInitNoDispatchOverride 8u
#define WDF_DRIVER_CONFIG_INIT(C,E) (*(C)=(WDF_DRIVER_CONFIG){E,0})
NTSTATUS WdfDriverCreate(PDRIVER_OBJECT,PUNICODE_STRING,WDF_OBJECT_ATTRIBUTES *,WDF_DRIVER_CONFIG *,WDFDRIVER *);
#endif
