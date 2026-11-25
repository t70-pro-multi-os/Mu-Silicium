// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-side execution of the production UFS handoff DXE and runtime FDT gate.
#include "../MediaTekTest.h"
#include <sys/mman.h>
#include <libfdt.h>
#include <Guid/Fdt.h>
#include <Pi/PiDxeCis.h>
#include <Library/FdtLib.h>
#include <Library/MtkUfsCore.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Protocol/MtkClock.h>
#include <Protocol/MtkPmic.h>
#include <Protocol/UfsDeviceConfig.h>
#include <Library/ArmSmcLib.h>
#define HANDOFF_ZERO_POOL_ALLOCATOR_NAME  HandoffTestAllocateZeroPool
#define HANDOFF_POOL_FREE_NAME            HandoffTestFreePool
static UINTN HandoffZeroPoolAllocations;
static UINTN HandoffPoolFrees;
static VOID *EFIAPI HandoffTestAllocateZeroPool (UINTN Size) {
  HandoffZeroPoolAllocations++;
  return calloc (1, Size);
}
static VOID EFIAPI HandoffTestFreePool (VOID *Buffer) {
  HandoffPoolFrees++;
  free (Buffer);
}
#define AllocateZeroPool  HANDOFF_ZERO_POOL_ALLOCATOR_NAME
#define FreePool          HANDOFF_POOL_FREE_NAME
#include "../../../../../Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsHandoffDxe/MtkUfsHandoffDxe.c"
#undef AllocateZeroPool
#undef FreePool
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruHci.c"

#define HANDOFF_BASE       0x43F7F000ULL
#define HANDOFF_MAGIC     0x0000000148303754ULL
#define UFS_BASE          0x11270000ULL
#define UFS_LENGTH        0x2300ULL
#define DMA_LIMIT         0x1000000000ULL
#define TEST_DMA_BASE     0x20000000ULL
#define TEST_DMA_LIMIT    0x20010000ULL
#define TEST_MAX_PAGE_ALLOCATIONS  32U
#define TEST_TREE_CAP     8192
#define TEST_TREE_SIZE    2048
#define IMAGE_HANDLE      ((EFI_HANDLE)(UINTN)0x1010U)
#define SEED_HANDLE       ((EFI_HANDLE)(UINTN)0x2020U)
#define FDT_EVENT_HANDLE  ((EFI_EVENT)(UINTN)0x3030U)
#define EXIT_EVENT_HANDLE ((EFI_EVENT)(UINTN)0x5050U)
#define END_EVENT_HANDLE ((EFI_EVENT)(UINTN)0x6060U)
#define READY_EVENT_HANDLE ((EFI_EVENT)(UINTN)0x4040U)

EFI_GUID gFdtTableGuid = FDT_TABLE_GUID;
EFI_GUID gEfiEndOfDxeEventGroupGuid = { 0x108U, 0U, 0U, { 0U } };
EFI_GUID gEfiEventExitBootServicesGuid = { 0x107U, 0U, 0U, { 0U } };
EFI_GUID gEfiEventReadyToBootGuid = { 0x7ce88fb3U, 0x4bd7U, 0x4679U, { 0x87U, 0xa8U, 0xa8U, 0xd8U, 0xdeU, 0xe5U, 0x0dU, 0x2bU } };
EFI_GUID gEfiDevicePathProtocolGuid = { 0x101U, 0U, 0U, { 0U } };
EFI_GUID gEfiDriverBindingProtocolGuid = { 0x102U, 0U, 0U, { 0U } };
EFI_GUID gEdkiiUfsHostControllerProtocolGuid = { 0x103U, 0U, 0U, { 0U } };
EFI_GUID gEdkiiUfsHcPlatformProtocolGuid = { 0x104U, 0U, 0U, { 0U } };
EFI_GUID gEfiUfsDeviceConfigProtocolGuid = EFI_UFS_DEVICE_CONFIG_GUID

static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS = &DxeServices;

static UINT64 HandoffMagic;
static EFI_STATUS HandoffStatus;
static EFI_EVENT_NOTIFY FdtNotify;
static EFI_EVENT_NOTIFY EndOfDxeNotify;
static EFI_EVENT_NOTIFY ReadyToBootNotify;
static EFI_STATUS CreateEndStatus, CloseEndStatus;
static UINTN CreateEndCalls, CloseEndCalls;
static BOOLEAN EndEventOpen;
static EFI_EVENT_NOTIFY ExitNotify;
static BOOLEAN ExitEventOpen, HceStuck, StopStuck, BaseStuck;
static UINT32 OsHandoff[4];
static UINTN OsPublications, OsCacheCleans;
static EFI_STATUS CreateExitStatus, CloseExitStatus;
static UINTN CreateExitCalls, CloseExitCalls;
static EFI_HANDLE SeedHandle;
static CONST VOID *RuntimeTree;
static VOID *TableOverride;
static EFI_STATUS TableStatus;
static EFI_STATUS GcdStatus;
static EFI_GCD_MEMORY_TYPE FdtMemoryType;
static EFI_GCD_MEMORY_TYPE UfsMemoryType;
static UINT64 UfsMemoryAttributes;
static EFI_TPL CurrentTpl;
typedef VOID (EFIAPI *RESTORE_TPL_HOOK)(VOID);
static RESTORE_TPL_HOOK RestoreTplHook;
static UINTN RestoreTplHookCalls;
static BOOLEAN WaitForEventStartStopHook;
static EFI_STATUS RestoreTplStopStatus;
static UINTN RestoreTplStopCalls;
static EFI_STATUS CreateFdtEventStatus;
static EFI_STATUS CreateReadyEventStatus;
static EFI_STATUS InstallBindingStatus;
static EFI_STATUS InstallSeedStatus;
static EFI_STATUS UninstallBindingStatus;
static EFI_STATUS UninstallSeedStatus;
static EFI_STATUS CloseFdtEventStatus;
static EFI_STATUS CloseReadyEventStatus;
static EFI_STATUS InstallUfsProtocolsStatus;
static EFI_STATUS UninstallUfsProtocolsStatus;
static EFI_STATUS InstallUfsHostProtocolStatus;
static EFI_STATUS InstallUfsPlatformProtocolStatus;
static EFI_STATUS UninstallUfsHostProtocolStatus;
static EFI_STATUS UninstallUfsPlatformProtocolStatus;
static EFI_STATUS UfsProtocolProbeStatus;
static BOOLEAN PartialBindingInstall;
static BOOLEAN PartialSeedInstall;
static BOOLEAN FdtEventOpen;
static BOOLEAN ReadyEventOpen;
static BOOLEAN BindingInstalled;
static BOOLEAN SeedInstalled;
static BOOLEAN UfsProtocolsInstalled;
static BOOLEAN UfsHostProtocolInstalled;
static BOOLEAN UfsPlatformProtocolInstalled;
static UINT32 PartialUfsInstallMask;
static UINT32 PartialUfsUninstallMask;
static BOOLEAN RunInstallCallbackExercise;
static EFI_STATUS PublishCallbackStatus;
static UINTN PublishCallbackCount;
static UINTN UfsHostProtocolProbeCalls;
static UINTN UfsPlatformProtocolProbeCalls;
static UINTN UfsSingleProtocolInstallCalls;
static UINTN UfsSingleProtocolUninstallCalls;
static EFI_STATUS ExerciseUfsCallbacksDuringInstall (VOID);
static VOID CaptureEbsHookSnapshot (VOID);
static UINTN CreateEventCalls;
static UINTN CreateReadyEventCalls;
static UINTN CloseFdtEventCalls;
static UINTN CloseReadyEventCalls;
static UINTN BindingInstallCalls;
static UINTN SeedInstallCalls;
static UINTN BindingUninstallCalls;
static UINTN SeedUninstallCalls;
static UINTN ConnectCalls;
static UINTN MmioReadCalls;
static UINTN MmioWriteCalls;
static BOOLEAN GuardUfsMmioResourcesReady;
static UINTN UfsMmioReadsWithResourcesOff;
static UINTN FirstUfsMmioResourceTraceCount;
static UINTN UfsProtocolInstallCalls;
static UINTN UfsProtocolUninstallCalls;
static UINTN RaiseTplCalls;
static UINTN RestoreTplCalls;
static UINT32 HceValue;
static UINT32 HcsValue;
static UINT32 TransferDoorbellValue;
static UINT32 TaskDoorbellValue;
static UINT32 CapabilitiesValue;
typedef struct {
  UINTN MmioRead;
  UINTN MmioWrite;
  UINTN AllocatePages;
  UINTN FreePages;
  UINTN SetMemoryAttributes;
  UINTN CacheWriteBack;
  UINTN CacheWriteBackInvalidate;
  UINTN CacheInvalidate;
  UINTN PoolAllocations;
  UINTN PoolFrees;
  UINTN ResourceEvents;
  UINTN RaiseTpl;
  UINTN RestoreTpl;
  UINT32 ActiveCallbacks;
  UINT32 ActiveDmaOperations;
} TEST_EFFECT_SNAPSHOT;
static TEST_EFFECT_SNAPSHOT EbsHookSnapshot;
static BOOLEAN EbsHookSnapshotValid;
static UINTN EbsHookEntryMmioRead;
static UINTN EbsHookEntryMmioWrite;
static EFI_STATUS ConnectSupportedStatus;
static EFI_STATUS ConnectStartStatus;
EFI_GUID gMediaTekClockProtocolGuid = { 0x105U, 0U, 0U, { 0U } };
EFI_GUID gMediaTekPmicProtocolGuid = { 0x106U, 0U, 0U, { 0U } };
static UINT32 ColdRegs[UFS_LENGTH / 4];
static EFI_PHYSICAL_ADDRESS NextFakeDmaAddress;
static UINTN AllocatePagesCalls, FreePagesCalls, SetMemoryAttributesCalls;
static EFI_STATUS AllocatePagesStatus, FreePagesStatus, SetMemoryAttributesStatus;
static EFI_PHYSICAL_ADDRESS LastAllocatedPageAddress;
static UINTN LastAllocatedPageCount;
static UINT64 FakeDmaAttributes;
static UINTN CacheWriteBackCalls, CacheWriteBackInvalidateCalls, CacheInvalidateCalls;
typedef struct {
  EFI_PHYSICAL_ADDRESS  Address;
  UINTN                 Pages;
  BOOLEAN               Live;
} TEST_PAGE_ALLOCATION;
static TEST_PAGE_ALLOCATION FakePageAllocations[TEST_MAX_PAGE_ALLOCATIONS];
static UINT32 Attributes[0x10000];
static UINTN SmcCalls, Smc32OffCalls, Smc32OnCalls, ResetWrites, UicCalls;
static BOOLEAN ColdResources, ClockState[6], VemcEnabled;
static UINT32 VemcVoltage;
static UINT64 ColdFrequency;
static EFI_STATUS ColdClockStatus, ColdPmicStatus, UicStatus;
static INT64 SmcError;
static UINT32 UicResult, ClockSetFailId, ClockDisableFailId, ClockDisableNoWriteId, ClockEnableNoWriteId;
static UINT32 ClockEnableNoEffectId, ClockEnableReadFailId;
static BOOLEAN PmicSetFailAfterWrite, PmicDisableNoWrite;
static UINTN IoCloseAtUicCall;
static UINTN IoClosedMmioReadCalls, IoClosedMmioWriteCalls, IoClosedSmcCalls;
static UINTN IoClosedUicCalls, IoClosedResetWrites, IoClosedResourceEvents;
static BOOLEAN IoClosedSnapshotValid;
typedef struct { CHAR8 Kind; UINT32 Id; BOOLEAN Enable; } RESOURCE_EVENT;
static RESOURCE_EVENT ResourceTrace[64];
static UINTN ResourceTraceCount, HibernateCalls;
static EFI_HANDLE DeviceConfigHandle;
static EFI_STATUS DeviceConfigStatus, DescriptorReadStatus;
static UINT8 DeviceDescriptor[255];
static UINT32 DescriptorReturnedSize;
static UINTN DeviceConfigLookupCalls, DescriptorReadCalls;
static UINT8 DescriptorLastId, DescriptorLastIndex, DescriptorLastSelector;
static BOOLEAN DescriptorLastRead;
static EFI_UFS_DEVICE_CONFIG_PROTOCOL DeviceConfigProtocol;
static UINTN ColdProviderCalls, ClockGetIdCalls, LocateProtocolCalls;
static UINTN GcdDescriptorCalls, SystemConfigTableCalls;
static UINTN HandoffMmioReadCalls, HandoffMmioWriteCalls;
static UINTN NestedEbsClockGetIdTarget, NestedEbsClockGetIdCalls;
static BOOLEAN NestedEbsClockGetIdSnapshotValid;
typedef struct {
  UINTN ProviderCalls, ClockGetIdCalls, LocateProtocolCalls;
  UINTN GcdDescriptorCalls, SystemConfigTableCalls;
  UINTN RaiseTplCalls, RestoreTplCalls;
  UINTN MmioReadCalls, MmioWriteCalls, HandoffMmioReadCalls, HandoffMmioWriteCalls;
  UINTN CacheWriteBackCalls, CacheWriteBackInvalidateCalls, CacheInvalidateCalls;
  UINTN ResourceTraceCount, ResetWrites, UicCalls, SmcCalls, HibernateCalls;
  UINTN DeviceConfigLookupCalls, DescriptorReadCalls;
} TEST_EBS_ACTIVITY_SNAPSHOT;
static TEST_EBS_ACTIVITY_SNAPSHOT NestedEbsClockGetIdSnapshot;

static EFI_STATUS EFIAPI MockRwUfsDescriptor(EFI_UFS_DEVICE_CONFIG_PROTOCOL *This, BOOLEAN Read,
                                               UINT8 DescId, UINT8 Index, UINT8 Selector,
                                               UINT8 *Descriptor, UINT32 *DescSize) {
  UINT32 CopySize;
  CHECK(This == &DeviceConfigProtocol && Descriptor != NULL && DescSize != NULL);
  DescriptorReadCalls++;
  DescriptorLastRead = Read; DescriptorLastId = DescId;
  DescriptorLastIndex = Index; DescriptorLastSelector = Selector;
  if (EFI_ERROR(DescriptorReadStatus)) return DescriptorReadStatus;
  CHECK(*DescSize == sizeof(DeviceDescriptor));
  CopySize = *DescSize < DescriptorReturnedSize ? *DescSize : DescriptorReturnedSize;
  if (CopySize != 0) memcpy(Descriptor, DeviceDescriptor, CopySize);
  *DescSize = DescriptorReturnedSize;
  return EFI_SUCCESS;
}

static VOID CaptureEbsActivitySnapshot(TEST_EBS_ACTIVITY_SNAPSHOT *Snapshot) {
  ZeroMem(Snapshot, sizeof(*Snapshot));
  Snapshot->ProviderCalls = ColdProviderCalls;
  Snapshot->ClockGetIdCalls = ClockGetIdCalls;
  Snapshot->LocateProtocolCalls = LocateProtocolCalls;
  Snapshot->GcdDescriptorCalls = GcdDescriptorCalls;
  Snapshot->SystemConfigTableCalls = SystemConfigTableCalls;
  Snapshot->RaiseTplCalls = RaiseTplCalls;
  Snapshot->RestoreTplCalls = RestoreTplCalls;
  Snapshot->MmioReadCalls = MmioReadCalls;
  Snapshot->MmioWriteCalls = MmioWriteCalls;
  Snapshot->HandoffMmioReadCalls = HandoffMmioReadCalls;
  Snapshot->HandoffMmioWriteCalls = HandoffMmioWriteCalls;
  Snapshot->CacheWriteBackCalls = CacheWriteBackCalls;
  Snapshot->CacheWriteBackInvalidateCalls = CacheWriteBackInvalidateCalls;
  Snapshot->CacheInvalidateCalls = CacheInvalidateCalls;
  Snapshot->ResourceTraceCount = ResourceTraceCount;
  Snapshot->ResetWrites = ResetWrites;
  Snapshot->UicCalls = UicCalls;
  Snapshot->SmcCalls = SmcCalls;
  Snapshot->HibernateCalls = HibernateCalls;
  Snapshot->DeviceConfigLookupCalls = DeviceConfigLookupCalls;
  Snapshot->DescriptorReadCalls = DescriptorReadCalls;
}

