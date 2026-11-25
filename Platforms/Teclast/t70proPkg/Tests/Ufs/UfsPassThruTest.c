// SPDX-License-Identifier: BSD-2-Clause-Patent
// Executes the production UFS PassThru/HCI/DeviceConfig sources against a
// bounded host-side UFSHCI model. No tablet or storage device is accessed.
#include "../MediaTekTest.h"
#include <sys/types.h>
#include <unistd.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Protocol/UfsDeviceConfig.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/DevicePath.h>
#include <IndustryStandard/Ufs.h>
#include <IndustryStandard/UfsHci.h>

#define UFS_PLATFORM_GUID              { 0x3d18ba13, 0xd9b1, 0x4dd4, { 0xb9, 0x16, 0xd3, 0x07, 0x96, 0x53, 0x9e, 0xd8 } }

EFI_GUID gEdkiiUfsHostControllerProtocolGuid = EDKII_UFS_HOST_CONTROLLER_PROTOCOL_GUID;
EFI_GUID gEdkiiUfsHcPlatformProtocolGuid = UFS_PLATFORM_GUID;
EFI_GUID gEfiUfsDeviceConfigProtocolGuid = EFI_UFS_DEVICE_CONFIG_GUID;
EFI_GUID gEfiExtScsiPassThruProtocolGuid = EFI_EXT_SCSI_PASS_THRU_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;

// This test only needs a small initialization deadline. The source under test
// still uses its real retry and cleanup control flow; fake time never sleeps.
#include <UfsPassThru.h>
#undef PcdGet32
#define PcdGet32(TokenName)  16U

#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThru.c"
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruHci.c"
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsDevConfigProtocol.c"

#define TEST_DEVICE_COUNT  2
#define TEST_MMIO_DWORDS   128
#define TEST_DMA_MAX       128
#define TEST_MAP_MAX       512
#define TEST_EVENT_MAX     128
#define TEST_PAGE_SIZE     4096
#define TEST_TRANSFER_SIZE  512

typedef struct _FAKE_DEVICE FAKE_DEVICE;

typedef struct {
  VOID       *Host;
  UINTN      Pages;
  BOOLEAN    Active;
  FAKE_DEVICE *Owner;
} FAKE_DMA;

typedef struct {
  VOID       *Host;
  UINTN      Length;
  BOOLEAN    Active;
  FAKE_DEVICE *Owner;
  EDKII_UFS_HOST_CONTROLLER_OPERATION Operation;
} FAKE_MAPPING;

typedef struct {
  EFI_EVENT_NOTIFY  Notify;
  VOID              *Context;
  EFI_TPL           NotifyTpl;
  BOOLEAN           Closed;
  BOOLEAN           Signaled;
  UINTN             SignalCount;
  UINTN             CloseCount;
} FAKE_EVENT;

struct _FAKE_DEVICE {
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  Host;
  EFI_DEVICE_PATH_PROTOCOL            DevicePath;
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL     *ExtScsi;
  EFI_UFS_DEVICE_CONFIG_PROTOCOL      *DeviceConfig;
  EDKII_UFS_HC_PLATFORM_PROTOCOL       LocalPlatform;
  BOOLEAN                              LocalPlatformAvailable;
  EFI_HANDLE                          Handle;
  UINT32                              Mmio[TEST_MMIO_DWORDS];
  UINTN                               ReadCount[TEST_MMIO_DWORDS];
  UINTN                               WriteCount[TEST_MMIO_DWORDS];
  UINTN                               OpenCount;
  UINTN                               CloseCount;
  UINTN                               InstallCount;
  UINTN                               UninstallCount;
  UINTN                               PlatformCalls[6];
  UINT8                               EnabledLuns;
  UINT8                               HpbLuns;
  UINT8                               FailedUnitDescriptors;
  UINT16                              UnitRequested[8];
  UINT8                               BgOpsFlag;
  BOOLEAN                             RefuseBgOpsClear;
  UINT8                               MalformedUnitDescriptor;
  UINT8                               MalformedQueryResponse;
  UINT8                               FailScsiOpcode;
  BOOLEAN                             FailPowerDownCleanup;
  UINTN                               FailPowerDownFlushCount;
  BOOLEAN                             TimeoutPowerDown;
  UINT8                               MalformedPowerDownResponse;
  EFI_STATUS                          FailReadStatus;
  UINT8                               PowerDownSubmitFault;
  UINTN                               FailPrepareStopCount;
  UINTN                               FailHostStoppedCount;
  UINTN                               CacheCommands[8];
  UINTN                               PowerDownCommands;
  BOOLEAN                             PowerDownEntered;
  UINTN                               CommandsAfterPowerDown;
  UINTN                               Order;
  UINTN                               LastCacheOrder;
  UINTN                               PowerDownOrder;
  UINTN                               PrepareStopOrder;
  UINTN                               HceStopOrder;
  UINTN                               HostStoppedOrder;
  UINTN                               ScsiCommands;
  UINTN                               NopCommands;
  UINTN                               QueryCommands;
  UINTN                               ReadCalls;
  UINTN                               WriteCalls;
  UINTN                               FailReadOffset;
  UINTN                               FailReadOccurrence;
  BOOLEAN                             FailReadAfterValue;
  BOOLEAN                             FailReadPersistent;
  UINTN                               FailWriteOffset;
  UINTN                               FailWriteOccurrence;
  BOOLEAN                             FailWriteAfterValue;
  UINTN                               ReenterReadOffset;
  UINTN                               ReenterReadOccurrence;
  UINTN                               ReenterScsiReadOffset;
  UINTN                               ReenterScsiReadOccurrence;
  BOOLEAN                             ReenterScsiOnReadArmed;
  EFI_STATUS                          ReenterStopStatus;
  BOOLEAN                             ReenterStopArmed;
  BOOLEAN                             HostOpened;
  BOOLEAN                             HoldHceOnDisable;
  BOOLEAN                             HoldTransferRsrOnStop;
  BOOLEAN                             HoldTaskRsrOnStop;
  BOOLEAN                             HoldTransferDbrOnStop;
  BOOLEAN                             HoldTaskDbrOnStop;
  BOOLEAN                             HoldInterruptEnableOnStop;
  BOOLEAN                             HoldBaseOnStop;
  BOOLEAN                             HoldTransferDbrOnStart;
  BOOLEAN                             FailPostLink;
  BOOLEAN                             ArmUnmapFailureAtPostLink;
  UINTN                               MapCount;
  UINTN                               MapCalls;
  UINTN                               ShortMapOnCall;
  UINTN                               FailMapAfterCall;
  EDKII_UFS_HOST_CONTROLLER_OPERATION LastMapOperation;
  VOID                                *LastMappedHost;
  UINT8                               LastDoorbellSlot;
  EFI_TPL                             LastDoorbellReadTpl;
  UINT8                               LastWriteData[TEST_TRANSFER_SIZE];
  UINTN                               LastWriteDataLength;
  UINT32                              ScsiResidual;
  UINT16                              ScsiSenseLength;
  BOOLEAN                             ScsiResidualValid;
  BOOLEAN                             ScsiResidualNoFlag;
  BOOLEAN                             ScsiResidualOverflow;
  UINTN                               UnmapCount;
  UINTN                               AllocateCount;
  UINTN                               FreeCount;
};

typedef struct {
  EDKII_UFS_HC_PLATFORM_PROTOCOL  Protocol;
  UINTN                           LocateCount;
} FAKE_PLATFORM;

static FAKE_DEVICE  Devices[TEST_DEVICE_COUNT];
static FAKE_DMA     Dma[TEST_DMA_MAX];
static FAKE_MAPPING Mappings[TEST_MAP_MAX];
static FAKE_EVENT   *Events[TEST_EVENT_MAX];
static UINTN        EventCount;
static UINTN        DmaCount;
static UINTN        MappingCount;
static UINTN        FailAllocateBufferCount;
static UINTN        FailMapCount;
static UINTN        FailUnmapCount;
static UINTN        FailFreeBufferCount;
static UINTN        FailCreateEventCount;
static UINTN        FailSetTimerCount;
static UINTN        FailCancelTimerCount;
static UINTN        FailCloseEventCount;
static UINTN        FailInstallCount;
static UINTN        FailUninstallCount;
static UINTN        FailCloseProtocolCount;
static CONST EFI_GUID *FailInstallGuid;
static CONST EFI_GUID *FailUninstallGuid;
static FAKE_PLATFORM Platform;
static BOOLEAN      PlatformAvailable;
static UINTN        CallerSignalCount;
static BOOLEAN      ReenterFromSignal;
static UINTN        RestoreTplCount;
static UINTN        RestoreReenterAt;
static BOOLEAN      RestoreReenterArmed;
static EFI_HANDLE   RestoreReenterHandle;
static EFI_STATUS   RestoreReenterStatus;
static EFI_TPL      CurrentTpl = TPL_APPLICATION;
static UINTN        LocalPlatformCallbackCount;
static UINTN        FallbackPlatformCallbackCount;
static UINTN        LocalPlatformOverrideCount;
static UINTN        FallbackPlatformOverrideCount;
static EFI_STATUS   ReentrantStopStatus;
static EFI_STATUS   ReentrantCallStatus;
static EFI_EVENT    ReenterScsiEvent;
static EFI_STATUS   ReentrantScsiStatus;
static EFI_EXT_SCSI_PASS_THRU_PROTOCOL *ReentrantExt;
static UINT8        ReentrantTarget[TARGET_MAX_BYTES];
static UINT8        ReentrantCdb[10] = { 0 };
static EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET ReentrantPacket;

static
BOOLEAN
GuidEqual (
  IN CONST EFI_GUID  *Left,
  IN CONST EFI_GUID  *Right
  )
{
  return (BOOLEAN)(memcmp (Left, Right, sizeof (EFI_GUID)) == 0);
}

static
FAKE_DEVICE *
DeviceFromHandle (
  IN EFI_HANDLE  Handle
  )
{
  UINTN  Index;

  for (Index = 0; Index < TEST_DEVICE_COUNT; Index++) {
    if (Devices[Index].Handle == Handle) {
      return &Devices[Index];
    }
  }

  return NULL;
}

static
UINT16
Swap16 (
  IN UINT16  Value
  )
{
  return (UINT16)((Value << 8) | (Value >> 8));
}

static
UINT32
Swap32 (
  IN UINT32  Value
  )
{
  return ((Value & 0x000000ffU) << 24) |
         ((Value & 0x0000ff00U) << 8) |
         ((Value & 0x00ff0000U) >> 8) |
         ((Value & 0xff000000U) >> 24);
}

static
VOID
FillReadPrdt (
  IN UTP_TRD  *Trd
  )
{
  UTP_TR_PRD  *Prd;
  UINT64      Address;
  UINTN       Index;
  UINTN       Length;

  if ((Trd == NULL) || (Trd->PrdtL == 0)) {
    return;
  }

  Prd = (UTP_TR_PRD *)((UINT8 *)((UINTN)(((UINT64)Trd->UcdBaU << 32) | ((UINT64)Trd->UcdBa << 7))) +
                       Trd->PrdtO * sizeof (UINT32));
  for (Index = 0; Index < Trd->PrdtL; Index++) {
    Address = ((UINT64)Prd[Index].DbAddrU << 32) | ((UINT64)Prd[Index].DbAddr << 2);
    Length  = Prd[Index].DbCount + 1;
    memset ((VOID *)(UINTN)Address, 0xA7, Length);
  }
}

static
VOID
CaptureWritePrdt (
  IN FAKE_DEVICE  *Device,
  IN UTP_TRD      *Trd
  )
{
  UTP_TR_PRD  *Prd;
  UINT64      Address;
  UINTN       Index;
  UINTN       Length;
  UINTN       CopyLength;

  Device->LastWriteDataLength = 0;
  if ((Trd == NULL) || (Trd->PrdtL == 0)) {
    return;
  }

  Prd = (UTP_TR_PRD *)((UINT8 *)((UINTN)(((UINT64)Trd->UcdBaU << 32) | ((UINT64)Trd->UcdBa << 7))) +
                       Trd->PrdtO * sizeof (UINT32));
  for (Index = 0; Index < Trd->PrdtL; Index++) {
    Address    = ((UINT64)Prd[Index].DbAddrU << 32) | ((UINT64)Prd[Index].DbAddr << 2);
    Length     = Prd[Index].DbCount + 1;
    CopyLength = sizeof (Device->LastWriteData) - Device->LastWriteDataLength;
    if (CopyLength > Length) {
      CopyLength = Length;
    }

    if (CopyLength != 0) {
      memcpy (Device->LastWriteData + Device->LastWriteDataLength, (VOID *)(UINTN)Address, CopyLength);
      Device->LastWriteDataLength += CopyLength;
    }
  }
}

