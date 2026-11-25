// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#include <Library/MtkBtifCore.h>
#include <Library/MtkWmtMode.h>
#include "HciTransport.h"
#include "../MtkConn/BtifInterface.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD BtifDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE BtifPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE BtifRelease;
EVT_WDF_DEVICE_D0_ENTRY BtifD0Entry;
EVT_WDF_DEVICE_D0_EXIT BtifD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL BtifSurpriseRemoval;
EVT_WDF_INTERRUPT_ISR BtifInterrupt;
EVT_WDF_INTERRUPT_WORKITEM BtifInterruptFailure;
EVT_WDF_IO_TARGET_QUERY_REMOVE BtifProviderQueryRemove;
EVT_WDF_IO_TARGET_REMOVE_COMPLETE BtifProviderRemoved;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS BtifQueryRelations;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL BtifChildIoctl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL BtifParentIoctl;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE BtifWriteCanceled;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE BtifReadCanceledOnQueue;

typedef struct {
    WDFWAITLOCK Lock;
    WDFIOTARGET Target;
    PDEVICE_OBJECT Provider;
    T70_CONN_BTIF_INTERFACE Conn;
    ULONGLONG Token, ModeSession;
    PVOID Mapping[3];
    MTK_BTIF_CORE Fifo;
    MTK_WMT_MODE Mode;
    MTK_WMT_FULL_CONTROL Radio;
    BOOLEAN RadioReady;
    T70_BT_HCI_TRANSPORT Hci;
    ULONG HciEpoch;
    WDFDEVICE BthxChild;
    WDFQUEUE ParentQueue, ReadEventQueue, ReadAclQueue, WriteQueue;
    MTK_STP_FRAME PendingHciFrame;
    ULONG PendingHciFrameOffset, PendingHciFrameLength;
    UCHAR PendingHciRaw[MTK_BTIF_RX_BUDGET];
    ULONG PendingHciRawLength, InterruptEnableBase;
    BOOLEAN PendingHciFrameActive, RxPaused;
    BOOLEAN Prepared;
    BOOLEAN Removed;
    BOOLEAN Faulted;
} BTIF_OWNER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTIF_OWNER, BtifContext)

static VOID BtifCompleteReadQueue(BTIF_OWNER *Owner, UCHAR Type);
static VOID BtifCompleteWrite(BTIF_OWNER *Owner, ULONG Written, NTSTATUS TransferStatus);
static VOID BtifFailQueuedRequests(BTIF_OWNER *Owner, NTSTATUS Status);

static const ULONGLONG BtifBase[3] = {0x1100c000, 0x10217b00, 0x10217b80};
static const ULONG BtifLength[3] = {0x1000, 0x80, 0x80};
static const ULONG BtifGsi[3] = {222, 216, 217};
/* MediaTek WMT source enum assigns WMTDRV_TYPE_BT = 0. */
#define T70_WMT_FUNCTION_BT 0U

static NTSTATUS BtifValidate(WDFCMRESLIST Raw, WDFCMRESLIST Translated) {
    ULONG Pass, Index;
    if (!Raw || !Translated || WdfCmResourceListGetCount(Raw) != 6 ||
        WdfCmResourceListGetCount(Translated) != 6) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (Pass = 0; Pass < 2; ++Pass) {
        for (Index = 0; Index < 6; ++Index) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR R = WdfCmResourceListGetDescriptor(Pass ? Translated : Raw, Index);
            if (!R) return STATUS_DEVICE_CONFIGURATION_ERROR;
            if (Index < 3) {
                if (R->Type != CmResourceTypeMemory ||
                    R->u.Memory.Start.QuadPart != (LONGLONG)BtifBase[Index] ||
                    R->u.Memory.Length != BtifLength[Index] || R->Flags != CM_RESOURCE_MEMORY_READ_WRITE)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
            } else {
                if (R->Type != CmResourceTypeInterrupt ||
                    (R->ShareDisposition != CmResourceShareDeviceExclusive &&
                     R->ShareDisposition != CmResourceShareDriverExclusive) ||
                    (R->Flags & (CM_RESOURCE_INTERRUPT_LATCHED | CM_RESOURCE_INTERRUPT_MESSAGE |
                                 CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT)) ||
                    (!Pass && R->u.Interrupt.Vector != BtifGsi[Index - 3]))
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                // Translated vector/level/affinity are OS-assigned, not GSIVs.
            }
        }
    }
    return STATUS_SUCCESS;
}

static VOID BtifDropMappings(BTIF_OWNER *Owner)
{
    for (ULONG Index = 3; Index != 0; --Index) {
        PVOID Mapping = Owner->Mapping[Index - 1];
        Owner->Mapping[Index - 1] = NULL;
        if (Mapping) {
            MmUnmapIoSpace(Mapping, BtifLength[Index - 1]);
        }
    }
}

// These functions are reachable only inside a WCN0 Access callback.
static int BtifRead(void *Context, unsigned Region, unsigned Offset, unsigned Width, unsigned *Value)
{
    BTIF_OWNER *Owner = Context;
    if (Region >= RTL_NUMBER_OF(BtifLength) || !Owner->Mapping[Region] ||
        (Width == 1 ? (Region != 0 || Offset != 0) :
         (Width != 4 || (Offset & 3) || Offset > BtifLength[Region] - 4))) {
        return -1;
    }
    volatile UCHAR *Address = (volatile UCHAR *)Owner->Mapping[Region] + Offset;
    *Value = Width == 1 ? READ_REGISTER_UCHAR(Address) : READ_REGISTER_ULONG((volatile ULONG *)Address);
    return 0;
}

static int BtifWrite(void *Context, unsigned Region, unsigned Offset, unsigned Width, unsigned Value)
{
    BTIF_OWNER *Owner = Context;
    if (Region != 0 || !Owner->Mapping[0] ||
        (Width == 1 ? (Offset != 0 || Value > MAXUCHAR) :
         (Width != 4 || (Offset & 3) || Offset > BtifLength[0] - 4))) {
        return -1;
    }
    volatile UCHAR *Address = (volatile UCHAR *)Owner->Mapping[0] + Offset;
    if (Width == 1) {
        WRITE_REGISTER_UCHAR(Address, (UCHAR)Value);
    } else {
        WRITE_REGISTER_ULONG((volatile ULONG *)Address, Value);
    }
    return 0;
}

static void BtifBarrier(void *Context)
{
    UNREFERENCED_PARAMETER(Context);
    __dsb(15);
}

static NTSTATUS ExactStatus(NTSTATUS Status)
{
    return Status == STATUS_SUCCESS || !NT_SUCCESS(Status) ? Status : STATUS_DEVICE_NOT_READY;
}

NTSTATUS BtifProviderQueryRemove(WDFIOTARGET Target)
{
    UNREFERENCED_PARAMETER(Target);
    return STATUS_DEVICE_BUSY;
}