static EFI_STATUS EFIAPI ClockId(CONST CHAR8 *Name, UINT32 *Id) {
  static CONST CHAR8 *Names[] = { "IFRAO_UFS", "IFRAO_UFS_SAP_BCLK", "IFRAO_UNIPRO_TICK", "IFRAO_UNIPRO_SYSCLK", "IFRAO_UFS_AES", "TOP_UFS" };
  UINTN I;
  ColdProviderCalls++;
  ClockGetIdCalls++;
  for (I = 0; I < ARRAY_SIZE(Names); I++) if (strcmp(Name, Names[I]) == 0) {
    *Id = I;
    if (ClockGetIdCalls == NestedEbsClockGetIdTarget) {
      CHECK(!EFI_ERROR(ColdClockStatus));
      CHECK(CurrentTpl == TPL_CALLBACK && ExitEventOpen && ExitNotify != NULL);
      NestedEbsClockGetIdTarget = 0U;
      NestedEbsClockGetIdCalls++;
      /* Defensive return-boundary fault injection, not normal TPL event scheduling. */
      ExitNotify(EXIT_EVENT_HANDLE, NULL);
      CHECK(mContext.ExitedBootServices);
      CaptureEbsActivitySnapshot(&NestedEbsClockGetIdSnapshot);
      NestedEbsClockGetIdSnapshotValid = TRUE;
    }
    return ColdClockStatus;
  }
  CHECK(FALSE); return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI ClockSetEnable(UINT32 Id, BOOLEAN Enable) {
  ColdProviderCalls++;
  CHECK(Id < 6 && ResourceTraceCount < ARRAY_SIZE(ResourceTrace));
  ResourceTrace[ResourceTraceCount++] = (RESOURCE_EVENT){ 'C', Id, Enable };
  if ((Id == ClockDisableNoWriteId && !Enable) || (Id == ClockEnableNoWriteId && Enable)) return EFI_DEVICE_ERROR;
  if (Id == ClockEnableNoEffectId && Enable) return EFI_SUCCESS;
  if (Id == 5 && !Enable) return EFI_UNSUPPORTED; /* Its selected parent can be off. */
  ClockState[Id] = Enable;
  return ((Id == ClockSetFailId && Enable) || (Id == ClockDisableFailId && !Enable)) ? EFI_DEVICE_ERROR : ColdClockStatus;
}
static EFI_STATUS EFIAPI ClockEnabled(UINT32 Id, BOOLEAN *Enabled) {
  ColdProviderCalls++;
  CHECK(Id < 6); *Enabled = ClockState[Id];
  return (Id == ClockEnableReadFailId && ClockState[Id]) ? EFI_DEVICE_ERROR : ColdClockStatus;
}
static EFI_STATUS EFIAPI ClockFrequency(UINT32 Id, UINT64 *Frequency) { ColdProviderCalls++; CHECK(Id < 6); *Frequency = ColdFrequency; return ColdClockStatus; }
static EFI_STATUS EFIAPI RegSetEnable(CONST CHAR8 *Name, BOOLEAN Enable) {
  ColdProviderCalls++;
  CHECK(strcmp(Name, "ldo_vemc") == 0 && ResourceTraceCount < ARRAY_SIZE(ResourceTrace));
  ResourceTrace[ResourceTraceCount++] = (RESOURCE_EVENT){ 'V', 0, Enable };
  if (!Enable && PmicDisableNoWrite) return EFI_DEVICE_ERROR;
  VemcEnabled = Enable;
  return PmicSetFailAfterWrite && Enable ? EFI_DEVICE_ERROR : ColdPmicStatus;
}
static EFI_STATUS EFIAPI RegEnabled(CONST CHAR8 *Name, BOOLEAN *Enabled) { ColdProviderCalls++; CHECK(strcmp(Name, "ldo_vemc") == 0); *Enabled = VemcEnabled; return ColdPmicStatus; }
static EFI_STATUS EFIAPI RegVoltage(CONST CHAR8 *Name, UINT32 *Voltage) { ColdProviderCalls++; CHECK(strcmp(Name, "ldo_vemc") == 0); *Voltage = VemcVoltage; return ColdPmicStatus; }
static VOID CheckVemcRetained(VOID) {
  UINTN I;
  CHECK(VemcEnabled);
  for (I = 0; I < ResourceTraceCount; I++)
    if (ResourceTrace[I].Kind == 'V') CHECK(ResourceTrace[I].Enable);
}
static MTK_CLOCK_PROTOCOL ColdClock = { .GetId = ClockId, .SetEnable = ClockSetEnable, .IsEnabled = ClockEnabled, .GetFrequency = ClockFrequency };
static MTK_PMIC_PROTOCOL ColdPmic = { .RegulatorSetEnable = RegSetEnable, .RegulatorIsEnabled = RegEnabled, .RegulatorGetVoltage = RegVoltage };
static EFI_STATUS EFIAPI LocateCold(EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  LocateProtocolCalls++;
  (VOID)Registration;
  if (CompareGuid(Guid, &gMediaTekClockProtocolGuid)) { *Interface = &ColdClock; return ColdResources ? EFI_SUCCESS : EFI_NOT_FOUND; }
  if (CompareGuid(Guid, &gMediaTekPmicProtocolGuid)) { *Interface = &ColdPmic; return ColdResources ? EFI_SUCCESS : EFI_NOT_FOUND; }
  CHECK(FALSE); return EFI_NOT_FOUND;
}
VOID ArmCallSmc(IN OUT ARM_SMC_ARGS *Args) {
  CHECK(ColdResources && Args->Arg0 == MTK_UFS_SMC_FUNCTION && (Args->Arg1 == 2 || Args->Arg1 == 8 || Args->Arg1 == 32));
  CHECK(Args->Arg4 == 0 && Args->Arg5 == 0 && Args->Arg6 == 0 && Args->Arg7 == 0 && Args->Arg17 == 0);
  if (Args->Arg1 == 32 && Args->Arg2 == 0) Smc32OffCalls++;
  if (Args->Arg1 == 32 && Args->Arg2 == 1) Smc32OnCalls++;
  SmcCalls++; Args->Arg0 = (UINTN)SmcError;
}
static VOID CloseIoAtSuccessfulUicReturn(VOID) {
  if ((IoCloseAtUicCall == 0U) || (UicCalls != IoCloseAtUicCall)) return;
  CHECK(!EFI_ERROR(UicStatus) && (UicResult == 0U));
  IoCloseAtUicCall = 0U;
  mContext.ColdHost.IoClosed = TRUE;
  mContext.ExitedBootServices = TRUE;
  IoClosedMmioReadCalls = MmioReadCalls;
  IoClosedMmioWriteCalls = MmioWriteCalls;
  IoClosedSmcCalls = SmcCalls;
  IoClosedUicCalls = UicCalls;
  IoClosedResetWrites = ResetWrites;
  IoClosedResourceEvents = ResourceTraceCount;
  IoClosedSnapshotValid = TRUE;
}

static EFI_STATUS EFIAPI ColdUic(EDKII_UFS_HC_DRIVER_INTERFACE *This, EDKII_UIC_COMMAND *Command) {
  UINT32 Attr = Command->Arg1 >> 16;
  CHECK(This->UfsHcProtocol == &mContext.HostController);
  CHECK(CurrentTpl <= TPL_CALLBACK);
  UicCalls++;
  if (EFI_ERROR(UicStatus)) return UicStatus;
  if (Command->Opcode == UfsUicDmeGet || Command->Opcode == UfsUicDmeSet) {
    CHECK((Command->Arg1 & 0xFFFF) == 0 && Command->Arg2 == 0);
    if (Command->Opcode == UfsUicDmeGet) Command->Arg3 = Attributes[Attr];
    else Attributes[Attr] = Command->Arg3;
    Command->Arg2 = UicResult;
    if (UicResult == 0U) CloseIoAtSuccessfulUicReturn();
    return EFI_SUCCESS;
  }
  CHECK(Command->Opcode == UfsUicDmeHibernateEnter || Command->Opcode == UfsUicDmeHibernateExit);
  CHECK(Command->Arg1 == 0 && Command->Arg2 == 0 && Command->Arg3 == 0);
  HibernateCalls++;
  ColdRegs[0x22C8 / 4] = (Command->Opcode == UfsUicDmeHibernateEnter ? (3U << 28) : (2U << 28)) | 1U;
  Command->Arg2 = UicResult;
  if (UicResult == 0U) CloseIoAtSuccessfulUicReturn();
  return EFI_SUCCESS;
}


static BOOLEAN
GuidEqual (
  IN CONST EFI_GUID  *Left,
  IN CONST EFI_GUID  *Right
  )
{
  return (BOOLEAN)(memcmp (Left, Right, sizeof (*Left)) == 0);
}

BOOLEAN EFIAPI CompareGuid (IN CONST EFI_GUID *Left, IN CONST EFI_GUID *Right) {
  return GuidEqual (Left, Right);
}
EFI_GUID *EFIAPI CopyGuid (OUT EFI_GUID *Destination, IN CONST EFI_GUID *Source) {
  memcpy (Destination, Source, sizeof (*Destination)); return Destination;
}
UINTN EFIAPI DevicePathNodeLength (IN CONST VOID *Node) {
  CONST EFI_DEVICE_PATH_PROTOCOL *Path = Node;
  return (UINT16)(Path->Length[0] | ((UINT16)Path->Length[1] << 8));
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode (IN CONST VOID *Node) {
  return (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Node + DevicePathNodeLength (Node));
}
BOOLEAN EFIAPI IsDevicePathEnd (IN CONST VOID *Node) {
  return (BOOLEAN)(((CONST EFI_DEVICE_PATH_PROTOCOL *)Node)->Type == END_DEVICE_PATH_TYPE);
}
UINT16 EFIAPI SetDevicePathNodeLength (IN OUT VOID *Node, IN UINTN Length) {
  EFI_DEVICE_PATH_PROTOCOL *Path = Node;
  Path->Length[0] = (UINT8)Length; Path->Length[1] = (UINT8)(Length >> 8);
  return (UINT16)Length;
}
VOID EFIAPI SetDevicePathEndNode (OUT VOID *NodeBuffer) {
  EFI_DEVICE_PATH_PROTOCOL *Node = NodeBuffer;
  Node->Type = END_DEVICE_PATH_TYPE; Node->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength (Node, sizeof (*Node));
}
VOID *EFIAPI AllocateZeroPool (IN UINTN Size) { return calloc (1, Size); }
VOID *EFIAPI AllocatePool (IN UINTN Size) { return malloc (Size); }
VOID EFIAPI MemoryFence (VOID) { __sync_synchronize (); }
UINTN EFIAPI ArmDataCacheLineLength(VOID) { return 64; }
VOID *EFIAPI WriteBackDataCacheRange (IN VOID *Address, IN UINTN Length) {
  CacheWriteBackCalls++;
  if ((UINTN)Address == HANDOFF_BASE+0xb0) { CHECK(Length == 16); OsCacheCleans++; }
  return Address;
}
VOID *EFIAPI WriteBackInvalidateDataCacheRange (IN VOID *Address, IN UINTN Length) {
  (VOID)Length; CacheWriteBackInvalidateCalls++; return Address;
}
VOID *EFIAPI InvalidateDataCacheRange (IN VOID *Address, IN UINTN Length) {
  (VOID)Length; CacheInvalidateCalls++; return Address;
}

UINT64 EFIAPI MmioRead64 (IN UINTN Address) {
  UINTN Offset;
  HandoffMmioReadCalls++;
  CHECK ((UINT64)Address >= HANDOFF_BASE && (UINT64)Address < HANDOFF_BASE + 0x1000U);
  Offset = (Address - (UINTN)HANDOFF_BASE);
  if (Offset == 0U) return HandoffMagic;
  if (Offset == 0x48U) return (UINT64)(UINTN)(TableOverride != NULL ? TableOverride : (VOID *)RuntimeTree);
  if (Offset == 0x50U) return (RuntimeTree != NULL) ? FdtTotalSize (RuntimeTree) : 0U;
  if (Offset == 0x58U) return (UINT64)HandoffStatus;
  return 0U;
}

UINT32 EFIAPI MmioRead32 (IN UINTN Address) {
  UINT64 Offset;
  if (Address >= HANDOFF_BASE+0xb0 && Address < HANDOFF_BASE+0xc0) { HandoffMmioReadCalls++; return OsHandoff[(Address-HANDOFF_BASE-0xb0)/4]; }
  CHECK ((UINT64)Address >= UFS_BASE && (UINT64)Address < UFS_BASE + UFS_LENGTH);
  if (GuardUfsMmioResourcesReady) {
    BOOLEAN ResourcesReady = (BOOLEAN)(ColdResources && VemcEnabled);
    UINTN I;
    if (MmioReadCalls == 0) FirstUfsMmioResourceTraceCount = ResourceTraceCount;
    for (I = 0; I < ARRAY_SIZE(ClockState); I++) {
      if (!ClockState[I]) ResourcesReady = FALSE;
    }
    if (!ResourcesReady) UfsMmioReadsWithResourcesOff++;
  }
  MmioReadCalls++;
  Offset = (UINT64)Address - UFS_BASE;
  switch (Offset) {
    case UFS_HC_ENABLE_OFFSET: return HceValue;
    case UFS_HC_STATUS_OFFSET: return HcsValue;
    case UFS_HC_UTRLDBR_OFFSET: return TransferDoorbellValue;
    case UFS_HC_UTMRLDBR_OFFSET: return TaskDoorbellValue;
    case UFS_HC_CAP_OFFSET: return CapabilitiesValue;
    default: return ColdRegs[Offset / 4];
  }
}
UINT32 EFIAPI MmioWrite32 (IN UINTN Address, IN UINT32 Value) {
  UINTN Offset;
  if (Address >= HANDOFF_BASE+0xb0 && Address < HANDOFF_BASE+0xc0) {
    HandoffMmioWriteCalls++;
    if (Address == HANDOFF_BASE+0xb0 && Value == 1) {
      CHECK(HceValue == 1 && !ColdRegs[0x50/4] && !ColdRegs[0x54/4] && !ColdRegs[0x70/4] && !ColdRegs[0x74/4]);
      CHECK(!ColdRegs[0x60/4] && !ColdRegs[0x80/4] && !ColdRegs[0x24/4] && !TransferDoorbellValue && !TaskDoorbellValue);
      OsPublications++;
    }
    OsHandoff[(Address-HANDOFF_BASE-0xb0)/4] = Value; return Value;
  }
  MmioWriteCalls++;
  if (Address >= 0x10001000U && Address < 0x10002000U) {
    CHECK(ColdResources && mContext.ColdStart);
    CHECK(Address == 0x10001130U || Address == 0x10001134U || Address == 0x10001140U ||
          Address == 0x10001144U || Address == 0x10001150U || Address == 0x10001154U);
    ResetWrites++; return Value;
  }
  CHECK ((UINT64)Address >= UFS_BASE && (UINT64)Address < UFS_BASE + UFS_LENGTH);
  Offset = Address - UFS_BASE;
  if (Offset == UFS_HC_ENABLE_OFFSET) { if (!HceStuck) HceValue = Value; }
  else if (Offset == UFS_HC_IS_OFFSET) ColdRegs[Offset/4] &= ~Value;
  else if (StopStuck && (Offset == 0x60 || Offset == 0x80)) { }
  else if (BaseStuck && Offset == 0x50 && Value == 0) { }
  else if (Offset == 0x144U) ColdRegs[Offset / 4] = Value ? 3 : 0;
  else ColdRegs[Offset / 4] = Value;
  return Value;
}
UINT16 EFIAPI MmioRead16 (IN UINTN Address) { (VOID)Address; CHECK (FALSE); return 0; }
UINT8 EFIAPI MmioRead8 (IN UINTN Address) { (VOID)Address; CHECK (FALSE); return 0; }
UINT16 EFIAPI MmioWrite16 (IN UINTN Address, IN UINT16 Value) { (VOID)Address; CHECK (FALSE); return Value; }
UINT8 EFIAPI MmioWrite8 (IN UINTN Address, IN UINT8 Value) { (VOID)Address; CHECK (FALSE); return Value; }

EFI_STATUS EFIAPI EfiGetSystemConfigurationTable (IN EFI_GUID *Guid, OUT VOID **Table) {
  SystemConfigTableCalls++;
  CHECK (GuidEqual (Guid, &gFdtTableGuid));
  if (EFI_ERROR (TableStatus)) return TableStatus;
  *Table = TableOverride != NULL ? TableOverride : (VOID *)RuntimeTree;
  return (*Table == NULL) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI GetGcdDescriptor (
  IN EFI_PHYSICAL_ADDRESS Address,
  OUT EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor
  ) {
  CHECK (Descriptor != NULL);
  GcdDescriptorCalls++;
  CHECK (CurrentTpl <= TPL_CALLBACK);
  if (EFI_ERROR (GcdStatus)) return GcdStatus;
  memset (Descriptor, 0, sizeof (*Descriptor));
  if ((Address >= TEST_DMA_BASE) && (Address < TEST_DMA_LIMIT)) {
    Descriptor->BaseAddress = TEST_DMA_BASE;
    Descriptor->Length = TEST_DMA_LIMIT - TEST_DMA_BASE;
    Descriptor->GcdMemoryType = EfiGcdMemoryTypeSystemMemory;
    Descriptor->Capabilities = EFI_MEMORY_UC;
    Descriptor->Attributes = FakeDmaAttributes;
    return EFI_SUCCESS;
  }
  if ((RuntimeTree != NULL) && ((UINTN)RuntimeTree <= Address) &&
      (Address < (UINTN)RuntimeTree + FdtTotalSize (RuntimeTree))) {
    Descriptor->BaseAddress = (UINTN)RuntimeTree;
    Descriptor->Length = FdtTotalSize (RuntimeTree);
    Descriptor->GcdMemoryType = FdtMemoryType;
    return EFI_SUCCESS;
  }
  if ((Address >= UFS_BASE) && (Address < UFS_BASE + UFS_LENGTH)) {
    Descriptor->BaseAddress = UFS_BASE;
    Descriptor->Length = UFS_LENGTH;
    Descriptor->GcdMemoryType = UfsMemoryType;
    Descriptor->Attributes = UfsMemoryAttributes;
    return EFI_SUCCESS;
  }
  if (ColdResources && ((Address >= 0x10000000U) && (Address < 0x10002000U))) {
    Descriptor->BaseAddress = Address & ~0xFFFULL; Descriptor->Length = 0x1000;
    Descriptor->GcdMemoryType = EfiGcdMemoryTypeMemoryMappedIo; Descriptor->Attributes = EFI_MEMORY_UC;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockAllocatePages (
  IN EFI_ALLOCATE_TYPE Type,
  IN EFI_MEMORY_TYPE MemoryType,
  IN UINTN Pages,
  IN OUT EFI_PHYSICAL_ADDRESS *Memory
  ) {
  UINTN Index;

  CHECK (CurrentTpl <= TPL_CALLBACK && Memory != NULL && Pages != 0U);
  CHECK (Type == AllocateMaxAddress);
  CHECK ((MemoryType == EfiBootServicesData) || (MemoryType == EfiRuntimeServicesData));
  AllocatePagesCalls++;
  if (EFI_ERROR (AllocatePagesStatus)) return AllocatePagesStatus;
  for (Index = 0U; Index < ARRAY_SIZE (FakePageAllocations); Index++) {
    if (!FakePageAllocations[Index].Live) break;
  }

  if (Index == ARRAY_SIZE (FakePageAllocations)) return EFI_OUT_OF_RESOURCES;

  *Memory = NextFakeDmaAddress;
  LastAllocatedPageAddress = NextFakeDmaAddress;
  LastAllocatedPageCount = Pages;
  FakePageAllocations[Index].Address = NextFakeDmaAddress;
  FakePageAllocations[Index].Pages   = Pages;
  FakePageAllocations[Index].Live    = TRUE;
  NextFakeDmaAddress += EFI_PAGES_TO_SIZE (Pages);
  CHECK (NextFakeDmaAddress <= TEST_DMA_LIMIT);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockFreePages (
  IN EFI_PHYSICAL_ADDRESS Memory,
  IN UINTN Pages
  ) {
  UINTN Index;

  CHECK (CurrentTpl <= TPL_CALLBACK && Pages != 0U);
  for (Index = 0U; Index < ARRAY_SIZE (FakePageAllocations); Index++) {
    if (FakePageAllocations[Index].Live &&
        (FakePageAllocations[Index].Address == Memory) &&
        (FakePageAllocations[Index].Pages == Pages))
    {
      break;
    }
  }

  CHECK (Index < ARRAY_SIZE (FakePageAllocations));
  if (Index == ARRAY_SIZE (FakePageAllocations)) return EFI_INVALID_PARAMETER;

  FreePagesCalls++;
  if (EFI_ERROR (FreePagesStatus)) return FreePagesStatus;

  FakePageAllocations[Index].Live = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockSetMemorySpaceAttributes (
  IN EFI_PHYSICAL_ADDRESS BaseAddress,
  IN UINT64 Length,
  IN UINT64 Attributes
  ) {
  CHECK (CurrentTpl <= TPL_CALLBACK);
  CHECK ((BaseAddress >= TEST_DMA_BASE) && (BaseAddress < TEST_DMA_LIMIT) &&
         (Length != 0U) && (Length <= TEST_DMA_LIMIT - BaseAddress));
  SetMemoryAttributesCalls++;
  if (EFI_ERROR (SetMemoryAttributesStatus)) return SetMemoryAttributesStatus;
  FakeDmaAttributes = Attributes;
  return EFI_SUCCESS;
}

static EFI_TPL EFIAPI MockRaiseTpl (IN EFI_TPL NewTpl) {
  EFI_TPL OldTpl = CurrentTpl;
  CHECK (NewTpl >= CurrentTpl && NewTpl < TPL_HIGH_LEVEL);
  RaiseTplCalls++;
  CurrentTpl = NewTpl;
  return OldTpl;
}

static VOID EFIAPI MockRestoreTpl (IN EFI_TPL OldTpl) {
  RESTORE_TPL_HOOK Hook;
  BOOLEAN Ready;

  CHECK (OldTpl <= CurrentTpl);
  RestoreTplCalls++;
  CurrentTpl = OldTpl;
  Hook = RestoreTplHook;
  Ready = (BOOLEAN)(!WaitForEventStartStopHook ||
                    ((mContext.EventStartCallbacks != 0U) &&
                     !mContext.StartBusy && mContext.Started));
  if ((Hook != NULL) && Ready) {
    RestoreTplHook = NULL;
    WaitForEventStartStopHook = FALSE;
    RestoreTplHookCalls++;
    Hook ();
  }
}

static VOID EFIAPI TryStopAtRestoreTpl (VOID) {
  RestoreTplStopCalls++;
  RestoreTplStopStatus = MtkUfsDriverStop (&mDriverBinding, mContext.ControllerHandle, 0U, NULL);
}

static VOID EFIAPI ExitAtRestoreTpl (VOID) {
  CHECK (ExitEventOpen && ExitNotify != NULL && CurrentTpl == TPL_APPLICATION);
  EbsHookEntryMmioRead = MmioReadCalls;
  EbsHookEntryMmioWrite = MmioWriteCalls;
  CurrentTpl = TPL_CALLBACK;
  ExitNotify (EXIT_EVENT_HANDLE, NULL);
  CurrentTpl = TPL_APPLICATION;
  CaptureEbsHookSnapshot ();
}

static VOID
CaptureEbsHookSnapshot (VOID)
{
  EbsHookSnapshot.MmioRead = MmioReadCalls;
  EbsHookSnapshot.MmioWrite = MmioWriteCalls;
  EbsHookSnapshot.AllocatePages = AllocatePagesCalls;
  EbsHookSnapshot.FreePages = FreePagesCalls;
  EbsHookSnapshot.SetMemoryAttributes = SetMemoryAttributesCalls;
  EbsHookSnapshot.CacheWriteBack = CacheWriteBackCalls;
  EbsHookSnapshot.CacheWriteBackInvalidate = CacheWriteBackInvalidateCalls;
  EbsHookSnapshot.CacheInvalidate = CacheInvalidateCalls;
  EbsHookSnapshot.PoolAllocations = HandoffZeroPoolAllocations;
  EbsHookSnapshot.PoolFrees = HandoffPoolFrees;
  EbsHookSnapshot.ResourceEvents = ResourceTraceCount;
  EbsHookSnapshot.RaiseTpl = RaiseTplCalls;
  EbsHookSnapshot.RestoreTpl = RestoreTplCalls;
  EbsHookSnapshot.ActiveCallbacks = mContext.ActiveCallbacks;
  EbsHookSnapshot.ActiveDmaOperations = mContext.ActiveDmaOperations;
  EbsHookSnapshotValid = TRUE;
}

static VOID
CheckNoEffectsAfterEbsHook (VOID)
{
  CHECK (EbsHookSnapshotValid);
  CHECK (MmioReadCalls == EbsHookSnapshot.MmioRead &&
         MmioWriteCalls == EbsHookSnapshot.MmioWrite &&
         AllocatePagesCalls == EbsHookSnapshot.AllocatePages &&
         FreePagesCalls == EbsHookSnapshot.FreePages &&
         SetMemoryAttributesCalls == EbsHookSnapshot.SetMemoryAttributes &&
         CacheWriteBackCalls == EbsHookSnapshot.CacheWriteBack &&
         CacheWriteBackInvalidateCalls == EbsHookSnapshot.CacheWriteBackInvalidate &&
         CacheInvalidateCalls == EbsHookSnapshot.CacheInvalidate &&
         HandoffZeroPoolAllocations == EbsHookSnapshot.PoolAllocations &&
         HandoffPoolFrees == EbsHookSnapshot.PoolFrees &&
         ResourceTraceCount == EbsHookSnapshot.ResourceEvents &&
         RaiseTplCalls == EbsHookSnapshot.RaiseTpl &&
         RestoreTplCalls == EbsHookSnapshot.RestoreTpl);
}

static EFI_STATUS EFIAPI MockCreateEventEx (
  IN UINT32 Type, IN EFI_TPL NotifyTpl, IN EFI_EVENT_NOTIFY NotifyFunction,
  IN CONST VOID *NotifyContext, IN CONST EFI_GUID *EventGroup, OUT EFI_EVENT *Event
  ) {
  // Failure outputs deliberately poisoned: only successful calls own handles.
  CHECK (Type == EVT_NOTIFY_SIGNAL);
  CHECK (NotifyFunction == (GuidEqual(EventGroup, &gEfiEventExitBootServicesGuid) ? MtkUfsExitBootServices : MtkUfsFdtTableChanged));
  CHECK (NotifyContext == NULL);
  // Match DXE CoreCreateEventInternal: NotifyTpl must be greater than
  // TPL_APPLICATION and below TPL_HIGH_LEVEL.
  if ((NotifyTpl <= TPL_APPLICATION) || (NotifyTpl >= TPL_HIGH_LEVEL)) {
    return EFI_INVALID_PARAMETER;
  }
  CHECK (NotifyTpl == TPL_CALLBACK);
  if (GuidEqual (EventGroup, &gFdtTableGuid)) {
    CreateEventCalls++;
    if (EFI_ERROR (CreateFdtEventStatus)) { *Event = (EFI_EVENT)(UINTN)0xbad0U; return CreateFdtEventStatus; }
    FdtNotify = NotifyFunction;
    FdtEventOpen = TRUE; *Event = FDT_EVENT_HANDLE;
  } else if (GuidEqual (EventGroup, &gEfiEndOfDxeEventGroupGuid)) {
    CreateEndCalls++;
    if (EFI_ERROR (CreateEndStatus)) { *Event = (EFI_EVENT)(UINTN)0xbad0U; return CreateEndStatus; }
    EndOfDxeNotify = NotifyFunction;
    EndEventOpen = TRUE; *Event = END_EVENT_HANDLE;
  } else if (GuidEqual (EventGroup, &gEfiEventReadyToBootGuid)) {
    CreateReadyEventCalls++;
    if (EFI_ERROR (CreateReadyEventStatus)) { *Event = (EFI_EVENT)(UINTN)0xbad0U; return CreateReadyEventStatus; }
    ReadyToBootNotify = NotifyFunction;
    ReadyEventOpen = TRUE; *Event = READY_EVENT_HANDLE;
  } else if (GuidEqual(EventGroup, &gEfiEventExitBootServicesGuid)) {
    CreateExitCalls++;
    if (EFI_ERROR (CreateExitStatus)) { *Event = (EFI_EVENT)(UINTN)0xbad0U; return CreateExitStatus; }
    ExitNotify = NotifyFunction; ExitEventOpen = TRUE; *Event = EXIT_EVENT_HANDLE;
  } else {
    CHECK (FALSE);
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockCloseEvent (IN EFI_EVENT Event) {
  if (Event == END_EVENT_HANDLE) {
    CHECK(EndEventOpen); CloseEndCalls++;
    if (EFI_ERROR(CloseEndStatus)) return CloseEndStatus;
    EndEventOpen = FALSE; EndOfDxeNotify = NULL; return EFI_SUCCESS;
  }
  if (Event == EXIT_EVENT_HANDLE) {
    CHECK(ExitEventOpen); CloseExitCalls++;
    if (EFI_ERROR(CloseExitStatus)) return CloseExitStatus;
    ExitEventOpen = FALSE; ExitNotify = NULL; return EFI_SUCCESS;
  }
  if (Event == FDT_EVENT_HANDLE) {
    CHECK (FdtEventOpen);
    CloseFdtEventCalls++;
    if (EFI_ERROR (CloseFdtEventStatus)) return CloseFdtEventStatus;
    FdtEventOpen = FALSE; FdtNotify = NULL;
    return EFI_SUCCESS;
  }
  CHECK (Event == READY_EVENT_HANDLE && ReadyEventOpen);
  CloseReadyEventCalls++;
  if (EFI_ERROR (CloseReadyEventStatus)) return CloseReadyEventStatus;
  ReadyEventOpen = FALSE; ReadyToBootNotify = NULL;
  return EFI_SUCCESS;
}

static VOID
FireEventAtCallbackTpl (
  IN EFI_EVENT_NOTIFY Notify,
  IN EFI_EVENT        Event
  )
{
  CHECK (Notify != NULL);
  CHECK (CurrentTpl == TPL_APPLICATION);
  CurrentTpl = TPL_CALLBACK;
  Notify (Event, NULL);
  CurrentTpl = TPL_APPLICATION;
}

static VOID
SyncUfsProtocolAggregate (VOID)
{
  UfsProtocolsInstalled = (BOOLEAN)(UfsHostProtocolInstalled && UfsPlatformProtocolInstalled);
}

static EFI_STATUS EFIAPI MockInstallProtocolInterface (
  IN OUT EFI_HANDLE *Handle, IN EFI_GUID *Protocol, IN EFI_INTERFACE_TYPE InterfaceType,
  IN VOID *Interface
  ) {
  EFI_STATUS Status;
  UINT32 ProtocolMask;

  CHECK (InterfaceType == EFI_NATIVE_INTERFACE);
  if (GuidEqual (Protocol, &gEfiDriverBindingProtocolGuid)) {
    BindingInstallCalls++;
    CHECK (Interface == &mDriverBinding);
    if (!EFI_ERROR (InstallBindingStatus) || PartialBindingInstall) {
      *Handle = IMAGE_HANDLE; BindingInstalled = TRUE;
    }
    return InstallBindingStatus;
  }
  if (GuidEqual (Protocol, &gEfiDevicePathProtocolGuid)) {
    SeedInstallCalls++;
    CHECK (Interface == &mSeedDevicePath);
    if (!EFI_ERROR (InstallSeedStatus) || PartialSeedInstall) {
      *Handle = SEED_HANDLE; SeedHandle = SEED_HANDLE; SeedInstalled = TRUE;
    }
    return InstallSeedStatus;
  }
  CHECK (*Handle == SEED_HANDLE);
  UfsSingleProtocolInstallCalls++;
  if (GuidEqual (Protocol, &gEdkiiUfsHostControllerProtocolGuid)) {
    CHECK (Interface == &mContext.HostController);
    Status = InstallUfsHostProtocolStatus;
    ProtocolMask = 1U;
    if (!EFI_ERROR (Status) || (PartialUfsInstallMask & ProtocolMask) != 0U) {
      UfsHostProtocolInstalled = TRUE;
    }
  } else if (GuidEqual (Protocol, &gEdkiiUfsHcPlatformProtocolGuid)) {
    CHECK (Interface == &mContext.Platform);
    Status = InstallUfsPlatformProtocolStatus;
    ProtocolMask = 2U;
    if (!EFI_ERROR (Status) || (PartialUfsInstallMask & ProtocolMask) != 0U) {
      UfsPlatformProtocolInstalled = TRUE;
    }
  } else {
    CHECK (FALSE);
    return EFI_UNSUPPORTED;
  }

  if (UfsHostProtocolInstalled || UfsPlatformProtocolInstalled) *Handle = SEED_HANDLE;
  SyncUfsProtocolAggregate ();
  return Status;
}

static EFI_STATUS EFIAPI MockUninstallProtocolInterface (
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, IN VOID *Interface
  ) {
  EFI_STATUS Status;
  UINT32 ProtocolMask;

  if (GuidEqual (Protocol, &gEfiDriverBindingProtocolGuid)) {
    BindingUninstallCalls++;
    CHECK (Handle == IMAGE_HANDLE && Interface == &mDriverBinding);
    if (EFI_ERROR (UninstallBindingStatus)) return UninstallBindingStatus;
    if (!BindingInstalled) return EFI_NOT_FOUND;
    BindingInstalled = FALSE; return EFI_SUCCESS;
  }
  if (GuidEqual (Protocol, &gEfiDevicePathProtocolGuid)) {
    SeedUninstallCalls++;
    CHECK (Handle == SEED_HANDLE && Interface == &mSeedDevicePath);
    if (EFI_ERROR (UninstallSeedStatus)) return UninstallSeedStatus;
    if (!SeedInstalled) return EFI_NOT_FOUND;
    SeedInstalled = FALSE; SeedHandle = NULL; return EFI_SUCCESS;
  }

  CHECK (Handle == SEED_HANDLE);
  UfsSingleProtocolUninstallCalls++;
  if (GuidEqual (Protocol, &gEdkiiUfsHostControllerProtocolGuid)) {
    CHECK (Interface == &mContext.HostController);
    Status = UninstallUfsHostProtocolStatus;
    ProtocolMask = 1U;
    if (EFI_ERROR (Status) && (PartialUfsUninstallMask & ProtocolMask) == 0U) return Status;
    if (!UfsHostProtocolInstalled) return EFI_NOT_FOUND;
    UfsHostProtocolInstalled = FALSE;
  } else if (GuidEqual (Protocol, &gEdkiiUfsHcPlatformProtocolGuid)) {
    CHECK (Interface == &mContext.Platform);
    Status = UninstallUfsPlatformProtocolStatus;
    ProtocolMask = 2U;
    if (EFI_ERROR (Status) && (PartialUfsUninstallMask & ProtocolMask) == 0U) return Status;
    if (!UfsPlatformProtocolInstalled) return EFI_NOT_FOUND;
    UfsPlatformProtocolInstalled = FALSE;
  } else {
    CHECK (FALSE);
    return EFI_UNSUPPORTED;
  }

  SyncUfsProtocolAggregate ();
  return Status;
}

static EFI_STATUS EFIAPI MockHandleProtocol (
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface
  ) {
  CHECK (CurrentTpl <= TPL_CALLBACK && Protocol != NULL && Interface != NULL);
  if (GuidEqual (Protocol, &gEfiUfsDeviceConfigProtocolGuid)) {
    DeviceConfigLookupCalls++;
    if (Handle == NULL || Handle != DeviceConfigHandle) return EFI_NOT_FOUND;
    if (EFI_ERROR(DeviceConfigStatus)) return DeviceConfigStatus;
    *Interface = &DeviceConfigProtocol;
    return EFI_SUCCESS;
  }
  CHECK (Handle == SEED_HANDLE);
  if (GuidEqual (Protocol, &gEdkiiUfsHostControllerProtocolGuid)) {
    UfsHostProtocolProbeCalls++;
    if (EFI_ERROR (UfsProtocolProbeStatus)) return UfsProtocolProbeStatus;
    if (!UfsHostProtocolInstalled) return EFI_NOT_FOUND;
    *Interface = &mContext.HostController;
    return EFI_SUCCESS;
  }
  if (GuidEqual (Protocol, &gEdkiiUfsHcPlatformProtocolGuid)) {
    UfsPlatformProtocolProbeCalls++;
    if (EFI_ERROR (UfsProtocolProbeStatus)) return UfsProtocolProbeStatus;
    if (!UfsPlatformProtocolInstalled) return EFI_NOT_FOUND;
    *Interface = &mContext.Platform;
    return EFI_SUCCESS;
  }
  CHECK (GuidEqual (Protocol, &gEfiDevicePathProtocolGuid));
  if (!SeedInstalled) return EFI_NOT_FOUND;
  *Interface = &mSeedDevicePath;
  return EFI_SUCCESS;
}

static EFI_STATUS
ExerciseUfsCallbacksDuringInstall (VOID)
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL *Host;
  EDKII_UFS_HC_DRIVER_INTERFACE DriverInterface;
  UINTN MmioBar;
  UINT32 Value;
  VOID *HostAddress;
  VOID *Mapping;
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  UINTN NumberOfBytes;

  Host = &mContext.HostController;
  CHECK (mContext.Starting && mContext.StartBusy && !mContext.Started &&
         !mContext.CallbackClosing && CurrentTpl == TPL_APPLICATION);
  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_NOT_READY);
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_NOT_READY);
  CHECK (mContext.Starting && mContext.StartBusy && !mContext.StopBusy &&
         !mContext.CallbackClosing && !mContext.Started);

  MmioBar = 0U;
  CHECK (Host->GetUfsHcMmioBar (Host, &MmioBar) == EFI_SUCCESS);
  CHECK (MmioBar == (UINTN)UFS_BASE);

  Value = 0U;
  CHECK (Host->Read (Host, EfiUfsHcWidthUint32, UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_SUCCESS);
  CHECK (Value == HceValue);
  Value = 0U;
  CHECK (Host->Write (Host, EfiUfsHcWidthUint32, UFS_HC_UTRLDBR_OFFSET, 1U, &Value) == EFI_SUCCESS);
  CHECK (TransferDoorbellValue == 0U);

  DriverInterface.UfsHcProtocol = Host;
  DriverInterface.UfsExecUicCommand = ColdUic;
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPreHce, &DriverInterface) == EFI_SUCCESS);

  HostAddress = NULL;
  CHECK (Host->AllocateBuffer (Host, AllocateAnyPages, EfiBootServicesData, 1U,
          &HostAddress, 0U) == EFI_SUCCESS);
  CHECK (HostAddress != NULL);
  NumberOfBytes = 64U;
  Mapping = NULL;
  DeviceAddress = 0U;
  CHECK (Host->Map (Host, EdkiiUfsHcOperationBusMasterCommonBuffer, HostAddress,
          &NumberOfBytes, &DeviceAddress, &Mapping) == EFI_SUCCESS);
  CHECK (Mapping != NULL && DeviceAddress == (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress);
  CHECK (Host->Flush (Host) == EFI_SUCCESS);
  CHECK (Host->Unmap (Host, Mapping) == EFI_SUCCESS);
  CHECK (Host->FreeBuffer (Host, 1U, HostAddress) == EFI_SUCCESS);
  return EFI_SUCCESS;
}

static VOID CheckManufacturerReadOnlyPath(VOID)
{
  UINTN Lookups, Reads;
  DeviceConfigHandle = mContext.ControllerHandle;
  CHECK(DeviceConfigHandle != NULL && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady);
  mContext.ColdHost.RefClockWaitConfigured = FALSE;
  FireEventAtCallbackTpl(FdtNotify, FDT_EVENT_HANDLE);
  CHECK(mContext.ColdHost.RefClockWaitConfigured && mContext.ColdHost.Platform.Config.RefClockGatingWaitUs == 1U);
  CHECK(DeviceConfigLookupCalls == 1U && DescriptorReadCalls == 1U);
  CHECK(DescriptorLastRead && DescriptorLastId == 0 && DescriptorLastIndex == 0 && DescriptorLastSelector == 0);

  mContext.ColdHost.RefClockWaitConfigured = FALSE;
  DeviceDescriptor[0x18] = 0x01; DeviceDescriptor[0x19] = 0xAD;
  CHECK(MtkUfsConfigureManufacturer() == EFI_SUCCESS && mContext.ColdHost.Platform.Config.RefClockGatingWaitUs == 30U);
  mContext.ColdHost.RefClockWaitConfigured = FALSE;
  DeviceDescriptor[0x18] = 0x01; DeviceDescriptor[0x19] = 0x98;
  CHECK(MtkUfsConfigureManufacturer() == EFI_SUCCESS && mContext.ColdHost.Platform.Config.RefClockGatingWaitUs == 100U);
  mContext.ColdHost.RefClockWaitConfigured = FALSE;
  DeviceDescriptor[0x18] = 0xFF; DeviceDescriptor[0x19] = 0xFF;
  CHECK(MtkUfsConfigureManufacturer() == EFI_SUCCESS && mContext.ColdHost.Platform.Config.RefClockGatingWaitUs == 32U);

  mContext.ColdHost.RefClockWaitConfigured = FALSE;
  DescriptorReturnedSize = 0x19;
  CHECK(MtkUfsConfigureManufacturer() == EFI_COMPROMISED_DATA && !mContext.ColdHost.RefClockWaitConfigured);
  DescriptorReturnedSize = 0x1A; DeviceDescriptor[1] = 1;
  CHECK(MtkUfsConfigureManufacturer() == EFI_COMPROMISED_DATA && !mContext.ColdHost.RefClockWaitConfigured);
  DeviceDescriptor[1] = 0; DeviceDescriptor[0] = 0x19;
  CHECK(MtkUfsConfigureManufacturer() == EFI_COMPROMISED_DATA && !mContext.ColdHost.RefClockWaitConfigured);
  DeviceDescriptor[0] = 0x1A; DescriptorReturnedSize = sizeof(DeviceDescriptor) + 1U;
  CHECK(MtkUfsConfigureManufacturer() == EFI_COMPROMISED_DATA && !mContext.ColdHost.RefClockWaitConfigured);
  DescriptorReturnedSize = 0x1A;

  DescriptorReadStatus = EFI_DEVICE_ERROR;
  CHECK(MtkUfsConfigureManufacturer() == EFI_DEVICE_ERROR && !mContext.ColdHost.RefClockWaitConfigured);
  DescriptorReadStatus = EFI_SUCCESS;
  DeviceConfigStatus = EFI_ACCESS_DENIED;
  CHECK(MtkUfsConfigureManufacturer() == EFI_ACCESS_DENIED && !mContext.ColdHost.RefClockWaitConfigured);
  DeviceConfigStatus = EFI_SUCCESS;

  DeviceConfigHandle = (EFI_HANDLE)(UINTN)0x9090U;
  Reads = DescriptorReadCalls;
  CHECK(MtkUfsConfigureManufacturer() == EFI_NOT_FOUND && DescriptorReadCalls == Reads);
  DeviceConfigHandle = mContext.ControllerHandle;
  Lookups = DeviceConfigLookupCalls; Reads = DescriptorReadCalls;
  TransferDoorbellValue = 1;
  CHECK(MtkUfsConfigureManufacturer() == EFI_NOT_READY && DeviceConfigLookupCalls == Lookups && DescriptorReadCalls == Reads);
  TransferDoorbellValue = 0; TaskDoorbellValue = 1;
  CHECK(MtkUfsConfigureManufacturer() == EFI_NOT_READY && DeviceConfigLookupCalls == Lookups && DescriptorReadCalls == Reads);
  TaskDoorbellValue = 0;

  /* ExitBootServices must stop both the protocol getter and the FDT/Ready event
   * path before either can reach the descriptor interface. */
  mContext.ExitedBootServices = TRUE;
  CHECK(MtkUfsConfigureManufacturer() == EFI_NOT_READY);
  MtkUfsFdtTableChanged(FDT_EVENT_HANDLE, NULL);
  CHECK(DeviceConfigLookupCalls == Lookups && DescriptorReadCalls == Reads);
  mContext.ExitedBootServices = FALSE;
  CHECK(MtkUfsColdHostSetManufacturer(&mContext.ColdHost, 0x01CEU) == EFI_SUCCESS);
}

static EFI_STATUS EFIAPI MockConnectController (
  IN EFI_HANDLE ControllerHandle, IN EFI_HANDLE *DriverImageHandle,
  IN EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath, IN BOOLEAN Recursive
  ) {
  ConnectCalls++;
  CHECK (ControllerHandle == SEED_HANDLE);
  CHECK (DriverImageHandle != NULL);
  CHECK (DriverImageHandle[0] == IMAGE_HANDLE);
  CHECK (DriverImageHandle[1] == NULL);
  CHECK (RemainingDevicePath == NULL && Recursive == FALSE);
  ConnectSupportedStatus = mDriverBinding.Supported (&mDriverBinding, ControllerHandle, NULL);
  if (!EFI_ERROR (ConnectSupportedStatus)) {
    ConnectStartStatus = mDriverBinding.Start (&mDriverBinding, ControllerHandle, NULL);
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockInstallMultiple (IN OUT EFI_HANDLE *Handle, ...) {
  va_list Args;
  EFI_GUID *Protocol;
  VOID *Interface;
  UINTN Seen;
  UINT32 RequestedMask;
  UINT32 AddedMask;

  va_start (Args, Handle);
  Seen = 0;
  RequestedMask = 0U;
  while ((Protocol = va_arg (Args, EFI_GUID *)) != NULL) {
    Interface = va_arg (Args, VOID *);
    if (GuidEqual (Protocol, &gEdkiiUfsHostControllerProtocolGuid)) {
      CHECK (Interface == &mContext.HostController); Seen++; RequestedMask |= 1U;
    } else if (GuidEqual (Protocol, &gEdkiiUfsHcPlatformProtocolGuid)) {
      CHECK (Interface == &mContext.Platform); Seen++; RequestedMask |= 2U;
    } else {
      CHECK (FALSE);
    }
  }
  va_end (Args);
  UfsProtocolInstallCalls++;
  CHECK (CurrentTpl <= TPL_CALLBACK);
  CHECK (Seen >= 1U && Seen <= 2U);

  if (EFI_ERROR (InstallUfsProtocolsStatus)) {
    AddedMask = PartialUfsInstallMask & RequestedMask;
  } else {
    AddedMask = RequestedMask;
  }
  if ((AddedMask & 1U) != 0U) UfsHostProtocolInstalled = TRUE;
  if ((AddedMask & 2U) != 0U) UfsPlatformProtocolInstalled = TRUE;
  if (AddedMask != 0U || !EFI_ERROR (InstallUfsProtocolsStatus)) *Handle = SEED_HANDLE;
  SyncUfsProtocolAggregate ();

  if (RunInstallCallbackExercise && UfsHostProtocolInstalled &&
      UfsPlatformProtocolInstalled)
  {
    RunInstallCallbackExercise = FALSE;
    PublishCallbackCount++;
    PublishCallbackStatus = ExerciseUfsCallbacksDuringInstall ();
    if (EFI_ERROR (PublishCallbackStatus)) return PublishCallbackStatus;
  }
  return InstallUfsProtocolsStatus;
}

static EFI_STATUS EFIAPI MockUninstallMultiple (IN EFI_HANDLE Handle, ...) {
  va_list Args;
  EFI_GUID *Protocol;
  VOID *Interface;
  UINTN Seen;
  UINT32 RequestedMask;
  UINT32 RemovedMask;

  CHECK (Handle == SEED_HANDLE);
  va_start (Args, Handle);
  Seen = 0;
  RequestedMask = 0U;
  while ((Protocol = va_arg (Args, EFI_GUID *)) != NULL) {
    Interface = va_arg (Args, VOID *);
    if (GuidEqual (Protocol, &gEdkiiUfsHostControllerProtocolGuid)) {
      CHECK (Interface == &mContext.HostController); Seen++; RequestedMask |= 1U;
    } else if (GuidEqual (Protocol, &gEdkiiUfsHcPlatformProtocolGuid)) {
      CHECK (Interface == &mContext.Platform); Seen++; RequestedMask |= 2U;
    } else {
      CHECK (FALSE);
    }
  }
  va_end (Args);
  UfsProtocolUninstallCalls++;
  CHECK (Seen >= 1U && Seen <= 2U);

  if (EFI_ERROR (UninstallUfsProtocolsStatus)) {
    RemovedMask = PartialUfsUninstallMask & RequestedMask;
    if ((RemovedMask & 1U) != 0U) UfsHostProtocolInstalled = FALSE;
    if ((RemovedMask & 2U) != 0U) UfsPlatformProtocolInstalled = FALSE;
    SyncUfsProtocolAggregate ();
    return UninstallUfsProtocolsStatus;
  }

  if ((RequestedMask & 1U) != 0U) UfsHostProtocolInstalled = FALSE;
  if ((RequestedMask & 2U) != 0U) UfsPlatformProtocolInstalled = FALSE;
  SyncUfsProtocolAggregate ();
  return EFI_SUCCESS;
}

static void
ResetHarness (void)
{
  ZeroMem (&mContext, sizeof (mContext));
  InitializeListHead (&mContext.CommonAllocations);
  InitializeListHead (&mContext.Mappings);
  HandoffZeroPoolAllocations = HandoffPoolFrees = 0U;
  DeviceConfigHandle = NULL; DeviceConfigStatus = DescriptorReadStatus = EFI_SUCCESS;
  ColdProviderCalls = ClockGetIdCalls = LocateProtocolCalls = 0U;
  GcdDescriptorCalls = SystemConfigTableCalls = 0U;
  HandoffMmioReadCalls = HandoffMmioWriteCalls = 0U;
  NestedEbsClockGetIdTarget = NestedEbsClockGetIdCalls = 0U;
  NestedEbsClockGetIdSnapshotValid = FALSE;
  ZeroMem (&NestedEbsClockGetIdSnapshot, sizeof (NestedEbsClockGetIdSnapshot));
  ZeroMem(DeviceDescriptor, sizeof(DeviceDescriptor));
  DeviceDescriptor[0] = 0x1A; DeviceDescriptor[1] = 0; DeviceDescriptor[0x18] = 0x01; DeviceDescriptor[0x19] = 0xCE;
  DescriptorReturnedSize = 0x1A; DeviceConfigLookupCalls = DescriptorReadCalls = 0;
  DescriptorLastId = DescriptorLastIndex = DescriptorLastSelector = 0; DescriptorLastRead = FALSE;
  DeviceConfigProtocol.RwUfsDescriptor = MockRwUfsDescriptor;
  ZeroMem (&mDriverBinding, sizeof (mDriverBinding));
  ZeroMem (&mSeedDevicePath, sizeof (mSeedDevicePath));
  mFdtEvent = NULL; mReadyToBootEvent = NULL; mExitBootServicesEvent = NULL; mEntryQuarantined = FALSE;
  ExitNotify = NULL; ExitEventOpen = HceStuck = StopStuck = BaseStuck = FALSE;
  memset(OsHandoff,0xa5,sizeof(OsHandoff)); OsPublications = OsCacheCleans = 0; CreateExitStatus = CloseExitStatus = EFI_SUCCESS;
  CreateExitCalls = CloseExitCalls = 0; TimeNs = 0;
  FdtNotify = NULL; ReadyToBootNotify = NULL; SeedHandle = NULL;
  EndOfDxeNotify = NULL; EndEventOpen = FALSE;
  CreateEndStatus = CloseEndStatus = EFI_SUCCESS;
  CreateEndCalls = CloseEndCalls = 0;
  CurrentTpl = TPL_APPLICATION;
  RestoreTplHook = NULL; RestoreTplHookCalls = 0U;
  WaitForEventStartStopHook = FALSE;
  RestoreTplStopStatus = EFI_NOT_READY; RestoreTplStopCalls = 0U;
  RuntimeTree = NULL; TableOverride = NULL; TableStatus = EFI_SUCCESS;
  HandoffMagic = HANDOFF_MAGIC; HandoffStatus = EFI_SUCCESS;
  GcdStatus = EFI_SUCCESS; FdtMemoryType = EfiGcdMemoryTypeSystemMemory;
  UfsMemoryType = EfiGcdMemoryTypeMemoryMappedIo; UfsMemoryAttributes = EFI_MEMORY_UC;
  CreateFdtEventStatus = EFI_SUCCESS; CreateReadyEventStatus = EFI_SUCCESS;
  InstallBindingStatus = EFI_SUCCESS; InstallSeedStatus = EFI_SUCCESS;
  UninstallBindingStatus = EFI_SUCCESS; UninstallSeedStatus = EFI_SUCCESS;
  CloseFdtEventStatus = EFI_SUCCESS; CloseReadyEventStatus = EFI_SUCCESS;
  InstallUfsProtocolsStatus = EFI_SUCCESS; UninstallUfsProtocolsStatus = EFI_SUCCESS;
  InstallUfsHostProtocolStatus = InstallUfsPlatformProtocolStatus = EFI_SUCCESS;
  UninstallUfsHostProtocolStatus = UninstallUfsPlatformProtocolStatus = EFI_SUCCESS;
  UfsProtocolProbeStatus = EFI_SUCCESS;
  PartialUfsInstallMask = PartialUfsUninstallMask = 0U;
  RunInstallCallbackExercise = FALSE;
  PublishCallbackStatus = EFI_NOT_READY; PublishCallbackCount = 0U;
  UfsHostProtocolProbeCalls = UfsPlatformProtocolProbeCalls = 0U;
  UfsSingleProtocolInstallCalls = UfsSingleProtocolUninstallCalls = 0U;
  PartialBindingInstall = FALSE; PartialSeedInstall = FALSE;
  FdtEventOpen = FALSE; ReadyEventOpen = FALSE;
  BindingInstalled = FALSE; SeedInstalled = FALSE;
  UfsProtocolsInstalled = FALSE;
  UfsHostProtocolInstalled = UfsPlatformProtocolInstalled = FALSE;
  CreateEventCalls = 0; CreateReadyEventCalls = 0;
  CloseFdtEventCalls = 0; CloseReadyEventCalls = 0;
  BindingInstallCalls = 0; SeedInstallCalls = 0; BindingUninstallCalls = 0;
  SeedUninstallCalls = 0; ConnectCalls = 0; MmioReadCalls = 0; MmioWriteCalls = 0;
  UfsProtocolInstallCalls = 0; UfsProtocolUninstallCalls = 0;
  RaiseTplCalls = RestoreTplCalls = 0U;
  ZeroMem (&EbsHookSnapshot, sizeof (EbsHookSnapshot)); EbsHookSnapshotValid = FALSE;
  EbsHookEntryMmioRead = EbsHookEntryMmioWrite = 0U;
  GuardUfsMmioResourcesReady = FALSE;
  UfsMmioReadsWithResourcesOff = 0;
  FirstUfsMmioResourceTraceCount = MAX_UINTN;
  HceValue = MTK_UFS_HCE_ENABLED;
  HcsValue = MTK_UFS_HCS_UIC_COMMAND_READY | MTK_UFS_HCS_DEVICE_PRESENT;
  TransferDoorbellValue = 0; TaskDoorbellValue = 0; CapabilitiesValue = UFS_HC_CAP_64ADDR;
  ConnectSupportedStatus = EFI_NOT_READY; ConnectStartStatus = EFI_NOT_READY;
  NextFakeDmaAddress = TEST_DMA_BASE;
  AllocatePagesCalls = FreePagesCalls = SetMemoryAttributesCalls = 0U;
  AllocatePagesStatus = FreePagesStatus = SetMemoryAttributesStatus = EFI_SUCCESS;
  LastAllocatedPageAddress = 0U; LastAllocatedPageCount = 0U;
  ZeroMem (FakePageAllocations, sizeof (FakePageAllocations));
  FakeDmaAttributes = EFI_MEMORY_WB;
  CacheWriteBackCalls = CacheWriteBackInvalidateCalls = CacheInvalidateCalls = 0U;
  ZeroMem (&Services, sizeof (Services)); ZeroMem (&DxeServices, sizeof (DxeServices));
  Services.CreateEventEx = MockCreateEventEx; Services.CloseEvent = MockCloseEvent;
  Services.RaiseTPL = MockRaiseTpl; Services.RestoreTPL = MockRestoreTpl;
  Services.AllocatePages = MockAllocatePages; Services.FreePages = MockFreePages;
  Services.InstallProtocolInterface = MockInstallProtocolInterface;
  Services.UninstallProtocolInterface = MockUninstallProtocolInterface;
  Services.HandleProtocol = MockHandleProtocol; Services.ConnectController = MockConnectController;
  Services.InstallMultipleProtocolInterfaces = MockInstallMultiple;
  Services.UninstallMultipleProtocolInterfaces = MockUninstallMultiple;
  DxeServices.GetMemorySpaceDescriptor = GetGcdDescriptor;
  DxeServices.SetMemorySpaceAttributes = MockSetMemorySpaceAttributes;
  Services.LocateProtocol = LocateCold;
  SmcCalls = Smc32OffCalls = Smc32OnCalls = ResetWrites = UicCalls = 0;
  ZeroMem(ColdRegs, sizeof(ColdRegs)); ZeroMem(Attributes, sizeof(Attributes));
  ColdRegs[0x2240 / 4] = 0x10450000;
  ColdResources = FALSE; VemcEnabled = TRUE;
  for (UINTN I = 0; I < ARRAY_SIZE(ClockState); I++) ClockState[I] = TRUE;
  VemcVoltage = 3000000; ColdFrequency = 26000000;
  ColdClockStatus = ColdPmicStatus = UicStatus = EFI_SUCCESS;
  SmcError = 0; UicResult = 0; IoCloseAtUicCall = 0U;
  IoClosedMmioReadCalls = IoClosedMmioWriteCalls = IoClosedSmcCalls = 0U;
  IoClosedUicCalls = IoClosedResetWrites = IoClosedResourceEvents = 0U;
  IoClosedSnapshotValid = FALSE;
  ClockSetFailId = ClockDisableFailId = ClockDisableNoWriteId = ClockEnableNoWriteId = MAX_UINT32;
  ClockEnableNoEffectId = ClockEnableReadFailId = MAX_UINT32;
  PmicSetFailAfterWrite = PmicDisableNoWrite = FALSE;
  ResourceTraceCount = HibernateCalls = 0;
}

static VOID *
BuildTree (
  IN UINT32  Fault
  )
{
  VOID *Tree;
  INT32 Soc;
  INT32 Ufs;
  fdt32_t Reg[4] = { cpu_to_fdt32 (0), cpu_to_fdt32 ((Fault & 1U) ? 0x11271000U : (UINT32)UFS_BASE),
                     cpu_to_fdt32 (0), cpu_to_fdt32 (UFS_LENGTH) };
  fdt32_t DmaRanges[6] = { cpu_to_fdt32 (0), cpu_to_fdt32 (0), cpu_to_fdt32 (0),
                           cpu_to_fdt32 (0), cpu_to_fdt32 ((Fault & 2U) ? 0x0fU : 0x10U),
                           cpu_to_fdt32 (0) };
  fdt32_t Marker = cpu_to_fdt32 (1);

  Tree = calloc (1, TEST_TREE_CAP);
  CHECK (Tree != NULL);
  CHECK (fdt_create_empty_tree (Tree, TEST_TREE_CAP) == 0);
  CHECK (fdt_setprop_string (Tree, 0, "compatible",
                             (Fault & 4U) ? "mediatek,not-this-board" : "mediatek,MT6789") == 0);
  CHECK (fdt_setprop_u32 (Tree, 0, "#address-cells", 2) == 0);
  CHECK (fdt_setprop_u32 (Tree, 0, "#size-cells", 2) == 0);
  Soc = fdt_add_subnode (Tree, 0, "soc"); CHECK (Soc >= 0);
  CHECK (fdt_setprop_u32 (Tree, Soc, "#address-cells", 2) == 0);
  CHECK (fdt_setprop_u32 (Tree, Soc, "#size-cells", 2) == 0);
  CHECK (fdt_setprop (Tree, Soc, "ranges", NULL, 0) == 0);
  CHECK (fdt_setprop (Tree, Soc, "dma-ranges", DmaRanges, sizeof (DmaRanges)) == 0);
  if ((Fault & 8U) != 0U) CHECK (fdt_setprop (Tree, Soc, "iommu-map", &Marker, sizeof (Marker)) == 0);
  if ((Fault & 16U) != 0U) CHECK (fdt_setprop (Tree, Soc, "dma-ranges-extra", &Marker, sizeof (Marker)) == 0);
  Ufs = fdt_add_subnode (Tree, Soc, "ufshci@11270000"); CHECK (Ufs >= 0);
  CHECK (fdt_setprop_string (Tree, Ufs, "compatible", "mediatek,mt8183-ufshci") == 0);
  CHECK (fdt_setprop (Tree, Ufs, "reg", Reg, sizeof (Reg)) == 0);
  if ((Fault & 32U) != 0U) CHECK (fdt_setprop_string (Tree, Ufs, "status", "disabled") == 0);
  if ((Fault & 64U) != 0U) CHECK (fdt_setprop (Tree, Ufs, "iommus", &Marker, sizeof (Marker)) == 0);
  if ((Fault & 128U) != 0U) CHECK (fdt_setprop (Tree, Ufs, "dma-coherent", NULL, 0) == 0);
  if ((Fault & 256U) != 0U) {
    INT32 Other = fdt_add_subnode (Tree, Soc, "ufshci@11274000"); CHECK (Other >= 0);
    CHECK (fdt_setprop_string (Tree, Other, "compatible", "mediatek,mt8183-ufshci") == 0);
  }
  CHECK (fdt_pack (Tree) == 0);
  CHECK (FdtCheckFull (Tree, FdtTotalSize (Tree)) == 0);
  return Tree;
}

static VOID
PublishRuntimeTree (
  IN CONST VOID  *Tree
  )
{
  RuntimeTree = Tree; TableOverride = NULL; TableStatus = EFI_SUCCESS;
  HandoffMagic = HANDOFF_MAGIC; HandoffStatus = EFI_SUCCESS;
}

static VOID
CheckRejectedBeforeMmio (
  IN CONST VOID  *Tree
  )
{
  PublishRuntimeTree (Tree);
  MmioReadCalls = 0;
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) != EFI_SUCCESS);
  CHECK (MmioReadCalls == 0U);
}

static VOID
CheckEntryFailureCases (VOID)
{
  EFI_STATUS Status;

  ResetHarness (); CreateFdtEventStatus = EFI_OUT_OF_RESOURCES;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_OUT_OF_RESOURCES && CreateEventCalls == 1U);
  CHECK (CreateReadyEventCalls == 0U && !FdtEventOpen && !ReadyEventOpen);
  CHECK (BindingInstallCalls == 0U && SeedInstallCalls == 0U);

  ResetHarness (); CreateEndStatus = EFI_OUT_OF_RESOURCES;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_OUT_OF_RESOURCES && CreateEndCalls == 1 && !EndEventOpen);
  CHECK (CloseFdtEventCalls == 1 && CreateReadyEventCalls == 0);

  ResetHarness (); CreateReadyEventStatus = EFI_OUT_OF_RESOURCES;
  CloseEndStatus = EFI_DEVICE_ERROR;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_SUCCESS && EndEventOpen && mEntryQuarantined);
  FireEventAtCallbackTpl (EndOfDxeNotify, END_EVENT_HANDLE);
  CHECK (ConnectCalls == 0 && MmioReadCalls == 0);

  ResetHarness (); CreateReadyEventStatus = EFI_OUT_OF_RESOURCES;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_OUT_OF_RESOURCES && CreateEventCalls == 1U && CreateReadyEventCalls == 1U);
  CHECK (CloseFdtEventCalls == 1U && CloseReadyEventCalls == 0U);
  CHECK (!FdtEventOpen && !ReadyEventOpen && !mEntryQuarantined);

  ResetHarness (); CreateReadyEventStatus = EFI_OUT_OF_RESOURCES;
  CloseFdtEventStatus = EFI_DEVICE_ERROR;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_SUCCESS && FdtEventOpen && !ReadyEventOpen && mEntryQuarantined);
  FireEventAtCallbackTpl (FdtNotify, FDT_EVENT_HANDLE);
  CHECK (ConnectCalls == 0U && MmioReadCalls == 0U);

  ResetHarness (); InstallBindingStatus = EFI_ACCESS_DENIED;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_ACCESS_DENIED && CloseFdtEventCalls == 1U && CloseReadyEventCalls == 1U);
  CHECK (!FdtEventOpen && !ReadyEventOpen && !BindingInstalled && !SeedInstalled);
  CHECK (!mEntryQuarantined);

  ResetHarness (); InstallBindingStatus = EFI_ACCESS_DENIED;
  CloseReadyEventStatus = EFI_DEVICE_ERROR;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_SUCCESS && mEntryQuarantined && !FdtEventOpen && ReadyEventOpen);
  FireEventAtCallbackTpl (ReadyToBootNotify, READY_EVENT_HANDLE);
  CHECK (ConnectCalls == 0U && MmioReadCalls == 0U);

  ResetHarness (); InstallSeedStatus = EFI_DEVICE_ERROR; PartialSeedInstall = TRUE;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_DEVICE_ERROR && !FdtEventOpen && !ReadyEventOpen);
  CHECK (!BindingInstalled && !SeedInstalled);
  CHECK (SeedUninstallCalls == 1U && BindingUninstallCalls == 1U && !mEntryQuarantined);

  ResetHarness (); InstallSeedStatus = EFI_DEVICE_ERROR; PartialSeedInstall = TRUE;
  UninstallBindingStatus = EFI_ACCESS_DENIED;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_SUCCESS && mEntryQuarantined && !FdtEventOpen && !ReadyEventOpen);
  CHECK (BindingInstalled && !SeedInstalled);
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);

  ResetHarness (); InstallSeedStatus = EFI_DEVICE_ERROR; PartialSeedInstall = TRUE;
  CloseFdtEventStatus = EFI_DEVICE_ERROR;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_SUCCESS && mEntryQuarantined && FdtEventOpen && !ReadyEventOpen);
  CHECK (!BindingInstalled && !SeedInstalled);

  ResetHarness (); InstallBindingStatus = EFI_ACCESS_DENIED; PartialBindingInstall = TRUE;
  UninstallBindingStatus = EFI_ACCESS_DENIED;
  Status = MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL);
  CHECK (Status == EFI_SUCCESS && mEntryQuarantined && BindingInstalled);
  CHECK (!FdtEventOpen && !ReadyEventOpen);
}


