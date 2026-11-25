// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>

#include <Library/DevicePathLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/EventGroup.h>
#include <Protocol/DevicePath.h>
#include <Protocol/Usb2HostController.h>

#include <Library/MusbHostCore.h>
#include <Protocol/MtkMusbHostPlatformReady.h>

#define MUSB_DXE_SIGNATURE SIGNATURE_32('M', 'D', 'X', 'E')
#define MUSB_ASYNC_MAX_POLL_MS 1000U
#define MUSB_ASYNC_MAX_DATA MUSB_HOST_DATA_FIFO_BYTES
#define MUSB_ASYNC_HIGH_SPEED_TIMEOUT_MS 1U
#define MUSB_ASYNC_FRAME_TIMEOUT_MS 2U
#define MUSB_ASYNC_SLOT_COUNT 16U

typedef struct MUSB_HOST_DXE MUSB_HOST_DXE;

typedef struct {
  MUSB_HOST_DXE             *Owner;
  EFI_EVENT                  AsyncEvent;
  BOOLEAN                    AsyncActive;
  BOOLEAN                    AsyncInCallback;
  UINT64                     AsyncGeneration;
  UINT8                      AsyncAddress;
  UINT8                      AsyncEndpoint;
  UINT8                      AsyncSpeed;
  UINT8                      AsyncToggle;
  MUSB_HOST_ROUTE            AsyncRoute;
  UINT16                     AsyncMaximumPacket;
  UINTN                      AsyncDataLength;
  EFI_ASYNC_USB_TRANSFER_CALLBACK AsyncCallback;
  VOID                       *AsyncCallbackContext;
  UINT8                      AsyncBuffer[MUSB_ASYNC_MAX_DATA];
} MUSB_ASYNC_SLOT;

struct MUSB_HOST_DXE {
  EFI_USB2_HC_PROTOCOL       UsbHc;
  VENDOR_DEVICE_PATH         VendorPath;
  EFI_DEVICE_PATH_PROTOCOL   EndPath;
  MUSB_HOST                  Host;
  EFI_PHYSICAL_ADDRESS       ControllerBase;
  EFI_LOCK                   CoreLock;
  EFI_LOCK                   AsyncLock;
  EFI_EVENT                  ExitBootServicesEvent;
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL *PlatformReady;
  EFI_HANDLE                 ControllerHandle;
  UINT32                     Signature;
  volatile BOOLEAN           ExitingBootServices;
  BOOLEAN                    Stopping;
  BOOLEAN                    ReleaseInProgress;
  BOOLEAN                    ProtocolsInstalled;
  BOOLEAN                    UsbHcInstalled;
  BOOLEAN                    DevicePathInstalled;
  BOOLEAN                    AsyncCloseFailed;
  MUSB_ASYNC_SLOT            AsyncSlots[MUSB_ASYNC_SLOT_COUNT];
};

STATIC EFI_EVENT mReadyNotifyEvent;
STATIC VOID      *mReadyRegistration;
STATIC BOOLEAN   mHostStarted;
STATIC BOOLEAN   mHostInstallBlocked;
STATIC BOOLEAN   mReadyNotifyDisabled;
STATIC BOOLEAN   mKeepImageResident;

STATIC
MUSB_HOST_DXE *
MusbContextFromProtocol (
  IN EFI_USB2_HC_PROTOCOL  *This
  )
{
  if (This == NULL) {
    return NULL;
  }
  return (MUSB_HOST_DXE *)This;
}

STATIC
BOOLEAN
MusbValidContext (
  IN MUSB_HOST_DXE  *Context
  )
{
  return (Context != NULL) &&
         (Context->Signature == MUSB_DXE_SIGNATURE) &&
         !Context->ExitingBootServices && !Context->Stopping;
}

STATIC
int
MusbMmioRead8 (
  IN VOID      *Opaque,
  IN UINT32    Offset,
  OUT UINT8    *Value
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (!MusbValidContext(Context) || (Value == NULL)) {
    return 0;
  }
  *Value = MmioRead8((UINTN)Context->ControllerBase + Offset);
  return 1;
}

STATIC
int
MusbMmioRead16 (
  IN VOID      *Opaque,
  IN UINT32    Offset,
  OUT UINT16   *Value
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (!MusbValidContext(Context) || (Value == NULL)) {
    return 0;
  }
  *Value = MmioRead16((UINTN)Context->ControllerBase + Offset);
  return 1;
}

STATIC
int
MusbMmioRead32 (
  IN VOID      *Opaque,
  IN UINT32    Offset,
  OUT UINT32   *Value
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (!MusbValidContext(Context) || (Value == NULL)) {
    return 0;
  }
  *Value = MmioRead32((UINTN)Context->ControllerBase + Offset);
  return 1;
}

STATIC
int
MusbMmioWrite8 (
  IN VOID    *Opaque,
  IN UINT32  Offset,
  IN UINT8   Value
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (!MusbValidContext(Context)) {
    return 0;
  }
  MmioWrite8((UINTN)Context->ControllerBase + Offset, Value);
  return 1;
}

STATIC
int
MusbMmioWrite16 (
  IN VOID    *Opaque,
  IN UINT32  Offset,
  IN UINT16  Value
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (!MusbValidContext(Context)) {
    return 0;
  }
  MmioWrite16((UINTN)Context->ControllerBase + Offset, Value);
  return 1;
}

STATIC
int
MusbMmioWrite32 (
  IN VOID    *Opaque,
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (!MusbValidContext(Context)) {
    return 0;
  }
  MmioWrite32((UINTN)Context->ControllerBase + Offset, Value);
  return 1;
}

STATIC
uint64_t
MusbNowUs (
  IN VOID  *Context
  )
{
  (VOID)Context;
  return GetTimeInNanoSecond(GetPerformanceCounter()) / 1000U;
}

STATIC
VOID
MusbDelayUs (
  IN VOID    *Context,
  IN UINT32  Microseconds
  )
{
  (VOID)Context;
  MicroSecondDelay(Microseconds);
}

STATIC
int
MusbTryLock (
  IN VOID  *Opaque
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (!MusbValidContext(Context)) {
    return 0;
  }
  return (EfiAcquireLockOrFail(&Context->CoreLock) == EFI_SUCCESS);
}

STATIC
VOID
MusbUnlock (
  IN VOID  *Opaque
  )
{
  MUSB_HOST_DXE *Context;

  Context = (MUSB_HOST_DXE *)Opaque;
  if (MusbValidContext(Context)) {
    EfiReleaseLock(&Context->CoreLock);
  }
}

STATIC
UINT32
MusbTransferError (
  IN MUSB_STATUS  Status
  )
{
  switch (Status) {
    case MusbOk:
      return EFI_USB_NOERROR;
    case MusbStall:
      return EFI_USB_ERR_STALL;
    case MusbBufferOverflow:
      return EFI_USB_ERR_BUFFER;
    case MusbTimeout:
      return EFI_USB_ERR_TIMEOUT;
    case MusbBusy:
      return EFI_USB_ERR_NOTEXECUTE;
    case MusbDisconnected:
      return EFI_USB_ERR_NOTEXECUTE;
    case MusbInvalid:
    case MusbUnsupported:
    case MusbNotReady:
    case MusbProtocolError:
    case MusbIoError:
    default:
      return EFI_USB_ERR_SYSTEM;
  }
}

