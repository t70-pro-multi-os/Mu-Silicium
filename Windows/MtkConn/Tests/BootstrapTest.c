// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../Bootstrap.h"
#include "../Firmware.h"
#include <ntifs.h>
#include <bcrypt.h>

#define main ConnRomCoreTest
#include "../../../Platforms/Teclast/t70proPkg/Tests/MtkConnRomTest.c"
#undef main

#define WINDOW_PA 0x19000000LL
#define RESET_PA 0x11007018LL
#define POWER_TOKEN 19U
#define PINS_TOKEN 23U
#define PMIC_TOKEN 29U

static T70_CONN_BOOTSTRAP Device;
static BOOT_MODEL Board;
static ULONG Window[0x100000 / sizeof(ULONG)];
static ULONG ResetWord;
static PUCHAR Grant;
static LONGLONG PhysicalBase;
static unsigned Maps, Unmaps, Allocs, Frees, MapFailure, FailAllocation;
static unsigned PinLease, PowerLease, PowerRefs, PinRefs, PmicActive;
static unsigned FailPins, FailPmic, FailUnlock, FailPowerRelease, RaisedIrql;
static BOOLEAN ResourcesRevoked;
static ULONG BtifGate;
static unsigned BtifEnableCalls, BtifRestoreCalls;
static BOOLEAN FailBtifEnable, FailBtifRestore, RestoreAfterPowerOff;
static BOOLEAN FailScoMaskWriteOnce, FailScoRestoreOnce;

PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS Start, SIZE_T Bytes, ULONG Protect)
{
    CHECK(Protect == (PAGE_READWRITE | PAGE_NOCACHE));
    CHECK((Start.QuadPart == WINDOW_PA && Bytes == sizeof(Window)) ||
          (Start.QuadPart == RESET_PA && Bytes == sizeof(ResetWord)));
    if (++Maps == MapFailure) {
        return NULL;
    }
    return Start.QuadPart == WINDOW_PA ? (PVOID)Window : &ResetWord;
}

VOID MmUnmapIoSpace(PVOID Address, SIZE_T Bytes)
{
    if (ResourcesRevoked) {
        CHECK(Grant && PowerLease && PinLease && PmicActive && BootOn(&Board));
    } else {
        CHECK(!Grant && !PowerLease && !PinLease && !PmicActive && !BootOn(&Board));
    }
    CHECK((Address == Window && Bytes == sizeof(Window)) ||
          (Address == &ResetWord && Bytes == sizeof(ResetWord)));
    ++Unmaps;
}

PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T Bytes, PHYSICAL_ADDRESS Low,
    PHYSICAL_ADDRESS High, PHYSICAL_ADDRESS Boundary, MEMORY_CACHING_TYPE Cache)
{
    CHECK(!Grant && Bytes == MTK_CONN_EMI_ALIGN && Low.QuadPart == 0x40000000 &&
          High.QuadPart == 0x7fffffff && Boundary.QuadPart == MTK_CONN_EMI_ALIGN &&
          Cache == MmNonCached);
    if (FailAllocation) {
        return NULL;
    }
    Grant = malloc(Bytes);
    CHECK(Grant);
    memset(Grant, GUARD, Bytes);
    ++Allocs;
    return Grant;
}

PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID Address)
{
    CHECK(Address == Grant);
    return (PHYSICAL_ADDRESS){.QuadPart = PhysicalBase};
}

VOID MmFreeContiguousMemory(PVOID Address)
{
    CHECK(Address == Grant && !PowerLease && !PinLease && !PmicActive &&
          PowerRefs == 1 && PinRefs == 1 && !BootOn(&Board));
    free(Grant);
    Grant = NULL;
    ++Frees;
}

static MTK_CONN_ROM_REGION Decode(volatile ULONG *Address, unsigned *Offset)
{
    if (Address == &ResetWord) {
        *Offset = 0x18U;
        return MtkConnRomRgu;
    }
    uintptr_t AddressValue = (uintptr_t)Address;
    CHECK(AddressValue >= (uintptr_t)Window && AddressValue < (uintptr_t)Window + sizeof(Window));
    unsigned Index = (unsigned)(AddressValue - (uintptr_t)Window);
    *Offset = Index & 0xfffU;
    switch (Index & ~0xfffU) {
    case 0xb1000U: return MtkConnRomTop;
    case 0x02000U: return MtkConnRomMcuCfg;
    case 0x07000U: return MtkConnHostCsr;
    case 0x04000U: return MtkConnSysram;
    case 0xc0000U: return MtkConnMcuMem;
    case 0xc6000U: return MtkConnRfSpi;
    default: CHECK(0); return MtkConnRomRgu;
    }
}

ULONG READ_REGISTER_ULONG(volatile ULONG *Address)
{
    unsigned Offset, Value;
    MTK_CONN_ROM_REGION Region = Decode(Address, &Offset);
    CHECK(PowerLease && PinLease);
    CHECK(!BootRead32(&Board, Region, Offset, &Value));
    return Value;
}

VOID WRITE_REGISTER_ULONG(volatile ULONG *Address, ULONG Value)
{
    unsigned Offset;
    MTK_CONN_ROM_REGION Region = Decode(Address, &Offset);
    CHECK(PowerLease && PinLease && PmicActive);
    CHECK(!BootWrite32(&Board, Region, Offset, Value));
}