static VOID *LoadStockTree(CONST CHAR8 *Path)
{
  FILE *Input = fopen(Path, "rb");
  VOID *Stock;
  VOID *Tree;
  long Length;
  CHECK(Input != NULL && fseek(Input, 0, SEEK_END) == 0);
  Length = ftell(Input);
  CHECK(Length > 0 && Length < 0x200000 && fseek(Input, 0, SEEK_SET) == 0);
  Stock = malloc((UINTN)Length);
  Tree = malloc((UINTN)Length + 4096);
  CHECK(Stock != NULL && Tree != NULL);
  CHECK(fread(Stock, 1, (UINTN)Length, Input) == (UINTN)Length);
  CHECK(fclose(Input) == 0);
  CHECK(fdt_open_into(Stock, Tree, (UINTN)Length + 4096) == 0);
  free(Stock);
  CHECK(fdt_pack(Tree) == 0 && FdtCheckFull(Tree, FdtTotalSize(Tree)) == 0);
  return Tree;
}

static VOID CheckOsHandoff(CONST CHAR8 *StockPath)
{
  UINTN I;
  VOID *Tree = LoadStockTree(StockPath);
  for (I = 0; I < 7; I++) {
    UINTN Reads, Writes;
    EFI_STATUS Expected = I < 2 ? EFI_SUCCESS : I == 3 ? EFI_DEVICE_ERROR :
                          (I == 2 || I == 4) ? EFI_TIMEOUT : EFI_NOT_READY;
    ResetHarness(); ColdResources = TRUE; PublishRuntimeTree(Tree);
    CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS && ExitEventOpen);
    CHECK(mContext.Started);
    if (I == 1) mContext.ColdStart = TRUE;
    if (I == 2) TransferDoorbellValue = 1;
    if (I == 3) HceValue = MAX_UINT32;
    if (I == 4) HceStuck = TRUE;
    if (I == 5) mContext.ColdHost.Suspended = TRUE;
    if (I == 6) {
      mContext.ColdHost.CleanupPending = TRUE;
      mContext.ColdHost.CleanupMode = MtkUfsColdCleanupStoppedHost;
    }
    Reads = MmioReadCalls; Writes = MmioWriteCalls;
    GuardUfsMmioResourcesReady = TRUE;
    FireEventAtCallbackTpl(ExitNotify, EXIT_EVENT_HANDLE);
    CHECK(mContext.ExitedBootServices && mContext.QuiesceStatus == Expected);
    if (I >= 5) {
      CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes && HceValue == 1);
      CHECK(!OsPublications && !OsHandoff[0] && TimeNs == 0);
    } else {
      CHECK(MmioWriteCalls == 4 && SmcCalls == 0 && UicCalls == 0 && ResetWrites == 0);
      CHECK(ColdRegs[UFS_HC_UTRLRSR_OFFSET / 4] == 0 && ColdRegs[UFS_HC_UTMRLRSR_OFFSET / 4] == 0);
      CHECK(TimeNs == ((I == 2 || I == 4) ? 100000000ULL : 0));
    }
    Reads = MmioReadCalls; Writes = MmioWriteCalls;
    FireEventAtCallbackTpl(ExitNotify, EXIT_EVENT_HANDLE);
    FireEventAtCallbackTpl(FdtNotify, FDT_EVENT_HANDLE);
    CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes);
    CHECK(MtkUfsWriteMmio(&mContext.HostController, EfiUfsHcWidthUint32, UFS_HC_UTRLDBR_OFFSET, 1, &CapabilitiesValue) == EFI_UNSUPPORTED);
    CHECK(MtkUfsDriverStart(&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  }
  free(Tree);
  ResetHarness(); CreateExitStatus = EFI_OUT_OF_RESOURCES;
  CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_OUT_OF_RESOURCES);
  CHECK(CreateExitCalls == 1 && !ExitEventOpen && !FdtEventOpen && !ReadyEventOpen);
  ResetHarness(); InstallBindingStatus = EFI_ACCESS_DENIED; CloseExitStatus = EFI_DEVICE_ERROR;
  CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK(mEntryQuarantined && ExitEventOpen && CloseExitCalls == 1);
  FireEventAtCallbackTpl(ExitNotify, EXIT_EVENT_HANDLE);
  CHECK(mContext.ExitedBootServices && MmioReadCalls == 0 && MmioWriteCalls == 0);
}

