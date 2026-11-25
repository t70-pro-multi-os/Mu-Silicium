// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../Driver.c"



#define CHECK(X) do { ++Checks; if (!(X)) { fprintf(stderr, "%d: %s\n", __LINE__, #X); abort(); } } while (0)
#define DEVICE ((WDFDEVICE)(uintptr_t)1)
#define LOCK ((WDFWAITLOCK)(uintptr_t)2)
#define TARGET ((WDFIOTARGET)(uintptr_t)3)
#define IRQ ((WDFINTERRUPT)(uintptr_t)4)
#define PROVIDER ((PDEVICE_OBJECT)(uintptr_t)5)
#define PDO ((PDEVICE_OBJECT)(uintptr_t)6)
#define CHILD ((WDFDEVICE)(uintptr_t)7)
#define PDO_INIT ((PWDFDEVICE_INIT)(uintptr_t)8)
#define PARENT_QUEUE ((WDFQUEUE)(uintptr_t)9)
#define CHILD_QUEUE ((WDFQUEUE)(uintptr_t)10)

typedef struct {
    WDFREQUEST Request;
    PVOID Input, Output;
    size_t InputBytes, OutputBytes;
} TEST_REQUEST_BUFFERS;

static unsigned Checks;
static BTIF_OWNER Owner;
static MTK_STP_FULL_LINK TestPeer;
static BOOLEAN TestPeerStarted;
static struct { ULONG Count; CM_PARTIAL_RESOURCE_DESCRIPTOR R[6]; } Lists[2];
static TEST_REQUEST_BUFFERS RequestBuffers[8];
static struct {
    unsigned Maps, Unmaps, FailMap, NullIndex, Reads, Writes, Barriers;
    unsigned Ref, PdoRef, Opens, Closes, Deletes, Interrupts, Work, Failed, Relations;
    unsigned Acquires, Access, Releases, Quiesced, Lock, Guard, BarrierPending;
    unsigned Paths, BadInterface, RemoveDuringQuery, Lost, RejectStartWrite;
    unsigned TxUsed, TxFrames, AckNext;
    unsigned Silent, RejectMode, DelayAck, RemoveDuringWait;
    unsigned PdoAllocations, PdoIds, QueueCreates, ChildAdds, ForwardRequests, Completions, DeviceInitFrees;
    unsigned CancelMarks, CancelUnmarks;
    unsigned DataFrames, RadioFrames, DataChannel, DataPayloadBytes;
    WDFREQUEST PendingRequests[3];
    NTSTATUS CompletionStatuses[16];
    ULONG_PTR CompletionInfo[16];
    NTSTATUS CreateStatus, ChildCreateStatus, DriverStatus, AcquireStatus, IrqStatus;
    ULONGLONG Token, NextToken, Session, NowMs, AckAt;
    ULONG Regs[3][0x1000 / 4];
    unsigned RxHead, RxTail;
    UCHAR Rx[1024], Tx[64];
} S;
static WDF_PNPPOWER_EVENT_CALLBACKS Callbacks;
static WDF_DRIVER_CONFIG DriverConfig;
static WDF_INTERRUPT_CONFIG IrqConfig;
static WDF_IO_TARGET_OPEN_PARAMS TargetConfig;

static TEST_REQUEST_BUFFERS *BuffersFor(WDFREQUEST Request)
{
    for (unsigned Index = 0; Index < RTL_NUMBER_OF(RequestBuffers); ++Index)
        if (RequestBuffers[Index].Request == Request) return &RequestBuffers[Index];
    return NULL;
}

static void MapRequestBuffers(WDFREQUEST Request, PVOID Input, size_t InputBytes,
                              PVOID Output, size_t OutputBytes)
{
    for (unsigned Index = 0; Index < RTL_NUMBER_OF(RequestBuffers); ++Index) {
        if (!RequestBuffers[Index].Request || RequestBuffers[Index].Request == Request) {
            RequestBuffers[Index] = (TEST_REQUEST_BUFFERS){
                Request, Input, Output, InputBytes, OutputBytes
            };
            return;
        }
    }
    abort();
}


PVOID TestWdfContext(WDFOBJECT Device)
{
    CHECK(Device == DEVICE);
    return &Owner;
}

static int MatchesUnicode(PUNICODE_STRING Value, const WCHAR *Text, USHORT Bytes)
{
    const UNICODE_STRING *String = (const UNICODE_STRING *)Value;
    return String && String->Length == Bytes && memcmp(String->Buffer, Text, Bytes) == 0;
}

ULONG WdfCmResourceListGetCount(WDFCMRESLIST List)
{
    CHECK(List == &Lists[0] || List == &Lists[1]);
    return List == &Lists[0] ? Lists[0].Count : Lists[1].Count;
}

PCM_PARTIAL_RESOURCE_DESCRIPTOR WdfCmResourceListGetDescriptor(WDFCMRESLIST List, ULONG Index)
{
    CHECK(List == &Lists[0] || List == &Lists[1]);
    CHECK(Index < 6);
    return S.NullIndex == Index + 1 ? NULL : &Lists[List == &Lists[0] ? 0 : 1].R[Index];
}

PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS Address, SIZE_T Bytes, ULONG Protection)
{
    unsigned Index = S.Maps++;
    CHECK(Index < 3 && (ULONGLONG)Address.QuadPart == BtifBase[Index] && Bytes == BtifLength[Index]);
    CHECK(Protection == (PAGE_READWRITE | PAGE_NOCACHE));
    return S.FailMap == Index + 1 ? NULL : S.Regs[Index];
}

VOID MmUnmapIoSpace(PVOID Address, SIZE_T Bytes)
{
    unsigned Index = S.Maps - S.Unmaps++ - (S.FailMap != 0 ? 2 : 1);
    CHECK(Index < 3 && Address == S.Regs[Index] && Bytes == BtifLength[Index] && !S.Guard && !S.Token);
}

static unsigned Register(const volatile void *Address, unsigned *Region)
{
    CHECK(S.Lock && S.Guard && S.Token && !S.Lost && !S.BarrierPending);
    for (*Region = 0; *Region < 3; ++*Region) {
        uintptr_t Offset = (uintptr_t)Address - (uintptr_t)S.Regs[*Region];
        if (Offset < BtifLength[*Region]) {
            CHECK(Owner.Mapping[*Region] == S.Regs[*Region]);
            return (unsigned)Offset;
        }
    }
    abort();
}

ULONG READ_REGISTER_ULONG(volatile ULONG *Address)
{
    unsigned Region, Offset = Register(Address, &Region);
    CHECK((Offset & 3) == 0 && (Region == 0 || Offset == 8));
    S.Reads++;
    if (Region == 0 && Offset == 8) {
        return (S.RxHead == S.RxTail ? 0 : 4U) | (S.Regs[0][1] & 2U);
    }
    return S.Regs[Region][Offset / 4];
}

VOID WRITE_REGISTER_ULONG(volatile ULONG *Address, ULONG Value)
{
    unsigned Region, Offset = Register(Address, &Region);
    CHECK(Region == 0 && Offset != 0 && (Offset & 3) == 0);
    S.Writes++;
    S.BarrierPending = 1;
    if (S.RejectStartWrite && Offset == 0x6c) {
        S.RejectStartWrite--;
    } else {
        S.Regs[0][Offset / 4] = Value;
    }
}