VOID KeMemoryBarrier(VOID)
{
    if (Board.Writes > Board.Fences) {
        BootFence(&Board);
    }
}

VOID RtlCopyMemory(PVOID To, const void *From, SIZE_T Bytes)
{
    uintptr_t Destination = (uintptr_t)To, Source = (uintptr_t)From;
    if (Grant && Destination >= (uintptr_t)Grant && Destination < (uintptr_t)Grant + MTK_CONN_EMI_ALIGN) {
        CHECK(!BootWriteEmi(&Board, EMI_BASE + Destination - (uintptr_t)Grant, From, (unsigned)Bytes));
    } else if (Grant && Source >= (uintptr_t)Grant && Source < (uintptr_t)Grant + MTK_CONN_EMI_ALIGN) {
        CHECK(BootOn(&Board) && BootReset(&Board) && Board.Writes == Board.Fences &&
              Source - (uintptr_t)Grant + Bytes <= MTK_CONN_EMI_SIZE);
    }
    memcpy(To, From, Bytes);
}

UCHAR KeGetCurrentIrql(VOID)
{
    return (UCHAR)RaisedIrql;
}

static NTSTATUS WaitBoard(ULONG Microseconds)
{
    if (Microseconds == 270 || (Microseconds == 10 && !BootReset(&Board)) ||
        (Microseconds == 300 && Board.RuntimeAdie)) {
        CHECK(!WaitRail(&Board, Microseconds));
        return STATUS_SUCCESS;
    }
    if (Microseconds == 300) {
        Board.RailTime += Microseconds;
    }
    return BootWait(&Board, Microseconds) ? STATUS_IO_TIMEOUT : STATUS_SUCCESS;
}

VOID KeStallExecutionProcessor(ULONG Microseconds)
{
    CHECK(Microseconds <= 50 && WaitBoard(Microseconds) == STATUS_SUCCESS);
}

NTSTATUS KeDelayExecutionThread(int Mode, BOOLEAN Alertable, const LARGE_INTEGER *Delay)
{
    CHECK(Mode == KernelMode && !Alertable && Delay->QuadPart < -500 && Delay->QuadPart % 10 == 0);
    return WaitBoard((ULONG)(-Delay->QuadPart / 10));
}

NTSTATUS BCryptHash(PVOID Handle, PUCHAR Secret, ULONG SecretSize, PUCHAR Data,
    ULONG Bytes, PUCHAR Output, ULONG OutputSize)
{
    CHECK(Handle == BCRYPT_SHA256_ALG_HANDLE && !Secret && !SecretSize && OutputSize == 32);
    return BootHash(&Board, Data, Bytes, Output) ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}

static void AddReference(PVOID Context)
{
    unsigned *Refs = Context;
    CHECK((*Refs)++ == 1);
}

static void DropReference(PVOID Context)
{
    unsigned *Refs = Context;
    CHECK((*Refs)-- == 2);
}

static NTSTATUS AcquirePower(PVOID Context, ULONGLONG Base, PULONGLONG Token)
{
    CHECK(Context == &PowerRefs && PowerRefs == 2 && !PowerLease && Grant && Base == EMI_BASE);
    for (SIZE_T Index = 0; Index < MTK_CONN_EMI_ALIGN; ++Index) {
        assert(Grant[Index] == 0);
    }
    PowerLease = POWER_TOKEN;
    *Token = PowerLease;
    return STATUS_SUCCESS;
}

static NTSTATUS ReleasePower(PVOID Context, ULONGLONG Token)
{
    CHECK(Context == &PowerRefs && Token == POWER_TOKEN && Token == PowerLease &&
          !PmicActive && !PinLease && !BootOn(&Board));
    if (FailPowerRelease) {
        return STATUS_DEVICE_BUSY;
    }
    PowerLease = 0;
    return STATUS_SUCCESS;
}

static ULONG PowerField(MTK_CONN_REGION Region, ULONG Offset)
{
    if (Region == MtkConnSpm) {
        switch (Offset) {
        case 0: return 0xffffU;
        case 0x304: return 0x1fU;
        case 0x16c: case 0x170: return 2U;
        default: return 0;
        }
    }
    CHECK(Region == MtkConnInfra);
    switch (Offset) {
    case 0x0c8: return CcifBit;
    case 0x220: case 0x228: return 0x46000U;
    case 0x250: case 0x258: return 0x400U;
    case 0x380: case 0x388: return 0xfffffU;
    case 0x0f00: return 0x00000800U;
    default: return 0;
    }
}

static NTSTATUS PowerRead(PVOID Context, ULONGLONG Token, MTK_CONN_REGION Region, ULONG Address, PULONG Value)
{
    CHECK(Context == &PowerRefs && Token == POWER_TOKEN && Token == PowerLease);
    if (Region == MtkConnInfra && Address == MTK_CONN_BTIF_GATE_STATUS) {
        CHECK(!ResourcesRevoked && BootOn(&Board));
        *Value = BtifGate;
        return STATUS_SUCCESS;
    }
    ULONG Mask = PowerField(Region, Address);
    CHECK(Mask && !BootReadPower(&Board, Region, Address, Value));
    *Value &= Mask;
    return STATUS_SUCCESS;
}

