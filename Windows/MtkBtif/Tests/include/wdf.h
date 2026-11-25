// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef BTIF_TEST_WDF_H
#define BTIF_TEST_WDF_H
#include "../../../MtkConn/Tests/include/wdm.h"
#include "../../../MtkConn/Tests/include/wdf.h"

#define WDF_NO_HANDLE NULL
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003e)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xc0000010)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023)
#ifndef C_ASSERT
#define C_ASSERT(Expression) _Static_assert((Expression), #Expression)
#endif
#define MAXUCHAR UINT8_MAX
#define __dsb(Barrier) TestDsb(Barrier)

BOOLEAN WdfInterruptQueueWorkItemForIsr(WDFINTERRUPT);
UCHAR READ_REGISTER_UCHAR(volatile UCHAR *);
VOID WRITE_REGISTER_UCHAR(volatile UCHAR *, UCHAR);
VOID TestDsb(unsigned);

typedef void *WDFQUEUE, *WDFREQUEST;
typedef VOID EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL(WDFQUEUE, WDFREQUEST, size_t, size_t, ULONG);
typedef VOID EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE(WDFQUEUE, WDFREQUEST);
typedef VOID EVT_WDF_REQUEST_CANCEL(WDFREQUEST);
typedef enum { WdfIoQueueDispatchParallel, WdfIoQueueDispatchManual } WDF_IO_QUEUE_DISPATCH_TYPE;
typedef struct {
    BOOLEAN PowerManaged;
    EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL *EvtIoDeviceControl;
    EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE *EvtIoCanceledOnQueue;
    WDF_IO_QUEUE_DISPATCH_TYPE DispatchType;
} WDF_IO_QUEUE_CONFIG;
typedef struct { ULONG Flags; } WDF_REQUEST_FORWARD_OPTIONS;
typedef struct {
    BOOLEAN Removable;
    BOOLEAN SurpriseRemovalOK;
    BOOLEAN EjectSupported;
    ULONG Address;
    ULONG UINumber;
} WDF_DEVICE_PNP_CAPABILITIES;
#define WdfFalse FALSE
#define WdfTrue TRUE
#define WdfIoQueueDispatchParallel ((WDF_IO_QUEUE_DISPATCH_TYPE)0)
#define WdfIoQueueDispatchManual ((WDF_IO_QUEUE_DISPATCH_TYPE)1)
#define WDF_REQUEST_FORWARD_OPTION_SEND_AND_FORGET 1U
#define WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(Config, Dispatch) \
    (*(Config) = (WDF_IO_QUEUE_CONFIG){.PowerManaged = TRUE, .DispatchType = (Dispatch)})
#define WDF_IO_QUEUE_CONFIG_INIT(Config, Dispatch) \
    (*(Config) = (WDF_IO_QUEUE_CONFIG){.PowerManaged = TRUE, .DispatchType = (Dispatch)})
#define WDF_DEVICE_PNP_CAPABILITIES_INIT(Capabilities) memset((Capabilities), 0, sizeof(*(Capabilities)))
#define WDF_REQUEST_FORWARD_OPTIONS_INIT(Options) memset((Options), 0, sizeof(*(Options)))

PWDFDEVICE_INIT WdfPdoInitAllocate(WDFDEVICE);
NTSTATUS WdfPdoInitAssignDeviceID(PWDFDEVICE_INIT, PUNICODE_STRING);
NTSTATUS WdfPdoInitAddHardwareID(PWDFDEVICE_INIT, PUNICODE_STRING);
NTSTATUS WdfPdoInitAddCompatibleID(PWDFDEVICE_INIT, PUNICODE_STRING);
NTSTATUS WdfPdoInitAssignInstanceID(PWDFDEVICE_INIT, PUNICODE_STRING);
VOID WdfPdoInitAllowForwardingRequestToParent(PWDFDEVICE_INIT);
VOID WdfDeviceInitFree(PWDFDEVICE_INIT);
VOID WdfDeviceSetPnpCapabilities(WDFDEVICE, WDF_DEVICE_PNP_CAPABILITIES *);
NTSTATUS WdfIoQueueCreate(WDFDEVICE, WDF_IO_QUEUE_CONFIG *, WDF_OBJECT_ATTRIBUTES *, WDFQUEUE *);
NTSTATUS WdfFdoAddStaticChild(WDFDEVICE, WDFDEVICE);
WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE);
WDFDEVICE WdfPdoGetParent(WDFDEVICE);
WDFQUEUE WdfDeviceGetDefaultQueue(WDFDEVICE);
NTSTATUS WdfRequestForwardToParentDeviceIoQueue(WDFREQUEST, WDFQUEUE, WDF_REQUEST_FORWARD_OPTIONS *);
NTSTATUS WdfRequestForwardToIoQueue(WDFREQUEST, WDFQUEUE);
NTSTATUS WdfIoQueueRetrieveNextRequest(WDFQUEUE, WDFREQUEST *);
NTSTATUS WdfRequestMarkCancelableEx(WDFREQUEST, EVT_WDF_REQUEST_CANCEL *);
NTSTATUS WdfRequestUnmarkCancelable(WDFREQUEST);
NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST, size_t, PVOID *, size_t *);
NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST, size_t, PVOID *, size_t *);
VOID WdfRequestComplete(WDFREQUEST, NTSTATUS);
VOID WdfRequestCompleteWithInformation(WDFREQUEST, NTSTATUS, ULONG_PTR);
#endif