static VOID CheckRetainedOsHandoff(CONST CHAR8 *StockPath)
{
  FILE *Input=fopen(StockPath,"rb");
  VOID *Stock,*Tree;
  long Size;
  UINTN I;
  CHECK(Input != NULL && fseek(Input,0,SEEK_END) == 0);
  Size=ftell(Input); CHECK(Size>0 && Size<0x200000 && fseek(Input,0,SEEK_SET) == 0);
  Stock=malloc(Size); Tree=malloc(Size+4096); CHECK(Stock != NULL && Tree != NULL);
  CHECK(fread(Stock,1,Size,Input) == (size_t)Size); fclose(Input);
  for(I=0;I<11;I++) {
    INT32 Ufs,Gic;
    UINTN Reads, Writes;
    ResetHarness(); CHECK(fdt_open_into(Stock,Tree,Size+4096) == 0);
    Ufs=fdt_path_offset(Tree,"/soc/ufshci@11270000"); Gic=fdt_path_offset(Tree,"/interrupt-controller");
    CHECK(Ufs>=0 && Gic>=0);
    if(I==1) { fdt32_t Irq[]={0,cpu_to_fdt32(138),cpu_to_fdt32(4),0}; CHECK(fdt_setprop(Tree,Ufs,"interrupts",Irq,sizeof(Irq)) == 0); }
    if(I==2) CHECK(fdt_setprop_string(Tree,Gic,"status","disabled") == 0);
    CHECK(fdt_pack(Tree) == 0); PublishRuntimeTree(Tree); ColdResources=TRUE;
    HcsValue=15; ColdRegs[UFS_HC_VER_OFFSET/4]=0x210;
    ColdRegs[0x50/4]=0x80000000; ColdRegs[0x54/4]=1; ColdRegs[0x70/4]=0x80000400; ColdRegs[0x74/4]=1;
    ColdRegs[0x60/4]=1; ColdRegs[0x80/4]=1; ColdRegs[0x24/4]=MTK_UFS_INTERRUPTS;
    CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE,NULL) == EFI_SUCCESS && mContext.Started);
    if(I==3) for (UINTN C = 0; C < 5; C++) ClockState[C] = FALSE;
    if(I==4) VemcEnabled=FALSE;
    if(I==5) ColdRegs[UFS_HC_IS_OFFSET/4]=1U<<16;
    if(I==6) HcsValue=MAX_UINT32;
    if(I==7) ColdRegs[UFS_HC_VER_OFFSET/4]=0x400;
    if(I==8) StopStuck=TRUE;
    if(I==9) BaseStuck=TRUE;
    if(I==10) ColdRegs[UFS_HC_AHIT_OFFSET/4]=1;
    ResourceTraceCount = 0;
    Reads = MmioReadCalls; Writes = MmioWriteCalls;
    GuardUfsMmioResourcesReady = TRUE;
    FireEventAtCallbackTpl(ExitNotify,EXIT_EVENT_HANDLE);
    CHECK(mContext.ExitedBootServices && OsCacheCleans>=1 && !SmcCalls && !UicCalls && !ResetWrites);
    CHECK(OsHandoff[1] == UFS_BASE && OsHandoff[2] == 2 && OsHandoff[3] == 1);
    if (I == 3 || I == 4) {
      CHECK(ResourceTraceCount == 0);
      CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes && EFI_ERROR(mContext.QuiesceStatus));
      CHECK(UfsMmioReadsWithResourcesOff == 0);
      if (I == 3) {
        for (UINTN C = 0; C < 5; C++) CHECK(!ClockState[C]);
        CHECK(VemcEnabled);
      } else {
        for (UINTN C = 0; C < 5; C++) CHECK(ClockState[C]);
        CHECK(!VemcEnabled);
      }
    }
    if(I==0) {
      CHECK(OsPublications == 1 && OsHandoff[0] == 1 && HceValue == 1 && mContext.QuiesceStatus == EFI_SUCCESS);
      FireEventAtCallbackTpl(ExitNotify,EXIT_EVENT_HANDLE);
      CHECK(OsHandoff[0] == 0 && OsPublications == 1 && HceValue == 1);
    } else {
      CHECK(!OsPublications && !OsHandoff[0]);
      /* With a missing clock/rail, even HCE is inaccessible. The early
       * resource guard must preserve it instead of attempting HCE-off. */
      CHECK(HceValue == ((I == 3 || I == 4) ? 1U : 0U));
    }
  }
  free(Tree); free(Stock);
}

static VOID SetColdControllerStoppedRoots(VOID)
{
  HceValue = 0U;
  TransferDoorbellValue = 0U;
  TaskDoorbellValue = 0U;
  ColdRegs[0x60U / 4U] = 0U;
  ColdRegs[0x80U / 4U] = 0U;
  ColdRegs[0x50U / 4U] = 0U;
  ColdRegs[0x54U / 4U] = 0U;
  ColdRegs[0x70U / 4U] = 0U;
  ColdRegs[0x74U / 4U] = 0U;
  ColdRegs[0x24U / 4U] = 0U;
}

static VOID CheckColdHost(CONST CHAR8 *StockPath)
{
  VOID *Stock, *Mutable;
  FILE *Input = fopen(StockPath, "rb");
  long Length;
  UINTN I;
  EDKII_UFS_HC_DRIVER_INTERFACE Interface;
  CHECK(Input != NULL && fseek(Input, 0, SEEK_END) == 0);
  Length = ftell(Input); CHECK(Length > 0 && Length < 0x200000 && fseek(Input, 0, SEEK_SET) == 0);
  Stock = malloc(Length); Mutable = malloc(Length + 4096);
  CHECK(Stock != NULL && Mutable != NULL && fread(Stock, 1, Length, Input) == (UINTN)Length);
  CHECK(fclose(Input) == 0);
  /* TOP_UFS is a factor with a possibly gated parent, not an always-on
   * fixture. Exercise the cold acquisition, error and shared-retention paths. */
  for (UINTN Case = 0; Case < 7; Case++) {
    MTK_UFS_COLD_HOST Host;
    UINTN Trace;
    ResetHarness(); ColdResources = TRUE;
    SetColdControllerStoppedRoots();
    for (I = 0; I < ARRAY_SIZE(ClockState); I++) ClockState[I] = FALSE;
    if (Case == 6) ClockState[5] = TRUE;
    VemcEnabled = FALSE;
    GuardUfsMmioResourcesReady = TRUE;
    ZeroMem(&Host, sizeof(Host));
    CHECK(MtkUfsValidateRetainedHost(Stock, &Host) == EFI_NOT_READY);
    CHECK(ResourceTraceCount == 0 && MmioReadCalls == 0 && MmioWriteCalls == 0);
    if (Case == 1) ClockEnableNoWriteId = 5;
    if (Case == 2) ClockSetFailId = 5;
    if (Case == 3) ClockEnableNoEffectId = 5;
    if (Case == 4) ClockEnableReadFailId = 5;
    if (Case == 5) ClockSetFailId = 4;
    if (Case >= 1 && Case <= 5) {
      CHECK(MtkUfsConfigureColdHost(Stock, &Host) == EFI_DEVICE_ERROR);
      CHECK(!Host.Configured && !Host.CleanupPending && !Host.ClockOwned[5]);
      CHECK(Host.TopClockEnableRequested && Host.VccRetained && VemcEnabled);
      CHECK(ClockState[5] == (Case == 2 || Case == 4 || Case == 5));
      for (I = 0; I < 5; I++) CHECK(!ClockState[I] && !Host.ClockOwned[I]);
      if (Case <= 4) CHECK(ResourceTraceCount == 2 && MmioReadCalls == 0);
      CHECK(UfsMmioReadsWithResourcesOff == 0 && MmioWriteCalls == 0);
      ClockEnableNoWriteId = ClockSetFailId = MAX_UINT32;
      ClockEnableNoEffectId = ClockEnableReadFailId = MAX_UINT32;
    }
    Trace = ResourceTraceCount;
    CHECK(MtkUfsConfigureColdHost(Stock, &Host) == EFI_SUCCESS);
    CHECK(Host.Configured && ClockState[5] && !Host.ClockOwned[5]);
    CHECK(Host.TopClockEnableRequested == (Case != 6));
    CHECK(ResourceTraceCount >= Trace + 5);
    if (Case == 0) {
      CHECK(ResourceTraceCount == 7);
      CHECK(ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
      CHECK(ResourceTrace[1].Kind == 'C' && ResourceTrace[1].Id == 5 && ResourceTrace[1].Enable);
      CHECK(FirstUfsMmioResourceTraceCount == 7);
    }
    for (I = 0; I < 5; I++) CHECK(ClockState[I] && Host.ClockOwned[I]);
    CHECK(UfsMmioReadsWithResourcesOff == 0 && MmioWriteCalls == 0 && SmcCalls == 0);
    Trace = ResourceTraceCount;
    CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == EFI_SUCCESS);
    CHECK(!Host.Configured && !Host.CleanupPending && ResourceTraceCount == Trace + 5);
    CHECK(ClockState[5] && !Host.ClockOwned[5] && Host.VccRetained && VemcEnabled);
    CHECK(Host.TopClockEnableRequested == (Case != 6));
    for (I = 0; I < 5; I++) CHECK(!ClockState[I] && !Host.ClockOwned[I]);
    /* Reconfiguration must preserve history and retain the shared parent. */
    Trace = ResourceTraceCount;
    CHECK(MtkUfsConfigureColdHost(Stock, &Host) == EFI_SUCCESS);
    CHECK(ResourceTraceCount == Trace + 5 && Host.TopClockEnableRequested == (Case != 6));
    CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == EFI_SUCCESS);
    CHECK(UfsMmioReadsWithResourcesOff == 0 && ClockState[5]);
    for (I = 0; I < ResourceTraceCount; I++)
      if (ResourceTrace[I].Kind == 'C' && ResourceTrace[I].Id == 5) CHECK(ResourceTrace[I].Enable);
    CheckVemcRetained();
  }
  {
    MTK_UFS_COLD_HOST Probe;
    ZeroMem (&Probe, sizeof (Probe));
    ResetHarness(); ColdResources = TRUE;
    for (I = 0; I < 5; I++) ClockState[I] = FALSE;
    VemcEnabled = FALSE;
    CHECK(MtkUfsValidateRetainedHost(Stock, &Probe) == EFI_NOT_READY);
    CHECK(!Probe.Configured && !Probe.VccOwned && ResourceTraceCount == 0 && !VemcEnabled);
    for (I = 0; I < 6; I++) CHECK(!Probe.ClockOwned[I]);
    for (I = 0; I < 5; I++) CHECK(!ClockState[I]);
    CHECK(MmioWriteCalls == 0 && SmcCalls == 0 && UicCalls == 0 && ResetWrites == 0);

    ResetHarness(); ColdResources = TRUE;
    VemcEnabled = FALSE;
    CHECK(MtkUfsValidateRetainedHost(Stock, &Probe) == EFI_NOT_READY);
    CHECK(!Probe.Configured && !Probe.VccOwned && ResourceTraceCount == 0 && !VemcEnabled);
    for (I = 0; I < 6; I++) CHECK(!Probe.ClockOwned[I]);
    for (I = 0; I < 5; I++) CHECK(ClockState[I]);
    CHECK(MmioWriteCalls == 0 && SmcCalls == 0 && UicCalls == 0 && ResetWrites == 0);

    ResetHarness(); ColdResources = TRUE;
    CHECK(MtkUfsValidateRetainedHost(Stock, &Probe) == EFI_SUCCESS && Probe.Configured);
    CHECK(ResourceTraceCount == 0 && VemcEnabled && !Probe.VccOwned);
    for (I = 0; I < 6; I++) CHECK(!Probe.ClockOwned[I]);
    for (I = 0; I < 5; I++) CHECK(ClockState[I]);
    CHECK(MmioWriteCalls == 0 && SmcCalls == 0 && UicCalls == 0 && ResetWrites == 0);
    /* A pure retained probe owns nothing and is still Idle: a later Start
     * may retry preparation without discarding any acquired resource. */
    CHECK(MtkUfsConfigureColdHost(Stock, &Probe) == EFI_SUCCESS && Probe.Configured);
    CHECK(!Probe.VccOwned && !Probe.CleanupPending && ResourceTraceCount == 0);
    for (I = 0; I < 6; I++) CHECK(!Probe.ClockOwned[I]);
  }
  for (I = 0; I < 22; I++) {
    EFI_STATUS Status;
    MTK_UFS_COLD_HOST Host;
    INT32 Node;
    ResetHarness(); ColdResources = TRUE;
    ZeroMem (&Host, sizeof (Host));
    CHECK(fdt_open_into(Stock, Mutable, Length + 4096) == 0);
    Node = fdt_path_offset(Mutable, "/soc/ufshci@11270000"); CHECK(Node >= 0);
    switch (I) {
      case 0: break;
      case 1: ColdResources = FALSE; break;
      case 2: for (UINTN C = 0; C < 5; C++) ClockState[C] = FALSE; break;
      case 3: VemcEnabled = FALSE; break;
      case 4: VemcVoltage = 2800000; break;
      case 5: ColdFrequency = 0; break;
      case 6: ColdRegs[0x2240 / 4] = 0x10480000; break;
      case 7: CHECK(fdt_setprop_u32(Mutable, Node, "id", 1) == 0); break;
      case 8: CHECK(fdt_setprop_u32(Mutable, Node, "phys", 1) == 0); break;
      case 9: CHECK(fdt_setprop_u32(Mutable, Node, "resets", 1) == 0); break;
      case 10: CHECK(fdt_setprop_string(Mutable, Node, "clock-names", "wrong") == 0); break;
      case 11: ColdClockStatus = EFI_DEVICE_ERROR; break;
      case 12: ColdPmicStatus = EFI_DEVICE_ERROR; break;
      case 13: ColdRegs[0x2240 / 4] = 0x00480000; break;
      case 14: ColdRegs[0x2240 / 4] = 0x01480000; break;
      case 15:
        SetColdControllerStoppedRoots();
        for (UINTN C = 0; C < 5; C++) ClockState[C] = FALSE;
        VemcEnabled = FALSE; ClockSetFailId = 3; break;
      case 16:
        SetColdControllerStoppedRoots();
        for (UINTN C = 0; C < 5; C++) ClockState[C] = FALSE;
        VemcEnabled = FALSE; PmicSetFailAfterWrite = TRUE; break;
      case 17:
        for (UINTN C = 0; C < 5; C++) ClockState[C] = FALSE;
        VemcEnabled = FALSE; break;
      case 18: case 19: case 20:
        for (UINTN C = 0; C < 5; C++) ClockState[C] = FALSE;
        VemcEnabled = FALSE;
        VemcVoltage = I == 18 ? 0 : I == 19 ? 2800000 : 3400000;
        break;
      case 21:
        SetColdControllerStoppedRoots();
        for (UINTN C = 0; C < 5; C++) ClockState[C] = FALSE;
        VemcEnabled = FALSE;
        PmicSetFailAfterWrite = TRUE;
        PmicDisableNoWrite = TRUE;
        break;
    }
    RuntimeTree = Mutable;
    Status = MtkUfsConfigureColdHost(Mutable, &Host);
    if (I == 21) {
      CHECK(Status == EFI_DEVICE_ERROR && !Host.Configured && !Host.CleanupPending &&
            Host.CleanupMode == MtkUfsColdCleanupNone && Host.VccOwned && Host.VccRetained);
      CHECK(VemcEnabled && MmioReadCalls == 0 && MmioWriteCalls == 0 && SmcCalls == 0);
      CHECK(ResourceTraceCount == 1U && ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
      for (UINTN C = 0; C < 5; C++) CHECK(!ClockState[C] && !Host.ClockOwned[C]);
      CheckVemcRetained();
    } else if (I == 15) {
      CHECK(Status == EFI_DEVICE_ERROR && !Host.Configured && !Host.CleanupPending &&
            Host.CleanupMode == MtkUfsColdCleanupNone && Host.VccOwned && Host.VccRetained);
    } else {
      CHECK((I == 0 || I == 2 || I == 3 || I == 17) ?
            Status == EFI_SUCCESS && Host.Configured : EFI_ERROR(Status) && !Host.Configured);
    }
    CHECK(MmioWriteCalls == 0 && SmcCalls == 0);
    if (I == 2 || I == 3 || I == 17) {
      for (UINTN C = 0; C < 5; C++) CHECK(ClockState[C]);
      CHECK(VemcEnabled);
    }
    if (I == 15) {
      CHECK(ResourceTraceCount == 11U && VemcEnabled && Host.VccOwned && Host.VccRetained);
      CHECK(MmioReadCalls > 0 && MmioWriteCalls == 0);
      CHECK(ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
      for (UINTN C = 0; C < 4; C++)
        CHECK(ResourceTrace[1U + C].Kind == 'C' && ResourceTrace[1U + C].Id == C && ResourceTrace[1U + C].Enable);
      CHECK(ResourceTrace[5].Kind == 'C' && ResourceTrace[5].Id == 4U && ResourceTrace[5].Enable);
      for (UINTN C = 0; C < 5; C++) {
        CHECK(ResourceTrace[6U + C].Kind == 'C' && ResourceTrace[6U + C].Id == 4U - C && !ResourceTrace[6U + C].Enable);
        CHECK(!ClockState[C] && !Host.ClockOwned[C]);
      }
      CheckVemcRetained();
    }
    if (I == 16) {
      CHECK(ResourceTraceCount == 1U && MmioReadCalls == 0 && MmioWriteCalls == 0);
      CHECK(VemcEnabled && Host.VccOwned && Host.VccRetained && !Host.Configured && !Host.CleanupPending);
      CHECK(ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
      for (UINTN C = 0; C < 5; C++) CHECK(!ClockState[C] && !Host.ClockOwned[C]);
      CheckVemcRetained();
    }
    if (I >= 18 && I <= 20) {
      CHECK(Status == EFI_UNSUPPORTED && ResourceTraceCount == 0 && !VemcEnabled);
      for (UINTN C = 0; C < 5; C++) CHECK(!ClockState[C] && !Host.ClockOwned[C]);
      CHECK(!Host.VccOwned && !Host.Configured);
    }
    if (I == 17) {
      CHECK(ResourceTraceCount == 6);
      CHECK(ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
      for (UINTN C = 0; C < 5; C++) CHECK(ResourceTrace[1U + C].Kind == 'C' && ResourceTrace[1U + C].Id == C && ResourceTrace[1U + C].Enable);
      CHECK(Host.VccRampDelayUs == 60 && Host.VccOwned);
      for (UINTN C = 0; C < 5; C++) CHECK(Host.ClockOwned[C]);
    }
  }
  ResetHarness(); ColdResources = TRUE; HceValue = 0;
  for (I = 0; I < 5; I++) ClockState[I] = FALSE;
  VemcEnabled = FALSE;
  GuardUfsMmioResourcesReady = TRUE;
  PublishRuntimeTree(Stock);
  CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK(mContext.Started && mContext.ColdStart && !mContext.Platform.SkipHceReenable && !mContext.Platform.SkipLinkStartup);
  CHECK(MmioReadCalls == 6 && FirstUfsMmioResourceTraceCount == 6 && UfsMmioReadsWithResourcesOff == 0);
  CHECK(ResourceTraceCount == 6);
  CHECK(MmioWriteCalls == 0 && SmcCalls == 0);
  Interface.UfsHcProtocol = &mContext.HostController; Interface.UfsExecUicCommand = ColdUic;
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformHostPrepared && ResetWrites == 6 && SmcCalls == 4);
  CHECK(mContext.ColdHost.Interface == NULL);
  {
    UINT32 Value = 1;
    CHECK(MtkUfsWriteMmio(&mContext.HostController, EfiUfsHcWidthUint32, UFS_HC_ENABLE_OFFSET, 1, &Value) == EFI_SUCCESS);
  }
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPostHce, &Interface) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreLinkStartup, &Interface) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPostLinkStartup, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady && UicCalls == 10);
  CheckManufacturerReadOnlyPath();
  TransferDoorbellValue = 1;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_NOT_READY && !mContext.ColdHost.Suspended);
  TransferDoorbellValue = 0;
  ClockDisableFailId = 2;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_DEVICE_ERROR);
  CHECK(!mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady);
  CHECK(mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOn && Attributes[0xD0A8] == 0 && VemcEnabled);
  for (I = 0; I < 5; I++) CHECK(ClockState[I]);
  ClockDisableFailId = MAX_UINT32;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended);
  CHECK(mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOff && ColdRegs[0x144 / 4] == 0);
  CHECK(Attributes[0xD0A8] == 1 && VemcEnabled);
  for (I = 0; I < 5; I++) CHECK(!ClockState[I]);
  ClockSetFailId = 2;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_DEVICE_ERROR);
  CHECK(mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended);
  CHECK(mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOff && Attributes[0xD0A8] == 1);
  for (I = 0; I < 5; I++) CHECK(!ClockState[I]);
  ClockSetFailId = MAX_UINT32;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(!mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady);
  CHECK(mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOn && ColdRegs[0x144 / 4] == 3);
  CHECK(Attributes[0xD0A8] == 0 && VemcEnabled && HibernateCalls == 4);
  for (I = 0; I < 5; I++) CHECK(ClockState[I]);
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, NULL) == EFI_INVALID_PARAMETER);
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, NULL) == EFI_INVALID_PARAMETER);
  {
    UINTN TraceCount = ResourceTraceCount;
    CHECK(MtkUfsDriverStop(&mDriverBinding, SEED_HANDLE, 0, NULL) == EFI_NOT_READY);
    CHECK(mContext.Started && UfsProtocolsInstalled && MtkUfsColdResourcesOwned());
    CHECK(ResourceTraceCount == TraceCount && mContext.ColdHost.Configured);
    CHECK(mContext.ColdHost.VccOwned);
    for (I = 0; I < 5; I++) CHECK(mContext.ColdHost.ClockOwned[I]);
  }
  for (I = 0; I < 3; I++) {
    ResetHarness(); ColdResources = TRUE; HceValue = 0; PublishRuntimeTree(Stock);
    CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS && mContext.ColdStart);
    Interface.UfsHcProtocol = &mContext.HostController;
    if (I == 0) {
      SmcError = -1;
      CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_DEVICE_ERROR);
    } else {
      CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_SUCCESS);
      HceValue = 1;
      CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPostHce, &Interface) == EFI_SUCCESS);
      if (I == 1) UicStatus = EFI_TIMEOUT; else UicResult = 1;
      CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreLinkStartup, &Interface) == (I == 1 ? EFI_TIMEOUT : EFI_DEVICE_ERROR));
    }
    CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformFault && mContext.ColdHost.Interface == NULL);
  }
  ResetHarness(); ColdResources = TRUE; PublishRuntimeTree(Stock);
  CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS && mContext.Started && !mContext.ColdStart);
  Interface.UfsHcProtocol = &mContext.HostController;
  for (I = EdkiiUfsHcPreHce; I <= EdkiiUfsHcPostLinkStartup; I++) CHECK(mContext.Platform.Callback(SEED_HANDLE, I, &Interface) == EFI_SUCCESS);
  CHECK(MmioWriteCalls == 0 && SmcCalls == 0 && UicCalls == 0);
  CHECK(mContext.Platform.Callback(IMAGE_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_INVALID_PARAMETER);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, NULL) == EFI_INVALID_PARAMETER);
  free(Stock); free(Mutable);
}

static VOID StartColdUfsLink(CONST VOID *Tree, UINT32 PreEnabledClockMask,
                             EDKII_UFS_HC_DRIVER_INTERFACE *Interface)
{
  UINTN I;
  UINT32 Value = 1;
  ResetHarness();
  ColdResources = TRUE;
  HceValue = 0;
  for (I = 0; I < 5; I++) ClockState[I] = (BOOLEAN)((PreEnabledClockMask & (1U << I)) != 0);
  VemcEnabled = FALSE;
  GuardUfsMmioResourcesReady = TRUE;
  PublishRuntimeTree(Tree);
  CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS &&
        mContext.Started && mContext.ColdStart);
  ZeroMem(Interface, sizeof(*Interface));
  Interface->UfsHcProtocol = &mContext.HostController;
  Interface->UfsExecUicCommand = ColdUic;
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, Interface) == EFI_SUCCESS);
  CHECK(MtkUfsWriteMmio(&mContext.HostController, EfiUfsHcWidthUint32,
                        UFS_HC_ENABLE_OFFSET, 1, &Value) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPostHce, Interface) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreLinkStartup, Interface) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPostLinkStartup, Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady);
  CHECK(MtkUfsColdHostSetManufacturer(&mContext.ColdHost, 0x01CEU) == EFI_SUCCESS);
}

static VOID StopColdRefClockForRestart(VOID)
{
  MTK_UFS_PLATFORM *Platform = &mContext.ColdHost.Platform;
  MTK_UFS_PLATFORM_STATE SavedState = Platform->State;

  CHECK(Platform->RefClock == MtkUfsRefClockOn || Platform->RefClock == MtkUfsRefClockUnknown ||
        Platform->RefClock == MtkUfsRefClockOff);
  HceValue = 0U;
  TransferDoorbellValue = 0U;
  TaskDoorbellValue = 0U;
  /* SetRefClock is an Idle-state API. Model the already-stopped controller
   * for this transition, then retain LinkReady/Fault so RestartStopped tests
   * its documented non-Idle plus confirmed-Off admission path. */
  Platform->State = MtkUfsPlatformIdle;
  CHECK(MtkUfsPlatformSetRefClock(Platform, 0) == MtkUfsPlatformOk);
  CHECK(Platform->RefClock == MtkUfsRefClockOff &&
        (ColdRegs[0x144U / 4U] & 3U) == 0U);
  Platform->State = SavedState;
}

static VOID SetControllerStopRoot(UINT32 Offset, UINT32 Value)
{
  if (Offset == UFS_HC_ENABLE_OFFSET) HceValue = Value;
  else if (Offset == UFS_HC_UTRLDBR_OFFSET) TransferDoorbellValue = Value;
  else if (Offset == UFS_HC_UTMRLDBR_OFFSET) TaskDoorbellValue = Value;
  else ColdRegs[Offset / sizeof(UINT32)] = Value;
}