VOID BtifProviderRemoved(WDFIOTARGET Target)
{
    WDFDEVICE Device = WdfIoTargetGetDevice(Target);
    BTIF_OWNER *Owner = BtifContext(Device);
    WdfWaitLockAcquire(Owner->Lock, NULL);
    BOOLEAN Active = Owner->Target == Target && !Owner->Removed;
    if (Active) {
        Owner->Removed = TRUE;
        if (Owner->HciEpoch != 0) {
            (void)T70BtHciClose(&Owner->Hci, Owner->HciEpoch);
            Owner->HciEpoch = 0;
        }
    }
    WdfWaitLockRelease(Owner->Lock);

    WdfIoTargetClose(Target);
    if (Active) {
        BtifFailQueuedRequests(Owner, STATUS_DEVICE_NOT_CONNECTED);
        IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(Device), PowerRelations);
        WdfDeviceSetFailed(Device, WdfDeviceFailedNoRestart);
    }
}

static NTSTATUS BtifOpenProvider(WDFDEVICE Device)
{
    BTIF_OWNER *Owner = BtifContext(Device);
    PWSTR Paths = NULL;
    NTSTATUS Status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_T70_CONN_BTIF, NULL, 0, &Paths);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    if (!Paths || !*Paths) {
        if (Paths) {
            ExFreePool(Paths);
        }
        return STATUS_DEVICE_NOT_READY;
    }

    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, Paths);
    WDFIOTARGET Target = NULL;
    Status = Paths[Name.Length / sizeof(WCHAR) + 1] ? STATUS_DEVICE_CONFIGURATION_ERROR :
             WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &Target);
    if (Status == STATUS_SUCCESS) {
        WdfWaitLockAcquire(Owner->Lock, NULL);
        Owner->Target = Target;
        WdfWaitLockRelease(Owner->Lock);
        WDF_IO_TARGET_OPEN_PARAMS Params;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Params, &Name, FILE_READ_ATTRIBUTES);
        Params.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        Params.EvtIoTargetQueryRemove = BtifProviderQueryRemove;
        Params.EvtIoTargetRemoveComplete = BtifProviderRemoved;
        Status = WdfIoTargetOpen(Target, &Params);
    }
    ExFreePool(Paths);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }

    T70_CONN_BTIF_INTERFACE Conn = {0};
    Status = WdfIoTargetQueryForInterface(Target, &GUID_T70_CONN_BTIF_INTERFACE_V2,
                                        &Conn.Header, sizeof(Conn),
                                        T70_CONN_BTIF_INTERFACE_VERSION, NULL);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    WdfWaitLockAcquire(Owner->Lock, NULL);
    // The target can disappear during the synchronous query.
    if (Owner->Removed) {
        Status = STATUS_DEVICE_REMOVED;
    } else {
        PDEVICE_OBJECT Pdo = WdfIoTargetWdmGetTargetPhysicalDevice(Target);
        if (Conn.Header.Size != sizeof(Conn) || Conn.Header.Version != T70_CONN_BTIF_INTERFACE_VERSION ||
            !Conn.Header.Context || !Conn.Header.InterfaceReference || !Conn.Header.InterfaceDereference ||
            !Conn.Acquire || !Conn.Access || !Conn.Release || !Conn.ClaimSession || !Pdo ||
            Pdo == WdfDeviceWdmGetPhysicalDevice(Device)) {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else {
            ObReferenceObject(Pdo);
            Owner->Provider = Pdo;
            Owner->Conn = Conn;
        }
    }
    WdfWaitLockRelease(Owner->Lock);
    if (Status != STATUS_SUCCESS && Conn.Header.Context && Conn.Header.InterfaceDereference) {
        Conn.Header.InterfaceDereference(Conn.Header.Context);
    }
    return Status;
}

NTSTATUS BtifPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    BTIF_OWNER *Owner = BtifContext(Device);
    if (Owner->Prepared || Owner->Mapping[0] || Owner->Target) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    NTSTATUS Status = BtifValidate(Raw, Translated);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    for (ULONG Index = 0; Index < RTL_NUMBER_OF(BtifBase); ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR R = WdfCmResourceListGetDescriptor(Translated, Index);
        Owner->Mapping[Index] = MmMapIoSpaceEx(R->u.Memory.Start, BtifLength[Index], PAGE_READWRITE | PAGE_NOCACHE);
        if (!Owner->Mapping[Index]) {
            BtifDropMappings(Owner);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Status = BtifOpenProvider(Device);
    if (Status == STATUS_SUCCESS) {
        WDF_INTERRUPT_CONFIG Config;
        WDFINTERRUPT Interrupt;
        WDF_INTERRUPT_CONFIG_INIT(&Config, BtifInterrupt, NULL);
        Config.PassiveHandling = TRUE;
        Config.WaitLock = NULL;
        Config.EvtInterruptWorkItem = BtifInterruptFailure;
        Config.InterruptRaw = WdfCmResourceListGetDescriptor(Raw, 3);
        Config.InterruptTranslated = WdfCmResourceListGetDescriptor(Translated, 3);
        Status = WdfInterruptCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &Interrupt);
    }
    // KMDF calls ReleaseHardware even when PrepareHardware fails.
    WdfWaitLockAcquire(Owner->Lock, NULL);
    if (Status == STATUS_SUCCESS && Owner->Removed) {
        Status = STATUS_DEVICE_REMOVED;
    }
    Owner->Prepared = Status == STATUS_SUCCESS;
    WdfWaitLockRelease(Owner->Lock);
    if (Status == STATUS_SUCCESS) {
        IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(Device), PowerRelations);
    }
    return ExactStatus(Status);
}

static NTSTATUS BtifCoreStatus(MTK_BTIF_STATUS Status)
{
    switch (Status) {
    case MtkBtifOk:
        return STATUS_SUCCESS;
    case MtkBtifBusy:
        return STATUS_DEVICE_BUSY;
    default:
        return STATUS_IO_DEVICE_ERROR;
    }
}

static NTSTATUS NTAPI BtifStartFifo(PVOID Context)
{
    BTIF_OWNER *Owner = Context;
    NTSTATUS Status = BtifCoreStatus(MtkBtifStart(&Owner->Fifo, MTK_BTIF_TX_TRIGGER,
                                                  MTK_BTIF_RX_TRIGGER));
    unsigned Value;
    if (Status == STATUS_SUCCESS) {
        if (BtifRead(Owner, 0, 4, 4, &Value)) return STATUS_IO_DEVICE_ERROR;
        Owner->InterruptEnableBase = Value & ~2U;
    }
    return Status;
}

static NTSTATUS NTAPI BtifStopFifo(PVOID Context)
{
    BTIF_OWNER *Owner = Context;
    return BtifCoreStatus(MtkBtifStop(&Owner->Fifo));
}