STATIC
EFI_STATUS
MusbStatusToEfi (
  IN MUSB_STATUS  Status
  )
{
  switch (Status) {
    case MusbOk:
      return EFI_SUCCESS;
    case MusbInvalid:
      return EFI_INVALID_PARAMETER;
    case MusbUnsupported:
      return EFI_UNSUPPORTED;
    case MusbNotReady:
    case MusbBusy:
      return EFI_NOT_READY;
    case MusbTimeout:
      return EFI_TIMEOUT;
    case MusbBufferOverflow:
      return EFI_BUFFER_TOO_SMALL;
    case MusbStall:
    case MusbProtocolError:
    case MusbDisconnected:
    case MusbIoError:
    default:
      return EFI_DEVICE_ERROR;
  }
}

STATIC
UINT32
MusbTimeoutMs (
  IN UINTN  TimeOut
  )
{
  if (TimeOut == 0U) {
    return 0U;
  }
  if (TimeOut > 60000U) {
    return 60000U;
  }
  return (UINT32)TimeOut;
}

STATIC
MUSB_STATUS
MusbValidateReady (
  IN  CONST MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL  *Ready,
  OUT MUSB_HOST_CONFIG                             *Config
  )
{
  if ((Ready == NULL) || (Config == NULL) ||
      (Ready->Signature != MTK_MUSB_HOST_PLATFORM_READY_SIGNATURE) ||
      (Ready->Length < MTK_MUSB_HOST_PLATFORM_READY_V3_LENGTH) ||
      ((Ready->Revision == MTK_MUSB_HOST_PLATFORM_READY_REVISION) &&
       (Ready->Length < sizeof(*Ready))) ||
      (Ready->ControllerBase != MTK_MUSB_CONTROLLER_BASE) ||
      (Ready->ControllerLength < MTK_MUSB_CONTROLLER_LENGTH) ||
      (Ready->PhyBase != MTK_MUSB_PHY_BASE) ||
      (Ready->PhyLength < MTK_MUSB_PHY_LENGTH) ||
      (Ready->InterruptVector != MTK_MUSB_GSIV) ||
      (Ready->EndpointCount != MTK_MUSB_ENDPOINT_COUNT) ||
      (Ready->PortCount != MTK_MUSB_ROOT_PORT_COUNT) ||
      (Ready->FifoProfile != MTK_MUSB_FIFO_PROFILE_T70PRO_HOST_V1) ||
      ((Ready->InterruptMask & MTK_MUSB_CONNECT_DISCONNECT_MASK) !=
       MTK_MUSB_CONNECT_DISCONNECT_MASK)) {
    return MusbNotReady;
  }

  if (Ready->Revision == MTK_MUSB_HOST_PLATFORM_READY_LEGACY_REVISION) {
    if ((Ready->HostPowerPath != MTK_MUSB_HOST_POWER_PATH_LEGACY_TYPEC) ||
        (Ready->Flags != MTK_MUSB_READY_REQUIRED)) {
      return MusbNotReady;
    }
  } else if ((Ready->Revision == MTK_MUSB_HOST_PLATFORM_READY_V3_REVISION) ||
             (Ready->Revision == MTK_MUSB_HOST_PLATFORM_READY_REVISION)) {
    if (((Ready->HostPowerPath == MTK_MUSB_HOST_POWER_PATH_TYPEC_SOURCE) &&
         (Ready->Flags == MTK_MUSB_READY_REQUIRED)) ||
        ((Ready->HostPowerPath == MTK_MUSB_HOST_POWER_PATH_TYPEC_ONLY) &&
         (Ready->Flags == MTK_MUSB_READY_REQUIRED)) ||
        ((Ready->HostPowerPath == MTK_MUSB_HOST_POWER_PATH_ID_LOW_HUB) &&
         (Ready->Flags == MTK_MUSB_READY_ID_LOW_REQUIRED)) ||
        ((Ready->HostPowerPath == MTK_MUSB_HOST_POWER_PATH_HOST_FIRST) &&
         (Ready->Flags == MTK_MUSB_READY_HOST_FIRST_REQUIRED))) {
      // Direct Type-C and Type-C-source routes both require the VBUS flag.
    } else {
      return MusbNotReady;
    }
  } else {
    return MusbNotReady;
  }

  ZeroMem(Config, sizeof(*Config));
  Config->Revision = MUSB_HOST_CORE_REVISION;
  Config->Flags = Ready->Flags;
  Config->ControllerLength = Ready->ControllerLength;
  Config->PhyLength = Ready->PhyLength;
  Config->InterruptVector = Ready->InterruptVector;
  Config->EndpointCount = Ready->EndpointCount;
  Config->PortCount = Ready->PortCount;
  Config->FifoProfile = Ready->FifoProfile;
  Config->InterruptMask = Ready->InterruptMask;
  return MusbOk;
}

STATIC
EFI_STATUS
MusbCloseAsyncEvent (
  IN MUSB_HOST_DXE *Context,
  IN EFI_EVENT Event
  )
{
  EFI_STATUS Status;
  (VOID)gBS->SetTimer(Event, TimerCancel, 0U);
  Status = gBS->CloseEvent(Event);
  if (EFI_ERROR(Status)) {
    // Even a cancelled event may still reference a slot in this context.
    // Refuse normal release if the event service did not confirm closure.
    Context->AsyncCloseFailed = TRUE;
    mKeepImageResident = TRUE;
  }
  return Status;
}

STATIC
EFI_STATUS
MusbStopAsync (
  IN MUSB_HOST_DXE *Context
  )
{
  EFI_EVENT Events[MUSB_ASYNC_SLOT_COUNT];
  EFI_STATUS LockStatus;
  MUSB_ASYNC_SLOT *Slot;
  UINTN Index;
  UINTN EventCount;

  EventCount = 0U;
  LockStatus = EfiAcquireLockOrFail(&Context->AsyncLock);
  if (EFI_ERROR(LockStatus)) {
    return EFI_NOT_READY;
  }
  for (Index = 0U; Index < MUSB_ASYNC_SLOT_COUNT; ++Index) {
    if (Context->AsyncSlots[Index].AsyncInCallback) {
      EfiReleaseLock(&Context->AsyncLock);
      return EFI_NOT_READY;
    }
  }
  for (Index = 0U; Index < MUSB_ASYNC_SLOT_COUNT; ++Index) {
    Slot = &Context->AsyncSlots[Index];
    Slot->AsyncActive = FALSE;
    ++Slot->AsyncGeneration;
    if (!Slot->AsyncInCallback && (Slot->AsyncEvent != NULL)) {
      Events[EventCount++] = Slot->AsyncEvent;
      Slot->AsyncEvent = NULL;
    }
    Slot->AsyncCallback = NULL;
    Slot->AsyncCallbackContext = NULL;
  }
  EfiReleaseLock(&Context->AsyncLock);
  for (Index = 0U; Index < EventCount; ++Index) {
    (VOID)MusbCloseAsyncEvent(Context, Events[Index]);
  }
  return Context->AsyncCloseFailed ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MusbGetCapability (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  OUT UINT8                 *MaxSpeed,
  OUT UINT8                 *PortNumber,
  OUT UINT8                 *Is64BitCapable
  )
{
  MUSB_HOST_DXE *Context;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context) || (MaxSpeed == NULL) ||
      (PortNumber == NULL) || (Is64BitCapable == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *MaxSpeed = EFI_USB_SPEED_HIGH;
  *PortNumber = MTK_MUSB_ROOT_PORT_COUNT;
  *Is64BitCapable = 0U;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MusbReset (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN UINT16                Attributes
  )
{
  MUSB_HOST_DXE *Context;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Attributes != EFI_USB_HC_RESET_GLOBAL) &&
      (Attributes != EFI_USB_HC_RESET_HOST_CONTROLLER) &&
      (Attributes != EFI_USB_HC_RESET_GLOBAL_WITH_DEBUG) &&
      (Attributes != EFI_USB_HC_RESET_HOST_WITH_DEBUG)) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MusbGetState (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  OUT EFI_USB_HC_STATE      *State
  )
{
  MUSB_HOST_DXE *Context;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Context->Host.State == MUSB_HOST_STATE_OPERATIONAL) {
    *State = EfiUsbHcStateOperational;
  } else {
    *State = EfiUsbHcStateHalt;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MusbSetState (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN EFI_USB_HC_STATE      State
  )
{
  MUSB_HOST_DXE *Context;
  MUSB_STATUS   CoreStatus;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context)) {
    return EFI_INVALID_PARAMETER;
  }
  if (State == EfiUsbHcStateSuspend) {
    return EFI_UNSUPPORTED;
  }
  if ((State != EfiUsbHcStateHalt) &&
      (State != EfiUsbHcStateOperational)) {
    return EFI_INVALID_PARAMETER;
  }

  CoreStatus = MusbHostSetState(
                 &Context->Host,
                 (State == EfiUsbHcStateOperational) ?
                 MUSB_HOST_STATE_OPERATIONAL : MUSB_HOST_STATE_HALTED
                 );
  if (CoreStatus == MusbOk) {
    if (State == EfiUsbHcStateHalt) {
      return MusbStopAsync(Context);
    }
  }
  return MusbStatusToEfi(CoreStatus);
}