static
VOID
CompleteTransfer (
  IN FAKE_DEVICE  *Device,
  IN UINT8        Slot
  )
{
  UINT64    TrlAddress;
  UINT64    DescAddress;
  UTP_TRD   *Trd;
  UINT8     *Command;
  UINT8     Transaction;
  UINT8     *Response;

  TrlAddress = ((UINT64)Device->Mmio[UFS_HC_UTRLBAU_OFFSET / 4] << 32) |
               Device->Mmio[UFS_HC_UTRLBA_OFFSET / 4];
  if ((TrlAddress == 0) || (Slot >= 32)) {
    return;
  }

  Trd = ((UTP_TRD *)(UINTN)TrlAddress) + Slot;
  DescAddress = ((UINT64)Trd->UcdBaU << 32) | ((UINT64)Trd->UcdBa << 7);
  if (DescAddress == 0) {
    return;
  }

  Trd->Ocs = 0;
  Command = (UINT8 *)(UINTN)DescAddress;
  Transaction = Command[0] & 0x3f;
  Response = Command + Trd->RuO * sizeof (UINT32);

  if (Transaction == 0x00) {
    UTP_NOP_IN_UPIU  *NopIn;

    Device->NopCommands++;
    NopIn = (UTP_NOP_IN_UPIU *)Response;
    memset (NopIn, 0, sizeof (*NopIn));
    NopIn->TransCode = 0x20;
    NopIn->TaskTag = ((UTP_NOP_OUT_UPIU *)Command)->TaskTag;
    return;
  }

  if (Transaction == 0x16) {
    UTP_QUERY_REQ_UPIU   *Request;
    UTP_QUERY_RESP_UPIU  *QueryResponse;
    UINT8                *Payload;
    UINT16               Requested;
    UINT16               Actual;

    Device->QueryCommands++;
    Request = (UTP_QUERY_REQ_UPIU *)Command;
    QueryResponse = (UTP_QUERY_RESP_UPIU *)Response;
    memset (QueryResponse, 0, sizeof (*QueryResponse));
    QueryResponse->TransCode = 0x36;
    QueryResponse->TaskTag = Request->TaskTag;
    QueryResponse->QueryResp = UfsUtpQueryResponseSuccess;
    QueryResponse->QueryFunc = Request->QueryFunc;
    QueryResponse->Tsf = Request->Tsf;
    Payload = (UINT8 *)(QueryResponse + 1);
    Requested = Swap16 (Request->Tsf.Length);
    Actual = 0;

    if (Request->Tsf.Opcode == UtpQueryFuncOpcodeRdDesc) {
      if (Request->Tsf.DescId == UfsUnitDesc) {
        UFS_UNIT_DESC  Unit;

        CHECK (Request->Tsf.Index < 8);
        Device->UnitRequested[Request->Tsf.Index] = Requested;
        if ((Device->FailedUnitDescriptors & (1U << Request->Tsf.Index)) != 0) {
          QueryResponse->QueryResp = 0xfc; // Absent LU, as returned by QEMU UFS.
          return;
        }
        memset (&Unit, 0, sizeof (Unit));
        Unit.Length = sizeof (Unit);
        Unit.DescType = UfsUnitDesc;
        Unit.UnitIdx = Request->Tsf.Index;
        Unit.LunEn = (Device->EnabledLuns & (1U << Request->Tsf.Index)) != 0;
        if (Unit.LunEn && (Device->HpbLuns & (1U << Request->Tsf.Index))) Unit.LunEn = 2;
        if (Device->MalformedUnitDescriptor != 0) {
          if (Device->MalformedUnitDescriptor == 1) Unit.Length = 2;
          if (Device->MalformedUnitDescriptor == 2) Unit.DescType = UfsDeviceDesc;
          if (Device->MalformedUnitDescriptor == 3) Unit.UnitIdx ^= 1;
          if (Device->MalformedUnitDescriptor == 4) Unit.LunEn = 3;
          if (Device->MalformedUnitDescriptor == 5) Unit.Length = sizeof (Unit) + 1;
        }
        Actual = (Requested < sizeof (Unit)) ? Requested : sizeof (Unit);
        if (Device->MalformedUnitDescriptor == 6) Actual = MIN (Actual, 2);
        memcpy (Payload, &Unit, Actual);
      } else if (Request->Tsf.DescId == UfsDeviceDesc) {
        UFS_DEV_DESC  DeviceDesc;

        memset (&DeviceDesc, 0, sizeof (DeviceDesc));
        DeviceDesc.Length = sizeof (DeviceDesc);
        DeviceDesc.DescType = UfsDeviceDesc;
        Actual = (Requested < sizeof (DeviceDesc)) ? Requested : sizeof (DeviceDesc);
        memcpy (Payload, &DeviceDesc, Actual);
      } else {
        Actual = Requested;
        memset (Payload, 0, Actual);
      }

      QueryResponse->Tsf.Length = Swap16 (Actual);
      QueryResponse->DataSegLen = Swap16 (Actual);
    } else if ((Request->Tsf.Opcode == UtpQueryFuncOpcodeRdFlag) ||
               (Request->Tsf.Opcode == UtpQueryFuncOpcodeRdAttr))
    {
      QueryResponse->Tsf.Value = 0;
      if ((Request->Tsf.Opcode == UtpQueryFuncOpcodeRdFlag) &&
          (Request->Tsf.DescId == UfsFlagBgOpsEn))
      {
        QueryResponse->Tsf.Value = Swap32 (Device->BgOpsFlag);
      }
    } else if ((Request->Tsf.Opcode == UtpQueryFuncOpcodeClrFlag) &&
               (Request->Tsf.DescId == UfsFlagBgOpsEn) && !Device->RefuseBgOpsClear)
    {
      Device->BgOpsFlag = 0;
    }

    if (Device->PowerDownEntered) {
      Device->CommandsAfterPowerDown++;
      QueryResponse->QueryResp = 0xff;
    }

    switch (Device->MalformedQueryResponse) {
      case 1: QueryResponse->TransCode = 0x20; break;
      case 2: QueryResponse->TaskTag ^= 1; break;
      case 3: QueryResponse->EhsLen = 1; break;
      case 4: QueryResponse->QueryFunc ^= 0x80; break;
      case 5: QueryResponse->Tsf.Opcode ^= 1; break;
      case 6: QueryResponse->Tsf.DescId ^= 1; break;
      case 7: QueryResponse->Tsf.Index ^= 1; break;
      case 8: QueryResponse->Tsf.Selector ^= 1; break;
      case 9: QueryResponse->DataSegLen = Swap16 (Actual ? Actual - 1 : 1); break;
      case 10: QueryResponse->Tsf.Length = Swap16 (Actual ? Actual - 1 : 1); break;
      case 11:
        QueryResponse->DataSegLen = QueryResponse->Tsf.Length = Swap16 (Requested + 1);
        break;
      case 12: Trd->RuL = 0; break;
      case 13: Trd->RuO = MAX_UINT16; break;
    }

    return;
  }

  if (Transaction == 0x01) {
    UTP_RESPONSE_UPIU  *ScsiResponse;

    Device->ScsiCommands++;
    if (Trd->Dd == UfsDataIn) {
      FillReadPrdt (Trd);
    } else if (Trd->Dd == UfsDataOut) {
      CaptureWritePrdt (Device, Trd);
    }

    ScsiResponse = (UTP_RESPONSE_UPIU *)Response;
    memset (ScsiResponse, 0, sizeof (*ScsiResponse));
    ScsiResponse->TransCode = 0x21;
    ScsiResponse->TaskTag = ((UTP_COMMAND_UPIU *)Command)->TaskTag;
    ScsiResponse->Lun = ((UTP_COMMAND_UPIU *)Command)->Lun;
    ScsiResponse->Response = 0;
    ScsiResponse->Status = EFI_EXT_SCSI_STATUS_TARGET_GOOD;
    {
      UTP_COMMAND_UPIU *Request = (UTP_COMMAND_UPIU *)Command;
      if ((Request->Cdb[0] == Device->FailScsiOpcode) && (Device->FailScsiOpcode != 0)) {
        ScsiResponse->Status = EFI_EXT_SCSI_STATUS_TARGET_CHECK_CONDITION;
      }
      if (Device->PowerDownEntered) {
        Device->CommandsAfterPowerDown++;
        ScsiResponse->Status = EFI_EXT_SCSI_STATUS_TARGET_CHECK_CONDITION;
      }
      if (Request->Cdb[0] == 0x35) {
        CHECK (Request->Lun < 8);
        CHECK (Request->Cdb[1] == 0 && Request->Cdb[9] == 0);
        Device->CacheCommands[Request->Lun]++;
        Device->LastCacheOrder = ++Device->Order;
      }
      if (Request->Cdb[0] == 0x1b) {
        CHECK (Request->Lun == UFS_WLUN_UFS_DEV);
        CHECK (Request->Cdb[1] == 0 && Request->Cdb[2] == 0 &&
               Request->Cdb[3] == 0 && Request->Cdb[4] == 0x30 &&
               Request->Cdb[5] == 0);
        Device->PowerDownCommands++;
        Device->PowerDownOrder = ++Device->Order;
        if (ScsiResponse->Status == EFI_EXT_SCSI_STATUS_TARGET_GOOD) {
          Device->PowerDownEntered = TRUE;
          if (Device->TimeoutPowerDown) {
            Device->TimeoutPowerDown = FALSE;
            Device->FailReadOffset = UFS_HC_UTRLDBR_OFFSET;
            Device->FailReadOccurrence = Device->ReadCount[UFS_HC_UTRLDBR_OFFSET / 4] + 1;
            Device->FailReadStatus = EFI_TIMEOUT;
          }
          if (Device->MalformedPowerDownResponse == 1) ScsiResponse->TransCode = 0;
          if (Device->MalformedPowerDownResponse == 2) ScsiResponse->TaskTag ^= 1;
          if (Device->MalformedPowerDownResponse == 3) Trd->Ocs = 1;
          if (Device->MalformedPowerDownResponse == 4) ScsiResponse->Lun ^= 1;
          if (Device->MalformedPowerDownResponse == 5) Trd->RuL = 1;
          if (Device->MalformedPowerDownResponse == 6) ScsiResponse->EhsLen = 1;
          if (Device->MalformedPowerDownResponse == 7) ScsiResponse->DataSegLen = Swap16 (1);
          if (Device->MalformedPowerDownResponse == 8) ScsiResponse->DataSegLen = MAX_UINT16;
          if (Device->MalformedPowerDownResponse == 9) {
            ScsiResponse->DataSegLen = Swap16 (20);
            ScsiResponse->SenseDataLen = Swap16 (17);
          }
          if (Device->MalformedPowerDownResponse == 10) ScsiResponse->Flags |= BIT5;
          if (Device->MalformedPowerDownResponse == 11) {
            // A valid nonempty GOOD response is not a CHECK CONDITION.
            ScsiResponse->DataSegLen = Swap16 (20);
            ScsiResponse->SenseDataLen = Swap16 (18);
            ScsiResponse->SenseData[0] = 0x80; // HPB descriptor area.
          }
          if (Device->MalformedPowerDownResponse == 12) {
            // No segment was transferred; these unreceived padding bytes
            // cannot be interpreted as a sense-data length.
            ScsiResponse->SenseDataLen = MAX_UINT16;
          }
          if (Device->FailPowerDownCleanup) {
            Device->FailPowerDownCleanup = FALSE;
            FailUnmapCount = 1;
          }
        }
      }
    }
    if (Device->ScsiResidualValid) {
      if (!Device->ScsiResidualNoFlag) ScsiResponse->Flags |= BIT5;
      if (Device->ScsiResidualOverflow) ScsiResponse->Flags |= BIT6;
      ScsiResponse->ResTranCount = Swap32 (Device->ScsiResidual);
    }

    if (Device->ScsiSenseLength != 0) {
      ScsiResponse->DataSegLen = Swap16 ((UINT16)(sizeof (ScsiResponse->SenseDataLen) + Device->ScsiSenseLength));
      ScsiResponse->SenseDataLen = Swap16 (Device->ScsiSenseLength);
      memset (ScsiResponse->SenseData, 0x5a,
              (Device->ScsiSenseLength < sizeof (ScsiResponse->SenseData)) ?
              Device->ScsiSenseLength : sizeof (ScsiResponse->SenseData));
    }

    return;
  }
}