static NTSTATUS PowerWrite(PVOID Context, ULONGLONG Token, MTK_CONN_REGION Region, ULONG Address, ULONG Value)
{
    CHECK(Context == &PowerRefs && Token == POWER_TOKEN && Token == PowerLease);
    if (Region == MtkConnInfra && Address == MTK_CONN_BTIF_GATE_CLEAR) {
        CHECK(Value == MTK_CONN_BTIF_GATE_MASK && !ResourcesRevoked && BootOn(&Board));
        ++BtifEnableCalls;
        BtifGate = 0;
        return FailBtifEnable ? STATUS_IO_TIMEOUT : STATUS_SUCCESS;
    }
    if (Region == MtkConnInfra && Address == MTK_CONN_BTIF_GATE_SET) {
        CHECK(Value == MTK_CONN_BTIF_GATE_MASK && !ResourcesRevoked);
        CHECK(!RestoreAfterPowerOff || !BootOn(&Board));
        ++BtifRestoreCalls;
        if (FailBtifRestore) {
            return STATUS_IO_DEVICE_ERROR;
        }
        BtifGate = MTK_CONN_BTIF_GATE_MASK;
        return STATUS_SUCCESS;
    }
    if (Region == MtkConnInfra && Address == 0x0f00U) {
        const ULONG Mask = 0x00000800U;
        CHECK((Value & ~Mask) == 0);
        ULONG *Register = &REG(&Board, BootInfra, Address);
        *Register = (*Register & ~Mask) | Value;
        if (Value == Mask && FailScoMaskWriteOnce) {
            FailScoMaskWriteOnce = FALSE;
            return STATUS_IO_DEVICE_ERROR;
        }
        if (Value == Device.ScoIrqMaskBaseline && FailScoRestoreOnce) {
            FailScoRestoreOnce = FALSE;
            return STATUS_IO_DEVICE_ERROR;
        }
        return STATUS_SUCCESS;
    }
    ULONG Mask = PowerField(Region, Address);
    if (Mask && (Region != MtkConnSpm || Address != 0)) {
        unsigned Bank = Region == MtkConnSpm ? BootSpm : BootInfra;
        CHECK(!(Value & ~Mask));
        Value |= REG(&Board, Bank, Address) & ~Mask;
    }
    return BootWritePower(&Board, Region, Address, Value) ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}

static NTSTATUS AcquirePins(PVOID Context, PULONGLONG Token)
{
    CHECK(Context == &PinRefs && PinRefs == 2 && !PinLease && PowerLease);
    if (FailPins) {
        return STATUS_DEVICE_BUSY;
    }
    PinLease = PINS_TOKEN;
    *Token = PinLease;
    return STATUS_SUCCESS;
}

static NTSTATUS ReleasePins(PVOID Context, ULONGLONG Token)
{
    CHECK(Context == &PinRefs && Token == PINS_TOKEN && Token == PinLease && !PmicActive && !BootOn(&Board));
    PinLease = 0;
    return STATUS_SUCCESS;
}

static ULONG PinField(MTK_CONN_PIN_SPACE Region, ULONG Offset)
{
    if (Region == MtkConnPinsIoCfg) {
        CHECK(Offset == 0x10);
        return 0x3ffffff8U;
    }
    CHECK(Region == MtkConnPinsGpio);
    switch (Offset) {
    case 0x20: case 0x120: return 0x10U;
    case 0x380: return 0xf0000U;
    case 0x460: return 0x77777770U;
    case 0x470: return 0x77U;
    case 0x50: case 0x54: case 0x58:
    case 0x150: case 0x154: case 0x158: return 0x02000000U;
    default: CHECK(0); return 0;
    }
}

static NTSTATUS PinsRead(PVOID Context, ULONGLONG Token, MTK_CONN_PIN_SPACE Region, ULONG Address, PULONG Value)
{
    CHECK(Context == &PinRefs && Token == PINS_TOKEN && Token == PinLease && PmicActive);
    if (Region == MtkConnPinsGpio && (Address == 0x20 || Address == 0x120 || Address == 0x380)) {
        CHECK(!ReadRailPin(&Board, Address, Value));
    } else {
        CHECK(!BootRead32(&Board, Region == MtkConnPinsGpio ? MtkConnGpio : MtkConnIoCfg, Address, Value));
    }
    *Value &= PinField(Region, Address);
    return STATUS_SUCCESS;
}

static NTSTATUS PinsWrite(PVOID Context, ULONGLONG Token, MTK_CONN_PIN_SPACE Region, ULONG Address, ULONG Value)
{
    CHECK(Context == &PinRefs && Token == PINS_TOKEN && Token == PinLease && PmicActive);
    int Error;
    ULONG Mask = PinField(Region, Address);
    MTK_CONN_ROM_REGION Bank = Region == MtkConnPinsGpio ? MtkConnGpio : MtkConnIoCfg;
    CHECK(!(Value & ~Mask));
    if (Address != 0x54 && Address != 0x58 && Address != 0x154 && Address != 0x158) {
        Value |= REG(&Board, Bank, Address) & ~Mask;
    }
    if (Region == MtkConnPinsGpio && (Address == 0x20 || Address == 0x120 || Address == 0x380)) {
        Error = WriteRailPin(&Board, Address, Value);
    } else {
        Error = BootWrite32(&Board, Bank, Address, Value);
    }
    return Error ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}