static VOID CheckUnstartedReleaseRequiresAllRoots(CONST CHAR8 *StockPath)
{
  static CONST UINT32 Roots[] = {
    UFS_HC_ENABLE_OFFSET, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTMRLRSR_OFFSET,
    UFS_HC_UTRLDBR_OFFSET, UFS_HC_UTMRLDBR_OFFSET, UFS_HC_UTRLBA_OFFSET,
    UFS_HC_UTRLBAU_OFFSET, UFS_HC_UTMRLBA_OFFSET, UFS_HC_UTMRLBAU_OFFSET,
    UFS_HC_IE_OFFSET
  };
  VOID *Tree = LoadStockTree(StockPath);
  UINTN I, Failure;

  /* A successful configuration that never left Idle may release gates only
   * after all ten DMA roots are readable and exactly zero. UINT32_MAX is the
   * fake MMIO bus-error sentinel used by the production proof code. */
  for (I = 0; I < ARRAY_SIZE(Roots); I++) {
    for (Failure = 0; Failure < 2; Failure++) {
      MTK_UFS_COLD_HOST Host;
      UINTN Trace, Writes, Reads, C;
      EFI_STATUS Expected = Failure == 0 ? EFI_NOT_READY : EFI_DEVICE_ERROR;

      ResetHarness();
      ColdResources = TRUE;
      HceValue = 0;
      for (C = 0; C < 5; C++) ClockState[C] = FALSE;
      VemcEnabled = FALSE;
      ZeroMem(&Host, sizeof(Host));
      CHECK(MtkUfsConfigureColdHost(Tree, &Host) == EFI_SUCCESS);
      CHECK(Host.Configured && Host.Platform.State == MtkUfsPlatformIdle &&
            Host.VccOwned && VemcEnabled);
      for (C = 0; C < 5; C++) CHECK(Host.ClockOwned[C] && ClockState[C]);

      SetControllerStopRoot(Roots[I], Failure == 0 ? 1U : MAX_UINT32);
      Trace = ResourceTraceCount;
      Writes = MmioWriteCalls;
      Reads = MmioReadCalls;
      CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == Expected);
      CHECK(ResourceTraceCount == Trace && MmioWriteCalls == Writes && MmioReadCalls > Reads);
      CHECK(Host.Configured && !Host.CleanupPending && Host.Platform.State == MtkUfsPlatformIdle);
      for (C = 0; C < 5; C++) CHECK(Host.ClockOwned[C] && ClockState[C]);
      CHECK(VemcEnabled && Host.VccOwned);

      SetControllerStopRoot(Roots[I], 0U);
      Trace = ResourceTraceCount;
      CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == EFI_SUCCESS);
      CHECK(!Host.Configured && !Host.CleanupPending && ResourceTraceCount == Trace + 5U);
      CHECK(Host.VccOwned && Host.VccRetained && VemcEnabled);
      for (C = 0; C < 5; C++) CHECK(!Host.ClockOwned[C] && !ClockState[C]);
      CheckVemcRetained();
    }
  }

  free(Tree);
}

static VOID CheckPartialEnableRollbackRequiresAllRoots(CONST CHAR8 *StockPath)
{
  static CONST UINT32 Roots[] = {
    UFS_HC_ENABLE_OFFSET, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTMRLRSR_OFFSET,
    UFS_HC_UTRLDBR_OFFSET, UFS_HC_UTMRLDBR_OFFSET, UFS_HC_UTRLBA_OFFSET,
    UFS_HC_UTRLBAU_OFFSET, UFS_HC_UTMRLBA_OFFSET, UFS_HC_UTMRLBAU_OFFSET,
    UFS_HC_IE_OFFSET
  };
  VOID *Tree = LoadStockTree(StockPath);
  UINTN I, Failure;

  /* If the final UFS gate setter reports an error after its write landed,
   * retain all five owned gates while any controller root is occupied or unreadable. */
  for (I = 0; I < ARRAY_SIZE(Roots); I++) {
    for (Failure = 0; Failure < 2; Failure++) {
      MTK_UFS_COLD_HOST Host;
      UINTN Trace, Writes, C;

      ResetHarness();
      ColdResources = TRUE;
      HceValue = 0;
      for (C = 0; C < 5; C++) ClockState[C] = FALSE;
      VemcEnabled = FALSE;
      ClockSetFailId = 4U;
      SetControllerStopRoot(Roots[I], Failure == 0 ? 1U : MAX_UINT32);
      ZeroMem(&Host, sizeof(Host));

      CHECK(EFI_ERROR(MtkUfsConfigureColdHost(Tree, &Host)));
      CHECK(Host.Configured && Host.CleanupPending &&
            Host.CleanupMode == MtkUfsColdCleanupOwnedOnly &&
            Host.VccOwned && Host.VccRetained && VemcEnabled);
      Trace = ResourceTraceCount;
      Writes = MmioWriteCalls;
      CHECK(Trace == 6U && Writes == 0U);
      CHECK(ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
      for (C = 0; C < 5; C++) {
        CHECK(Host.ClockOwned[C] && ClockState[C]);
        CHECK(ResourceTrace[1U + C].Kind == 'C' && ResourceTrace[1U + C].Id == C && ResourceTrace[1U + C].Enable);
      }

      /* Restore the root and retry the saved cleanup proof. Only then may
       * the five owned UFS gates be released; shared VEMC stays on. */
      SetControllerStopRoot(Roots[I], 0U);
      ClockSetFailId = MAX_UINT32;
      CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == EFI_SUCCESS);
      CHECK(!Host.Configured && !Host.CleanupPending && ResourceTraceCount == Trace + 5U);
      CHECK(Host.VccOwned && Host.VccRetained && VemcEnabled);
      for (C = 0; C < 5; C++) CHECK(!Host.ClockOwned[C] && !ClockState[C]);
      CheckVemcRetained();
    }
  }

  free(Tree);
}

static VOID CheckColdStoppedRestart(CONST CHAR8 *StockPath)
{
  static CONST UINT32 Roots[] = { 0x34, 0x60, 0x80, 0x58, 0x78, 0x50, 0x54, 0x70, 0x74, 0x24 };
  VOID *Tree = LoadStockTree(StockPath);
  EDKII_UFS_HC_DRIVER_INTERFACE Interface;
  UINTN I, C, Writes, Smcs, Reads, Trace;
  MTK_UFS_PLATFORM Before;
  BOOLEAN OldOwned[6], OldVcc;
  UINT32 Value = 1;

  for (I = 0; I < ARRAY_SIZE(Roots); I++) {
    StartColdUfsLink(Tree, 1U << 2, &Interface);
    StopColdRefClockForRestart();
    if (Roots[I] == 0x34) HceValue = 1U;
    else if (Roots[I] == 0x58) TransferDoorbellValue = 1;
    else if (Roots[I] == 0x78) TaskDoorbellValue = 1;
    else ColdRegs[Roots[I] / 4] = 0x80000000U;
    Before = mContext.ColdHost.Platform;
    Reads = MmioReadCalls; Writes = MmioWriteCalls; Smcs = SmcCalls; Trace = ResourceTraceCount;
    CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_NOT_READY);
    CHECK(memcmp(&Before, &mContext.ColdHost.Platform, sizeof(Before)) == 0);
    CHECK(MmioReadCalls > Reads && Writes == MmioWriteCalls && Smcs == SmcCalls && Trace == ResourceTraceCount);
    CHECK(mContext.ColdHost.Interface == NULL);
  }

  /* Missing or unprovable power blocks every init callback before any HCI
   * read, reset or refclock operation, and never acquires a borrowed gate. */
  for (I = 0; I < 10; I++) {
    StartColdUfsLink(Tree, 1U << 2, &Interface);
    HceValue = 0;
    if (I < 5) ClockState[I] = FALSE;
    else if (I == 5) VemcEnabled = FALSE;
    else if (I == 6) VemcVoltage = 2800000;
    else if (I == 7) ColdFrequency = 0;
    else if (I == 8) ColdPmicStatus = EFI_DEVICE_ERROR;
    else ColdClockStatus = EFI_DEVICE_ERROR;
    Reads = MmioReadCalls; Writes = MmioWriteCalls; Smcs = SmcCalls; Trace = ResourceTraceCount;
    CHECK(EFI_ERROR(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface)));
    CHECK(Reads == MmioReadCalls && Writes == MmioWriteCalls && Smcs == SmcCalls && Trace == ResourceTraceCount);
    CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady && mContext.ColdHost.Interface == NULL);
  }

  StartColdUfsLink(Tree, 1U << 2, &Interface);
  StopColdRefClockForRestart();
  CopyMem(OldOwned, mContext.ColdHost.ClockOwned, sizeof(OldOwned)); OldVcc = mContext.ColdHost.VccOwned;
  Trace = ResourceTraceCount; Smcs = SmcCalls; Writes = ResetWrites;
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformHostPrepared);
  CHECK(mContext.ColdHost.Platform.Config.RefClockGatingWaitUs == 1 && mContext.ColdHost.RefClockWaitConfigured);
  CHECK(SmcCalls == Smcs + 4 && ResetWrites == Writes + 6 && ResourceTraceCount == Trace);
  CHECK(memcmp(OldOwned, mContext.ColdHost.ClockOwned, sizeof(OldOwned)) == 0 && OldVcc == mContext.ColdHost.VccOwned);
  CHECK(!mContext.ColdHost.ClockOwned[2] && ClockState[2] && VemcEnabled);
  CHECK(MtkUfsWriteMmio(&mContext.HostController, EfiUfsHcWidthUint32, UFS_HC_ENABLE_OFFSET, 1, &Value) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPostHce, &Interface) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreLinkStartup, &Interface) == EFI_SUCCESS);
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPostLinkStartup, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady && mContext.ColdHost.Interface == NULL);

  StartColdUfsLink(Tree, 0, &Interface);
  StopColdRefClockForRestart();
  SmcError = -3;
  Trace = ResourceTraceCount;
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_DEVICE_ERROR);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformFault && mContext.ColdHost.Interface == NULL);
  CHECK(ResourceTraceCount == Trace && VemcEnabled);
  for (C = 0; C < 5; C++) CHECK(ClockState[C] && mContext.ColdHost.ClockOwned[C]);
  SmcError = 0;
  StopColdRefClockForRestart();
  CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformHostPrepared && ResourceTraceCount == Trace);

  /* A cold restart is not an escape from an unfinished sleep transaction. */
  for (I = 0; I < 5; I++) {
    StartColdUfsLink(Tree, 0, &Interface); HceValue = 0;
    if (I == 0) mContext.ColdHost.Suspended = TRUE;
    else if (I == 1) mContext.ColdHost.CleanupPending = TRUE;
    else if (I == 2) mContext.ColdHost.CleanupVccOffConfirmed = TRUE;
    else if (I == 3) mContext.ColdHost.Platform.SramResume = MtkUfsSramPowerOnRequired;
    else mContext.ColdHost.Platform.SramResume = MtkUfsSramRetained;
    Reads = MmioReadCalls; Writes = MmioWriteCalls; Smcs = SmcCalls; Trace = ResourceTraceCount;
    CHECK(mContext.Platform.Callback(SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_NOT_READY);
    CHECK(Reads == MmioReadCalls && Writes == MmioWriteCalls && Smcs == SmcCalls && Trace == ResourceTraceCount);
    CHECK(mContext.ColdHost.Interface == NULL && VemcEnabled);
  }
  free(Tree);
}

static VOID CheckColdSuspendResourceChecks(CONST CHAR8 *StockPath)
{
  VOID *Tree = LoadStockTree(StockPath);
  EDKII_UFS_HC_DRIVER_INTERFACE Interface;
  UINTN Reads, Writes, Smcs, Uics, Hibernates;

  StartColdUfsLink(Tree, 0, &Interface);
  Reads = MmioReadCalls; Writes = MmioWriteCalls; Smcs = SmcCalls;
  Uics = UicCalls; Hibernates = HibernateCalls;
  VemcEnabled = FALSE;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_NOT_READY);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes && SmcCalls == Smcs &&
        UicCalls == Uics && HibernateCalls == Hibernates && !mContext.ColdHost.Suspended &&
        mContext.ColdHost.Interface == NULL);
  VemcEnabled = TRUE;

  VemcVoltage = 2800000U;
  Reads = MmioReadCalls; Writes = MmioWriteCalls; Smcs = SmcCalls;
  Uics = UicCalls; Hibernates = HibernateCalls;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_UNSUPPORTED);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes && SmcCalls == Smcs &&
        UicCalls == Uics && HibernateCalls == Hibernates && !mContext.ColdHost.Suspended &&
        mContext.ColdHost.Interface == NULL);
  VemcVoltage = 3000000U;

  ClockState[2] = FALSE;
  Reads = MmioReadCalls; Writes = MmioWriteCalls; Smcs = SmcCalls;
  Uics = UicCalls; Hibernates = HibernateCalls;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_NOT_READY);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes && SmcCalls == Smcs &&
        UicCalls == Uics && HibernateCalls == Hibernates && !mContext.ColdHost.Suspended &&
        mContext.ColdHost.Interface == NULL);
  ClockState[2] = TRUE;

  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended);
  VemcEnabled = FALSE;
  Reads = MmioReadCalls; Writes = MmioWriteCalls;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_NOT_READY);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes &&
        mContext.ColdHost.Suspended && mContext.ColdHost.Interface == NULL &&
        mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended);
  for (UINTN I = 0; I < 5; I++) CHECK(!ClockState[I]);
  VemcEnabled = TRUE;

  VemcVoltage = 3400000U;
  Reads = MmioReadCalls; Writes = MmioWriteCalls;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_UNSUPPORTED);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes &&
        mContext.ColdHost.Suspended && mContext.ColdHost.Interface == NULL &&
        mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended);
  for (UINTN I = 0; I < 5; I++) CHECK(!ClockState[I]);
  VemcVoltage = 3000000U;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(!mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady);

  StartColdUfsLink(Tree, 1U << 4, &Interface);
  CHECK(!mContext.ColdHost.ClockOwned[4] && ClockState[4]);
  ClockState[4] = FALSE;
  Reads = MmioReadCalls; Writes = MmioWriteCalls;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_NOT_READY);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes && !mContext.ColdHost.Suspended);
  ClockState[4] = TRUE;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  ClockState[4] = FALSE;
  Reads = MmioReadCalls; Writes = MmioWriteCalls;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_NOT_READY);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == Writes &&
        mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended);
  for (UINTN I = 0; I < 4; I++) CHECK(!ClockState[I]);
  ClockState[4] = TRUE;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(!mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady);

  StartColdUfsLink(Tree, 0, &Interface);
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  UicStatus = EFI_DEVICE_ERROR;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_DEVICE_ERROR);
  CHECK(mContext.ColdHost.Suspended && mContext.ColdHost.Interface == NULL &&
        mContext.ColdHost.Platform.State == MtkUfsPlatformFault);
  {
    UINTN TraceCount = ResourceTraceCount;
    Reads = MmioReadCalls; Writes = MmioWriteCalls;
    UicStatus = EFI_SUCCESS;
    CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_NOT_READY);
    CHECK(ResourceTraceCount == TraceCount && MmioReadCalls == Reads && MmioWriteCalls == Writes);
    CHECK(mContext.ColdHost.Interface == NULL);
    for (UINTN I = 0; I < 5; I++) CHECK(ClockState[I] && mContext.ColdHost.ClockOwned[I]);
  }

  /* A failed suspend followed by failed gate restoration must not touch
   * the now-unclocked HCI; preserve the suspended context for a later resume. */
  StartColdUfsLink(Tree, 0, &Interface);
  ClockDisableFailId = 2;
  ClockEnableNoWriteId = 0;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_DEVICE_ERROR);
  CHECK(mContext.ColdHost.Suspended && mContext.ColdHost.Interface == NULL &&
        mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended &&
        mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOff);
  CHECK(!ClockState[0] && Attributes[0xD0A8] == 1 && HibernateCalls == 1 &&
        UfsMmioReadsWithResourcesOff == 0);
  ClockDisableFailId = ClockEnableNoWriteId = MAX_UINT32;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(!mContext.ColdHost.Suspended && mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady &&
        UfsMmioReadsWithResourcesOff == 0);
  for (UINTN I = 0; I < 5; I++) CHECK(ClockState[I] && mContext.ColdHost.ClockOwned[I]);

  StartColdUfsLink(Tree, 0, &Interface);
  CHECK(mContext.ColdHost.Platform.Config.AllowSramRetention == 1U);
  SmcError = -1;
  Smcs = Smc32OffCalls;
  CHECK(MtkUfsColdHostSuspend(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformLinkSuspended &&
        mContext.ColdHost.Platform.SramResume == MtkUfsSramRetained &&
        Smc32OffCalls == Smcs + 1U);
  Smcs = Smc32OnCalls;
  CHECK(MtkUfsColdHostResume(&mContext.ColdHost, &Interface) == EFI_SUCCESS);
  CHECK(Smc32OnCalls == Smcs + 1U && !mContext.ColdHost.Suspended &&
        mContext.ColdHost.Platform.State == MtkUfsPlatformLinkReady &&
        mContext.ColdHost.Platform.SramResume == MtkUfsSramNoRestore);
  free(Tree);
}

static VOID CheckColdReleaseRetry(CONST CHAR8 *StockPath)
{
  VOID *Tree = LoadStockTree(StockPath);
  MTK_UFS_COLD_HOST Host;
  UINTN Reads, I;
  ZeroMem(&Host, sizeof(Host));
  ResetHarness();
  ColdResources = TRUE;
  HceValue = 0;
  for (I = 0; I < 5; I++) ClockState[I] = FALSE;
  VemcEnabled = FALSE;
  CHECK(MtkUfsConfigureColdHost(Tree, &Host) == EFI_SUCCESS);
  ClockDisableNoWriteId = 2;
  CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == EFI_DEVICE_ERROR);
  CHECK(Host.Configured && Host.CleanupPending &&
        Host.CleanupMode == MtkUfsColdCleanupStoppedHost &&
        !Host.CleanupVccOffConfirmed && Host.VccOwned && Host.VccRetained && VemcEnabled);
  for (I = 0; I < 6; I++) CHECK(Host.ClockOwned[I] == (I <= 2));
  for (I = 0; I < 5; I++) CHECK(ClockState[I] == (I <= 2));
  {
    UINTN TraceCount = ResourceTraceCount;
    CHECK(MtkUfsConfigureColdHost(Tree, &Host) == EFI_NOT_READY);
    CHECK(MtkUfsValidateRetainedHost(Tree, &Host) == EFI_NOT_READY);
    CHECK(Host.Configured && Host.CleanupPending && Host.ClockOwned[2] &&
          ResourceTraceCount == TraceCount);
  }
  Reads = MmioReadCalls;
  {
    UINTN TraceCount = ResourceTraceCount;
    UINTN ReadsBeforeRetry = MmioReadCalls;
    CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == EFI_DEVICE_ERROR);
    CHECK(ResourceTraceCount == TraceCount + 1U && MmioReadCalls == ReadsBeforeRetry &&
          Host.CleanupPending && !Host.CleanupVccOffConfirmed && Host.ClockOwned[2] && ClockState[2] && VemcEnabled);
  }
  ClockDisableNoWriteId = MAX_UINT32;
  CHECK(MtkUfsReleaseUnstartedColdHost(&Host) == EFI_SUCCESS);
  CHECK(MmioReadCalls == Reads && MmioWriteCalls == 0);
  CHECK(!Host.Configured && !Host.CleanupPending && Host.VccOwned && Host.VccRetained && VemcEnabled);
  for (I = 0; I < 6; I++) CHECK(!Host.ClockOwned[I]);
  for (I = 0; I < 5; I++) CHECK(!ClockState[I]);
  CheckVemcRetained();
  free(Tree);
}