static
EFI_STATUS
EFIAPI
FakeGetMmioBar (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                              *MmioBar
  )
{
  FAKE_DEVICE  *Device;

  Device = BASE_CR (This, FAKE_DEVICE, Host);
  if (MmioBar == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *MmioBar = (UINTN)Device->Mmio;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeAllocateBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN EFI_ALLOCATE_TYPE                   Type,
  IN EFI_MEMORY_TYPE                     MemoryType,
  IN UINTN                               Pages,
  OUT VOID                               **HostAddress,
  IN UINT64                              Attributes
  )
{
  FAKE_DEVICE  *Device;
  VOID         *Address;
  UINTN        Index;

  (VOID)Type;
  (VOID)MemoryType;
  (VOID)Attributes;
  Device = BASE_CR (This, FAKE_DEVICE, Host);
  if ((HostAddress == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (FailAllocateBufferCount != 0) {
    FailAllocateBufferCount--;
    *HostAddress = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < TEST_DMA_MAX; Index++) {
    if (!Dma[Index].Active) {
      break;
    }
  }

  if (Index == TEST_DMA_MAX) {
    return EFI_OUT_OF_RESOURCES;
  }

  Address = NULL;
  if (posix_memalign (&Address, TEST_PAGE_SIZE, Pages * TEST_PAGE_SIZE) != 0) {
    return EFI_OUT_OF_RESOURCES;
  }

  memset (Address, 0, Pages * TEST_PAGE_SIZE);
  Dma[Index].Host = Address;
  Dma[Index].Pages = Pages;
  Dma[Index].Active = TRUE;
  Dma[Index].Owner = Device;
  DmaCount++;
  Device->AllocateCount++;
  *HostAddress = Address;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeFreeBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN UINTN                               Pages,
  IN VOID                                *HostAddress
  )
{
  FAKE_DEVICE  *Device;
  UINTN        Index;

  Device = BASE_CR (This, FAKE_DEVICE, Host);
  if (FailFreeBufferCount != 0) {
    FailFreeBufferCount--;
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < TEST_DMA_MAX; Index++) {
    if (Dma[Index].Active && (Dma[Index].Host == HostAddress) && (Dma[Index].Pages == Pages)) {
      free (Dma[Index].Host);
      Dma[Index].Host = NULL;
      Dma[Index].Active = FALSE;
      DmaCount--;
      Device->FreeCount++;
      return EFI_SUCCESS;
    }
  }

  return EFI_INVALID_PARAMETER;
}

static
EFI_STATUS
EFIAPI
FakeMap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN VOID                                 *HostAddress,
  IN OUT UINTN                            *NumberOfBytes,
  OUT EFI_PHYSICAL_ADDRESS                *DeviceAddress,
  OUT VOID                                **Mapping
  )
{
  FAKE_DEVICE  *Device;
  UINTN        Index;

  Device = BASE_CR (This, FAKE_DEVICE, Host);
  if ((HostAddress == NULL) || (NumberOfBytes == NULL) || (DeviceAddress == NULL) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Device->MapCalls++;
  if (FailMapCount != 0) {
    FailMapCount--;
    *Mapping = NULL;
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < TEST_MAP_MAX; Index++) {
    if (!Mappings[Index].Active) {
      break;
    }
  }

  if (Index == TEST_MAP_MAX) {
    return EFI_OUT_OF_RESOURCES;
  }

  Mappings[Index].Host = HostAddress;
  Mappings[Index].Length = *NumberOfBytes;
  Mappings[Index].Active = TRUE;
  Mappings[Index].Owner = Device;
  Mappings[Index].Operation = Operation;
  MappingCount++;
  Device->MapCount++;
  Device->LastMapOperation = Operation;
  Device->LastMappedHost = HostAddress;
  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping = &Mappings[Index];
  if (Device->ShortMapOnCall == Device->MapCalls) {
    *NumberOfBytes = (*NumberOfBytes > sizeof (UINT32)) ? (*NumberOfBytes - sizeof (UINT32)) : 0;
  }

  if (Device->FailMapAfterCall == Device->MapCalls) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeUnmap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN VOID                                *Mapping
  )
{
  FAKE_DEVICE  *Device;
  UINTN        Index;

  Device = BASE_CR (This, FAKE_DEVICE, Host);
  if (FailUnmapCount != 0) {
    FailUnmapCount--;
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < TEST_MAP_MAX; Index++) {
    if (Mappings[Index].Active && (Mapping == &Mappings[Index])) {
      Mappings[Index].Active = FALSE;
      Mappings[Index].Host = NULL;
      MappingCount--;
      Device->UnmapCount++;
      return EFI_SUCCESS;
    }
  }

  return EFI_INVALID_PARAMETER;
}

static
EFI_STATUS
EFIAPI
FakeFlush (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  FAKE_DEVICE *Device = BASE_CR (This, FAKE_DEVICE, Host);
  if (Device->PowerDownEntered && Device->FailPowerDownFlushCount != 0) {
    Device->FailPowerDownFlushCount--;
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

static
BOOLEAN
PowerDownQueued (
  FAKE_DEVICE *Device
  )
{
  UFS_PASS_THRU_PRIVATE_DATA *Private;
  LIST_ENTRY *Entry;
  if (Device->ExtScsi == NULL) return FALSE;
  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
  BASE_LIST_FOR_EACH (Entry, &Private->Queue) {
    UFS_PASS_THRU_TRANS_REQ *Request = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (Entry);
    if (Request->InternalPowerDown) return TRUE;
  }
  return FALSE;
}

static
EFI_STATUS
EFIAPI
FakeRead (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN UINT64                                    Offset,
  IN UINTN                                     Count,
  IN OUT VOID                                  *Buffer
  )
{
  FAKE_DEVICE  *Device;
  UINTN        Register;
  UINT32       Value;

  Device = BASE_CR (This, FAKE_DEVICE, Host);
  if ((Width != EfiUfsHcWidthUint32) || (Count != 1) || (Buffer == NULL) ||
      ((Offset & 3) != 0) || (Offset / 4 >= TEST_MMIO_DWORDS))
  {
    return EFI_INVALID_PARAMETER;
  }

  Register = (UINTN)(Offset / 4);
  Device->ReadCalls++;
  Device->ReadCount[Register]++;
  Value = Device->Mmio[Register];
  if ((Offset == UFS_HC_UTRLRSR_OFFSET) && PowerDownQueued (Device)) {
    if (Device->PowerDownSubmitFault == 1) {
      Device->PowerDownSubmitFault = 0;
      return EFI_DEVICE_ERROR;
    }
    if (Device->PowerDownSubmitFault == 2) Value = 0;
  }
  if (Offset == UFS_HC_UTRLDBR_OFFSET) {
    Device->LastDoorbellReadTpl = CurrentTpl;
  }

  if (Device->ReenterStopArmed && (Offset == Device->ReenterReadOffset) &&
      (Device->ReadCount[Register] == Device->ReenterReadOccurrence))
  {
    Device->ReenterStopArmed = FALSE;
    Device->ReenterStopStatus = gUfsPassThruDriverBinding.Stop (
                                  &gUfsPassThruDriverBinding,
                                  Device->Handle,
                                  0,
                                  NULL
                                  );
  }

  if (Device->ReenterScsiOnReadArmed && (Offset == Device->ReenterScsiReadOffset) &&
      (Device->ReadCount[Register] == Device->ReenterScsiReadOccurrence))
  {
    Device->ReenterScsiOnReadArmed = FALSE;
    ReentrantScsiStatus = Device->ExtScsi->PassThru (
                            Device->ExtScsi,
                            ReentrantTarget,
                            0,
                            &ReentrantPacket,
                            ReenterScsiEvent
                            );
  }

  if ((Device->FailReadOccurrence != 0) && (Offset == Device->FailReadOffset) &&
      ((Device->FailReadPersistent && (Device->ReadCount[Register] >= Device->FailReadOccurrence)) ||
       (!Device->FailReadPersistent && (Device->ReadCount[Register] == Device->FailReadOccurrence))) &&
      !Device->FailReadAfterValue)
  {
    return (Device->FailReadStatus != EFI_SUCCESS) ? Device->FailReadStatus : EFI_DEVICE_ERROR;
  }

  *(UINT32 *)Buffer = Value;
  if ((Device->FailReadOccurrence != 0) && (Offset == Device->FailReadOffset) &&
      ((Device->FailReadPersistent && (Device->ReadCount[Register] >= Device->FailReadOccurrence)) ||
       (!Device->FailReadPersistent && (Device->ReadCount[Register] == Device->FailReadOccurrence))) &&
      Device->FailReadAfterValue)
  {
    return (Device->FailReadStatus != EFI_SUCCESS) ? Device->FailReadStatus : EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

static
VOID
ClearDmaDoorbellsOnHceOff (
  IN FAKE_DEVICE  *Device
  )
{
  if (!Device->HoldTransferDbrOnStop) {
    Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] = 0;
  }

  if (!Device->HoldTaskDbrOnStop) {
    Device->Mmio[UFS_HC_UTMRLDBR_OFFSET / 4] = 0;
  }
}

static
VOID
HandleTransferDoorbell (
  IN FAKE_DEVICE  *Device,
  IN UINT32       Value
  )
{
  UINT8  Slot;

  Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] |= Value;
  for (Slot = 0; Slot < 32; Slot++) {
    if ((Value & ((UINT32)1U << Slot)) != 0) {
      if (Device->HoldTransferDbrOnStart) {
        UINT64  TrlAddress;
        UTP_TRD *Trd;

        TrlAddress = ((UINT64)Device->Mmio[UFS_HC_UTRLBAU_OFFSET / 4] << 32) |
                     Device->Mmio[UFS_HC_UTRLBA_OFFSET / 4];
        if (TrlAddress != 0) {
          Trd = ((UTP_TRD *)(UINTN)TrlAddress) + Slot;
          FillReadPrdt (Trd);
        }
      } else {
        CompleteTransfer (Device, Slot);
      }
    }
  }

  if (!Device->HoldTransferDbrOnStart) {
    Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] &= ~Value;
  }
}

static
EFI_STATUS
EFIAPI
FakeWrite (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN UINT64                                    Offset,
  IN UINTN                                     Count,
  IN OUT VOID                                  *Buffer
  )
{
  FAKE_DEVICE  *Device;
  UINTN        Register;
  UINT32       Value;
  BOOLEAN      FailAfter;

  Device = BASE_CR (This, FAKE_DEVICE, Host);
  if ((Width != EfiUfsHcWidthUint32) || (Count != 1) || (Buffer == NULL) ||
      ((Offset & 3) != 0) || (Offset / 4 >= TEST_MMIO_DWORDS))
  {
    return EFI_INVALID_PARAMETER;
  }

  Register = (UINTN)(Offset / 4);
  Device->WriteCalls++;
  Device->WriteCount[Register]++;
  Value = *(UINT32 *)Buffer;
  FailAfter = FALSE;
  if (PowerDownQueued (Device)) {
    if ((Offset == UFS_HC_UTRLRSR_OFFSET) && (Device->PowerDownSubmitFault == 2)) {
      Device->PowerDownSubmitFault = 0;
      return EFI_DEVICE_ERROR;
    }
    if ((Offset == UFS_HC_UTRLDBR_OFFSET) && (Device->PowerDownSubmitFault == 3)) {
      Device->PowerDownSubmitFault = 0;
      FailAfter = TRUE;
    }
  }
  if ((Device->FailWriteOccurrence != 0) && (Offset == Device->FailWriteOffset) &&
      (Device->WriteCount[Register] == Device->FailWriteOccurrence))
  {
    if (!Device->FailWriteAfterValue) {
      return EFI_DEVICE_ERROR;
    }

    FailAfter = TRUE;
  }

  if (Offset == UFS_HC_IS_OFFSET) {
    Device->Mmio[Register] &= ~Value;
  } else if (Offset == UFS_HC_UTRLDBR_OFFSET) {
    if (Value != 0) {
      UINT8  Slot;

      for (Slot = 0; Slot < 32; Slot++) {
        if ((Value & ((UINT32)1U << Slot)) != 0) {
          Device->LastDoorbellSlot = Slot;
          break;
        }
      }
    }

    HandleTransferDoorbell (Device, Value);
  } else if (Offset == UFS_HC_UTRLCLR_OFFSET) {
    if (!Device->HoldTransferDbrOnStop) {
      Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] &= ~Value;
    }
  } else if ((Offset == UFS_HC_UTMRLDBR_OFFSET) || (Offset == UFS_HC_UTMRLCLR_OFFSET)) {
    Device->Mmio[Register] = Value;
  } else if (Offset == UFS_HC_ENABLE_OFFSET) {
    if (Value == 0) {
      Device->HceStopOrder = ++Device->Order;
    }
    if ((Value == 0) && Device->HoldHceOnDisable) {
      // Model a controller whose HCE write reached the bus but did not stop it.
    } else {
      Device->Mmio[Register] = Value;
      if (Value == 0) {
        ClearDmaDoorbellsOnHceOff (Device);
      }
    }
  } else if ((Offset == UFS_HC_UTRLRSR_OFFSET) && (Value == 0) && Device->HoldTransferRsrOnStop) {
    // Preserve the run bit to model a stuck transfer list.
  } else if ((Offset == UFS_HC_UTMRLRSR_OFFSET) && (Value == 0) && Device->HoldTaskRsrOnStop) {
    // Preserve the run bit to model a stuck task list.
  } else if ((Offset == UFS_HC_IE_OFFSET) && (Value == 0) && Device->HoldInterruptEnableOnStop) {
    // Preserve any interrupt-enable bits to model a stuck register.
  } else if (((Offset == UFS_HC_UTRLBA_OFFSET) || (Offset == UFS_HC_UTRLBAU_OFFSET) ||
              (Offset == UFS_HC_UTMRLBA_OFFSET) || (Offset == UFS_HC_UTMRLBAU_OFFSET)) &&
             (Value == 0) && Device->HoldBaseOnStop)
  {
    // Preserve the descriptor base to model a controller retaining DMA access.
  } else if (Offset == UFS_HC_UIC_CMD_OFFSET) {
    Device->Mmio[Register] = Value;
    Device->Mmio[UFS_HC_IS_OFFSET / 4] |= UFS_HC_IS_UCCS;
    Device->Mmio[UFS_HC_STATUS_OFFSET / 4] |= UFS_HC_HCS_DP | UFS_HC_HCS_UCRDY;
    Device->Mmio[UFS_HC_UCMD_ARG2_OFFSET / 4] = 0;
    Device->Mmio[UFS_HC_UCMD_ARG3_OFFSET / 4] = 0;
  } else {
    Device->Mmio[Register] = Value;
  }

  return FailAfter ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeOpenProtocol (
  IN EFI_HANDLE        Handle,
  IN EFI_GUID          *Protocol,
  OUT VOID             **Interface,
  IN EFI_HANDLE        AgentHandle,
  IN EFI_HANDLE        ControllerHandle,
  IN UINT32            Attributes
  )
{
  FAKE_DEVICE  *Device;

  (VOID)AgentHandle;
  (VOID)ControllerHandle;
  Device = DeviceFromHandle (Handle);
  if ((Device == NULL) || (Protocol == NULL) || (Interface == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (GuidEqual (Protocol, &gEdkiiUfsHostControllerProtocolGuid)) {
    if ((Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER) && Device->HostOpened) {
      return EFI_ALREADY_STARTED;
    }

    if (Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER) {
      Device->HostOpened = TRUE;
      Device->OpenCount++;
    }

    *Interface = &Device->Host;
    return EFI_SUCCESS;
  }

  if (GuidEqual (Protocol, &gEfiExtScsiPassThruProtocolGuid) && (Device->ExtScsi != NULL)) {
    *Interface = Device->ExtScsi;
    return EFI_SUCCESS;
  }

  if (GuidEqual (Protocol, &gEfiUfsDeviceConfigProtocolGuid) && (Device->DeviceConfig != NULL)) {
    *Interface = Device->DeviceConfig;
    return EFI_SUCCESS;
  }

  if (GuidEqual (Protocol, &gEfiDevicePathProtocolGuid)) {
    *Interface = &Device->DevicePath;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

static
EFI_STATUS
EFIAPI
FakeHandleProtocol (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  OUT VOID       **Interface
  )
{
  FAKE_DEVICE  *Device;

  Device = DeviceFromHandle (Handle);
  if ((Device == NULL) || (Protocol == NULL) || (Interface == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (GuidEqual (Protocol, &gEdkiiUfsHcPlatformProtocolGuid)) {
    if (!Device->LocalPlatformAvailable) {
      return EFI_NOT_FOUND;
    }

    *Interface = &Device->LocalPlatform;
    return EFI_SUCCESS;
  }

  if (GuidEqual (Protocol, &gEfiExtScsiPassThruProtocolGuid) && (Device->ExtScsi != NULL)) {
    *Interface = Device->ExtScsi;
    return EFI_SUCCESS;
  }

  if (GuidEqual (Protocol, &gEfiUfsDeviceConfigProtocolGuid) && (Device->DeviceConfig != NULL)) {
    *Interface = Device->DeviceConfig;
    return EFI_SUCCESS;
  }

  if (GuidEqual (Protocol, &gEfiDevicePathProtocolGuid)) {
    *Interface = &Device->DevicePath;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

static
EFI_STATUS
EFIAPI
FakeCloseProtocol (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN EFI_HANDLE  AgentHandle,
  IN EFI_HANDLE  ControllerHandle
  )
{
  FAKE_DEVICE  *Device;

  (VOID)AgentHandle;
  (VOID)ControllerHandle;
  Device = DeviceFromHandle (Handle);
  if (Device == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (FailCloseProtocolCount != 0) {
    FailCloseProtocolCount--;
    return EFI_DEVICE_ERROR;
  }

  if (GuidEqual (Protocol, &gEdkiiUfsHostControllerProtocolGuid)) {
    Device->HostOpened = FALSE;
    Device->CloseCount++;
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeInstallOne (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN VOID        *Interface
  )
{
  FAKE_DEVICE  *Device;

  Device = DeviceFromHandle (Handle);
  if (Device == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((FailInstallCount != 0) && (FailInstallGuid != NULL) && GuidEqual (FailInstallGuid, Protocol)) {
    FailInstallCount--;
    return EFI_DEVICE_ERROR;
  }

  if (GuidEqual (Protocol, &gEfiExtScsiPassThruProtocolGuid)) {
    Device->ExtScsi = (EFI_EXT_SCSI_PASS_THRU_PROTOCOL *)Interface;
  } else if (GuidEqual (Protocol, &gEfiUfsDeviceConfigProtocolGuid)) {
    Device->DeviceConfig = (EFI_UFS_DEVICE_CONFIG_PROTOCOL *)Interface;
  }

  Device->InstallCount++;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeInstallProtocolInterface (
  IN OUT EFI_HANDLE         *Handle,
  IN EFI_GUID               *Protocol,
  IN EFI_INTERFACE_TYPE     InterfaceType,
  IN VOID                   *Interface
  )
{
  if ((Handle == NULL) || (InterfaceType != EFI_NATIVE_INTERFACE)) {
    return EFI_INVALID_PARAMETER;
  }

  return FakeInstallOne (*Handle, Protocol, Interface);
}

static
EFI_STATUS
EFIAPI
FakeInstallMultiple (
  IN OUT EFI_HANDLE  *Handle,
  ...
  )
{
  EFI_STATUS  Status;
  EFI_GUID    *Protocol;
  VOID        *Interface;
  va_list     Args;

  if (Handle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  va_start (Args, Handle);
  Status = EFI_SUCCESS;
  while ((Protocol = va_arg (Args, EFI_GUID *)) != NULL) {
    Interface = va_arg (Args, VOID *);
    Status = FakeInstallOne (*Handle, Protocol, Interface);
    if (EFI_ERROR (Status)) {
      break;
    }
  }

  va_end (Args);
  return Status;
}

static
EFI_STATUS
EFIAPI
FakeUninstallOne (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN VOID        *Interface
  )
{
  FAKE_DEVICE  *Device;

  (VOID)Interface;
  Device = DeviceFromHandle (Handle);
  if (Device == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((FailUninstallCount != 0) && (FailUninstallGuid != NULL) && GuidEqual (FailUninstallGuid, Protocol)) {
    FailUninstallCount--;
    return EFI_DEVICE_ERROR;
  }

  if (GuidEqual (Protocol, &gEfiExtScsiPassThruProtocolGuid)) {
    Device->ExtScsi = NULL;
  } else if (GuidEqual (Protocol, &gEfiUfsDeviceConfigProtocolGuid)) {
    Device->DeviceConfig = NULL;
  }

  Device->UninstallCount++;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeUninstallProtocolInterface (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN VOID        *Interface
  )
{
  return FakeUninstallOne (Handle, Protocol, Interface);
}

static
EFI_STATUS
EFIAPI
FakeUninstallMultiple (
  IN EFI_HANDLE  Handle,
  ...
  )
{
  EFI_STATUS  Status;
  EFI_GUID    *Protocol;
  VOID        *Interface;
  va_list     Args;

  va_start (Args, Handle);
  Status = EFI_SUCCESS;
  while ((Protocol = va_arg (Args, EFI_GUID *)) != NULL) {
    Interface = va_arg (Args, VOID *);
    Status = FakeUninstallOne (Handle, Protocol, Interface);
    if (EFI_ERROR (Status)) {
      break;
    }
  }

  va_end (Args);
  return Status;
}

static
EFI_STATUS
EFIAPI
FakeCreateEvent (
  IN UINT32             Type,
  IN EFI_TPL            NotifyTpl,
  IN EFI_EVENT_NOTIFY   NotifyFunction,
  IN VOID               *NotifyContext,
  OUT EFI_EVENT         *Event
  )
{
  FAKE_EVENT  *NewEvent;

  (VOID)Type;
  if (Event == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (FailCreateEventCount != 0) {
    FailCreateEventCount--;
    // Undefined failure output must not reach SetTimer/CloseEvent cleanup.
    *Event = (EFI_EVENT)(UINTN)0xbad0U;
    return EFI_OUT_OF_RESOURCES;
  }

  if (EventCount == TEST_EVENT_MAX) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewEvent = calloc (1, sizeof (*NewEvent));
  if (NewEvent == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewEvent->Notify = NotifyFunction;
  NewEvent->Context = NotifyContext;
  NewEvent->NotifyTpl = NotifyTpl;
  Events[EventCount++] = NewEvent;
  *Event = (EFI_EVENT)NewEvent;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeSetTimer (
  IN EFI_EVENT       Event,
  IN EFI_TIMER_DELAY Type,
  IN UINT64          TriggerTime
  )
{
  (VOID)Event;
  (VOID)TriggerTime;
  if ((Type == TimerCancel) && (FailCancelTimerCount != 0)) {
    FailCancelTimerCount--;
    return EFI_DEVICE_ERROR;
  }

  if ((Type != TimerCancel) && (FailSetTimerCount != 0)) {
    FailSetTimerCount--;
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeCloseEvent (
  IN EFI_EVENT  Event
  )
{
  FAKE_EVENT  *FakeEvent;

  FakeEvent = (FAKE_EVENT *)Event;
  if (FakeEvent == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (FailCloseEventCount != 0) {
    FailCloseEventCount--;
    return EFI_DEVICE_ERROR;
  }

  FakeEvent->Closed = TRUE;
  FakeEvent->CloseCount++;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeSignalEvent (
  IN EFI_EVENT  Event
  )
{
  FAKE_EVENT  *FakeEvent;
  EFI_TPL     OldTpl;

  FakeEvent = (FAKE_EVENT *)Event;
  if ((FakeEvent == NULL) || FakeEvent->Closed) {
    return EFI_INVALID_PARAMETER;
  }

  FakeEvent->SignalCount++;
  if (!FakeEvent->Signaled) {
    FakeEvent->Signaled = TRUE;
    CallerSignalCount++;
    if (FakeEvent->Notify != NULL) {
      OldTpl = CurrentTpl;
      if (FakeEvent->NotifyTpl > CurrentTpl) {
        CurrentTpl = FakeEvent->NotifyTpl;
      }
      FakeEvent->Notify (Event, FakeEvent->Context);
      CurrentTpl = OldTpl;
    }
  }

  return EFI_SUCCESS;
}

static
EFI_TPL
EFIAPI
FakeRaiseTpl (
  IN EFI_TPL  NewTpl
  )
{
  EFI_TPL  OldTpl;

  OldTpl = CurrentTpl;
  if (NewTpl > CurrentTpl) {
    CurrentTpl = NewTpl;
  }

  return OldTpl;
}

static
VOID
EFIAPI
FakeRestoreTpl (
  IN EFI_TPL  OldTpl
  )
{
  CurrentTpl = OldTpl;
  RestoreTplCount++;
  if (RestoreReenterArmed && (RestoreTplCount == RestoreReenterAt)) {
    RestoreReenterArmed = FALSE;
    RestoreReenterStatus = gUfsPassThruDriverBinding.Stop (
                             &gUfsPassThruDriverBinding,
                             RestoreReenterHandle,
                             0,
                             NULL
                             );
  }
}

static
EFI_STATUS
EFIAPI
FakeLocateProtocol (
  IN EFI_GUID  *Protocol,
  IN VOID      *Registration,
  OUT VOID     **Interface
  )
{
  (VOID)Registration;
  if ((Protocol == NULL) || (Interface == NULL) || !GuidEqual (Protocol, &gEdkiiUfsHcPlatformProtocolGuid)) {
    return EFI_NOT_FOUND;
  }

  if (!PlatformAvailable) {
    return EFI_NOT_FOUND;
  }

  Platform.LocateCount++;
  *Interface = &Platform.Protocol;
  return EFI_SUCCESS;
}

static UINTN ActiveDmaFor (FAKE_DEVICE *Device);

static
EFI_STATUS
EFIAPI
FakePlatformCallback (
  IN EFI_HANDLE                            ControllerHandle,
  IN EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                              *CallbackData
  )
{
  FAKE_DEVICE  *Device;

  (VOID)CallbackData;
  Device = DeviceFromHandle (ControllerHandle);
  if ((Device == NULL) || ((UINTN)CallbackPhase >= ARRAY_SIZE (Device->PlatformCalls))) {
    return EFI_INVALID_PARAMETER;
  }

  Device->PlatformCalls[CallbackPhase]++;
  if (CallbackPhase == EdkiiUfsHcPrepareStop) {
    EDKII_UFS_HC_DRIVER_INTERFACE *Interface = CallbackData;
    EDKII_UIC_COMMAND Command;
    UINT8 Flag;
    UINTN BeforeWrites;
    CHECK (Device->PowerDownEntered);
    CHECK (Interface != NULL && Interface->UfsHcProtocol == &Device->Host);
    ZeroMem (&Command, sizeof (Command));
    Command.Opcode = UfsUicDmeHibernateEnter;
    CHECK (Interface->UfsExecUicCommand (Interface, &Command) == EFI_SUCCESS);
    Command.Opcode = UfsUicDmeSet;
    Command.Arg1 = 0xd0a80000;
    Command.Arg2 = 0;
    Command.Arg3 = 1;
    CHECK (Interface->UfsExecUicCommand (Interface, &Command) == EFI_SUCCESS);
    BeforeWrites = Device->WriteCalls;
    ZeroMem (&Command, sizeof (Command));
    Command.Opcode = UfsUicDmeReset;
    CHECK (Interface->UfsExecUicCommand (Interface, &Command) == EFI_NOT_READY);
    CHECK (Device->WriteCalls == BeforeWrites);
    Flag = 0;
    CHECK (Device->DeviceConfig->RwUfsFlag (Device->DeviceConfig, TRUE, UfsFlagDevInit, &Flag) == EFI_NOT_READY);
    CHECK (Device->Mmio[UFS_HC_ENABLE_OFFSET / 4] == 1);
    CHECK (Device->HostOpened && Device->ExtScsi != NULL);
    CHECK (Device->PowerDownOrder > Device->LastCacheOrder);
    CHECK (ActiveDmaFor (Device) >= 2);
    Device->PrepareStopOrder = ++Device->Order;
    if (Device->FailPrepareStopCount != 0) {
      Device->FailPrepareStopCount--;
      return EFI_DEVICE_ERROR;
    }
  }
  if (CallbackPhase == EdkiiUfsHcHostStopped) {
    static CONST UINTN Offsets[] = { UFS_HC_ENABLE_OFFSET, UFS_HC_IE_OFFSET,
      UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTMRLRSR_OFFSET, UFS_HC_UTRLDBR_OFFSET,
      UFS_HC_UTMRLDBR_OFFSET, UFS_HC_UTRLBA_OFFSET, UFS_HC_UTRLBAU_OFFSET,
      UFS_HC_UTMRLBA_OFFSET, UFS_HC_UTMRLBAU_OFFSET };
    UINTN Index;
    for (Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
      CHECK (Device->Mmio[Offsets[Index] / 4] == 0);
    }
    CHECK (Device->HostOpened && Device->ExtScsi != NULL);
    CHECK (ActiveDmaFor (Device) >= 2);
    CHECK (Device->HceStopOrder > Device->PrepareStopOrder);
    Device->HostStoppedOrder = ++Device->Order;
    if (Device->FailHostStoppedCount != 0) {
      Device->FailHostStoppedCount--;
      return EFI_DEVICE_ERROR;
    }
  }

  if (CallbackPhase == EdkiiUfsHcPostLinkStartup) {
    if (Device->ArmUnmapFailureAtPostLink) {
      Device->ArmUnmapFailureAtPostLink = FALSE;
      FailUnmapCount = 1;
    }

    if (Device->FailPostLink) {
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeLocalPlatformCallback (
  IN EFI_HANDLE                            ControllerHandle,
  IN EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                              *CallbackData
  )
{
  LocalPlatformCallbackCount++;
  return FakePlatformCallback (ControllerHandle, CallbackPhase, CallbackData);
}

static
EFI_STATUS
EFIAPI
FakeFallbackPlatformCallback (
  IN EFI_HANDLE                            ControllerHandle,
  IN EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                              *CallbackData
  )
{
  FallbackPlatformCallbackCount++;
  return FakePlatformCallback (ControllerHandle, CallbackPhase, CallbackData);
}

static
EFI_STATUS
EFIAPI
FakeLocalPlatformOverride (
  IN EFI_HANDLE         ControllerHandle,
  IN OUT EDKII_UFS_HC_INFO  *HcInfo
  )
{
  if ((DeviceFromHandle (ControllerHandle) == NULL) || (HcInfo == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  LocalPlatformOverrideCount++;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
FakeFallbackPlatformOverride (
  IN EFI_HANDLE         ControllerHandle,
  IN OUT EDKII_UFS_HC_INFO  *HcInfo
  )
{
  if ((DeviceFromHandle (ControllerHandle) == NULL) || (HcInfo == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  FallbackPlatformOverrideCount++;
  return EFI_SUCCESS;
}

static
VOID
EFIAPI
ReenterFromCallerEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  FAKE_DEVICE  *Device;

  (VOID)Event;
  Device = (FAKE_DEVICE *)Context;
  if (!ReenterFromSignal) {
    return;
  }

  ReenterFromSignal = FALSE;
  ReentrantStopStatus = gUfsPassThruDriverBinding.Stop (
                          &gUfsPassThruDriverBinding,
                          Device->Handle,
                          0,
                          NULL
                          );

  if (ReentrantExt != NULL) {
    ReentrantCallStatus = ReentrantExt->PassThru (
                            ReentrantExt,
                            ReentrantTarget,
                            0,
                            &ReentrantPacket,
                            NULL
                            );
  }
}

static
VOID
SetupServices (
  VOID
  )
{
  ZeroMem (&Services, sizeof (Services));
  Services.RaiseTPL = FakeRaiseTpl;
  Services.RestoreTPL = FakeRestoreTpl;
  Services.HandleProtocol = FakeHandleProtocol;
  Services.CreateEvent = FakeCreateEvent;
  Services.SetTimer = FakeSetTimer;
  Services.SignalEvent = FakeSignalEvent;
  Services.CloseEvent = FakeCloseEvent;
  Services.OpenProtocol = FakeOpenProtocol;
  Services.CloseProtocol = FakeCloseProtocol;
  Services.LocateProtocol = FakeLocateProtocol;
  Services.InstallProtocolInterface = FakeInstallProtocolInterface;
  Services.UninstallProtocolInterface = FakeUninstallProtocolInterface;
  Services.InstallMultipleProtocolInterfaces = FakeInstallMultiple;
  Services.UninstallMultipleProtocolInterfaces = FakeUninstallMultiple;
  gBS = &Services;
}

static
VOID
ResetHarness (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < EventCount; Index++) {
    free (Events[Index]);
    Events[Index] = NULL;
  }

  memset (Devices, 0, sizeof (Devices));
  memset (Dma, 0, sizeof (Dma));
  memset (Mappings, 0, sizeof (Mappings));
  memset (Events, 0, sizeof (Events));
  DmaCount = 0;
  MappingCount = 0;
  EventCount = 0;
  FailAllocateBufferCount = 0;
  FailMapCount = 0;
  FailUnmapCount = 0;
  FailFreeBufferCount = 0;
  FailCreateEventCount = 0;
  FailSetTimerCount = 0;
  FailCancelTimerCount = 0;
  FailCloseEventCount = 0;
  FailInstallCount = 0;
  FailUninstallCount = 0;
  FailCloseProtocolCount = 0;
  FailInstallGuid = NULL;
  FailUninstallGuid = NULL;
  PlatformAvailable = FALSE;
  ZeroMem (&Platform, sizeof (Platform));
  Platform.Protocol.Version = EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
  Platform.Protocol.Callback = FakePlatformCallback;
  Platform.Protocol.RefClkFreq = EdkiiUfsCardRefClkFreqObsolete;
  Platform.Protocol.SkipHceReenable = FALSE;
  Platform.Protocol.SkipLinkStartup = FALSE;
  CallerSignalCount = 0;
  ReenterFromSignal = FALSE;
  RestoreTplCount = 0;
  RestoreReenterAt = 0;
  RestoreReenterArmed = FALSE;
  RestoreReenterHandle = NULL;
  RestoreReenterStatus = EFI_SUCCESS;
  CurrentTpl = TPL_APPLICATION;
  LocalPlatformCallbackCount = 0;
  FallbackPlatformCallbackCount = 0;
  LocalPlatformOverrideCount = 0;
  FallbackPlatformOverrideCount = 0;
  ReentrantStopStatus = EFI_SUCCESS;
  ReentrantCallStatus = EFI_SUCCESS;
  ReenterScsiEvent = NULL;
  ReentrantScsiStatus = EFI_SUCCESS;
  ReentrantExt = NULL;
  ZeroMem (&ReentrantPacket, sizeof (ReentrantPacket));
  ZeroMem (ReentrantTarget, sizeof (ReentrantTarget));
  TimeNs = 0;
  mUfsHcPlatform = NULL;
  SetupServices ();

  for (Index = 0; Index < TEST_DEVICE_COUNT; Index++) {
    FAKE_DEVICE  *Device;

    Device = &Devices[Index];
    Device->Handle = (EFI_HANDLE)Device;
    Device->EnabledLuns = 1;
    Device->LocalPlatform.Version = EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
    Device->LocalPlatform.OverrideHcInfo = FakeLocalPlatformOverride;
    Device->LocalPlatform.Callback = FakeLocalPlatformCallback;
    Device->LocalPlatform.RefClkFreq = EdkiiUfsCardRefClkFreqObsolete;
    Device->LocalPlatform.SkipHceReenable = FALSE;
    Device->LocalPlatform.SkipLinkStartup = FALSE;
    Device->Host.GetUfsHcMmioBar = FakeGetMmioBar;
    Device->Host.AllocateBuffer = FakeAllocateBuffer;
    Device->Host.FreeBuffer = FakeFreeBuffer;
    Device->Host.Map = FakeMap;
    Device->Host.Unmap = FakeUnmap;
    Device->Host.Flush = FakeFlush;
    Device->Host.Read = FakeRead;
    Device->Host.Write = FakeWrite;
    Device->DevicePath.Type = HARDWARE_DEVICE_PATH;
    Device->DevicePath.SubType = HW_VENDOR_DP;
    Device->DevicePath.Length[0] = sizeof (EFI_DEVICE_PATH_PROTOCOL);
    Device->DevicePath.Length[1] = 0;
    Device->Mmio[UFS_HC_CAP_OFFSET / 4] = UFS_HC_CAP_64ADDR | UFS_HC_CAP_NUTRS | UFS_HC_CAP_NUTMRS;
    Device->Mmio[UFS_HC_VER_OFFSET / 4] = 0x00000200;
    Device->Mmio[UFS_HC_STATUS_OFFSET / 4] = UFS_HC_HCS_DP | UFS_HC_HCS_UCRDY;
    Device->LastDoorbellSlot = MAX_UINT8;
  }
}

static
UINTN
ActiveDmaFor (
  IN FAKE_DEVICE  *Device
  )
{
  UINTN  Index;
  UINTN  Count;

  Count = 0;
  for (Index = 0; Index < TEST_DMA_MAX; Index++) {
    if (Dma[Index].Active && (Dma[Index].Owner == Device)) {
      Count++;
    }
  }

  return Count;
}

static
UINTN
ActiveMappingsFor (
  IN FAKE_DEVICE  *Device
  )
{
  UINTN  Index;
  UINTN  Count;

  Count = 0;
  for (Index = 0; Index < TEST_MAP_MAX; Index++) {
    if (Mappings[Index].Active && (Mappings[Index].Owner == Device)) {
      Count++;
    }
  }

  return Count;
}

static
EFI_STATUS
StartDevice (
  IN FAKE_DEVICE  *Device
  )
{
  return gUfsPassThruDriverBinding.Start (
                                      &gUfsPassThruDriverBinding,
                                      Device->Handle,
                                      NULL
                                      );
}

static
EFI_STATUS
StopDevice (
  IN FAKE_DEVICE  *Device
  )
{
  return gUfsPassThruDriverBinding.Stop (
                                      &gUfsPassThruDriverBinding,
                                      Device->Handle,
                                      0,
                                      NULL
                                      );
}

static
VOID
RunTimerAtNotify (
  IN FAKE_DEVICE  *Device
  )
{
  UFS_PASS_THRU_PRIVATE_DATA  *Private;
  EFI_TPL                    OldTpl;

  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
  OldTpl = CurrentTpl;
  CurrentTpl = TPL_NOTIFY;
  ProcessAsyncTaskList (Private->TimerEvent, Private);
  CurrentTpl = OldTpl;
}

static
UINT8
QueuedSlotForEvent (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN EFI_EVENT                   Event
  )
{
  LIST_ENTRY               *Entry;
  UFS_PASS_THRU_TRANS_REQ  *Request;

  BASE_LIST_FOR_EACH (Entry, &Private->Queue) {
    Request = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (Entry);
    if (Request->CallerEvent == Event) {
      return Request->Slot;
    }
  }

  return MAX_UINT8;
}

static
VOID
CheckClean (
  IN FAKE_DEVICE  *Device
  )
{
  static CONST UINTN  StopProofOffsets[] = {
    UFS_HC_ENABLE_OFFSET,
    UFS_HC_IE_OFFSET,
    UFS_HC_UTRLRSR_OFFSET,
    UFS_HC_UTMRLRSR_OFFSET,
    UFS_HC_UTRLDBR_OFFSET,
    UFS_HC_UTMRLDBR_OFFSET,
    UFS_HC_UTRLBA_OFFSET,
    UFS_HC_UTRLBAU_OFFSET,
    UFS_HC_UTMRLBA_OFFSET,
    UFS_HC_UTMRLBAU_OFFSET
  };
  UINTN  Index;

  CHECK (Device->ExtScsi == NULL);
  CHECK (Device->DeviceConfig == NULL);
  CHECK (!Device->HostOpened);
  CHECK (ActiveDmaFor (Device) == 0);
  CHECK (ActiveMappingsFor (Device) == 0);
  for (Index = 0; Index < ARRAY_SIZE (StopProofOffsets); Index++) {
    CHECK (Device->Mmio[StopProofOffsets[Index] / sizeof (UINT32)] == 0);
  }
}

static
VOID
TestStartSynchronousAndStop (
  VOID
  )
{
  FAKE_DEVICE                              *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  UINT8                                    Target[TARGET_MAX_BYTES];
  UINT8                                    Cdb[6];
  UINT8                                    Flag;
  UINT8                                    Descriptor[sizeof (UFS_DEV_DESC)];
  UINT32                                   DescriptorSize;
  UINTN                                    BeforeDma;
  UINTN                                    BeforeMaps;
  EFI_STATUS                               Status;

  ResetHarness ();
  Device = &Devices[0];
  PlatformAvailable = TRUE;
  Status = StartDevice (Device);
  CHECK (Status == EFI_SUCCESS);
  CHECK (Device->ExtScsi != NULL);
  CHECK (Device->DeviceConfig != NULL);
  CHECK (Device->HostOpened);
  CHECK (ActiveDmaFor (Device) == 2);
  CHECK (ActiveMappingsFor (Device) == 2);
  CHECK (Device->NopCommands >= 1);
  CHECK (Device->QueryCommands >= 10);
  CHECK (Device->PlatformCalls[EdkiiUfsHcPreHce] == 1);
  CHECK (Device->PlatformCalls[EdkiiUfsHcPostHce] == 1);
  CHECK (Device->PlatformCalls[EdkiiUfsHcPreLinkStartup] == 1);
  CHECK (Device->PlatformCalls[EdkiiUfsHcPostLinkStartup] == 1);

  Flag = 0xff;
  CHECK (Device->DeviceConfig->RwUfsFlag (Device->DeviceConfig, TRUE, UfsFlagDevInit, &Flag) == EFI_SUCCESS);
  CHECK (Flag == 0);
  DescriptorSize = sizeof (Descriptor);
  CHECK (Device->DeviceConfig->RwUfsDescriptor (
                                  Device->DeviceConfig,
                                  TRUE,
                                  UfsDeviceDesc,
                                  0,
                                  0,
                                  Descriptor,
                                  &DescriptorSize
                                  ) == EFI_SUCCESS);
  CHECK (DescriptorSize == sizeof (Descriptor));
  CHECK (Descriptor[0] == sizeof (UFS_DEV_DESC));

  BeforeDma = ActiveDmaFor (Device);
  BeforeMaps = ActiveMappingsFor (Device);
  memset (&Packet, 0, sizeof (Packet));
  memset (Target, 0, sizeof (Target));
  memset (Cdb, 0, sizeof (Cdb));
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL) == EFI_SUCCESS);
  CHECK (Device->ScsiCommands == 1);
  CHECK (Packet.TargetStatus == EFI_EXT_SCSI_STATUS_TARGET_GOOD);
  CHECK (ActiveDmaFor (Device) == BeforeDma);
  CHECK (ActiveMappingsFor (Device) == BeforeMaps);

  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestStopRetryFailures (
  VOID
  )
{
  UINTN  CaseIndex;

  for (CaseIndex = 0; CaseIndex < 19; CaseIndex++) {
    FAKE_DEVICE  *Device;
    EFI_STATUS   Status;

    ResetHarness ();
    Device = &Devices[0];
    CHECK (StartDevice (Device) == EFI_SUCCESS);

    switch (CaseIndex) {
      case 0:
        FailCancelTimerCount = 1;
        break;
      case 1:
        FailCloseEventCount = 1;
        break;
      case 2:
        Device->FailReadOffset = UFS_HC_ENABLE_OFFSET;
        Device->FailReadOccurrence = Device->ReadCount[UFS_HC_ENABLE_OFFSET / 4] + 1;
        break;
      case 3:
        Device->FailWriteOffset = UFS_HC_UTMRLRSR_OFFSET;
        Device->FailWriteOccurrence = Device->WriteCount[UFS_HC_UTMRLRSR_OFFSET / 4] + 1;
        Device->FailWriteAfterValue = TRUE;
        break;
      case 4:
        Device->FailWriteOffset = UFS_HC_UTRLRSR_OFFSET;
        Device->FailWriteOccurrence = Device->WriteCount[UFS_HC_UTRLRSR_OFFSET / 4] + 1;
        break;
      case 5:
        Device->FailWriteOffset = UFS_HC_ENABLE_OFFSET;
        Device->FailWriteOccurrence = Device->WriteCount[UFS_HC_ENABLE_OFFSET / 4] + 1;
        Device->FailWriteAfterValue = TRUE;
        break;
      case 6:
        Device->HoldHceOnDisable = TRUE;
        break;
      case 7:
        Device->HoldTransferRsrOnStop = TRUE;
        break;
      case 8:
        Device->HoldTaskRsrOnStop = TRUE;
        break;
      case 9:
        Device->HoldTransferDbrOnStop = TRUE;
        Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] = BIT0;
        break;
      case 10:
        Device->HoldTaskDbrOnStop = TRUE;
        Device->Mmio[UFS_HC_UTMRLDBR_OFFSET / 4] = BIT0;
        break;
      case 11:
        Device->HoldBaseOnStop = TRUE;
        break;
      case 12:
        FailUnmapCount = 1;
        break;
      case 13:
        FailFreeBufferCount = 1;
        break;
      case 14:
        Device->HoldInterruptEnableOnStop = TRUE;
        Device->Mmio[UFS_HC_IE_OFFSET / 4] = 0xfffffffeU;
        break;
      case 15:
        Device->FailWriteOffset = UFS_HC_IE_OFFSET;
        Device->FailWriteOccurrence = Device->WriteCount[UFS_HC_IE_OFFSET / 4] + 1;
        Device->FailWriteAfterValue = TRUE;
        break;
      case 16:
        Device->HoldTransferRsrOnStop = TRUE;
        Device->Mmio[UFS_HC_UTRLRSR_OFFSET / 4] = 0xfffffffeU;
        break;
      case 17:
        Device->HoldTaskRsrOnStop = TRUE;
        Device->Mmio[UFS_HC_UTMRLRSR_OFFSET / 4] = 0xfffffffeU;
        break;
      case 18:
        Device->HoldTransferDbrOnStop = TRUE;
        Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] = 0xfffffffeU;
        break;
    }

    Status = StopDevice (Device);
    CHECK (EFI_ERROR (Status));
    CHECK ((ActiveDmaFor (Device) != 0) || (ActiveMappingsFor (Device) != 0) || Device->HostOpened ||
           (Device->ExtScsi != NULL) || (Device->DeviceConfig != NULL));

    Device->HoldHceOnDisable = FALSE;
    Device->HoldTransferRsrOnStop = FALSE;
    Device->HoldTaskRsrOnStop = FALSE;
    Device->HoldTransferDbrOnStop = FALSE;
    Device->HoldTaskDbrOnStop = FALSE;
    Device->HoldInterruptEnableOnStop = FALSE;
    Device->HoldBaseOnStop = FALSE;
    Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] = 0;
    Device->Mmio[UFS_HC_UTMRLDBR_OFFSET / 4] = 0;
    Device->Mmio[UFS_HC_UTRLRSR_OFFSET / 4] = 0;
    Device->Mmio[UFS_HC_UTMRLRSR_OFFSET / 4] = 0;
    Device->Mmio[UFS_HC_IE_OFFSET / 4] = 0;
    Device->FailReadOccurrence = 0;
    Device->FailWriteOccurrence = 0;

    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CheckClean (Device);
  }

  // Protocol uninstall and the final host-protocol close keep the registry
  // entry alive and permit a retry after the failed Boot Services call.
  for (CaseIndex = 0; CaseIndex < 3; CaseIndex++) {
    FAKE_DEVICE  *Device;

    ResetHarness ();
    Device = &Devices[0];
    CHECK (StartDevice (Device) == EFI_SUCCESS);
    if (CaseIndex == 0) {
      FailSetTimerCount = 0;
      FailUninstallGuid = &gEfiExtScsiPassThruProtocolGuid;
      FailUninstallCount = 1;
    } else if (CaseIndex == 1) {
      FailUninstallGuid = &gEfiUfsDeviceConfigProtocolGuid;
      FailUninstallCount = 1;
    } else {
      FailCloseProtocolCount = 1;
    }

    CHECK (EFI_ERROR (StopDevice (Device)));
    CHECK (Device->HostOpened || (Device->ExtScsi != NULL) || (Device->DeviceConfig != NULL));
    FailUninstallCount = 0;
    FailCloseProtocolCount = 0;
    FailUninstallGuid = NULL;
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CheckClean (Device);
  }
}

static
VOID
TestStartFailureAndRetry (
  VOID
  )
{
  UINTN  CaseIndex;

  for (CaseIndex = 0; CaseIndex < 8; CaseIndex++) {
    FAKE_DEVICE  *Device;
    EFI_STATUS   Status;

    ResetHarness ();
    Device = &Devices[0];
    PlatformAvailable = TRUE;
    if (CaseIndex == 0) {
      FailCreateEventCount = 1;
    } else if (CaseIndex == 1) {
      FailSetTimerCount = 1;
    } else if (CaseIndex == 2) {
      FailInstallGuid = &gEfiUfsDeviceConfigProtocolGuid;
      FailInstallCount = 1;
    } else if (CaseIndex == 3) {
      FailInstallGuid = &gEfiExtScsiPassThruProtocolGuid;
      FailInstallCount = 1;
    } else if (CaseIndex == 4) {
      Device->FailPostLink = TRUE;
    } else if (CaseIndex == 5) {
      FailAllocateBufferCount = 1;
    } else if (CaseIndex == 6) {
      Device->FailWriteOffset = UFS_HC_UTMRLBA_OFFSET;
      Device->FailWriteOccurrence = 1;
      Device->FailWriteAfterValue = TRUE;
    } else {
      FailMapCount = 1;
    }

    Status = StartDevice (Device);
    CHECK (EFI_ERROR (Status));
    CHECK (Device->ExtScsi == NULL);
    CHECK (Device->DeviceConfig == NULL);
    CHECK (ActiveDmaFor (Device) == 0);
    CHECK (ActiveMappingsFor (Device) == 0);
    CHECK (!Device->HostOpened);

    // A repeated Stop is harmless after complete Start rollback. When the
    // Start cleanup was interrupted, the same call resumes the retained work.
    Device->FailPostLink = FALSE;
    Device->FailWriteOccurrence = 0;
    CHECK (StartDevice (Device) == EFI_SUCCESS);
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CheckClean (Device);
  }

  // Force the Start rollback itself to stop at Unmap; retry through the
  // private registry even though ExtSCSI was never published.
  ResetHarness ();
  Devices[0].FailPostLink = TRUE;
  Devices[0].ArmUnmapFailureAtPostLink = TRUE;
  PlatformAvailable = TRUE;
  CHECK (EFI_ERROR (StartDevice (&Devices[0])));
  CHECK (ActiveDmaFor (&Devices[0]) != 0);
  CHECK (Devices[0].HostOpened);
  Devices[0].FailPostLink = FALSE;
  CHECK (StopDevice (&Devices[0]) == EFI_SUCCESS);
  CheckClean (&Devices[0]);
}

static
VOID
TestSynchronousReentryAndTwoControllers (
  VOID
  )
{
  FAKE_DEVICE                              *First;
  FAKE_DEVICE                              *Second;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  UINT8                                    Target[TARGET_MAX_BYTES];
  UINT8                                    Cdb[6];
  UINTN                                    TotalDma;
  UINTN                                    TotalMappings;
  UFS_PASS_THRU_PRIVATE_DATA               *FirstPrivate;
  UFS_PASS_THRU_PRIVATE_DATA               *SecondPrivate;

  ResetHarness ();
  PlatformAvailable = TRUE;
  First = &Devices[0];
  Second = &Devices[1];
  First->LocalPlatformAvailable = TRUE;
  Platform.Protocol.Callback = FakeFallbackPlatformCallback;
  Platform.Protocol.OverrideHcInfo = FakeFallbackPlatformOverride;
  CHECK (StartDevice (First) == EFI_SUCCESS);
  FirstPrivate = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (First->ExtScsi);
  CHECK (FirstPrivate->UfsHcPlatform == &First->LocalPlatform);
  CHECK (StartDevice (Second) == EFI_SUCCESS);
  SecondPrivate = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Second->ExtScsi);
  CHECK (SecondPrivate->UfsHcPlatform == &Platform.Protocol);
  CHECK (FirstPrivate->UfsHcPlatform == &First->LocalPlatform);
  CHECK (Platform.LocateCount == 1);
  CHECK (LocalPlatformCallbackCount != 0);
  CHECK (FallbackPlatformCallbackCount != 0);
  CHECK (LocalPlatformOverrideCount == 1);
  CHECK (FallbackPlatformOverrideCount == 1);
  CHECK (First->PlatformCalls[EdkiiUfsHcPostLinkStartup] == 1);
  CHECK (Second->PlatformCalls[EdkiiUfsHcPostLinkStartup] == 1);
  CHECK (GetUfsHcInfo (FirstPrivate) == EFI_SUCCESS);
  CHECK (LocalPlatformOverrideCount == 2);
  CHECK (FallbackPlatformOverrideCount == 1);
  CHECK (GetUfsHcInfo (SecondPrivate) == EFI_SUCCESS);
  CHECK (FallbackPlatformOverrideCount == 2);
  TotalDma = DmaCount;
  TotalMappings = MappingCount;
  CHECK (TotalDma == 4);
  CHECK (TotalMappings == 4);

  memset (&Packet, 0, sizeof (Packet));
  memset (Target, 0, sizeof (Target));
  memset (Cdb, 0, sizeof (Cdb));
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  First->ReenterReadOffset = UFS_HC_UTRLDBR_OFFSET;
  First->ReenterReadOccurrence = First->ReadCount[UFS_HC_UTRLDBR_OFFSET / 4] + 2;
  First->ReenterStopArmed = TRUE;
  CHECK (First->ExtScsi->PassThru (First->ExtScsi, Target, 0, &Packet, NULL) == EFI_SUCCESS);
  CHECK (First->ReenterStopStatus == EFI_NOT_READY);
  CHECK (First->HostOpened && (First->ExtScsi != NULL));
  RestoreReenterHandle = First->Handle;
  RestoreReenterAt = RestoreTplCount + 2;
  RestoreReenterStatus = EFI_SUCCESS;
  RestoreReenterArmed = TRUE;
  CHECK (StopDevice (First) == EFI_SUCCESS);
  CHECK (!RestoreReenterArmed);
  CHECK (RestoreReenterStatus == EFI_NOT_READY);
  CHECK (DmaCount == 2);
  CHECK (MappingCount == 2);
  CHECK (Second->ExtScsi != NULL);
  CHECK (Second->DeviceConfig != NULL);
  CHECK (StopDevice (Second) == EFI_SUCCESS);
  CHECK (DmaCount == 0);
  CHECK (MappingCount == 0);
}

static
VOID
TestTplGuards (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  UINT8                                     Target[TARGET_MAX_BYTES];
  UINT8                                     Cdb[6];
  UINTN                                     WritesBefore;
  UINTN                                     DmaBefore;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (&Packet, 0, sizeof (Packet));
  memset (Target, 0, sizeof (Target));
  memset (Cdb, 0, sizeof (Cdb));
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  WritesBefore = Device->WriteCalls;
  DmaBefore = ActiveDmaFor (Device);

  CurrentTpl = TPL_NOTIFY;
  CHECK (EfiGetCurrentTpl () == TPL_NOTIFY);
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL) == EFI_NOT_READY);
  CHECK (StopDevice (Device) == EFI_NOT_READY);
  CHECK (CurrentTpl == TPL_NOTIFY);
  CHECK (Device->WriteCalls == WritesBefore);
  CHECK (ActiveDmaFor (Device) == DmaBefore);

  CurrentTpl = TPL_HIGH_LEVEL;
  CHECK (EfiGetCurrentTpl () == TPL_HIGH_LEVEL);
  CHECK (StopDevice (Device) == EFI_NOT_READY);
  CHECK (CurrentTpl == TPL_HIGH_LEVEL);
  CHECK (Device->WriteCalls == WritesBefore);
  CHECK (ActiveDmaFor (Device) == DmaBefore);

  CurrentTpl = TPL_APPLICATION;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestSynchronousDataBounce (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  UINT8                                     Target[TARGET_MAX_BYTES];
  UINT8                                     Cdb[10];
  _Alignas(16) UINT8                        ReadData[TEST_TRANSFER_SIZE];
  _Alignas(16) UINT8                        WriteData[TEST_TRANSFER_SIZE];
  UINTN                                     Index;
  UINTN                                     MapCalls;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (Target, 0, sizeof (Target));
  memset (&Packet, 0, sizeof (Packet));
  memset (Cdb, 0, sizeof (Cdb));
  memset (ReadData, 0x31, sizeof (ReadData));
  Cdb[0] = 0x28;
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.InDataBuffer = ReadData;
  Packet.InTransferLength = sizeof (ReadData);
  MapCalls = Device->MapCalls;
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL) == EFI_SUCCESS);
  CHECK (Device->MapCalls == MapCalls + 2);
  CHECK (Device->LastMapOperation == EdkiiUfsHcOperationBusMasterWrite);
  CHECK (Device->LastMappedHost != ReadData);
  for (Index = 0; Index < sizeof (ReadData); Index++) {
    CHECK (ReadData[Index] == 0xa7);
  }

  memset (Cdb, 0, sizeof (Cdb));
  memset (WriteData, 0x5c, sizeof (WriteData));
  Cdb[0] = 0x2a;
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_WRITE;
  Packet.OutDataBuffer = WriteData;
  Packet.OutTransferLength = sizeof (WriteData);
  Packet.InDataBuffer = NULL;
  Packet.InTransferLength = 0;
  MapCalls = Device->MapCalls;
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL) == EFI_SUCCESS);
  CHECK (Device->MapCalls == MapCalls + 2);
  CHECK (Device->LastMapOperation == EdkiiUfsHcOperationBusMasterRead);
  CHECK (Device->LastMappedHost != WriteData);
  CHECK (Device->LastWriteDataLength == sizeof (WriteData));
  for (Index = 0; Index < sizeof (WriteData); Index++) {
    CHECK (Device->LastWriteData[Index] == WriteData[Index]);
  }

  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestScsiResponseAndMappingFailures (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  UINT8                                     Target[TARGET_MAX_BYTES];
  UINT8                                     Cdb[10];
  _Alignas(16) UINT8                        Sense[64];
  _Alignas(16) UINT8                        Data[TEST_TRANSFER_SIZE];
  UINTN                                     DmaBefore;
  UINTN                                     MapsBefore;
  UINTN                                     MapCall;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (Target, 0, sizeof (Target));
  memset (&Packet, 0, sizeof (Packet));
  memset (Cdb, 0, sizeof (Cdb));
  memset (Data, 0x42, sizeof (Data));
  Cdb[0] = 0x28;
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.InDataBuffer = Data;
  Packet.InTransferLength = sizeof (Data);
  Device->ScsiResidualValid = TRUE;
  // Real normal READ completion, including devices omitting UNDERFLOW.
  for (UINTN NoFlag = 0; NoFlag < 2; NoFlag++) {
    Device->ScsiResidualNoFlag = (BOOLEAN)NoFlag;
    for (UINTN Residual = 0; Residual < 2; Residual++) {
      Device->ScsiResidual = (UINT32)Residual;
      Packet.InTransferLength = sizeof (Data);
      memset (Data, 0x42, sizeof (Data));
      CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL) == EFI_SUCCESS);
      CHECK (Packet.HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK);
      CHECK (Packet.TargetStatus == EFI_EXT_SCSI_STATUS_TARGET_GOOD);
      CHECK (Packet.InTransferLength == sizeof (Data) - Residual);
      for (UINTN Byte = 0; Byte < sizeof (Data) - Residual; Byte++) CHECK (Data[Byte] == 0xa7);
      if (Residual) CHECK (Data[sizeof (Data)-1] == 0x42);
    }
    Device->ScsiResidual = sizeof (Data) + 1;
    Packet.InTransferLength = sizeof (Data);
    memset (Data, 0x42, sizeof (Data));
    CHECK (EFI_ERROR (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL)));
    CHECK (Packet.HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR);
    CHECK (Packet.InTransferLength == sizeof (Data) && Data[0] == 0x42);
    Device->ScsiResidualOverflow = TRUE;
    Device->ScsiResidual = 1;
    Packet.InTransferLength = sizeof (Data);
    CHECK (EFI_ERROR (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL)));
    CHECK (Packet.InTransferLength == sizeof (Data) && Data[0] == 0x42);
    Device->ScsiResidualOverflow = FALSE;
  }
  Device->ScsiResidualNoFlag = FALSE;
  Device->ScsiResidual = sizeof (Data) + 1;
  CHECK (EFI_ERROR (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL)));
  CHECK (Packet.HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR);
  CHECK (Packet.InTransferLength == sizeof (Data));
  CHECK (Data[0] == 0x42);

  memset (&Packet, 0, sizeof (Packet));
  memset (Cdb, 0, sizeof (Cdb));
  memset (Sense, 0x6d, sizeof (Sense));
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = 6;
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.SenseData = Sense;
  Packet.SenseDataLength = sizeof (Sense);
  Device->ScsiResidualValid = FALSE;
  Device->ScsiSenseLength = (UINT16)(sizeof (((UTP_RESPONSE_UPIU *)0)->SenseData) + 1);
  CHECK (EFI_ERROR (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL)));
  CHECK (Packet.HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR);
  CHECK (Packet.SenseDataLength == 0);
  CHECK (Sense[0] == 0x6d);
  Device->ScsiSenseLength = 0;

  memset (&Packet, 0, sizeof (Packet));
  memset (Cdb, 0, sizeof (Cdb));
  memset (Data, 0x53, sizeof (Data));
  Cdb[0] = 0x28;
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.InDataBuffer = Data;
  Packet.InTransferLength = sizeof (Data);

  // A short successful Map is rejected and its temporary mapping is released.
  DmaBefore = ActiveDmaFor (Device);
  MapsBefore = ActiveMappingsFor (Device);
  Device->ShortMapOnCall = Device->MapCalls + 2;
  CHECK (EFI_ERROR (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL)));
  Device->ShortMapOnCall = 0;
  CHECK (ActiveDmaFor (Device) == DmaBefore);
  CHECK (ActiveMappingsFor (Device) == MapsBefore);
  CHECK (Data[0] == 0x53);

  // A failed Map may still return a mapping. Two failed Unmaps must retain it
  // with the command descriptor until Stop can safely retry cleanup.
  MapCall = Device->MapCalls + 2;
  Device->FailMapAfterCall = MapCall;
  FailUnmapCount = 2;
  CHECK (EFI_ERROR (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL)));
  Device->FailMapAfterCall = 0;
  CHECK (ActiveDmaFor (Device) > DmaBefore);
  CHECK (ActiveMappingsFor (Device) > MapsBefore);
  CHECK (Data[0] == 0x53);
  FailUnmapCount = 0;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);

  // A command-buffer FreeBuffer failure keeps the remaining owner for Stop.
  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (&Packet, 0, sizeof (Packet));
  memset (Cdb, 0, sizeof (Cdb));
  memset (Data, 0x29, sizeof (Data));
  Cdb[0] = 0x28;
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.InDataBuffer = Data;
  Packet.InTransferLength = sizeof (Data);
  DmaBefore = ActiveDmaFor (Device);
  FailFreeBufferCount = 1;
  CHECK (EFI_ERROR (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, NULL)));
  CHECK (ActiveDmaFor (Device) > DmaBefore);
  CHECK (Data[0] == 0xa7);
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestAsyncSlotReservationAndBounce (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packets[32];
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET ExtraPacket;
  UINT8                                     Target[TARGET_MAX_BYTES];
  UINT8                                     ExtraCdb[10];
  UINT8                                     Cdb[32][10];
  _Alignas(16) UINT8                        Data[32][TEST_TRANSFER_SIZE];
  EFI_EVENT                                 CallerEvents[32];
  FAKE_EVENT                                *ExtraEvent;
  UFS_PASS_THRU_PRIVATE_DATA                *Private;
  UINT32                                    SeenSlots;
  UINT8                                     Flag;
  UINT8                                     Slot;
  UINTN                                     Index;
  UINTN                                     ByteIndex;
  UINTN                                     NopCount;
  UINTN                                     QueryCount;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
  memset (Target, 0, sizeof (Target));
  memset (Packets, 0, sizeof (Packets));
  memset (Cdb, 0, sizeof (Cdb));
  SeenSlots = 0;

  for (Index = 0; Index < ARRAY_SIZE (Packets); Index++) {
    CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NULL, NULL, &CallerEvents[Index]) == EFI_SUCCESS);
    Packets[Index].Timeout = 100000;
    Packets[Index].Cdb = Cdb[Index];
    Packets[Index].CdbLength = sizeof (Cdb[Index]);
    if ((Index & 1) == 0) {
      Cdb[Index][0] = 0x28;
      memset (Data[Index], 0x19, sizeof (Data[Index]));
      Packets[Index].DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
      Packets[Index].InDataBuffer = Data[Index];
      Packets[Index].InTransferLength = sizeof (Data[Index]);
    } else {
      Cdb[Index][0] = 0x2a;
      memset (Data[Index], (int)(0x30 + Index), sizeof (Data[Index]));
      Packets[Index].DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_WRITE;
      Packets[Index].OutDataBuffer = Data[Index];
      Packets[Index].OutTransferLength = sizeof (Data[Index]);
    }

    CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packets[Index], CallerEvents[Index]) == EFI_SUCCESS);
    Slot = QueuedSlotForEvent (Private, CallerEvents[Index]);
    CHECK (Slot == Index);
    CHECK (Device->LastDoorbellSlot == Slot);
    CHECK ((SeenSlots & ((UINT32)1U << Slot)) == 0);
    SeenSlots |= (UINT32)1U << Slot;
    CHECK (Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] == 0);
    if ((Index & 1) != 0) {
      CHECK (Device->LastWriteDataLength == sizeof (Data[Index]));
      CHECK (memcmp (Device->LastWriteData, Data[Index], sizeof (Data[Index])) == 0);
    }

    if (Index == 0) {
      QueryCount = Device->QueryCommands;
      Flag = 0;
      CHECK (Device->DeviceConfig->RwUfsFlag (Device->DeviceConfig, TRUE, UfsFlagDevInit, &Flag) == EFI_SUCCESS);
      CHECK (Device->LastDoorbellSlot != Slot);
      NopCount = Device->NopCommands;
      CHECK (UfsExecNopCmds (Private) == EFI_SUCCESS);
      CHECK (Device->LastDoorbellSlot != Slot);
      CHECK (Device->NopCommands == NopCount + 1);
      CHECK (Device->QueryCommands > QueryCount);
    }
  }

  CHECK (SeenSlots == 0xffffffffU);
  CHECK (Device->ScsiCommands == 32);
  NopCount = Device->NopCommands;
  QueryCount = Device->QueryCommands;
  Flag = 0;
  CHECK (Device->DeviceConfig->RwUfsFlag (Device->DeviceConfig, TRUE, UfsFlagDevInit, &Flag) == EFI_NOT_READY);
  CHECK (UfsExecNopCmds (Private) == EFI_NOT_READY);
  CHECK (Device->NopCommands == NopCount);
  CHECK (Device->QueryCommands == QueryCount);

  CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NULL, NULL, (EFI_EVENT *)&ExtraEvent) == EFI_SUCCESS);
  memset (&ExtraPacket, 0, sizeof (ExtraPacket));
  memset (ExtraCdb, 0, sizeof (ExtraCdb));
  ExtraPacket.Timeout = 100000;
  ExtraPacket.Cdb = ExtraCdb;
  ExtraPacket.CdbLength = sizeof (ExtraCdb);
  ExtraPacket.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &ExtraPacket, (EFI_EVENT)ExtraEvent) == EFI_NOT_READY);
  CHECK (ExtraEvent->SignalCount == 0);

  for (Index = 0; Index < ARRAY_SIZE (Packets); Index++) {
    CHECK (CallerEvents[Index] != NULL);
    CHECK (((FAKE_EVENT *)CallerEvents[Index])->SignalCount == 0);
  }

  RunTimerAtNotify (Device);
  CHECK (Private->ReservedTrlSlots == 0);
  for (Index = 0; Index < ARRAY_SIZE (Packets); Index++) {
    CHECK (((FAKE_EVENT *)CallerEvents[Index])->SignalCount == 1);
    if ((Index & 1) == 0) {
      for (ByteIndex = 0; ByteIndex < sizeof (Data[Index]); ByteIndex++) {
        CHECK (Data[Index][ByteIndex] == 0xa7);
      }
    } else {
      for (ByteIndex = 0; ByteIndex < sizeof (Data[Index]); ByteIndex++) {
        CHECK (Data[Index][ByteIndex] == (UINT8)(0x30 + Index));
      }
    }
  }

  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestAsyncSelectionReentry (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET OuterPacket;
  FAKE_EVENT                                *OuterEvent;
  FAKE_EVENT                                *NestedEvent;
  UINT8                                     Target[TARGET_MAX_BYTES];
  UINT8                                     OuterCdb[10];
  _Alignas(16) UINT8                        OuterData[TEST_TRANSFER_SIZE];
  _Alignas(16) UINT8                        NestedData[TEST_TRANSFER_SIZE];
  UFS_PASS_THRU_PRIVATE_DATA                *Private;
  UINT8                                     OuterSlot;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (Target, 0, sizeof (Target));
  memset (&OuterPacket, 0, sizeof (OuterPacket));
  memset (OuterCdb, 0, sizeof (OuterCdb));
  memset (OuterData, 0x22, sizeof (OuterData));
  memset (NestedData, 0x33, sizeof (NestedData));
  OuterCdb[0] = 0x28;
  OuterPacket.Timeout = 100000;
  OuterPacket.Cdb = OuterCdb;
  OuterPacket.CdbLength = sizeof (OuterCdb);
  OuterPacket.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  OuterPacket.InDataBuffer = OuterData;
  OuterPacket.InTransferLength = sizeof (OuterData);
  CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NULL, NULL, (EFI_EVENT *)&OuterEvent) == EFI_SUCCESS);
  CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NULL, NULL, (EFI_EVENT *)&NestedEvent) == EFI_SUCCESS);

  ReentrantExt = Device->ExtScsi;
  ReenterScsiEvent = (EFI_EVENT)NestedEvent;
  memset (&ReentrantPacket, 0, sizeof (ReentrantPacket));
  memset (ReentrantCdb, 0, sizeof (ReentrantCdb));
  memset (ReentrantTarget, 0, sizeof (ReentrantTarget));
  ReentrantCdb[0] = 0x28;
  ReentrantPacket.Timeout = 100000;
  ReentrantPacket.Cdb = ReentrantCdb;
  ReentrantPacket.CdbLength = sizeof (ReentrantCdb);
  ReentrantPacket.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  ReentrantPacket.InDataBuffer = NestedData;
  ReentrantPacket.InTransferLength = sizeof (NestedData);

  Device->ReenterScsiReadOffset = UFS_HC_UTRLDBR_OFFSET;
  Device->ReenterScsiReadOccurrence = Device->ReadCount[UFS_HC_UTRLDBR_OFFSET / 4] + 1;
  Device->ReenterScsiOnReadArmed = TRUE;
  ReentrantScsiStatus = EFI_SUCCESS;
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &OuterPacket, (EFI_EVENT)OuterEvent) == EFI_SUCCESS);
  CHECK (!Device->ReenterScsiOnReadArmed);
  CHECK (ReentrantScsiStatus == EFI_NOT_READY);

  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
  OuterSlot = QueuedSlotForEvent (Private, (EFI_EVENT)OuterEvent);
  CHECK (OuterSlot < 32);
  CHECK (QueuedSlotForEvent (Private, (EFI_EVENT)NestedEvent) == MAX_UINT8);
  RunTimerAtNotify (Device);
  CHECK (OuterEvent->SignalCount == 1);
  CHECK (NestedEvent->SignalCount == 0);
  CHECK (OuterData[0] == 0xa7);
  CHECK (NestedData[0] == 0x33);
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestSyncTimeoutCallerLifetime (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet;
  UINT8                                     *Target;
  UINT8                                     *Cdb;
  UINT8                                     *Data;
  EFI_STATUS                                Status;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  Packet = calloc (1, sizeof (*Packet));
  Target = calloc (TARGET_MAX_BYTES, sizeof (*Target));
  Cdb = calloc (10, sizeof (*Cdb));
  CHECK (Packet != NULL && Target != NULL && Cdb != NULL);
  CHECK (posix_memalign ((VOID **)&Data, 16, 512) == 0);
  memset (Data, 0x52, 512);
  Cdb[0] = 0x28;
  Packet->Timeout = 2000;
  Packet->Cdb = Cdb;
  Packet->CdbLength = 10;
  Packet->DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet->InDataBuffer = Data;
  Packet->InTransferLength = 512;
  Device->HoldTransferDbrOnStart = TRUE;
  Device->HoldTransferDbrOnStop = TRUE;

  Status = Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, Packet, NULL);
  CHECK (EFI_ERROR (Status));
  CHECK (ActiveDmaFor (Device) > 2);
  CHECK (ActiveMappingsFor (Device) > 2);
  CHECK (Data[0] == 0x52);

  // The caller can immediately release every pointer after a synchronous
  // failure. A later Stop retry must only touch the driver's retained DMA.
  free (Packet);
  free (Target);
  free (Cdb);
  free (Data);
  Device->HoldTransferDbrOnStart = FALSE;
  Device->HoldTransferDbrOnStop = FALSE;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestQueryAndNopCallerLifetime (
  VOID
  )
{
  FAKE_DEVICE  *Device;
  UINT8        *Descriptor;
  UINT32       DescriptorSize;
  EFI_STATUS   Status;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  Descriptor = malloc (sizeof (UFS_DEV_DESC));
  CHECK (Descriptor != NULL);
  memset (Descriptor, 0x61, sizeof (UFS_DEV_DESC));
  DescriptorSize = sizeof (UFS_DEV_DESC);
  Device->HoldTransferDbrOnStart = TRUE;
  Device->FailReadOffset = UFS_HC_UTRLDBR_OFFSET;
  Device->FailReadOccurrence = Device->ReadCount[UFS_HC_UTRLDBR_OFFSET / 4] + 2;
  Device->FailReadPersistent = TRUE;
  Status = Device->DeviceConfig->RwUfsDescriptor (
                                  Device->DeviceConfig,
                                  TRUE,
                                  UfsDeviceDesc,
                                  0,
                                  0,
                                  Descriptor,
                                  &DescriptorSize
                                  );
  CHECK (EFI_ERROR (Status));
  CHECK (ActiveDmaFor (Device) > 2);
  free (Descriptor);
  Device->FailReadOccurrence = 0;
  Device->FailReadPersistent = FALSE;
  Device->HoldTransferDbrOnStart = FALSE;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);

  // Fail the Start NOP after its doorbell write, then interrupt Start cleanup
  // once so Stop resumes it after the NOP helper's stack frame is gone.
  ResetHarness ();
  Device = &Devices[0];
  Device->HoldTransferDbrOnStart = TRUE;
  Device->FailReadOffset = UFS_HC_UTRLDBR_OFFSET;
  Device->FailReadOccurrence = 2;
  Device->FailReadPersistent = TRUE;
  FailUnmapCount = 1;
  CHECK (EFI_ERROR (StartDevice (Device)));
  CHECK (ActiveDmaFor (Device) != 0);
  CHECK (Device->HostOpened);
  Device->FailReadOccurrence = 0;
  Device->FailReadPersistent = FALSE;
  Device->HoldTransferDbrOnStart = FALSE;
  CHECK (EFI_ERROR (StopDevice (Device)));
  CHECK (FailUnmapCount == 0);
  CHECK (ActiveDmaFor (Device) != 0);
  CHECK (ActiveMappingsFor (Device) != 0);
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static
VOID
TestAsyncAbortNoCallerCopyAndReentry (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  UINT8                                     Target[TARGET_MAX_BYTES];
  UINT8                                     Cdb[10];
  _Alignas(16) UINT8                        Data[512];
  FAKE_EVENT                                *CallerEvent;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (&Packet, 0, sizeof (Packet));
  memset (Target, 0, sizeof (Target));
  memset (Cdb, 0, sizeof (Cdb));
  Cdb[0] = 0x28;
  memset (Data, 0x3c, sizeof (Data));
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.InDataBuffer = Data;
  Packet.InTransferLength = sizeof (Data);
  CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ReenterFromCallerEvent, Device, (EFI_EVENT *)&CallerEvent) == EFI_SUCCESS);
  Device->HoldTransferDbrOnStart = TRUE;
  ReenterFromSignal = TRUE;
  ReentrantExt = Device->ExtScsi;
  ReentrantPacket = Packet;
  ReentrantPacket.Cdb = ReentrantCdb;
  ReentrantPacket.CdbLength = sizeof (ReentrantCdb);
  memset (ReentrantTarget, 0, sizeof (ReentrantTarget));

  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, (EFI_EVENT)CallerEvent) == EFI_SUCCESS);
  CHECK (ActiveMappingsFor (Device) >= 3);
  CHECK (Data[0] == 0x3c);
  FailUnmapCount = 1;
  CHECK (EFI_ERROR (StopDevice (Device)));
  CHECK (CallerEvent->SignalCount == 1);
  CHECK (CallerSignalCount == 1);
  CHECK (ReentrantStopStatus == EFI_NOT_READY);
  CHECK (ReentrantCallStatus == EFI_NOT_READY);
  CHECK (Data[0] == 0x3c);
  CHECK (ActiveDmaFor (Device) != 0);

  Device->HoldTransferDbrOnStart = FALSE;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CHECK (CallerEvent->SignalCount == 1);
  CHECK (Data[0] == 0x3c);
  CheckClean (Device);
}