UCHAR READ_REGISTER_UCHAR(volatile UCHAR *Address)
{
    unsigned Region, Offset = Register(Address, &Region);
    CHECK(!Region && !Offset && S.RxHead < S.RxTail);
    S.Reads++;
    return S.Rx[S.RxHead++];
}

static void ReceiveBytes(const UCHAR *Bytes, unsigned Length)
{
    CHECK(S.RxTail + Length <= sizeof(S.Rx));
    memcpy(S.Rx + S.RxTail, Bytes, Length);
    S.RxTail += Length;
}

static MTK_STP_STATUS WritePeer(void *Context, const UCHAR *Bytes, unsigned Length, unsigned *Written)
{
    CHECK(!Context);
    ReceiveBytes(Bytes, Length);
    *Written = Length;
    return MtkStpOk;
}

VOID WRITE_REGISTER_UCHAR(volatile UCHAR *Address, UCHAR Value)
{
    static const struct {
        UCHAR Command[15], Reply[17];
        unsigned CommandLength, ReplyLength;
    } Exchange[] = {
        {{0x80,0x40,5,0,1,4,1,0,4,0,0},
         {0x80,0x40,10,0,2,4,6,0,0,4,0x11,0,0,0,0,0}, 11, 16},
        {{0x80,0x40,9,0,1,4,5,0,3,0xdb,0x0e,0x68,1,0,0},
         {0x80,0x40,6,0,2,4,2,0,0,3,0,0}, 15, 12},
        {{0x87,0x40,5,0xcc,1,4,1,0,4,0x6c,0xf3},
         {0x80,0x40,10,0xca,2,4,6,0,0,4,0xdb,0x0e,0x68,1,0x57,0xc7}, 11, 16}
    };
    unsigned Region, Offset = Register(Address, &Region);
    CHECK(!Region && !Offset && S.TxUsed < sizeof(S.Tx));
    ++S.Writes;
    S.BarrierPending = 1;
    S.Tx[S.TxUsed++] = Value;

    unsigned Step = S.TxFrames;
    if (Step < RTL_NUMBER_OF(Exchange)) {
        if (S.TxUsed < Exchange[Step].CommandLength) {
            return;
        }
        if (memcmp(S.Tx, Exchange[Step].Command, S.TxUsed)) {
            fprintf(stderr, "WMT mismatch step=%u used=%u expected=%u bytes=", Step,
                    S.TxUsed, Exchange[Step].CommandLength);
            for (unsigned I = 0; I < S.TxUsed; ++I) fprintf(stderr, "%02x", S.Tx[I]);
            fprintf(stderr, "\n");
            abort();
        }
        if (Step == 2) {
            CHECK(S.NowMs >= MTK_WMT_MODE_SETTLE_MS);
        }
        if (!S.Silent) {
            UCHAR Reply[sizeof(Exchange[Step].Reply)];
            memcpy(Reply, Exchange[Step].Reply, sizeof(Reply));
            if (Step == 1 && S.RejectMode) {
                Reply[8] = 1;
            }
            if (Step == 2 && S.DelayAck) {
                Reply[0] = 0x87;
                Reply[3] = 0xd1;
                S.AckAt = S.NowMs + 25;
            }
            ReceiveBytes(Reply, Exchange[Step].ReplyLength);
        }
        ++S.TxFrames;
        S.TxUsed = 0;
    } else if (S.TxUsed >= 4 && S.Tx[0] == (UCHAR)(0x80U | S.AckNext) &&
               S.Tx[1] == 0 && S.Tx[2] == 0 && S.Tx[3] == S.Tx[0]) {
        CHECK(S.TxUsed == 4);
        S.AckNext = (S.AckNext + 1U) & 7U;
        ++S.TxFrames;
        S.TxUsed = 0;
    } else {
        unsigned Prefix = S.TxUsed >= 4 && S.Tx[0] == 0x7f && S.Tx[1] == 0x7f &&
                          S.Tx[2] == 0x7f && S.Tx[3] == 0x7f ? 4U : 0U;
        if (S.TxUsed < Prefix + 4U) return;
        unsigned Header = Prefix;
        unsigned Payload = ((unsigned)(S.Tx[Header + 1U] & 15U) << 8) |
                           S.Tx[Header + 2U];
        unsigned FrameBytes = Prefix + Payload + 6U;
        if (S.TxUsed < FrameBytes) return;
        CHECK(S.TxUsed == FrameBytes && (S.Tx[Header] & 0x80U) &&
              S.Tx[Header + 3U] == (UCHAR)(S.Tx[Header] + S.Tx[Header + 1U] +
                                             S.Tx[Header + 2U]));
        unsigned Channel = (S.Tx[Header + 1U] >> 4) & 7U;
        if (Channel == MTK_STP_WMT_CHANNEL && Owner.Radio.Pending) {
            static const UCHAR Reply[] = {2, 6, 1, 0, 0};
            MTK_STP_FRAME Wmt;
            unsigned Consumed = 0;
            if (!TestPeerStarted) {
                CHECK(MtkStpFullInit(&TestPeer, NULL, WritePeer) == MtkStpOk);
                TestPeer.TxNext = Owner.Mode.Link.RxNext;
                TestPeer.RxNext = Owner.Mode.Link.TxNext;
                TestPeerStarted = TRUE;
            }
            CHECK(MtkStpFullReceive(&TestPeer, S.Tx, S.TxUsed, &Consumed, &Wmt) ==
                  MtkStpFrameReady && Consumed == S.TxUsed && Wmt.Channel == MTK_STP_WMT_CHANNEL &&
                  Wmt.Length == 6 && Wmt.Data[0] == 1 && Wmt.Data[1] == 6 &&
                  Wmt.Data[4] == T70_WMT_FUNCTION_BT && Wmt.Data[5] == 1);
            CHECK(MtkStpFullAccept(&TestPeer) == MtkStpOk);
            CHECK(MtkStpFullService(&TestPeer, S.NowMs) == MtkStpOk);
            CHECK(MtkStpFullTransmit(&TestPeer, MTK_STP_WMT_CHANNEL, Reply,
                                    sizeof(Reply), S.NowMs) == MtkStpOk);
            S.RadioFrames++;
        } else if (Channel == T70_BT_STP_CHANNEL) {
            S.DataFrames++;
            S.DataChannel = Channel;
            S.DataPayloadBytes = Payload;
        }
        S.TxUsed = 0;
    }
}

VOID TestDsb(unsigned Barrier)
{
    CHECK(Barrier == 15 && S.Guard && S.Lock);
    if (S.BarrierPending) {
        ++S.Barriers;
    } else {
        CHECK(Owner.Fifo.Count);
    }
    S.BarrierPending = 0;
}

ULONGLONG KeQueryInterruptTimePrecise(ULONGLONG *Counter)
{
    CHECK(Counter && S.Lock);
    *Counter = 0;
    return S.NowMs * 10000;
}

NTSTATUS KeDelayExecutionThread(int Mode, BOOLEAN Alertable, const LARGE_INTEGER *Delay)
{
    CHECK(!S.Lock && !S.Guard && Mode == KernelMode && !Alertable && Delay->QuadPart == -10000);
    ++S.NowMs;
    if (S.AckAt && S.NowMs == S.AckAt) {
        static const UCHAR Ack[] = {0x80,0,0,0x80};
        CHECK(S.TxFrames == 4 && !Owner.ModeSession && Owner.Mode.Link.TxLength);
        ReceiveBytes(Ack, sizeof(Ack));
    }
    if (S.RemoveDuringWait) {
        S.RemoveDuringWait = 0;
        S.Lost = 1;
        TargetConfig.EvtIoTargetRemoveComplete(TARGET);
    }
    return STATUS_SUCCESS;
}