static VOID CheckColdConfigureRetry(CONST CHAR8 *StockPath)
{
  VOID *Tree = LoadStockTree(StockPath);
  UINTN I;
  ResetHarness();
  ColdResources = TRUE;
  HceValue = 0;
  for (I = 0; I < 5; I++) ClockState[I] = FALSE;
  /* Keep the shared rail already on so the cleanup proof can distinguish a
   * gate disable that landed with an error from a gate that stayed on. */
  VemcEnabled = TRUE;
  ClockSetFailId = 3;
  ClockDisableNoWriteId = 3;
  GuardUfsMmioResourcesReady = TRUE;
  PublishRuntimeTree(Tree);
  CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK(!mContext.Started && !mEntryQuarantined && mContext.ColdHost.Configured &&
        mContext.ColdHost.CleanupPending &&
        mContext.ColdHost.CleanupMode == MtkUfsColdCleanupStoppedHost);
  CHECK(!mContext.ColdHost.VccOwned && mContext.ColdHost.VccRetained && VemcEnabled);
  for (I = 0; I < 5; I++) {
    CHECK(ClockState[I] == (I <= 3U));
    CHECK(mContext.ColdHost.ClockOwned[I] == (I <= 3U));
  }
  CHECK(MmioReadCalls > 0 && MmioWriteCalls == 0 && UfsMmioReadsWithResourcesOff == 0);

  ClockSetFailId = MAX_UINT32;
  ClockDisableNoWriteId = MAX_UINT32;
  CHECK(MtkUfsDriverStart(&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
  CHECK(mContext.Started && !mEntryQuarantined && !mContext.ColdHost.CleanupPending);
  CHECK(mContext.ColdHost.Configured && MtkUfsColdResourcesOwned() && VemcEnabled);
  CHECK(UfsMmioReadsWithResourcesOff == 0);
  for (I = 0; I < 5; I++) CHECK(ClockState[I] && mContext.ColdHost.ClockOwned[I]);
  free(Tree);
}

static VOID CheckIdleRetainedRetry(CONST CHAR8 *StockPath)
{
  VOID *Tree = LoadStockTree(StockPath);
  ResetHarness(); ColdResources = TRUE; HceValue = 1; HcsValue = 0;
  PublishRuntimeTree(Tree);
  CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK(!mContext.Started && !mEntryQuarantined && mContext.ColdHost.Configured &&
        mContext.ColdHost.Platform.State == MtkUfsPlatformIdle && !MtkUfsColdResourcesOwned());
  CHECK(ResourceTraceCount == 0 && MmioWriteCalls == 0);
  HcsValue = 15;
  FireEventAtCallbackTpl(EndOfDxeNotify, END_EVENT_HANDLE);
  CHECK(mContext.Started && !mEntryQuarantined && UfsProtocolsInstalled && !mContext.ColdStart);
  CHECK(ResourceTraceCount == 0 && MmioWriteCalls == 0);
  free(Tree);
}

static VOID CheckColdBusyStart(CONST CHAR8 *StockPath)
{
  VOID *Tree = LoadStockTree(StockPath);
  UINTN Case, C;
  for (Case = 0; Case < 4; Case++) {
    UINTN TraceCount;
    ResetHarness();
    ColdResources = TRUE;
    for (C = 0; C < 5; C++) ClockState[C] = FALSE;
    VemcEnabled = FALSE;
    HceValue = 0;
    if (Case == 0) HceValue = MTK_UFS_HCE_ENABLED;
    if (Case == 1) TransferDoorbellValue = 1;
    if (Case == 2) TaskDoorbellValue = 1;
    if (Case == 3) HceValue = MAX_UINT32;
    GuardUfsMmioResourcesReady = TRUE;
    PublishRuntimeTree(Tree);
    CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS);
    CHECK(mEntryQuarantined && !mContext.Started && !UfsProtocolsInstalled);
    CHECK(mContext.ColdHost.Configured && MtkUfsColdResourcesOwned());
    CHECK(MmioReadCalls == 6 && FirstUfsMmioResourceTraceCount == 6);
    CHECK(UfsMmioReadsWithResourcesOff == 0 && ResourceTraceCount == 6);
    CHECK(mContext.ColdHost.VccOwned);
    CHECK(ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
    for (C = 0; C < 5; C++) {
      CHECK(ClockState[C] && mContext.ColdHost.ClockOwned[C]);
      CHECK(ResourceTrace[1U + C].Kind == 'C' && ResourceTrace[1U + C].Id == C && ResourceTrace[1U + C].Enable);
    }
    TraceCount = ResourceTraceCount;
    CHECK(MtkUfsDriverStart(&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
    CHECK(ResourceTraceCount == TraceCount && MtkUfsColdResourcesOwned());
  }
  free(Tree);
}

static VOID CheckColdIpFailure(CONST CHAR8 *StockPath)
{
  VOID *Tree = LoadStockTree(StockPath);
  UINTN Case, C;
  for (Case = 0; Case < 5; Case++) {
    ResetHarness(); ColdResources = TRUE;
    for (C = 0; C < 5; C++) ClockState[C] = FALSE;
    VemcEnabled = FALSE; HceValue = 0;
    ColdRegs[0x2240 / 4] = 0x10480000U;
    if (Case == 0) HceValue = 1;
    if (Case == 1) TransferDoorbellValue = 1;
    if (Case == 2) TaskDoorbellValue = 1;
    if (Case == 3) HceValue = MAX_UINT32;
    GuardUfsMmioResourcesReady = TRUE;
    PublishRuntimeTree(Tree);
    CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS);
    CHECK(!mContext.Started && !UfsProtocolsInstalled && MmioWriteCalls == 0 && SmcCalls == 0);
    CHECK(UfsMmioReadsWithResourcesOff == 0 && FirstUfsMmioResourceTraceCount == 6);
    if (Case < 4) {
      CHECK(mEntryQuarantined && mContext.ColdHost.Configured && MtkUfsColdResourcesOwned());
      CHECK(ResourceTraceCount == 6 && VemcEnabled && mContext.ColdHost.VccOwned);
      for (C = 0; C < 5; C++) CHECK(ClockState[C] && mContext.ColdHost.ClockOwned[C]);
      CHECK(MtkUfsDriverStart(&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
    } else {
      CHECK(!mEntryQuarantined && !mContext.ColdHost.Configured && !MtkUfsColdResourcesOwned());
      CHECK(ResourceTraceCount == 11 && VemcEnabled && mContext.ColdHost.VccOwned);
      for (C = 0; C < 5; C++) CHECK(!ClockState[C]);
      CHECK(MmioReadCalls == 11U);
      CheckVemcRetained();
    }
  }
  free(Tree);
}

static VOID CheckColdHostInstallFailure(CONST CHAR8 *StockPath)
{
  FILE *Input = fopen(StockPath, "rb");
  VOID *Stock;
  long Length;
  UINTN Case, C;
  CHECK(Input != NULL && fseek(Input, 0, SEEK_END) == 0);
  Length = ftell(Input);
  CHECK(Length > 0 && Length < 0x200000 && fseek(Input, 0, SEEK_SET) == 0);
  Stock = malloc(Length);
  CHECK(Stock != NULL && fread(Stock, 1, Length, Input) == (UINTN)Length);
  CHECK(fclose(Input) == 0);
  {
    MTK_UFS_COLD_HOST Probe;
    EDKII_UFS_HC_DRIVER_INTERFACE Interface;
    ZeroMem(&Interface, sizeof(Interface));
    ZeroMem (&Probe, sizeof (Probe));
    ResetHarness(); ColdResources = TRUE; VemcEnabled = FALSE; HceValue = 0;
    for (C = 0; C < 5; C++) ClockState[C] = FALSE;
    CHECK(MtkUfsConfigureColdHost(Stock, &Probe) == EFI_SUCCESS);
    CHECK(Probe.Configured && Probe.Platform.State == MtkUfsPlatformIdle && Probe.Interface == NULL);
    CHECK(Probe.VccOwned && ResourceTraceCount == 6);
    for (C = 0; C < 5; C++) CHECK(Probe.ClockOwned[C] && ClockState[C]);
    {
      UINTN TraceCount = ResourceTraceCount;
      HceValue = MTK_UFS_HCE_ENABLED;
      CHECK(MtkUfsReleaseUnstartedColdHost(&Probe) == EFI_NOT_READY);
      CHECK(ResourceTraceCount == TraceCount && Probe.Configured && Probe.VccOwned);
      for (C = 0; C < 5; C++) CHECK(Probe.ClockOwned[C] && ClockState[C]);
      HceValue = 0; TransferDoorbellValue = 1;
      CHECK(MtkUfsReleaseUnstartedColdHost(&Probe) == EFI_NOT_READY);
      CHECK(ResourceTraceCount == TraceCount && Probe.Configured && Probe.VccOwned);
      TransferDoorbellValue = 0; TaskDoorbellValue = 1;
      CHECK(MtkUfsReleaseUnstartedColdHost(&Probe) == EFI_NOT_READY);
      CHECK(ResourceTraceCount == TraceCount && Probe.Configured && Probe.VccOwned);
      TaskDoorbellValue = 0;
    }
    Probe.Platform.State = MtkUfsPlatformHostPrepared;
    CHECK(MtkUfsReleaseUnstartedColdHost(&Probe) == EFI_NOT_READY);
    CHECK(Probe.Configured && Probe.VccOwned && ResourceTraceCount == 6 && VemcEnabled);
    for (C = 0; C < 5; C++) CHECK(Probe.ClockOwned[C] && ClockState[C]);
    Probe.Platform.State = MtkUfsPlatformIdle; Probe.Interface = &Interface;
    CHECK(MtkUfsReleaseUnstartedColdHost(&Probe) == EFI_NOT_READY);
    CHECK(Probe.Configured && ResourceTraceCount == 6 && VemcEnabled);
    Probe.Interface = NULL;
    CHECK(MtkUfsReleaseUnstartedColdHost(&Probe) == EFI_SUCCESS);
    CHECK(!Probe.Configured && Probe.VccOwned && Probe.VccRetained && VemcEnabled && ResourceTraceCount == 11);
    for (C = 0; C < 6; C++) CHECK(!Probe.ClockOwned[C]);
    for (C = 0; C < 5; C++) CHECK(!ClockState[C]);
    CheckVemcRetained();

    ResetHarness(); ColdResources = TRUE;
    CHECK(MtkUfsConfigureColdHost(Stock, &Probe) == EFI_SUCCESS);
    CHECK(Probe.VccOwned && Probe.VccRetained && VemcEnabled && ResourceTraceCount == 0);
    for (C = 0; C < 6; C++) CHECK(!Probe.ClockOwned[C]);
    CHECK(MtkUfsReleaseUnstartedColdHost(&Probe) == EFI_SUCCESS);
    CHECK(!Probe.Configured && VemcEnabled && ResourceTraceCount == 0);
    for (C = 0; C < 5; C++) CHECK(ClockState[C]);
  }

  for (Case = 0; Case < 2; Case++) {
    ResetHarness();
    ColdResources = TRUE; HceValue = 0; VemcEnabled = FALSE;
    for (C = 0; C < 5; C++) ClockState[C] = FALSE;
    if (Case == 1) ClockDisableNoWriteId = 2;
    InstallUfsProtocolsStatus = EFI_DEVICE_ERROR;
    PublishRuntimeTree(Stock);
    CHECK(MtkUfsHandoffEntryPoint(IMAGE_HANDLE, NULL) == EFI_SUCCESS);
    CHECK(UfsProtocolInstallCalls == 1 && !mContext.Started &&
          ResourceTraceCount == (Case == 0 ? 11U : 9U));
    CHECK (mContext.ActiveCallbacks == 0U && mContext.EventStartCallbacks == 0U &&
           mContext.ActiveDmaOperations == 0U);

    /* VEMC is enabled before the gates; a protocol publish failure releases
     * only the owned gates, in reverse order, and keeps shared VEMC on. */
    CHECK(ResourceTrace[0].Kind == 'V' && ResourceTrace[0].Enable);
    for (C = 0; C < 5; C++)
      CHECK(ResourceTrace[1U + C].Kind == 'C' && ResourceTrace[1U + C].Id == C && ResourceTrace[1U + C].Enable);
    for (C = 0; C < (Case == 0 ? 5U : 3U); C++)
      CHECK(ResourceTrace[6U + C].Kind == 'C' && ResourceTrace[6U + C].Id == 4U - C && !ResourceTrace[6U + C].Enable);

    if (Case == 0) {
      CHECK(!mEntryQuarantined && !mContext.ColdStart && !mContext.ColdHost.Configured);
      CHECK(mContext.ColdHost.VccOwned && mContext.ColdHost.VccRetained);
      for (C = 0; C < 6; C++) CHECK(!mContext.ColdHost.ClockOwned[C]);
      for (C = 0; C < 5; C++) CHECK(!ClockState[C]);
      CHECK(VemcEnabled);
      CheckVemcRetained();
    } else {
      UINTN TraceCount = ResourceTraceCount;
      UINTN Reads = MmioReadCalls;
      CHECK(!mEntryQuarantined && mContext.ColdStart && !mContext.Started &&
            mContext.ColdHost.Configured && mContext.ColdHost.CleanupPending &&
            mContext.ColdHost.CleanupMode == MtkUfsColdCleanupStoppedHost);
      CHECK(mContext.ColdHost.Platform.State == MtkUfsPlatformIdle && mContext.ColdHost.Interface == NULL);
      CHECK(mContext.ColdHost.VccOwned && mContext.ColdHost.VccRetained && VemcEnabled);
      CheckVemcRetained();
      for (C = 0; C < 5; C++) {
        CHECK(ClockState[C] == (C <= 2));
        CHECK(mContext.ColdHost.ClockOwned[C] == (C <= 2));
      }
      CHECK(!mContext.ColdHost.ClockOwned[5]);
      CHECK(ResourceTraceCount == TraceCount && ClockState[2]);
      ClockDisableNoWriteId = MAX_UINT32;
      InstallUfsProtocolsStatus = EFI_SUCCESS;
      CHECK(MtkUfsDriverStart(&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
      CHECK(!mEntryQuarantined && mContext.Started && !mContext.ColdHost.CleanupPending);
      CHECK(MtkUfsColdResourcesOwned() && VemcEnabled && UfsMmioReadsWithResourcesOff == 0);
      for (C = 0; C < 5; C++) CHECK(ClockState[C] && mContext.ColdHost.ClockOwned[C]);
      CHECK(MmioReadCalls > Reads);
    }
  }
  free(Stock);
}

static VOID
PrepareControllerStopFixture (
  OUT UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  BOOLEAN                     ColdStart
  )
{
  ResetHarness ();
  mContext.ColdStart = ColdStart;
  mContext.ControllerHandle = SEED_HANDLE;
  mContext.Started = TRUE;
  mContext.HostPublished = TRUE;
  mContext.PlatformPublished = TRUE;
  UfsHostProtocolInstalled = TRUE;
  UfsPlatformProtocolInstalled = TRUE;
  UfsProtocolsInstalled = TRUE;
  mContext.HostController.Read  = MtkUfsReadMmio;
  mContext.HostController.Write = MtkUfsWriteMmio;
  Private->UfsHostController = &mContext.HostController;

  HceValue = UFS_HC_HCE_EN;
  ColdRegs[UFS_HC_IE_OFFSET / sizeof (UINT32)] = 0x00000125U;
  ColdRegs[UFS_HC_UTRLBA_OFFSET / sizeof (UINT32)] = 0x11223344U;
  ColdRegs[UFS_HC_UTRLBAU_OFFSET / sizeof (UINT32)] = 1U;
  ColdRegs[UFS_HC_UTRLRSR_OFFSET / sizeof (UINT32)] = UFS_HC_UTRLRSR;
  TransferDoorbellValue = 0U;
  ColdRegs[UFS_HC_UTMRLBA_OFFSET / sizeof (UINT32)] = 0x55667788U;
  ColdRegs[UFS_HC_UTMRLBAU_OFFSET / sizeof (UINT32)] = 2U;
  TaskDoorbellValue = 0U;
  ColdRegs[UFS_HC_UTMRLRSR_OFFSET / sizeof (UINT32)] = UFS_HC_UTMRLRSR;
}

static VOID
CheckControllerStopRootsAreZero (
  VOID
  )
{
  CHECK (HceValue == 0U);
  CHECK (ColdRegs[UFS_HC_IE_OFFSET / sizeof (UINT32)] == 0U);
  CHECK (ColdRegs[UFS_HC_UTRLBA_OFFSET / sizeof (UINT32)] == 0U);
  CHECK (ColdRegs[UFS_HC_UTRLBAU_OFFSET / sizeof (UINT32)] == 0U);
  CHECK (TransferDoorbellValue == 0U);
  CHECK (ColdRegs[UFS_HC_UTRLRSR_OFFSET / sizeof (UINT32)] == 0U);
  CHECK (ColdRegs[UFS_HC_UTMRLBA_OFFSET / sizeof (UINT32)] == 0U);
  CHECK (ColdRegs[UFS_HC_UTMRLBAU_OFFSET / sizeof (UINT32)] == 0U);
  CHECK (TaskDoorbellValue == 0U);
  CHECK (ColdRegs[UFS_HC_UTMRLRSR_OFFSET / sizeof (UINT32)] == 0U);
}

static VOID
CheckControllerStopUsesHandoffCallbacks (
  VOID
  )
{
  UFS_PASS_THRU_PRIVATE_DATA  Private;
  EFI_STATUS                 Status;
  UINTN                      Mode;

  for (Mode = 0U; Mode < 2U; Mode++) {
    ZeroMem (&Private, sizeof (Private));
    PrepareControllerStopFixture (&Private, (BOOLEAN)(Mode == 0U));
    Status = UfsControllerStop (&Private);
    CHECK (Status == EFI_SUCCESS && Private.ControllerStopComplete);
    CHECK (MmioWriteCalls == 8U);
    CheckControllerStopRootsAreZero ();
  }

  // Cold mode may enable HCE explicitly, while interrupt delivery remains
  // disabled for the polled UEFI path.
  ZeroMem (&Private, sizeof (Private));
  PrepareControllerStopFixture (&Private, TRUE);
  HceValue = 0U;
  {
    UINT32  Value;
    UINTN   Writes;

    Value = UFS_HC_HCE_EN;
    CHECK (MtkUfsWriteMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                            UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_SUCCESS);
    CHECK (HceValue == UFS_HC_HCE_EN);
    Value = 1U;
    Writes = MmioWriteCalls;
    CHECK (MtkUfsWriteMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                            UFS_HC_IE_OFFSET, 1U, &Value) == EFI_UNSUPPORTED);
    CHECK (MmioWriteCalls == Writes &&
           ColdRegs[UFS_HC_IE_OFFSET / sizeof (UINT32)] == 0x00000125U);
  }

  // Retained operation is allowed to clear HCE, but it cannot enable HCE or
  // install a nonzero interrupt mask. Check the actual protocol callbacks.
  ZeroMem (&Private, sizeof (Private));
  PrepareControllerStopFixture (&Private, FALSE);
  HceValue = 0U;
  {
    UINT32  Value;
    UINTN   Reads;
    UINTN   Writes;

    Value = UFS_HC_HCE_EN;
    Reads = MmioReadCalls;
    Writes = MmioWriteCalls;
    CHECK (MtkUfsWriteMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                            UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_UNSUPPORTED);
    CHECK (MmioReadCalls == Reads && MmioWriteCalls == Writes && HceValue == 0U);

    Value = 1U;
    CHECK (MtkUfsWriteMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                            UFS_HC_IE_OFFSET, 1U, &Value) == EFI_UNSUPPORTED);
    CHECK (MmioWriteCalls == Writes &&
           ColdRegs[UFS_HC_IE_OFFSET / sizeof (UINT32)] == 0x00000125U);

    // The first word is allowed, but the adjacent reserved word is not. The
    // callback must validate the full request before touching the first word.
    {
      UINT32  Values[2] = { 0U, 0U };

      CHECK (MtkUfsWriteMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                              UFS_HC_IE_OFFSET, ARRAY_SIZE (Values), Values) == EFI_UNSUPPORTED);
      CHECK (MmioWriteCalls == Writes &&
             ColdRegs[UFS_HC_IE_OFFSET / sizeof (UINT32)] == 0x00000125U);
    }

    {
      UINT32  Values[2] = { 0U, 0U };

      Reads = MmioReadCalls;
      CHECK (MtkUfsReadMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                             UFS_HC_IE_OFFSET, ARRAY_SIZE (Values), Values) == EFI_UNSUPPORTED);
      CHECK (MmioReadCalls == Reads);
    }
  }
}

static VOID
CheckColdStartedStopAndRestart (
  IN CONST CHAR8  *StockPath
  )
{
  VOID                              *Tree;
  EDKII_UFS_HC_DRIVER_INTERFACE     Interface;
  UFS_PASS_THRU_PRIVATE_DATA        Private;
  EFI_STATUS                        Status;
  UINT32                            Value;
  uint64_t                          SmcArgs[8] = { 0 };
  int64_t                           SmcResult;
  UINTN                             Trace;
  UINTN                             Reads;
  UINTN                             Writes;
  UINTN                             Smcs;
  UINTN                             Uics;
  UINTN                             I;

  Tree = LoadStockTree (StockPath);

  /* Start with one borrowed gate. The board must prove link powerdown first,
   * let the actual UFS HCI worker clear all ten DMA roots, then mint the stop
   * token before any provider protocol or owned clock is released. */
  StartColdUfsLink (Tree, 1U << 0, &Interface);
  CHECK (mContext.Platform.Version == EDKII_UFS_HC_PLATFORM_PROTOCOL_STOP_VERSION);
  CHECK (!mContext.ColdHost.ClockOwned[0] && ClockState[0]);
  for (I = 1; I < 5; I++) CHECK (mContext.ColdHost.ClockOwned[I] && ClockState[I]);
  CHECK (mContext.ColdHost.VccOwned && mContext.ColdHost.VccRetained && VemcEnabled);
  CheckVemcRetained ();

  Trace = ResourceTraceCount;
  Smcs = SmcCalls;
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPrepareStop, &Interface) == EFI_SUCCESS);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopPrepared &&
         !mContext.ColdHost.StopProofValid &&
         mContext.ColdHost.Platform.State == MtkUfsPlatformStopPrepared &&
         mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOn && HceValue == 1U);
  CHECK (ResourceTraceCount == Trace && SmcCalls == Smcs && VemcEnabled);
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcHostStopped, &Interface) == EFI_NOT_READY);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopPrepared &&
         mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOn && ResourceTraceCount == Trace);

  ZeroMem (&Private, sizeof (Private));
  Private.UfsHostController = &mContext.HostController;
  Private.UfsHcPlatform = &mContext.Platform;
  Private.UfsHcDriverInterface = Interface;
  CHECK (UfsControllerStop (&Private) == EFI_SUCCESS && Private.ControllerStopComplete);
  CheckControllerStopRootsAreZero ();

  /* A nonzero high address word invalidates the proof and must leave the
   * refclock and every board resource untouched. */
  ColdRegs[UFS_HC_UTMRLBAU_OFFSET / sizeof (UINT32)] = 0x80000000U;
  Trace = ResourceTraceCount;
  Writes = MmioWriteCalls;
  Smcs = SmcCalls;
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcHostStopped, &Interface) == EFI_NOT_READY);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopPrepared &&
         mContext.ColdHost.Platform.State == MtkUfsPlatformStopPrepared &&
         mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOn &&
         ResourceTraceCount == Trace && MmioWriteCalls == Writes && SmcCalls == Smcs);
  ColdRegs[UFS_HC_UTMRLBAU_OFFSET / sizeof (UINT32)] = 0U;

  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcHostStopped, &Interface) == EFI_SUCCESS);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopHceProven &&
         mContext.ColdHost.StopProofValid &&
         mContext.ColdHost.StopProofGeneration == mContext.ColdHost.StopGeneration &&
         mContext.ColdHost.Platform.State == MtkUfsPlatformStopped &&
         mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOff &&
         ColdRegs[0x144U / sizeof (UINT32)] == 0U);
  CHECK (Smc32OffCalls == 0U && VemcEnabled && ResourceTraceCount == Trace);
  CheckControllerStopRootsAreZero ();

  /* A partially applied protocol uninstall cannot release power or allow a
   * callback to re-enter the host. The remaining protocol and stop token stay
   * owned for a later retry. */
  UninstallUfsProtocolsStatus = EFI_ACCESS_DENIED;
  PartialUfsUninstallMask = 1U;
  Trace = ResourceTraceCount;
  Reads = MmioReadCalls;
  Writes = MmioWriteCalls;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_ACCESS_DENIED);
  CHECK (!UfsHostProtocolInstalled && UfsPlatformProtocolInstalled &&
         !mContext.HostPublished && mContext.PlatformPublished &&
         mContext.Started && mContext.ControllerHandle == SEED_HANDLE &&
         mContext.CallbackClosing && mContext.DmaStopping &&
         mContext.ColdHost.StopPhase == MtkUfsColdStopHceProven &&
         mContext.ColdHost.StopProofValid && ResourceTraceCount == Trace);
  Value = 0U;
  CHECK (mContext.HostController.Read (&mContext.HostController, EfiUfsHcWidthUint32,
                                        UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_NOT_READY);
  CHECK (mContext.HostController.Write (&mContext.HostController, EfiUfsHcWidthUint32,
                                         UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_NOT_READY);
  CHECK (MmioReadCalls == Reads && MmioWriteCalls == Writes && ResourceTraceCount == Trace);

  /* The retry removes the remaining interface, then performs a fresh root
   * proof before gate writes. A corrupted root keeps the provider retryable. */
  UninstallUfsProtocolsStatus = EFI_SUCCESS;
  PartialUfsUninstallMask = 0U;
  ColdRegs[UFS_HC_UTMRLBAU_OFFSET / sizeof (UINT32)] = 0x80000000U;
  Trace = ResourceTraceCount;
  Writes = MmioWriteCalls;
  Reads = MmioReadCalls;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_NOT_READY);
  CHECK (!UfsHostProtocolInstalled && !UfsPlatformProtocolInstalled &&
         mContext.Started && mContext.ControllerHandle == SEED_HANDLE &&
         mContext.ColdHost.StopProofValid && ResourceTraceCount == Trace &&
         MmioWriteCalls == Writes && MmioReadCalls > Reads);
  for (I = 0; I < 5; I++) CHECK (ClockState[I] &&
      mContext.ColdHost.ClockOwned[I] == (I != 0U));
  ColdRegs[UFS_HC_UTMRLBAU_OFFSET / sizeof (UINT32)] = 0U;

  /* Fail before the ID2 gate changes. The owned reverse-prefix remains on;
   * the borrowed ID0 gate and shared VEMC are never part of rollback. */
  ClockDisableNoWriteId = 2U;
  Trace = ResourceTraceCount;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_DEVICE_ERROR);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopGateReleasePending &&
         mContext.ColdHost.CleanupPending && mContext.ColdHost.StopProofValid &&
         mContext.Started && mContext.ControllerHandle == SEED_HANDLE &&
         !UfsHostProtocolInstalled && !UfsPlatformProtocolInstalled &&
         ResourceTraceCount == Trace + 3U && VemcEnabled &&
         OsHandoff[0] == 0U);
  CHECK (ResourceTrace[Trace].Kind == 'C' && ResourceTrace[Trace].Id == 4U && !ResourceTrace[Trace].Enable);
  CHECK (ResourceTrace[Trace + 1U].Kind == 'C' && ResourceTrace[Trace + 1U].Id == 3U && !ResourceTrace[Trace + 1U].Enable);
  CHECK (ResourceTrace[Trace + 2U].Kind == 'C' && ResourceTrace[Trace + 2U].Id == 2U && !ResourceTrace[Trace + 2U].Enable);
  CHECK (!ClockState[4] && !ClockState[3] && ClockState[2] && ClockState[1] && ClockState[0]);
  CHECK (!mContext.ColdHost.ClockOwned[4] && !mContext.ColdHost.ClockOwned[3] &&
         mContext.ColdHost.ClockOwned[2] && mContext.ColdHost.ClockOwned[1] &&
         !mContext.ColdHost.ClockOwned[0]);

  /* A lost shared rail during a partial clock cleanup must not trigger HCI
   * MMIO against gated hardware. Restoring the rail allows saved cleanup to
   * continue without re-reading any controller register. */
  Reads = MmioReadCalls;
  Writes = MmioWriteCalls;
  Trace = ResourceTraceCount;
  VemcEnabled = FALSE;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_NOT_READY);
  CHECK (MmioReadCalls == Reads && MmioWriteCalls == Writes && ResourceTraceCount == Trace &&
         mContext.Started && mContext.ColdHost.StopPhase == MtkUfsColdStopGateReleasePending &&
         ClockState[2] && ClockState[1] && ClockState[0]);
  VemcEnabled = TRUE;
  ClockDisableNoWriteId = MAX_UINT32;
  ClockDisableFailId = 1U;  // The disable lands despite an error; readback is authoritative.
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  CHECK (MmioReadCalls == Reads && MmioWriteCalls == Writes &&
         ResourceTraceCount == Trace + 2U &&
         ResourceTrace[Trace].Id == 2U && ResourceTrace[Trace + 1U].Id == 1U);
  ClockDisableFailId = MAX_UINT32;
  CHECK (!mContext.Started && mContext.ControllerHandle == NULL &&
         !mContext.HostPublished && !mContext.PlatformPublished &&
         mContext.ColdHost.StopPhase == MtkUfsColdStopReleasedNeedsInit &&
         !mContext.ColdHost.StopProofValid && !mContext.ColdHost.CleanupPending &&
         mContext.ColdHost.VccOwned && mContext.ColdHost.VccRetained && VemcEnabled);
  CHECK (ClockState[0] && !ClockState[1] && !ClockState[2] && !ClockState[3] && !ClockState[4]);
  CHECK (!mContext.ColdHost.ClockOwned[0] && !mContext.ColdHost.ClockOwned[1] &&
         !mContext.ColdHost.ClockOwned[2] && !mContext.ColdHost.ClockOwned[3] &&
         !mContext.ColdHost.ClockOwned[4]);
  CheckVemcRetained ();

  /* If another consumer removed shared VEMC, Start must stop before UFS gate
   * writes or any controller MMIO. External rail restoration lets the same
   * stopped marker enter the ordinary zero-root/reset proof. */
  Reads = MmioReadCalls;
  Writes = MmioWriteCalls;
  Trace = ResourceTraceCount;
  VemcEnabled = FALSE;
  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_NOT_READY);
  CHECK (!mEntryQuarantined && !mContext.Started && mContext.ControllerHandle == NULL &&
         MmioReadCalls == Reads && MmioWriteCalls == Writes && ResourceTraceCount == Trace &&
         ClockState[0] && !ClockState[1] && !ClockState[2] && !ClockState[3] && !ClockState[4]);
  VemcEnabled = TRUE;
  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (mContext.Started && mContext.ColdStart &&
         mContext.ColdHost.StopPhase == MtkUfsColdStopReleasedNeedsInit &&
         mContext.ColdHost.Platform.State == MtkUfsPlatformStopped && VemcEnabled);
  CHECK (ResourceTraceCount == Trace + 4U);
  for (I = 0; I < 5; I++) {
    CHECK (ClockState[I]);
    CHECK (mContext.ColdHost.ClockOwned[I] == (I != 0U));
  }
  CHECK (ResourceTrace[Trace].Kind == 'C' && ResourceTrace[Trace].Id == 1U && ResourceTrace[Trace].Enable);
  CHECK (ResourceTrace[Trace + 1U].Kind == 'C' && ResourceTrace[Trace + 1U].Id == 2U && ResourceTrace[Trace + 1U].Enable);
  CHECK (ResourceTrace[Trace + 2U].Kind == 'C' && ResourceTrace[Trace + 2U].Id == 3U && ResourceTrace[Trace + 2U].Enable);
  CHECK (ResourceTrace[Trace + 3U].Kind == 'C' && ResourceTrace[Trace + 3U].Id == 4U && ResourceTrace[Trace + 3U].Enable);
  Interface.UfsHcProtocol = &mContext.HostController;
  Interface.UfsExecUicCommand = ColdUic;
  Trace = ResourceTraceCount;
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_SUCCESS);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopNone &&
         !mContext.ColdHost.StopProofValid &&
         mContext.ColdHost.Platform.State == MtkUfsPlatformHostPrepared &&
         mContext.ColdHost.Platform.RefClock == MtkUfsRefClockOn &&
         ResourceTraceCount == Trace && UfsMmioReadsWithResourcesOff == 0U);
  for (I = 0; I < 5; I++) CHECK (ClockState[I] && mContext.ColdHost.ClockOwned[I] == (I != 0U));

  /* All five gates may be borrowed. A successful cold stop still consumes
   * its one-shot proof token without touching any gate or VEMC, and a new
   * Start must re-prove ten zero roots before reset. */
  StartColdUfsLink (Tree, 0x1FU, &Interface);
  Trace = ResourceTraceCount;
  for (I = 0; I < 5; I++) CHECK (ClockState[I] && !mContext.ColdHost.ClockOwned[I]);
  CHECK (mContext.ColdHost.VccRetained && VemcEnabled);
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPrepareStop, &Interface) == EFI_SUCCESS);
  ZeroMem (&Private, sizeof (Private));
  Private.UfsHostController = &mContext.HostController;
  Private.UfsHcPlatform = &mContext.Platform;
  Private.UfsHcDriverInterface = Interface;
  CHECK (UfsControllerStop (&Private) == EFI_SUCCESS && Private.ControllerStopComplete);
  CheckControllerStopRootsAreZero ();
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcHostStopped, &Interface) == EFI_SUCCESS);
  CHECK (mContext.ColdHost.StopProofValid && ResourceTraceCount == Trace);
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopReleasedNeedsInit &&
         !mContext.ColdHost.StopProofValid && ResourceTraceCount == Trace && VemcEnabled);
  for (I = 0; I < 5; I++) CHECK (ClockState[I] && !mContext.ColdHost.ClockOwned[I]);
  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (mContext.Started && mContext.ColdStart && ResourceTraceCount == Trace);
  Interface.UfsHcProtocol = &mContext.HostController;
  Interface.UfsExecUicCommand = ColdUic;
  CHECK (mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPreHce, &Interface) == EFI_SUCCESS);
  CHECK (mContext.ColdHost.StopPhase == MtkUfsColdStopNone &&
         mContext.ColdHost.Platform.State == MtkUfsPlatformHostPrepared &&
         ResourceTraceCount == Trace && VemcEnabled);
  for (I = 0; I < 5; I++) CHECK (ClockState[I] && !mContext.ColdHost.ClockOwned[I]);

  /* Simulate ExitBootServices becoming visible at the return boundary of the
   * successful DME POWERDOWN UIC. No later cold MMIO, DME, Hibern8, SMC or
   * resource operation may reach the controller or shared rails. */
  StartColdUfsLink (Tree, 0U, &Interface);
  Uics = UicCalls;
  IoCloseAtUicCall = Uics + 2U;
  Reads = MmioReadCalls;
  Writes = MmioWriteCalls;
  Smcs = SmcCalls;
  Status = mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPrepareStop, &Interface);
  CHECK (IoCloseAtUicCall == 0U && mContext.ColdHost.IoClosed && mContext.ExitedBootServices);
  CHECK (Status == EFI_UNSUPPORTED && UicCalls == Uics + 2U && IoClosedSnapshotValid);
  CHECK (MmioReadCalls == IoClosedMmioReadCalls && MmioWriteCalls == IoClosedMmioWriteCalls &&
         SmcCalls == IoClosedSmcCalls && UicCalls == IoClosedUicCalls &&
         ResetWrites == IoClosedResetWrites && ResourceTraceCount == IoClosedResourceEvents);
  Value = 0U;
  CHECK (mContext.ColdHost.Platform.Io.Dme (mContext.ColdHost.Platform.Io.Context,
          0, 0U, &Value) == MtkUfsPlatformUnsupported);
  CHECK (mContext.ColdHost.Platform.Io.Hibernate (mContext.ColdHost.Platform.Io.Context,
          1) == MtkUfsPlatformUnsupported);
  CHECK (mContext.ColdHost.Platform.Io.Read32 (mContext.ColdHost.Platform.Io.Context,
          MtkUfsControllerBank, UFS_HC_ENABLE_OFFSET, &Value) == MtkUfsPlatformUnsupported);
  CHECK (mContext.ColdHost.Platform.Io.Write32 (mContext.ColdHost.Platform.Io.Context,
          MtkUfsResetBank, 0x130U, 1U) == MtkUfsPlatformUnsupported);
  CHECK (mContext.ColdHost.Platform.Io.Smc (mContext.ColdHost.Platform.Io.Context,
          SmcArgs, &SmcResult) == MtkUfsPlatformUnsupported);
  CHECK (MmioReadCalls == IoClosedMmioReadCalls && MmioWriteCalls == IoClosedMmioWriteCalls &&
         SmcCalls == IoClosedSmcCalls && UicCalls == IoClosedUicCalls &&
         ResetWrites == IoClosedResetWrites && ResourceTraceCount == IoClosedResourceEvents);
  CHECK (mContext.HostController.Read (&mContext.HostController, EfiUfsHcWidthUint32,
          UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_UNSUPPORTED);
  CHECK (MmioReadCalls == IoClosedMmioReadCalls && MmioWriteCalls == IoClosedMmioWriteCalls &&
         SmcCalls == IoClosedSmcCalls && UicCalls == IoClosedUicCalls &&
         ResetWrites == IoClosedResetWrites && ResourceTraceCount == IoClosedResourceEvents);

  free (Tree);
}

static VOID
PrepareHandoffDmaFixture (
  VOID
  )
{
  ResetHarness ();
  mContext.Supports64BitDma = TRUE;
  mContext.SeedHandle = SEED_HANDLE;
  mContext.ControllerHandle = SEED_HANDLE;
  mContext.Started = TRUE;
  mContext.HostPublished = TRUE;
  mContext.PlatformPublished = TRUE;
  UfsHostProtocolInstalled = TRUE;
  UfsPlatformProtocolInstalled = TRUE;
  UfsProtocolsInstalled = TRUE;
  mContext.HostController.AllocateBuffer = MtkUfsAllocateBuffer;
  mContext.HostController.FreeBuffer     = MtkUfsFreeBuffer;
  mContext.HostController.Map            = MtkUfsMap;
  mContext.HostController.Unmap          = MtkUfsUnmap;
}