static
VOID
TestAsyncTimerTimeoutAndReadFault (
  VOID
  )
{
  FAKE_DEVICE                               *Device;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  UINT8                                     Target[TARGET_MAX_BYTES];
  UINT8                                     Cdb[10];
  _Alignas(16) UINT8                        Data[512];
  FAKE_EVENT                                *CallerEvent;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (&Packet, 0, sizeof (Packet));
  memset (Target, 0, sizeof (Target));
  memset (Cdb, 0, sizeof (Cdb));
  Cdb[0] = 0x28;
  memset (Data, 0x69, sizeof (Data));
  Packet.Timeout = UFS_HC_ASYNC_TIMER;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.InDataBuffer = Data;
  Packet.InTransferLength = sizeof (Data);
  CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NULL, NULL, (EFI_EVENT *)&CallerEvent) == EFI_SUCCESS);
  Device->HoldTransferDbrOnStart = TRUE;
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, (EFI_EVENT)CallerEvent) == EFI_SUCCESS);
  RunTimerAtNotify (Device);
  CHECK (Device->LastDoorbellReadTpl == TPL_NOTIFY);
  CHECK (CallerEvent->SignalCount == 0);
  CHECK (Data[0] == 0x69);
  RunTimerAtNotify (Device);
  CHECK (CallerEvent->SignalCount == 0);
  Device->HoldTransferDbrOnStart = FALSE;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CHECK (CallerEvent->SignalCount == 1);
  CheckClean (Device);

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  memset (&Packet, 0, sizeof (Packet));
  memset (Target, 0, sizeof (Target));
  memset (Cdb, 0, sizeof (Cdb));
  Packet.Timeout = 100000;
  Packet.Cdb = Cdb;
  Packet.CdbLength = sizeof (Cdb);
  CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NULL, NULL, (EFI_EVENT *)&CallerEvent) == EFI_SUCCESS);
  Device->HoldTransferDbrOnStart = TRUE;
  CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, (EFI_EVENT)CallerEvent) == EFI_SUCCESS);
  Device->FailReadOffset = UFS_HC_UTRLDBR_OFFSET;
  Device->FailReadOccurrence = Device->ReadCount[UFS_HC_UTRLDBR_OFFSET / 4] + 1;
  RunTimerAtNotify (Device);
  CHECK (Device->LastDoorbellReadTpl == TPL_NOTIFY);
  CHECK (CallerEvent->SignalCount == 0);
  Device->FailReadOccurrence = 0;
  Device->HoldTransferDbrOnStart = FALSE;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CHECK (CallerEvent->SignalCount == 1);
  CheckClean (Device);
}

