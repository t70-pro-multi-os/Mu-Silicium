// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Bootstrap.h"
#include <bcrypt.h>

#define CONN_WINDOW_BASE 0x18000000ULL
#define CONN_WINDOW_SIZE 0x100000U
#define CONN_REGISTER_PAGE 0x1000U
#define CONN_RESET_ADDRESS 0x10007018ULL
#define CONN_RESET_OFFSET 0x18U
#define CONN_RESET_KEY 0x88000000U
#define CONN_RESET_FIELD 0x1000U
#define CONN_RESET_PRESERVE 0x00ffefffU
#define CONN_DRAM_BASE 0x40000000ULL
/* Stock mtk-btcvsd-snd mediatek,offset: INFRACFG_AO +0xf00, bit 11. */
#define CONN_SCO_IRQ_MASK_OFFSET 0x0f00U
#define CONN_SCO_IRQ_MASK 0x00000800U

static NTSTATUS ExactSuccess(NTSTATUS Status);

static NTSTATUS ConnStatus(MTK_CONN_POWER_STATUS Status)
{
    switch (Status) {
    case MtkConnPowerOk: return STATUS_SUCCESS;
    case MtkConnPowerInvalid: return STATUS_INVALID_PARAMETER;
    case MtkConnPowerBusy: return STATUS_DEVICE_BUSY;
    case MtkConnPowerNotReady: return STATUS_DEVICE_NOT_READY;
    case MtkConnPowerTimeout: return STATUS_IO_TIMEOUT;
    default: return STATUS_IO_DEVICE_ERROR;
    }
}

static BOOLEAN ValidResource(const CM_PARTIAL_RESOURCE_DESCRIPTOR *Resource, ULONG Bytes)
{
    return Resource && Resource->Type == CmResourceTypeMemory &&
           Resource->ShareDisposition == CmResourceShareDeviceExclusive &&
           (Resource->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)) == 0 &&
           Resource->u.Memory.Length == Bytes && Resource->u.Memory.Start.QuadPart > 0 &&
           Resource->u.Memory.Start.QuadPart <= MAXLONGLONG - Bytes &&
           (Resource->u.Memory.Start.QuadPart & (sizeof(ULONG) - 1)) == 0;
}

