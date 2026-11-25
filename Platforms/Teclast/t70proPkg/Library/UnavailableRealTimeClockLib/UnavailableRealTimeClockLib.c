// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/RtPropertiesTable.h>
#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MtkRtcCalendar.h>
#include <Protocol/MtkPmicRtcRuntime.h>
#include <Library/RealTimeClockLib.h>
#include "StockRtcRead.h"

// Advertise calendar services once the shared PWRAP runtime owner is available.
static EFI_RT_PROPERTIES_TABLE mProperties;
static EFI_RUNTIME_SERVICES *mRtcRuntimeServices;
static MTK_PMIC_RTC_ACCESS mRtcRuntimeAccess;
static EFI_EVENT mRtcVirtualEvent;
static UINTN mRtcRuntimeBase;
static UINT32 mRtcAllowed;
#define RTC_PW_BASE 0x10026000U
#define RTC_PW_SIZE 0x1000U
#define CALENDAR_SERVICES (EFI_RT_SUPPORTED_GET_TIME | EFI_RT_SUPPORTED_SET_TIME)
#define TIME_SERVICES (EFI_RT_SUPPORTED_GET_TIME | EFI_RT_SUPPORTED_SET_TIME | \
                       EFI_RT_SUPPORTED_GET_WAKEUP_TIME | EFI_RT_SUPPORTED_SET_WAKEUP_TIME)
#define ALL_SERVICES (TIME_SERVICES | EFI_RT_SUPPORTED_GET_VARIABLE | \
  EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME | EFI_RT_SUPPORTED_SET_VARIABLE | \
  EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP | EFI_RT_SUPPORTED_CONVERT_POINTER | \
  EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT | EFI_RT_SUPPORTED_RESET_SYSTEM | \
  EFI_RT_SUPPORTED_UPDATE_CAPSULE | EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES | \
  EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO)

static VOID EFIAPI StockRtcRuntimeReady(EFI_EVENT Event,VOID *Context) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Range;
  MTK_PMIC_RTC_RUNTIME_PROTOCOL *Provider=NULL;
  (VOID)Event;
  if (mRtcEnded) return;
  StockRtcFdtChanged(NULL,Context);
  mRtcRuntimeAccess=NULL;
  mProperties.RuntimeServicesSupported&=~CALENDAR_SERVICES;
  if (!mRtcFdtValid || mRtcWriteFault ||
      EFI_ERROR(gDS->GetMemorySpaceDescriptor(RTC_PW_BASE,&Range)) ||
      (Range.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo &&
       Range.GcdMemoryType!=EfiGcdMemoryTypeReserved) ||
      !(Range.Attributes&EFI_MEMORY_RUNTIME) ||
      Range.BaseAddress>RTC_PW_BASE || Range.Length<RTC_PW_BASE-Range.BaseAddress+RTC_PW_SIZE) return;
  if (EFI_ERROR(mRtcBoot->LocateProtocol(&gMtkPmicRtcRuntimeProtocolGuid,NULL,(VOID **)&Provider)) ||
      !Provider || !Provider->Access) return;
  mRtcRuntimeBase=RTC_PW_BASE;
  mRtcRuntimeAccess=Provider->Access;
  mProperties.RuntimeServicesSupported|=mRtcAllowed;
}

static VOID EFIAPI StockRtcVirtual(EFI_EVENT Event,VOID *Context) {
  VOID *Base=(VOID *)mRtcRuntimeBase;
  MTK_PMIC_RTC_ACCESS Access=mRtcRuntimeAccess;
  (VOID)Event; (VOID)Context;
  if (!Access) return;
  if (EFI_ERROR(mRtcRuntimeServices->ConvertPointer(0,&Base)) ||
      !Base || ((UINTN)Base&EFI_PAGE_MASK) ||
      EFI_ERROR(mRtcRuntimeServices->ConvertPointer(0,(VOID **)&Access)) || !Access) {
    mRtcRuntimeAccess=NULL;
    mRtcRuntimeBase=0;
    mProperties.RuntimeServicesSupported&=~CALENDAR_SERVICES;
    return;
  }
  mRtcRuntimeBase=(UINTN)Base;
  mRtcRuntimeAccess=Access;
}