VOID RtlCopyMemory(PVOID To, const void *From, SIZE_T Bytes) { memcpy(To, From, Bytes); }
PVOID RtlSecureZeroMemory(PVOID Memory, SIZE_T Bytes) { memset(Memory, 0, Bytes); return Memory; }
PVOID ExAllocatePool2(ULONGLONG Flags, SIZE_T Bytes, ULONG Tag)
{
    CHECK(Flags == POOL_FLAG_PAGED && Tag != 0);
    return calloc(1, Bytes);
}
VOID ExFreePool(PVOID Memory) { CHECK(Memory); free(Memory); }
VOID ObReferenceObject(PDEVICE_OBJECT Object) { CHECK(Object == PROVIDER); ++S.PdoRef; }
VOID ObDereferenceObject(PDEVICE_OBJECT Object) { CHECK(Object == PROVIDER && S.PdoRef); --S.PdoRef; }

static VOID RefConn(PVOID Context) { CHECK(Context == PROVIDER); ++S.Ref; }
static VOID DerefConn(PVOID Context) { CHECK(Context == PROVIDER && S.Ref && !S.Lock && !S.Token); --S.Ref; }

static NTSTATUS AcquireConn(PVOID Context, PULONGLONG Token)
{
    CHECK(Context == PROVIDER && S.Ref && S.Lock && !S.Token);
    ++S.Acquires;
    *Token = 0;
    if (S.AcquireStatus == STATUS_SUCCESS) {
        *Token = S.Token = ++S.NextToken;
    }
    return S.AcquireStatus;
}

static NTSTATUS AccessConn(PVOID Context, ULONGLONG Token, T70_CONN_BTIF_ACCESS *Op, PVOID Arg)
{
    CHECK(Context == PROVIDER && S.Ref && S.Lock && !S.Guard && Token && Token == S.Token);
    ++S.Access;
    if (S.Lost) {
        return STATUS_DEVICE_REMOVED;
    }
    S.Guard = 1;
    NTSTATUS Status = Op(Arg);
    CHECK(!S.BarrierPending);
    S.Guard = 0;
    return Status;
}

static NTSTATUS ReleaseConn(PVOID Context, ULONGLONG Token, BOOLEAN Quiesced)
{
    CHECK(Context == PROVIDER && S.Ref && S.Lock && !S.Guard && Token && Token == S.Token);
    CHECK(!Quiesced || (!S.Lost && S.Regs[0][1] == 0 && !Owner.Fifo.Owned && !Owner.Fifo.Running));
    ++S.Releases;
    S.Quiesced += Quiesced;
    S.Token = 0;
    return S.Lost ? STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
}

static NTSTATUS ClaimConn(PVOID Context, ULONGLONG Token, PULONGLONG Session, BOOLEAN *Fresh)
{
    CHECK(Context == PROVIDER && S.Ref && S.Lock && !S.Guard && Token && Token == S.Token);
    *Fresh = !S.Session;
    if (*Fresh) {
        S.Session = Token;
    }
    *Session = S.Session;
    return S.Lost ? STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
}

NTSTATUS IoGetDeviceInterfaces(const GUID *Class, PDEVICE_OBJECT Pdo, ULONG Flags, PWSTR *Paths)
{
    CHECK(!memcmp(Class, &GUID_DEVINTERFACE_T70_CONN_BTIF, sizeof(*Class)) && !Pdo && !Flags && !S.Lock);
    static const wchar_t Empty[] = L"\0";
    static const wchar_t One[] = L"\\??\\WCN0\0";
    static const wchar_t Two[] = L"\\??\\WCN0\0\\??\\WCN1\0";
    const wchar_t *Data = S.Paths == 1 ? One : S.Paths ? Two : Empty;
    size_t Size = S.Paths == 1 ? sizeof(One) : S.Paths ? sizeof(Two) : sizeof(Empty);
    *Paths = malloc(Size);
    CHECK(*Paths);
    memcpy(*Paths, Data, Size);
    return STATUS_SUCCESS;
}

NTSTATUS WdfIoTargetCreate(WDFDEVICE Device, WDF_OBJECT_ATTRIBUTES *Attrs, WDFIOTARGET *Target)
{
    CHECK(Device == DEVICE && !Attrs && !S.Lock);
    *Target = TARGET;
    return STATUS_SUCCESS;
}

NTSTATUS WdfIoTargetOpen(WDFIOTARGET Target, WDF_IO_TARGET_OPEN_PARAMS *Params)
{
    CHECK(Target == TARGET && Owner.Target == TARGET && !S.Lock);
    CHECK(!wcscmp(Params->Name.Buffer, L"\\??\\WCN0"));
    CHECK(Params->Access == FILE_READ_ATTRIBUTES && Params->ShareAccess == (FILE_SHARE_READ | FILE_SHARE_WRITE));
    CHECK(Params->EvtIoTargetQueryRemove(Target) == STATUS_DEVICE_BUSY && Params->EvtIoTargetRemoveComplete);
    TargetConfig = *Params;
    ++S.Opens;
    return STATUS_SUCCESS;
}

WDFDEVICE WdfIoTargetGetDevice(WDFIOTARGET Target) { CHECK(Target == TARGET); return DEVICE; }

NTSTATUS WdfIoTargetQueryForInterface(WDFIOTARGET Target, const GUID *Id, INTERFACE *Header,
                                    USHORT Size, USHORT Version, void *Data)
{
    CHECK(Target == TARGET && !S.Lock && !Data && !memcmp(Id, &GUID_T70_CONN_BTIF_INTERFACE_V2, sizeof(*Id)));
    CHECK(Size == sizeof(T70_CONN_BTIF_INTERFACE) && Version == T70_CONN_BTIF_INTERFACE_VERSION);
    if (S.RemoveDuringQuery) {
        S.Lost = 1;
        TargetConfig.EvtIoTargetRemoveComplete(TARGET);
    }
    *(T70_CONN_BTIF_INTERFACE *)Header = (T70_CONN_BTIF_INTERFACE){
        {Size, (USHORT)(Version + S.BadInterface), PROVIDER, RefConn, DerefConn},
        AcquireConn, AccessConn, ReleaseConn, ClaimConn
    };
    Header->InterfaceReference(Header->Context);
    return STATUS_SUCCESS;
}

PDEVICE_OBJECT WdfIoTargetWdmGetTargetPhysicalDevice(WDFIOTARGET Target)
{
    CHECK(Target == TARGET && S.Lock && !S.Lost);
    return PROVIDER;
}

VOID WdfIoTargetClose(WDFIOTARGET Target) { CHECK(Target == TARGET && !S.Lock); ++S.Closes; }
VOID WdfObjectDelete(WDFOBJECT Target) { CHECK(Target == TARGET && !S.Lock && !Owner.Target); ++S.Deletes; }
VOID WdfDeviceSetFailed(WDFDEVICE Device, ULONG How)
{
    CHECK(Device == DEVICE && !S.Lock && How == WdfDeviceFailedNoRestart);
    ++S.Failed;
}