NTSTATUS T70ConnPrepareHardware(T70_CONN_BOOTSTRAP *Owner,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawWindow,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *TranslatedWindow,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawReset,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *TranslatedReset)
{
    if (!Owner || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Owner->Window || Owner->Reset) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!ValidResource(RawWindow, CONN_WINDOW_SIZE) ||
        !ValidResource(TranslatedWindow, CONN_WINDOW_SIZE) ||
        !ValidResource(RawReset, sizeof(ULONG)) ||
        !ValidResource(TranslatedReset, sizeof(ULONG)) ||
        RawWindow->u.Memory.Start.QuadPart != CONN_WINDOW_BASE ||
        RawReset->u.Memory.Start.QuadPart != CONN_RESET_ADDRESS) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    LONGLONG Window = TranslatedWindow->u.Memory.Start.QuadPart;
    LONGLONG Reset = TranslatedReset->u.Memory.Start.QuadPart;
    if (Reset < Window + CONN_WINDOW_SIZE && Window < Reset + (LONGLONG)sizeof(ULONG)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    PUCHAR Aperture = MmMapIoSpaceEx(TranslatedWindow->u.Memory.Start,
                                    CONN_WINDOW_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Aperture) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PULONG ResetRegister = MmMapIoSpaceEx(TranslatedReset->u.Memory.Start,
                                        sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
    if (!ResetRegister) {
        MmUnmapIoSpace(Aperture, CONN_WINDOW_SIZE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Owner->Window = Aperture;
    Owner->Reset = ResetRegister;
    return STATUS_SUCCESS;
}

static NTSTATUS ReserveEmi(T70_CONN_BOOTSTRAP *Owner)
{
    PHYSICAL_ADDRESS Low = { .QuadPart = CONN_DRAM_BASE };
    PHYSICAL_ADDRESS High = { .QuadPart = MTK_CONN_EMI_LIMIT - 1 };
    PHYSICAL_ADDRESS Boundary = { .QuadPart = MTK_CONN_EMI_ALIGN };
    /* Request the full remap granule so Windows cannot place pages in its padding. */
    PUCHAR Emi = MmAllocateContiguousMemorySpecifyCache(
        MTK_CONN_EMI_ALIGN, Low, High, Boundary, MmNonCached);
    if (!Emi) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    LONGLONG Base = MmGetPhysicalAddress(Emi).QuadPart;
    if (Base < Low.QuadPart || Base > High.QuadPart + 1 - MTK_CONN_EMI_ALIGN ||
        (Base & (MTK_CONN_EMI_ALIGN - 1)) != 0) {
        MmFreeContiguousMemory(Emi);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RtlZeroMemory(Emi, MTK_CONN_EMI_ALIGN);
    KeMemoryBarrier();
    Owner->Emi = Emi;
    Owner->EmiBase = (ULONGLONG)Base;
    return STATUS_SUCCESS;
}

static PULONG Register(T70_CONN_BOOTSTRAP *Owner, MTK_CONN_ROM_REGION Region, unsigned Offset)
{
    if (Region == MtkConnRomRgu) {
        return Offset == CONN_RESET_OFFSET ? Owner->Reset : NULL;
    }
    static const ULONG Base[] = {
        [MtkConnRomTop] = 0xb1000, [MtkConnRomMcuCfg] = 0x02000,
        [MtkConnHostCsr] = 0x07000, [MtkConnSysram] = 0x04000,
        [MtkConnMcuMem] = 0xc0000, [MtkConnRfSpi] = 0xc6000
    };
    if ((unsigned)Region >= RTL_NUMBER_OF(Base) || !Base[Region] ||
        Offset >= CONN_REGISTER_PAGE || (Offset & (sizeof(ULONG) - 1)) != 0) {
        return NULL;
    }
    return (PULONG)(Owner->Window + Base[Region] + Offset);
}

static int ReadPin(T70_CONN_BOOTSTRAP *Owner, MTK_CONN_PIN_SPACE Region,
                   unsigned Offset, unsigned *Value)
{
    ULONG Result;
    NTSTATUS Status = Owner->Pins.Read(Owner->Pins.Header.Context, Owner->PinsToken,
                                      Region, Offset, &Result);
    if (Status == STATUS_SUCCESS) {
        *Value = Result;
    }
    return Status;
}

static int WritePin(T70_CONN_BOOTSTRAP *Owner, MTK_CONN_PIN_SPACE Region,
                    unsigned Offset, unsigned Value)
{
    return Owner->Pins.Write(Owner->Pins.Header.Context, Owner->PinsToken, Region, Offset, Value);
}

static int ReadRegister(void *Context, MTK_CONN_ROM_REGION Region, unsigned Offset, unsigned *Value)
{
    T70_CONN_BOOTSTRAP *Owner = Context;
    if (Region == MtkConnGpio || Region == MtkConnIoCfg) {
        return ReadPin(Owner, Region == MtkConnGpio ? MtkConnPinsGpio : MtkConnPinsIoCfg,
                       Offset, Value);
    }
    PULONG Address = Register(Owner, Region, Offset);
    if (!Address) {
        return STATUS_INVALID_PARAMETER;
    }
    *Value = READ_REGISTER_ULONG(Address);
    return STATUS_SUCCESS;
}

static int WriteRegister(void *Context, MTK_CONN_ROM_REGION Region, unsigned Offset, unsigned Value)
{
    T70_CONN_BOOTSTRAP *Owner = Context;
    if (Region == MtkConnGpio || Region == MtkConnIoCfg) {
        return WritePin(Owner, Region == MtkConnGpio ? MtkConnPinsGpio : MtkConnPinsIoCfg,
                        Offset, Value);
    }
    PULONG Address = Register(Owner, Region, Offset);
    if (!Address) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Region == MtkConnRomRgu) {
        ULONG Current = READ_REGISTER_ULONG(Address);
        if (Current == MAXULONG) {
            return STATUS_IO_DEVICE_ERROR;
        }
        if (Value != (CONN_RESET_KEY | (Current & CONN_RESET_PRESERVE) | (Value & CONN_RESET_FIELD))) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    WRITE_REGISTER_ULONG(Address, Value);
    return STATUS_SUCCESS;
}

static int ReadPower(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned *Value)
{
    T70_CONN_BOOTSTRAP *Owner = Context;
    ULONG Result;
    NTSTATUS Status = Owner->Power.Read(Owner->Power.Header.Context, Owner->PowerToken,
                                       Region, Offset, &Result);
    if (Status == STATUS_SUCCESS) {
        *Value = Result;
    }
    return Status;
}

static int WritePower(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned Value)
{
    T70_CONN_BOOTSTRAP *Owner = Context;
    return Owner->Power.Write(Owner->Power.Header.Context, Owner->PowerToken, Region, Offset, Value);
}

/* Keep the sideband line quiet until a client owns the SCO data path. */
static NTSTATUS CaptureAndMaskScoIrq(T70_CONN_BOOTSTRAP *Owner)
{
    unsigned Value;
    NTSTATUS Status = ExactSuccess(ReadPower(Owner, MtkConnInfra, CONN_SCO_IRQ_MASK_OFFSET, &Value));
    if (Status != STATUS_SUCCESS) return Status;
    if (Value & ~CONN_SCO_IRQ_MASK) return STATUS_DEVICE_CONFIGURATION_ERROR;
    Owner->ScoIrqMaskBaseline = Value;
    Owner->ScoIrqMaskCaptured = TRUE;
    Status = ExactSuccess(WritePower(Owner, MtkConnInfra, CONN_SCO_IRQ_MASK_OFFSET, CONN_SCO_IRQ_MASK));
    if (Status != STATUS_SUCCESS) return Status;
    Status = ExactSuccess(ReadPower(Owner, MtkConnInfra, CONN_SCO_IRQ_MASK_OFFSET, &Value));
    if (Status != STATUS_SUCCESS) return Status;
    return Value == CONN_SCO_IRQ_MASK ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS RestoreScoIrqMask(T70_CONN_BOOTSTRAP *Owner)
{
    if (!Owner->ScoIrqMaskCaptured) return STATUS_SUCCESS;
    if (!Owner->PowerToken || !Owner->Power.Write || !Owner->Power.Read) return STATUS_DEVICE_NOT_READY;
    NTSTATUS Status = ExactSuccess(WritePower(Owner, MtkConnInfra, CONN_SCO_IRQ_MASK_OFFSET,
                                               Owner->ScoIrqMaskBaseline));
    if (Status != STATUS_SUCCESS) return Status;
    unsigned Value;
    Status = ExactSuccess(ReadPower(Owner, MtkConnInfra, CONN_SCO_IRQ_MASK_OFFSET, &Value));
    if (Status != STATUS_SUCCESS) return Status;
    if (Value != Owner->ScoIrqMaskBaseline) return STATUS_IO_DEVICE_ERROR;
    Owner->ScoIrqMaskCaptured = FALSE;
    Owner->ScoIrqMaskBaseline = 0;
    return STATUS_SUCCESS;
}

static int WaitUs(void *Context, unsigned Microseconds)
{
    UNREFERENCED_PARAMETER(Context);
    if (Microseconds <= 50U) {
        KeStallExecutionProcessor(Microseconds);
        return STATUS_SUCCESS;
    }
    LARGE_INTEGER Delay = { .QuadPart = -(LONGLONG)Microseconds * 10 };
    return KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

static void OrderIo(void *Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeMemoryBarrier();
}

static int Digest(void *Context, const unsigned char *Data, unsigned Bytes, unsigned char Hash[32])
{
    UNREFERENCED_PARAMETER(Context);
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)Data, Bytes, Hash, 32);
}

static PUCHAR EmiRange(T70_CONN_BOOTSTRAP *Owner, ULONGLONG Address, unsigned Size)
{
    if (!Owner->Emi || Address < Owner->EmiBase || !Size || Size > MTK_CONN_EMI_SIZE ||
        Address - Owner->EmiBase > MTK_CONN_EMI_SIZE - Size) {
        return NULL;
    }
    return Owner->Emi + (SIZE_T)(Address - Owner->EmiBase);
}

static int WriteEmi(void *Context, unsigned long long Address, const unsigned char *Data, unsigned Size)
{
    PUCHAR Target = EmiRange(Context, Address, Size);
    if (!Target) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(Target, Data, Size);
    return STATUS_SUCCESS;
}

static int ReadEmi(void *Context, unsigned long long Address, unsigned char *Data, unsigned Size)
{
    PUCHAR Source = EmiRange(Context, Address, Size);
    if (!Source) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(Data, Source, Size);
    return STATUS_SUCCESS;
}

static int ReadRail(void *Context, unsigned Address, unsigned *Value)
{
    T70_CONN_BOOTSTRAP *Owner = Context;
    return T70ConnPmicRead(&Owner->Pmic, Address, Value);
}

static int WriteRail(void *Context, unsigned Address, unsigned Value)
{
    T70_CONN_BOOTSTRAP *Owner = Context;
    return T70ConnPmicWrite(&Owner->Pmic, Address, Value);
}

static int ReadRailPin(void *Context, unsigned Offset, unsigned *Value)
{
    return ReadPin(Context, MtkConnPinsGpio, Offset, Value);
}

static int WriteRailPin(void *Context, unsigned Offset, unsigned Value)
{
    return WritePin(Context, MtkConnPinsGpio, Offset, Value);
}

static int RailAuthorized(void *Context, MTK_CONN_RAIL Rail)
{
    T70_CONN_BOOTSTRAP *Owner = Context;
    return (unsigned)Rail <= MtkConnRuntimeVcn33 && Owner->PinsToken && Owner->Pmic.Lease &&
           Owner->Pmic.ModuleLocked && !Owner->Pmic.Uncertain;
}

static BOOLEAN ValidInterface(const INTERFACE *Header, USHORT Size, USHORT Version)
{
    return Header->Size >= Size && Header->Version == Version && Header->Context &&
           Header->InterfaceReference && Header->InterfaceDereference;
}

static BOOLEAN ValidImages(const MTK_CONN_ROM_SET Images[2])
{
    for (unsigned Variant = 0; Variant < 2; ++Variant) {
        for (unsigned Part = 0; Part < MtkConnRomPartCount; ++Part) {
            const MTK_CONN_ROM_IMAGE *Image = &Images[Variant].Image[Part];
            if (!Image->Data || !Image->Bytes || Image->Bytes > MTK_CONN_EMI_SIZE) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static NTSTATUS ExactSuccess(NTSTATUS Status)
{
    return Status != STATUS_SUCCESS && NT_SUCCESS(Status) ? STATUS_IO_DEVICE_ERROR : Status;
}

NTSTATUS T70ConnStart(T70_CONN_BOOTSTRAP *Owner, const MTK_CONN_POWER_INTERFACE *Power,
    const MTK_CONN_PINS_INTERFACE *Pins, const MTK_CONN_ROM_SET Images[2])
{
    if (!Owner || !Power || !Pins || !Images || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !ValidInterface(&Power->Header, sizeof(*Power), MTK_CONN_POWER_INTERFACE_VERSION) ||
        !ValidInterface(&Pins->Header, sizeof(*Pins), MTK_CONN_PINS_INTERFACE_VERSION) ||
        !Power->Acquire || !Power->Read || !Power->Write || !Power->Release ||
        !Pins->Acquire || !Pins->Read || !Pins->Write || !Pins->Release || !ValidImages(Images)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Owner->Window || !Owner->Reset || Owner->Emi) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    NTSTATUS Status = ReserveEmi(Owner);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    Owner->Power = *Power;
    Owner->Pins = *Pins;
    Power->Header.InterfaceReference(Power->Header.Context);
    Pins->Header.InterfaceReference(Pins->Header.Context);
    Status = Power->Acquire(Power->Header.Context, Owner->EmiBase, &Owner->PowerToken);
    if (Status != STATUS_SUCCESS || !Owner->PowerToken) {
        goto Fail;
    }
    Status = CaptureAndMaskScoIrq(Owner);
    if (Status != STATUS_SUCCESS) {
        goto Fail;
    }
    Status = Pins->Acquire(Pins->Header.Context, &Owner->PinsToken);
    if (Status != STATUS_SUCCESS || !Owner->PinsToken) {
        goto Fail;
    }
    Status = T70ConnPmicAcquire(&Owner->Pmic);
    if (Status != STATUS_SUCCESS) {
        goto Fail;
    }

    const MTK_CONN_POWER_IO PowerIo = {Owner, ReadPower, WritePower, WaitUs, OrderIo};
    const MTK_CONN_BOOT_IO BootIo = {
        {Owner, ReadRegister, Digest, WriteEmi, ReadEmi}, WriteRegister
    };
    const MTK_CONN_RAIL_IO RailsIo = {
        Owner, ReadRail, WriteRail, ReadRailPin, WriteRailPin, WaitUs, RailAuthorized
    };
    const MTK_CONN_PROBE_IO ProbeIo = {&Owner->Rails, MtkConnRailsSetProbeRail};
    unsigned Adie;
    MTK_CONN_POWER_STATUS Result = MtkConnRailsInit(&Owner->Rails, &RailsIo);
    if (Result == MtkConnPowerOk) {
        Result = MtkConnBootInit(&Owner->Boot, &PowerIo, &BootIo);
    }
    if (Result == MtkConnPowerOk) {
        Result = MtkConnBootProbeAdie(&Owner->Boot, Owner->EmiBase, MTK_CONN_EMI_SIZE, &ProbeIo, &Adie);
    }
    if (Result == MtkConnPowerOk) {
        Result = MtkConnBootStartPowered(&Owner->Boot, Owner->EmiBase, MTK_CONN_EMI_SIZE, Adie,
                                        &Images[Adie == 0x6635U], &Owner->Rails);
    }
    if (Result == MtkConnPowerOk) {
        Result = MtkConnBootCheck(&Owner->Boot);
    }
    Status = ConnStatus(Result);
    if (Status == STATUS_SUCCESS) {
        Owner->Adie = Adie;
        return Status;
    }

Fail:
    if (NT_SUCCESS(Status)) {
        Status = STATUS_IO_DEVICE_ERROR;
    }
    NTSTATUS Cleanup = T70ConnStop(Owner);
    return Cleanup == STATUS_SUCCESS ? Status : Cleanup;
}

NTSTATUS T70ConnCheck(const T70_CONN_BOOTSTRAP *Owner)
{
    if (!Owner || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    return Owner->Adie ? ConnStatus(MtkConnBootCheck(&Owner->Boot)) : STATUS_DEVICE_NOT_READY;
}

NTSTATUS T70ConnRestoreBtifClock(T70_CONN_BOOTSTRAP *Owner)
{
    if (!Owner || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Owner->BtifRestore) {
        return STATUS_SUCCESS;
    }
    if (!Owner->Window || !Owner->Reset || !Owner->PowerToken) {
        return STATUS_DEVICE_NOT_READY;
    }
    NTSTATUS Status = WritePower(Owner, MtkConnInfra, MTK_CONN_BTIF_GATE_SET, MTK_CONN_BTIF_GATE_MASK);
    if (Status == STATUS_SUCCESS) {
        Owner->BtifRestore = FALSE;
    }
    return ExactSuccess(Status);
}

NTSTATUS T70ConnEnableBtifClock(T70_CONN_BOOTSTRAP *Owner)
{
    NTSTATUS Status = T70ConnCheck(Owner);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    Status = T70ConnRestoreBtifClock(Owner);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }

    unsigned Gate;
    Status = ReadPower(Owner, MtkConnInfra, MTK_CONN_BTIF_GATE_STATUS, &Gate);
    if (Status != STATUS_SUCCESS) {
        return ExactSuccess(Status);
    }
    if (Gate & ~MTK_CONN_BTIF_GATE_MASK) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Owner->BtifRestore = Gate != 0;
    // Always ask the SoC provider to verify the parents, including a borrowed clock.
    Status = WritePower(Owner, MtkConnInfra, MTK_CONN_BTIF_GATE_CLEAR, MTK_CONN_BTIF_GATE_MASK);
    if (Status != STATUS_SUCCESS) {
        NTSTATUS Cleanup = T70ConnRestoreBtifClock(Owner);
        return Cleanup == STATUS_SUCCESS ? ExactSuccess(Status) : Cleanup;
    }
    return STATUS_SUCCESS;
}

NTSTATUS T70ConnStop(T70_CONN_BOOTSTRAP *Owner)
{
    if (!Owner || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Owner->Emi) {
        return STATUS_SUCCESS;
    }
    if (!Owner->Window || !Owner->Reset) {
        return STATUS_DEVICE_NOT_READY;
    }
    Owner->Adie = 0;
    if (Owner->Boot.Io.Write32) {
        NTSTATUS Status = ConnStatus(MtkConnBootStop(&Owner->Boot));
        if (Status != STATUS_SUCCESS) {
            return Status;
        }
        RtlZeroMemory(&Owner->Boot, sizeof(Owner->Boot));
        RtlZeroMemory(&Owner->Rails, sizeof(Owner->Rails));
    }
    // The SCO IRQ mask is shared through the SoC owner's lease. Restore it
    // after CONN stops; retain the lease if the write/readback is uncertain.
    NTSTATUS Status = RestoreScoIrqMask(Owner);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    // A lost BTIF client may have left the peer active. Isolate CONN before
    // restoring the peripheral gate when the client could not prove idle.
    Status = T70ConnRestoreBtifClock(Owner);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    Status = T70ConnPmicRelease(&Owner->Pmic);
    if (Status != STATUS_SUCCESS) {
        return ExactSuccess(Status);
    }
    if (Owner->PinsToken) {
        Status = Owner->Pins.Release(Owner->Pins.Header.Context, Owner->PinsToken);
        if (Status != STATUS_SUCCESS) {
            return ExactSuccess(Status);
        }
        Owner->PinsToken = 0;
    }
    if (Owner->PowerToken) {
        Status = Owner->Power.Release(Owner->Power.Header.Context, Owner->PowerToken);
        if (Status != STATUS_SUCCESS) {
            return ExactSuccess(Status);
        }
        Owner->PowerToken = 0;
    }
    Owner->Pins.Header.InterfaceDereference(Owner->Pins.Header.Context);
    Owner->Power.Header.InterfaceDereference(Owner->Power.Header.Context);
    RtlZeroMemory(&Owner->Pins, sizeof(Owner->Pins));
    RtlZeroMemory(&Owner->Power, sizeof(Owner->Power));
    RtlZeroMemory(&Owner->Pmic, sizeof(Owner->Pmic));
    MmFreeContiguousMemory(Owner->Emi);
    Owner->Emi = NULL;
    Owner->EmiBase = 0;
    return STATUS_SUCCESS;
}

NTSTATUS T70ConnReleaseHardware(T70_CONN_BOOTSTRAP *Owner)
{
    if (!Owner || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    Owner->Adie = 0;
    Owner->Boot.Ready = 0;
    if (Owner->Reset) {
        MmUnmapIoSpace(Owner->Reset, sizeof(ULONG));
        Owner->Reset = NULL;
    }
    if (Owner->Window) {
        MmUnmapIoSpace(Owner->Window, CONN_WINDOW_SIZE);
        Owner->Window = NULL;
    }
    return Owner->Emi ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
