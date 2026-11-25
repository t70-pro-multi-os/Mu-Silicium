// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/MtkClock.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MtkUsbPhyCore.h>
#include <Library/MtkUsbPhyPlatformLib.h>
#include <libfdt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native-only BaseLib/BaseMemoryLib shims for the included production C. */
UINTN EFIAPI
AsciiStrLen (IN CONST CHAR8 *String)
{
  return (UINTN)strlen (String);
}

INTN EFIAPI
AsciiStrCmp (IN CONST CHAR8 *First, IN CONST CHAR8 *Second)
{
  return (INTN)strcmp (First, Second);
}

VOID *EFIAPI
ZeroMem (OUT VOID *Buffer, IN UINTN Length)
{
  return memset (Buffer, 0, Length);
}

VOID *EFIAPI
CopyMem (OUT VOID *Destination, IN CONST VOID *Source, IN UINTN Length)
{
  return memmove (Destination, Source, Length);
}

INTN EFIAPI
CompareMem (IN CONST VOID *First, IN CONST VOID *Second, IN UINTN Length)
{
  return (INTN)memcmp (First, Second, Length);
}

#include "../Library/MtkUsbPhyPlatformLib/MtkUsbPhyPlatform.c"

#define CHECK(Condition) do { \
  ++Checks; \
  if (!(Condition)) { \
    fprintf (stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #Condition); \
    abort (); \
  } \
} while (0)

static unsigned Checks;
static unsigned MapCalls;
static unsigned MmioReads;
static unsigned MmioWrites;
static unsigned ClockGetIdCalls;
static unsigned ClockGetNameCalls;
static unsigned ClockFrequencyCalls;
static unsigned ClockEnabledCalls;
static unsigned ClockSetCalls;
static unsigned NvmemCalls;
EFI_GUID gFdtTableGuid = FDT_TABLE_GUID;
static CONST VOID *PublishedBootFdt;
static UINTN PublishedBootBytes;
static UINT32 BootFault;
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(IN EFI_GUID *Guid, OUT VOID **Table)
{
  CHECK(Guid == &gFdtTableGuid);
  *Table = (VOID *)PublishedBootFdt;
  return PublishedBootFdt != NULL && BootFault != 5 ? EFI_SUCCESS : EFI_NOT_FOUND;
}
UINT64 EFIAPI MmioRead64(IN UINTN Address)
{
  if (Address == 0x43F7F000) return BootFault == 1 ? 0 : 0x0000000148303754ULL;
  if (Address == 0x43F7F048) return (UINT64)(UINTN)PublishedBootFdt + (BootFault == 2 ? 8 : 0);
  if (Address == 0x43F7F050) return PublishedBootBytes + (BootFault == 3 ? 4 : 0);
  CHECK(Address == 0x43F7F058);
  return BootFault == 4 ? EFI_NOT_READY : EFI_SUCCESS;
}
static UINT64 MapBase = T70_PHY_BASE;
static UINT64 MapBytes = 0x10000ULL;
static UINT64 ClockFrequencyValue = T70_REF_CLOCK_HZ;
static BOOLEAN ClockEnabledValue = TRUE;
static EFI_STATUS MapStatus = EFI_SUCCESS;
static EFI_STATUS ClockApiStatus = EFI_SUCCESS;
static EFI_STATUS ClockSetStatus = EFI_SUCCESS;
static EFI_STATUS NvmemStatus = MtkUsbPhySuccess;
static UINT32 LastClockId;
static BOOLEAN LastClockEnable;
static UINT32 PhyRegisters[T70_PHY_BYTES / sizeof (UINT32)];
static UINT32 SyntheticRaw = 0xc3b158aeU;
static UINT32 LastNvmemMask;
static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS = &DxeServices;

static EFI_STATUS EFIAPI
FakeGetMemorySpaceDescriptor (
  IN EFI_PHYSICAL_ADDRESS Address,
  OUT EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor
  )
{
  ++MapCalls;
  CHECK (Address == T70_PHY_BASE);
  if (EFI_ERROR (MapStatus)) return MapStatus;
  ZeroMem (Descriptor, sizeof (*Descriptor));
  Descriptor->BaseAddress = MapBase;
  Descriptor->Length = MapBytes;
  Descriptor->GcdMemoryType = EfiGcdMemoryTypeMemoryMappedIo;
  Descriptor->Attributes = EFI_MEMORY_UC;
  return EFI_SUCCESS;
}