PDEVICE_OBJECT WdfDeviceWdmGetPhysicalDevice(WDFDEVICE Device) { CHECK(Device == DEVICE); return PDO; }
VOID IoInvalidateDeviceRelations(PDEVICE_OBJECT Pdo, DEVICE_RELATION_TYPE Type)
{
    CHECK(Pdo == PDO && Type == PowerRelations && !S.Lock);
    ++S.Relations;
}
NTSTATUS WdfDeviceWdmDispatchPreprocessedIrp(WDFDEVICE Device, PIRP Irp)
{
    CHECK(Device == DEVICE && !S.Lock && Irp->Skipped == 1);
    return Irp->IoStatus.Status;
}

VOID WdfWaitLockAcquire(WDFWAITLOCK Lock, const LARGE_INTEGER *Timeout)
{
    CHECK(Lock == LOCK && !Timeout && !S.Lock);
    S.Lock = 1;
}

VOID WdfWaitLockRelease(WDFWAITLOCK Lock)
{
    CHECK(Lock == LOCK && S.Lock && !S.Guard);
    S.Lock = 0;
}

NTSTATUS WdfWaitLockCreate(WDF_OBJECT_ATTRIBUTES *Attrs, WDFWAITLOCK *Lock)
{
    CHECK(Attrs->ParentObject == DEVICE);
    *Lock = LOCK;
    return STATUS_SUCCESS;
}

NTSTATUS WdfInterruptCreate(WDFDEVICE Device, WDF_INTERRUPT_CONFIG *Config,
                            WDF_OBJECT_ATTRIBUTES *Attrs, WDFINTERRUPT *Interrupt)
{
    CHECK(Device == DEVICE && !Attrs && !S.Lock && !S.Token);
    CHECK(Config->PassiveHandling && Config->WaitLock == NULL && Owner.Lock == LOCK && Config->EvtInterruptWorkItem == BtifInterruptFailure);
    CHECK(Config->InterruptRaw == &Lists[0].R[3] && Config->InterruptTranslated == &Lists[1].R[3]);
    CHECK(Config->InterruptRaw->u.Interrupt.Vector == 222 && Config->InterruptTranslated->u.Interrupt.Vector == 0x80);
    IrqConfig = *Config;
    ++S.Interrupts;
    *Interrupt = IRQ;
    return S.IrqStatus;
}

WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT Interrupt) { CHECK(Interrupt == IRQ); return DEVICE; }
BOOLEAN WdfInterruptQueueWorkItemForIsr(WDFINTERRUPT Interrupt)
{
    CHECK(Interrupt == IRQ && S.Lock && S.Work == 0);
    ++S.Work;
    return TRUE;
}

VOID WdfDeviceInitSetPnpPowerEventCallbacks(PWDFDEVICE_INIT Init, WDF_PNPPOWER_EVENT_CALLBACKS *C)
{
    CHECK(Init);
    Callbacks = *C;
}

NTSTATUS WdfDeviceInitAssignWdmIrpPreprocessCallback(PWDFDEVICE_INIT Init,
                  EVT_WDFDEVICE_WDM_IRP_PREPROCESS *Callback, UCHAR Major, UCHAR *Minor, ULONG Count)
{
    CHECK(Init && Callback == BtifQueryRelations && Major == IRP_MJ_PNP);
    CHECK(Count == 1 && *Minor == IRP_MN_QUERY_DEVICE_RELATIONS);
    return STATUS_SUCCESS;
}

NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *Init, WDF_OBJECT_ATTRIBUTES *Attrs, WDFDEVICE *Device)
{
    CHECK(Init && *Init && Attrs && Device);
    if (*Init == (PWDFDEVICE_INIT)1) {
        CHECK(Attrs->ContextSize == sizeof(Owner));
        CHECK(Attrs->ExecutionLevel == WdfExecutionLevelPassive &&
              Attrs->SynchronizationScope == WdfSynchronizationScopeNone);
        *Device = NT_SUCCESS(S.CreateStatus) ? DEVICE : NULL;
        return S.CreateStatus;
    }
    CHECK(*Init == PDO_INIT && Attrs->ContextSize == 0);
    *Device = NT_SUCCESS(S.ChildCreateStatus) ? CHILD : NULL;
    return S.ChildCreateStatus;
}

PWDFDEVICE_INIT WdfPdoInitAllocate(WDFDEVICE Parent)
{
    CHECK(Parent == DEVICE);
    ++S.PdoAllocations;
    return PDO_INIT;
}

NTSTATUS WdfPdoInitAssignDeviceID(PWDFDEVICE_INIT Init, PUNICODE_STRING Id)
{
    static const WCHAR Expected[] = L"T70BTHX\\HCI";
    CHECK(Init == PDO_INIT && MatchesUnicode(Id, Expected, sizeof(Expected)-sizeof(WCHAR)));
    ++S.PdoIds;
    return STATUS_SUCCESS;
}

NTSTATUS WdfPdoInitAddHardwareID(PWDFDEVICE_INIT Init, PUNICODE_STRING Id)
{
    static const WCHAR Expected[] = L"T70BTHX\\HCI";
    CHECK(Init == PDO_INIT && MatchesUnicode(Id, Expected, sizeof(Expected)-sizeof(WCHAR)));
    ++S.PdoIds;
    return STATUS_SUCCESS;
}

NTSTATUS WdfPdoInitAddCompatibleID(PWDFDEVICE_INIT Init, PUNICODE_STRING Id)
{
    static const WCHAR Expected[] = L"MS_BTHX_BTHMINI";
    CHECK(Init == PDO_INIT && MatchesUnicode(Id, Expected, sizeof(Expected)-sizeof(WCHAR)));
    ++S.PdoIds;
    return STATUS_SUCCESS;
}

NTSTATUS WdfPdoInitAssignInstanceID(PWDFDEVICE_INIT Init, PUNICODE_STRING Id)
{
    static const WCHAR Expected[] = L"0";
    CHECK(Init == PDO_INIT && MatchesUnicode(Id, Expected, sizeof(Expected)-sizeof(WCHAR)));
    ++S.PdoIds;
    return STATUS_SUCCESS;
}

VOID WdfPdoInitAllowForwardingRequestToParent(PWDFDEVICE_INIT Init)
{
    CHECK(Init == PDO_INIT);
}

VOID WdfDeviceInitFree(PWDFDEVICE_INIT Init)
{
    CHECK(Init == PDO_INIT);
    ++S.DeviceInitFrees;
}

VOID WdfDeviceSetPnpCapabilities(WDFDEVICE Device, WDF_DEVICE_PNP_CAPABILITIES *Capabilities)
{
    CHECK(Device == CHILD && Capabilities && !Capabilities->Removable &&
          Capabilities->SurpriseRemovalOK);
}