STATIC
EFI_STATUS
MusbRouteFromTranslator (
  IN UINT8 DeviceSpeed,
  IN CONST EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator OPTIONAL,
  OUT MUSB_HOST_ROUTE *Route
  )
{
  Route->HubAddress = 0U;
  Route->HubPort = 0U;
  Route->MultiTt = 0U;
  Route->Reserved = 0U;
  if (DeviceSpeed > EFI_USB_SPEED_HIGH) {
    return EFI_INVALID_PARAMETER;
  }
  if (Translator == NULL) {
    return EFI_SUCCESS;
  }
  Route->HubAddress = Translator->TranslatorHubAddress;
  Route->HubPort = Translator->TranslatorPortNumber;
  // UsbBusDxe passes the address of a zero-filled translator even for
  // direct root devices.  Its default hub interface uses single-TT mode;
  // the UEFI record does not describe a multi-TT alternate setting.
  if ((Route->HubAddress > 127U) || (Route->HubPort > 127U) ||
      ((Route->HubAddress == 0U) != (Route->HubPort == 0U)) ||
      ((Route->HubAddress != 0U) && (DeviceSpeed == EFI_USB_SPEED_HIGH))) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MusbControlTransfer (
  IN     EFI_USB2_HC_PROTOCOL               *This,
  IN     UINT8                              DeviceAddress,
  IN     UINT8                              DeviceSpeed,
  IN     UINTN                              MaximumPacketLength,
  IN     EFI_USB_DEVICE_REQUEST             *Request,
  IN     EFI_USB_DATA_DIRECTION             TransferDirection,
  IN OUT VOID                               *Data OPTIONAL,
  IN OUT UINTN                              *DataLength OPTIONAL,
  IN     UINTN                              TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  OUT    UINT32                             *TransferResult
  )
{
  MUSB_HOST_DXE *Context;
  UINT8          Setup[8];
  UINT32         Transferred;
  UINTN          Capacity;
  MUSB_STATUS    CoreStatus;
  MUSB_HOST_ROUTE Route;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context) || (Request == NULL) ||
      (TransferResult == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *TransferResult = EFI_USB_NOERROR;
  if (EFI_ERROR(MusbRouteFromTranslator(DeviceSpeed, Translator, &Route)) ||
      (DeviceAddress > 127U) || (MaximumPacketLength > MAX_UINT8)) {
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_INVALID_PARAMETER;
  }

  if (DataLength == NULL) {
    if ((TransferDirection != EfiUsbNoData) ||
        (Request->Length != 0U) || (Data != NULL)) {
      *TransferResult = EFI_USB_ERR_SYSTEM;
      return EFI_INVALID_PARAMETER;
    }
    Capacity = 0U;
  } else {
    Capacity = *DataLength;
    if (Capacity > MAX_UINT32) {
      *TransferResult = EFI_USB_ERR_BUFFER;
      return EFI_INVALID_PARAMETER;
    }
    if ((TransferDirection == EfiUsbNoData) &&
        ((Request->Length != 0U) || (Capacity != 0U) || (Data != NULL))) {
      *TransferResult = EFI_USB_ERR_SYSTEM;
      return EFI_INVALID_PARAMETER;
    }
    if ((TransferDirection != EfiUsbNoData) &&
        ((Request->Length == 0U) || (Data == NULL) || (Capacity == 0U))) {
      *TransferResult = EFI_USB_ERR_SYSTEM;
      return EFI_INVALID_PARAMETER;
    }
    if (((TransferDirection == EfiUsbDataIn) &&
         ((Request->RequestType & 0x80U) == 0U)) ||
        ((TransferDirection == EfiUsbDataOut) &&
         ((Request->RequestType & 0x80U) != 0U)) ||
        ((TransferDirection != EfiUsbDataIn) &&
         (TransferDirection != EfiUsbDataOut) &&
         (TransferDirection != EfiUsbNoData))) {
      *TransferResult = EFI_USB_ERR_SYSTEM;
      return EFI_INVALID_PARAMETER;
    }
  }

  Setup[0] = Request->RequestType;
  Setup[1] = Request->Request;
  Setup[2] = (UINT8)Request->Value;
  Setup[3] = (UINT8)(Request->Value >> 8);
  Setup[4] = (UINT8)Request->Index;
  Setup[5] = (UINT8)(Request->Index >> 8);
  Setup[6] = (UINT8)Request->Length;
  Setup[7] = (UINT8)(Request->Length >> 8);

  CoreStatus = MusbHostControlTransferRouted(
                 &Context->Host,
                 DeviceAddress,
                 DeviceSpeed,
                 (UINT8)MaximumPacketLength,
                 Setup,
                 Data,
                 (UINT32)Capacity,
                 MusbTimeoutMs(TimeOut),
                 &Route,
                 &Transferred
                 );
  if (DataLength != NULL) {
    *DataLength = Transferred;
  }
  *TransferResult = MusbTransferError(CoreStatus);
  return MusbStatusToEfi(CoreStatus);
}

STATIC
EFI_STATUS
EFIAPI
MusbBulkTransfer (
  IN     EFI_USB2_HC_PROTOCOL               *This,
  IN     UINT8                              DeviceAddress,
  IN     UINT8                              EndPointAddress,
  IN     UINT8                              DeviceSpeed,
  IN     UINTN                              MaximumPacketLength,
  IN     UINT8                              DataBuffersNumber,
  IN OUT VOID                               *Data[EFI_USB_MAX_BULK_BUFFER_NUM],
  IN OUT UINTN                              *DataLength,
  IN OUT UINT8                              *DataToggle,
  IN     UINTN                              TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  OUT    UINT32                             *TransferResult
  )
{
  MUSB_HOST_DXE *Context;
  UINT32         Transferred;
  MUSB_STATUS    CoreStatus;
  MUSB_HOST_ROUTE Route;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context) || (Data == NULL) ||
      (DataLength == NULL) || (DataToggle == NULL) ||
      (TransferResult == NULL) || (DataBuffersNumber == 0U) ||
      (DataBuffersNumber > EFI_USB_MAX_BULK_BUFFER_NUM) ||
      (Data[0] == NULL) || (*DataLength == 0U) ||
      (*DataLength > MAX_UINT32) ||
      (*DataToggle > 1U) || (DeviceSpeed > EFI_USB_SPEED_HIGH) ||
      (MaximumPacketLength == 0U) ||
      (MaximumPacketLength > MUSB_HOST_DATA_FIFO_BYTES) ||
      (DeviceAddress > 127U)) {
    return EFI_INVALID_PARAMETER;
  }
  *TransferResult = EFI_USB_NOERROR;
  if (EFI_ERROR(MusbRouteFromTranslator(DeviceSpeed, Translator, &Route))) {
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_INVALID_PARAMETER;
  }
  if ((DeviceSpeed == EFI_USB_SPEED_LOW) ||
      ((DeviceSpeed == EFI_USB_SPEED_FULL) &&
       (MaximumPacketLength > 64U))) {
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_INVALID_PARAMETER;
  }
  //
  // This PIO engine consumes one contiguous buffer.  Full-speed UEFI bulk
  // transfers use Data[0]; high-speed scatter buffers have no per-buffer
  // lengths in this protocol and are not guessed here.
  //
  if ((DeviceSpeed == EFI_USB_SPEED_HIGH) && (DataBuffersNumber != 1U)) {
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_UNSUPPORTED;
  }

  CoreStatus = MusbHostDataTransferRouted(
                 &Context->Host,
                 MUSB_TRANSFER_BULK,
                 DeviceAddress,
                 EndPointAddress,
                 DeviceSpeed,
                 (UINT16)MaximumPacketLength,
                 0U,
                 Data[0],
                 (UINT32)*DataLength,
                 DataToggle,
                 MusbTimeoutMs(TimeOut),
                 &Route,
                 &Transferred
                 );
  *DataLength = Transferred;
  *TransferResult = MusbTransferError(CoreStatus);
  return MusbStatusToEfi(CoreStatus);
}