static NTSTATUS NTAPI BtifMaskFifoInterrupts(PVOID Context)
{
    unsigned Value;
    if (BtifRead(Context, 0, 4, 4, &Value) || Value == MAXULONG ||
        BtifWrite(Context, 0, 4, 4, Value & ~3U)) {
        return STATUS_IO_DEVICE_ERROR;
    }
    BtifBarrier(Context);
    unsigned Actual;
    return !BtifRead(Context, 0, 4, 4, &Actual) && Actual == (Value & ~3U) ?
           STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS NTAPI BtifRestoreFifoInterrupts(PVOID Context)
{
    BTIF_OWNER *Owner = Context;
    unsigned Value = Owner->InterruptEnableBase | (Owner->Fifo.Count ? 2U : 0U);
    if (BtifWrite(Owner, 0, 4, 4, Value) != 0) return STATUS_IO_DEVICE_ERROR;
    BtifBarrier(Owner);
    unsigned Actual;
    return !BtifRead(Owner, 0, 4, 4, &Actual) && Actual == Value ?
           STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static ULONGLONG BtifNowMs(void)
{
    ULONGLONG Counter;
    return KeQueryInterruptTimePrecise(&Counter) / 10000;
}

// The caller holds Lock. A failed attempt can leave the CONN clock journal
// active, but never leaves a client token available to the ISR.
static NTSTATUS BtifEndLease(BTIF_OWNER *Owner, BOOLEAN TryStop)
{
    if (!Owner->Token) {
        return STATUS_SUCCESS;
    }
    ULONGLONG Token = Owner->Token;
    Owner->Token = 0;
    T70_CONN_BTIF_INTERFACE *Conn = &Owner->Conn;
    NTSTATUS Status = STATUS_DEVICE_REMOVED;
    if (!Owner->Removed) {
        Status = STATUS_IO_DEVICE_ERROR;
        const MTK_STP_FULL_LINK *Link = &Owner->Mode.Link;
        if (TryStop && !Owner->Faulted && !Owner->Mode.Decoder.State &&
            !Link->HeaderUsed && !Link->OfferedLength && !Link->Resync &&
            !Link->AckPending && !Link->TxLength) {
            Status = ExactStatus(Conn->Access(Conn->Header.Context, Token, BtifStopFifo, Owner));
        }
        if (Status != STATUS_SUCCESS && Owner->Fifo.Owned) {
            // Stop may have failed before disabling the controller source.
            Conn->Access(Conn->Header.Context, Token, BtifMaskFifoInterrupts, Owner);
        }
    }
    NTSTATUS Release = ExactStatus(Conn->Release(Conn->Header.Context, Token, Status == STATUS_SUCCESS));
    if (Status == STATUS_SUCCESS) {
        Status = Release;
    }
    if (Status != STATUS_SUCCESS) {
        Owner->Faulted = TRUE;
    }
    return Owner->Removed ? STATUS_SUCCESS : Status;
}

static VOID BtifClearPausedRx(BTIF_OWNER *Owner)
{
    Owner->PendingHciFrameActive = FALSE;
    Owner->PendingHciFrameOffset = 0U;
    Owner->PendingHciFrameLength = 0U;
    Owner->PendingHciRawLength = 0U;
    Owner->RxPaused = FALSE;
    RtlZeroMemory(&Owner->PendingHciFrame, sizeof(Owner->PendingHciFrame));
    RtlZeroMemory(Owner->PendingHciRaw, sizeof(Owner->PendingHciRaw));
}

static NTSTATUS BtifStartMode(BTIF_OWNER *Owner, ULONGLONG Token, ULONGLONG Session);
static NTSTATUS BtifStartRadio(BTIF_OWNER *Owner, ULONGLONG Token);

NTSTATUS BtifD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    BTIF_OWNER *Owner = BtifContext(Device);
    ULONGLONG Token = 0, Session = 0;
    BOOLEAN Fresh = FALSE;
    WdfWaitLockAcquire(Owner->Lock, NULL);
    BtifClearPausedRx(Owner);
    NTSTATUS Status = STATUS_DEVICE_NOT_READY;
    if (Owner->Removed) {
        Status = STATUS_DEVICE_REMOVED;
    } else if (Owner->Prepared && !Owner->Faulted && !Owner->Token) {
        T70_CONN_BTIF_INTERFACE *Conn = &Owner->Conn;
        Status = ExactStatus(Conn->Acquire(Conn->Header.Context, &Owner->Token));
        if (Status == STATUS_SUCCESS && Owner->Token) {
            Token = Owner->Token;
            Status = ExactStatus(Conn->Access(Conn->Header.Context, Owner->Token, BtifStartFifo, Owner));
            if (Status == STATUS_SUCCESS) {
                Status = ExactStatus(Conn->ClaimSession(Conn->Header.Context, Token, &Session, &Fresh));
            }
            if (Status == STATUS_SUCCESS && (!Session || (!Fresh && Session != Owner->ModeSession))) {
                // A lost client may have left the peer in full mode with live sequence numbers.
                Status = STATUS_DEVICE_NOT_READY;
            }
            if (Status == STATUS_SUCCESS && Fresh) {
                Owner->ModeSession = 0;
                MtkWmtModeInit(&Owner->Mode, &Owner->Fifo, MtkBtifStpWrite,
                               MtkBtifFullStpWrite, BtifNowMs());
            }
            if (Status != STATUS_SUCCESS) {
                // Only attempt the transactional restore if Start took ownership.
                BtifEndLease(Owner, Owner->Fifo.Owned != 0);
            }
        } else if (Status == STATUS_SUCCESS) {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    WdfWaitLockRelease(Owner->Lock);
    if (Status == STATUS_SUCCESS && Fresh) {
        Status = BtifStartMode(Owner, Token, Session);
    }
    if (Status == STATUS_SUCCESS) {
        // A clean D0 exit closes the HCI epoch while preserving STP sequence
        // state for the same MCU session. Rebind on both cold start and resume.
        WdfWaitLockAcquire(Owner->Lock, NULL);
        Owner->Hci.NowMs = BtifNowMs();
        Status = T70BtHciOpen(&Owner->Hci, &Owner->Mode.Link, &Owner->Fifo,
                              &Owner->HciEpoch);
        WdfWaitLockRelease(Owner->Lock);
    }
    if (Status == STATUS_SUCCESS) Status = BtifStartRadio(Owner, Token);
    if (Status != STATUS_SUCCESS && Token) {
        WdfWaitLockAcquire(Owner->Lock, NULL);
        if (Owner->Token == Token) {
            if (Owner->HciEpoch) {
                (void)T70BtHciClose(&Owner->Hci, Owner->HciEpoch);
                Owner->HciEpoch = 0;
            }
            Owner->RadioReady = FALSE;
            Owner->ModeSession = 0;
            Owner->Faulted = TRUE;
            (void)BtifEndLease(Owner, FALSE);
        }
        WdfWaitLockRelease(Owner->Lock);
    }
    return Status;
}

NTSTATUS BtifD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    BTIF_OWNER *Owner = BtifContext(Device);
    WdfWaitLockAcquire(Owner->Lock, NULL);
    if (Owner->HciEpoch != 0) {
        (void)T70BtHciClose(&Owner->Hci, Owner->HciEpoch);
        Owner->HciEpoch = 0;
    }
    Owner->RadioReady = FALSE;
    BtifClearPausedRx(Owner);
    NTSTATUS Status = BtifEndLease(Owner, TRUE);
    WdfWaitLockRelease(Owner->Lock);
    BtifFailQueuedRequests(Owner, STATUS_DEVICE_NOT_CONNECTED);
    return Status;
}

VOID BtifSurpriseRemoval(WDFDEVICE Device)
{
    BTIF_OWNER *Owner = BtifContext(Device);
    WdfWaitLockAcquire(Owner->Lock, NULL);
    Owner->Removed = TRUE;
    if (Owner->HciEpoch != 0) {
        (void)T70BtHciClose(&Owner->Hci, Owner->HciEpoch);
        Owner->HciEpoch = 0;
    }
    WdfWaitLockRelease(Owner->Lock);
    BtifFailQueuedRequests(Owner, STATUS_DEVICE_NOT_CONNECTED);
}

NTSTATUS BtifRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    BTIF_OWNER *Owner = BtifContext(Device);
    WdfWaitLockAcquire(Owner->Lock, NULL);
    Owner->Prepared = FALSE;
    Owner->Removed = TRUE;
    BtifEndLease(Owner, FALSE);
    WDFIOTARGET Target = Owner->Target;
    PDEVICE_OBJECT Provider = Owner->Provider;
    INTERFACE Header = Owner->Conn.Header;
    Owner->Target = NULL;
    Owner->Provider = NULL;
    Owner->ModeSession = 0;
    if (Owner->HciEpoch != 0) {
        (void)T70BtHciClose(&Owner->Hci, Owner->HciEpoch);
        Owner->HciEpoch = 0;
    }
    RtlZeroMemory(&Owner->Conn, sizeof(Owner->Conn));
    BtifDropMappings(Owner);
    Owner->Removed = FALSE;
    WdfWaitLockRelease(Owner->Lock);

    if (Provider) {
        IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(Device), PowerRelations);
        ObDereferenceObject(Provider);
    }
    if (Header.InterfaceDereference) {
        Header.InterfaceDereference(Header.Context);
    }
    if (Target) {
        WdfObjectDelete(Target);
    }
    BtifFailQueuedRequests(Owner, STATUS_DEVICE_NOT_CONNECTED);
    return STATUS_SUCCESS;
}