static VOID TestDiscoveryAfterMissingLuns (VOID) {
  FAKE_DEVICE *Device;
  UFS_PASS_THRU_PRIVATE_DATA *Private;
  UINTN Mode;
  ResetHarness ();
  Device = &Devices[0];
  Device->EnabledLuns = BIT2;
  Device->FailedUnitDescriptors = (UINT8)~BIT2;
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
  CHECK (Device->UnitRequested[2] == sizeof (UFS_UNIT_DESC));
  CHECK ((Private->Luns.BitMask & 0xff) == BIT2);
  Device->FailedUnitDescriptors = 0;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
  for (Mode = 1; Mode <= 6; Mode++) {
    ResetHarness ();
    Device = &Devices[0];
    Device->EnabledLuns = BIT2;
    Device->MalformedUnitDescriptor = (UINT8)Mode;
    CHECK (StartDevice (Device) == EFI_SUCCESS);
    Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
    CHECK ((Private->Luns.BitMask & 0xff) == 0);
    Device->MalformedUnitDescriptor = 0;
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CheckClean (Device);
  }
}

static VOID TestDiscoveryHpbUserLun (VOID) {
  FAKE_DEVICE *Device;
  UFS_PASS_THRU_PRIVATE_DATA *Private;
  UINT8 Target[TARGET_MAX_BYTES], *TargetPointer;
  UINT64 Lun;
  UINTN Index;
  ResetHarness ();
  Device = &Devices[0];
  Device->EnabledLuns = BIT0 | BIT1 | BIT2;
  Device->HpbLuns = BIT2; // Exact lu_enable values observed on stock Android.
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
  CHECK ((Private->Luns.BitMask & 0xff) == (BIT0 | BIT1 | BIT2));
  memset (Target, 0xff, sizeof (Target));
  TargetPointer = Target;
  Lun = 0;
  for (Index = 0; Index < 3; Index++) {
    CHECK (Device->ExtScsi->GetNextTargetLun (Device->ExtScsi, &TargetPointer, &Lun) == EFI_SUCCESS);
    CHECK (Lun == ((UINT64)Index << 8));
  }
  CHECK (Device->ExtScsi->GetNextTargetLun (Device->ExtScsi, &TargetPointer, &Lun) == EFI_NOT_FOUND);
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

static VOID TestPoweredStopLifecycle (VOID);
static VOID TestPoweredStopDrainsAsync (VOID);
static VOID TestPowerDownUncertainResponse (VOID);
static VOID TestPowerDownSubmissionFailures (VOID);

static
VOID
TestMain (
  VOID
  )
{
  TestStartSynchronousAndStop ();
  TestDiscoveryAfterMissingLuns ();
  TestDiscoveryHpbUserLun ();
  TestPoweredStopLifecycle ();
  TestPoweredStopDrainsAsync ();
  TestPowerDownUncertainResponse ();
  TestPowerDownSubmissionFailures ();
  TestStopRetryFailures ();
  TestStartFailureAndRetry ();
  TestSynchronousReentryAndTwoControllers ();
  TestTplGuards ();
  TestSynchronousDataBounce ();
  TestScsiResponseAndMappingFailures ();
  TestAsyncSlotReservationAndBounce ();
  TestAsyncSelectionReentry ();
  TestSyncTimeoutCallerLifetime ();
  TestQueryAndNopCallerLifetime ();
  TestAsyncAbortNoCallerCopyAndReentry ();
  TestAsyncTimerTimeoutAndReadFault ();
}

static
FAKE_DEVICE *
StartPowerStopDevice (
  VOID
  )
{
  FAKE_DEVICE *Device;
  ResetHarness ();
  Device = &Devices[0];
  Device->LocalPlatformAvailable = TRUE;
  Device->LocalPlatform.Version = EDKII_UFS_HC_PLATFORM_PROTOCOL_STOP_VERSION;
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  CHECK ((UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi))->DeviceInitializationComplete);
  return Device;
}