STATIC
VOID
EFIAPI
MusbAsyncTimerNotify (
  IN EFI_EVENT  Event,
  IN VOID       *NotifyContext
  )
{
  MUSB_HOST_DXE *Context;
  MUSB_ASYNC_SLOT *Slot;
  EFI_STATUS     LockStatus;
  EFI_EVENT      CloseEvent;
  EFI_ASYNC_USB_TRANSFER_CALLBACK Callback;
  VOID          *CallbackContext;
  UINT64         Generation;
  UINT8          Address;
  UINT8          Endpoint;
  UINT8          Speed;
  UINT8          Toggle;
  MUSB_HOST_ROUTE Route;
  UINT16         MaximumPacket;
  UINTN          DataLength;
  UINT32         Transferred;
  UINT32         CallbackResult;
  MUSB_STATUS    CoreStatus;
  BOOLEAN        InvokeCallback;

  Slot = (MUSB_ASYNC_SLOT *)NotifyContext;
  Context = (Slot == NULL) ? NULL : Slot->Owner;
  if (!MusbValidContext(Context) || (Event == NULL)) {
    return;
  }

  LockStatus = EfiAcquireLockOrFail(&Context->AsyncLock);
  if (EFI_ERROR(LockStatus)) {
    return;
  }
  if (!Slot->AsyncActive || (Slot->AsyncEvent != Event) ||
      Slot->AsyncInCallback) {
    EfiReleaseLock(&Context->AsyncLock);
    return;
  }
  Slot->AsyncInCallback = TRUE;
  Generation = Slot->AsyncGeneration;
  Address = Slot->AsyncAddress;
  Endpoint = Slot->AsyncEndpoint;
  Speed = Slot->AsyncSpeed;
  Toggle = Slot->AsyncToggle;
  Route = Slot->AsyncRoute;
  MaximumPacket = Slot->AsyncMaximumPacket;
  DataLength = Slot->AsyncDataLength;
  Callback = Slot->AsyncCallback;
  CallbackContext = Slot->AsyncCallbackContext;
  // Stay at TPL_CALLBACK with the slot lock through this bounded PIO poll.
  // No client callback runs under the lock; other periodic pipes are serialized
  // by UEFI event dispatch and the controller has its own lock.

  // The UEFI timer sets the cadence. Let MUSB schedule the IN token at
  // its next opportunity, including a split transaction across a frame.
  Transferred = 0U;
  CoreStatus = MusbHostDataTransferRouted(
                 &Context->Host,
                 MUSB_TRANSFER_INTERRUPT_POLL,
                 Address,
                 Endpoint,
                 Speed,
                 MaximumPacket,
                 1U,
                 Slot->AsyncBuffer,
                 (UINT32)DataLength,
                 &Toggle,
                 (Speed == EFI_USB_SPEED_HIGH) ? MUSB_ASYNC_HIGH_SPEED_TIMEOUT_MS :
                                                MUSB_ASYNC_FRAME_TIMEOUT_MS,
                 &Route,
                 &Transferred
                 );
  CallbackResult = MusbTransferError(CoreStatus);
  InvokeCallback = FALSE;
  CloseEvent = NULL;
  if (Slot->AsyncActive &&
      (Slot->AsyncGeneration == Generation) &&
      (Slot->AsyncEvent == Event)) {
    // No report is normal for an idle keyboard or hub. Their error handlers
    // cancel and recover the pipe, so a bounded NAK poll must stay internal.
    if ((CoreStatus != MusbTimeout) && (CoreStatus != MusbBusy)) {
      InvokeCallback = TRUE;
    }
    if (CoreStatus == MusbStall) {
      // A stalled pipe resumes at DATA0 after the client's CLEAR_HALT.
      // Its cancel call must not restore the pre-stall DATA1 into UsbBusDxe.
      Slot->AsyncToggle = 0U;
    } else {
      // The core updates Toggle as soon as it observes an ACKed RX packet,
      // including overflow or later FIFO-copy errors. Preserve that progress
      // for a client that cancels/re-registers from its error callback.
      Slot->AsyncToggle = Toggle;
    }
  }
  EfiReleaseLock(&Context->AsyncLock);

  {
    EFI_STATUS CallbackStatus;

    CallbackStatus = EFI_SUCCESS;
    if (InvokeCallback && (Callback != NULL)) {
      CallbackStatus = Callback(
                         Slot->AsyncBuffer,
                         (CoreStatus == MusbOk) ? Transferred : 0U,
                         CallbackContext,
                         CallbackResult
                         );
    }

    // Timer callbacks run at TPL_CALLBACK.  Every public async operation
    // releases AsyncLock before returning, including cancel/re-register from
    // the client callback.  No other TPL_CALLBACK event can own it here.
    // An unconditional acquire expresses that invariant and avoids abandoning
    // AsyncInCallback on a fallible post-transfer cleanup path.
    EfiAcquireLock(&Context->AsyncLock);
    // Keep the old pipe cancellable during the client error callback.
    // UsbBusDxe may cancel and replace it here; retire only this generation.
    if ((EFI_ERROR(CallbackStatus) ||
         ((CoreStatus != MusbOk) && (CoreStatus != MusbTimeout) &&
          (CoreStatus != MusbBusy))) &&
        (Slot->AsyncGeneration == Generation) && Slot->AsyncActive) {
      Slot->AsyncActive = FALSE;
      ++Slot->AsyncGeneration;
      Slot->AsyncCallback = NULL;
      Slot->AsyncCallbackContext = NULL;
      Slot->AsyncEvent = NULL;
    }
    Slot->AsyncInCallback = FALSE;
    if ((Slot->AsyncGeneration != Generation) ||
        !Slot->AsyncActive ||
        (Slot->AsyncEvent != Event)) {
      if (Slot->AsyncEvent == Event) {
        Slot->AsyncEvent = NULL;
      }
      CloseEvent = Event;
    }
    EfiReleaseLock(&Context->AsyncLock);
  }

  if (CloseEvent != NULL) {
    (VOID)MusbCloseAsyncEvent(Context, CloseEvent);
  }
}