static VOID StockRtcEnableRuntime(EFI_SYSTEM_TABLE *SystemTable) {
  EFI_EVENT Ready=NULL,Virtual=NULL,BeforeExit=NULL;
  EFI_STATUS Status;
  if (!mRtcBoot || mRtcVirtualEvent || !mRtcAllowed || !mRtcBoot->CloseEvent ||
      !gDS || !gDS->GetMemorySpaceDescriptor ||
      !SystemTable->RuntimeServices || !SystemTable->RuntimeServices->ConvertPointer) return;
  Status=mRtcBoot->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,StockRtcVirtual,NULL,
                               &gEfiEventVirtualAddressChangeGuid,&Virtual);
  if (EFI_ERROR(Status) || !Virtual) return;
  // PRM maps the runtime MMIO at EndOfDxe. Shell boots can skip ReadyToBoot.
  Status=mRtcBoot->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,StockRtcRuntimeReady,SystemTable,
                               &gEfiEventBeforeExitBootServicesGuid,&BeforeExit);
  if (EFI_ERROR(Status) || !BeforeExit) { (VOID)mRtcBoot->CloseEvent(Virtual); return; }
  (VOID)mRtcBoot->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,StockRtcRuntimeReady,SystemTable,
                               &gEfiEventReadyToBootGuid,&Ready);
  mRtcRuntimeServices=SystemTable->RuntimeServices;
  mRtcVirtualEvent=Virtual;
}

EFI_STATUS EFIAPI LibGetTime (OUT EFI_TIME *Time, OUT EFI_TIME_CAPABILITIES *Capabilities) {
  EFI_STATUS Status;
  if (!mRtcEnded) {
    Status=StockRtcGetTime(Time);
  } else {
    if (!mRtcRuntimeAccess || !(mRtcAllowed&EFI_RT_SUPPORTED_GET_TIME)) return EFI_UNSUPPORTED;
    if (!Time) return EFI_INVALID_PARAMETER;
    if (mRtcWriteFault) return EFI_DEVICE_ERROR;
    Status=mRtcRuntimeAccess(mRtcRuntimeBase,FALSE,Time);
  }
  if (!EFI_ERROR(Status) && Capabilities) {
    Capabilities->Resolution=1;
    Capabilities->Accuracy=MAX_UINT32; // No calibrated accuracy is available.
    Capabilities->SetsToZero=FALSE;
  }
  return Status;
}
EFI_STATUS EFIAPI LibSetTime (IN EFI_TIME *Time) {
  if (!mRtcEnded) return StockRtcSetTime(Time);
  if (!mRtcRuntimeAccess || !(mRtcAllowed&EFI_RT_SUPPORTED_SET_TIME)) return EFI_UNSUPPORTED;
  if (mRtcWriteFault) return EFI_DEVICE_ERROR;
  return mRtcRuntimeAccess(mRtcRuntimeBase,TRUE,Time);
}
EFI_STATUS EFIAPI LibGetWakeupTime (OUT BOOLEAN *Enabled, OUT BOOLEAN *Pending, OUT EFI_TIME *Time) {
  (VOID)Enabled; (VOID)Pending; (VOID)Time; return EFI_UNSUPPORTED;
}
EFI_STATUS EFIAPI LibSetWakeupTime (IN BOOLEAN Enabled, IN EFI_TIME *Time) {
  (VOID)Enabled; (VOID)Time; return EFI_UNSUPPORTED;
}
EFI_STATUS EFIAPI LibRtcInitialize (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  UINTN Index;
  EFI_RT_PROPERTIES_TABLE Properties = {EFI_RT_PROPERTIES_TABLE_VERSION, sizeof(Properties), ALL_SERVICES};
  (VOID)ImageHandle;
  if (SystemTable == NULL || SystemTable->BootServices == NULL ||
      SystemTable->BootServices->InstallConfigurationTable == NULL ||
      (SystemTable->NumberOfTableEntries != 0 && SystemTable->ConfigurationTable == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  // Preserve every other provider's unsupported-service bits. Without a table,
  // UEFI's default is that all runtime services are supported.
  for (Index = 0; Index < SystemTable->NumberOfTableEntries; ++Index) {
    EFI_CONFIGURATION_TABLE *Entry = &SystemTable->ConfigurationTable[Index];
    if (CompareGuid (&Entry->VendorGuid, &gEfiRtPropertiesTableGuid)) {
      EFI_RT_PROPERTIES_TABLE *Previous = Entry->VendorTable;
      if (Previous == NULL || Previous->Version != EFI_RT_PROPERTIES_TABLE_VERSION ||
          Previous->Length != sizeof(Properties)) {
        return EFI_COMPROMISED_DATA;
      }
      Properties = *Previous;
      break;
    }
  }
  mRtcAllowed=Properties.RuntimeServicesSupported&CALENDAR_SERVICES;
  Properties.RuntimeServicesSupported &= ~TIME_SERVICES;
  mProperties = Properties;
  // Failure stops the parent RTC driver from publishing an inconsistent service.
  {
    EFI_STATUS Status=SystemTable->BootServices->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, &mProperties);
    if (!EFI_ERROR(Status)) {
      StockRtcEnable(SystemTable);
      StockRtcEnableRuntime(SystemTable);
    }
    return Status;
  }
}