NTSTATUS WdfIoQueueCreate(WDFDEVICE Device, WDF_IO_QUEUE_CONFIG *Config,
                          WDF_OBJECT_ATTRIBUTES *Attributes, WDFQUEUE *Queue)
{
    CHECK(Config && !Attributes && Config->PowerManaged == WdfFalse);
    ++S.QueueCreates;
    if (Device == DEVICE && Config->DispatchType == WdfIoQueueDispatchParallel) {
        CHECK(Queue == &Owner.ParentQueue && Config->EvtIoDeviceControl == BtifParentIoctl);
        *Queue = PARENT_QUEUE;
    } else if (Device == DEVICE && Config->DispatchType == WdfIoQueueDispatchManual) {
        CHECK(Queue != NULL && Config->EvtIoDeviceControl == NULL && !Config->PowerManaged);
        if (Queue == &Owner.ReadEventQueue) {
            CHECK(Config->EvtIoCanceledOnQueue == BtifReadCanceledOnQueue);
            *Queue = (WDFQUEUE)(uintptr_t)12;
        } else if (Queue == &Owner.ReadAclQueue) {
            CHECK(Config->EvtIoCanceledOnQueue == BtifReadCanceledOnQueue);
            *Queue = (WDFQUEUE)(uintptr_t)13;
        } else if (Queue == &Owner.WriteQueue) {
            CHECK(Config->EvtIoCanceledOnQueue == BtifWriteCanceled);
            *Queue = (WDFQUEUE)(uintptr_t)14;
        } else abort();
    } else {
        CHECK(Device == CHILD && Queue && Config->DispatchType == WdfIoQueueDispatchParallel &&
              Config->EvtIoDeviceControl == BtifChildIoctl);
        *Queue = CHILD_QUEUE;
    }
    return STATUS_SUCCESS;
}
NTSTATUS WdfFdoAddStaticChild(WDFDEVICE Parent, WDFDEVICE ChildDevice)
{
    CHECK(Parent == DEVICE && ChildDevice == CHILD);
    ++S.ChildAdds;
    return STATUS_SUCCESS;
}

WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE Queue)
{
    if (Queue == PARENT_QUEUE) return DEVICE;
    CHECK(Queue == CHILD_QUEUE);
    return CHILD;
}

WDFDEVICE WdfPdoGetParent(WDFDEVICE ChildDevice)
{
    CHECK(ChildDevice == CHILD);
    return DEVICE;
}

WDFQUEUE WdfDeviceGetDefaultQueue(WDFDEVICE Device)
{
    CHECK(Device == DEVICE);
    return Owner.ParentQueue;
}

NTSTATUS WdfRequestForwardToParentDeviceIoQueue(WDFREQUEST Request, WDFQUEUE Queue,
                                                WDF_REQUEST_FORWARD_OPTIONS *Options)
{
    CHECK(Request && Queue == PARENT_QUEUE && Options &&
          Options->Flags == WDF_REQUEST_FORWARD_OPTION_SEND_AND_FORGET);
    ++S.ForwardRequests;
    return STATUS_SUCCESS;
}

NTSTATUS WdfRequestForwardToIoQueue(WDFREQUEST Request, WDFQUEUE Queue)
{
    unsigned Index;
    CHECK(Request && (Queue == Owner.ReadEventQueue || Queue == Owner.ReadAclQueue ||
                      Queue == Owner.WriteQueue));
    Index = Queue == Owner.ReadEventQueue ? 0 : Queue == Owner.ReadAclQueue ? 1 : 2;
    if (S.PendingRequests[Index]) return STATUS_DEVICE_BUSY;
    S.PendingRequests[Index] = Request;
    return STATUS_SUCCESS;
}

NTSTATUS WdfIoQueueRetrieveNextRequest(WDFQUEUE Queue, WDFREQUEST *Request)
{
    unsigned Index;
    CHECK(Queue == Owner.ReadEventQueue || Queue == Owner.ReadAclQueue || Queue == Owner.WriteQueue);
    Index = Queue == Owner.ReadEventQueue ? 0 : Queue == Owner.ReadAclQueue ? 1 : 2;
    *Request = S.PendingRequests[Index];
    S.PendingRequests[Index] = NULL;
    return *Request ? STATUS_SUCCESS : STATUS_NO_MORE_ENTRIES;
}

NTSTATUS WdfRequestMarkCancelableEx(WDFREQUEST Request, EVT_WDF_REQUEST_CANCEL *Cancel)
{
    CHECK(Request && Cancel);
    S.CancelMarks++;
    return STATUS_SUCCESS;
}

NTSTATUS WdfRequestUnmarkCancelable(WDFREQUEST Request)
{
    CHECK(Request);
    S.CancelUnmarks++;
    return STATUS_SUCCESS;
}

NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST Request, size_t Minimum, PVOID *Buffer,
                                       size_t *Length)
{
    TEST_REQUEST_BUFFERS *Mapped = BuffersFor(Request);
    CHECK(Request && Buffer && Length);
    if (!Mapped || !Mapped->Input || Mapped->InputBytes < Minimum) return STATUS_BUFFER_TOO_SMALL;
    *Buffer = Mapped->Input;
    *Length = Mapped->InputBytes;
    return STATUS_SUCCESS;
}

NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST Request, size_t Minimum, PVOID *Buffer,
                                        size_t *Length)
{
    TEST_REQUEST_BUFFERS *Mapped = BuffersFor(Request);
    CHECK(Request && Buffer && Length);
    if (!Mapped || !Mapped->Output || Mapped->OutputBytes < Minimum) return STATUS_BUFFER_TOO_SMALL;
    *Buffer = Mapped->Output;
    *Length = Mapped->OutputBytes;
    return STATUS_SUCCESS;
}

VOID WdfRequestComplete(WDFREQUEST Request, NTSTATUS Status)
{
    CHECK(Request != NULL && S.Completions < RTL_NUMBER_OF(S.CompletionStatuses));
    S.CompletionStatuses[S.Completions++] = Status;
}

VOID WdfRequestCompleteWithInformation(WDFREQUEST Request, NTSTATUS Status, ULONG_PTR Information)
{
    unsigned Index = S.Completions;
    CHECK(Request != NULL && Index < RTL_NUMBER_OF(S.CompletionInfo));
    WdfRequestComplete(Request, Status);
    S.CompletionInfo[Index] = Information;
}

NTSTATUS WdfDriverCreate(PDRIVER_OBJECT Driver, PUNICODE_STRING Path, WDF_OBJECT_ATTRIBUTES *Attrs,
                         WDF_DRIVER_CONFIG *Config, WDFDRIVER *Handle)
{
    DriverConfig = *Config;
    CHECK(!Attrs && !Handle);
    return S.DriverStatus;
}

static void Fresh(void)
{
    CHECK(!S.Ref && !S.PdoRef && !S.Lock && !S.Token);
    memset(&Owner, 0, sizeof(Owner));
    memset(&S, 0, sizeof(S));
    memset(&TestPeer, 0, sizeof(TestPeer)); TestPeerStarted = FALSE;
    memset(Lists, 0, sizeof(Lists));
    memset(RequestBuffers, 0, sizeof(RequestBuffers));
    S.Paths = 1;
    S.Regs[0][0x14 / 4] = 0x40;
    S.Regs[0][0x6c / 4] = 0x20;
    S.Regs[0][0x0c / 4] = 0x51;
    for (unsigned Page = 0; Page < 2; ++Page) {
        Lists[Page].Count = 6;
        for (unsigned I = 0; I < 3; ++I) {
            Lists[Page].R[I].Type = CmResourceTypeMemory;
            Lists[Page].R[I].u.Memory.Start.QuadPart = BtifBase[I];
            Lists[Page].R[I].u.Memory.Length = BtifLength[I];
            Lists[Page].R[I + 3].Type = CmResourceTypeInterrupt;
            Lists[Page].R[I + 3].ShareDisposition = CmResourceShareDeviceExclusive;
            Lists[Page].R[I + 3].u.Interrupt.Vector = Page ? 0x80 + I : BtifGsi[I];
            Lists[Page].R[I + 3].u.Interrupt.Level = 5 + I;
            Lists[Page].R[I + 3].u.Interrupt.Affinity = 1ULL << I;
        }
    }
    CHECK(BtifDeviceAdd(NULL, (void *)1) == STATUS_SUCCESS && Owner.Lock == LOCK);
    CHECK(Owner.BthxChild == CHILD && Owner.ParentQueue == PARENT_QUEUE);
    CHECK(Owner.ReadEventQueue == (WDFQUEUE)(uintptr_t)12 &&
          Owner.ReadAclQueue == (WDFQUEUE)(uintptr_t)13 && Owner.WriteQueue == (WDFQUEUE)(uintptr_t)14);
    CHECK(S.PdoAllocations == 1 && S.PdoIds == 4 && S.QueueCreates == 5 && S.ChildAdds == 1);
    BtifChildIoctl(CHILD_QUEUE, (WDFREQUEST)(uintptr_t)11, 0, 0, IOCTL_BTHX_GET_VERSION);
    CHECK(S.ForwardRequests == 1 && S.Completions == 0);
    BtifChildIoctl(CHILD_QUEUE, (WDFREQUEST)(uintptr_t)11, 0, 0, 0);
    CHECK(S.ForwardRequests == 1 && S.Completions == 1);
}