static VOID
CheckHandoffDmaOwnership (
  VOID
  )
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *HostController;
  EFI_PHYSICAL_ADDRESS                DeviceAddress;
  EFI_STATUS                          Status;
  UINTN                               Bytes;
  VOID                                *HostAddress;
  VOID                                *ExtraHostAddress;
  VOID                                *Mapping;
  VOID                                *StaleMapping;
  UINTN                               FreePagesBefore;
  UINTN                               AttributeCallsBefore;
  UINTN                               AllocatePagesBefore;
  UINTN                               MmioReadsBefore;
  UINTN                               MmioWritesBefore;
  UINTN                               PoolAllocationsBefore;
  UINTN                               PoolFreesBefore;
  UINTN                               CacheWriteBefore;
  UINTN                               CacheWriteBackBefore;
  UINTN                               CacheInvalidateBefore;

  PrepareHandoffDmaFixture ();
  HostController = &mContext.HostController;

  // A nonaligned inbound transfer uses a low DMA page and preserves neighbors.
  mContext.Supports64BitDma=FALSE;
  UINT8 Original[128];
  memset(Original,0x55,sizeof(Original));
  UINT8 *Bounce=mmap((VOID *)(UINTN)TEST_DMA_BASE,EFI_PAGE_SIZE,PROT_READ|PROT_WRITE,
                     MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0);
  CHECK((UINTN)Bounce==TEST_DMA_BASE);
  Bytes=13U;
  CHECK(HostController->Map(HostController,EdkiiUfsHcOperationBusMasterWrite,
         Original+3,&Bytes,&DeviceAddress,&Mapping)==EFI_SUCCESS);
  CHECK(DeviceAddress==TEST_DMA_BASE && Bytes==13 && Bounce[12]==0x55);
  memset(Bounce,0x99,4);
  FreePagesStatus=EFI_DEVICE_ERROR;
  CHECK(HostController->Unmap(HostController,Mapping)==EFI_DEVICE_ERROR);
  CHECK(Original[2]==0x55 && Original[3]==0x99 && Original[6]==0x99 &&
        Original[7]==0x55 && Original[16]==0x55);
  Original[3]=0x11;
  FreePagesStatus=EFI_SUCCESS;
  CHECK(HostController->Unmap(HostController,Mapping)==EFI_SUCCESS && Original[3]==0x11);
  CHECK(munmap(Bounce,EFI_PAGE_SIZE)==0);
  PrepareHandoffDmaFixture();
  // A common-buffer mapping pins its backing pages; it may be freed only
  // after the exact map token is released.
  HostAddress = NULL;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &HostAddress, 0U) == EFI_SUCCESS);
  CHECK ((UINTN)HostAddress == TEST_DMA_BASE && !IsListEmpty (&mContext.CommonAllocations));

  Bytes = 32U;
  DeviceAddress = 0xAAAAAAAAU;
  Mapping = (VOID *)(UINTN)0xBBBBU;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterCommonBuffer,
          (VOID *)(UINTN)(TEST_DMA_BASE + EFI_PAGE_SIZE - 16U), &Bytes,
          &DeviceAddress, &Mapping) == EFI_UNSUPPORTED);
  CHECK (Mapping == NULL && DeviceAddress == 0U && FreePagesCalls == 0U);

  Bytes = 64U;
  DeviceAddress = 0U;
  Mapping = NULL;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterWrite,
          HostAddress, &Bytes, &DeviceAddress, &Mapping) == EFI_SUCCESS);
  CHECK (Mapping != NULL && DeviceAddress == TEST_DMA_BASE &&
         CacheWriteBackInvalidateCalls == 1U);
  FreePagesBefore = FreePagesCalls;
  AttributeCallsBefore = SetMemoryAttributesCalls;
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_NOT_READY);
  CHECK (FreePagesCalls == FreePagesBefore && SetMemoryAttributesCalls == AttributeCallsBefore &&
         !IsListEmpty (&mContext.CommonAllocations));
  CHECK (MtkUfsBeginDmaCleanup (&mContext, FALSE) == EFI_SUCCESS);
  CHECK (MtkUfsReclaimUnexposedAllocations (&mContext) == EFI_NOT_READY);
  CHECK (FreePagesCalls == FreePagesBefore && !IsListEmpty (&mContext.CommonAllocations) &&
         !IsListEmpty (&mContext.Mappings));
  MtkUfsEndDmaCleanup (&mContext, FALSE, FALSE);

  // Unknown and already-consumed tokens must be rejected by registry lookup
  // without dereferencing stale caller-controlled addresses.
  StaleMapping = Mapping;
  PoolFreesBefore = HandoffPoolFrees;
  CHECK (HostController->Unmap (HostController, (VOID *)(UINTN)0x1234U) == EFI_INVALID_PARAMETER);
  CHECK (HandoffPoolFrees == PoolFreesBefore);
  CHECK (HostController->Unmap (HostController, Mapping) == EFI_SUCCESS);
  CHECK (CacheInvalidateCalls == 1U);
  CHECK (HostController->Unmap (HostController, StaleMapping) == EFI_INVALID_PARAMETER);
  CHECK (HandoffPoolFrees == PoolFreesBefore + 1U);
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_SUCCESS);
  CHECK (IsListEmpty (&mContext.CommonAllocations) && IsListEmpty (&mContext.Mappings));
  CHECK (FreePagesCalls == FreePagesBefore + 1U);

  // Attribute restoration and page release are separate retryable steps.
  HostAddress = NULL;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &HostAddress, 0U) == EFI_SUCCESS);
  SetMemoryAttributesStatus = EFI_DEVICE_ERROR;
  FreePagesBefore = FreePagesCalls;
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_DEVICE_ERROR);
  CHECK (!IsListEmpty (&mContext.CommonAllocations) && FreePagesCalls == FreePagesBefore);
  SetMemoryAttributesStatus = EFI_SUCCESS;
  FreePagesStatus = EFI_DEVICE_ERROR;
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_DEVICE_ERROR);
  CHECK (!IsListEmpty (&mContext.CommonAllocations) && FreePagesCalls == FreePagesBefore + 1U);
  FreePagesStatus = EFI_SUCCESS;
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_SUCCESS);
  CHECK (IsListEmpty (&mContext.CommonAllocations) && FreePagesCalls == FreePagesBefore + 2U);

  // A failed allocate-time attribute transition leaves an unexposed allocation
  // owner for the serialized Stop cleanup helper to reclaim on retry.
  HostAddress = (VOID *)(UINTN)0xCCU;
  SetMemoryAttributesStatus = EFI_DEVICE_ERROR;
  FreePagesBefore = FreePagesCalls;
  Status = HostController->AllocateBuffer (HostController, AllocateAnyPages,
             EfiBootServicesData, 1U, &HostAddress, 0U);
  CHECK (EFI_ERROR (Status) && HostAddress == NULL &&
         !IsListEmpty (&mContext.CommonAllocations) && FreePagesCalls == FreePagesBefore);
  SetMemoryAttributesStatus = EFI_SUCCESS;
  CHECK (MtkUfsBeginDmaCleanup (&mContext, FALSE) == EFI_SUCCESS);
  CHECK (MtkUfsReclaimUnexposedAllocations (&mContext) == EFI_SUCCESS);
  CHECK (IsListEmpty (&mContext.CommonAllocations) && FreePagesCalls == FreePagesBefore + 1U);
  MtkUfsEndDmaCleanup (&mContext, FALSE, FALSE);

  // Once ExitBootServices has fired, all DMA callbacks fail before touching
  // cache, pool, page, attribute, or MMIO services. Existing ownership remains
  // intact so the caller never loses track of a live mapping.
  HostAddress = NULL;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &HostAddress, 0U) == EFI_SUCCESS);
  Bytes = 64U;
  Mapping = NULL;
  DeviceAddress = 0U;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterWrite,
          HostAddress, &Bytes, &DeviceAddress, &Mapping) == EFI_SUCCESS);
  CHECK (Mapping != NULL);

  mContext.ExitedBootServices = TRUE;
  MmioReadsBefore = MmioReadCalls;
  MmioWritesBefore = MmioWriteCalls;
  PoolAllocationsBefore = HandoffZeroPoolAllocations;
  PoolFreesBefore = HandoffPoolFrees;
  CacheWriteBefore = CacheWriteBackInvalidateCalls;
  CacheWriteBackBefore = CacheWriteBackCalls;
  CacheInvalidateBefore = CacheInvalidateCalls;
  AllocatePagesBefore = AllocatePagesCalls;
  FreePagesBefore = FreePagesCalls;
  AttributeCallsBefore = SetMemoryAttributesCalls;
  ExtraHostAddress = (VOID *)(UINTN)0xDDU;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &ExtraHostAddress, 0U) == EFI_UNSUPPORTED);
  CHECK (ExtraHostAddress == NULL);
  StaleMapping = (VOID *)(UINTN)0xEEEEU;
  DeviceAddress = 0xFFFFFFFFU;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterWrite,
          HostAddress, &Bytes, &DeviceAddress, &StaleMapping) == EFI_UNSUPPORTED);
  CHECK (DeviceAddress == 0U && StaleMapping == NULL);
  CHECK (HostController->Unmap (HostController, Mapping) == EFI_UNSUPPORTED);
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_UNSUPPORTED);
  CHECK (MmioReadCalls == MmioReadsBefore && MmioWriteCalls == MmioWritesBefore &&
         HandoffZeroPoolAllocations == PoolAllocationsBefore && HandoffPoolFrees == PoolFreesBefore &&
         CacheWriteBackCalls == CacheWriteBackBefore &&
         CacheWriteBackInvalidateCalls == CacheWriteBefore && CacheInvalidateCalls == CacheInvalidateBefore &&
         AllocatePagesCalls == AllocatePagesBefore && FreePagesCalls == FreePagesBefore &&
         SetMemoryAttributesCalls == AttributeCallsBefore && mContext.ActiveDmaOperations == 0U);
  CHECK (!IsListEmpty (&mContext.CommonAllocations) && !IsListEmpty (&mContext.Mappings));

  mContext.ExitedBootServices = FALSE;
  CHECK (HostController->Unmap (HostController, Mapping) == EFI_SUCCESS);
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_SUCCESS);
  CHECK (IsListEmpty (&mContext.CommonAllocations) && IsListEmpty (&mContext.Mappings));
}

static VOID
CheckDriverStopDmaLifecycle (
  IN CONST CHAR8  *StockPath
  )
{
  VOID                                *Tree;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *HostController;
  EFI_PHYSICAL_ADDRESS                DeviceAddress;
  EFI_STATUS                          Status;
  UINT32                              ReadValue;
  UINTN                               Bytes;
  UINTN                               Reads;
  VOID                                *HostAddress;
  VOID                                *ExtraHostAddress;
  VOID                                *FirstMapping;
  VOID                                *Mapping;

  ResetHarness ();
  ColdResources = TRUE;
  Tree = LoadStockTree (StockPath);
  PublishRuntimeTree (Tree);
  CHECK (MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (mContext.Started && UfsProtocolsInstalled && mContext.SeedHandle == SEED_HANDLE &&
         !mContext.DmaStopping);
  HostController = &mContext.HostController;

  // A mapped, exposed allocation makes Stop retryable. The failed cleanup
  // must release its pin so ordinary mapping/allocation/flush work can resume.
  HostAddress = NULL;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &HostAddress, 0U) == EFI_SUCCESS);
  Bytes = 64U;
  FirstMapping = NULL;
  DeviceAddress = 0U;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterCommonBuffer,
          HostAddress, &Bytes, &DeviceAddress, &FirstMapping) == EFI_SUCCESS);
  CHECK (FirstMapping != NULL && DeviceAddress == (UINTN)HostAddress);
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_NOT_READY);
  CHECK (mContext.Started && UfsProtocolsInstalled && !mContext.DmaStopping &&
         !mContext.DmaCleanupActive && mContext.ActiveDmaOperations == 0U &&
         UfsProtocolUninstallCalls == 0U);

  Bytes = 32U;
  Mapping = NULL;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterRead,
          (VOID *)(UINTN)(TEST_DMA_BASE + 0x8000U), &Bytes, &DeviceAddress, &Mapping) == EFI_SUCCESS);
  CHECK (Mapping != NULL);
  CHECK (HostController->Unmap (HostController, Mapping) == EFI_SUCCESS);
  ExtraHostAddress = NULL;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &ExtraHostAddress, 0U) == EFI_SUCCESS);
  CHECK (HostController->FreeBuffer (HostController, 1U, ExtraHostAddress) == EFI_SUCCESS);
  CHECK (HostController->Flush (HostController) == EFI_SUCCESS);
  CHECK (HostController->Unmap (HostController, (VOID *)(UINTN)0x1234U) == EFI_INVALID_PARAMETER);
  // The original common allocation remains pinned until its exact token is
  // consumed, despite the temporary map being released.
  CHECK (!IsListEmpty (&mContext.CommonAllocations));
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_NOT_READY);
  CHECK (HostController->Unmap (HostController, FirstMapping) == EFI_SUCCESS);
  CHECK (HostController->Unmap (HostController, FirstMapping) == EFI_INVALID_PARAMETER);
  CHECK (HostController->FreeBuffer (HostController, 1U, HostAddress) == EFI_SUCCESS);

  // A failed allocation-time cache transition retains an unexposed owner.
  // Stop retries its cleanup; a failed page release retains it again.
  HostAddress = (VOID *)(UINTN)0xCCU;
  SetMemoryAttributesStatus = EFI_DEVICE_ERROR;
  Status = HostController->AllocateBuffer (HostController, AllocateAnyPages,
             EfiBootServicesData, 1U, &HostAddress, 0U);
  CHECK (EFI_ERROR (Status) && HostAddress == NULL && !IsListEmpty (&mContext.CommonAllocations));
  SetMemoryAttributesStatus = EFI_SUCCESS;
  FreePagesStatus = EFI_DEVICE_ERROR;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_DEVICE_ERROR);
  CHECK (mContext.Started && UfsProtocolsInstalled && !mContext.DmaStopping &&
         !mContext.DmaCleanupActive && !IsListEmpty (&mContext.CommonAllocations) &&
         UfsProtocolUninstallCalls == 0U);

  // The next Stop releases the now-recoverable owner, then hits an injected
  // protocol-uninstall failure. A further Stop must finish without losing the
  // already-released DMA state.
  FreePagesStatus = EFI_SUCCESS;
  UninstallUfsProtocolsStatus = EFI_ACCESS_DENIED;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_ACCESS_DENIED);
  CHECK (mContext.Started && UfsProtocolsInstalled && IsListEmpty (&mContext.CommonAllocations) &&
         mContext.CallbackClosing && mContext.DmaStopping && !mContext.DmaCleanupActive &&
         mContext.ActiveCallbacks == 0U && mContext.ActiveDmaOperations == 0U &&
         UfsProtocolUninstallCalls == 1U);

  // A failed protocol removal keeps every public provider path closed until a
  // later Stop proves both GUIDs are gone.
  ExtraHostAddress = (VOID *)(UINTN)0xDDU;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &ExtraHostAddress, 0U) == EFI_NOT_READY);
  CHECK (ExtraHostAddress == NULL);
  Bytes = 32U;
  Mapping = (VOID *)(UINTN)0xEEEEU;
  DeviceAddress = MAX_UINT64;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterRead,
          (VOID *)(UINTN)(TEST_DMA_BASE + 0x8000U), &Bytes, &DeviceAddress, &Mapping) == EFI_NOT_READY);
  CHECK (Mapping == NULL && DeviceAddress == 0U);
  Reads = MmioReadCalls;
  ReadValue = 0xA5A5A5A5U;
  CHECK (HostController->Read (HostController, EfiUfsHcWidthUint32,
          UFS_HC_ENABLE_OFFSET, 1U, &ReadValue) == EFI_NOT_READY);
  CHECK (ReadValue == 0xA5A5A5A5U && MmioReadCalls == Reads);
  CHECK (HostController->Flush (HostController) == EFI_NOT_READY && MmioReadCalls == Reads);
  CHECK (mContext.ActiveCallbacks == 0U && mContext.ActiveDmaOperations == 0U);

  UninstallUfsProtocolsStatus = EFI_SUCCESS;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  CHECK (!mContext.Started && !UfsProtocolsInstalled && mContext.DmaStopping &&
         !mContext.DmaCleanupActive && IsListEmpty (&mContext.CommonAllocations) &&
         IsListEmpty (&mContext.Mappings) && UfsProtocolUninstallCalls == 2U);

  // After successful Stop, every DMA callback is pinned until a fresh Start
  // clears the pin only after the registries are empty.
  ExtraHostAddress = (VOID *)(UINTN)0xDDU;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &ExtraHostAddress, 0U) == EFI_NOT_READY);
  CHECK (ExtraHostAddress == NULL);
  Bytes = 32U;
  Mapping = (VOID *)(UINTN)0xEEEEU;
  DeviceAddress = MAX_UINT64;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterRead,
          (VOID *)(UINTN)(TEST_DMA_BASE + 0x8000U), &Bytes, &DeviceAddress, &Mapping) == EFI_NOT_READY);
  CHECK (Mapping == NULL && DeviceAddress == 0U);
  Reads = MmioReadCalls;
  CHECK (HostController->Flush (HostController) == EFI_NOT_READY && MmioReadCalls == Reads);

  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (mContext.Started && !mContext.DmaStopping && !mContext.DmaCleanupActive);
  CHECK (HostController->Flush (HostController) == EFI_SUCCESS);
  ExtraHostAddress = NULL;
  CHECK (HostController->AllocateBuffer (HostController, AllocateAnyPages,
          EfiBootServicesData, 1U, &ExtraHostAddress, 0U) == EFI_SUCCESS);
  Bytes = 32U;
  Mapping = NULL;
  CHECK (HostController->Map (HostController, EdkiiUfsHcOperationBusMasterRead,
          (VOID *)(UINTN)(TEST_DMA_BASE + 0x8000U), &Bytes, &DeviceAddress, &Mapping) == EFI_SUCCESS);
  CHECK (HostController->Unmap (HostController, Mapping) == EFI_SUCCESS);
  CHECK (HostController->FreeBuffer (HostController, 1U, ExtraHostAddress) == EFI_SUCCESS);
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  CHECK (!mContext.Started && mContext.DmaStopping && !mContext.DmaCleanupActive);
  free (Tree);
}

static VOID *
StartDeferredRetainedHost (
  IN CONST CHAR8 *StockPath
  )
{
  VOID *Tree;

  ResetHarness ();
  ColdResources = TRUE;
  Tree = LoadStockTree (StockPath);
  PublishRuntimeTree (Tree);
  HandoffStatus = EFI_NOT_READY;
  CHECK (MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (!mContext.Started && mContext.SeedHandle == SEED_HANDLE && ExitEventOpen);

  PublishRuntimeTree (Tree);
  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (mContext.Started && !mContext.ColdStart && UfsProtocolsInstalled);
  CHECK (!mContext.Starting && !mContext.StartBusy && !mContext.CallbackClosing &&
         !mContext.StopBusy && mContext.ActiveCallbacks == 0U);
  CHECK (mContext.HostPublished && mContext.PlatformPublished);
  return Tree;
}

static VOID
ArmStopAtNextRestoreTpl (VOID)
{
  CHECK (RestoreTplHook == NULL);
  RestoreTplStopStatus = EFI_NOT_READY;
  RestoreTplStopCalls = 0U;
  RestoreTplHookCalls = 0U;
  RestoreTplHook = TryStopAtRestoreTpl;
}

static VOID
CheckStopWasRejectedDuringCallback (
  IN EFI_STATUS CallbackStatus,
  IN UINTN      UninstallCallsBefore
  )
{
  CHECK (CallbackStatus == EFI_SUCCESS);
  CHECK (RestoreTplHook == NULL && RestoreTplHookCalls == 1U);
  CHECK (RestoreTplStopCalls == 1U && RestoreTplStopStatus == EFI_NOT_READY);
  CHECK (mContext.Started && UfsProtocolsInstalled && !mContext.CallbackClosing &&
         !mContext.StopBusy && mContext.ActiveCallbacks == 0U &&
         mContext.ActiveDmaOperations == 0U);
  CHECK (UfsProtocolUninstallCalls == UninstallCallsBefore);
}

static VOID
CheckEventStartStopAdmission (
  IN CONST CHAR8 *StockPath
  )
{
  VOID *Tree;
  UINTN UninstallCallsBefore;

  ResetHarness ();
  ColdResources = TRUE;
  Tree = LoadStockTree (StockPath);
  PublishRuntimeTree (Tree);
  HandoffStatus = EFI_NOT_READY;
  CHECK (MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (!mContext.Started && FdtEventOpen && SeedInstalled);

  /* Deliver FDT after the runtime handoff is ready. The injected Stop runs
   * after StartBusy clears but while the FDT event's own start pin is held. */
  PublishRuntimeTree (Tree);
  UninstallCallsBefore = UfsProtocolUninstallCalls;
  ArmStopAtNextRestoreTpl ();
  WaitForEventStartStopHook = TRUE;
  FireEventAtCallbackTpl (FdtNotify, FDT_EVENT_HANDLE);
  CHECK (RestoreTplHook == NULL && RestoreTplHookCalls == 1U &&
         RestoreTplStopCalls == 1U && RestoreTplStopStatus == EFI_NOT_READY);
  CHECK (mContext.Started && UfsProtocolsInstalled && !mContext.Starting &&
         !mContext.StartBusy && !mContext.StopBusy && !mContext.CallbackClosing &&
         mContext.ActiveCallbacks == 0U && mContext.EventStartCallbacks == 0U &&
         mContext.ActiveDmaOperations == 0U);
  CHECK (UfsProtocolUninstallCalls == UninstallCallsBefore);

  /* Once the event callback releases its pin, the retained provider may stop. */
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  CHECK (!mContext.Started && !UfsProtocolsInstalled && mContext.ControllerHandle == NULL);
  free (Tree);
}

static VOID
CheckProviderCallbackRundown (
  IN CONST CHAR8 *StockPath
  )
{
  VOID *Tree;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL *Host;
  EDKII_UFS_HC_DRIVER_INTERFACE DriverInterface;
  UINTN MmioBar;
  UINTN UninstallCallsBefore;
  UINT32 Value;
  VOID *HostAddress;
  VOID *Mapping;
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  UINTN NumberOfBytes;
  EFI_STATUS Status;

  Tree = StartDeferredRetainedHost (StockPath);
  Host = &mContext.HostController;
  DriverInterface.UfsHcProtocol = Host;
  DriverInterface.UfsExecUicCommand = ColdUic;
  UninstallCallsBefore = UfsProtocolUninstallCalls;

  MmioBar = 0U;
  ArmStopAtNextRestoreTpl ();
  Status = Host->GetUfsHcMmioBar (Host, &MmioBar);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);
  CHECK (MmioBar == (UINTN)UFS_BASE);

  Value = 0U;
  ArmStopAtNextRestoreTpl ();
  Status = Host->Read (Host, EfiUfsHcWidthUint32, UFS_HC_ENABLE_OFFSET, 1U, &Value);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);
  CHECK (Value == HceValue);

  Value = 0U;
  ArmStopAtNextRestoreTpl ();
  Status = Host->Write (Host, EfiUfsHcWidthUint32, UFS_HC_UTRLDBR_OFFSET, 1U, &Value);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);
  CHECK (TransferDoorbellValue == 0U);

  ArmStopAtNextRestoreTpl ();
  Status = mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPreHce, &DriverInterface);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);

  ArmStopAtNextRestoreTpl ();
  Status = Host->Flush (Host);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);

  HostAddress = NULL;
  ArmStopAtNextRestoreTpl ();
  Status = Host->AllocateBuffer (Host, AllocateAnyPages, EfiBootServicesData, 1U,
                                 &HostAddress, 0U);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);
  CHECK (HostAddress != NULL);

  NumberOfBytes = 32U;
  Mapping = NULL;
  DeviceAddress = 0U;
  ArmStopAtNextRestoreTpl ();
  Status = Host->Map (Host, EdkiiUfsHcOperationBusMasterRead,
                      (VOID *)(UINTN)(TEST_DMA_BASE + 0x8000U),
                      &NumberOfBytes, &DeviceAddress, &Mapping);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);
  CHECK (Mapping != NULL && DeviceAddress == (EFI_PHYSICAL_ADDRESS)(UINTN)(TEST_DMA_BASE + 0x8000U));

  ArmStopAtNextRestoreTpl ();
  Status = Host->Unmap (Host, Mapping);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);

  ArmStopAtNextRestoreTpl ();
  Status = Host->FreeBuffer (Host, 1U, HostAddress);
  CheckStopWasRejectedDuringCallback (Status, UninstallCallsBefore);

  ArmStopAtNextRestoreTpl ();
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  CHECK (RestoreTplStopCalls == 1U && RestoreTplStopStatus == EFI_NOT_READY &&
         RestoreTplHook == NULL && RestoreTplHookCalls == 1U);
  CHECK (!mContext.Started && !UfsProtocolsInstalled && mContext.CallbackClosing &&
         mContext.ActiveCallbacks == 0U && mContext.ActiveDmaOperations == 0U);
  free (Tree);
}

static VOID
CheckProtocolPublisherAdmission (
  IN CONST CHAR8 *StockPath
  )
{
  VOID *Tree;

  ResetHarness ();
  ColdResources = TRUE;
  Tree = LoadStockTree (StockPath);
  PublishRuntimeTree (Tree);
  HandoffStatus = EFI_NOT_READY;
  CHECK (MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (!mContext.Started);

  PublishRuntimeTree (Tree);
  RunInstallCallbackExercise = TRUE;
  CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (PublishCallbackCount == 1U && PublishCallbackStatus == EFI_SUCCESS);
  CHECK (mContext.Started && !mContext.Starting && !mContext.StartBusy &&
         mContext.ActiveCallbacks == 0U && mContext.ActiveDmaOperations == 0U);
  CHECK (UfsHostProtocolInstalled && UfsPlatformProtocolInstalled &&
         mContext.HostPublished && mContext.PlatformPublished);
  CHECK (UfsHostProtocolProbeCalls != 0U && UfsPlatformProtocolProbeCalls != 0U);
  CHECK (IsListEmpty (&mContext.CommonAllocations) && IsListEmpty (&mContext.Mappings));
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  free (Tree);
}

static VOID
CheckPartialProtocolPublication (
  IN CONST CHAR8 *StockPath
  )
{
  CONST UINT32 PartialMasks[] = { 1U, 2U };
  UINTN Index;

  for (Index = 0U; Index < ARRAY_SIZE (PartialMasks); Index++) {
    VOID *Tree;
    BOOLEAN HostOnly;
    EFI_STATUS Status;

    ResetHarness ();
    ColdResources = TRUE;
    Tree = LoadStockTree (StockPath);
    PublishRuntimeTree (Tree);
    HandoffStatus = EFI_NOT_READY;
    CHECK (MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL) == EFI_SUCCESS);
    CHECK (!mContext.Started);

    PublishRuntimeTree (Tree);
    InstallUfsProtocolsStatus = EFI_DEVICE_ERROR;
    PartialUfsInstallMask = PartialMasks[Index];
    Status = MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL);
    CHECK (Status == EFI_DEVICE_ERROR);
    HostOnly = (BOOLEAN)(PartialMasks[Index] == 1U);
    CHECK (mContext.Starting && !mContext.StartBusy && !mContext.Started &&
           !mContext.CallbackClosing && mContext.ControllerHandle == SEED_HANDLE);
    CHECK (UfsHostProtocolInstalled == HostOnly &&
           UfsPlatformProtocolInstalled == (BOOLEAN)!HostOnly &&
           UfsProtocolsInstalled == FALSE);
    CHECK (mContext.HostPublished == HostOnly &&
           mContext.PlatformPublished == (BOOLEAN)!HostOnly);
    CHECK (UfsProtocolInstallCalls == 1U &&
           UfsHostProtocolProbeCalls != 0U && UfsPlatformProtocolProbeCalls != 0U);

    InstallUfsProtocolsStatus = EFI_SUCCESS;
    PartialUfsInstallMask = 0U;
    CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
    CHECK (UfsProtocolInstallCalls == 1U && UfsSingleProtocolInstallCalls == 1U);
    CHECK (UfsHostProtocolInstalled && UfsPlatformProtocolInstalled &&
           mContext.HostPublished && mContext.PlatformPublished);
    CHECK (mContext.Started && !mContext.Starting && !mContext.StartBusy &&
           !mContext.CallbackClosing && mContext.ActiveCallbacks == 0U);
    CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
    CHECK (!UfsHostProtocolInstalled && !UfsPlatformProtocolInstalled);
    free (Tree);
  }
}