NTSTATUS T70ConnPmicAcquire(T70_CONN_PMIC *Pmic)
{
    CHECK(PowerLease && PinLease && !PmicActive && !Pmic->Lease);
    if (FailPmic) {
        return STATUS_DEVICE_NOT_READY;
    }
    PmicActive = Pmic->Lease = PMIC_TOKEN;
    Pmic->ModuleLocked = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS T70ConnPmicRelease(T70_CONN_PMIC *Pmic)
{
    CHECK(!BootOn(&Board) && Pmic->Lease == PmicActive);
    if (FailUnlock && PmicActive) {
        return STATUS_DEVICE_BUSY;
    }
    if (PmicActive) {
        CHECK(PinLease && PowerLease && Pmic->ModuleLocked && !Board.Rails &&
              !(PMIC(&Board, 0x1c58) & 1));
        PmicActive = 0;
        Pmic->Lease = 0;
        Pmic->ModuleLocked = FALSE;
    }
    return STATUS_SUCCESS;
}

int T70ConnPmicRead(void *Context, unsigned Address, unsigned *Value)
{
    CHECK(Context == &Device.Pmic && Device.Pmic.Lease == PmicActive && PmicActive);
    return ReadRailPmic(&Board, Address, Value);
}

int T70ConnPmicWrite(void *Context, unsigned Address, unsigned Value)
{
    CHECK(Context == &Device.Pmic && Device.Pmic.Lease == PmicActive && PmicActive);
    if (Device.Rails.RuntimeOwner) {
        Board.RuntimeAdie = Device.Rails.RuntimeAdie;
    }
    return WriteRailPmic(&Board, Address, Value);
}

static const MTK_CONN_POWER_INTERFACE Power = {
    {sizeof(Power), MTK_CONN_POWER_INTERFACE_VERSION, &PowerRefs, AddReference, DropReference},
    AcquirePower, PowerRead, PowerWrite, ReleasePower
};
static const MTK_CONN_PINS_INTERFACE Pins = {
    {sizeof(Pins), MTK_CONN_PINS_INTERFACE_VERSION, &PinRefs, AddReference, DropReference},
    AcquirePins, PinsRead, PinsWrite, ReleasePins
};

static CM_PARTIAL_RESOURCE_DESCRIPTOR Resource(LONGLONG Address, ULONG Length)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR R = {0};
    R.Type = CmResourceTypeMemory;
    R.ShareDisposition = CmResourceShareDeviceExclusive;
    R.u.Memory.Start.QuadPart = Address;
    R.u.Memory.Length = Length;
    return R;
}

static void Fresh(unsigned Chip)
{
    MTK_CONN_BOOT Scratch;
    CHECK(!Device.Window && !Device.Reset && !Grant);
    PowerRefs = PinRefs = 1;
    PhysicalBase = EMI_BASE;
    Maps = Unmaps = Allocs = Frees = 0;
    ResetBoot(&Board, &Scratch);
    BtifGate = MTK_CONN_BTIF_GATE_MASK;
    BtifEnableCalls = BtifRestoreCalls = 0;
    FailBtifEnable = FailBtifRestore = RestoreAfterPowerOff = FALSE;
    FailScoMaskWriteOnce = FailScoRestoreOnce = FALSE;
    Board.Chip = Chip;
    Board.IdleAfter = 2U;
    PMIC(&Board, 0x1720) = 1;
}

static void Prepare(void)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR W = Resource(0x18000000, sizeof(Window));
    CM_PARTIAL_RESOURCE_DESCRIPTOR R = Resource(0x10007018, sizeof(ResetWord));
    CM_PARTIAL_RESOURCE_DESCRIPTOR TW = Resource(WINDOW_PA, sizeof(Window));
    CM_PARTIAL_RESOURCE_DESCRIPTOR TR = Resource(RESET_PA, sizeof(ResetWord));
    R.Flags = CM_RESOURCE_MEMORY_READ_ONLY;
    CHECK(T70ConnPrepareHardware(&Device, &W, &TW, &R, &TR) == STATUS_DEVICE_CONFIGURATION_ERROR);
    R.Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    TR.u.Memory.Start.QuadPart = WINDOW_PA + 0x18;
    CHECK(T70ConnPrepareHardware(&Device, &W, &TW, &R, &TR) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!Maps && !Unmaps);
    TR.u.Memory.Start.QuadPart = RESET_PA;
    MapFailure = 2;
    CHECK(T70ConnPrepareHardware(&Device, &W, &TW, &R, &TR) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!Device.Window && !Device.Reset && Maps == 2 && Unmaps == 1);
    MapFailure = Maps = Unmaps = 0;
    CHECK(T70ConnPrepareHardware(&Device, &W, &TW, &R, &TR) == STATUS_SUCCESS);
    CHECK(T70ConnPrepareHardware(&Device, &W, &TW, &R, &TR) == STATUS_INVALID_DEVICE_STATE);
    CHECK(Maps == 2 && !Unmaps && T70ConnCheck(&Device) == STATUS_DEVICE_NOT_READY);
}