static
VOID
TestPoweredStopLifecycle (
  VOID
  )
{
  FAKE_DEVICE *Device;
  UFS_PASS_THRU_PRIVATE_DATA *Private;
  UINTN Before;
  UINTN Mode;
  EFI_STATUS Status;

  Device = StartPowerStopDevice ();
  // A LU missed by the Start discovery mask must still be flushed at Stop.
  Device->EnabledLuns |= BIT2;
  Device->HpbLuns = BIT2;
  Device->BgOpsFlag = 1;
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CHECK (Device->CacheCommands[0] == 1 && Device->CacheCommands[2] == 1);
  CHECK (Device->BgOpsFlag == 0);
  CHECK (Device->PowerDownCommands == 1);
  CHECK (Device->CommandsAfterPowerDown == 0);
  CHECK (Device->PrepareStopOrder > Device->PowerDownOrder);
  CHECK (Device->HceStopOrder > Device->PrepareStopOrder);
  CHECK (Device->HostStoppedOrder > Device->HceStopOrder);
  CheckClean (Device);

  // Every failure before successful POWERDOWN keeps the controller usable
  // for a retry, rather than using HCE0 to hide failed device quiescence.
  for (Mode = 0; Mode < 9; Mode++) {
    Device = StartPowerStopDevice ();
    Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
    Before = Device->WriteCount[UFS_HC_ENABLE_OFFSET / 4];
    if (Mode == 0) Device->FailScsiOpcode = 0x35;
    if (Mode == 1) Device->FailScsiOpcode = 0x1b;
    if (Mode >= 2 && Mode < 7) Device->MalformedUnitDescriptor = (UINT8)(Mode - 1);
    if (Mode == 7) Device->BgOpsFlag = 2;
    if (Mode == 8) { Device->BgOpsFlag = 1; Device->RefuseBgOpsClear = TRUE; }
    Status = StopDevice (Device);
    CHECK (EFI_ERROR (Status));
    CHECK (!Private->DevicePowerDownComplete && !Private->PlatformStopPrepared);
    CHECK (Device->PlatformCalls[EdkiiUfsHcPrepareStop] == 0);
    CHECK (Device->WriteCount[UFS_HC_ENABLE_OFFSET / 4] == Before);
    CHECK (Device->HostOpened && Device->Mmio[UFS_HC_ENABLE_OFFSET / 4] == 1);
    Device->FailScsiOpcode = 0;
    Device->MalformedUnitDescriptor = 0;
    Device->RefuseBgOpsClear = FALSE;
    Device->BgOpsFlag = 0;
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CHECK (Device->CommandsAfterPowerDown == 0);
    CheckClean (Device);
  }

  for (Mode = 0; Mode < 5; Mode++) {
    Device = StartPowerStopDevice ();
    Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
    if (Mode == 0) Device->FailPowerDownCleanup = TRUE;
    if (Mode == 1) Device->FailPrepareStopCount = 1;
    if (Mode == 2) Device->FailHostStoppedCount = 1;
    if (Mode == 3) Device->FailPowerDownFlushCount = 1;
    if (Mode == 4) Device->TimeoutPowerDown = TRUE;
    CHECK (EFI_ERROR (StopDevice (Device)));
    if (Mode < 3) {
      CHECK (Private->DevicePowerDownComplete);
      CHECK (!Private->DevicePowerDownUncertain);
    } else {
      CHECK (!Private->DevicePowerDownComplete);
      CHECK (Private->DevicePowerDownUncertain);
      CHECK (!IsListEmpty (&Private->Queue));
      CHECK (Device->PlatformCalls[EdkiiUfsHcPrepareStop] == 0);
    }
    CHECK (Device->PowerDownCommands == 1);
    if (Mode == 0) {
      CHECK (!IsListEmpty (&Private->Queue));
      CHECK (Device->PlatformCalls[EdkiiUfsHcPrepareStop] == 0);
    }
    if (Mode == 2) {
      CHECK (Private->ControllerStopComplete && Private->PlatformStopPrepared);
      CHECK (!Private->PlatformStopComplete);
    }
    {
      EDKII_UIC_COMMAND Command;
      ZeroMem (&Command, sizeof (Command));
      Command.Opcode = UfsUicDmeHibernateEnter;
      CHECK (!Private->StopUicAllowed);
      CHECK (Private->UfsHcDriverInterface.UfsExecUicCommand (
               &Private->UfsHcDriverInterface, &Command) == EFI_NOT_READY);
    }
    Before = Device->WriteCount[UFS_HC_ENABLE_OFFSET / 4];
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CHECK (Device->PowerDownCommands == 1);
    CHECK (Device->CommandsAfterPowerDown == 0);
    if (Mode == 2) CHECK (Device->WriteCount[UFS_HC_ENABLE_OFFSET / 4] == Before);
    CheckClean (Device);
  }
}