UINT32 EFIAPI
MmioRead32 (IN UINTN Address)
{
  ++MmioReads;
  CHECK ((Address >= T70_PHY_BASE) &&
         ((Address - T70_PHY_BASE) < T70_PHY_BYTES) &&
         (((Address - T70_PHY_BASE) & 3U) == 0));
  return PhyRegisters[(Address - T70_PHY_BASE) / sizeof (UINT32)];
}

UINT32 EFIAPI
MmioWrite32 (IN UINTN Address, IN UINT32 Value)
{
  ++MmioWrites;
  CHECK ((Address >= T70_PHY_BASE) &&
         ((Address - T70_PHY_BASE) < T70_PHY_BYTES) &&
         (((Address - T70_PHY_BASE) & 3U) == 0));
  PhyRegisters[(Address - T70_PHY_BASE) / sizeof (UINT32)] = Value;
  return Value;
}

UINT64 EFIAPI
GetPerformanceCounter (VOID)
{
  return 500U;
}

UINT64 EFIAPI
GetTimeInNanoSecond (IN UINT64 Ticks)
{
  return Ticks * 1000ULL;
}

UINTN EFIAPI
MicroSecondDelay (IN UINTN Microseconds)
{
  return Microseconds;
}

static EFI_STATUS EFIAPI
FakeClockGetId (IN CONST CHAR8 *Name, OUT UINT32 *ClockId)
{
  ++ClockGetIdCalls;
  if (EFI_ERROR (ClockApiStatus)) return ClockApiStatus;
  CHECK (AsciiStrCmp (Name, T70_REF_CLOCK_NAME) == 0);
  *ClockId = 0x26U;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeClockGetName (IN UINT32 ClockId, OUT CONST CHAR8 **Name)
{
  ++ClockGetNameCalls;
  if (EFI_ERROR (ClockApiStatus)) return ClockApiStatus;
  CHECK (ClockId == 0x26U);
  *Name = T70_REF_CLOCK_NAME;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeClockSetEnable (IN UINT32 ClockId, IN BOOLEAN Enable)
{
  ++ClockSetCalls;
  LastClockId = ClockId;
  LastClockEnable = Enable;
  return ClockSetStatus;
}

static EFI_STATUS EFIAPI
FakeClockIsEnabled (IN UINT32 ClockId, OUT BOOLEAN *Enabled)
{
  ++ClockEnabledCalls;
  if (EFI_ERROR (ClockApiStatus)) return ClockApiStatus;
  CHECK (ClockId == 0x26U);
  *Enabled = ClockEnabledValue;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeClockGetFrequency (IN UINT32 ClockId, OUT UINT64 *Frequency)
{
  ++ClockFrequencyCalls;
  if (EFI_ERROR (ClockApiStatus)) return ClockApiStatus;
  CHECK (ClockId == 0x26U);
  *Frequency = ClockFrequencyValue;
  return EFI_SUCCESS;
}

static MTK_CLOCK_PROTOCOL FakeClock = {
  .GetId = FakeClockGetId,
  .GetName = FakeClockGetName,
  .SetEnable = FakeClockSetEnable,
  .IsEnabled = FakeClockIsEnabled,
  .GetFrequency = FakeClockGetFrequency,
  .SetFrequency = NULL,
  .GetParent = NULL,
  .SetParent = NULL
};

static MTK_USB_PHY_STATUS
FakeReadOnlyNvmem (
  IN VOID *Context,
  IN CONST CHAR8 *Name,
  IN UINT32 Mask,
  OUT UINT32 *RawValue
  )
{
  ++NvmemCalls;
  CHECK (Context == (VOID *)(UINTN)0x1234U);
  CHECK (AsciiStrCmp (Name, "intr_cal") == 0);
  LastNvmemMask = Mask;
  if (NvmemStatus == MtkUsbPhySuccess) *RawValue = SyntheticRaw;
  return NvmemStatus;
}

static VOID
ResetMocks (VOID)
{
  if (mT70Phy.Active) {
    mT70Phy.ClockPreparedMask = 0;
    mT70Phy.ClockEnabledMask = 0;
    CHECK (MtkUsbPhyPlatformUnbind () == EFI_SUCCESS);
  }
  PublishedBootFdt = NULL; PublishedBootBytes = 0; BootFault = 0;
  MapCalls = MmioReads = MmioWrites = 0;
  ClockGetIdCalls = ClockGetNameCalls = ClockFrequencyCalls = 0;
  ClockEnabledCalls = ClockSetCalls = NvmemCalls = 0;
  MapBase = T70_PHY_BASE;
  MapBytes = 0x10000ULL;
  ClockFrequencyValue = T70_REF_CLOCK_HZ;
  MapStatus = ClockApiStatus = ClockSetStatus = EFI_SUCCESS;
  ClockEnabledValue = TRUE;
  NvmemStatus = MtkUsbPhySuccess;
  LastClockId = 0;
  LastClockEnable = FALSE;
  LastNvmemMask = 0;
  ZeroMem (PhyRegisters, sizeof (PhyRegisters));
  ZeroMem (&DxeServices, sizeof (DxeServices));
  DxeServices.GetMemorySpaceDescriptor = FakeGetMemorySpaceDescriptor;
}

static VOID *
OpenTree (IN CONST VOID *Original, IN UINTN OriginalBytes, OUT UINTN *Bytes)
{
  VOID *Tree = malloc (OriginalBytes + 4096U);
  CHECK (Tree != NULL);
  CHECK (fdt_open_into (Original, Tree, OriginalBytes + 4096U) == 0);
  *Bytes = fdt_totalsize (Tree);
  return Tree;
}

static VOID
SetU32 (
  IN VOID *Tree,
  IN CONST CHAR8 *Path,
  IN CONST CHAR8 *Name,
  IN UINT32 Value
  )
{
  fdt32_t Wire = cpu_to_fdt32 (Value);
  INT32 Node = fdt_path_offset (Tree, Path);
  CHECK (Node >= 0);
  CHECK (fdt_setprop (Tree, Node, Name, &Wire, sizeof (Wire)) == 0);
}

static VOID
CheckRejected (
  IN CONST VOID *Tree,
  IN UINTN Bytes
  )
{
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  MTK_USB_PHY_HOST_TUNING Host;
  EFI_STATUS Status;
  ResetMocks ();
  Status = MtkUsbPhyPlatformBind (Tree, Bytes, &FakeClock, FakeReadOnlyNvmem,
                                  (VOID *)(UINTN)0x1234U, &Io, &Config, &Host);
  CHECK (EFI_ERROR (Status));
  CHECK (MapCalls == 0);
  CHECK (ClockGetIdCalls == 0);
  CHECK (NvmemCalls == 0);
  CHECK (MmioReads == 0 && MmioWrites == 0);
}

static VOID
CheckU32Mutation (
  IN CONST VOID *Original,
  IN UINTN OriginalBytes,
  IN CONST CHAR8 *Path,
  IN CONST CHAR8 *Property,
  IN UINT32 Value
  )
{
  UINTN Bytes;
  VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
  SetU32 (Tree, Path, Property, Value);
  CheckRejected (Tree, Bytes);
  free (Tree);
}

static VOID
CheckTuningMutation (
  IN CONST VOID *Original,
  IN UINTN OriginalBytes,
  IN CONST CHAR8 *Property,
  IN UINT32 Value
  )
{
  UINTN Bytes;
  VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
  SetU32 (Tree, T70_PHY_NODE, Property, Value);
  CheckRejected (Tree, Bytes);
  free (Tree);
}

static VOID
CheckCoreLifecycleIntegration (IN CONST VOID *Tree, IN UINTN Bytes)
{
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  MTK_USB_PHY_HOST_TUNING Host;
  MTK_USB_PHY_CORE Core;
  EFI_STATUS Status;

  ResetMocks ();
  Status = MtkUsbPhyPlatformBind (Tree, Bytes, &FakeClock, FakeReadOnlyNvmem,
                                  (VOID *)(UINTN)0x1234U, &Io, &Config, &Host);
  CHECK (Status == EFI_SUCCESS);
  ZeroMem (&Core, sizeof (Core));
  CHECK (MtkUsbPhyCoreInitialize (&Core, &Io, &Config) == MtkUsbPhySuccess);
  CHECK (MtkUsbPhyCoreIsInitialized (&Core) != 0U);
  CHECK (MtkUsbPhyCoreIsPowered (&Core) == 0U);
  CHECK (NvmemCalls == 1U && LastNvmemMask == T70_INTR_CAL_MASK);
  CHECK (MmioReads > 0U && MmioWrites > 0U);
  CHECK (mT70Phy.ClockPreparedMask == 1U);
  CHECK (mT70Phy.ClockEnabledMask == 1U);
  CHECK (MtkUsbPhyCoreExit (&Core) == MtkUsbPhySuccess);
  CHECK (MtkUsbPhyCoreIsInitialized (&Core) == 0U);
  CHECK (mT70Phy.ClockPreparedMask == 0U);
  CHECK (mT70Phy.ClockEnabledMask == 0U);
  CHECK (ClockSetCalls == 0U);
  CHECK (MtkUsbPhyPlatformUnbind () == EFI_SUCCESS);
}

static VOID PutLe32(UINT8 *P, UINT32 V)
{ P[0]=V; P[1]=V>>8; P[2]=V>>16; P[3]=V>>24; }
static VOID CheckStockNvmem(CONST VOID *Original, UINTN OriginalBytes)
{
  UINTN Bytes, I;
  VOID *Tree=OpenTree(Original,OriginalBytes,&Bytes);
  UINT8 Tag[4+401*4];
  UINT32 Value;
  INT32 Chosen=fdt_path_offset(Tree,"/chosen");
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  MTK_USB_PHY_HOST_TUNING Host;
  MTK_USB_PHY_CORE Core;
  CHECK(Chosen >= 0);
  ZeroMem(Tag,sizeof(Tag)); PutLe32(Tag,109); PutLe32(Tag+4+T70_INTR_CAL_OFFSET,0xc3b158aeU);
  // Synthetic words replace the original property before any provider read.
  for (I=0; I<=444; I++) {
    ResetMocks(); PublishedBootFdt=Tree; PublishedBootBytes=Bytes;
    CHECK(fdt_setprop(Tree,Chosen,"atag,devinfo",Tag,I) == 0);
    CHECK(ReadBootIntrCal(Tree,Bytes,&Value) == (I>=440 ? MtkUsbPhySuccess : MtkUsbPhyNvmemError));
    CHECK(Value == (I>=440 ? 0xc3b158aeU : 0));
    CHECK(MmioReads == 0 && MmioWrites == 0 && MapCalls == 0 && ClockSetCalls == 0);
  }
  for (I=0; I<=401; I++) {
    ResetMocks(); PublishedBootFdt=Tree; PublishedBootBytes=Bytes;
    PutLe32(Tag,I); CHECK(fdt_setprop(Tree,Chosen,"atag,devinfo",Tag,4+I*4) == 0);
    CHECK(ReadBootIntrCal(Tree,Bytes,&Value) == ((I>=109 && I<=400) ? MtkUsbPhySuccess : MtkUsbPhyNvmemError));
  }
  PutLe32(Tag,109); CHECK(fdt_setprop(Tree,Chosen,"atag,devinfo",Tag,440) == 0);
  for (I=1; I<=5; I++) {
    ResetMocks(); PublishedBootFdt=Tree; PublishedBootBytes=Bytes; BootFault=I;
    CHECK(MtkUsbPhyPlatformBind(Tree,Bytes,&FakeClock,NULL,NULL,&Io,&Config,&Host) == EFI_NOT_READY);
    CHECK(MapCalls == 0 && ClockGetIdCalls == 0 && MmioReads == 0 && MmioWrites == 0);
  }
  ResetMocks(); PublishedBootFdt=Tree; PublishedBootBytes=Bytes;
  CHECK(MtkUsbPhyPlatformBind(Tree,Bytes,&FakeClock,NULL,NULL,&Io,&Config,&Host) == EFI_SUCCESS);
  CHECK(Io.ReadNvmemCell(Io.Context,"intr_cal",T70_INTR_CAL_MASK,&Value) == MtkUsbPhySuccess && Value == 0xc3b158aeU);
  BootFault=4;
  CHECK(Io.ReadNvmemCell(Io.Context,"intr_cal",T70_INTR_CAL_MASK,&Value) == MtkUsbPhyNotReady && Value == 0);
  BootFault=0; ZeroMem(&Core,sizeof(Core));
  CHECK(MtkUsbPhyCoreInitialize(&Core,&Io,&Config) == MtkUsbPhySuccess);
  CHECK(((PhyRegisters[0x304/4]>>19)&0x1f) == (0xc3b158aeU&0x1f));
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
  CHECK(MtkUsbPhyPlatformUnbind() == EFI_SUCCESS);
  ResetMocks(); PublishedBootFdt=Tree; PublishedBootBytes=Bytes;
  Tag[0]=0; Tag[1]=0; Tag[2]=0; Tag[3]=109; // FDT-cell endian is not the stock ATAG format.
  CHECK(fdt_setprop(Tree,Chosen,"atag,devinfo",Tag,440) == 0);
  CHECK(MtkUsbPhyPlatformBind(Tree,Bytes,&FakeClock,NULL,NULL,&Io,&Config,&Host) == EFI_COMPROMISED_DATA);
  CHECK(MapCalls == 0 && ClockGetIdCalls == 0);
  CHECK(fdt_delprop(Tree,Chosen,"atag,devinfo") == 0);
  CHECK(ReadBootIntrCal(Tree,Bytes,&Value) == MtkUsbPhyNotReady);
  CHECK(ReadBootIntrCal(Tree,Bytes,NULL) == MtkUsbPhyInvalid);
  ResetMocks(); free(Tree);
}

int
main (int Argc, char **Argv)
{
  FILE *File;
  long FileBytes;
  VOID *Original;
  UINTN OriginalBytes;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  MTK_USB_PHY_HOST_TUNING Host;
  MTK_USB_PHY_STATUS PhyStatus;
  EFI_STATUS Status;
  UINT8 Present;
  UINT32 RawValue;
  uint64_t Nanoseconds;

  CHECK (Argc == 2);
  File = fopen (Argv[1], "rb");
  CHECK (File != NULL);
  CHECK (fseek (File, 0, SEEK_END) == 0);
  FileBytes = ftell (File);
  CHECK (FileBytes > 0);
  rewind (File);
  Original = malloc ((UINTN)FileBytes);
  CHECK (Original != NULL);
  CHECK (fread (Original, 1, (UINTN)FileBytes, File) == (size_t)FileBytes);
  fclose (File);
  OriginalBytes = (UINTN)FileBytes;

  ResetMocks ();
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock, NULL, NULL,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_NOT_READY);
  CHECK (ClockGetIdCalls == 0 && MapCalls == 0);
  CHECK (MmioReads == 0 && MmioWrites == 0 && NvmemCalls == 0);

  ResetMocks ();
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_SUCCESS);
  CHECK (Io.Context == &mT70Phy);
  CHECK (Io.PhyBase == T70_PHY_BASE && Io.PhyLength == T70_PHY_BYTES);
  CHECK (Config.PhyType == MTK_USB_TPHY_T70_U2_TYPE);
  CHECK (Config.TphyVersion == MTK_USB_TPHY_VERSION_V2);
  CHECK (Config.SetEfuseV1 == 1U);
  CHECK (Config.TuningPresent == MTK_USB_TPHY_TUNING_T70_REQUIRED);
  CHECK (Config.EyeVrt == 4U && Config.EyeTerm == 4U && Config.Rev4 == 1U);
  CHECK (Config.DiscTh == 7U && Config.RxSqth == 5U);
  CHECK (Host.SlewControl == 0U && Host.EyeVrt == 0U && Host.EyeTerm == 0U);
  CHECK (Host.Rev6 == 0U && Host.DisconnectThreshold == 7U);
  CHECK (MapCalls == 1 && MmioReads == 0 && MmioWrites == 0);
  CHECK (NvmemCalls == 0 && ClockGetIdCalls == 1U);
  CHECK (ClockGetNameCalls == 1U && ClockFrequencyCalls == 1U);
  CHECK (ClockEnabledCalls == 1U && ClockSetCalls == 0U);

  PhyStatus = Io.ResolveClock (Io.Context, MtkUsbPhyClockRef, &Present);
  CHECK (PhyStatus == MtkUsbPhySuccess && Present == 1U);
  CHECK (Io.ResolveClock (Io.Context, (MTK_USB_PHY_CLOCK_SLOT)-1, &Present) ==
         MtkUsbPhyInvalid);
  CHECK (Io.ResolveClock (Io.Context, MtkUsbPhyClockCount, &Present) ==
         MtkUsbPhyInvalid);
  CHECK (Io.ClockPrepare (Io.Context, (MTK_USB_PHY_CLOCK_SLOT)-1) ==
         MtkUsbPhyInvalid);
  CHECK (Io.ClockPrepare (Io.Context, MtkUsbPhyClockCount) == MtkUsbPhyInvalid);
  CHECK (Io.ClockEnable (Io.Context, (MTK_USB_PHY_CLOCK_SLOT)-1) ==
         MtkUsbPhyInvalid);
  CHECK (Io.ClockEnable (Io.Context, MtkUsbPhyClockCount) == MtkUsbPhyInvalid);
  CHECK (Io.ClockDisable (Io.Context, (MTK_USB_PHY_CLOCK_SLOT)-1) ==
         MtkUsbPhyInvalid);
  CHECK (Io.ClockDisable (Io.Context, MtkUsbPhyClockCount) == MtkUsbPhyInvalid);
  CHECK (Io.ClockUnprepare (Io.Context, (MTK_USB_PHY_CLOCK_SLOT)-1) ==
         MtkUsbPhyInvalid);
  CHECK (Io.ClockUnprepare (Io.Context, MtkUsbPhyClockCount) ==
         MtkUsbPhyInvalid);
  PhyStatus = Io.ResolveClock (Io.Context, MtkUsbPhyClockDaRef, &Present);
  CHECK (PhyStatus == MtkUsbPhySuccess && Present == 0U);
  CHECK (Io.ClockEnable (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhyClockError);
  CHECK (Io.ClockPrepare (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhySuccess);
  ClockApiStatus = EFI_DEVICE_ERROR;
  CHECK (Io.ClockEnable (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhyClockError);
  CHECK (mT70Phy.ClockEnabledMask == 0U);
  ClockApiStatus = EFI_SUCCESS;
  ClockFrequencyValue = 26000001U;
  CHECK (Io.ClockEnable (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhyClockError);
  CHECK (mT70Phy.ClockEnabledMask == 0U);
  ClockFrequencyValue = T70_REF_CLOCK_HZ;
  ClockEnabledValue = FALSE;
  CHECK (Io.ClockEnable (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhyClockError);
  CHECK (mT70Phy.ClockEnabledMask == 0U);
  ClockEnabledValue = TRUE;
  CHECK (Io.ClockEnable (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhySuccess);
  CHECK (Io.ClockEnable (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhyClockError);
  CHECK (MtkUsbPhyPlatformUnbind () == EFI_NOT_READY);
  ClockApiStatus = EFI_DEVICE_ERROR;
  CHECK (Io.ClockDisable (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhySuccess);
  CHECK (mT70Phy.ClockEnabledMask == 0U);
  ClockApiStatus = EFI_SUCCESS;
  CHECK (Io.ClockUnprepare (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhySuccess);
  CHECK (Io.ClockUnprepare (Io.Context, MtkUsbPhyClockRef) == MtkUsbPhyClockError);
  CHECK (ClockSetCalls == 0U);

  RawValue = 0;
  PhyStatus = Io.ReadNvmemCell (Io.Context, "intr_cal", T70_INTR_CAL_MASK, &RawValue);
  CHECK (PhyStatus == MtkUsbPhySuccess && RawValue == SyntheticRaw);
  CHECK (NvmemCalls == 1);
  CHECK (LastNvmemMask == T70_INTR_CAL_MASK);
  RawValue = 0xffffffffU;
  CHECK (Io.ReadNvmemCell (Io.Context, "other", T70_INTR_CAL_MASK, &RawValue) ==
         MtkUsbPhyUnsupported);
  CHECK (RawValue == 0U && NvmemCalls == 1U);
  RawValue = 0xffffffffU;
  CHECK (Io.ReadNvmemCell (Io.Context, "intr_cal", 0x7fU, &RawValue) ==
         MtkUsbPhyUnsupported);
  CHECK (RawValue == 0U && NvmemCalls == 1U);
  NvmemStatus = MtkUsbPhyNvmemError;
  RawValue = 0xffffffffU;
  CHECK (Io.ReadNvmemCell (Io.Context, "intr_cal", T70_INTR_CAL_MASK, &RawValue) ==
         MtkUsbPhyNvmemError);
  CHECK (RawValue == 0U && NvmemCalls == 2U);

  PhyRegisters[0x40U / sizeof (UINT32)] = 0x12345678U;
  CHECK (Io.Read32 (Io.Context, 0x40U, &RawValue) == MtkUsbPhySuccess);
  CHECK (RawValue == 0x12345678U && MmioReads == 1U);
  CHECK (Io.Write32 (Io.Context, 0x44U, 0xabcdef01U) == MtkUsbPhySuccess);
  CHECK (PhyRegisters[0x44U / sizeof (UINT32)] == 0xabcdef01U && MmioWrites == 1U);
  CHECK (Io.Read32 (Io.Context, T70_PHY_BYTES, &RawValue) == MtkUsbPhyIoError);
  CHECK (Io.Write32 (Io.Context, T70_PHY_BYTES, 0) == MtkUsbPhyIoError);
  CHECK (Io.Read32 (Io.Context, 2U, &RawValue) == MtkUsbPhyIoError);
  CHECK (Io.NowNs (Io.Context, &Nanoseconds) == MtkUsbPhySuccess);
  CHECK (Nanoseconds == 500000ULL);
  CHECK (Io.DelayUs (Io.Context, 17U) == MtkUsbPhySuccess);
  CHECK (MtkUsbPhyPlatformUnbind () == EFI_SUCCESS);
  CHECK (Io.Read32 (Io.Context, 0, &RawValue) == MtkUsbPhyIoError);
  CHECK (MmioReads == 1U && MmioWrites == 1U);

  ResetMocks ();
  MapStatus = EFI_NOT_FOUND;
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_NOT_FOUND);
  CHECK (MapCalls == 1 && NvmemCalls == 0 && MmioReads == 0 && MmioWrites == 0);
  ResetMocks ();
  MapBase++;
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_UNSUPPORTED && MmioReads == 0 && MmioWrites == 0);
  ResetMocks ();
  MapBytes = T70_PHY_BYTES - 1U;
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_UNSUPPORTED && MmioReads == 0 && MmioWrites == 0);
  ResetMocks ();
  ClockApiStatus = EFI_DEVICE_ERROR;
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_DEVICE_ERROR && MapCalls == 0);
  ResetMocks ();
  ClockApiStatus = EFI_SUCCESS;
  FakeClock.GetFrequency = NULL;
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_NOT_READY && MapCalls == 0);
  FakeClock.GetFrequency = FakeClockGetFrequency;
  ResetMocks ();
  ClockFrequencyValue = 24000000ULL;
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_UNSUPPORTED && MapCalls == 0);
  ResetMocks ();
  // Clock protocol must resolve the same fixed 26 MHz source as the DT node.
  ClockApiStatus = EFI_SUCCESS;
  Status = MtkUsbPhyPlatformBind (Original, OriginalBytes, &FakeClock,
                                  FakeReadOnlyNvmem, (VOID *)(UINTN)0x1234U,
                                  &Io, &Config, &Host);
  CHECK (Status == EFI_SUCCESS);
  CHECK (MtkUsbPhyPlatformUnbind () == EFI_SUCCESS);

  CheckU32Mutation (Original, OriginalBytes, T70_USB_NODE, "phys", 0);
  {
    UINTN Bytes;
    VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
    fdt32_t Cells[2] = {cpu_to_fdt32 (0xb7U), cpu_to_fdt32 (4U)};
    INT32 Node = fdt_path_offset (Tree, T70_USB_NODE);
    CHECK (Node >= 0);
    CHECK (fdt_setprop (Tree, Node, "phys", Cells, sizeof (Cells)) == 0);
    CheckRejected (Tree, Bytes);
    free (Tree);
  }
  {
    UINTN Bytes;
    VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
    static CONST CHAR8 Names[] = "ref\0da_ref\0";
    INT32 Node = fdt_path_offset (Tree, T70_PHY_NODE);
    CHECK (Node >= 0);
    CHECK (fdt_setprop (Tree, Node, "clock-names", Names, sizeof (Names)) == 0);
    CheckRejected (Tree, Bytes);
    free (Tree);
  }
  {
    UINTN Bytes;
    VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
    static CONST CHAR8 Host[] = "host";
    INT32 Node = fdt_path_offset (Tree, T70_USB_NODE);
    CHECK (Node >= 0);
    CHECK (fdt_setprop (Tree, Node, "dr_mode", Host, sizeof (Host)) == 0);
    CheckRejected (Tree, Bytes);
    free (Tree);
  }
  {
    UINTN Bytes;
    VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
    CHECK (fdt_delprop (Tree, fdt_path_offset (Tree, T70_PHY_NODE),
                        "mediatek,set-efuse-v1") == 0);
    CheckRejected (Tree, Bytes);
    free (Tree);
  }
  {
    UINTN Bytes;
    VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
    static CONST CHAR8 BadTail[] = "mediatek,generic-tphy-v2\0tail";
    INT32 Node = fdt_path_offset (Tree, T70_PHY_BUS);
    CHECK (Node >= 0);
    CHECK (fdt_setprop (Tree, Node, "compatible", BadTail,
                        sizeof (BadTail) - 1U) == 0);
    CheckRejected (Tree, Bytes);
    free (Tree);
  }
  CheckTuningMutation (Original, OriginalBytes, "mediatek,eye-vrt", 8U);
  CheckTuningMutation (Original, OriginalBytes, "mediatek,eye-term", 8U);
  CheckTuningMutation (Original, OriginalBytes, "mediatek,discth", 16U);
  CheckTuningMutation (Original, OriginalBytes, "mediatek,rx-sqth", 16U);
  CheckTuningMutation (Original, OriginalBytes, "mediatek,rev4", 2U);
  CheckTuningMutation (Original, OriginalBytes, "mediatek,disc-host", 16U);
  CheckCoreLifecycleIntegration (Original, OriginalBytes);
  CheckU32Mutation (Original, OriginalBytes, T70_PHY_NODE, "nvmem-cell-masks", 0U);
  CheckU32Mutation (Original, OriginalBytes, T70_PHY_NODE, "nvmem-cells", 1U);
  CheckU32Mutation (Original, OriginalBytes, T70_PHY_NODE, "clocks", 1U);
  CheckU32Mutation (Original, OriginalBytes, T70_CLOCK_NODE, "clock-frequency", 19200000U);
  CheckU32Mutation (Original, OriginalBytes, T70_NVMEM_CELL_NODE, "reg", 0x1b4U);
  CheckU32Mutation (Original, OriginalBytes, T70_NVMEM_PROVIDER_NODE, "reg", 0U);
  {
    UINTN Bytes;
    VOID *Tree = OpenTree (Original, OriginalBytes, &Bytes);
    INT32 Node = fdt_path_offset (Tree, "/clocks");
    CHECK (Node >= 0);
    CHECK (fdt_setprop_string (Tree, Node, "status", "disabled") == 0);
    CheckRejected (Tree, Bytes);
    free (Tree);
  }

  CheckStockNvmem(Original,OriginalBytes);
  free (Original);
  printf ("MtkUsbPhyPlatformTest checks: %u\n", Checks);
  return 0;
}