static void Closed(void)
{
    CHECK(T70ConnStop(&Device) == STATUS_SUCCESS);
    CHECK(T70ConnReleaseHardware(&Device) == STATUS_SUCCESS);
    CHECK(T70ConnReleaseHardware(&Device) == STATUS_SUCCESS);
    CHECK(!Device.Window && !Device.Reset && !Device.Emi && !Device.EmiBase && !Device.Adie);
    CHECK(Maps == Unmaps && Allocs == Frees && PowerRefs == 1 && PinRefs == 1);
    CHECK(!BootOn(&Board) && !BootReset(&Board) && !Board.Rails);
    CHECK(REG(&Board, BootInfra, 0x380) == 0xdab06300 && REG(&Board, BootInfra, 0x388) == 0x4ff04400);
    CHECK(REG(&Board, MtkConnRomRgu, 0x18) == 0x92344021 && REG(&Board, MtkConnGpio, 0x460) == 0x82a2000fU);
}

typedef enum {
    RomIoOk, RomIoMissingDirectory, RomIoMissingFile, RomIoQueryError,
    RomIoWrongSize, RomIoAllocationFailure, RomIoReadError, RomIoShortRead
} ROM_IO_FAULT;

static struct {
    const char *Path;
    FILE *File;
    unsigned OpenAttempts, OpenFiles, ClosedFiles, Pools, Frees, Directories, ClosedDirectories;
    ROM_IO_FAULT Fault;
} RomIo;
static char TestDriver, TestDirectory;

NTSTATUS IoGetDriverDirectory(PDRIVER_OBJECT Driver, ULONG Type, ULONG Flags, HANDLE *Handle)
{
    CHECK(Driver == &TestDriver && Type == DriverDirectoryImage && !Flags);
    CHECK(!RomIo.Directories);
    if (RomIo.Fault == RomIoMissingDirectory) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    ++RomIo.Directories;
    *Handle = &TestDirectory;
    return STATUS_SUCCESS;
}

NTSTATUS ZwOpenFile(HANDLE *Handle, ULONG Access, OBJECT_ATTRIBUTES *Attrs,
                    IO_STATUS_BLOCK *Status, ULONG Share, ULONG Options)
{
    (void)Status;
    CHECK(RomIo.Directories == 1 && !RomIo.ClosedDirectories && !RomIo.File);
    CHECK(Attrs->RootDirectory == &TestDirectory && Attrs->Attributes ==
          (OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_DONT_REPARSE));
    CHECK(Access == (FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE));
    CHECK(Share == FILE_SHARE_READ && Options == (FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE));
    unsigned Index = RomIo.OpenAttempts++;
    CHECK(Index < 2 * MtkConnRomPartCount);
    char Expected[40], Actual[40], Path[1024];
    int Bytes = snprintf(Expected, sizeof(Expected), "soc2_2_ram_%s_%s_1_hdr.bin",
                         Name[Index % MtkConnRomPartCount], Index < MtkConnRomPartCount ? "1d" : "1e");
    CHECK(Bytes > 0 && (size_t)Bytes < sizeof(Expected));
    CHECK(wcstombs(Actual, Attrs->ObjectName->Buffer, sizeof(Actual)) == (size_t)Bytes);
    CHECK(!strcmp(Expected, Actual));
    if (Index == 5 && RomIo.Fault == RomIoMissingFile) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    Bytes = snprintf(Path, sizeof(Path), "%s/%s", RomIo.Path, Actual);
    CHECK(Bytes > 0 && (size_t)Bytes < sizeof(Path));
    RomIo.File = fopen(Path, "rb");
    CHECK(RomIo.File);
    ++RomIo.OpenFiles;
    *Handle = RomIo.File;
    return STATUS_SUCCESS;
}

NTSTATUS ZwQueryInformationFile(HANDLE File, IO_STATUS_BLOCK *Status, void *Output,
                                ULONG Bytes, ULONG Class)
{
    CHECK(File == RomIo.File && Bytes == sizeof(FILE_STANDARD_INFORMATION) && Class == FileStandardInformation);
    if (RomIo.OpenAttempts == 6 && RomIo.Fault == RomIoQueryError) {
        return STATUS_IO_DEVICE_ERROR;
    }
    CHECK(!fseek(File, 0, SEEK_END));
    long Size = ftell(File);
    CHECK(Size > 0 && !fseek(File, 0, SEEK_SET));
    if (RomIo.OpenAttempts == 6 && RomIo.Fault == RomIoWrongSize) {
        ++Size;
    }
    *(FILE_STANDARD_INFORMATION *)Output = (FILE_STANDARD_INFORMATION){{.QuadPart = Size}, FALSE};
    Status->Information = Bytes;
    return STATUS_SUCCESS;
}