static NTSTATUS Prepare(void);
static NTSTATUS Start(void);
static NTSTATUS Stop(void);
static void Release(void);
static BOOLEAN Interrupt(void);

static void TestHciOwnerQueues(void)
{
    UCHAR ReadInput[1] = {HciPacketEvent};
    UCHAR ReadOutput[FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + 257U] = {0};
    static const UCHAR Event[] = {HciPacketEvent, 0x0e, 1, 0};
    WDFREQUEST ReadRequest = (WDFREQUEST)(uintptr_t)301;
    MTK_STP_FULL_LINK Peer;
    unsigned Completed;

    Fresh();
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_SUCCESS);
    MapRequestBuffers(ReadRequest, ReadInput, sizeof(ReadInput), ReadOutput, sizeof(ReadOutput));
    Completed = S.Completions;
    BtifParentIoctl(PARENT_QUEUE, ReadRequest, sizeof(ReadOutput), sizeof(ReadInput),
                    IOCTL_BTHX_READ_HCI);
    CHECK(S.PendingRequests[0] == ReadRequest && S.Completions == Completed);
    CHECK(MtkStpFullInit(&Peer, NULL, WritePeer) == MtkStpOk);
    Peer.TxNext = Owner.Mode.Link.RxNext;
    Peer.RxNext = Owner.Mode.Link.TxNext;
    CHECK(MtkStpFullTransmit(&Peer, T70_BT_STP_CHANNEL, Event, sizeof(Event), S.NowMs) == MtkStpOk);
    CHECK(Interrupt());
    PBTHX_HCI_READ_WRITE_CONTEXT ReadResult = (PBTHX_HCI_READ_WRITE_CONTEXT)ReadOutput;
    CHECK(S.Completions == Completed + 1 && S.CompletionStatuses[Completed] == STATUS_SUCCESS);
    CHECK(S.CompletionInfo[Completed] == FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + 3U);
    CHECK(ReadResult->Type == HciPacketEvent && ReadResult->DataLen == 3 &&
          ReadResult->Data[0] == 0x0e && ReadResult->Data[1] == 1 && ReadResult->Data[2] == 0);
    CHECK(Owner.Hci.Events.Count == 0 && Owner.Mode.Link.RxNext == 3);
    CHECK(Interrupt()); // Drain the full-mode link ACK queued by Accept.
    CHECK(S.CancelMarks == 0U && S.CancelUnmarks == 0U);
    Owner.RxPaused = TRUE;
    Owner.PendingHciFrameActive = TRUE;
    Owner.PendingHciFrameOffset = 7U;
    Owner.PendingHciFrameLength = 11U;
    Owner.PendingHciRawLength = 9U;
    CHECK(BtifD0Exit(DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    CHECK(!Owner.RxPaused && !Owner.PendingHciFrameActive &&
          Owner.PendingHciFrameOffset == 0U && Owner.PendingHciFrameLength == 0U &&
          Owner.PendingHciRawLength == 0U);
    Release();

    UCHAR WriteInput[FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + 3U] = {0};
    ULONG BytesWritten = 0;
    WDFREQUEST WriteRequest = (WDFREQUEST)(uintptr_t)302;
    PBTHX_HCI_READ_WRITE_CONTEXT WritePacket = (PBTHX_HCI_READ_WRITE_CONTEXT)WriteInput;
    WritePacket->Type = HciPacketCommand;
    WritePacket->DataLen = 3;
    WritePacket->Data[0] = 0x03;
    WritePacket->Data[1] = 0x0c;
    WritePacket->Data[2] = 0;
    Fresh();
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_SUCCESS);
    MapRequestBuffers(WriteRequest, WriteInput, sizeof(WriteInput), &BytesWritten, sizeof(BytesWritten));
    Completed = S.Completions;
    BtifParentIoctl(PARENT_QUEUE, WriteRequest, sizeof(BytesWritten), sizeof(WriteInput),
                    IOCTL_BTHX_WRITE_HCI);
    if (S.PendingRequests[2] != WriteRequest || S.Completions != Completed) {
        fprintf(stderr, "write submit: pending=%p request=%p completions=%u base=%u status=%#x\\n",
                S.PendingRequests[2], WriteRequest, S.Completions, Completed,
                S.Completions > Completed ? S.CompletionStatuses[Completed] : 0);
        abort();
    }
    CHECK(Interrupt());
    CHECK(S.DataFrames == 1 && S.DataChannel == T70_BT_STP_CHANNEL && S.DataPayloadBytes == 4);
    CHECK(S.Completions == Completed + 1 && S.CompletionStatuses[Completed] == STATUS_SUCCESS);
    CHECK(S.CompletionInfo[Completed] == sizeof(BytesWritten) && BytesWritten == 3);
    CHECK(S.CancelMarks == 0U && S.CancelUnmarks == 0U);
    UCHAR Ack[4] = {(UCHAR)(0x80U | Owner.Mode.Link.TxNext), 0, 0,
                    (UCHAR)(0x80U | Owner.Mode.Link.TxNext)};
    ReceiveBytes(Ack, sizeof(Ack));
    CHECK(Interrupt() && Owner.Mode.Link.TxLength == 0);
    CHECK(Stop() == STATUS_SUCCESS);
    Release();
}

static void TestQueueCancellation(void)
{
    unsigned Completed = S.Completions;
    BtifReadCanceledOnQueue(Owner.ReadEventQueue, (WDFREQUEST)(uintptr_t)201);
    BtifReadCanceledOnQueue(Owner.ReadAclQueue, (WDFREQUEST)(uintptr_t)202);
    BtifWriteCanceled(Owner.WriteQueue, (WDFREQUEST)(uintptr_t)203);
    CHECK(!S.Lock && S.Completions == Completed + 3);
    CHECK(S.CompletionStatuses[Completed] == STATUS_CANCELLED &&
          S.CompletionStatuses[Completed + 1] == STATUS_CANCELLED &&
          S.CompletionStatuses[Completed + 2] == STATUS_CANCELLED);
}

static NTSTATUS Prepare(void) { return BtifPrepare(DEVICE, &Lists[0], &Lists[1]); }
static NTSTATUS Start(void) { TestPeerStarted = FALSE; return BtifD0Entry(DEVICE, WdfPowerDeviceD3); }
static NTSTATUS Stop(void) { return BtifD0Exit(DEVICE, WdfPowerDeviceD3); }