static
VOID
TestPowerDownUncertainResponse (
  VOID
  )
{
  FAKE_DEVICE *Device;
  UFS_PASS_THRU_PRIVATE_DATA *Private;
  UFS_PASS_THRU_TRANS_REQ *Request;
  UTP_RESPONSE_UPIU *Response;
  UINTN Mode;
  UINTN BeforeDma;

  for (Mode = 1; Mode <= 10; Mode++) {
    Device = StartPowerStopDevice ();
    Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
    Device->MalformedPowerDownResponse = (UINT8)Mode;
    BeforeDma = ActiveDmaFor (Device);
    CHECK (StopDevice (Device) == EFI_DEVICE_ERROR);
    CHECK (Private->DevicePowerDownUncertain && !Private->DevicePowerDownComplete);
    CHECK (!IsListEmpty (&Private->Queue));
    CHECK (ActiveDmaFor (Device) > BeforeDma);
    CHECK (Device->PowerDownCommands == 1);
    CHECK (StopDevice (Device) == EFI_NOT_READY);
    CHECK (Device->PowerDownCommands == 1 && Device->CommandsAfterPowerDown == 0);
    CHECK (Private->DevicePowerDownUncertain && !Private->DevicePowerDownComplete);
    CHECK (Device->PlatformCalls[EdkiiUfsHcPrepareStop] == 0);
    Request = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (GetFirstNode (&Private->Queue));
    CHECK (Request->InternalPowerDown && Request->Packet == NULL && Request->CallerEvent == NULL);
    Response = (UTP_RESPONSE_UPIU *)((UINT8 *)Request->CmdDescHost +
                                     Request->Trd->RuO * sizeof (UINT32));
    Response->TransCode = 0x21;
    Response->TaskTag = ((UTP_COMMAND_UPIU *)Request->CmdDescHost)->TaskTag;
    Request->Trd->Ocs = 0;
    Request->Trd->RuL = ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)) / sizeof (UINT32);
    Response->Lun = ((UTP_COMMAND_UPIU *)Request->CmdDescHost)->Lun;
    Response->EhsLen = 0;
    Response->DataSegLen = 0;
    Response->SenseDataLen = 0;
    Response->Flags = 0;
    Response->ResTranCount = 0;
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CHECK (Device->PowerDownCommands == 1 && Device->CommandsAfterPowerDown == 0);
    CheckClean (Device);
  }
  for (Mode = 11; Mode <= 12; Mode++) {
    Device = StartPowerStopDevice ();
    Device->MalformedPowerDownResponse = (UINT8)Mode;
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CHECK (Device->PowerDownCommands == 1 && Device->CommandsAfterPowerDown == 0);
    CheckClean (Device);
  }
}