typedef struct {
    BTIF_OWNER *Owner;
    BOOLEAN Negotiating;
    UCHAR Bytes[MTK_BTIF_RX_BUDGET];
    MTK_BTIF_SERVICE Service;
    ULONG HciWritten;
    BOOLEAN EventsAvailable, AclAvailable;
} BTIF_INTERRUPT_IO;

static NTSTATUS BtifProcessFullBytes(BTIF_OWNER *Owner, const UCHAR *Bytes,
                                     ULONG Length, BTIF_INTERRUPT_IO *Io)
{
    MTK_STP_FULL_LINK *Link = &Owner->Mode.Link;
    for (ULONG Position = 0; Position < Length;) {
        unsigned RawConsumed = 0;
        ULONG HciConsumed = 0;
        MTK_STP_FRAME Frame;
        MTK_STP_STATUS Rx = MtkStpFullReceive(Link, Bytes + Position,
                                             Length - Position, &RawConsumed, &Frame);
        if (Rx != MtkStpNeedMore && Rx != MtkStpFrameReady) return STATUS_DATA_ERROR;
        if (Rx == MtkStpFrameReady) {
            if (Owner->HciEpoch != 0 && Frame.Channel == T70_BT_STP_CHANNEL) {
                NTSTATUS HciStatus = T70BtHciReceive(&Owner->Hci, Owner->HciEpoch,
                                                     &Frame, &HciConsumed);
                if (HciStatus != STATUS_SUCCESS &&
                    !(HciStatus == STATUS_DEVICE_BUSY && HciConsumed == Frame.Length)) {
                    if (HciStatus != STATUS_DEVICE_BUSY) return HciStatus;
                    Owner->PendingHciFrame = Frame;
                    Owner->PendingHciFrameOffset = HciConsumed;
                    Owner->PendingHciFrameLength = Frame.Length;
                    Owner->PendingHciFrameActive = TRUE;
                    ULONG TailBytes = Length - Position - RawConsumed;
                    if (TailBytes > sizeof(Owner->PendingHciRaw)) return STATUS_BUFFER_OVERFLOW;
                    if (TailBytes) RtlCopyMemory(Owner->PendingHciRaw,
                                                 Bytes + Position + RawConsumed, TailBytes);
                    Owner->PendingHciRawLength = TailBytes;
                    Owner->RxPaused = TRUE;
                    if (Io) {
                        Io->EventsAvailable = Owner->Hci.Events.Count != 0;
                        Io->AclAvailable = Owner->Hci.Acl.Count != 0;
                    }
                    return BtifMaskFifoInterrupts(Owner);
                }
                if (Io) {
                    Io->EventsAvailable = Owner->Hci.Events.Count != 0;
                    Io->AclAvailable = Owner->Hci.Acl.Count != 0;
                }
            } else if (Frame.Channel == MTK_STP_WMT_CHANNEL && Owner->Radio.Link) {
                MTK_WMT_STATUS Wmt = MtkWmtFullControlEvent(&Owner->Radio, &Frame);
                if (Wmt == MtkWmtOk) {
                    if (Owner->Radio.Function != T70_WMT_FUNCTION_BT || !Owner->Radio.Enable)
                        return STATUS_DEVICE_DATA_ERROR;
                    Owner->RadioReady = TRUE;
                } else if (Wmt != MtkWmtUnrelated) {
                    return Wmt == MtkWmtRejected ? STATUS_DEVICE_NOT_READY :
                           Wmt == MtkWmtMalformed ? STATUS_DATA_ERROR : STATUS_IO_DEVICE_ERROR;
                }
            }
            MTK_STP_STATUS Accepted = MtkStpFullAccept(Link);
            if (Accepted != MtkStpOk) return STATUS_DATA_ERROR;
        }
        if (!RawConsumed) return STATUS_DATA_ERROR;
        Position += RawConsumed;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI BtifResumePendingRx(PVOID Context)
{
    BTIF_OWNER *Owner = Context;
    if (!Owner->RxPaused) return STATUS_SUCCESS;
    if (Owner->PendingHciFrameActive) {
        MTK_STP_FRAME Remaining = Owner->PendingHciFrame;
        if (Owner->PendingHciFrameOffset > Owner->PendingHciFrameLength)
            return STATUS_DATA_ERROR;
        Remaining.Data += Owner->PendingHciFrameOffset;
        Remaining.Length = Owner->PendingHciFrameLength - Owner->PendingHciFrameOffset;
        ULONG Consumed = 0;
        NTSTATUS Status = T70BtHciReceive(&Owner->Hci, Owner->HciEpoch,
                                          &Remaining, &Consumed);
        Owner->PendingHciFrameOffset += Consumed;
        if (Status == STATUS_DEVICE_BUSY &&
            Owner->PendingHciFrameOffset < Owner->PendingHciFrameLength)
            return STATUS_SUCCESS;
        if (Status != STATUS_SUCCESS &&
            !(Status == STATUS_DEVICE_BUSY &&
              Owner->PendingHciFrameOffset == Owner->PendingHciFrameLength))
            return Status;
        if (Owner->PendingHciFrameOffset != Owner->PendingHciFrameLength)
            return STATUS_DATA_ERROR;
        if (MtkStpFullAccept(&Owner->Mode.Link) != MtkStpOk) return STATUS_DATA_ERROR;
        Owner->PendingHciFrameActive = FALSE;
        Owner->PendingHciFrameOffset = Owner->PendingHciFrameLength = 0;
    }
    if (Owner->PendingHciRawLength) {
        UCHAR Pending[MTK_BTIF_RX_BUDGET];
        ULONG Length = Owner->PendingHciRawLength;
        RtlCopyMemory(Pending, Owner->PendingHciRaw, Length);
        Owner->PendingHciRawLength = 0;
        BTIF_INTERRUPT_IO Io = { .Owner = Owner };
        NTSTATUS Status = BtifProcessFullBytes(Owner, Pending, Length, &Io);
        if (!NT_SUCCESS(Status)) return Status;
    }
    if (Owner->PendingHciFrameActive) return STATUS_SUCCESS;
    Owner->Hci.NowMs = BtifNowMs();
    MTK_STP_STATUS Tx = MtkStpFullService(&Owner->Mode.Link, Owner->Hci.NowMs);
    if (Tx != MtkStpOk && Tx != MtkStpFull) return STATUS_IO_DEVICE_ERROR;
    Owner->RxPaused = FALSE;
    return BtifRestoreFifoInterrupts(Owner);
}

static NTSTATUS BtifResumeRx(BTIF_OWNER *Owner)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WdfWaitLockAcquire(Owner->Lock, NULL);
    if (Owner->RxPaused && Owner->HciEpoch != 0 && Owner->Token != 0)
        Status = ExactStatus(Owner->Conn.Access(Owner->Conn.Header.Context,
                                                Owner->Token,
                                                BtifResumePendingRx, Owner));
    if (!NT_SUCCESS(Status)) Owner->Faulted = TRUE;
    WdfWaitLockRelease(Owner->Lock);
    return Status;
}

static NTSTATUS NTAPI BtifServiceFifo(PVOID Context)
{
    BTIF_INTERRUPT_IO *Io = Context;
    BTIF_OWNER *Owner = Io->Owner;
    if (!Io->Negotiating && Owner->RxPaused) {
        RtlZeroMemory(&Io->Service, sizeof(Io->Service));
        return STATUS_SUCCESS;
    }

    MTK_BTIF_STATUS BtifStatus;
    if (!Io->Negotiating && Owner->HciEpoch != 0) {
        Owner->Hci.NowMs = BtifNowMs();
        NTSTATUS Status = T70BtBthxService(&Owner->Hci, Owner->HciEpoch,
                                           Io->Bytes, sizeof(Io->Bytes),
                                           &Io->Service, &Io->HciWritten);
        if (!NT_SUCCESS(Status)) return Status;
        BtifStatus = MtkBtifOk;
    } else {
        BtifStatus = MtkBtifService(&Owner->Fifo, Io->Bytes, sizeof(Io->Bytes), &Io->Service);
    }
    if (BtifStatus != MtkBtifOk && BtifStatus != MtkBtifMore && BtifStatus != MtkBtifNotOurs)
        return BtifCoreStatus(BtifStatus);

    if (Io->Negotiating) {
        ULONGLONG Now = BtifNowMs();
        MTK_WMT_STATUS Wmt = MtkWmtModeReceive(&Owner->Mode, Io->Bytes, Io->Service.Received, Now);
        if (Wmt == MtkWmtPending) Wmt = MtkWmtModeService(&Owner->Mode, Now);
        switch (Wmt) {
        case MtkWmtOk:
            if (!Owner->Fifo.Count) return STATUS_SUCCESS;
            return Now - Owner->Mode.SinceMs >= MTK_WMT_MODE_TIMEOUT_MS ?
                STATUS_IO_TIMEOUT : STATUS_PENDING;
        case MtkWmtPending:
            return STATUS_PENDING;
        case MtkWmtRejected:
        case MtkWmtMalformed:
            return STATUS_DATA_ERROR;
        default:
            return Now - Owner->Mode.SinceMs >= MTK_WMT_MODE_TIMEOUT_MS ?
                STATUS_IO_TIMEOUT : STATUS_IO_DEVICE_ERROR;
        }
    }

    NTSTATUS Status = BtifProcessFullBytes(Owner, Io->Bytes, Io->Service.Received, Io);
    if (!NT_SUCCESS(Status)) return Status;
    Owner->Hci.NowMs = BtifNowMs();
    MTK_STP_STATUS Tx = MtkStpFullService(&Owner->Mode.Link, Owner->Hci.NowMs);
    return Tx == MtkStpOk || Tx == MtkStpFull ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS NTAPI BtifBeginRadio(PVOID Context)
{
    BTIF_OWNER *Owner = Context;
    MTK_WMT_STATUS Status = MtkWmtFullControlInit(&Owner->Radio, &Owner->Mode.Link);
    if (Status != MtkWmtOk) return STATUS_DEVICE_NOT_READY;
    Owner->RadioReady = FALSE;
    Status = MtkWmtFullControlBegin(&Owner->Radio, T70_WMT_FUNCTION_BT, 1U, BtifNowMs());
    if (Status == MtkWmtPending) return STATUS_SUCCESS;
    if (Status == MtkWmtBusy) return STATUS_DEVICE_BUSY;
    if (Status == MtkWmtInvalid) return STATUS_INVALID_PARAMETER;
    return Status == MtkWmtUncertain ? STATUS_IO_DEVICE_ERROR : STATUS_DEVICE_NOT_READY;
}

static NTSTATUS BtifStartRadio(BTIF_OWNER *Owner, ULONGLONG Token)
{
    ULONGLONG Started, Now = 0;
    NTSTATUS Status;
    WdfWaitLockAcquire(Owner->Lock, NULL);
    Started = BtifNowMs();
    if (!Owner->Prepared || Owner->Removed || Owner->Faulted ||
        Owner->Token != Token || !Owner->HciEpoch) {
        Status = STATUS_DEVICE_NOT_READY;
    } else {
        Status = Owner->Conn.Access(Owner->Conn.Header.Context, Token,
                                    BtifBeginRadio, Owner);
    }
    WdfWaitLockRelease(Owner->Lock);
    if (Status != STATUS_SUCCESS) return ExactStatus(Status);

    for (;;) {
        BTIF_INTERRUPT_IO Io = { .Owner = Owner };
        MTK_WMT_STATUS Expiry;
        WdfWaitLockAcquire(Owner->Lock, NULL);
        if (!Owner->Prepared || Owner->Removed || Owner->Faulted ||
            Owner->Token != Token || !Owner->HciEpoch) {
            Status = STATUS_DEVICE_REMOVED;
            Now = BtifNowMs();
        } else if (Owner->RadioReady && Owner->Radio.Confirmed &&
                   !Owner->Fifo.Count && !Owner->Mode.Link.TxLength &&
                   !Owner->Mode.Link.AckPending && !Owner->Mode.Link.OfferedLength) {
            Status = STATUS_SUCCESS;
            Now = BtifNowMs();
        } else {
            Status = Owner->Conn.Access(Owner->Conn.Header.Context, Token,
                                        BtifServiceFifo, &Io);
            Now = BtifNowMs();
            if (Status == STATUS_SUCCESS && Owner->RadioReady && Owner->Radio.Confirmed &&
                !Owner->Fifo.Count && !Owner->Mode.Link.TxLength &&
                !Owner->Mode.Link.AckPending && !Owner->Mode.Link.OfferedLength) {
                Status = STATUS_SUCCESS;
            } else if (Status == STATUS_SUCCESS && Owner->Radio.Pending) {
                Expiry = MtkWmtFullControlExpire(&Owner->Radio, Now);
                Status = Expiry == MtkWmtPending ? STATUS_PENDING : STATUS_IO_TIMEOUT;
            } else if (Status == STATUS_SUCCESS) {
                Status = STATUS_PENDING;
            }
        }
        WdfWaitLockRelease(Owner->Lock);
        if (Status != STATUS_PENDING) return ExactStatus(Status);
        if (Now >= Started && Now - Started >= MTK_WMT_FULL_CONTROL_TIMEOUT_MS)
            return STATUS_IO_TIMEOUT;
        LARGE_INTEGER Delay = { .QuadPart = -10000 };
        NTSTATUS Wait = KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        if (Wait != STATUS_SUCCESS) return ExactStatus(Wait);
    }
}

static NTSTATUS BtifStartMode(BTIF_OWNER *Owner, ULONGLONG Token, ULONGLONG Session)
{
    NTSTATUS Status;
    do {
        WdfWaitLockAcquire(Owner->Lock, NULL);
        if (!Owner->Prepared || Owner->Removed || Owner->Token != Token) {
            Status = STATUS_DEVICE_REMOVED;
        } else {
            T70_CONN_BTIF_INTERFACE *Conn = &Owner->Conn;
            BTIF_INTERRUPT_IO Io = { .Owner = Owner, .Negotiating = TRUE };
            Status = Conn->Access(Conn->Header.Context, Token, BtifServiceFifo, &Io);
            if (Status == STATUS_SUCCESS) {
                Owner->ModeSession = Session;
            }
        }
        WdfWaitLockRelease(Owner->Lock);
        if (Status == STATUS_PENDING) {
            // D0 entry runs before interrupt delivery. Poll without either lock held.
            LARGE_INTEGER Delay = { .QuadPart = -10000 };
            NTSTATUS Wait = KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            if (Wait != STATUS_SUCCESS) {
                Status = ExactStatus(Wait);
            }
        }
    } while (Status == STATUS_PENDING);

    if (Status != STATUS_SUCCESS) {
        WdfWaitLockAcquire(Owner->Lock, NULL);
        if (Owner->Token == Token) {
            Owner->Faulted = TRUE;
            BtifEndLease(Owner, FALSE);
        }
        WdfWaitLockRelease(Owner->Lock);
    }
    return ExactStatus(Status);
}

BOOLEAN BtifInterrupt(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);
    BTIF_OWNER *Owner = BtifContext(WdfInterruptGetDevice(Interrupt));
    WdfWaitLockAcquire(Owner->Lock, NULL);
    if (!Owner->Prepared || Owner->Removed || Owner->Faulted || !Owner->Token || !Owner->ModeSession) {
        WdfWaitLockRelease(Owner->Lock);
        return FALSE;
    }
    BTIF_INTERRUPT_IO Io = { .Owner = Owner };
    T70_CONN_BTIF_INTERFACE *Conn = &Owner->Conn;
    NTSTATUS Status = Conn->Access(Conn->Header.Context, Owner->Token, BtifServiceFifo, &Io);
    if (!NT_SUCCESS(Status)) {
        Owner->Faulted = TRUE;
        (void)Conn->Access(Conn->Header.Context, Owner->Token, BtifMaskFifoInterrupts, Owner);
        WdfInterruptQueueWorkItemForIsr(Interrupt);
    }
    WdfWaitLockRelease(Owner->Lock);

    if (Status == STATUS_SUCCESS) {
        if (Io.EventsAvailable) BtifCompleteReadQueue(Owner, HciPacketEvent);
        if (Io.AclAvailable) BtifCompleteReadQueue(Owner, HciPacketAclData);
        if (Io.HciWritten) BtifCompleteWrite(Owner, Io.HciWritten, STATUS_SUCCESS);
        if (Owner->RxPaused) (void)BtifResumeRx(Owner);
    } else {
        BtifCompleteWrite(Owner, 0, Status);
    }
    // At the budget boundary a level-triggered source interrupts again.
    return Io.Service.Handled != 0;
}

VOID BtifInterruptFailure(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);
    WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
    BTIF_OWNER *Owner = BtifContext(Device);
    WdfWaitLockAcquire(Owner->Lock, NULL);
    BOOLEAN Faulted = Owner->Prepared && !Owner->Removed && Owner->Faulted;
    WdfWaitLockRelease(Owner->Lock);
    if (Faulted) {
        WdfDeviceSetFailed(Device, WdfDeviceFailedNoRestart);
    }
}