NTSTATUS ZwReadFile(HANDLE File, HANDLE Event, void *Routine, void *Context,
                    IO_STATUS_BLOCK *Status, void *Data, ULONG Bytes, LARGE_INTEGER *Offset, ULONG *Key)
{
    CHECK(File == RomIo.File && !Event && !Routine && !Context && !Offset->QuadPart && !Key);
    if (RomIo.OpenAttempts == 6 && RomIo.Fault == RomIoReadError) {
        return STATUS_IO_DEVICE_ERROR;
    }
    ULONG Requested = Bytes - (RomIo.OpenAttempts == 6 && RomIo.Fault == RomIoShortRead);
    Status->Information = fread(Data, 1, Requested, File);
    CHECK(Status->Information == Requested);
    return STATUS_SUCCESS;
}

NTSTATUS ZwClose(HANDLE Handle)
{
    if (Handle == &TestDirectory) {
        CHECK(!RomIo.File && ++RomIo.ClosedDirectories == RomIo.Directories);
    } else {
        CHECK(Handle == RomIo.File && !fclose(RomIo.File));
        RomIo.File = NULL;
        ++RomIo.ClosedFiles;
    }
    return STATUS_SUCCESS;
}

PVOID ExAllocatePool2(ULONGLONG Flags, SIZE_T Bytes, ULONG Tag)
{
    CHECK(Flags == POOL_FLAG_PAGED && Bytes && Tag == 'mFnC' && RomIo.File);
    if (RomIo.OpenAttempts == 6 && RomIo.Fault == RomIoAllocationFailure) {
        return NULL;
    }
    PVOID Buffer = malloc(Bytes);
    CHECK(Buffer);
    ++RomIo.Pools;
    return Buffer;
}

VOID ExFreePoolWithTag(PVOID Buffer, ULONG Tag)
{
    CHECK(Buffer && Tag == 'mFnC' && RomIo.Frees < RomIo.Pools);
    ++RomIo.Frees;
    free(Buffer);
}

static void CheckFirmwareFailures(const char *Directory)
{
    static const NTSTATUS Expected[] = {
        [RomIoMissingDirectory] = STATUS_OBJECT_NAME_NOT_FOUND,
        [RomIoMissingFile] = STATUS_OBJECT_NAME_NOT_FOUND,
        [RomIoQueryError] = STATUS_IO_DEVICE_ERROR,
        [RomIoWrongSize] = STATUS_INVALID_IMAGE_FORMAT,
        [RomIoAllocationFailure] = STATUS_INSUFFICIENT_RESOURCES,
        [RomIoReadError] = STATUS_IO_DEVICE_ERROR,
        [RomIoShortRead] = STATUS_END_OF_FILE
    };
    for (unsigned Fault = RomIoMissingDirectory; Fault < RTL_NUMBER_OF(Expected); ++Fault) {
        T70_CONN_FIRMWARE Firmware = {0};
        memset(&RomIo, 0, sizeof(RomIo));
        RomIo.Path = Directory;
        RomIo.Fault = Fault;
        CHECK(T70ConnLoadFirmware(&TestDriver, &Firmware) == Expected[Fault]);
        CHECK(RomIo.OpenFiles == RomIo.ClosedFiles && RomIo.Pools == RomIo.Frees &&
              RomIo.Directories == RomIo.ClosedDirectories);
        for (unsigned Variant = 0; Variant < 2; ++Variant) {
            for (unsigned Part = 0; Part < MtkConnRomPartCount; ++Part) {
                CHECK(!Firmware.Images[Variant].Image[Part].Data && !Firmware.Images[Variant].Image[Part].Bytes);
            }
        }
        T70ConnFreeFirmware(&Firmware);
    }
    CHECK(!MtkConnRomImageBytes(0x6632U, MtkConnRomMcu));
    CHECK(!MtkConnRomImageBytes(0x6631U, MtkConnRomPartCount));
}

static void TestScoIrqMask(const MTK_CONN_ROM_SET Images[2])
{
    Fresh(0x6631U);
    Prepare();
    REG(&Board, BootInfra, 0x0f00U) = 0x12345678U;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Images) == STATUS_SUCCESS);
    CHECK(Device.ScoIrqMaskCaptured && Device.ScoIrqMaskBaseline == 0U &&
          REG(&Board, BootInfra, 0x0f00U) == 0x12345e78U);
    CHECK(T70ConnStop(&Device) == STATUS_SUCCESS);
    CHECK(!Device.ScoIrqMaskCaptured && REG(&Board, BootInfra, 0x0f00U) == 0x12345678U);
    Closed();

    Fresh(0x6631U);
    Prepare();
    REG(&Board, BootInfra, 0x0f00U) = 0x12345e78U;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Images) == STATUS_SUCCESS);
    CHECK(Device.ScoIrqMaskCaptured && Device.ScoIrqMaskBaseline == 0x800U);
    FailScoRestoreOnce = TRUE;
    CHECK(T70ConnStop(&Device) == STATUS_IO_DEVICE_ERROR &&
          Device.ScoIrqMaskCaptured && PowerLease && Grant);
    CHECK(T70ConnStop(&Device) == STATUS_SUCCESS &&
          !Device.ScoIrqMaskCaptured && REG(&Board, BootInfra, 0x0f00U) == 0x12345e78U);
    Closed();

    Fresh(0x6635U);
    Prepare();
    REG(&Board, BootInfra, 0x0f00U) = 0x12345678U;
    FailScoMaskWriteOnce = TRUE;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Images) == STATUS_IO_DEVICE_ERROR &&
          REG(&Board, BootInfra, 0x0f00U) == 0x12345678U &&
          !Device.ScoIrqMaskCaptured);
    Closed();
}