static
VOID
TestPowerDownSubmissionFailures (
  VOID
  )
{
  FAKE_DEVICE *Device;
  UFS_PASS_THRU_PRIVATE_DATA *Private;
  UFS_PASS_THRU_TRANS_REQ *Request;
  UINTN Mode;

  for (Mode = 1; Mode <= 3; Mode++) {
    Device = StartPowerStopDevice ();
    Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
    Device->PowerDownSubmitFault = (UINT8)Mode;
    CHECK (StopDevice (Device) == EFI_DEVICE_ERROR);
    CHECK (Private->DevicePowerDownUncertain && !Private->DevicePowerDownComplete);
    CHECK (!IsListEmpty (&Private->Queue));
    Request = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (GetFirstNode (&Private->Queue));
    CHECK (Request->InternalPowerDown);
    CHECK ((BOOLEAN)Request->DoorbellAttempted == (BOOLEAN)(Mode == 3));
    CHECK (Device->PowerDownCommands == (UINTN)(Mode == 3));
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CHECK (Device->PowerDownCommands == 1 && Device->CommandsAfterPowerDown == 0);
    CheckClean (Device);
  }
}

static
VOID
TestPoweredStopDrainsAsync (
  VOID
  )
{
  FAKE_DEVICE *Device;
  UFS_PASS_THRU_PRIVATE_DATA *Private;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet;
  EFI_EVENT Caller;
  UINT8 Target[TARGET_MAX_BYTES];
  UINT8 Cdb[6];
  UINT8 Slot;
  UINTN Before;
  UINTN Mode;

  for (Mode = 0; Mode < 2; Mode++) {
    Device = StartPowerStopDevice ();
    Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_THIS (Device->ExtScsi);
    ZeroMem (&Packet, sizeof (Packet));
    ZeroMem (Target, sizeof (Target));
    ZeroMem (Cdb, sizeof (Cdb));
    Packet.Timeout = UFS_TIMEOUT;
    Packet.Cdb = Cdb;
    Packet.CdbLength = sizeof (Cdb);
    Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
    CHECK (FakeCreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NULL, NULL, &Caller) == EFI_SUCCESS);
    Device->HoldTransferDbrOnStart = (BOOLEAN)(Mode == 1);
    CHECK (Device->ExtScsi->PassThru (Device->ExtScsi, Target, 0, &Packet, Caller) == EFI_SUCCESS);
    Slot = QueuedSlotForEvent (Private, Caller);
    CHECK (Slot < 32);
    Device->HoldTransferDbrOnStart = FALSE;
    Before = Device->WriteCount[UFS_HC_ENABLE_OFFSET / 4];
    if (Mode == 1) {
      CHECK (StopDevice (Device) == EFI_NOT_READY);
      CHECK (Device->PowerDownCommands == 0);
      CHECK (Device->WriteCount[UFS_HC_ENABLE_OFFSET / 4] == Before);
      CompleteTransfer (Device, Slot);
      Device->Mmio[UFS_HC_UTRLDBR_OFFSET / 4] &= ~((UINT32)1U << Slot);
    }
    CHECK (StopDevice (Device) == EFI_SUCCESS);
    CHECK (((FAKE_EVENT *)Caller)->SignalCount == 1);
    CHECK (Device->PowerDownCommands == 1);
    CHECK (Device->CommandsAfterPowerDown == 0);
    CHECK (FakeCloseEvent (Caller) == EFI_SUCCESS);
    CheckClean (Device);
  }
}

static void
TestQueryResponseValidation (void)
{
  FAKE_DEVICE *Device;
  UINT8 Descriptor[sizeof (UFS_DEV_DESC)];
  UINT8 LongDescriptor[255];
  UINT8 Flag;
  UINT32 Attribute;
  UINT32 AttrSize;
  UINT32 Size;
  UINTN BeforeDma, BeforeMappings, BeforeQueries;

  ResetHarness ();
  Device = &Devices[0];
  CHECK (StartDevice (Device) == EFI_SUCCESS);
  BeforeDma = ActiveDmaFor (Device);
  BeforeMappings = ActiveMappingsFor (Device);
  for (UINT8 Mode = 1; Mode <= 13; ++Mode) {
    Device->MalformedQueryResponse = Mode;
    memset (Descriptor, 0xa5, sizeof (Descriptor));
    Size = sizeof (Descriptor);
    BeforeQueries = Device->QueryCommands;
    CHECK (Device->DeviceConfig->RwUfsDescriptor (Device->DeviceConfig, TRUE,
             UfsDeviceDesc, 0, 0, Descriptor, &Size) == EFI_DEVICE_ERROR);
    CHECK (Size == 0 && Device->QueryCommands == BeforeQueries + 5);
    for (UINTN I = 0; I < sizeof (Descriptor); ++I) CHECK (Descriptor[I] == 0xa5);
    CHECK (ActiveDmaFor (Device) == BeforeDma);
    CHECK (ActiveMappingsFor (Device) == BeforeMappings);
  }
  // A spurious data segment or length must not publish flag/attribute values.
  for (UINT8 Mode = 9; Mode <= 10; ++Mode) {
    Device->MalformedQueryResponse = Mode;
    Flag = 0xa5;
    CHECK (Device->DeviceConfig->RwUfsFlag (Device->DeviceConfig, TRUE,
             UfsFlagBgOpsEn, &Flag) == EFI_DEVICE_ERROR && Flag == 0xa5);
    Attribute = 0xa5a5a5a5;
    AttrSize = sizeof (Attribute);
    CHECK (Device->DeviceConfig->RwUfsAttribute (Device->DeviceConfig, TRUE,
             0, 0, 0, (UINT8 *)&Attribute, &AttrSize) == EFI_DEVICE_ERROR &&
           Attribute == 0xa5a5a5a5 && AttrSize == 0);
  }
  Device->MalformedQueryResponse = 0;
  Size = sizeof (Descriptor);
  CHECK (Device->DeviceConfig->RwUfsDescriptor (Device->DeviceConfig, TRUE,
           UfsDeviceDesc, 0, 0, Descriptor, &Size) == EFI_SUCCESS);
  CHECK (Size == sizeof (Descriptor) && Descriptor[0] == sizeof (UFS_DEV_DESC));
  // A legal short descriptor must not be rejected or overwrite its unused tail.
  memset (LongDescriptor, 0xa5, sizeof (LongDescriptor));
  Size = sizeof (LongDescriptor);
  BeforeQueries = Device->QueryCommands;
  CHECK (Device->DeviceConfig->RwUfsDescriptor (Device->DeviceConfig, TRUE,
           UfsUnitDesc, 2, 0, LongDescriptor, &Size) == EFI_SUCCESS);
  CHECK (Device->UnitRequested[2] == 255 && Device->QueryCommands == BeforeQueries + 1);
  CHECK (Size == 45 && Size == sizeof (UFS_UNIT_DESC));
  CHECK (LongDescriptor[0] == 45 && LongDescriptor[1] == UfsUnitDesc && LongDescriptor[2] == 2);
  for (UINTN I = Size; I < sizeof (LongDescriptor); ++I) CHECK (LongDescriptor[I] == 0xa5);
  CHECK (ActiveDmaFor (Device) == BeforeDma && ActiveMappingsFor (Device) == BeforeMappings);
  CHECK (StopDevice (Device) == EFI_SUCCESS);
  CheckClean (Device);
}

int
main (
  VOID
  )
{
  UINTN  Index;

  TestMain ();
  TestQueryResponseValidation ();
  for (Index = 0; Index < EventCount; Index++) {
    free (Events[Index]);
    Events[Index] = NULL;
  }

  printf ("UfsPassThruTest: %u checks passed\n", Checks);
  return 0;
}

// Host-side EDK II library shims used by the included production sources.
EFI_TPL EFIAPI EfiGetCurrentTpl (VOID) { return CurrentTpl; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
UINTN EFIAPI DevicePathNodeLength (IN CONST VOID *Node) {
  CONST UINT8 *Bytes = (CONST UINT8 *)Node;
  return (UINTN)Bytes[2] | ((UINTN)Bytes[3] << 8);
}
VOID *EFIAPI SetMem (VOID *Buffer, UINTN Size, UINT8 Value) { return memset (Buffer, Value, Size); }
UINT64 EFIAPI DivU64x32 (UINT64 Dividend, UINT32 Divisor) { return (Divisor == 0) ? 0 : Dividend / Divisor; }
VOID *EFIAPI AllocateZeroPool (UINTN Size) { return calloc (1, Size); }
VOID *EFIAPI AllocateAlignedPages (UINTN Pages, UINTN Alignment) {
  VOID *Buffer = NULL;
  UINTN Align = (Alignment < sizeof (VOID *)) ? sizeof (VOID *) : Alignment;
  if (posix_memalign (&Buffer, Align, Pages * TEST_PAGE_SIZE) != 0) return NULL;
  memset (Buffer, 0, Pages * TEST_PAGE_SIZE);
  return Buffer;
}
VOID EFIAPI FreeAlignedPages (VOID *Buffer, UINTN Pages) { (VOID)Pages; free (Buffer); }