NTSTATUS BtifQueryRelations(WDFDEVICE Device, PIRP Irp)
{
    if (IoGetCurrentIrpStackLocation(Irp)->Parameters.QueryDeviceRelations.Type == PowerRelations) {
        BTIF_OWNER *Owner = BtifContext(Device);
        WdfWaitLockAcquire(Owner->Lock, NULL);
        PDEVICE_RELATIONS Old = (PDEVICE_RELATIONS)Irp->IoStatus.Information;
        ULONG Count = Old ? Old->Count : 0;
        PDEVICE_OBJECT Provider = Owner->Prepared && !Owner->Removed ? Owner->Provider : NULL;
        for (ULONG Index = 0; Index < Count && Provider; ++Index) {
            if (Old->Objects[Index] == Provider) {
                Provider = NULL;
            }
        }
        if (Provider && Count < MAXULONG) {
            SIZE_T Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + ((SIZE_T)Count + 1) * sizeof(Provider);
            PDEVICE_RELATIONS Next = ExAllocatePool2(POOL_FLAG_PAGED, Size, 'rftB');
            if (Next) {
                Next->Count = Count + 1;
                if (Old) {
                    RtlCopyMemory(Next->Objects, Old->Objects, Count * sizeof(Provider));
                    ExFreePool(Old);
                }
                ObReferenceObject(Provider);
                Next->Objects[Count] = Provider;
                Irp->IoStatus.Information = (ULONG_PTR)Next;
                Irp->IoStatus.Status = STATUS_SUCCESS;
            }
        }
        WdfWaitLockRelease(Owner->Lock);
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}


static BOOLEAN BtifIsBthxIoctl(ULONG Code)
{
    return Code == IOCTL_BTHX_GET_VERSION || Code == IOCTL_BTHX_SET_VERSION ||
           Code == IOCTL_BTHX_QUERY_CAPABILITIES || Code == IOCTL_BTHX_READ_HCI ||
           Code == IOCTL_BTHX_WRITE_HCI;
}

VOID BtifChildIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                    size_t InputLength, ULONG Code)
{
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    if (BtifIsBthxIoctl(Code)) {
        WDFDEVICE Child = WdfIoQueueGetDevice(Queue);
        WDFDEVICE Parent = WdfPdoGetParent(Child);
        WDF_REQUEST_FORWARD_OPTIONS Options;
        WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
        Options.Flags = WDF_REQUEST_FORWARD_OPTION_SEND_AND_FORGET;
        Status = WdfRequestForwardToParentDeviceIoQueue(
                   Request, WdfDeviceGetDefaultQueue(Parent), &Options);
    }
    if (!NT_SUCCESS(Status)) WdfRequestComplete(Request, Status);
}