static void Release(void)
{
    CHECK(BtifRelease(DEVICE, NULL) == STATUS_SUCCESS);
    CHECK(!S.Ref && !S.PdoRef && !S.Token && !Owner.Prepared && !Owner.Target && !Owner.Provider);
}
static void Rejected(void)
{
    CHECK(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR && !S.Maps && !S.Unmaps && !Owner.Prepared);
}

static BOOLEAN Interrupt(void)
{
    // Passive ISR synchronization is explicit in the production callback.
    return IrqConfig.EvtInterruptIsr(IRQ, 0);
}

static void TestResources(void)
{
    for (unsigned I = 1; I <= 3; ++I) {
        Fresh();
        S.FailMap = I;
        CHECK(Prepare() == STATUS_INSUFFICIENT_RESOURCES && S.Maps == I && S.Unmaps == I - 1);
        Release();
        CHECK(!S.Reads && !S.Writes && !S.Opens);
    }
    for (unsigned Page = 0; Page < 2; ++Page) {
        for (unsigned Count = 0; Count < 8; ++Count) {
            if (Count != 6) { Fresh(); Lists[Page].Count = Count; Rejected(); }
        }
        for (unsigned I = 0; I < 3; ++I) {
            Fresh(); Lists[Page].R[I].u.Memory.Start.QuadPart++; Rejected();
            Fresh(); Lists[Page].R[I].u.Memory.Length--; Rejected();
            Fresh(); Lists[Page].R[I].Flags = 1; Rejected();
            Fresh(); Lists[Page].R[I].Type = CmResourceTypeInterrupt; Rejected();
        }
        for (unsigned I = 3; I < 6; ++I) {
            Fresh(); Lists[Page].R[I].ShareDisposition = CmResourceShareShared; Rejected();
            Fresh(); Lists[Page].R[I].Flags = CM_RESOURCE_INTERRUPT_LATCHED; Rejected();
            Fresh(); Lists[Page].R[I].Flags = CM_RESOURCE_INTERRUPT_MESSAGE; Rejected();
            Fresh(); Lists[Page].R[I].Flags = CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT; Rejected();
            if (!Page) { Fresh(); Lists[0].R[I].u.Interrupt.Vector++; Rejected(); }
        }
    }
    for (unsigned I = 1; I <= 6; ++I) {
        Fresh(); S.NullIndex = I; Rejected();
    }
    Fresh();
    CHECK(BtifPrepare(DEVICE, NULL, &Lists[1]) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(BtifPrepare(DEVICE, &Lists[0], NULL) == STATUS_DEVICE_CONFIGURATION_ERROR);
}

static void TestProviderFailures(void)
{
    Fresh();
    CHECK(Start() == STATUS_DEVICE_NOT_READY && !S.Reads);
    S.Paths = 0;
    CHECK(Prepare() == STATUS_DEVICE_NOT_READY && !S.Interrupts && !S.Opens);
    Release();
    CHECK(S.Unmaps == 3);
    Fresh();
    S.Paths = 2;
    CHECK(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR && !S.Opens);
    Release();
    Fresh();
    S.BadInterface = 1;
    CHECK(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR && S.Opens == 1 && !S.Interrupts && !S.Ref);
    Release();
    CHECK(S.Deletes == 1);
    Fresh();
    S.RemoveDuringQuery = 1;
    CHECK(Prepare() == STATUS_DEVICE_REMOVED && S.Closes == 1 && S.Failed == 1 && !S.Ref && !S.PdoRef);
    Release();
    Fresh();
    S.IrqStatus = STATUS_INSUFFICIENT_RESOURCES;
    CHECK(Prepare() == S.IrqStatus && S.Interrupts == 1 && !S.Reads && !S.Writes);
    Release();
    CHECK(S.Unmaps == 3 && S.Deletes == 1);
}

static void TestPowerRelations(void)
{
    IRP Irp = {0};
    Irp.Current.Parameters.QueryDeviceRelations.Type = PowerRelations;
    Irp.IoStatus.Status = STATUS_NOT_SUPPORTED;
    CHECK(BtifQueryRelations(DEVICE, &Irp) == STATUS_SUCCESS && S.PdoRef == 2);
    PDEVICE_RELATIONS Relations = (PDEVICE_RELATIONS)Irp.IoStatus.Information;
    CHECK(Relations && Relations->Count == 1 && Relations->Objects[0] == PROVIDER);
    Irp.Skipped = 0;
    CHECK(BtifQueryRelations(DEVICE, &Irp) == STATUS_SUCCESS && S.PdoRef == 2);
    CHECK((PDEVICE_RELATIONS)Irp.IoStatus.Information == Relations);
    ObDereferenceObject(Relations->Objects[0]);
    ExFreePool(Relations);
}

static void TestInterrupt(void)
{
    Fresh();
    CHECK(Prepare() == STATUS_SUCCESS && S.Maps == 3 && !S.Reads && !S.Writes && S.Ref == 1);
    CHECK(Prepare() == STATUS_INVALID_DEVICE_STATE);
    TestPowerRelations();
    CHECK(!Interrupt() && !S.Reads);
    S.AcquireStatus = STATUS_DEVICE_BUSY;
    CHECK(Start() == S.AcquireStatus && !S.Token && !S.Access && !S.Reads);
    S.AcquireStatus = STATUS_SUCCESS;
    S.DelayAck = 1;
    NTSTATUS Started = Start();
    CHECK(Started == STATUS_SUCCESS && S.Token && Owner.Fifo.Running && S.Regs[0][1] == 1);
    CHECK(S.TxFrames == 5 && S.NowMs >= S.AckAt && Owner.ModeSession == S.Session && Owner.RadioReady);
    CHECK(Owner.Mode.Link.TxNext == 2 && Owner.Mode.Link.RxNext == 2 && !Owner.Fifo.Count);

    UCHAR Data[300];
    memset(Data, 0xa5, sizeof(Data));
    MTK_STP_FULL_LINK Peer;
    CHECK(MtkStpFullInit(&Peer, NULL, WritePeer) == MtkStpOk && !Interrupt());
    Peer.TxNext = Owner.Mode.Link.RxNext;
    Peer.RxNext = Owner.Mode.Link.TxNext;
    unsigned StartRx = S.RxTail;
    CHECK(MtkStpFullTransmit(&Peer, 1, Data, sizeof(Data), S.NowMs) == MtkStpOk);
    unsigned EndRx = S.RxTail;
    S.RxTail = StartRx + 2;
    CHECK(Interrupt() && S.RxHead == StartRx + 2 && Owner.Mode.Link.HeaderUsed == 2);
    S.RxTail = EndRx;
    CHECK(Interrupt() && S.RxHead == StartRx + 2 + MTK_BTIF_RX_BUDGET && Owner.Mode.Link.BodyUsed);
    CHECK(Interrupt() && S.RxHead == EndRx && !Owner.Mode.Link.HeaderUsed);
    CHECK(Owner.Mode.Link.RxNext == 3 && !memcmp(Owner.Mode.Link.Body, Data, sizeof(Data)));
    CHECK(Interrupt() && S.TxFrames == 6 && !Owner.Fifo.Count);
    CHECK(!Interrupt() && !S.Failed && !S.Work);
    ULONGLONG Token = S.Token;
    unsigned Completed = S.Completions;
    S.PendingRequests[0] = (WDFREQUEST)(uintptr_t)101;
    S.PendingRequests[1] = (WDFREQUEST)(uintptr_t)102;
    S.PendingRequests[2] = (WDFREQUEST)(uintptr_t)103;
    CHECK(Stop() == STATUS_SUCCESS && !S.Token && S.Quiesced == 1 && S.Releases == 1);
    CHECK(S.Completions == Completed + 3);
    CHECK(S.CompletionStatuses[Completed] == STATUS_DEVICE_NOT_CONNECTED &&
          S.CompletionStatuses[Completed + 1] == STATUS_DEVICE_NOT_CONNECTED &&
          S.CompletionStatuses[Completed + 2] == STATUS_DEVICE_NOT_CONNECTED);
    CHECK(S.Regs[0][0x6c / 4] == 0x20 && S.Regs[0][0x0c / 4] == 0x51 && !S.Regs[0][1]);
    ULONG OldEpoch = Owner.HciEpoch;
    CHECK(Start() == STATUS_SUCCESS && S.Token > Token && S.TxFrames == 7 && Owner.RadioReady);
    CHECK(Owner.ModeSession == Token && Owner.Mode.Link.RxNext == 4 && Owner.Mode.Link.TxNext == 3);
    CHECK(Owner.HciEpoch != 0 && Owner.HciEpoch != OldEpoch && Owner.Hci.Link == &Owner.Mode.Link);
    CHECK(Stop() == STATUS_SUCCESS && S.Quiesced == 2 && S.Writes == S.Barriers);
    Release();
    Release();
    CHECK(S.Unmaps == 3 && S.Deletes == 1 && S.Releases == 2);
    S.Maps = S.Unmaps = 0;
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_DEVICE_NOT_READY && S.TxFrames == 7);
    S.Session = S.TxFrames = S.AckNext = S.DelayAck = 0;
    CHECK(Start() == STATUS_SUCCESS && S.TxFrames == 5 && Owner.ModeSession > Token && Owner.RadioReady);
    CHECK(Stop() == STATUS_SUCCESS);
    Release();
}