static void TestBtifClock(const MTK_CONN_ROM_SET Images[2])
{
    Fresh(0x6631U);
    Prepare();
    CHECK(T70ConnEnableBtifClock(&Device) == STATUS_DEVICE_NOT_READY && !BtifEnableCalls);
    CHECK(T70ConnStart(&Device, &Power, &Pins, Images) == STATUS_SUCCESS);
    CHECK(T70ConnEnableBtifClock(&Device) == STATUS_SUCCESS && !BtifGate && Device.BtifRestore);
    CHECK(T70ConnRestoreBtifClock(&Device) == STATUS_SUCCESS);
    CHECK(BtifGate == MTK_CONN_BTIF_GATE_MASK && !Device.BtifRestore && BtifRestoreCalls == 1);
    CHECK(T70ConnRestoreBtifClock(&Device) == STATUS_SUCCESS && BtifRestoreCalls == 1);

    // An inherited live clock still goes through the parent checks.
    BtifGate = 0;
    CHECK(T70ConnEnableBtifClock(&Device) == STATUS_SUCCESS && !Device.BtifRestore && BtifEnableCalls == 2);
    CHECK(T70ConnRestoreBtifClock(&Device) == STATUS_SUCCESS && !BtifGate && BtifRestoreCalls == 1);
    Closed();
    CHECK(!BtifGate && BtifRestoreCalls == 1);

    Fresh(0x6635U);
    Prepare();
    CHECK(T70ConnStart(&Device, &Power, &Pins, Images) == STATUS_SUCCESS);
    FailBtifEnable = FailBtifRestore = TRUE;
    CHECK(T70ConnEnableBtifClock(&Device) == STATUS_IO_DEVICE_ERROR);
    CHECK(!BtifGate && Device.BtifRestore && Grant && BtifRestoreCalls == 1);

    // A retry uses the original inherited gate even after an ambiguous write.
    FailBtifEnable = FailBtifRestore = FALSE;
    CHECK(T70ConnEnableBtifClock(&Device) == STATUS_SUCCESS);
    CHECK(!BtifGate && Device.BtifRestore && BtifRestoreCalls == 2);
    ResourcesRevoked = TRUE;
    CHECK(T70ConnReleaseHardware(&Device) == STATUS_DEVICE_BUSY);
    CHECK(T70ConnRestoreBtifClock(&Device) == STATUS_DEVICE_NOT_READY);
    CHECK(!BtifGate && Device.BtifRestore && BtifRestoreCalls == 2);
    ResourcesRevoked = FALSE;
    CM_PARTIAL_RESOURCE_DESCRIPTOR W = Resource(0x18000000, sizeof(Window));
    CM_PARTIAL_RESOURCE_DESCRIPTOR R = Resource(0x10007018, sizeof(ResetWord));
    CM_PARTIAL_RESOURCE_DESCRIPTOR TW = Resource(WINDOW_PA, sizeof(Window));
    CM_PARTIAL_RESOURCE_DESCRIPTOR TR = Resource(RESET_PA, sizeof(ResetWord));
    CHECK(T70ConnPrepareHardware(&Device, &W, &TW, &R, &TR) == STATUS_SUCCESS);
    RestoreAfterPowerOff = FailBtifRestore = TRUE;
    CHECK(T70ConnStop(&Device) == STATUS_IO_DEVICE_ERROR);
    CHECK(!BootOn(&Board) && Device.BtifRestore && Grant && PowerLease && BtifRestoreCalls == 3);
    FailBtifRestore = FALSE;
    Closed();
    CHECK(BtifGate == MTK_CONN_BTIF_GATE_MASK && !Device.BtifRestore && BtifRestoreCalls == 4);
}