typedef struct {
    BTIF_OWNER *Owner;
    ULONG Code;
    const VOID *Input;
    SIZE_T InputBytes;
    VOID *Output;
    SIZE_T OutputBytes;
    SIZE_T Returned;
} BTIF_BTHX_ACCESS;

static NTSTATUS NTAPI BtifBthxAccess(PVOID Context)
{
    BTIF_BTHX_ACCESS *Operation = Context;
    BTIF_OWNER *Owner = Operation->Owner;
    Owner->Hci.NowMs = BtifNowMs();
    return T70BtBthxRequest(&Owner->Hci, Owner->HciEpoch, Operation->Code,
                            Operation->Input, Operation->InputBytes,
                            Operation->Output, Operation->OutputBytes,
                            &Operation->Returned, FALSE);
}

VOID BtifReadCanceledOnQueue(WDFQUEUE Queue, WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Queue);
    WdfRequestComplete(Request, STATUS_CANCELLED);
}

VOID BtifWriteCanceled(WDFQUEUE Queue, WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Queue);
    // The accepted STP frame remains ordered in the shared stream. Let it
    // drain; canceling its bookkeeping here could bind its completion to a
    // later request and this queue callback can run above PASSIVE_LEVEL.
    WdfRequestComplete(Request, STATUS_CANCELLED);
}