STATIC
EFI_STATUS
EFIAPI
MusbAsyncInterruptTransfer (
  IN     EFI_USB2_HC_PROTOCOL               *This,
  IN     UINT8                              DeviceAddress,
  IN     UINT8                              EndPointAddress,
  IN     UINT8                              DeviceSpeed,
  IN     UINTN                              MaximumPacketLength,
  IN     BOOLEAN                            IsNewTransfer,
  IN OUT UINT8                              *DataToggle,
  IN     UINTN                              PollingInterval OPTIONAL,
  IN     UINTN                              DataLength OPTIONAL,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator OPTIONAL,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK    CallBackFunction OPTIONAL,
  IN     VOID                               *Context OPTIONAL
  )
{
  MUSB_HOST_DXE *HostContext;
  MUSB_ASYNC_SLOT *Slot;
  UINTN          SlotIndex;
  EFI_STATUS     Status;
  EFI_EVENT      Event;
  UINT64         Generation;
  MUSB_HOST_ROUTE Route;

  HostContext = MusbContextFromProtocol(This);
  if (!MusbValidContext(HostContext) || (DataToggle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = MusbRouteFromTranslator(DeviceSpeed, Translator, &Route);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = EfiAcquireLockOrFail(&HostContext->AsyncLock);
  if (EFI_ERROR(Status)) {
    return EFI_NOT_READY;
  }

  Slot = NULL;
  for (SlotIndex = 0U; SlotIndex < MUSB_ASYNC_SLOT_COUNT; ++SlotIndex) {
    if (HostContext->AsyncSlots[SlotIndex].AsyncActive &&
        (HostContext->AsyncSlots[SlotIndex].AsyncAddress == DeviceAddress) &&
        (HostContext->AsyncSlots[SlotIndex].AsyncEndpoint == EndPointAddress)) {
      Slot = &HostContext->AsyncSlots[SlotIndex];
      break;
    }
  }

  if (!IsNewTransfer) {
    if ((Slot == NULL) || !Slot->AsyncActive ||
        (Slot->AsyncAddress != DeviceAddress) ||
        (Slot->AsyncEndpoint != EndPointAddress) ||
        (Slot->AsyncSpeed != DeviceSpeed) ||
        (Slot->AsyncRoute.HubAddress != Route.HubAddress) ||
        (Slot->AsyncRoute.HubPort != Route.HubPort)) {
      EfiReleaseLock(&HostContext->AsyncLock);
      return EFI_INVALID_PARAMETER;
    }
    // UEFI borrows DataToggle only for this call.  Return progress on
    // cancellation instead of retaining or writing the registration pointer.
    *DataToggle = Slot->AsyncToggle;
    Slot->AsyncActive = FALSE;
    ++Slot->AsyncGeneration;
    Slot->AsyncCallback = NULL;
    Slot->AsyncCallbackContext = NULL;
    Event = Slot->AsyncEvent;
    if (!Slot->AsyncInCallback) {
      Slot->AsyncEvent = NULL;
    } else {
      Event = NULL;
    }
    EfiReleaseLock(&HostContext->AsyncLock);
    if (Event != NULL) {
      (VOID)MusbCloseAsyncEvent(HostContext, Event);
    }
    return EFI_SUCCESS;
  }

  if (Slot != NULL) {
    EfiReleaseLock(&HostContext->AsyncLock);
    return EFI_INVALID_PARAMETER;
  }
  for (SlotIndex = 0U; SlotIndex < MUSB_ASYNC_SLOT_COUNT; ++SlotIndex) {
    if (!HostContext->AsyncSlots[SlotIndex].AsyncActive) {
      Slot = &HostContext->AsyncSlots[SlotIndex];
      break;
    }
  }
  if (Slot == NULL) {
    EfiReleaseLock(&HostContext->AsyncLock);
    return EFI_OUT_OF_RESOURCES;
  }
  if ((HostContext->Host.State != MUSB_HOST_STATE_OPERATIONAL) ||
      (DataToggle == NULL) || (*DataToggle > 1U) ||
      (CallBackFunction == NULL) ||
      (DeviceAddress > 127U) ||
      ((EndPointAddress & 0x80U) == 0U) ||
      ((EndPointAddress & 0x70U) != 0U) ||
      ((EndPointAddress & 0x0FU) == 0U) ||
      (DeviceSpeed > EFI_USB_SPEED_HIGH) ||
      (MaximumPacketLength == 0U) ||
      (MaximumPacketLength > MUSB_ASYNC_MAX_DATA) ||
      (DataLength == 0U) ||
      (DataLength > MaximumPacketLength) ||
      (PollingInterval == 0U) ||
      (PollingInterval > MUSB_ASYNC_MAX_POLL_MS) ||
      ((DeviceSpeed == EFI_USB_SPEED_LOW) &&
       (MaximumPacketLength > 8U)) ||
      ((DeviceSpeed == EFI_USB_SPEED_FULL) &&
       (MaximumPacketLength > 64U))) {
    EfiReleaseLock(&HostContext->AsyncLock);
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->CreateEvent(
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MusbAsyncTimerNotify,
                  Slot,
                  &Event
                  );
  if (EFI_ERROR(Status)) {
    EfiReleaseLock(&HostContext->AsyncLock);
    return Status;
  }

  Slot->Owner = HostContext;
  ++Slot->AsyncGeneration;
  Generation = Slot->AsyncGeneration;
  Slot->AsyncEvent = Event;
  Slot->AsyncActive = TRUE;
  Slot->AsyncAddress = DeviceAddress;
  Slot->AsyncEndpoint = EndPointAddress;
  Slot->AsyncSpeed = DeviceSpeed;
  Slot->AsyncToggle = *DataToggle;
  Slot->AsyncRoute = Route;
  Slot->AsyncMaximumPacket = (UINT16)MaximumPacketLength;
  Slot->AsyncDataLength = DataLength;
  Slot->AsyncCallback = CallBackFunction;
  Slot->AsyncCallbackContext = Context;

  Status = gBS->SetTimer(
                  Event,
                  TimerPeriodic,
                  (UINT64)PollingInterval * 10000U
                  );
  if (EFI_ERROR(Status)) {
    if ((Slot->AsyncActive) &&
        (Slot->AsyncGeneration == Generation)) {
      Slot->AsyncActive = FALSE;
      Slot->AsyncEvent = NULL;
      Slot->AsyncCallback = NULL;
      Slot->AsyncCallbackContext = NULL;
      ++Slot->AsyncGeneration;
    }
    EfiReleaseLock(&HostContext->AsyncLock);
    (VOID)MusbCloseAsyncEvent(HostContext, Event);
    return Status;
  }
  EfiReleaseLock(&HostContext->AsyncLock);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MusbSyncInterruptTransfer (
  IN     EFI_USB2_HC_PROTOCOL               *This,
  IN     UINT8                              DeviceAddress,
  IN     UINT8                              EndPointAddress,
  IN     UINT8                              DeviceSpeed,
  IN     UINTN                              MaximumPacketLength,
  IN OUT VOID                               *Data,
  IN OUT UINTN                              *DataLength,
  IN OUT UINT8                              *DataToggle,
  IN     UINTN                              TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  OUT    UINT32                             *TransferResult
  )
{
  MUSB_HOST_DXE *Context;
  UINT32         Transferred;
  MUSB_STATUS    CoreStatus;
  MUSB_HOST_ROUTE Route;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context) || (Data == NULL) ||
      (DataLength == NULL) || (DataToggle == NULL) ||
      (TransferResult == NULL) || (*DataLength == 0U) ||
      (*DataLength > MAX_UINT32) ||
      (MaximumPacketLength > MAX_UINT16)) {
    return EFI_INVALID_PARAMETER;
  }
  *TransferResult = EFI_USB_NOERROR;
  if (EFI_ERROR(MusbRouteFromTranslator(DeviceSpeed, Translator, &Route))) {
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_INVALID_PARAMETER;
  }
  CoreStatus = MusbHostDataTransferRouted(
                 &Context->Host,
                 MUSB_TRANSFER_INTERRUPT,
                 DeviceAddress,
                 EndPointAddress,
                 DeviceSpeed,
                 (UINT16)MaximumPacketLength,
                 1U,
                 Data,
                 (UINT32)*DataLength,
                 DataToggle,
                 MusbTimeoutMs(TimeOut),
                 &Route,
                 &Transferred
                 );
  *DataLength = Transferred;
  *TransferResult = MusbTransferError(CoreStatus);
  return MusbStatusToEfi(CoreStatus);
}

STATIC
EFI_STATUS
EFIAPI
MusbIsochronousTransfer (
  IN     EFI_USB2_HC_PROTOCOL               *This,
  IN     UINT8                              DeviceAddress,
  IN     UINT8                              EndPointAddress,
  IN     UINT8                              DeviceSpeed,
  IN     UINTN                              MaximumPacketLength,
  IN     UINT8                              DataBuffersNumber,
  IN OUT VOID                               *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN     UINTN                              DataLength,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  OUT    UINT32                             *TransferResult
  )
{
  MUSB_HOST_DXE *Context;

  (VOID)DeviceAddress;
  (VOID)EndPointAddress;
  (VOID)DeviceSpeed;
  (VOID)MaximumPacketLength;
  (VOID)DataBuffersNumber;
  (VOID)Data;
  (VOID)DataLength;
  (VOID)Translator;
  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context) || (TransferResult == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *TransferResult = EFI_USB_ERR_SYSTEM;
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MusbAsyncIsochronousTransfer (
  IN     EFI_USB2_HC_PROTOCOL               *This,
  IN     UINT8                              DeviceAddress,
  IN     UINT8                              EndPointAddress,
  IN     UINT8                              DeviceSpeed,
  IN     UINTN                              MaximumPacketLength,
  IN     UINT8                              DataBuffersNumber,
  IN OUT VOID                               *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN     UINTN                              DataLength,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK    IsochronousCallBack,
  IN     VOID                               *Context OPTIONAL
  )
{
  MUSB_HOST_DXE *HostContext;

  (VOID)DeviceAddress;
  (VOID)EndPointAddress;
  (VOID)DeviceSpeed;
  (VOID)MaximumPacketLength;
  (VOID)DataBuffersNumber;
  (VOID)Data;
  (VOID)DataLength;
  (VOID)Translator;
  (VOID)IsochronousCallBack;
  (VOID)Context;
  HostContext = MusbContextFromProtocol(This);
  return MusbValidContext(HostContext) ? EFI_UNSUPPORTED :
                                        EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
EFIAPI
MusbGetRootHubPortStatus (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  IN  UINT8                 PortNumber,
  OUT EFI_USB_PORT_STATUS   *PortStatus
  )
{
  MUSB_HOST_DXE *Context;
  MUSB_STATUS    CoreStatus;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context) || (PortStatus == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  CoreStatus = MusbHostGetPortStatus(
                 &Context->Host,
                 PortNumber,
                 &PortStatus->PortStatus,
                 &PortStatus->PortChangeStatus
                 );
  return MusbStatusToEfi(CoreStatus);
}

STATIC
EFI_STATUS
EFIAPI
MusbSetRootHubPortFeature (
  IN EFI_USB2_HC_PROTOCOL    *This,
  IN UINT8                   PortNumber,
  IN EFI_USB_PORT_FEATURE    PortFeature
  )
{
  MUSB_HOST_DXE *Context;
  MUSB_STATUS    CoreStatus;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context)) {
    return EFI_INVALID_PARAMETER;
  }
  CoreStatus = MusbHostSetPortFeature(
                 &Context->Host,
                 PortNumber,
                 (UINT8)PortFeature
                 );
  return MusbStatusToEfi(CoreStatus);
}

STATIC
EFI_STATUS
EFIAPI
MusbClearRootHubPortFeature (
  IN EFI_USB2_HC_PROTOCOL    *This,
  IN UINT8                   PortNumber,
  IN EFI_USB_PORT_FEATURE    PortFeature
  )
{
  MUSB_HOST_DXE *Context;
  MUSB_STATUS    CoreStatus;

  Context = MusbContextFromProtocol(This);
  if (!MusbValidContext(Context)) {
    return EFI_INVALID_PARAMETER;
  }
  CoreStatus = MusbHostClearPortFeature(
                 &Context->Host,
                 PortNumber,
                 (UINT8)PortFeature
                 );
  return MusbStatusToEfi(CoreStatus);
}

STATIC
VOID
EFIAPI
MusbExitBootServicesNotify (
  IN EFI_EVENT  Event,
  IN VOID       *NotifyContext
  )
{
  MUSB_HOST_DXE *Context;
  EFI_EVENT      AsyncEvent;
  MUSB_ASYNC_SLOT *Slot;
  UINTN          Index;

  (VOID)Event;
  Context = (MUSB_HOST_DXE *)NotifyContext;
  if ((Context == NULL) || (Context->Signature != MUSB_DXE_SIGNATURE)) {
    return;
  }

  //
  // ExitBootServices notifications run at TPL_NOTIFY. A running
  // TPL_CALLBACK poll has completed before this notification is dispatched.
  // Do not acquire AsyncLock here; its ceiling remains TPL_CALLBACK.
  //
  // TPL_NOTIFY serializes this with all USB callbacks. The core lock has
  // the same ceiling, so taking a final port snapshot cannot lower TPL.
  if (Context->PlatformReady != NULL) {
    UINT16 PortStatus;
    UINT16 PortChange;
    Context->PlatformReady->HostSnapshotValid = 0U;
    MemoryFence();
    Context->Host.State = MUSB_HOST_STATE_HALTED;
    if (MusbHostGetPortStatus(&Context->Host, 0U, &PortStatus,
                             &PortChange) == MusbOk) {
      Context->PlatformReady->HostPortStatus = PortStatus;
      Context->PlatformReady->HostPortChangeStatus = PortChange;
      MemoryFence();
      Context->PlatformReady->HostSnapshotValid = 1U;
    }
  }
  Context->ExitingBootServices = TRUE;
  Context->Host.State = MUSB_HOST_STATE_HALTED;
  for (Index = 0U; Index < MUSB_ASYNC_SLOT_COUNT; ++Index) {
    Slot = &Context->AsyncSlots[Index];
    Slot->AsyncActive = FALSE;
    Slot->AsyncInCallback = FALSE;
    ++Slot->AsyncGeneration;
    Slot->AsyncCallback = NULL;
    Slot->AsyncCallbackContext = NULL;
    AsyncEvent = Slot->AsyncEvent;
    if (AsyncEvent != NULL) {
      (VOID)gBS->SetTimer(AsyncEvent, TimerCancel, 0U);
    }
  }
}

STATIC
EFI_STATUS
MusbFindReadyHandle (
  IN MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL *Ready,
  OUT EFI_HANDLE *ReadyHandle
  )
{
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  EFI_STATUS Status;
  VOID *Interface;

  *ReadyHandle = NULL;
  Status = gBS->LocateHandleBuffer(ByProtocol,
                    &gMtkMusbHostPlatformReadyProtocolGuid, NULL,
                    &Count, &Handles);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Status = EFI_NOT_FOUND;
  for (Index = 0U; Index < Count; ++Index) {
    Interface = NULL;
    if (!EFI_ERROR(gBS->HandleProtocol(Handles[Index],
                     &gMtkMusbHostPlatformReadyProtocolGuid, &Interface)) &&
        (Interface == Ready)) {
      *ReadyHandle = Handles[Index];
      Status = EFI_SUCCESS;
      break;
    }
  }
  FreePool(Handles);
  return Status;
}

STATIC
EFI_STATUS
MusbProtocolPresence (
  IN  MUSB_HOST_DXE *Context,
  IN  EFI_GUID      *Protocol,
  IN  VOID          *Expected,
  OUT BOOLEAN       *Present
  )
{
  EFI_STATUS Status;
  VOID *Interface;

  if ((Context == NULL) || (Protocol == NULL) || (Expected == NULL) ||
      (Present == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Present = FALSE;
  Interface = NULL;
  Status = gBS->HandleProtocol(Context->ControllerHandle, Protocol, &Interface);
  // CoreHandleProtocol reports EFI_UNSUPPORTED when this protocol is not
  // installed on the specific handle. Other errors do not prove absence.
  if (Status == EFI_UNSUPPORTED) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR(Status)) {
    return Status;
  }
  if (Interface != Expected) {
    return EFI_ACCESS_DENIED;
  }
  *Present = TRUE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MusbRefreshInstalledProtocols (
  IN OUT MUSB_HOST_DXE *Context
  )
{
  EFI_STATUS Status;
  BOOLEAN UsbHcInstalled;
  BOOLEAN DevicePathInstalled;

  Status = MusbProtocolPresence(Context, &gEfiUsb2HcProtocolGuid,
                                &Context->UsbHc, &UsbHcInstalled);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Status = MusbProtocolPresence(Context, &gEfiDevicePathProtocolGuid,
                                &Context->VendorPath, &DevicePathInstalled);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Context->UsbHcInstalled = UsbHcInstalled;
  Context->DevicePathInstalled = DevicePathInstalled;
  Context->ProtocolsInstalled = UsbHcInstalled || DevicePathInstalled;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MusbReleaseHost (
  IN VOID *HostContext
  )
{
  MUSB_HOST_DXE *Context;
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL *Ready;
  EFI_STATUS Status;
  EFI_STATUS UninstallStatus;
  MUSB_STATUS CoreStatus;

  Context = (MUSB_HOST_DXE *)HostContext;
  if ((Context == NULL) || (Context->Signature != MUSB_DXE_SIGNATURE) ||
      Context->ExitingBootServices || Context->ReleaseInProgress) {
    return EFI_NOT_READY;
  }
  Ready = Context->PlatformReady;
  if ((Ready == NULL) || (Ready->Revision != MTK_MUSB_HOST_PLATFORM_READY_REVISION) ||
      (Ready->Length < sizeof(*Ready)) || (Ready->HostContext != Context) ||
      (Ready->ReleaseHost != MusbReleaseHost)) {
    return EFI_INVALID_PARAMETER;
  }
  Context->ReleaseInProgress = TRUE;
  if (!Context->Stopping) {
    CoreStatus = MusbHostSetState(&Context->Host, MUSB_HOST_STATE_HALTED);
    Status = MusbStatusToEfi(CoreStatus);
    if (EFI_ERROR(Status)) goto Done;
    Status = MusbStopAsync(Context);
    if (EFI_ERROR(Status)) goto Done;
    Context->Stopping = TRUE;
  }

  Status = MusbRefreshInstalledProtocols(Context);
  if (EFI_ERROR(Status)) goto Done;
  if (Context->UsbHcInstalled && Context->DevicePathInstalled) {
    UninstallStatus = gBS->UninstallMultipleProtocolInterfaces(
                         Context->ControllerHandle,
                         &gEfiUsb2HcProtocolGuid, &Context->UsbHc,
                         &gEfiDevicePathProtocolGuid, &Context->VendorPath, NULL);
  } else if (Context->UsbHcInstalled) {
    UninstallStatus = gBS->UninstallProtocolInterface(
                         Context->ControllerHandle, &gEfiUsb2HcProtocolGuid,
                         &Context->UsbHc);
  } else if (Context->DevicePathInstalled) {
    UninstallStatus = gBS->UninstallProtocolInterface(
                         Context->ControllerHandle, &gEfiDevicePathProtocolGuid,
                         &Context->VendorPath);
  } else {
    UninstallStatus = EFI_SUCCESS;
  }
  Status = MusbRefreshInstalledProtocols(Context);
  if (EFI_ERROR(Status)) goto Done;
  if (Context->ProtocolsInstalled) {
    Status = EFI_ERROR(UninstallStatus) ? UninstallStatus : EFI_NOT_READY;
    goto Done;
  }

  if (Context->ExitBootServicesEvent != NULL) {
    Status = gBS->CloseEvent(Context->ExitBootServicesEvent);
    if (EFI_ERROR(Status)) goto Done;
    Context->ExitBootServicesEvent = NULL;
  }
  Ready->ReleaseHost = NULL;
  Ready->HostContext = NULL;
  Ready->HostSnapshotValid = 0U;
  Context->Signature = 0U;
  FreePool(Context);
  mHostStarted = FALSE;
  mHostInstallBlocked = FALSE;
  return EFI_SUCCESS;
Done:
  // Keep the context until handle protocols and callback resources are gone.
  Context->ReleaseInProgress = FALSE;
  return Status;
}

STATIC
EFI_STATUS
MusbInstallHost (
  IN MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL  *Ready
  )
{
  MUSB_HOST_DXE    *Context;
  MUSB_HOST_CONFIG  Config;
  MUSB_HOST_IO      Io;
  MUSB_STATUS       CoreStatus;
  EFI_STATUS        Status;
  EFI_HANDLE        ReadyHandle;

  if (mHostStarted) {
    return EFI_ALREADY_STARTED;
  }
  if (mHostInstallBlocked) {
    return EFI_NOT_READY;
  }
  CoreStatus = MusbValidateReady(Ready, &Config);
  if (CoreStatus != MusbOk) {
    return MusbStatusToEfi(CoreStatus);
  }

  Status = MusbFindReadyHandle(Ready, &ReadyHandle);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Context = AllocateZeroPool(sizeof(*Context));
  if (Context == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Context->Signature = MUSB_DXE_SIGNATURE;
  Context->ControllerBase = Ready->ControllerBase;
  Context->ControllerHandle = ReadyHandle;
  Context->PlatformReady = Ready;
  Ready->HostSnapshotValid = 0U;
  EfiInitializeLock(&Context->CoreLock, TPL_NOTIFY);
  EfiInitializeLock(&Context->AsyncLock, TPL_CALLBACK);

  ZeroMem(&Io, sizeof(Io));
  Io.Context = Context;
  Io.ControllerLength = Ready->ControllerLength;
  Io.Read8 = MusbMmioRead8;
  Io.Read16 = MusbMmioRead16;
  Io.Read32 = MusbMmioRead32;
  Io.Write8 = MusbMmioWrite8;
  Io.Write16 = MusbMmioWrite16;
  Io.Write32 = MusbMmioWrite32;
  Io.NowUs = MusbNowUs;
  Io.DelayUs = MusbDelayUs;
  Io.TryLock = MusbTryLock;
  Io.Unlock = MusbUnlock;
  CoreStatus = MusbHostInitialize(&Context->Host, &Io, &Config);
  if (CoreStatus != MusbOk) {
    Context->Signature = 0U;
    FreePool(Context);
    return MusbStatusToEfi(CoreStatus);
  }
  CoreStatus = MusbHostSetState(&Context->Host, MUSB_HOST_STATE_OPERATIONAL);
  if (CoreStatus != MusbOk) {
    Context->Signature = 0U;
    FreePool(Context);
    return MusbStatusToEfi(CoreStatus);
  }

  Context->UsbHc.GetCapability = MusbGetCapability;
  Context->UsbHc.Reset = MusbReset;
  Context->UsbHc.GetState = MusbGetState;
  Context->UsbHc.SetState = MusbSetState;
  Context->UsbHc.ControlTransfer = MusbControlTransfer;
  Context->UsbHc.BulkTransfer = MusbBulkTransfer;
  Context->UsbHc.AsyncInterruptTransfer = MusbAsyncInterruptTransfer;
  Context->UsbHc.SyncInterruptTransfer = MusbSyncInterruptTransfer;
  Context->UsbHc.IsochronousTransfer = MusbIsochronousTransfer;
  Context->UsbHc.AsyncIsochronousTransfer = MusbAsyncIsochronousTransfer;
  Context->UsbHc.GetRootHubPortStatus = MusbGetRootHubPortStatus;
  Context->UsbHc.SetRootHubPortFeature = MusbSetRootHubPortFeature;
  Context->UsbHc.ClearRootHubPortFeature = MusbClearRootHubPortFeature;
  Context->UsbHc.MajorRevision = 2U;
  Context->UsbHc.MinorRevision = 0U;

  Context->VendorPath.Header.Type = HARDWARE_DEVICE_PATH;
  Context->VendorPath.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength(&Context->VendorPath.Header, sizeof(VENDOR_DEVICE_PATH));
  CopyGuid(&Context->VendorPath.Guid, &gMtkMusbHostPlatformReadyProtocolGuid);
  Context->EndPath.Type = END_DEVICE_PATH_TYPE;
  Context->EndPath.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength(&Context->EndPath, sizeof(Context->EndPath));

  Status = gBS->CreateEventEx(
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  MusbExitBootServicesNotify,
                  Context,
                  &gEfiEventExitBootServicesGuid,
                  &Context->ExitBootServicesEvent
                  );
  if (EFI_ERROR(Status)) {
    (VOID)MusbHostSetState(&Context->Host, MUSB_HOST_STATE_HALTED);
    Context->Signature = 0U;
    FreePool(Context);
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces(
                  &Context->ControllerHandle,
                  &gEfiUsb2HcProtocolGuid,
                  &Context->UsbHc,
                  &gEfiDevicePathProtocolGuid,
                  &Context->VendorPath,
                  NULL
                  );
  if (EFI_ERROR(Status)) {
    EFI_STATUS InstallStatus;

    InstallStatus = Status;
    (VOID)MusbHostSetState(&Context->Host, MUSB_HOST_STATE_HALTED);
    Context->Host.State = MUSB_HOST_STATE_HALTED;
    // A failed multi-protocol install may have published only one interface.
    // Publish a release callback before attempting rollback so the board owner
    // can retry if protocol lookup, uninstall, or event close is incomplete.
    if ((Ready->Revision == MTK_MUSB_HOST_PLATFORM_READY_REVISION) &&
        (Ready->Length >= sizeof(*Ready))) {
      Ready->HostContext = Context;
      Ready->ReleaseHost = MusbReleaseHost;
    }
    mHostStarted = TRUE;
    mHostInstallBlocked = TRUE;
    Status = MusbRefreshInstalledProtocols(Context);
    if (!EFI_ERROR(Status) && Ready->HostContext == Context &&
        Ready->ReleaseHost == MusbReleaseHost) {
      Status = MusbReleaseHost(Context);
      if (!EFI_ERROR(Status)) {
        return InstallStatus;
      }
    }
    // Unknown or partly published protocols must remain attached to this
    // context; do not free a live interface or let a second host start.
    mHostInstallBlocked = TRUE;
    mKeepImageResident = TRUE;
    return InstallStatus;
  }
  Context->UsbHcInstalled = TRUE;
  Context->DevicePathInstalled = TRUE;
  Context->ProtocolsInstalled = TRUE;
  mHostStarted = TRUE;
  if (Ready->Revision == MTK_MUSB_HOST_PLATFORM_READY_REVISION) {
    Ready->HostContext = Context;
    Ready->ReleaseHost = MusbReleaseHost;
  }
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
MusbReadyNotify (
  IN EFI_EVENT  Event,
  IN VOID       *NotifyContext
  )
{
  EFI_STATUS                                  Status;
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL      *Ready;
  (VOID)Event;
  (VOID)NotifyContext;
  if (mHostStarted || mHostInstallBlocked || mReadyNotifyDisabled) {
    return;
  }

  Status = gBS->LocateProtocol(
                  &gMtkMusbHostPlatformReadyProtocolGuid,
                  NULL,
                  (VOID **)&Ready
                  );
  if (EFI_ERROR(Status) || (Ready == NULL)) {
    return;
  }
  Status = MusbInstallHost(Ready);
  if (Status == EFI_SUCCESS) {
    //
    // Keep the notify event alive; further notifications are harmless because
    // MusbInstallHost returns EFI_ALREADY_STARTED without touching hardware.
    //
  }
}

EFI_STATUS
EFIAPI
MusbHostDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                              Status;
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL  *Ready;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  if (mKeepImageResident) {
    return EFI_SUCCESS;
  }

  if (mReadyNotifyEvent != NULL) {
    MusbReadyNotify(NULL, NULL);
    return EFI_SUCCESS;
  }
  // Registration must also survive when readiness precedes driver dispatch;
  // the producer can retire that first host and publish a new route later.
  mReadyNotifyDisabled = FALSE;
  Status = gBS->CreateEvent(
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MusbReadyNotify,
                  NULL,
                  &mReadyNotifyEvent
                  );
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Status = gBS->RegisterProtocolNotify(
                  &gMtkMusbHostPlatformReadyProtocolGuid,
                  mReadyNotifyEvent,
                  &mReadyRegistration
                  );
  if (EFI_ERROR(Status)) {
    EFI_STATUS CloseStatus;

    mReadyNotifyDisabled = TRUE;
    CloseStatus = gBS->CloseEvent(mReadyNotifyEvent);
    mReadyRegistration = NULL;
    if (EFI_ERROR(CloseStatus)) {
      //
      // RegisterProtocolNotify failed, but a failed CloseEvent leaves a
      // callback reference into this image. Keep the event and image alive;
      // MusbReadyNotify is disabled above and will be a no-op if signaled.
      //
      mKeepImageResident = TRUE;
      return EFI_SUCCESS;
    }

    mReadyNotifyEvent = NULL;
    return Status;
  }
  //
  // Close the LocateProtocol/RegisterProtocolNotify race by scheduling the
  // same read-only readiness callback once after registration.
  //
  Status = gBS->LocateProtocol(
                  &gMtkMusbHostPlatformReadyProtocolGuid,
                  NULL,
                  (VOID **)&Ready
                  );
  if (!EFI_ERROR(Status) && (Ready != NULL)) {
    (VOID)gBS->SignalEvent(mReadyNotifyEvent);
  }
  return EFI_SUCCESS;
}