int main(int Argc, char **Argv)
{
    CHECK(Argc == 2);
    CheckFirmwareFailures(Argv[1]);
    memset(&RomIo, 0, sizeof(RomIo));
    RomIo.Path = Argv[1];
    T70_CONN_FIRMWARE Firmware = {0};
    CHECK(T70ConnLoadFirmware(&TestDriver, &Firmware) == STATUS_SUCCESS);
    CHECK(RomIo.OpenFiles == 6 && RomIo.ClosedFiles == 6 && RomIo.ClosedDirectories == 1 && RomIo.Pools == 6);
    CHECK(T70ConnLoadFirmware(&TestDriver, &Firmware) == STATUS_INVALID_DEVICE_STATE);
    MTK_CONN_ROM_SET *Set = Firmware.Images;
    for (unsigned Variant = 0; Variant < 2; ++Variant) {
        unsigned Chip = Variant == 0 ? 0x6631U : 0x6635U;
        Fresh(Chip);
        Prepare();
        CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_SUCCESS);
        CHECK(Device.Adie == Chip && T70ConnCheck(&Device) == STATUS_SUCCESS);
        unsigned Value;
        CHECK(Device.Boot.Io.Rom.Read32(&Device, MtkConnRomTop, 3, &Value) == STATUS_INVALID_PARAMETER);
        CHECK(Device.Boot.Io.Rom.Read32(&Device, MtkConnRomRgu, 0x24, &Value) == STATUS_INVALID_PARAMETER);
        CHECK(Device.Boot.Io.Write32(&Device, MtkConnRomRgu, 0x18, 0) == STATUS_INVALID_PARAMETER);
        CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_INVALID_DEVICE_STATE);
        CHECK(Board.ProbeAttempts == Variant + 1 && Board.IdleWait == 2 && Board.Starts == 1 &&
              Board.Copies == 3 && Board.Hashes == 3);
        for (unsigned Part = 0; Part < MtkConnRomPartCount; ++Part) {
            const MTK_CONN_ROM_IMAGE *Image = &Set[Variant].Image[Part];
            CHECK(!memcmp(Grant + Offset[Part], Image->Data + HEADER, Image->Bytes - HEADER));
        }
        for (SIZE_T Index = MTK_CONN_EMI_SIZE; Index < MTK_CONN_EMI_ALIGN; ++Index) {
            assert(Grant[Index] == 0);
        }
        Board.FailVcn18Off = 1;
        CHECK(T70ConnStop(&Device) == STATUS_IO_DEVICE_ERROR);
        CHECK(!Unmaps && !Frees && Grant && PinLease && PowerLease && PmicActive &&
              !BootOn(&Board) && T70ConnCheck(&Device) == STATUS_DEVICE_NOT_READY);
        Board.FailVcn18Off = 0;
        FailUnlock = 1;
        CHECK(T70ConnStop(&Device) == STATUS_DEVICE_BUSY && Grant && PinLease && PowerLease && PmicActive);
        FailUnlock = 0;
        FailPowerRelease = 1;
        CHECK(T70ConnStop(&Device) == STATUS_DEVICE_BUSY && Grant && PowerLease && !PinLease && !PmicActive);
        CHECK(!Unmaps && !Frees && PowerRefs == 2 && PinRefs == 2);
        FailPowerRelease = 0;
        Closed();
    }

    Fresh(0x6631U);
    Prepare();
    RaisedIrql = 2;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_INVALID_PARAMETER);
    RaisedIrql = 0;
    FailAllocation = 1;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_INSUFFICIENT_RESOURCES);
    FailAllocation = 0;
    PhysicalBase += 0x1000;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_DEVICE_CONFIGURATION_ERROR);
    PhysicalBase = EMI_BASE;
    FailPins = 1;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_DEVICE_BUSY);
    FailPins = 0;
    FailPmic = 1;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_DEVICE_NOT_READY);
    FailPmic = 0;
    CHECK(!Grant && !Board.Writes && Allocs == 3 && Frees == 3);
    Closed();

    Fresh(0x6635U);
    Prepare();
    MTK_CONN_ROM_IMAGE *Bad = &Set[1].Image[MtkConnRomMcu];
    unsigned char *Last = (unsigned char *)Bad->Data + Bad->Bytes - 1;
    *Last ^= 1;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_INVALID_PARAMETER);
    CHECK(Board.Hashes == 3 && !Board.Copies && !Board.Starts && !Grant);
    *Last ^= 1;
    Closed();

    Fresh(0x6631U);
    Prepare();
    Board.IdleAfter = 100;
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_IO_TIMEOUT);
    CHECK(Board.Starts == 1 && !Grant && !Board.StartsAtIdle);
    Closed();

    Fresh(0x6635U);
    Prepare();
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_SUCCESS);
    unsigned Before = Board.Writes;
    ResourcesRevoked = TRUE;
    CHECK(T70ConnReleaseHardware(&Device) == STATUS_DEVICE_BUSY);
    CHECK(T70ConnReleaseHardware(&Device) == STATUS_DEVICE_BUSY);
    CHECK(T70ConnCheck(&Device) == STATUS_DEVICE_NOT_READY);
    CHECK(T70ConnStop(&Device) == STATUS_DEVICE_NOT_READY);
    CHECK(!Device.Window && !Device.Reset && Maps == Unmaps && !Frees &&
          Grant && PowerRefs == 2 && PinRefs == 2 && Board.Writes == Before);
    ResourcesRevoked = FALSE;

    CM_PARTIAL_RESOURCE_DESCRIPTOR W = Resource(0x18000000, sizeof(Window));
    CM_PARTIAL_RESOURCE_DESCRIPTOR R = Resource(0x10007018, sizeof(ResetWord));
    CM_PARTIAL_RESOURCE_DESCRIPTOR TW = Resource(WINDOW_PA, sizeof(Window));
    CM_PARTIAL_RESOURCE_DESCRIPTOR TR = Resource(RESET_PA, sizeof(ResetWord));
    CHECK(T70ConnPrepareHardware(&Device, &W, &TW, &R, &TR) == STATUS_SUCCESS);
    CHECK(T70ConnStart(&Device, &Power, &Pins, Set) == STATUS_INVALID_DEVICE_STATE);
    Closed();

    TestScoIrqMask(Set);
    TestBtifClock(Set);
    T70ConnFreeFirmware(&Firmware);
    CHECK(RomIo.Frees == 6);
    T70ConnFreeFirmware(&Firmware);
    CHECK(RomIo.Frees == 6);
    printf("Windows CONN bootstrap: %u checks passed using stock ROMs and mocked kernel providers\n", Checks);
    return 0;
}