static VOID BtifFailQueuedRequests(BTIF_OWNER *Owner, NTSTATUS Status)
{
    WDFQUEUE Queues[] = {Owner->ReadEventQueue, Owner->ReadAclQueue, Owner->WriteQueue};
    for (ULONG Index = 0; Index < RTL_NUMBER_OF(Queues); ++Index) {
        WDFREQUEST Request;
        while (WdfIoQueueRetrieveNextRequest(Queues[Index], &Request) == STATUS_SUCCESS)
            WdfRequestComplete(Request, Status);
    }
}

static VOID BtifCompleteReadQueue(BTIF_OWNER *Owner, UCHAR Type)
{
    WDFQUEUE Queue = Type == HciPacketEvent ? Owner->ReadEventQueue : Owner->ReadAclQueue;
    for (;;) {
        WDFREQUEST Request;
        if (WdfIoQueueRetrieveNextRequest(Queue, &Request) != STATUS_SUCCESS) return;

        PVOID Input = NULL, Output = NULL;
        size_t InputBytes = 0, OutputBytes = 0;
        NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(UCHAR), &Input, &InputBytes);
        if (NT_SUCCESS(Status))
            Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_HCI_READ_WRITE_CONTEXT),
                                                    &Output, &OutputBytes);
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
            continue;
        }
        /* This path completes synchronously. Once removed from the manual
         * queue, it is owned here and is no longer cancelable by a WDF callback. */
        SIZE_T Returned = 0;
        WdfWaitLockAcquire(Owner->Lock, NULL);
        if (Owner->HciEpoch != 0) {
            Status = T70BtBthxRequest(&Owner->Hci, Owner->HciEpoch, IOCTL_BTHX_READ_HCI,
                                      Input, InputBytes, Output, OutputBytes, &Returned, FALSE);
        } else {
            Status = STATUS_DEVICE_NOT_CONNECTED;
        }
        WdfWaitLockRelease(Owner->Lock);
        if (Status == STATUS_NO_MORE_ENTRIES) {
            Status = WdfRequestForwardToIoQueue(Request, Queue);
            if (!NT_SUCCESS(Status)) WdfRequestComplete(Request, Status);
            return;
        }
        WdfRequestCompleteWithInformation(Request, Status, NT_SUCCESS(Status) ? Returned : 0);
    }
}

static VOID BtifCompleteWrite(BTIF_OWNER *Owner, ULONG Written, NTSTATUS TransferStatus)
{
    WDFREQUEST Request;
    NTSTATUS Status;
    if (WdfIoQueueRetrieveNextRequest(Owner->WriteQueue, &Request) != STATUS_SUCCESS) return;
    if (TransferStatus == STATUS_SUCCESS && Written != 0) {
        PULONG BytesWritten;
        size_t Length;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*BytesWritten),
                                                (PVOID *)&BytesWritten, &Length);
        if (NT_SUCCESS(Status)) {
            *BytesWritten = Written;
            Status = STATUS_SUCCESS;
        }
    } else {
        Status = TransferStatus == STATUS_SUCCESS ? STATUS_DEVICE_DATA_ERROR : TransferStatus;
    }
    WdfRequestCompleteWithInformation(Request, Status, NT_SUCCESS(Status) ? sizeof(ULONG) : 0);
}