static void TestFaults(void)
{
    Fresh();
    CHECK(Prepare() == STATUS_SUCCESS);
    S.Regs[1][2] = 1;
    CHECK(Start() == STATUS_DEVICE_BUSY && !S.Writes && !S.Quiesced && S.Releases == 1 && !S.Token);
    Release();
    Fresh();
    CHECK(Prepare() == STATUS_SUCCESS);
    S.RejectStartWrite = 1;
    CHECK(Start() == STATUS_IO_DEVICE_ERROR && !Owner.Fifo.Owned && S.Quiesced == 1 && S.Releases == 1);
    CHECK(Start() == STATUS_SUCCESS && Stop() == STATUS_SUCCESS);
    Release();
    Fresh();
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_SUCCESS);
    static const UCHAR InvalidChannel[] = {0x80,0x50,1,0xd1};
    ReceiveBytes(InvalidChannel, sizeof(InvalidChannel));
    CHECK(Interrupt() && S.Work == 1 && Owner.Faulted && !S.Regs[0][1]);
    CHECK(!Interrupt());
    IrqConfig.EvtInterruptWorkItem(IRQ, DEVICE);
    CHECK(S.Failed == 1 && Stop() == STATUS_IO_DEVICE_ERROR && S.Releases == 1 && !S.Quiesced);
    Release();
    Fresh();
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_SUCCESS);
    static const UCHAR Partial[] = {0x80,0x40};
    ReceiveBytes(Partial, sizeof(Partial));
    CHECK(Interrupt() && Owner.Mode.Link.HeaderUsed == sizeof(Partial) && !S.Work);
    CHECK(Stop() == STATUS_IO_DEVICE_ERROR && !S.Quiesced && !S.Regs[0][1]);
    Release();
}

static void TestModeFailure(void)
{
    Fresh();
    S.Silent = 1;
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_IO_TIMEOUT);
    CHECK(S.TxFrames == 1 && S.NowMs == MTK_WMT_MODE_TIMEOUT_MS && Owner.Faulted);
    CHECK(!S.Token && !Owner.ModeSession && S.Releases == 1 && !S.Quiesced && !S.Regs[0][1]);
    CHECK(!Interrupt() && Stop() == STATUS_SUCCESS && S.Releases == 1);
    Release();

    Fresh();
    S.RejectMode = 1;
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_DATA_ERROR);
    CHECK(S.TxFrames == 2 && !Owner.ModeSession && !S.Quiesced && S.Releases == 1);
    Release();

    Fresh();
    S.RemoveDuringWait = 1;
    CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_DEVICE_REMOVED);
    CHECK(!S.TxFrames && !Owner.ModeSession && !S.Token && !S.Quiesced && S.Releases == 1);
    CHECK(!Interrupt() && S.Failed == 1 && Stop() == STATUS_SUCCESS);
    Release();
}

static void TestRemoval(void)
{
    for (unsigned Case = 0; Case < 3; ++Case) {
        Fresh();
        CHECK(Prepare() == STATUS_SUCCESS && Start() == STATUS_SUCCESS);
        unsigned Reads = S.Reads, Writes = S.Writes;
        if (Case == 0) {
            S.Lost = 1;
            TargetConfig.EvtIoTargetRemoveComplete(TARGET);
            CHECK(S.Failed == 1 && S.Closes == 1 && !Interrupt());
            CHECK(Stop() == STATUS_SUCCESS);
        } else if (Case == 1) {
            Callbacks.EvtDeviceSurpriseRemoval(DEVICE);
            CHECK(!Interrupt() && Stop() == STATUS_SUCCESS);
        }
        // Case 2 exercises ReleaseHardware even if D0Exit was bypassed.
        Release();
        CHECK(S.Reads == Reads && S.Writes == Writes && S.Unmaps == 3);
        CHECK(S.Releases == 1 && !S.Quiesced && S.Deletes == 1);
    }
}

int main(void)
{
    Fresh();
    TestQueueCancellation();
    TestHciOwnerQueues();
    Fresh();
    S.DriverStatus = STATUS_INSUFFICIENT_RESOURCES;
    CHECK(DriverEntry(NULL, NULL) == S.DriverStatus && DriverConfig.EvtDriverDeviceAdd == BtifDeviceAdd);
    S.DriverStatus = STATUS_SUCCESS;
    CHECK(DriverEntry(NULL, NULL) == STATUS_SUCCESS);
    S.CreateStatus = STATUS_INSUFFICIENT_RESOURCES;
    CHECK(BtifDeviceAdd(NULL, (void *)1) == S.CreateStatus && !S.Maps);
    S.CreateStatus = STATUS_SUCCESS;
    S.ChildCreateStatus = STATUS_INSUFFICIENT_RESOURCES;
    CHECK(BtifDeviceAdd(NULL, (void *)1) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(S.DeviceInitFrees == 1 && S.ChildAdds == 1);
    CHECK(Callbacks.EvtDevicePrepareHardware == BtifPrepare && Callbacks.EvtDeviceReleaseHardware == BtifRelease);
    CHECK(Callbacks.EvtDeviceD0Entry == BtifD0Entry && Callbacks.EvtDeviceD0Exit == BtifD0Exit);
    TestResources();
    TestProviderFailures();
    TestInterrupt();
    TestFaults();
    TestModeFailure();
    TestRemoval();
    printf("BTIF owner: %u checks passed (PnP, WCN0 access, WMT mode/radio gate, FIFO IRQ and teardown)\n", Checks);
    return 0;
}