static VOID
CheckPartialProtocolUninstallRetry (
  IN CONST CHAR8 *StockPath
  )
{
  CONST UINT32 RemovedMasks[] = { 1U, 2U };
  UINTN Index;

  for (Index = 0U; Index < ARRAY_SIZE (RemovedMasks); Index++) {
    VOID *Tree;
    BOOLEAN HostRemoved;
    UINTN HostProbesBefore;
    UINTN PlatformProbesBefore;
    UINTN MultiUninstallsBefore;
    UINTN SingleUninstallsBefore;
    UINTN InstallsBefore;
    UINTN MmioReadsBefore;
    UINT32 Value;
    EFI_STATUS Status;

    Tree = StartDeferredRetainedHost (StockPath);
    UfsProtocolProbeStatus = EFI_DEVICE_ERROR;
    Status = MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL);
    CHECK (Status == EFI_DEVICE_ERROR);
    CHECK (mContext.Started && mContext.CallbackClosing && mContext.DmaStopping &&
           !mContext.DmaCleanupActive && mContext.HostPublished && mContext.PlatformPublished);
    CHECK (UfsHostProtocolInstalled && UfsPlatformProtocolInstalled && UfsProtocolsInstalled &&
           UfsProtocolUninstallCalls == 0U && UfsSingleProtocolUninstallCalls == 0U);
    UfsProtocolProbeStatus = EFI_SUCCESS;

    HostProbesBefore = UfsHostProtocolProbeCalls;
    PlatformProbesBefore = UfsPlatformProtocolProbeCalls;
    MultiUninstallsBefore = UfsProtocolUninstallCalls;
    SingleUninstallsBefore = UfsSingleProtocolUninstallCalls;
    InstallsBefore = UfsProtocolInstallCalls + UfsSingleProtocolInstallCalls;
    UninstallUfsProtocolsStatus = EFI_ACCESS_DENIED;
    PartialUfsUninstallMask = RemovedMasks[Index];

    Status = MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL);
    CHECK (Status == EFI_ACCESS_DENIED);
    HostRemoved = (BOOLEAN)(RemovedMasks[Index] == 1U);
    CHECK (UfsHostProtocolInstalled == (BOOLEAN)!HostRemoved &&
           UfsPlatformProtocolInstalled == HostRemoved &&
           UfsProtocolsInstalled == FALSE);
    CHECK (mContext.Started && mContext.CallbackClosing &&
           !mContext.StopBusy && mContext.ActiveCallbacks == 0U);
    CHECK (UfsHostProtocolProbeCalls > HostProbesBefore &&
           UfsPlatformProtocolProbeCalls > PlatformProbesBefore);
    CHECK (UfsProtocolUninstallCalls == MultiUninstallsBefore + 1U &&
           UfsSingleProtocolUninstallCalls == SingleUninstallsBefore);

    MmioReadsBefore = MmioReadCalls;
    FireEventAtCallbackTpl (FdtNotify, FDT_EVENT_HANDLE);
    CHECK (mContext.Started && mContext.CallbackClosing &&
           UfsHostProtocolInstalled == (BOOLEAN)!HostRemoved &&
           UfsPlatformProtocolInstalled == HostRemoved &&
           UfsProtocolInstallCalls + UfsSingleProtocolInstallCalls == InstallsBefore &&
           UfsProtocolUninstallCalls == MultiUninstallsBefore + 1U &&
           UfsSingleProtocolUninstallCalls == SingleUninstallsBefore &&
           MmioReadCalls == MmioReadsBefore && mContext.ActiveCallbacks == 0U &&
           mContext.EventStartCallbacks == 0U);

    Value = 0xA5A5A5A5U;
    CHECK (mContext.HostController.Read (&mContext.HostController, EfiUfsHcWidthUint32,
            UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_NOT_READY);
    CHECK (MmioReadCalls == MmioReadsBefore && Value == 0xA5A5A5A5U);
    CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_ALREADY_STARTED);
    CHECK (UfsProtocolInstallCalls + UfsSingleProtocolInstallCalls == InstallsBefore);

    UninstallUfsProtocolsStatus = EFI_SUCCESS;
    PartialUfsUninstallMask = 0U;
    CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
    CHECK (UfsProtocolUninstallCalls == MultiUninstallsBefore + 1U &&
           UfsSingleProtocolUninstallCalls == SingleUninstallsBefore + 1U);
    CHECK (!mContext.Started && !UfsHostProtocolInstalled &&
           !UfsPlatformProtocolInstalled && !UfsProtocolsInstalled);
    CHECK (mContext.ActiveCallbacks == 0U && mContext.ActiveDmaOperations == 0U);
    free (Tree);
  }
}

static VOID
CheckEntryRollbackProviderOwnership (
  IN CONST CHAR8 *StockPath
  )
{
  CONST UINT32 PartialMasks[] = { 1U, 2U };
  UINTN Index;

  /* A fully published provider can be stopped before the image's entry
   * resources are removed, so the original entry failure remains reportable. */
  {
    VOID *Tree;

    Tree = StartDeferredRetainedHost (StockPath);
    CHECK (MtkUfsRollbackEntryPoint (EFI_OUT_OF_RESOURCES, IMAGE_HANDLE) == EFI_OUT_OF_RESOURCES);
    CHECK (!mEntryQuarantined && !mContext.Started && !mContext.Starting &&
           !mContext.HostPublished && !mContext.PlatformPublished &&
           mContext.ControllerHandle == NULL && UfsProtocolsInstalled == FALSE);
    CHECK (IsListEmpty (&mContext.CommonAllocations) && IsListEmpty (&mContext.Mappings));
    CHECK (!BindingInstalled && !SeedInstalled && !FdtEventOpen && !EndEventOpen &&
           !ReadyEventOpen && !ExitEventOpen);
    free (Tree);
  }

  /* A failed partial publish is also owned state. Rollback must stop and
   * unpublish whichever provider GUID actually made it onto the handle. */
  for (Index = 0U; Index < ARRAY_SIZE (PartialMasks); Index++) {
    VOID *Tree;

    ResetHarness ();
    ColdResources = TRUE;
    Tree = LoadStockTree (StockPath);
    PublishRuntimeTree (Tree);
    HandoffStatus = EFI_NOT_READY;
    CHECK (MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL) == EFI_SUCCESS);
    CHECK (!mContext.Started);

    PublishRuntimeTree (Tree);
    InstallUfsProtocolsStatus = EFI_DEVICE_ERROR;
    PartialUfsInstallMask = PartialMasks[Index];
    CHECK (MtkUfsDriverStart (&mDriverBinding, SEED_HANDLE, NULL) == EFI_DEVICE_ERROR);
    CHECK (mContext.Starting && !mContext.Started &&
           (mContext.HostPublished || mContext.PlatformPublished));
    CHECK (UfsHostProtocolInstalled || UfsPlatformProtocolInstalled);

    CHECK (MtkUfsRollbackEntryPoint (EFI_OUT_OF_RESOURCES, IMAGE_HANDLE) == EFI_OUT_OF_RESOURCES);
    CHECK (!mEntryQuarantined && !mContext.Started && !mContext.Starting &&
           !mContext.HostPublished && !mContext.PlatformPublished &&
           mContext.ControllerHandle == NULL && !UfsHostProtocolInstalled &&
           !UfsPlatformProtocolInstalled && !UfsProtocolsInstalled);
    CHECK (!BindingInstalled && !SeedInstalled && !FdtEventOpen && !EndEventOpen &&
           !ReadyEventOpen && !ExitEventOpen);
    CHECK (IsListEmpty (&mContext.CommonAllocations) && IsListEmpty (&mContext.Mappings));
    free (Tree);
  }

  /* A live DMA map prevents Stop from proving that the provider is empty.
   * Rollback keeps every callback target and its image resident, then succeeds
   * on retry after the caller releases the map and common buffer. */
  {
    VOID *Tree;
    VOID *HostAddress;
    VOID *Mapping;
    EFI_PHYSICAL_ADDRESS DeviceAddress;
    UINTN NumberOfBytes;
    UINTN BindingUninstallsBefore;
    UINTN SeedUninstallsBefore;
    UINTN FdtClosesBefore;
    UINTN EndClosesBefore;
    UINTN ReadyClosesBefore;
    UINTN ExitClosesBefore;

    Tree = StartDeferredRetainedHost (StockPath);
    HostAddress = NULL;
    CHECK (mContext.HostController.AllocateBuffer (&mContext.HostController,
            AllocateAnyPages, EfiBootServicesData, 1U, &HostAddress, 0U) == EFI_SUCCESS);
    CHECK (HostAddress != NULL);
    NumberOfBytes = 64U;
    Mapping = NULL;
    DeviceAddress = 0U;
    CHECK (mContext.HostController.Map (&mContext.HostController,
            EdkiiUfsHcOperationBusMasterCommonBuffer, HostAddress,
            &NumberOfBytes, &DeviceAddress, &Mapping) == EFI_SUCCESS);
    CHECK (Mapping != NULL && !IsListEmpty (&mContext.CommonAllocations) &&
           !IsListEmpty (&mContext.Mappings));

    BindingUninstallsBefore = BindingUninstallCalls;
    SeedUninstallsBefore = SeedUninstallCalls;
    FdtClosesBefore = CloseFdtEventCalls;
    EndClosesBefore = CloseEndCalls;
    ReadyClosesBefore = CloseReadyEventCalls;
    ExitClosesBefore = CloseExitCalls;
    CHECK (MtkUfsRollbackEntryPoint (EFI_OUT_OF_RESOURCES, IMAGE_HANDLE) == EFI_SUCCESS);
    CHECK (mEntryQuarantined && mContext.Started && UfsProtocolsInstalled &&
           mContext.HostPublished && mContext.PlatformPublished &&
           mContext.ControllerHandle == SEED_HANDLE);
    CHECK (BindingInstalled && SeedInstalled && FdtEventOpen && EndEventOpen &&
           ReadyEventOpen && ExitEventOpen);
    CHECK (BindingUninstallCalls == BindingUninstallsBefore &&
           SeedUninstallCalls == SeedUninstallsBefore &&
           CloseFdtEventCalls == FdtClosesBefore && CloseEndCalls == EndClosesBefore &&
           CloseReadyEventCalls == ReadyClosesBefore && CloseExitCalls == ExitClosesBefore);
    CHECK (!IsListEmpty (&mContext.CommonAllocations) && !IsListEmpty (&mContext.Mappings));

    {
      UINTN ReadsBefore;
      UINTN WritesBefore;
      UINTN InstallsBefore;
      UINTN UninstallsBefore;

      ReadsBefore = MmioReadCalls;
      WritesBefore = MmioWriteCalls;
      InstallsBefore = UfsProtocolInstallCalls + UfsSingleProtocolInstallCalls;
      UninstallsBefore = UfsProtocolUninstallCalls + UfsSingleProtocolUninstallCalls;
      FireEventAtCallbackTpl (FdtNotify, FDT_EVENT_HANDLE);
      CHECK (mEntryQuarantined && mContext.Started && UfsProtocolsInstalled &&
             mContext.ActiveCallbacks == 0U && mContext.EventStartCallbacks == 0U &&
             MmioReadCalls == ReadsBefore && MmioWriteCalls == WritesBefore &&
             UfsProtocolInstallCalls + UfsSingleProtocolInstallCalls == InstallsBefore &&
             UfsProtocolUninstallCalls + UfsSingleProtocolUninstallCalls == UninstallsBefore &&
             !IsListEmpty (&mContext.CommonAllocations) && !IsListEmpty (&mContext.Mappings));
    }

    CHECK (mContext.HostController.Unmap (&mContext.HostController, Mapping) == EFI_SUCCESS);
    CHECK (mContext.HostController.FreeBuffer (&mContext.HostController, 1U, HostAddress) == EFI_SUCCESS);
    CHECK (IsListEmpty (&mContext.CommonAllocations) && IsListEmpty (&mContext.Mappings));
    CHECK (MtkUfsRollbackEntryPoint (EFI_OUT_OF_RESOURCES, IMAGE_HANDLE) == EFI_OUT_OF_RESOURCES);
    CHECK (!mEntryQuarantined && !mContext.Started && !UfsProtocolsInstalled &&
           !mContext.HostPublished && !mContext.PlatformPublished &&
           mContext.ControllerHandle == NULL);
    CHECK (!BindingInstalled && !SeedInstalled && !FdtEventOpen && !EndEventOpen &&
           !ReadyEventOpen && !ExitEventOpen);
    free (Tree);
  }
}

static VOID
ArmEbsAtNextRestoreTpl (VOID)
{
  CHECK (RestoreTplHook == NULL);
  RestoreTplHookCalls = 0U;
  EbsHookSnapshotValid = FALSE;
  RestoreTplHook = ExitAtRestoreTpl;
}

static VOID
CheckEbsAdmissionStop (
  IN EFI_STATUS ExpectedStatus,
  IN UINT32     ExpectedDmaOperations
  )
{
  CHECK (RestoreTplHook == NULL && RestoreTplHookCalls == 1U);
  CHECK (EbsHookSnapshotValid && EbsHookSnapshot.ActiveCallbacks == 1U);
  CHECK (EbsHookSnapshot.ActiveDmaOperations == ExpectedDmaOperations);
  CHECK (mContext.ExitedBootServices && mContext.CallbackClosing &&
         mContext.QuiesceStatus == EFI_NOT_READY);
  CHECK (mContext.Started && UfsProtocolsInstalled && mContext.ActiveCallbacks == 0U &&
         mContext.ActiveDmaOperations == 0U);
  CHECK (EbsHookSnapshot.MmioRead == MmioReadCalls &&
         EbsHookSnapshot.MmioWrite == MmioWriteCalls);
  CHECK (EbsHookSnapshot.MmioRead == EbsHookEntryMmioRead &&
         EbsHookSnapshot.MmioWrite == EbsHookEntryMmioWrite);
  CHECK (OsHandoff[0] == 0U);
  CheckNoEffectsAfterEbsHook ();
  CHECK (ExpectedStatus == EFI_UNSUPPORTED);
}

static VOID
CheckEbsPostExitCallbacks (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *Host,
  IN EDKII_UFS_HC_PLATFORM_PROTOCOL      *Platform
  )
{
  EDKII_UFS_HC_DRIVER_INTERFACE DriverInterface;
  UINTN MmioBar;
  UINT32 Value;
  VOID *Address;

  DriverInterface.UfsHcProtocol = Host;
  DriverInterface.UfsExecUicCommand = ColdUic;
  MmioBar = 0xA5A5U;
  CHECK (Host->GetUfsHcMmioBar (Host, &MmioBar) == EFI_UNSUPPORTED && MmioBar == 0xA5A5U);
  Value = 0x5A5A5A5AU;
  CHECK (Host->Read (Host, EfiUfsHcWidthUint32, UFS_HC_ENABLE_OFFSET, 1U, &Value) == EFI_UNSUPPORTED &&
         Value == 0x5A5A5A5AU);
  Address = (VOID *)(UINTN)0xA5A5U;
  CHECK (Host->AllocateBuffer (Host, AllocateAnyPages, EfiBootServicesData, 1U,
                               &Address, 0U) == EFI_UNSUPPORTED && Address == NULL);
  CHECK (Platform->Callback (SEED_HANDLE, EdkiiUfsHcPreHce, &DriverInterface) == EFI_UNSUPPORTED);
  CheckNoEffectsAfterEbsHook ();
}

static VOID
CheckEbsAdmissionBoundary (
  IN CONST CHAR8 *StockPath
  )
{
  VOID *Tree;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL *Host;
  EDKII_UFS_HC_DRIVER_INTERFACE DriverInterface;
  UINT32 Value;
  VOID *HostAddress;
  VOID *Mapping;
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  UINTN NumberOfBytes;
  EFI_STATUS Status;
  MTK_UFS_COLD_HOST ColdBefore;
  UINTN LookupsBefore;
  UINTN DescriptorReadsBefore;

  Tree = StartDeferredRetainedHost (StockPath);
  Host = &mContext.HostController;
  Value = 0xA5A5A5A5U;
  ArmEbsAtNextRestoreTpl ();
  Status = Host->Read (Host, EfiUfsHcWidthUint32, UFS_HC_ENABLE_OFFSET, 1U, &Value);
  CheckEbsAdmissionStop (EFI_UNSUPPORTED, 0U);
  CHECK (Status == EFI_UNSUPPORTED && Value == 0xA5A5A5A5U);
  CheckEbsPostExitCallbacks (Host, &mContext.Platform);
  free (Tree);

  Tree = StartDeferredRetainedHost (StockPath);
  Host = &mContext.HostController;
  HostAddress = (VOID *)(UINTN)0xA5A5U;
  ArmEbsAtNextRestoreTpl ();
  Status = Host->AllocateBuffer (Host, AllocateAnyPages, EfiBootServicesData, 1U,
                                 &HostAddress, 0U);
  CheckEbsAdmissionStop (EFI_UNSUPPORTED, 1U);
  CHECK (Status == EFI_UNSUPPORTED && HostAddress == NULL);
  CheckEbsPostExitCallbacks (Host, &mContext.Platform);
  free (Tree);

  Tree = StartDeferredRetainedHost (StockPath);
  Host = &mContext.HostController;
  NumberOfBytes = 32U;
  Mapping = (VOID *)(UINTN)0xA5A5U;
  DeviceAddress = MAX_UINT64;
  ArmEbsAtNextRestoreTpl ();
  Status = Host->Map (Host, EdkiiUfsHcOperationBusMasterRead,
                      (VOID *)(UINTN)(TEST_DMA_BASE + 0x8000U),
                      &NumberOfBytes, &DeviceAddress, &Mapping);
  CheckEbsAdmissionStop (EFI_UNSUPPORTED, 1U);
  CHECK (Status == EFI_UNSUPPORTED && Mapping == NULL && DeviceAddress == 0U);
  CheckEbsPostExitCallbacks (Host, &mContext.Platform);
  free (Tree);

  Tree = StartDeferredRetainedHost (StockPath);
  Host = &mContext.HostController;
  DriverInterface.UfsHcProtocol = Host;
  DriverInterface.UfsExecUicCommand = ColdUic;
  mContext.ColdStart = TRUE;
  memcpy (&ColdBefore, &mContext.ColdHost, sizeof (ColdBefore));
  CHECK (!ColdBefore.IoClosed);
  ArmEbsAtNextRestoreTpl ();
  Status = mContext.Platform.Callback (SEED_HANDLE, EdkiiUfsHcPreHce, &DriverInterface);
  CheckEbsAdmissionStop (EFI_UNSUPPORTED, 0U);
  CHECK (Status == EFI_UNSUPPORTED && mContext.ColdHost.IoClosed);
  ColdBefore.IoClosed = TRUE;
  CHECK (memcmp (&ColdBefore, &mContext.ColdHost, sizeof (ColdBefore)) == 0);
  CheckEbsPostExitCallbacks (Host, &mContext.Platform);
  CHECK (memcmp (&ColdBefore, &mContext.ColdHost, sizeof (ColdBefore)) == 0);
  free (Tree);

  /* The FDT event's manufacturer path is admitted through the same rundown
   * gate. EBS after that pin must prevent protocol lookup and descriptor I/O. */
  Tree = LoadStockTree (StockPath);
  StartColdUfsLink (Tree, 0U, &DriverInterface);
  mContext.ColdHost.RefClockWaitConfigured = FALSE;
  DeviceConfigHandle = mContext.ControllerHandle;
  LookupsBefore = DeviceConfigLookupCalls;
  DescriptorReadsBefore = DescriptorReadCalls;
  ArmEbsAtNextRestoreTpl ();
  FdtNotify (FDT_EVENT_HANDLE, NULL);
  CheckEbsAdmissionStop (EFI_UNSUPPORTED, 0U);
  CHECK (DeviceConfigLookupCalls == LookupsBefore &&
         DescriptorReadCalls == DescriptorReadsBefore);
  free (Tree);
}

static VOID
CheckNestedEbsAtRetainedProviderReturn (
  IN CONST CHAR8 *StockPath
  )
{
  VOID *Tree;
  TEST_EBS_ACTIVITY_SNAPSHOT Current;
  UINTN GetIdBaseline;

  Tree = StartDeferredRetainedHost (StockPath);
  CHECK (mContext.Started && !mContext.ColdStart && !mContext.ExitedBootServices);
  GetIdBaseline = ClockGetIdCalls;
  HcsValue = 15U;
  ColdRegs[UFS_HC_VER_OFFSET / sizeof (UINT32)] = 0x210U;
  NestedEbsClockGetIdTarget = GetIdBaseline + 7U;
  NestedEbsClockGetIdCalls = 0U;
  NestedEbsClockGetIdSnapshotValid = FALSE;

  /* Defensive fault injection at the second EBS readiness probe's first
   * successful GetId return. Real UEFI TPL rules do not normally dispatch a
   * nested ExitBootServices notification here. */
  FireEventAtCallbackTpl (ExitNotify, EXIT_EVENT_HANDLE);
  CHECK (NestedEbsClockGetIdTarget == 0U && NestedEbsClockGetIdCalls == 1U &&
         NestedEbsClockGetIdSnapshotValid);
  CHECK (mContext.ExitedBootServices && mContext.ColdHost.IoClosed);
  CaptureEbsActivitySnapshot (&Current);
  CHECK (memcmp (&NestedEbsClockGetIdSnapshot, &Current, sizeof (Current)) == 0);
  CHECK (ClockGetIdCalls == GetIdBaseline + 7U);
  free (Tree);
}

static VOID
CheckReserveMapTypes (VOID)
{
  _Alignas(8) UINT8      Tree[TEST_TREE_SIZE];
  EFI_PHYSICAL_ADDRESS Address;
  UINT64               Size;

  CHECK (fdt_create_empty_tree (Tree, sizeof (Tree)) == 0);
  CHECK (fdt_add_mem_rsv (Tree, 0x123456789000ULL, 0x23456789000ULL) == 0);
  Address = 0U;
  Size = 0U;
  CHECK (FdtGetReserveMapEntry (Tree, 0, &Address, &Size) == 0);
  CHECK (Address == 0x123456789000ULL && Size == 0x23456789000ULL);
  Address = 0x987654321000ULL;
  Size = 0x87654321000ULL;
  CHECK (FdtGetReserveMapEntry (Tree, 65536, &Address, &Size) == -FDT_ERR_BADOFFSET);
  CHECK (Address == 0x987654321000ULL && Size == 0x87654321000ULL);
}

int
main (int argc, char **argv)
{
  CONST UINT32 Faults[] = { 1U, 2U, 4U, 8U, 32U, 64U, 128U, 256U };
  UINTN Index;
  VOID *Tree;

  CHECK(argc == 2);
  CheckReserveMapTypes ();
  CheckOsHandoff(argv[1]);
  CheckRetainedOsHandoff(argv[1]);
  CheckColdHost(argv[1]);
  CheckUnstartedReleaseRequiresAllRoots(argv[1]);
  CheckPartialEnableRollbackRequiresAllRoots(argv[1]);
  CheckColdSuspendResourceChecks(argv[1]);
  CheckColdStoppedRestart(argv[1]);
  CheckColdReleaseRetry(argv[1]);
  CheckColdConfigureRetry(argv[1]);
  CheckIdleRetainedRetry(argv[1]);
  CheckColdBusyStart(argv[1]);
  CheckColdIpFailure(argv[1]);
  CheckColdHostInstallFailure(argv[1]);
  CheckEntryFailureCases ();
  CheckControllerStopUsesHandoffCallbacks ();
  CheckColdStartedStopAndRestart (argv[1]);
  CheckHandoffDmaOwnership ();

  ResetHarness ();
  ColdResources = TRUE;
  Tree = LoadStockTree (argv[1]);
  PublishRuntimeTree (Tree);
  // BootStatus installs gFdtTableGuid before writing the copied address and
  // success status. The entry-point retry and FDT notification must defer.
  HandoffStatus = EFI_NOT_READY;
  CHECK (MtkUfsHandoffEntryPoint (IMAGE_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (BindingInstalled && SeedInstalled && FdtEventOpen && EndEventOpen && ReadyEventOpen && !mContext.Started);
  CHECK (CreateEndCalls == 1);
  CHECK (CreateEventCalls == 1U && CreateReadyEventCalls == 1U);
  CHECK (ConnectCalls == 0U && MmioReadCalls == 0U);
  FireEventAtCallbackTpl (FdtNotify, FDT_EVENT_HANDLE);
  CHECK (!mContext.Started && MmioReadCalls == 0U);

  for (Index = 0; Index < sizeof (Faults) / sizeof (Faults[0]); Index++) {
    VOID *InvalidTree = BuildTree (Faults[Index]);
    CheckRejectedBeforeMmio (InvalidTree);
    free (InvalidTree);
  }

  PublishRuntimeTree (Tree);
  TableOverride = (VOID *)((UINTN)Tree + 8U);
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  TableOverride = NULL;
  HandoffStatus = EFI_NOT_READY;
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  HandoffStatus = EFI_SUCCESS;
  HandoffMagic = 0U;
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  HandoffMagic = HANDOFF_MAGIC;
  FdtMemoryType = EfiGcdMemoryTypeMemoryMappedIo;
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  FdtMemoryType = EfiGcdMemoryTypeSystemMemory;
  UfsMemoryType = EfiGcdMemoryTypeSystemMemory;
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  UfsMemoryType = EfiGcdMemoryTypeMemoryMappedIo;
  UfsMemoryAttributes = 0;
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_UNSUPPORTED);
  UfsMemoryAttributes = EFI_MEMORY_UC;
  CHECK (MtkUfsDriverSupported (&mDriverBinding, SEED_HANDLE, NULL) == EFI_SUCCESS);
  CHECK (MmioReadCalls == 0U);

  FireEventAtCallbackTpl (EndOfDxeNotify, END_EVENT_HANDLE);
  CHECK (ConnectCalls == 0U && mContext.Started && UfsProtocolsInstalled);
  CHECK (MmioReadCalls == 6U && MmioWriteCalls == 0U);
  CHECK (UfsProtocolInstallCalls == 1U);
  CHECK (mContext.Platform.SkipHceReenable && mContext.Platform.SkipLinkStartup);
  FireEventAtCallbackTpl (ReadyToBootNotify, READY_EVENT_HANDLE);
  CHECK (UfsProtocolInstallCalls == 1U);

  CHECK (MtkUfsWriteMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                          UFS_HC_STATUS_OFFSET, 1U, (VOID *)&CapabilitiesValue) == EFI_UNSUPPORTED);
  CHECK (MtkUfsWriteMmio (&mContext.HostController, EfiUfsHcWidthUint32,
                          UFS_HC_UTRLDBR_OFFSET, 1U, (VOID *)&CapabilitiesValue) == EFI_SUCCESS);
  CHECK (MmioWriteCalls == 1U);
  UninstallUfsProtocolsStatus = EFI_ACCESS_DENIED;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_ACCESS_DENIED);
  CHECK (mContext.Started && UfsProtocolsInstalled);
  UninstallUfsProtocolsStatus = EFI_SUCCESS;
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);
  CHECK (!mContext.Started && !UfsProtocolsInstalled && UfsProtocolUninstallCalls == 2U);
  FireEventAtCallbackTpl (FdtNotify, FDT_EVENT_HANDLE);
  CHECK (mContext.Started && UfsProtocolsInstalled && MmioReadCalls == 12U);
  CHECK (MtkUfsDriverStop (&mDriverBinding, SEED_HANDLE, 0U, NULL) == EFI_SUCCESS);

  free (Tree);
  CheckDriverStopDmaLifecycle (argv[1]);
  CheckProtocolPublisherAdmission (argv[1]);
  CheckPartialProtocolPublication (argv[1]);
  CheckPartialProtocolUninstallRetry (argv[1]);
  CheckEntryRollbackProviderOwnership (argv[1]);
  CheckEventStartStopAdmission (argv[1]);
  CheckProviderCallbackRundown (argv[1]);
  CheckEbsAdmissionBoundary (argv[1]);
  CheckNestedEbsAtRetainedProviderReturn (argv[1]);
  printf ("UFS handoff DXE: %u checks passed (FDT retry, resource lifecycle, provider admission/retry, EBS rundown)\n", Checks);
  return 0;
}