VOID BtifParentIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                     size_t InputLength, ULONG Code)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    BTIF_OWNER *Owner = BtifContext(Device);
    PVOID Input = NULL, Output = NULL;
    SIZE_T InputBytes = 0, OutputBytes = 0, Returned = 0;
    NTSTATUS Status;
    BOOLEAN Forwarded = FALSE;
    if (!BtifIsBthxIoctl(Code)) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    if (InputLength != 0) {
        Status = WdfRequestRetrieveInputBuffer(Request, InputLength, &Input, &InputBytes);
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
            return;
        }
    }
    if (OutputLength != 0) {
        Status = WdfRequestRetrieveOutputBuffer(Request, OutputLength, &Output, &OutputBytes);
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
            return;
        }
    }

    WdfWaitLockAcquire(Owner->Lock, NULL);
    if (Code == IOCTL_BTHX_GET_VERSION || Code == IOCTL_BTHX_SET_VERSION ||
        Code == IOCTL_BTHX_QUERY_CAPABILITIES) {
        Status = T70BtBthxRequest(&Owner->Hci, Owner->HciEpoch, Code,
                                  Input, InputBytes, Output, OutputBytes, &Returned, FALSE);
    } else if (Owner->Removed || Owner->Faulted || Owner->HciEpoch == 0 || !Owner->Token) {
        Status = STATUS_DEVICE_NOT_CONNECTED;
    } else if (Code == IOCTL_BTHX_READ_HCI) {
        Status = T70BtBthxRequest(&Owner->Hci, Owner->HciEpoch, Code,
                                  Input, InputBytes, Output, OutputBytes, &Returned, FALSE);
        if (Status == STATUS_NO_MORE_ENTRIES && Input && InputBytes >= sizeof(UCHAR)) {
            UCHAR Type = *(const UCHAR *)Input;
            WDFQUEUE ReadQueue = Type == HciPacketEvent ? Owner->ReadEventQueue :
                                 Type == HciPacketAclData ? Owner->ReadAclQueue : NULL;
            if (ReadQueue) {
                Status = WdfRequestForwardToIoQueue(Request, ReadQueue);
                Forwarded = NT_SUCCESS(Status);
            }
        }
    } else {
        BTIF_BTHX_ACCESS Operation = {Owner, Code, Input, InputBytes,
                                      Output, OutputBytes, 0};
        NTSTATUS AccessStatus = Owner->Conn.Access(Owner->Conn.Header.Context, Owner->Token,
                                                   BtifBthxAccess, &Operation);
        Status = AccessStatus == STATUS_PENDING ? STATUS_PENDING : ExactStatus(AccessStatus);
        Returned = Operation.Returned;
        if (Status == STATUS_PENDING) {
            Status = WdfRequestForwardToIoQueue(Request, Owner->WriteQueue);
            Forwarded = NT_SUCCESS(Status);
            if (!Forwarded) (void)T70BtBthxCancelWrite(&Owner->Hci, Owner->HciEpoch);
        }
    }
    WdfWaitLockRelease(Owner->Lock);
    if (Forwarded) return;
    WdfRequestCompleteWithInformation(Request, Status, NT_SUCCESS(Status) ? Returned : 0);
    if (Code == IOCTL_BTHX_READ_HCI && NT_SUCCESS(Status) && Owner->RxPaused)
        (void)BtifResumeRx(Owner);
}


static NTSTATUS BtifCreateBthxChild(WDFDEVICE Parent)
{
    static const WCHAR HardwareIdBuffer[] = L"T70BTHX\\HCI";
    static const WCHAR CompatibleIdBuffer[] = L"MS_BTHX_BTHMINI";
    static const WCHAR InstanceIdBuffer[] = L"0";
    BTIF_OWNER *Owner = BtifContext(Parent);
    PWDFDEVICE_INIT Init = WdfPdoInitAllocate(Parent);
    if (!Init) return STATUS_INSUFFICIENT_RESOURCES;

    UNICODE_STRING HardwareId, CompatibleId, InstanceId;
    RtlInitUnicodeString(&HardwareId, HardwareIdBuffer);
    RtlInitUnicodeString(&CompatibleId, CompatibleIdBuffer);
    RtlInitUnicodeString(&InstanceId, InstanceIdBuffer);
    NTSTATUS Status = WdfPdoInitAssignDeviceID(Init, &HardwareId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddHardwareID(Init, &HardwareId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddCompatibleID(Init, &CompatibleId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAssignInstanceID(Init, &InstanceId);
    if (NT_SUCCESS(Status)) WdfPdoInitAllowForwardingRequestToParent(Init);
    if (!NT_SUCCESS(Status)) {
        WdfDeviceInitFree(Init);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    WDFDEVICE Child = NULL;
    Status = WdfDeviceCreate(&Init, &Attributes, &Child);
    if (!NT_SUCCESS(Status)) {
        if (Init) WdfDeviceInitFree(Init);
        return Status;
    }

    WDF_DEVICE_PNP_CAPABILITIES Capabilities;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&Capabilities);
    Capabilities.Removable = WdfFalse;
    Capabilities.SurpriseRemovalOK = WdfTrue;
    Capabilities.Address = 0;
    Capabilities.UINumber = 0;
    WdfDeviceSetPnpCapabilities(Child, &Capabilities);

    WDF_IO_QUEUE_CONFIG Queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchParallel);
    Queue.PowerManaged = WdfFalse;
    Queue.EvtIoDeviceControl = BtifChildIoctl;
    WDFQUEUE ChildQueue = NULL;
    Status = WdfIoQueueCreate(Child, &Queue, WDF_NO_OBJECT_ATTRIBUTES, &ChildQueue);
    if (NT_SUCCESS(Status)) Status = WdfFdoAddStaticChild(Parent, Child);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(Child);
        return Status;
    }
    Owner->BthxChild = Child;
    return STATUS_SUCCESS;
}

NTSTATUS BtifDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS Callbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtDevicePrepareHardware = BtifPrepare;
    Callbacks.EvtDeviceReleaseHardware = BtifRelease;
    Callbacks.EvtDeviceD0Entry = BtifD0Entry;
    Callbacks.EvtDeviceD0Exit = BtifD0Exit;
    Callbacks.EvtDeviceSurpriseRemoval = BtifSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Callbacks);
    UCHAR Minor = IRP_MN_QUERY_DEVICE_RELATIONS;
    NTSTATUS Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(Init, BtifQueryRelations, IRP_MJ_PNP, &Minor, 1);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, BTIF_OWNER);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDFDEVICE Device;
    Status = WdfDeviceCreate(&Init, &Attributes, &Device);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    BTIF_OWNER *Owner = BtifContext(Device);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfWaitLockCreate(&Attributes, &Owner->Lock);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    const MTK_BTIF_IO Io = {Owner, BtifRead, BtifWrite, BtifBarrier};
    Status = BtifCoreStatus(MtkBtifInit(&Owner->Fifo, &Io));
    if (!NT_SUCCESS(Status)) return Status;
    WDF_IO_QUEUE_CONFIG Queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchParallel);
    Queue.PowerManaged = WdfFalse;
    Queue.EvtIoDeviceControl = BtifParentIoctl;
    Status = WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, &Owner->ParentQueue);
    if (!NT_SUCCESS(Status)) return ExactStatus(Status);
    WDF_IO_QUEUE_CONFIG_INIT(&Queue, WdfIoQueueDispatchManual);
    Queue.PowerManaged = WdfFalse;
    Queue.EvtIoCanceledOnQueue = BtifReadCanceledOnQueue;
    Status = WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, &Owner->ReadEventQueue);
    if (NT_SUCCESS(Status)) Status = WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, &Owner->ReadAclQueue);
    if (!NT_SUCCESS(Status)) return ExactStatus(Status);
    Queue.EvtIoCanceledOnQueue = BtifWriteCanceled;
    Status = WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, &Owner->WriteQueue);
    if (!NT_SUCCESS(Status)) return ExactStatus(Status);
    return ExactStatus(BtifCreateBthxChild(Device));
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, BtifDeviceAdd);
    return WdfDriverCreate(Driver, Path, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
