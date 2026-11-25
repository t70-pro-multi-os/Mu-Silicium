// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/MusbHostCore.h>
#include <Protocol/Usb2HostController.h>

#include "MusbHostDxe.c"
#include "UsbBus.h"

#define TEST_REG_BYTES 0x500U
#define TEST_INTRUSB 0x00AU
#define TEST_INDEX 0x00EU
#define TEST_DEVCTL 0x060U
#define TEST_EP_BASE 0x010U
#define TEST_FIFO_BASE 0x020U
#define TEST_RXCSR 0x006U
#define TEST_RXCOUNT 0x008U
#define TEST_RXPKTRDY 0x0001U
#define TEST_DATAERROR 0x0008U
#define TEST_FLUSHFIFO 0x0010U
#define TEST_H_REQPKT 0x0020U
#define TEST_H_RXSTALL 0x0040U
#define TEST_RXINTERVAL 0x00DU
#define TEST_RXTOG 0x080U
#define TEST_TXTOG 0x084U

#define CHECK(Condition) do { \
  ++Checks; \
  if (!(Condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #Condition); \
    abort(); \
  } \
} while (0)

unsigned Checks;
VOID EFIAPI MemoryFence(VOID) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
EFI_BOOT_SERVICES *gBS;
EFI_GUID gMtkMusbHostPlatformReadyProtocolGuid =
  { 0x6c62f8ab, 0x4ac1, 0x4aa7, { 0x9d, 0x43, 0x48, 0xbc, 0x48, 0x12, 0x0d, 0x7e } };
EFI_GUID gEfiUsb2HcProtocolGuid =
  { 0x3e745226, 0x9818, 0x45b6, { 0xa2, 0xac, 0xd7, 0xcd, 0x0e, 0x8b, 0xa2, 0xbc } };
EFI_GUID gEfiDevicePathProtocolGuid =
  { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
EFI_GUID gEfiEventExitBootServicesGuid =
  { 0x27abf055, 0xb1b8, 0x4c26, { 0x80, 0x48, 0x74, 0x8f, 0x37, 0xba, 0xa2, 0xdf } };

typedef struct {
  EFI_EVENT_NOTIFY Notify;
  VOID *NotifyContext;
  BOOLEAN Closed;
  EFI_TIMER_DELAY TimerType;
  UINT64 TriggerTime;
  UINT32 CloseCount;
} FAKE_EVENT;

typedef struct {
  UINT8 Registers[TEST_REG_BYTES];
  UINT8 Indexed[16][16];
  UINT8 Index;
  UINT8 RxData[512];
  UINT8 RxResponse[512];
  UINT16 RxLength;
  UINT16 RxResponseLength;
  UINT16 RxPosition;
  UINT8 RxSuccess;
  UINT8 RxStall;
  UINT8 LockHeld;
  UINT32 RxRequests;
  UINT32 RxDelayUs;
  UINT64 RxReadyAtUs;
  UINT64 TimeUs;
} FAKE_MUSB;

typedef struct {
  MUSB_HOST_DXE *Host;
  UINT8 *OldToggle;
  UINT8 NewToggle;
  UINT8 CanceledToggle;
  UINT8 Mode;
  BOOLEAN CarryToggleOnReregister;
  UINT32 CallbackCount;
  UINT32 SuccessCount;
  UINT32 NakCount;
  UINT32 StallCount;
  UINT32 SystemCount;
  UINTN LastDataLength;
  UINT32 LastResult;
  EFI_STATUS CallbackStatus;
} ASYNC_FIXTURE;

static EFI_BOOT_SERVICES Services;
static FAKE_EVENT Events[64];
static UINT32 EventCount;
static FAKE_MUSB Bus;
static EFI_USB2_HC_PROTOCOL *InstalledUsbHc;
static EFI_DEVICE_PATH_PROTOCOL *InstalledDevicePath;
static UINTN InstallFailureMask;
static EFI_STATUS ProtocolLookupStatus;
static EFI_STATUS SingleUninstallStatus;
static UINTN SingleUninstallCalls;
static BOOLEAN InstallObservedOperational;
static BOOLEAN InstallObservedDevicePath;
static EFI_STATUS InstallStatus;
static EFI_STATUS UninstallStatus;
static UINTN UninstallCalls;
static EFI_EVENT FailCloseEvent;
static INTN FailCloseEventIndex;
static EFI_STATUS CloseFailureStatus;
static EFI_EVENT FailTimerEvent;
static EFI_STATUS TimerFailureStatus;
static EFI_STATUS LocateProtocolStatus;
static VOID *LocatedProtocol;
static EFI_STATUS RegisterProtocolNotifyStatus;
static UINT32 LocateProtocolCalls;

EFI_LOCK *
EFIAPI
EfiInitializeLock (
  IN OUT EFI_LOCK  *Lock,
  IN EFI_TPL       Priority
  )
{
  Lock->Tpl = Priority;
  Lock->OwnerTpl = TPL_APPLICATION;
  Lock->Lock = EfiLockReleased;
  return Lock;
}

EFI_STATUS
EFIAPI
EfiAcquireLockOrFail (
  IN EFI_LOCK  *Lock
  )
{
  if (Lock->Lock == EfiLockAcquired) {
    return EFI_ACCESS_DENIED;
  }
  Lock->Lock = EfiLockAcquired;
  return EFI_SUCCESS;
}

VOID
EFIAPI
EfiAcquireLock (
  IN EFI_LOCK  *Lock
  )
{
  CHECK(Lock->Lock == EfiLockReleased);
  Lock->Lock = EfiLockAcquired;
}

VOID
EFIAPI
EfiReleaseLock (
  IN EFI_LOCK  *Lock
  )
{
  CHECK(Lock->Lock == EfiLockAcquired);
  Lock->Lock = EfiLockReleased;
}

static EFI_STATUS
EFIAPI
FakeCreateEvent (
  IN UINT32              Type,
  IN EFI_TPL             NotifyTpl,
  IN EFI_EVENT_NOTIFY    NotifyFunction,
  IN VOID                *NotifyContext,
  OUT EFI_EVENT          *Event
  )
{
  FAKE_EVENT *FakeEvent;

  CHECK((Type & EVT_NOTIFY_SIGNAL) != 0U);
  CHECK((NotifyTpl == TPL_CALLBACK) || (NotifyTpl == TPL_NOTIFY));
  CHECK(NotifyFunction != NULL);
  CHECK(EventCount < sizeof(Events) / sizeof(Events[0]));
  FakeEvent = &Events[EventCount++];
  memset(FakeEvent, 0, sizeof(*FakeEvent));
  FakeEvent->Notify = NotifyFunction;
  FakeEvent->NotifyContext = NotifyContext;
  *Event = (EFI_EVENT)FakeEvent;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
FakeCreateEventEx (
  IN       UINT32            Type,
  IN       EFI_TPL           NotifyTpl,
  IN       EFI_EVENT_NOTIFY  NotifyFunction OPTIONAL,
  IN CONST VOID              *NotifyContext OPTIONAL,
  IN CONST EFI_GUID          *EventGroup OPTIONAL,
  OUT      EFI_EVENT         *Event
  )
{
  CHECK(EventGroup != NULL);
  CHECK(memcmp(EventGroup, &gEfiEventExitBootServicesGuid,
               sizeof(EFI_GUID)) == 0);
  return FakeCreateEvent(Type, NotifyTpl, NotifyFunction,
                         (VOID *)NotifyContext, Event);
}

static EFI_STATUS
EFIAPI
FakeInstallMultipleProtocolInterfaces (
  IN OUT EFI_HANDLE  *Handle,
  ...
  )
{
  va_list Arguments;
  EFI_GUID *Protocol;
  VOID *Interface;
  EFI_USB2_HC_PROTOCOL *PendingUsbHc = NULL;
  EFI_DEVICE_PATH_PROTOCOL *PendingDevicePath = NULL;

  CHECK(Handle != NULL);
  CHECK(*Handle == (EFI_HANDLE)(UINTN)0x6789U);
  va_start(Arguments, Handle);
  for (;;) {
    EFI_USB_HC_STATE State;

    Protocol = va_arg(Arguments, EFI_GUID *);
    if (Protocol == NULL) {
      break;
    }
    Interface = va_arg(Arguments, VOID *);
    if (memcmp(Protocol, &gEfiUsb2HcProtocolGuid,
               sizeof(EFI_GUID)) == 0) {
      PendingUsbHc = (EFI_USB2_HC_PROTOCOL *)Interface;
      CHECK(PendingUsbHc != NULL);
      CHECK(PendingUsbHc->GetState(PendingUsbHc, &State) == EFI_SUCCESS);
      CHECK(State == EfiUsbHcStateOperational);
      InstallObservedOperational = TRUE;
    } else if (memcmp(Protocol, &gEfiDevicePathProtocolGuid,
                      sizeof(EFI_GUID)) == 0) {
      PendingDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Interface;
      CHECK(PendingDevicePath != NULL);
      InstallObservedDevicePath = TRUE;
    }
  }
  va_end(Arguments);
  CHECK(InstallObservedOperational && InstallObservedDevicePath);
  if (EFI_ERROR(InstallStatus)) {
    InstalledUsbHc = (InstallFailureMask & 1U) ? PendingUsbHc : NULL;
    InstalledDevicePath = (InstallFailureMask & 2U) ? PendingDevicePath : NULL;
    return InstallStatus;
  }
  CHECK(*Handle == (EFI_HANDLE)(UINTN)0x6789U);
  InstalledUsbHc = PendingUsbHc;
  InstalledDevicePath = PendingDevicePath;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
FakeUninstallMultipleProtocolInterfaces (
  IN EFI_HANDLE Handle,
  ...
  )
{
  va_list Arguments;
  CHECK(Handle == (EFI_HANDLE)(UINTN)0x6789U);
  CHECK(InstalledUsbHc != NULL && InstalledDevicePath != NULL);
  MUSB_HOST_DXE *Context = MusbContextFromProtocol(InstalledUsbHc);
  CHECK(Context->Host.State == MUSB_HOST_STATE_HALTED && Context->Stopping);
  va_start(Arguments, Handle);
  CHECK(va_arg(Arguments, EFI_GUID *) == &gEfiUsb2HcProtocolGuid);
  CHECK(va_arg(Arguments, VOID *) == &Context->UsbHc);
  CHECK(va_arg(Arguments, EFI_GUID *) == &gEfiDevicePathProtocolGuid);
  CHECK(va_arg(Arguments, VOID *) == &Context->VendorPath);
  CHECK(va_arg(Arguments, VOID *) == NULL);
  va_end(Arguments);
  ++UninstallCalls;
  if (!EFI_ERROR(UninstallStatus)) {
    InstalledUsbHc = NULL;
    InstalledDevicePath = NULL;
  }
  return UninstallStatus;
}

static EFI_STATUS
EFIAPI
FakeUninstallProtocolInterface (
  IN EFI_HANDLE Handle,
  IN EFI_GUID *Protocol,
  IN VOID *Interface
  )
{
  CHECK(Handle == (EFI_HANDLE)(UINTN)0x6789U);
  CHECK(InstalledUsbHc != NULL || InstalledDevicePath != NULL);
  MUSB_HOST_DXE *Context = InstalledUsbHc ? MusbContextFromProtocol(InstalledUsbHc) :
    (MUSB_HOST_DXE *)((UINT8 *)Interface - OFFSET_OF(MUSB_HOST_DXE, VendorPath));
  CHECK(Context->Host.State == MUSB_HOST_STATE_HALTED && Context->Stopping);
  ++SingleUninstallCalls;
  if (memcmp(Protocol, &gEfiUsb2HcProtocolGuid, sizeof(*Protocol)) == 0) {
    CHECK(Interface == &Context->UsbHc && InstalledUsbHc == &Context->UsbHc);
    if (!EFI_ERROR(SingleUninstallStatus)) InstalledUsbHc = NULL;
  } else {
    CHECK(memcmp(Protocol, &gEfiDevicePathProtocolGuid, sizeof(*Protocol)) == 0);
    CHECK(Interface == &Context->VendorPath && InstalledDevicePath == &Context->VendorPath);
    if (!EFI_ERROR(SingleUninstallStatus)) InstalledDevicePath = NULL;
  }
  return SingleUninstallStatus;
}

static EFI_STATUS
EFIAPI
FakeSetTimer (
  IN EFI_EVENT        Event,
  IN EFI_TIMER_DELAY  Type,
  IN UINT64           TriggerTime
  )
{
  FAKE_EVENT *FakeEvent;

  CHECK(Event != NULL);
  FakeEvent = (FAKE_EVENT *)Event;
  if ((Event == FailTimerEvent) && EFI_ERROR(TimerFailureStatus)) {
    return TimerFailureStatus;
  }
  FakeEvent->TimerType = Type;
  FakeEvent->TriggerTime = TriggerTime;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
FakeCloseEvent (
  IN EFI_EVENT  Event
  )
{
  FAKE_EVENT *FakeEvent;

  CHECK(Event != NULL);
  FakeEvent = (FAKE_EVENT *)Event;
  if ((Event == FailCloseEvent) ||
      ((FailCloseEventIndex >= 0) &&
       ((UINTN)FailCloseEventIndex < (sizeof(Events) / sizeof(Events[0]))) &&
       (FakeEvent == &Events[FailCloseEventIndex]))) {
    return CloseFailureStatus;
  }
  FakeEvent->Closed = TRUE;
  ++FakeEvent->CloseCount;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
FakeLocateProtocol (
  IN EFI_GUID  *Protocol,
  IN VOID      *Registration OPTIONAL,
  OUT VOID     **Interface
  )
{
  (VOID)Registration;
  ++LocateProtocolCalls;
  CHECK(Protocol != NULL);
  CHECK(memcmp(Protocol, &gMtkMusbHostPlatformReadyProtocolGuid,
               sizeof(EFI_GUID)) == 0);
  if (EFI_ERROR(LocateProtocolStatus)) {
    return LocateProtocolStatus;
  }
  CHECK(Interface != NULL);
  *Interface = LocatedProtocol;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
FakeLocateHandleBuffer (
  IN EFI_LOCATE_SEARCH_TYPE SearchType,
  IN EFI_GUID *Protocol,
  IN VOID *SearchKey OPTIONAL,
  OUT UINTN *Count,
  OUT EFI_HANDLE **Handles
  )
{
  (VOID)SearchKey;
  CHECK(SearchType == ByProtocol);
  CHECK(memcmp(Protocol, &gMtkMusbHostPlatformReadyProtocolGuid,
               sizeof(*Protocol)) == 0);
  if (EFI_ERROR(LocateProtocolStatus)) return LocateProtocolStatus;
  *Handles = malloc(2U * sizeof(**Handles));
  CHECK(*Handles != NULL);
  (*Handles)[0] = (EFI_HANDLE)(UINTN)0xBADU;
  (*Handles)[1] = (EFI_HANDLE)(UINTN)0x6789U;
  *Count = 2U;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
FakeHandleProtocol (
  IN EFI_HANDLE Handle,
  IN EFI_GUID *Protocol,
  OUT VOID **Interface
  )
{
  if (Handle == (EFI_HANDLE)(UINTN)0xBADU) return EFI_NOT_FOUND;
  CHECK(Handle == (EFI_HANDLE)(UINTN)0x6789U);
  if (memcmp(Protocol, &gMtkMusbHostPlatformReadyProtocolGuid,
             sizeof(*Protocol)) == 0) {
    *Interface = LocatedProtocol;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR(ProtocolLookupStatus)) return ProtocolLookupStatus;
  if (memcmp(Protocol, &gEfiUsb2HcProtocolGuid, sizeof(*Protocol)) == 0) {
    if (InstalledUsbHc == NULL) return EFI_UNSUPPORTED;
    *Interface = InstalledUsbHc;
    return EFI_SUCCESS;
  }
  if (memcmp(Protocol, &gEfiDevicePathProtocolGuid, sizeof(*Protocol)) == 0) {
    if (InstalledDevicePath == NULL) return EFI_UNSUPPORTED;
    *Interface = InstalledDevicePath;
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

static EFI_STATUS
EFIAPI
FakeRegisterProtocolNotify (
  IN EFI_GUID  *Protocol,
  IN EFI_EVENT Event,
  OUT VOID     **Registration
  )
{
  CHECK(Protocol != NULL);
  CHECK(memcmp(Protocol, &gMtkMusbHostPlatformReadyProtocolGuid,
               sizeof(EFI_GUID)) == 0);
  CHECK(Event != NULL);
  if (EFI_ERROR(RegisterProtocolNotifyStatus)) {
    return RegisterProtocolNotifyStatus;
  }
  CHECK(Registration != NULL);
  *Registration = Event;
  return EFI_SUCCESS;
}

static void
ResetEvents(void)
{
  memset(Events, 0, sizeof(Events));
  EventCount = 0U;
  memset(&Services, 0, sizeof(Services));
  InstallStatus = EFI_SUCCESS;
  UninstallStatus = EFI_SUCCESS;
  UninstallCalls = 0U;
  SingleUninstallCalls = 0U;
  SingleUninstallStatus = EFI_SUCCESS;
  InstallFailureMask = 0U;
  ProtocolLookupStatus = EFI_SUCCESS;
  InstalledUsbHc = NULL;
  InstalledDevicePath = NULL;
  FailCloseEvent = NULL;
  FailCloseEventIndex = -1;
  CloseFailureStatus = EFI_SUCCESS;
  FailTimerEvent = NULL;
  TimerFailureStatus = EFI_SUCCESS;
  LocateProtocolStatus = EFI_NOT_FOUND;
  LocatedProtocol = NULL;
  RegisterProtocolNotifyStatus = EFI_SUCCESS;
  LocateProtocolCalls = 0U;
  mReadyNotifyEvent = NULL;
  mReadyRegistration = NULL;
  mHostStarted = FALSE;
  mHostInstallBlocked = FALSE;
  mReadyNotifyDisabled = FALSE;
  mKeepImageResident = FALSE;
  Services.CreateEvent = FakeCreateEvent;
  Services.CreateEventEx = FakeCreateEventEx;
  Services.InstallMultipleProtocolInterfaces =
    FakeInstallMultipleProtocolInterfaces;
  Services.UninstallMultipleProtocolInterfaces =
    FakeUninstallMultipleProtocolInterfaces;
  Services.UninstallProtocolInterface = FakeUninstallProtocolInterface;
  Services.SetTimer = FakeSetTimer;
  Services.CloseEvent = FakeCloseEvent;
  Services.LocateProtocol = FakeLocateProtocol;
  Services.LocateHandleBuffer = FakeLocateHandleBuffer;
  Services.HandleProtocol = FakeHandleProtocol;
  Services.RegisterProtocolNotify = FakeRegisterProtocolNotify;
  gBS = &Services;
}

static EFI_STATUS EFIAPI
ReadySignal(EFI_EVENT Event)
{
  FAKE_EVENT *Fake = Event;
  CHECK(Fake && Fake->Notify && !Fake->Closed);
  Fake->Notify(Event, Fake->NotifyContext);
  return EFI_SUCCESS;
}

static int
IsIndexed(uint32_t Offset)
{
  return (Offset >= TEST_EP_BASE) && (Offset < TEST_EP_BASE + 0x10U);
}

static UINT8
ReadFifoByte(FAKE_MUSB *Fake)
{
  if (Fake->RxPosition < Fake->RxLength) {
    return Fake->RxData[Fake->RxPosition++];
  }
  return 0U;
}

static int
AsyncFakeRead8(void *Context, uint32_t Offset, uint8_t *Value)
{
  FAKE_MUSB *Fake;

  Fake = (FAKE_MUSB *)Context;
  if ((Fake == NULL) || (Value == NULL) || (Offset >= TEST_REG_BYTES)) {
    return 0;
  }
  if (Offset == TEST_INDEX) {
    *Value = Fake->Index;
  } else if (Offset == TEST_INTRUSB) {
    *Value = Fake->Registers[Offset];
  } else if (IsIndexed(Offset)) {
    *Value = Fake->Indexed[Fake->Index][Offset - TEST_EP_BASE];
  } else if ((Offset == TEST_FIFO_BASE) ||
             (Offset == TEST_FIFO_BASE + 4U)) {
    *Value = ReadFifoByte(Fake);
  } else {
    *Value = Fake->Registers[Offset];
  }
  return 1;
}

static void
CompleteAsyncFakeRx(FAKE_MUSB *Fake)
{
  Fake->RxLength = Fake->RxResponseLength;
  Fake->RxPosition = 0U;
  memcpy(Fake->RxData, Fake->RxResponse, Fake->RxResponseLength);
  // A successful RX has already ACKed and advanced the hardware PID.
  Fake->Registers[TEST_RXTOG] ^= (UINT8)(1U << 1U);
  Fake->Indexed[1][TEST_RXCSR] = TEST_RXPKTRDY;
  Fake->Indexed[1][TEST_RXCOUNT] = (UINT8)Fake->RxLength;
  Fake->Indexed[1][TEST_RXCOUNT + 1U] = (UINT8)(Fake->RxLength >> 8);
}

static int
AsyncFakeRead16(void *Context, uint32_t Offset, uint16_t *Value)
{
  FAKE_MUSB *Fake;

  Fake = (FAKE_MUSB *)Context;
  if ((Fake == NULL) || (Value == NULL) ||
      (Offset + 2U > TEST_REG_BYTES)) {
    return 0;
  }
  if ((Fake->Index == 1U) && (Offset == TEST_EP_BASE + TEST_RXCSR) &&
      ((Fake->Indexed[1][TEST_RXCSR] & TEST_H_REQPKT) != 0U) &&
      (Fake->TimeUs >= Fake->RxReadyAtUs)) {
    CompleteAsyncFakeRx(Fake);
  }
  if (IsIndexed(Offset)) {
    *Value = (UINT16)Fake->Indexed[Fake->Index][Offset - TEST_EP_BASE] |
             (UINT16)((UINT16)Fake->Indexed[Fake->Index]
                       [Offset + 1U - TEST_EP_BASE] << 8);
  } else if ((Offset == TEST_FIFO_BASE) ||
             (Offset == TEST_FIFO_BASE + 4U)) {
    UINT8 Low;
    UINT8 High;

    Low = ReadFifoByte(Fake);
    High = ReadFifoByte(Fake);
    *Value = (UINT16)Low | (UINT16)((UINT16)High << 8);
  } else {
    *Value = (UINT16)Fake->Registers[Offset] |
             (UINT16)((UINT16)Fake->Registers[Offset + 1U] << 8);
  }
  return 1;
}

static int
AsyncFakeRead32(void *Context, uint32_t Offset, uint32_t *Value)
{
  FAKE_MUSB *Fake;

  Fake = (FAKE_MUSB *)Context;
  if ((Fake == NULL) || (Value == NULL) ||
      (Offset + 4U > TEST_REG_BYTES)) {
    return 0;
  }
  if ((Offset == TEST_FIFO_BASE) ||
      (Offset == TEST_FIFO_BASE + 4U)) {
    UINT8 B0;
    UINT8 B1;
    UINT8 B2;
    UINT8 B3;

    B0 = ReadFifoByte(Fake);
    B1 = ReadFifoByte(Fake);
    B2 = ReadFifoByte(Fake);
    B3 = ReadFifoByte(Fake);
    *Value = (UINT32)B0 | ((UINT32)B1 << 8) |
             ((UINT32)B2 << 16) | ((UINT32)B3 << 24);
  } else {
    *Value = (UINT32)Fake->Registers[Offset] |
             ((UINT32)Fake->Registers[Offset + 1U] << 8) |
             ((UINT32)Fake->Registers[Offset + 2U] << 16) |
             ((UINT32)Fake->Registers[Offset + 3U] << 24);
  }
  return 1;
}

static int
AsyncFakeWrite8(void *Context, uint32_t Offset, uint8_t Value)
{
  FAKE_MUSB *Fake;

  Fake = (FAKE_MUSB *)Context;
  if ((Fake == NULL) || (Offset >= TEST_REG_BYTES)) {
    return 0;
  }
  if (Offset == TEST_INDEX) {
    Fake->Index = Value;
  } else if (Offset == TEST_INTRUSB) {
    Fake->Registers[Offset] &= (UINT8)~Value;
  } else if (IsIndexed(Offset)) {
    Fake->Indexed[Fake->Index][Offset - TEST_EP_BASE] = Value;
  } else {
    Fake->Registers[Offset] = Value;
  }
  return 1;
}

static int
AsyncFakeWrite16(void *Context, uint32_t Offset, uint16_t Value)
{
  FAKE_MUSB *Fake;
  UINT8 *Csr;

  Fake = (FAKE_MUSB *)Context;
  if ((Fake == NULL) || (Offset + 2U > TEST_REG_BYTES)) {
    return 0;
  }
  if (IsIndexed(Offset) && (Offset == TEST_EP_BASE + 2U) &&
      (Fake->Index <= 1U)) {
    // The tests below use no-data EP0 requests and successful PIO TX.
    // Model completion after setup/status or TXPKTRDY, as the core fake does.
    Fake->Indexed[Fake->Index][2U] = 0U;
    Fake->Indexed[Fake->Index][3U] = 0U;
    if (Fake->Index == 0U) {
      Fake->Indexed[0][TEST_RXCOUNT] = 0U;
      Fake->Indexed[0][TEST_RXCOUNT + 1U] = 0U;
    }
  } else if (IsIndexed(Offset) && (Fake->Index == 1U) &&
      (Offset == TEST_EP_BASE + TEST_RXCSR)) {
    Csr = &Fake->Indexed[1][TEST_RXCSR];
    if ((Value & TEST_FLUSHFIFO) != 0U) {
      *Csr = 0U;
      Fake->RxLength = 0U;
      Fake->RxPosition = 0U;
    } else if ((Value & TEST_H_REQPKT) != 0U) {
      ++Fake->RxRequests;
      if (Fake->RxStall) {
        Fake->RxStall = 0U;
        *Csr = TEST_H_RXSTALL;
      } else if (Fake->RxSuccess) {
        Fake->RxReadyAtUs = Fake->TimeUs + Fake->RxDelayUs;
        *Csr = TEST_H_REQPKT;
      } else {
        *Csr = TEST_DATAERROR;
      }
    } else {
      *Csr = (UINT8)Value;
    }
  } else if (IsIndexed(Offset)) {
    UINT8 Index;

    Index = (UINT8)(Offset - TEST_EP_BASE);
    Fake->Indexed[Fake->Index][Index] = (UINT8)Value;
    Fake->Indexed[Fake->Index][Index + 1U] = (UINT8)(Value >> 8);
  } else {
    Fake->Registers[Offset] = (UINT8)Value;
    Fake->Registers[Offset + 1U] = (UINT8)(Value >> 8);
  }
  return 1;
}

static int
AsyncFakeWrite32(void *Context, uint32_t Offset, uint32_t Value)
{
  FAKE_MUSB *Fake;
  UINT8 *Bytes;
  UINTN Index;

  Fake = (FAKE_MUSB *)Context;
  if ((Fake == NULL) || (Offset + 4U > TEST_REG_BYTES)) {
    return 0;
  }
  Bytes = &Fake->Registers[Offset];
  if (Offset == TEST_RXTOG || Offset == TEST_TXTOG) {
    UINT16 Current = (UINT16)Bytes[0] | ((UINT16)Bytes[1] << 8);
    UINT16 Enable = (UINT16)(Value >> 16);
    Current = (UINT16)((Current & ~Enable) | ((UINT16)Value & Enable));
    Bytes[0] = (UINT8)Current;
    Bytes[1] = (UINT8)(Current >> 8);
    Bytes[2] = Bytes[3] = 0U;
    return 1;
  }
  for (Index = 0U; Index < 4U; ++Index) {
    Bytes[Index] = (UINT8)(Value >> (Index * 8U));
  }
  return 1;
}

static uint64_t
AsyncFakeNowUs(void *Context)
{
  return ((FAKE_MUSB *)Context)->TimeUs;
}

static void
AsyncFakeDelayUs(void *Context, uint32_t Microseconds)
{
  ((FAKE_MUSB *)Context)->TimeUs += Microseconds;
}

static int
AsyncFakeTryLock(void *Context)
{
  FAKE_MUSB *Fake;

  Fake = (FAKE_MUSB *)Context;
  if (Fake->LockHeld) {
    return 0;
  }
  Fake->LockHeld = 1U;
  return 1;
}

static void
AsyncFakeUnlock(void *Context)
{
  ((FAKE_MUSB *)Context)->LockHeld = 0U;
}

UINT8 EFIAPI MmioRead8(UINTN Address)
{
  UINT8 Value;
  CHECK(Address >= MTK_MUSB_CONTROLLER_BASE);
  CHECK(AsyncFakeRead8(&Bus,
                       (UINT32)(Address - MTK_MUSB_CONTROLLER_BASE),
                       &Value));
  return Value;
}

UINT16 EFIAPI MmioRead16(UINTN Address)
{
  UINT16 Value;
  CHECK(Address >= MTK_MUSB_CONTROLLER_BASE);
  CHECK(AsyncFakeRead16(&Bus,
                        (UINT32)(Address - MTK_MUSB_CONTROLLER_BASE),
                        &Value));
  return Value;
}

UINT32 EFIAPI MmioRead32(UINTN Address)
{
  UINT32 Value;
  CHECK(Address >= MTK_MUSB_CONTROLLER_BASE);
  CHECK(AsyncFakeRead32(&Bus,
                        (UINT32)(Address - MTK_MUSB_CONTROLLER_BASE),
                        &Value));
  return Value;
}

UINT8 EFIAPI MmioWrite8(UINTN Address, UINT8 Value)
{
  CHECK(Address >= MTK_MUSB_CONTROLLER_BASE);
  CHECK(AsyncFakeWrite8(&Bus,
                        (UINT32)(Address - MTK_MUSB_CONTROLLER_BASE),
                        Value));
  return Value;
}

UINT16 EFIAPI MmioWrite16(UINTN Address, UINT16 Value)
{
  CHECK(Address >= MTK_MUSB_CONTROLLER_BASE);
  CHECK(AsyncFakeWrite16(&Bus,
                         (UINT32)(Address - MTK_MUSB_CONTROLLER_BASE),
                         Value));
  return Value;
}

UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  CHECK(Address >= MTK_MUSB_CONTROLLER_BASE);
  CHECK(AsyncFakeWrite32(&Bus,
                         (UINT32)(Address - MTK_MUSB_CONTROLLER_BASE),
                         Value));
  return Value;
}

VOID *EFIAPI AllocateZeroPool(UINTN Size)
{
  return calloc(1U, Size);
}

VOID EFIAPI FreePool(VOID *Buffer)
{
  free(Buffer);
}

VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Size)
{
  return memset(Buffer, 0, Size);
}

GUID *EFIAPI CopyGuid(GUID *Destination, CONST GUID *Source)
{
  return (GUID *)memcpy(Destination, Source, sizeof(*Destination));
}

UINT16 EFIAPI SetDevicePathNodeLength(VOID *Node, UINTN Length)
{
  EFI_DEVICE_PATH_PROTOCOL *Header;

  Header = (EFI_DEVICE_PATH_PROTOCOL *)Node;
  Header->Length[0] = (UINT8)Length;
  Header->Length[1] = (UINT8)(Length >> 8);
  return (UINT16)Length;
}

UINT64 EFIAPI GetPerformanceCounter(VOID)
{
  return Bus.TimeUs;
}

UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks)
{
  return Ticks * 1000U;
}

UINTN EFIAPI MicroSecondDelay(UINTN Microseconds)
{
  Bus.TimeUs += Microseconds;
  return Microseconds;
}

static void
PrepareFixture(ASYNC_FIXTURE *Fixture)
{
  MUSB_HOST_IO Io;
  MUSB_HOST_CONFIG Config;
  MUSB_HOST_DXE *HostContext;

  ResetEvents();
  memset(&Bus, 0, sizeof(Bus));
  Bus.Registers[TEST_DEVCTL] = 0x5DU;
  Bus.Registers[TEST_INTRUSB] = 0x10U;
  Bus.RxResponseLength = 8U;
  Bus.RxResponse[0] = 0x11U;
  Bus.RxResponse[1] = 0x22U;
  Bus.RxResponse[2] = 0x33U;
  Bus.RxResponse[3] = 0x44U;
  Bus.RxResponse[4] = 0x55U;
  Bus.RxResponse[5] = 0x66U;
  Bus.RxResponse[6] = 0x77U;
  Bus.RxResponse[7] = 0x88U;

  memset(Fixture, 0, sizeof(*Fixture));
  HostContext = (MUSB_HOST_DXE *)calloc(1U, sizeof(*HostContext));
  CHECK(HostContext != NULL);
  HostContext->Signature = MUSB_DXE_SIGNATURE;
  HostContext->UsbHc.AsyncInterruptTransfer = MusbAsyncInterruptTransfer;
  EfiInitializeLock(&HostContext->CoreLock, TPL_NOTIFY);
  EfiInitializeLock(&HostContext->AsyncLock, TPL_CALLBACK);
  Fixture->Host = HostContext;

  memset(&Io, 0, sizeof(Io));
  Io.Context = &Bus;
  Io.ControllerLength = 0x10000U;
  Io.Read8 = AsyncFakeRead8;
  Io.Read16 = AsyncFakeRead16;
  Io.Read32 = AsyncFakeRead32;
  Io.Write8 = AsyncFakeWrite8;
  Io.Write16 = AsyncFakeWrite16;
  Io.Write32 = AsyncFakeWrite32;
  Io.NowUs = AsyncFakeNowUs;
  Io.DelayUs = AsyncFakeDelayUs;
  Io.TryLock = AsyncFakeTryLock;
  Io.Unlock = AsyncFakeUnlock;

  memset(&Config, 0, sizeof(Config));
  Config.Revision = MUSB_HOST_CORE_REVISION;
  Config.Flags = MUSB_READY_REQUIRED;
  Config.ControllerLength = 0x10000U;
  Config.PhyLength = 0x10000U;
  Config.InterruptVector = MUSB_HOST_GSIV;
  Config.EndpointCount = MUSB_HOST_MAX_ENDPOINTS;
  Config.PortCount = MUSB_HOST_PORT_COUNT;
  Config.FifoProfile = MUSB_HOST_T70_FIFO_PROFILE;
  Config.InterruptMask = MUSB_HOST_CONNECT_DISCONNECT_MASK;
  CHECK(MusbHostInitialize(&HostContext->Host, &Io, &Config) == MusbOk);
  CHECK(MusbHostSetState(&HostContext->Host,
                         MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
}

static void
EnableFixturePort(ASYNC_FIXTURE *Fixture)
{
  CHECK(MusbHostSetPortFeature(&Fixture->Host->Host, 0U,
                               MUSB_PORT_FEATURE_RESET) == MusbOk);
}

static void
CleanupFixture(ASYNC_FIXTURE *Fixture)
{
  free(Fixture->Host);
  Fixture->Host = NULL;
}

static EFI_STATUS
EFIAPI
AsyncCallback (
  IN VOID    *Data,
  IN UINTN   DataLength,
  IN VOID    *Context,
  IN UINT32  Status
  )
{
  ASYNC_FIXTURE *Fixture;
  EFI_STATUS StatusValue;

  Fixture = (ASYNC_FIXTURE *)Context;
  CHECK(Fixture != NULL);
  CHECK(Fixture->Host->AsyncLock.Lock == EfiLockReleased);
  CHECK(Data != NULL);
  ++Fixture->CallbackCount;
  Fixture->LastDataLength = DataLength;
  Fixture->LastResult = Status;

  if (Status == EFI_USB_NOERROR) {
    ++Fixture->SuccessCount;
    CHECK(DataLength == 8U);
    CHECK(((UINT8 *)Data)[0] == 0x11U);
    CHECK(((UINT8 *)Data)[7] == 0x88U);
  } else if (Status == EFI_USB_ERR_NAK) {
    ++Fixture->NakCount;
    CHECK(DataLength == 0U);
  } else if (Status == EFI_USB_ERR_STALL) {
    ++Fixture->StallCount;
  } else if (Status == EFI_USB_ERR_SYSTEM) {
    ++Fixture->SystemCount;
  }

  if (Fixture->Mode == 1U) {
    EFI_EVENT OldEvent;
    UINT8 *OldToggle;

    OldEvent = Fixture->Host->AsyncSlots[0].AsyncEvent;
    OldToggle = Fixture->OldToggle;
    StatusValue = Fixture->Host->UsbHc.AsyncInterruptTransfer(
                    &Fixture->Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
                    8U, FALSE, OldToggle, 0U, 0U, NULL, NULL, NULL);
    CHECK(StatusValue == EFI_SUCCESS);
    Fixture->CanceledToggle = *OldToggle;
    free(OldToggle);
    Fixture->OldToggle = NULL;
    Fixture->NewToggle = Fixture->CarryToggleOnReregister?
      Fixture->CanceledToggle:0U;
    StatusValue = Fixture->Host->UsbHc.AsyncInterruptTransfer(
                    &Fixture->Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
                    8U, TRUE, &Fixture->NewToggle, 1U, 8U, NULL,
                    AsyncCallback, Fixture);
    CHECK(StatusValue == EFI_SUCCESS);
    CHECK(Fixture->Host->AsyncSlots[0].AsyncEvent != OldEvent);
    Fixture->Mode = 2U;
  } else if (Fixture->Mode == 2U) {
    StatusValue = Fixture->Host->UsbHc.AsyncInterruptTransfer(
                    &Fixture->Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
                    8U, FALSE, &Fixture->NewToggle, 0U, 0U, NULL, NULL, NULL);
    CHECK(StatusValue == EFI_SUCCESS);
    Fixture->Mode = 0U;
  }
  return Fixture->CallbackStatus;
}

static EFI_EVENT
RegisterAsync(ASYNC_FIXTURE *Fixture, UINT8 *Toggle)
{
  EFI_STATUS Status;

  Status = Fixture->Host->UsbHc.AsyncInterruptTransfer(
             &Fixture->Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
             8U, TRUE, Toggle, 1U, 8U, NULL, AsyncCallback, Fixture);
  CHECK(Status == EFI_SUCCESS);
  CHECK(Fixture->Host->AsyncSlots[0].AsyncEvent != NULL);
  CHECK(((FAKE_EVENT *)Fixture->Host->AsyncSlots[0].AsyncEvent)->TimerType == TimerPeriodic);
  CHECK(((FAKE_EVENT *)Fixture->Host->AsyncSlots[0].AsyncEvent)->TriggerTime == 10000U);
  return Fixture->Host->AsyncSlots[0].AsyncEvent;
}

static void
Trigger(EFI_EVENT Event)
{
  FAKE_EVENT *FakeEvent;

  FakeEvent = (FAKE_EVENT *)Event;
  CHECK(!FakeEvent->Closed);
  CHECK(FakeEvent->Notify != NULL);
  FakeEvent->Notify(Event, FakeEvent->NotifyContext);
}

static void
TestAsyncCancelReregister(BOOLEAN Stall)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT OldEvent;
  EFI_EVENT NewEvent;
  UINT8 *Toggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle = (UINT8 *)malloc(sizeof(*Toggle));
  CHECK(Toggle != NULL);
  *Toggle = 0U;
  Fixture.OldToggle = Toggle;
  Fixture.Mode = 1U;
  Fixture.CallbackStatus = EFI_DEVICE_ERROR;
  OldEvent = RegisterAsync(&Fixture, Toggle);
  Bus.RxSuccess = Stall ? 0U : 1U;
  Bus.RxStall = Stall;
  Trigger(OldEvent);

  CHECK(Fixture.CallbackCount == 1U);
  CHECK(Fixture.SuccessCount == (Stall ? 0U : 1U));
  CHECK(Fixture.StallCount == (Stall ? 1U : 0U));
  CHECK(Fixture.NakCount == 0U);
  CHECK(((FAKE_EVENT *)OldEvent)->Closed);
  CHECK(((FAKE_EVENT *)OldEvent)->CloseCount == 1U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncInCallback);
  NewEvent = Fixture.Host->AsyncSlots[0].AsyncEvent;
  CHECK(NewEvent != NULL && NewEvent != OldEvent);
  CHECK(!((FAKE_EVENT *)NewEvent)->Closed);

  Fixture.CallbackStatus = EFI_SUCCESS;
  Bus.RxStall = 0U;
  Bus.RxSuccess = 1U;
  Trigger(NewEvent);
  CHECK(Fixture.CallbackCount == 2U);
  CHECK(Fixture.SuccessCount == (Stall ? 1U : 2U));
  CHECK(Fixture.NakCount == 0U);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NULL);
  CHECK(((FAKE_EVENT *)NewEvent)->Closed);
  CHECK(((FAKE_EVENT *)NewEvent)->CloseCount == 1U);
  CHECK(Fixture.OldToggle == NULL);
  CHECK(Fixture.CanceledToggle == (Stall?0U:1U));
  CleanupFixture(&Fixture);
}

static void
TestAsyncOverflowToggleSurvivesCancelReregister(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT OldEvent;
  EFI_EVENT NewEvent;
  UINT8 *Toggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle=(UINT8 *)malloc(sizeof(*Toggle));CHECK(Toggle!=NULL);*Toggle=0U;
  Fixture.OldToggle=Toggle;Fixture.Mode=1U;Fixture.CarryToggleOnReregister=TRUE;
  Fixture.CallbackStatus=EFI_DEVICE_ERROR;
  OldEvent=RegisterAsync(&Fixture,Toggle);
  Bus.RxResponseLength=9U;Bus.RxSuccess=1U;
  Trigger(OldEvent);
  CHECK(Fixture.CallbackCount==1U&&Fixture.LastResult==EFI_USB_ERR_BUFFER&&
        Fixture.CanceledToggle==1U&&Fixture.NewToggle==1U);
  CHECK(((FAKE_EVENT *)OldEvent)->Closed&&
        Fixture.Host->AsyncSlots[0].AsyncActive&&
        Fixture.Host->AsyncSlots[0].AsyncToggle==1U);
  NewEvent=Fixture.Host->AsyncSlots[0].AsyncEvent;
  CHECK(NewEvent&&NewEvent!=OldEvent&&!((FAKE_EVENT *)NewEvent)->Closed);
  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc,1U,0x81U,EFI_USB_SPEED_HIGH,8U,FALSE,
          &Fixture.NewToggle,0U,0U,NULL,NULL,NULL)==EFI_SUCCESS);
  CHECK(Fixture.NewToggle==1U&&!Fixture.Host->AsyncSlots[0].AsyncActive&&
        ((FAKE_EVENT *)NewEvent)->Closed);
  CleanupFixture(&Fixture);
}

static void
TestAsyncIdleKeepsPolling(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT Event;
  UINT8 Toggle = 0U;
  UINT64 Generation;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Fixture.CallbackStatus = EFI_DEVICE_ERROR;
  Event = RegisterAsync(&Fixture, &Toggle);
  Generation = Fixture.Host->AsyncSlots[0].AsyncGeneration;
  Bus.RxSuccess = 0U;
  for (UINTN Index = 0U; Index < 32U; ++Index) {
    UINT32 Requests = Bus.RxRequests;
    UINT64 StartedUs = Bus.TimeUs;

    Trigger(Event);
    CHECK(Bus.RxRequests == Requests + 1U && Bus.TimeUs == StartedUs);
    CHECK(Fixture.CallbackCount == 0U);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
    CHECK(!Fixture.Host->AsyncSlots[0].AsyncInCallback);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == Event);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncGeneration == Generation);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 0U);
    CHECK(!((FAKE_EVENT *)Event)->Closed);
  }
  Fixture.CallbackStatus = EFI_SUCCESS;
  Bus.RxSuccess = 1U;
  Trigger(Event);
  CHECK(Fixture.CallbackCount == 1U && Fixture.SuccessCount == 1U);
  CHECK(Bus.Indexed[1][TEST_RXINTERVAL] == 1U);
  CHECK(Fixture.NakCount == 0U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, FALSE, &Toggle, 0U, 0U, NULL,
          NULL, NULL) == EFI_SUCCESS);
  CHECK(Toggle == 1U && ((FAKE_EVENT *)Event)->Closed);
  CleanupFixture(&Fixture);
}

EFI_STATUS EFIAPI UsbOnHubInterrupt(
  VOID *Data, UINTN DataLength, VOID *Context, UINT32 Result);

static UINT32 HubSignals;
static UINT32 HubHaltClears;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...)
{ (VOID)Level; (VOID)Format; }
VOID *EFIAPI CopyMem(VOID *Destination, CONST VOID *Source, UINTN Length)
{ return memcpy(Destination, Source, Length); }

static EFI_TPL EFIAPI HubRaiseTpl(EFI_TPL NewTpl)
{ CHECK(NewTpl == USB_BUS_TPL); return TPL_CALLBACK; }
static VOID EFIAPI HubRestoreTpl(EFI_TPL OldTpl)
{ CHECK(OldTpl == TPL_CALLBACK); }
static EFI_STATUS EFIAPI HubSignal(EFI_EVENT Event)
{ CHECK(Event == (EFI_EVENT)(UINTN)0x1234U); ++HubSignals; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI HubClearHalt(
  EFI_USB_IO_PROTOCOL *This, EFI_USB_DEVICE_REQUEST *Request,
  EFI_USB_DATA_DIRECTION Direction, UINT32 Timeout, VOID *Data,
  UINTN Length, UINT32 *Result)
{
  USB_INTERFACE *Interface = USB_INTERFACE_FROM_USBIO(This);
  CHECK(Request->Request == USB_REQ_CLEAR_FEATURE);
  CHECK(Request->RequestType == USB_REQUEST_TYPE(
    EfiUsbNoData, USB_REQ_TYPE_STANDARD, USB_TARGET_ENDPOINT));
  CHECK(Request->Value == USB_FEATURE_ENDPOINT_HALT && Request->Index == 0x81U);
  CHECK(Direction == EfiUsbNoData && Timeout != 0U && !Data && Length == 0U);
  ++HubHaltClears;
  Interface->HubEp->Toggle = 0U;
  Bus.RxStall = 0U;
  *Result = EFI_USB_NOERROR;
  return EFI_SUCCESS;
}

static void
TestEdk2HubIdleAndErrorRecovery(BOOLEAN PollIdle)
{
  ASYNC_FIXTURE Fixture;
  USB_BUS UsbBus = {0};
  USB_DEVICE Device = {0};
  USB_INTERFACE Interface = {0};
  USB_INTERFACE_SETTING Setting = {0};
  USB_ENDPOINT_DESC Endpoint = {0};
  USB_ENDPOINT_DESC *Endpoints[] = {&Endpoint};
  EFI_EVENT OldEvent, NewEvent;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Services.RaiseTPL = HubRaiseTpl;
  Services.RestoreTPL = HubRestoreTpl;
  Services.SignalEvent = HubSignal;
  HubSignals = HubHaltClears = 0U;
  UsbBus.Usb2Hc = &Fixture.Host->UsbHc;
  Device.Bus = &UsbBus;
  Device.Connected = TRUE;
  Device.Address = 1U;
  Device.Speed = EFI_USB_SPEED_HIGH;
  Interface.Signature = USB_INTERFACE_SIGNATURE;
  Interface.Device = &Device;
  Interface.IfSetting = &Setting;
  Interface.HubEp = &Endpoint;
  Interface.NumOfPort = 7U;
  Interface.HubNotify = (EFI_EVENT)(UINTN)0x1234U;
  Interface.UsbIo.UsbAsyncInterruptTransfer = UsbIoAsyncInterruptTransfer;
  Interface.UsbIo.UsbControlTransfer = HubClearHalt;
  Setting.Desc.NumEndpoints = 1U;
  Setting.Endpoints = Endpoints;
  Endpoint.Desc.EndpointAddress = 0x81U;
  Endpoint.Desc.Attributes = USB_ENDPOINT_INTERRUPT;
  Endpoint.Desc.MaxPacketSize = 8U;
  CHECK(UsbIoAsyncInterruptTransfer(&Interface.UsbIo, 0x81U, TRUE,
          USB_HUB_POLL_INTERVAL, 1U, UsbOnHubInterrupt, &Interface) == EFI_SUCCESS);
  OldEvent = Fixture.Host->AsyncSlots[0].AsyncEvent;
  Bus.RxSuccess = 0U;
  for (UINTN Index = 0U; Index < (PollIdle ? 8U : 0U); ++Index) {
    Trigger(OldEvent);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == OldEvent);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncActive && HubSignals == 0U);
    CHECK(!((FAKE_EVENT *)OldEvent)->Closed);
  }
  Bus.RxSuccess = 1U;
  Bus.RxResponseLength = 1U;
  Bus.RxResponse[0] = 2U;
  Trigger(OldEvent);
  CHECK(HubSignals == 1U && Interface.ChangeMap && Interface.ChangeMap[0] == 2U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  FreePool(Interface.ChangeMap);
  Interface.ChangeMap = NULL;
  Bus.RxSuccess = 0U;
  Bus.RxStall = 1U;
  Trigger(OldEvent);
  CHECK(HubHaltClears == 1U);
  CHECK(((FAKE_EVENT *)OldEvent)->Closed);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  NewEvent = Fixture.Host->AsyncSlots[0].AsyncEvent;
  CHECK(NewEvent && NewEvent != OldEvent && !((FAKE_EVENT *)NewEvent)->Closed);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncCallback == UsbOnHubInterrupt);
  CHECK(Endpoint.Toggle == 0U && Fixture.Host->AsyncSlots[0].AsyncToggle == 0U);

  Bus.RxSuccess = 1U;
  Bus.RxResponseLength = 1U;
  Bus.RxResponse[0] = 2U;
  Trigger(NewEvent);
  CHECK(HubSignals == 2U && Interface.ChangeMap && Interface.ChangeMap[0] == 2U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  CHECK(UsbIoAsyncInterruptTransfer(&Interface.UsbIo, 0x81U, FALSE,
          0U, 0U, NULL, NULL) == EFI_SUCCESS);
  CHECK(Endpoint.Toggle == 1U && ((FAKE_EVENT *)NewEvent)->Closed);
  FreePool(Interface.ChangeMap);
  CleanupFixture(&Fixture);
}

static void
TestAsyncCallbackFailureReleasesSlot(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT Event;
  UINT8 Toggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle = 0U;
  Fixture.CallbackStatus = EFI_DEVICE_ERROR;
  Event = RegisterAsync(&Fixture, &Toggle);
  Bus.RxSuccess = 1U;
  Trigger(Event);
  CHECK(Fixture.CallbackCount == 1U);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncInCallback);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NULL);
  CHECK(Fixture.Host->AsyncLock.Lock == EfiLockReleased);
  CHECK(((FAKE_EVENT *)Event)->Closed);
  Fixture.CallbackStatus = EFI_SUCCESS;
  Event = RegisterAsync(&Fixture, &Toggle);
  Trigger(Event);
  CHECK(Fixture.CallbackCount == 2U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncInCallback);
  CleanupFixture(&Fixture);
}

static void
TestAsyncSuccessToggleAndExplicitCancel(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT Event;
  UINT8 Toggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle = 0U;
  Event = RegisterAsync(&Fixture, &Toggle);
  Bus.RxSuccess = 1U;
  Trigger(Event);
  CHECK(Fixture.CallbackCount == 1U);
  CHECK(Fixture.SuccessCount == 1U);
  CHECK(Toggle == 0U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  CHECK(Fixture.NewToggle == 0U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(!((FAKE_EVENT *)Event)->Closed);

  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
          8U, FALSE, &Toggle, 0U, 0U, NULL, NULL, NULL) == EFI_SUCCESS);
  CHECK(Toggle == 1U);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NULL);
  CHECK(((FAKE_EVENT *)Event)->Closed);
  CHECK(((FAKE_EVENT *)Event)->CloseCount == 1U);
  CleanupFixture(&Fixture);
}

static void
TestAsyncCancelCloseFailureDoesNotRunNewSlot(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT OldEvent;
  EFI_EVENT NewEvent;
  UINT8 OldToggle;
  UINT8 NewToggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  OldToggle = 0U;
  NewToggle = 0U;
  OldEvent = RegisterAsync(&Fixture, &OldToggle);
  FailTimerEvent = OldEvent;
  TimerFailureStatus = EFI_DEVICE_ERROR;
  FailCloseEvent = OldEvent;
  CloseFailureStatus = EFI_DEVICE_ERROR;

  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
          8U, FALSE, &OldToggle, 0U, 0U, NULL, NULL, NULL) == EFI_SUCCESS);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NULL);
  CHECK(!((FAKE_EVENT *)OldEvent)->Closed);
  CHECK(((FAKE_EVENT *)OldEvent)->TimerType == TimerPeriodic);

  NewEvent = RegisterAsync(&Fixture, &NewToggle);
  CHECK(NewEvent != OldEvent);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NewEvent);

  ((FAKE_EVENT *)OldEvent)->Notify(
    OldEvent, ((FAKE_EVENT *)OldEvent)->NotifyContext);
  CHECK(Fixture.CallbackCount == 0U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NewEvent);

  FailTimerEvent = NULL;
  TimerFailureStatus = EFI_SUCCESS;
  FailCloseEvent = NULL;
  CloseFailureStatus = EFI_SUCCESS;
  Bus.RxSuccess = 1U;
  Trigger(NewEvent);
  CHECK(Fixture.CallbackCount == 1U);
  CHECK(Fixture.SuccessCount == 1U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NewEvent);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);

  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
          8U, FALSE, &NewToggle, 0U, 0U, NULL, NULL, NULL) == EFI_SUCCESS);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == NULL);
  CleanupFixture(&Fixture);
}

static void
TestExitBootServicesQuiescesAsync(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT Event;
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready;
  UINT8 Toggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle = 0U;
  Event = RegisterAsync(&Fixture, &Toggle);
  FailTimerEvent = Event;
  TimerFailureStatus = EFI_DEVICE_ERROR;
  FailCloseEvent = Event;
  CloseFailureStatus = EFI_DEVICE_ERROR;
  Fixture.Host->AsyncLock.Lock = EfiLockAcquired;
  memset(&Ready, 0, sizeof(Ready));
  Ready.HostSnapshotValid = 1U;
  Fixture.Host->PlatformReady = &Ready;
  Bus.LockHeld = 1U;
  MusbExitBootServicesNotify(NULL, Fixture.Host);
  CHECK(Fixture.Host->ExitingBootServices);
  CHECK(Ready.HostSnapshotValid == 0U);
  Bus.LockHeld = 0U;
  CHECK(Fixture.Host->Host.State == MUSB_HOST_STATE_HALTED);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == Event);
  CHECK(!((FAKE_EVENT *)Event)->Closed);
  CHECK(((FAKE_EVENT *)Event)->CloseCount == 0U);
  CHECK(((FAKE_EVENT *)Event)->TimerType == TimerPeriodic);
  Fixture.Host->AsyncLock.Lock = EfiLockReleased;

  ((FAKE_EVENT *)Event)->Notify(Event, ((FAKE_EVENT *)Event)->NotifyContext);
  CHECK(Fixture.CallbackCount == 0U);
  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
          8U, FALSE, &Toggle, 0U, 0U, NULL, NULL, NULL) ==
        EFI_INVALID_PARAMETER);
  CleanupFixture(&Fixture);
}

static void
TestAsyncStallAndDisconnect(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT Event;
  UINT8 Toggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle = 0U;
  Event = RegisterAsync(&Fixture, &Toggle);
  Bus.RxStall = 1U;
  Trigger(Event);
  CHECK(Fixture.StallCount == 1U);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(((FAKE_EVENT *)Event)->Closed);
  CleanupFixture(&Fixture);

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle = 0U;
  Event = RegisterAsync(&Fixture, &Toggle);
  Bus.Registers[TEST_DEVCTL] = 0U;
  Trigger(Event);
  CHECK(Fixture.SystemCount == 1U);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(((FAKE_EVENT *)Event)->Closed);
  CleanupFixture(&Fixture);
}

static void
TestAsyncValidationAndBusy(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT Event;
  UINT8 Toggle;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Toggle = 0U;
  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc, 1U, 0x01U, EFI_USB_SPEED_HIGH,
          8U, TRUE, &Toggle, 1U, 8U, NULL, AsyncCallback, &Fixture) ==
        EFI_INVALID_PARAMETER);
  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
          8U, TRUE, &Toggle, 1001U, 8U, NULL, AsyncCallback, &Fixture) ==
        EFI_INVALID_PARAMETER);
  CHECK(Events[0].Notify == NULL);

  Event = RegisterAsync(&Fixture, &Toggle);
  Bus.LockHeld = 1U;
  Trigger(Event);
  CHECK(Fixture.CallbackCount == 0U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(!((FAKE_EVENT *)Event)->Closed);
  Bus.LockHeld = 0U;
  CHECK(Fixture.Host->UsbHc.AsyncInterruptTransfer(
          &Fixture.Host->UsbHc, 1U, 0x81U, EFI_USB_SPEED_HIGH,
          8U, FALSE, &Toggle, 0U, 0U, NULL, NULL, NULL) == EFI_SUCCESS);
  CHECK(((FAKE_EVENT *)Event)->Closed);
  CleanupFixture(&Fixture);
}

static void
InitializeReadyProtocol(MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL *Ready)
{
  memset(Ready, 0, sizeof(*Ready));
  Ready->Signature = MTK_MUSB_HOST_PLATFORM_READY_SIGNATURE;
  Ready->Revision = MTK_MUSB_HOST_PLATFORM_READY_REVISION;
  Ready->Length = sizeof(*Ready);
  Ready->Flags = MTK_MUSB_READY_REQUIRED;
  Ready->HostPowerPath = MTK_MUSB_HOST_POWER_PATH_TYPEC_SOURCE;
  Ready->ControllerBase = MTK_MUSB_CONTROLLER_BASE;
  Ready->ControllerLength = MTK_MUSB_CONTROLLER_LENGTH;
  Ready->PhyBase = MTK_MUSB_PHY_BASE;
  Ready->PhyLength = MTK_MUSB_PHY_LENGTH;
  Ready->InterruptVector = MTK_MUSB_GSIV;
  Ready->EndpointCount = MTK_MUSB_ENDPOINT_COUNT;
  Ready->PortCount = MTK_MUSB_ROOT_PORT_COUNT;
  Ready->FifoProfile = MTK_MUSB_FIFO_PROFILE_T70PRO_HOST_V1;
  Ready->InterruptMask = MTK_MUSB_CONNECT_DISCONNECT_MASK;
}

static void
TestHostReleaseLifecycle(void)
{
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready;
  EFI_EVENT ExitEvent, AsyncEvent;
  MUSB_HOST_DXE *Context;
  ASYNC_FIXTURE Fixture = {0};
  UINT8 Toggle = 0U;
  ResetEvents();
  memset(&Bus, 0, sizeof(Bus));
  Bus.Registers[TEST_DEVCTL] = 0x5dU;
  InitializeReadyProtocol(&Ready);
  LocatedProtocol = &Ready; LocateProtocolStatus = EFI_SUCCESS;
  CHECK(MusbInstallHost(&Ready) == EFI_SUCCESS);
  Context = Ready.HostContext;
  CHECK(Context && Ready.ReleaseHost == MusbReleaseHost);
  Fixture.Host = Context;
  AsyncEvent = RegisterAsync(&Fixture, &Toggle);
  ExitEvent = Context->ExitBootServicesEvent;
  Context->CoreLock.Lock = EfiLockAcquired;
  CHECK(Ready.ReleaseHost(Context) == EFI_NOT_READY);
  CHECK(!UninstallCalls && mHostStarted && Context->ProtocolsInstalled);
  Context->CoreLock.Lock = EfiLockReleased;
  Context->AsyncSlots[0].AsyncInCallback = TRUE;
  CHECK(Ready.ReleaseHost(Context) == EFI_NOT_READY);
  CHECK(!UninstallCalls && !((FAKE_EVENT *)AsyncEvent)->Closed);
  Context->AsyncSlots[0].AsyncInCallback = FALSE;
  UninstallStatus = EFI_ACCESS_DENIED;
  CHECK(Ready.ReleaseHost(Context) == EFI_ACCESS_DENIED);
  CHECK(Context->ProtocolsInstalled && Context->Stopping && mHostStarted);
  CHECK(((FAKE_EVENT *)AsyncEvent)->Closed);
  ((FAKE_EVENT *)AsyncEvent)->Notify(AsyncEvent, ((FAKE_EVENT *)AsyncEvent)->NotifyContext);
  CHECK(Fixture.CallbackCount == 0U);
  UninstallStatus = EFI_SUCCESS;
  FailCloseEvent = ExitEvent; CloseFailureStatus = EFI_DEVICE_ERROR;
  CHECK(Ready.ReleaseHost(Context) == EFI_DEVICE_ERROR);
  CHECK(!Context->ProtocolsInstalled && Ready.HostContext == Context && mHostStarted);
  CHECK(UninstallCalls == 2U);
  FailCloseEvent = NULL;
  CHECK(Ready.ReleaseHost(Context) == EFI_SUCCESS);
  CHECK(!Ready.HostContext && !Ready.ReleaseHost && !mHostStarted);
  CHECK(UninstallCalls == 2U && ((FAKE_EVENT *)ExitEvent)->CloseCount == 1U);
  for (UINTN Cycle = 0U; Cycle < 3U; ++Cycle) {
    CHECK(MusbInstallHost(&Ready) == EFI_SUCCESS);
    CHECK(Ready.ReleaseHost(Ready.HostContext) == EFI_SUCCESS);
    CHECK(!Ready.HostContext && !mHostStarted);
  }

  // Exact old-sized allocation catches extension writes with ASAN/Unicorn.
  InitializeReadyProtocol(&Ready);
  Ready.Revision = MTK_MUSB_HOST_PLATFORM_READY_V3_REVISION;
  Ready.Length = MTK_MUSB_HOST_PLATFORM_READY_V3_LENGTH;
  VOID *Legacy = AllocateZeroPool(Ready.Length);
  memcpy(Legacy, &Ready, Ready.Length); LocatedProtocol = Legacy;
  CHECK(MusbInstallHost(Legacy) == EFI_SUCCESS);
  Context = MusbContextFromProtocol(InstalledUsbHc);
  Context->Stopping = TRUE; Context->Host.State = MUSB_HOST_STATE_HALTED;
  CHECK(gBS->UninstallMultipleProtocolInterfaces(Context->ControllerHandle,
    &gEfiUsb2HcProtocolGuid, &Context->UsbHc,
    &gEfiDevicePathProtocolGuid, &Context->VendorPath, NULL) == EFI_SUCCESS);
  CHECK(gBS->CloseEvent(Context->ExitBootServicesEvent) == EFI_SUCCESS);
  FreePool(Context); FreePool(Legacy); mHostStarted = FALSE;

  // A failed close from an earlier cancellation must also prevent freeing.
  InitializeReadyProtocol(&Ready); LocatedProtocol = &Ready;
  CHECK(MusbInstallHost(&Ready) == EFI_SUCCESS);
  Context = Ready.HostContext; Fixture.Host = Context;
  AsyncEvent = RegisterAsync(&Fixture, &Toggle);
  FailCloseEvent = AsyncEvent; CloseFailureStatus = EFI_DEVICE_ERROR;
  CHECK(MusbAsyncInterruptTransfer(&Context->UsbHc, 1U, 0x81U,
    EFI_USB_SPEED_HIGH, 8U, FALSE, &Toggle, 0U, 0U, NULL, NULL, NULL) == EFI_SUCCESS);
  CHECK(Context->AsyncCloseFailed);
  CHECK(Ready.ReleaseHost(Context) == EFI_DEVICE_ERROR);
  CHECK(Ready.HostContext == Context && mHostStarted && Context->ProtocolsInstalled);
  FailCloseEvent = NULL;
  CHECK(gBS->CloseEvent(AsyncEvent) == EFI_SUCCESS);
  CHECK(gBS->CloseEvent(Context->ExitBootServicesEvent) == EFI_SUCCESS);
  Context->Stopping = TRUE;
  CHECK(gBS->UninstallMultipleProtocolInterfaces(Context->ControllerHandle,
    &gEfiUsb2HcProtocolGuid, &Context->UsbHc,
    &gEfiDevicePathProtocolGuid, &Context->VendorPath, NULL) == EFI_SUCCESS);
  Context->Signature = 0U; FreePool(Context); mHostStarted = FALSE;
}

static void
TestReadyNotificationSurvivesInitialOrdering(void)
{
  for (UINTN InitialReady = 0U; InitialReady < 2U; ++InitialReady) {
    MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready[3];
    ResetEvents();
    Services.SignalEvent = ReadySignal;
    memset(&Bus, 0, sizeof(Bus));
    Bus.Registers[TEST_DEVCTL] = 0x5dU;
    InitializeReadyProtocol(&Ready[0]);
    LocatedProtocol = &Ready[0];
    LocateProtocolStatus = InitialReady ? EFI_SUCCESS : EFI_NOT_FOUND;
    CHECK(MusbHostDxeEntryPoint(NULL, NULL) == EFI_SUCCESS);
    EFI_EVENT Notify = mReadyNotifyEvent;
    CHECK(Notify && mReadyRegistration == Notify);
    CHECK(mHostStarted == (BOOLEAN)InitialReady);
    if (!InitialReady) {
      LocateProtocolStatus = EFI_SUCCESS;
      CHECK(ReadySignal(Notify) == EFI_SUCCESS);
    }
    for (UINTN Cycle = 0U; Cycle < ARRAY_SIZE(Ready); ++Cycle) {
      CHECK(mHostStarted && Ready[Cycle].ReleaseHost && Ready[Cycle].HostContext);
      UINTN Count = EventCount;
      CHECK(MusbHostDxeEntryPoint(NULL, NULL) == EFI_SUCCESS);
      CHECK(EventCount == Count && mReadyNotifyEvent == Notify);
      CHECK(Ready[Cycle].ReleaseHost(Ready[Cycle].HostContext) == EFI_SUCCESS);
      CHECK(!mHostStarted && !Ready[Cycle].ReleaseHost && !Ready[Cycle].HostContext);
      LocateProtocolStatus = EFI_NOT_FOUND;
      CHECK(ReadySignal(Notify) == EFI_SUCCESS);
      CHECK(!mHostStarted && !((FAKE_EVENT *)Notify)->Closed);
      if (Cycle + 1U < ARRAY_SIZE(Ready)) {
        InitializeReadyProtocol(&Ready[Cycle + 1U]);
        LocatedProtocol = &Ready[Cycle + 1U];
        LocateProtocolStatus = EFI_SUCCESS;
        CHECK(ReadySignal(Notify) == EFI_SUCCESS);
      }
    }
    CHECK(gBS->CloseEvent(Notify) == EFI_SUCCESS);
    mReadyNotifyEvent = NULL;
  }
}

static void
TestReadyProtocolPowerPaths(void)
{
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready;
  MUSB_HOST_CONFIG Config;

  InitializeReadyProtocol(&Ready);
  CHECK(MusbValidateReady(&Ready,&Config)==MusbOk);
  CHECK(Config.Flags==MTK_MUSB_READY_REQUIRED);

  Ready.HostPowerPath=MTK_MUSB_HOST_POWER_PATH_TYPEC_ONLY;
  CHECK(MusbValidateReady(&Ready,&Config)==MusbOk);
  CHECK(Config.Flags==MTK_MUSB_READY_REQUIRED);
  Ready.Flags=MTK_MUSB_READY_ID_LOW_REQUIRED;
  CHECK(MusbValidateReady(&Ready,&Config)==MusbNotReady);
  Ready.Flags=MTK_MUSB_READY_REQUIRED;

  Ready.Revision=MTK_MUSB_HOST_PLATFORM_READY_LEGACY_REVISION;
  Ready.HostPowerPath=MTK_MUSB_HOST_POWER_PATH_LEGACY_TYPEC;
  CHECK(MusbValidateReady(&Ready,&Config)==MusbOk);

  Ready.Revision=MTK_MUSB_HOST_PLATFORM_READY_REVISION;
  Ready.HostPowerPath=MTK_MUSB_HOST_POWER_PATH_ID_LOW_HUB;
  Ready.Flags=MTK_MUSB_READY_ID_LOW_REQUIRED;
  CHECK(MusbValidateReady(&Ready,&Config)==MusbOk);
  CHECK(Config.Flags==MTK_MUSB_READY_ID_LOW_REQUIRED);

  Ready.Flags=MTK_MUSB_READY_REQUIRED;
  CHECK(MusbValidateReady(&Ready,&Config)==MusbNotReady);
  Ready.HostPowerPath=MTK_MUSB_HOST_POWER_PATH_TYPEC_SOURCE;
  Ready.Flags=MTK_MUSB_READY_ID_LOW_REQUIRED;
  CHECK(MusbValidateReady(&Ready,&Config)==MusbNotReady);
  Ready.Revision=MTK_MUSB_HOST_PLATFORM_READY_LEGACY_REVISION;
  Ready.HostPowerPath=MTK_MUSB_HOST_POWER_PATH_LEGACY_TYPEC;
  Ready.Flags=MTK_MUSB_READY_ID_LOW_REQUIRED;
  CHECK(MusbValidateReady(&Ready,&Config)==MusbNotReady);
}

static void
TestInstallFailureCloseFailureKeepsImageResident(void)
{
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready;
  MUSB_HOST_DXE *Context;
  EFI_EVENT ExitEvent;

  ResetEvents();
  memset(&Bus, 0, sizeof(Bus));
  Bus.Registers[TEST_DEVCTL] = 0x5DU;
  Bus.Registers[TEST_INTRUSB] = 0x10U;
  InitializeReadyProtocol(&Ready);
  LocatedProtocol = &Ready;
  LocateProtocolStatus = EFI_SUCCESS;
  InstallStatus = EFI_DEVICE_ERROR;
  FailCloseEventIndex = 0;
  CloseFailureStatus = EFI_DEVICE_ERROR;

  CHECK(MusbInstallHost(&Ready) == EFI_DEVICE_ERROR);
  CHECK(mKeepImageResident);
  CHECK(mHostInstallBlocked);
  CHECK(mHostStarted);
  CHECK(Ready.HostContext != NULL && Ready.ReleaseHost == MusbReleaseHost);
  CHECK(EventCount == 1U);
  ExitEvent = (EFI_EVENT)&Events[0];
  CHECK(Events[0].Notify == MusbExitBootServicesNotify);
  CHECK(!Events[0].Closed);
  Context = (MUSB_HOST_DXE *)Events[0].NotifyContext;
  CHECK(Context != NULL);
  CHECK(Context->Signature == MUSB_DXE_SIGNATURE);
  CHECK(Context->ExitBootServicesEvent == ExitEvent);
  CHECK(!Context->ExitingBootServices);
  CHECK(Context->Host.State == MUSB_HOST_STATE_HALTED);

  CHECK(Context->Signature == MUSB_DXE_SIGNATURE);
  CHECK(!Context->ExitingBootServices);
  CHECK(MusbHostDxeEntryPoint(NULL, NULL) == EFI_SUCCESS);
  CHECK(EventCount == 1U);

  FailCloseEventIndex = -1;
  CloseFailureStatus = EFI_SUCCESS;
  CHECK(Ready.ReleaseHost(Ready.HostContext) == EFI_SUCCESS);
  CHECK(!Ready.ReleaseHost && !Ready.HostContext && !mHostStarted &&
        !mHostInstallBlocked && ((FAKE_EVENT *)ExitEvent)->Closed);
}

static void
TestInstallFailureProtocolOwnership(void)
{
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready;
  MUSB_HOST_DXE *Context;

  // A failed atomic install that published nothing can be fully retired.
  ResetEvents();
  memset(&Bus, 0, sizeof(Bus));
  Bus.Registers[TEST_DEVCTL] = 0x5DU;
  InitializeReadyProtocol(&Ready);
  LocatedProtocol = &Ready;
  LocateProtocolStatus = EFI_SUCCESS;
  InstallStatus = EFI_DEVICE_ERROR;
  CHECK(MusbInstallHost(&Ready) == EFI_DEVICE_ERROR);
  CHECK(!Ready.ReleaseHost && !Ready.HostContext && !mHostStarted &&
        !mHostInstallBlocked && InstalledUsbHc == NULL && InstalledDevicePath == NULL);

  // A partial publication is detected, then kept reachable if its removal
  // fails. A later explicit release removes it and closes its event.
  ResetEvents();
  memset(&Bus, 0, sizeof(Bus));
  Bus.Registers[TEST_DEVCTL] = 0x5DU;
  InitializeReadyProtocol(&Ready);
  LocatedProtocol = &Ready;
  LocateProtocolStatus = EFI_SUCCESS;
  InstallStatus = EFI_DEVICE_ERROR;
  InstallFailureMask = 1U;
  SingleUninstallStatus = EFI_ACCESS_DENIED;
  CHECK(MusbInstallHost(&Ready) == EFI_DEVICE_ERROR);
  CHECK(Ready.ReleaseHost == MusbReleaseHost && Ready.HostContext != NULL &&
        mHostStarted && mHostInstallBlocked && InstalledUsbHc != NULL &&
        InstalledDevicePath == NULL && SingleUninstallCalls == 1U);
  Context = Ready.HostContext;
  CHECK(Context->UsbHcInstalled && !Context->DevicePathInstalled &&
        !Context->ExitingBootServices);
  SingleUninstallStatus = EFI_SUCCESS;
  CHECK(Ready.ReleaseHost(Context) == EFI_SUCCESS);
  CHECK(!Ready.ReleaseHost && !Ready.HostContext && !mHostStarted &&
        !mHostInstallBlocked && InstalledUsbHc == NULL && InstalledDevicePath == NULL);

  // Unknown protocol visibility also retains a retryable release callback.
  ResetEvents();
  memset(&Bus, 0, sizeof(Bus));
  Bus.Registers[TEST_DEVCTL] = 0x5DU;
  InitializeReadyProtocol(&Ready);
  LocatedProtocol = &Ready;
  LocateProtocolStatus = EFI_SUCCESS;
  InstallStatus = EFI_DEVICE_ERROR;
  ProtocolLookupStatus = EFI_DEVICE_ERROR;
  CHECK(MusbInstallHost(&Ready) == EFI_DEVICE_ERROR);
  CHECK(Ready.ReleaseHost == MusbReleaseHost && Ready.HostContext != NULL &&
        mHostStarted && mHostInstallBlocked && mKeepImageResident);
  Context = Ready.HostContext;
  CHECK(!Context->ExitingBootServices);
  ProtocolLookupStatus = EFI_NOT_FOUND;
  CHECK(Ready.ReleaseHost(Context) == EFI_NOT_FOUND);
  CHECK(Ready.HostContext == Context && Ready.ReleaseHost == MusbReleaseHost &&
        mHostStarted && mHostInstallBlocked);
  ProtocolLookupStatus = EFI_SUCCESS;
  CHECK(Ready.ReleaseHost(Context) == EFI_SUCCESS);
  CHECK(!Ready.ReleaseHost && !Ready.HostContext && !mHostStarted &&
        InstalledUsbHc == NULL && InstalledDevicePath == NULL);
}

static void
TestNotifyCloseFailureKeepsImageResident(void)
{
  EFI_EVENT NotifyEvent;
  UINT32 CallsBeforeNotify;

  ResetEvents();
  LocateProtocolStatus = EFI_NOT_FOUND;
  RegisterProtocolNotifyStatus = EFI_DEVICE_ERROR;
  FailCloseEventIndex = 0;
  CloseFailureStatus = EFI_DEVICE_ERROR;

  CHECK(MusbHostDxeEntryPoint(NULL, NULL) == EFI_SUCCESS);
  CHECK(mKeepImageResident);
  CHECK(mReadyNotifyDisabled);
  CHECK(mReadyNotifyEvent == (EFI_EVENT)&Events[0]);
  CHECK(EventCount == 1U);
  NotifyEvent = mReadyNotifyEvent;
  CHECK(Events[0].Notify == MusbReadyNotify);
  CHECK(!Events[0].Closed);
  CallsBeforeNotify = LocateProtocolCalls;
  Events[0].Notify(NotifyEvent, Events[0].NotifyContext);
  CHECK(LocateProtocolCalls == CallsBeforeNotify);
  CHECK(mReadyNotifyEvent == NotifyEvent);

  FailCloseEventIndex = -1;
  CloseFailureStatus = EFI_SUCCESS;
  CHECK(gBS->CloseEvent(NotifyEvent) == EFI_SUCCESS);
  mReadyNotifyEvent = NULL;
}

static void TestUsb2HighSpeedSsCapableBosEnumeration(
  EFI_USB2_HC_PROTOCOL *InstalledProtocol);

static void
TestReadyHostPublishesOperationalAndStopsAtExit(void)
{
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready;
  MUSB_HOST_DXE *HostContext;
  EFI_EVENT ExitEvent;
  EFI_USB_HC_STATE State;
  EFI_STATUS Status;

  ResetEvents();
  memset(&Bus, 0, sizeof(Bus));
  Bus.Registers[TEST_DEVCTL] = 0x5DU;
  Bus.Registers[TEST_INTRUSB] = 0x10U;
  memset(&Ready, 0, sizeof(Ready));
  Ready.Signature = MTK_MUSB_HOST_PLATFORM_READY_SIGNATURE;
  Ready.Revision = MTK_MUSB_HOST_PLATFORM_READY_REVISION;
  Ready.Length = sizeof(Ready);
  Ready.Flags = MTK_MUSB_READY_REQUIRED;
  Ready.HostPowerPath = MTK_MUSB_HOST_POWER_PATH_TYPEC_SOURCE;
  Ready.ControllerBase = MTK_MUSB_CONTROLLER_BASE;
  Ready.ControllerLength = MTK_MUSB_CONTROLLER_LENGTH;
  Ready.PhyBase = MTK_MUSB_PHY_BASE;
  Ready.PhyLength = MTK_MUSB_PHY_LENGTH;
  Ready.InterruptVector = MTK_MUSB_GSIV;
  Ready.EndpointCount = MTK_MUSB_ENDPOINT_COUNT;
  Ready.PortCount = MTK_MUSB_ROOT_PORT_COUNT;
  Ready.FifoProfile = MTK_MUSB_FIFO_PROFILE_T70PRO_HOST_V1;
  Ready.InterruptMask = MTK_MUSB_CONNECT_DISCONNECT_MASK;

  LocatedProtocol = &Ready;
  LocateProtocolStatus = EFI_SUCCESS;
  Status = MusbInstallHost(&Ready);
  CHECK(Status == EFI_SUCCESS);
  CHECK(InstallObservedOperational);
  CHECK(InstalledUsbHc != NULL);
  CHECK(InstalledUsbHc->MajorRevision == 2U &&
        InstalledUsbHc->MinorRevision == 0U);
  TestUsb2HighSpeedSsCapableBosEnumeration(InstalledUsbHc);
  HostContext = (MUSB_HOST_DXE *)InstalledUsbHc;
  CHECK(HostContext->Host.State == MUSB_HOST_STATE_OPERATIONAL);
  CHECK(HostContext->ControllerHandle == (EFI_HANDLE)(UINTN)0x6789U);
  CHECK(HostContext->CoreLock.Tpl == TPL_NOTIFY);
  CHECK(Ready.HostSnapshotValid == 0U);
  CHECK(InstalledUsbHc->GetState(InstalledUsbHc, &State) == EFI_SUCCESS);
  CHECK(State == EfiUsbHcStateOperational);
  CHECK(InstalledUsbHc->Reset(InstalledUsbHc,
                             EFI_USB_HC_RESET_HOST_CONTROLLER) ==
        EFI_UNSUPPORTED);

  ExitEvent = HostContext->ExitBootServicesEvent;
  CHECK(ExitEvent != NULL);
  CHECK(((FAKE_EVENT *)ExitEvent)->Notify == MusbExitBootServicesNotify);
  ((FAKE_EVENT *)ExitEvent)->Notify(ExitEvent,
                                    ((FAKE_EVENT *)ExitEvent)->NotifyContext);
  CHECK(HostContext->ExitingBootServices);
  CHECK(HostContext->Host.State == MUSB_HOST_STATE_HALTED);
  CHECK(Ready.HostSnapshotValid == 1U);
  CHECK(Ready.HostPortStatus == HostContext->Host.PortStatus);
  CHECK(Ready.HostPortChangeStatus == HostContext->Host.PortChangeStatus);
  CHECK((Ready.HostPortStatus & MUSB_PORT_CONNECTION) != 0U);
  CHECK(InstalledUsbHc->GetState(InstalledUsbHc, &State) ==
        EFI_INVALID_PARAMETER);
  CHECK(((FAKE_EVENT *)ExitEvent)->CloseCount == 0U);
  HostContext->Signature = 0U;
  FreePool(HostContext);
}

static UINT16
DxeTarget(UINT32 Offset)
{
  return (UINT16)Bus.Registers[Offset] |
         (UINT16)((UINT16)Bus.Registers[Offset + 1U] << 8);
}

static void
TestSynchronousTranslatorRoutes(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Translator = {0U, 0U};
  EFI_USB_DEVICE_REQUEST Request = {0U, 5U, 9U, 0U, 0U};
  UINT8 Data[8] = {0};
  VOID *Buffers[EFI_USB_MAX_BULK_BUFFER_NUM] = {Data};
  UINTN Length;
  UINT8 Toggle, Mode;
  UINT32 Result;
  UINT16 ExpectedHub;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Bus.Registers[1U] = 0x10U;
  for (Mode = 0U; Mode < 2U; ++Mode) {
    Translator.TranslatorHubAddress = Mode ? 5U : 0U;
    Translator.TranslatorPortNumber = Mode ? 3U : 0U;
    ExpectedHub = Mode ? 0x0305U : 0U;
    CHECK(MusbControlTransfer(&Fixture.Host->UsbHc, 9U, EFI_USB_SPEED_FULL,
            8U, &Request, EfiUsbNoData, NULL, NULL, 10U, &Translator,
            &Result) == EFI_SUCCESS);
    CHECK(Result == EFI_USB_NOERROR);
    CHECK(DxeTarget(0x480U) == 0x109U);
    CHECK(DxeTarget(0x482U) == ExpectedHub);

    Length = sizeof(Data); Toggle = 0U;
    CHECK(MusbBulkTransfer(&Fixture.Host->UsbHc, 9U, 1U, EFI_USB_SPEED_FULL,
            8U, 1U, Buffers, &Length, &Toggle, 10U, &Translator,
            &Result) == EFI_SUCCESS);
    CHECK(Result == EFI_USB_NOERROR && Length == sizeof(Data) && Toggle == 1U);
    CHECK(DxeTarget(0x488U) == 0x109U);
    CHECK(DxeTarget(0x48AU) == ExpectedHub);

    Bus.RxSuccess = 1U;
    Length = sizeof(Data); Toggle = 0U;
    CHECK(MusbSyncInterruptTransfer(&Fixture.Host->UsbHc, 9U, 0x81U,
            EFI_USB_SPEED_LOW, 8U, Data, &Length, &Toggle, 10U, &Translator,
            &Result) == EFI_SUCCESS);
    CHECK(Result == EFI_USB_NOERROR && Length == sizeof(Data) && Toggle == 1U);
    CHECK(DxeTarget(0x48CU) == 9U);
    CHECK(DxeTarget(0x48EU) == ExpectedHub);
  }
  // Route switches back to a direct device through a non-NULL zero record.
  Translator = (EFI_USB2_HC_TRANSACTION_TRANSLATOR){0U, 0U};
  Length = sizeof(Data); Toggle = 0U;
  CHECK(MusbSyncInterruptTransfer(&Fixture.Host->UsbHc, 9U, 0x81U,
          EFI_USB_SPEED_LOW, 8U, Data, &Length, &Toggle, 10U, &Translator,
          &Result) == EFI_SUCCESS);
  CHECK(DxeTarget(0x48EU) == 0U);
  CleanupFixture(&Fixture);
}

static void
TestAsyncTranslatorIsCopiedAndCancelMatchesRoute(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Translator = {2U, 3U};
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Original = Translator;
  EFI_EVENT Event;
  UINT8 Toggle = 0U;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Bus.Registers[1U] = 0x10U;
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_FULL, 8U, TRUE, &Toggle, 1U, 8U, &Translator,
          AsyncCallback, &Fixture) == EFI_SUCCESS);
  Event = Fixture.Host->AsyncSlots[0].AsyncEvent;
  Translator.TranslatorHubAddress = 9U;
  Translator.TranslatorPortNumber = 10U;
  Bus.RxSuccess = 1U;
  Trigger(Event);
  CHECK(Fixture.SuccessCount == 1U && Toggle == 0U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  CHECK(DxeTarget(0x48EU) == 0x0302U);
  CHECK(DxeTarget(0x48CU) == 1U);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_FULL, 8U, FALSE, &Toggle, 0U, 0U, &Translator,
          NULL, NULL) == EFI_INVALID_PARAMETER);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive && !((FAKE_EVENT *)Event)->Closed);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_FULL, 8U, FALSE, &Toggle, 0U, 0U, NULL,
          NULL, NULL) == EFI_INVALID_PARAMETER);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_FULL, 8U, FALSE, &Toggle, 0U, 0U, &Original,
          NULL, NULL) == EFI_SUCCESS);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive && ((FAKE_EVENT *)Event)->Closed);
  CleanupFixture(&Fixture);

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Translator = (EFI_USB2_HC_TRANSACTION_TRANSLATOR){0U, 0U};
  Toggle = 0U;
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, TRUE, &Toggle, 1U, 8U, &Translator,
          AsyncCallback, &Fixture) == EFI_SUCCESS);
  Event = Fixture.Host->AsyncSlots[0].AsyncEvent;
  Translator = Original;
  Bus.Registers[0x48EU] = 0x55U;
  Bus.Registers[0x48FU] = 0x66U;
  Bus.RxSuccess = 1U;
  Trigger(Event);
  CHECK(Fixture.SuccessCount == 1U);
  CHECK(DxeTarget(0x48EU) == 0U);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, FALSE, &Toggle, 0U, 0U, NULL,
          NULL, NULL) == EFI_SUCCESS);
  CleanupFixture(&Fixture);
}

static void
TestMalformedTranslatorDoesNotReachController(void)
{
  ASYNC_FIXTURE Fixture;
  const EFI_USB2_HC_TRANSACTION_TRANSLATOR Invalid[] = {
    {0U, 1U}, {1U, 0U}, {128U, 1U}, {1U, 128U}
  };
  EFI_USB_DEVICE_REQUEST Request = {0U, 5U, 1U, 0U, 0U};
  FAKE_MUSB Before;
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Translator;
  UINT8 Data[8] = {0}, Toggle = 0U;
  VOID *Buffers[EFI_USB_MAX_BULK_BUFFER_NUM] = {Data};
  UINTN I, Length;
  UINT32 Result;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Before = Bus;
  for (I = 0U; I < ARRAY_SIZE(Invalid); ++I) {
    Translator = Invalid[I];
    CHECK(MusbControlTransfer(&Fixture.Host->UsbHc, 1U, EFI_USB_SPEED_FULL,
            8U, &Request, EfiUsbNoData, NULL, NULL, 10U, &Translator,
            &Result) == EFI_INVALID_PARAMETER);
    CHECK(Result == EFI_USB_ERR_SYSTEM);
    Length = sizeof(Data);
    CHECK(MusbBulkTransfer(&Fixture.Host->UsbHc, 1U, 1U, EFI_USB_SPEED_FULL,
            8U, 1U, Buffers, &Length, &Toggle, 10U, &Translator,
            &Result) == EFI_INVALID_PARAMETER);
    CHECK(MusbSyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
            EFI_USB_SPEED_LOW, 8U, Data, &Length, &Toggle, 10U, &Translator,
            &Result) == EFI_INVALID_PARAMETER);
    CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
            EFI_USB_SPEED_FULL, 8U, TRUE, &Toggle, 1U, 8U, &Translator,
            AsyncCallback, &Fixture) == EFI_INVALID_PARAMETER);
    CHECK(memcmp(&Before, &Bus, sizeof(Bus)) == 0 && EventCount == 0U);
  }
  Translator = (EFI_USB2_HC_TRANSACTION_TRANSLATOR){1U, 1U};
  CHECK(MusbControlTransfer(&Fixture.Host->UsbHc, 1U, EFI_USB_SPEED_HIGH,
          64U, &Request, EfiUsbNoData, NULL, NULL, 10U, &Translator,
          &Result) == EFI_INVALID_PARAMETER);
  CHECK(memcmp(&Before, &Bus, sizeof(Bus)) == 0);
  CleanupFixture(&Fixture);
}

static void
TestAsyncDoesNotRetainRegistrationPointer(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_EVENT Event;
  UINT8 *Input;
  UINT8 Output = 0xFFU;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Input = malloc(1U);
  CHECK(Input != NULL);
  *Input = 0U;
  Event = RegisterAsync(&Fixture, Input);
  free(Input); // UEFI does not require the input pointer to outlive registration.
  Bus.RxSuccess = 1U;
  Trigger(Event);
  Trigger(Event);
  Trigger(Event);
  CHECK(Fixture.SuccessCount == 3U && Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, FALSE, NULL, 0U, 0U, NULL,
          NULL, NULL) == EFI_INVALID_PARAMETER);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, FALSE, &Output, 0U, 0U, NULL,
          NULL, NULL) == EFI_SUCCESS);
  CHECK(Output == 1U && !Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(((FAKE_EVENT *)Event)->Closed);
  CleanupFixture(&Fixture);
}

static void
TestHubAndKeyboardPollIndependently(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Route = {2U, 3U};
  EFI_EVENT HubEvent, KeyEvent, NewEvent;
  UINT8 HubToggle = 0U, KeyToggle = 0U, NewToggle = 0U;
  UINT32 Count;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  Bus.Registers[1U] = 0x10U;
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 2U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, TRUE, &HubToggle, 8U, 8U, NULL,
          AsyncCallback, &Fixture) == EFI_SUCCESS);
  HubEvent = Fixture.Host->AsyncSlots[0].AsyncEvent;
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 9U, 0x81U,
          EFI_USB_SPEED_FULL, 8U, TRUE, &KeyToggle, 1U, 8U, &Route,
          AsyncCallback, &Fixture) == EFI_SUCCESS);
  KeyEvent = Fixture.Host->AsyncSlots[1].AsyncEvent;
  CHECK(HubEvent != NULL && KeyEvent != NULL && HubEvent != KeyEvent);
  Bus.RxSuccess = 1U;
  Trigger(HubEvent);
  CHECK(DxeTarget(0x48CU) == 2U && DxeTarget(0x48EU) == 0U);
  Trigger(KeyEvent);
  CHECK(DxeTarget(0x48CU) == 9U && DxeTarget(0x48EU) == 0x0302U);
  CHECK(Fixture.SuccessCount == 2U);
  CHECK(HubToggle == 0U && KeyToggle == 0U);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  CHECK(Fixture.Host->AsyncSlots[1].AsyncToggle == 1U);
  Bus.RxSuccess = 0U;
  for (UINTN Index = 0U; Index < 8U; ++Index) {
    Trigger(HubEvent);
    Trigger(KeyEvent);
    CHECK(Fixture.CallbackCount == 2U);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncActive &&
          Fixture.Host->AsyncSlots[1].AsyncActive);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U &&
          Fixture.Host->AsyncSlots[1].AsyncToggle == 1U);
    CHECK(!((FAKE_EVENT *)HubEvent)->Closed && !((FAKE_EVENT *)KeyEvent)->Closed);
  }
  Bus.RxSuccess = 1U;
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 2U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, FALSE, &HubToggle, 0U, 0U, NULL,
          NULL, NULL) == EFI_SUCCESS);
  CHECK(HubToggle == 1U && Fixture.Host->AsyncSlots[1].AsyncActive);
  Count = Fixture.CallbackCount;
  ((FAKE_EVENT *)HubEvent)->Notify(HubEvent, ((FAKE_EVENT *)HubEvent)->NotifyContext);
  CHECK(Fixture.CallbackCount == Count);
  Trigger(KeyEvent);
  CHECK(Fixture.SuccessCount == 3U && Fixture.Host->AsyncSlots[1].AsyncToggle == 0U);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 3U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, TRUE, &NewToggle, 4U, 8U, NULL,
          AsyncCallback, &Fixture) == EFI_SUCCESS);
  NewEvent = Fixture.Host->AsyncSlots[0].AsyncEvent;
  CHECK(NewEvent != HubEvent);
  Count = Fixture.CallbackCount;
  ((FAKE_EVENT *)HubEvent)->Notify(HubEvent, ((FAKE_EVENT *)HubEvent)->NotifyContext);
  CHECK(Fixture.CallbackCount == Count && Fixture.Host->AsyncSlots[0].AsyncActive);

  MusbExitBootServicesNotify(NULL, Fixture.Host);
  CHECK(!((FAKE_EVENT *)KeyEvent)->Closed && !((FAKE_EVENT *)NewEvent)->Closed);
  CHECK(((FAKE_EVENT *)KeyEvent)->TimerType == TimerCancel);
  CHECK(((FAKE_EVENT *)NewEvent)->TimerType == TimerCancel);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive &&
        !Fixture.Host->AsyncSlots[1].AsyncActive);
  ((FAKE_EVENT *)KeyEvent)->Notify(KeyEvent, ((FAKE_EVENT *)KeyEvent)->NotifyContext);
  CHECK(Fixture.CallbackCount == Count);
  CleanupFixture(&Fixture);
}

static void
TestAsyncCapacityAndControllerStop(void)
{
  ASYNC_FIXTURE Fixture;
  UINT8 Toggle[MUSB_ASYNC_SLOT_COUNT + 1U] = {0};
  EFI_EVENT Active[MUSB_ASYNC_SLOT_COUNT];
  EFI_EVENT Old;
  UINTN Index;
  UINT32 Count;

  PrepareFixture(&Fixture);
  EnableFixturePort(&Fixture);
  for (Index = 0U; Index < MUSB_ASYNC_SLOT_COUNT; ++Index) {
    CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, (UINT8)(Index + 1U),
            0x81U, EFI_USB_SPEED_HIGH, 8U, TRUE, &Toggle[Index], 1U, 8U,
            NULL, AsyncCallback, &Fixture) == EFI_SUCCESS);
    Active[Index] = Fixture.Host->AsyncSlots[Index].AsyncEvent;
    CHECK(Active[Index] != NULL);
  }
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 1U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, TRUE, &Toggle[0], 1U, 8U,
          NULL, AsyncCallback, &Fixture) == EFI_INVALID_PARAMETER);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc,
          (UINT8)(MUSB_ASYNC_SLOT_COUNT + 1U), 0x81U, EFI_USB_SPEED_HIGH,
          8U, TRUE, &Toggle[MUSB_ASYNC_SLOT_COUNT], 1U, 8U,
          NULL, AsyncCallback, &Fixture) == EFI_OUT_OF_RESOURCES);
  CHECK(EventCount == MUSB_ASYNC_SLOT_COUNT);
  Old = Active[5];
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc, 6U, 0x81U,
          EFI_USB_SPEED_HIGH, 8U, FALSE, &Toggle[5], 0U, 0U,
          NULL, NULL, NULL) == EFI_SUCCESS);
  CHECK(MusbAsyncInterruptTransfer(&Fixture.Host->UsbHc,
          (UINT8)(MUSB_ASYNC_SLOT_COUNT + 1U), 0x81U, EFI_USB_SPEED_HIGH,
          8U, TRUE, &Toggle[MUSB_ASYNC_SLOT_COUNT], 1U, 8U,
          NULL, AsyncCallback, &Fixture) == EFI_SUCCESS);
  Active[5] = Fixture.Host->AsyncSlots[5].AsyncEvent;
  CHECK(Active[5] != Old);
  ((FAKE_EVENT *)Old)->Notify(Old, ((FAKE_EVENT *)Old)->NotifyContext);
  CHECK(Fixture.CallbackCount == 0U && Fixture.Host->AsyncSlots[5].AsyncActive);
  Bus.RxSuccess = 1U;
  for (Index = 0U; Index < MUSB_ASYNC_SLOT_COUNT; ++Index) {
    Trigger(Active[Index]);
  }
  CHECK(Fixture.SuccessCount == MUSB_ASYNC_SLOT_COUNT);
  CHECK(MusbSetState(&Fixture.Host->UsbHc, EfiUsbHcStateHalt) == EFI_SUCCESS);
  Count = Fixture.CallbackCount;
  for (Index = 0U; Index < MUSB_ASYNC_SLOT_COUNT; ++Index) {
    CHECK(!Fixture.Host->AsyncSlots[Index].AsyncActive);
    CHECK(((FAKE_EVENT *)Active[Index])->Closed);
    ((FAKE_EVENT *)Active[Index])->Notify(
      Active[Index], ((FAKE_EVENT *)Active[Index])->NotifyContext);
  }
  CHECK(Fixture.CallbackCount == Count);
  CleanupFixture(&Fixture);
}

EFI_STATUS UsbRootHubResetPort(USB_INTERFACE *RootIf, UINT8 Port);

static EFI_STATUS EFIAPI
RootPortStall(UINTN Microseconds)
{
  Bus.TimeUs += Microseconds;
  return EFI_SUCCESS;
}

static void
TestRealUsbBusRootReset(void)
{
  const UINT8 DevCtl[] = {0x5dU, 0x5dU, 0x3dU};
  const UINT8 Power[] = {0x10U, 0U, 0U};
  const UINT16 Speed[] = {MUSB_PORT_HIGH_SPEED, 0U, MUSB_PORT_LOW_SPEED};
  for (UINTN Index = 0U; Index < ARRAY_SIZE(DevCtl); ++Index) {
    ASYNC_FIXTURE Fixture;
    USB_BUS UsbBus = {0};
    USB_DEVICE RootHub = {0};
    USB_INTERFACE RootIf = {0};
    EFI_USB_PORT_STATUS Port;
    EFI_STATUS Status;
    PrepareFixture(&Fixture);
    Bus.Registers[TEST_INTRUSB] = 0U;
    Bus.Registers[TEST_DEVCTL] = DevCtl[Index];
    Bus.Registers[1U] = Power[Index];
    Services.Stall = RootPortStall;
    Fixture.Host->UsbHc.GetRootHubPortStatus = MusbGetRootHubPortStatus;
    Fixture.Host->UsbHc.SetRootHubPortFeature = MusbSetRootHubPortFeature;
    Fixture.Host->UsbHc.ClearRootHubPortFeature = MusbClearRootHubPortFeature;
    UsbBus.Usb2Hc = &Fixture.Host->UsbHc;
    RootHub.Bus = &UsbBus;
    RootIf.Device = &RootHub;
    RootIf.MaxSpeed = EFI_USB_SPEED_HIGH;
    Status = UsbRootHubResetPort(&RootIf, 0U);
    fprintf(stderr, "real UsbBus root reset speed=%u status=%llx\n",
            (unsigned)Index, (unsigned long long)Status);
    CHECK(Status == EFI_SUCCESS);
    CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
    CHECK(Port.PortStatus ==
          (MUSB_PORT_POWER | MUSB_PORT_CONNECTION | MUSB_PORT_ENABLE | Speed[Index]));
    CHECK((Port.PortChangeStatus & MUSB_PORT_CHANGE_RESET) != 0U);
    CHECK(Bus.TimeUs >= 50000U);
    CleanupFixture(&Fixture);
  }
}

static void
TestRootPortHotplugWithoutInterrupts(void)
{
  ASYNC_FIXTURE Fixture;
  EFI_USB_PORT_STATUS Port;
  PrepareFixture(&Fixture);
  Bus.Registers[TEST_INTRUSB] = 0U;
  Bus.Registers[TEST_DEVCTL] = 0x1dU;
  CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
  CHECK(Port.PortStatus == MUSB_PORT_POWER && Port.PortChangeStatus == 0U);
  for (UINTN Cycle = 0U; Cycle < 3U; ++Cycle) {
    Bus.Registers[TEST_DEVCTL] = 0x5dU;
    CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
    CHECK(Port.PortStatus == (MUSB_PORT_POWER | MUSB_PORT_CONNECTION));
    CHECK((Port.PortChangeStatus & MUSB_PORT_CHANGE_CONNECTION) != 0U);
    CHECK(MusbSetRootHubPortFeature(&Fixture.Host->UsbHc, 0U, EfiUsbPortReset) == EFI_SUCCESS);
    CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
    CHECK((Port.PortStatus & MUSB_PORT_ENABLE) != 0U);
    Bus.Registers[TEST_DEVCTL] = 0x1dU;
    CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
    CHECK(Port.PortStatus == MUSB_PORT_POWER);
    CHECK(MusbClearRootHubPortFeature(&Fixture.Host->UsbHc, 0U, EfiUsbPortConnectChange) == EFI_SUCCESS);
    CHECK(MusbClearRootHubPortFeature(&Fixture.Host->UsbHc, 0U, EfiUsbPortEnableChange) == EFI_SUCCESS);
    CHECK(MusbClearRootHubPortFeature(&Fixture.Host->UsbHc, 0U, EfiUsbPortResetChange) == EFI_SUCCESS);
    CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
    CHECK(Port.PortStatus == MUSB_PORT_POWER && Port.PortChangeStatus == 0U);
  }
  CleanupFixture(&Fixture);
}


// The root hub reports USB2 High Speed for a device with an SS-capable BOS
// descriptor. The USB bus runs its actual descriptor parsing and retry policy.
typedef struct {
  EFI_USB2_HC_PROTOCOL Hc;
  USB_BUS Bus;
  USB_DEVICE Root;
  USB_INTERFACE RootIf;
  USB_HUB_API HubApi;
  UINT32 PortResetCount;
  UINT32 PortStatusCount;
  UINT32 DeviceDescriptorRequests;
  UINT32 ConfigurationDescriptorRequests;
  UINT32 BosDescriptorRequests;
  UINT32 SetAddressRequests;
  UINT32 SetConfigurationRequests;
  UINT32 InstallCalls;
  UINT32 OpenCalls;
} USB_ENUM_FIXTURE;

static USB_ENUM_FIXTURE *mUsbEnumFixture;
EFI_GUID gEfiUsbIoProtocolGuid =
  { 0x2b2f68d6, 0x0cd2, 0x44cf, { 0x8e, 0x8b, 0xbb, 0xa2, 0x0b, 0x1b, 0x5b, 0x75 } };

static const UINT8 mEnumDeviceDescriptor[] = {
  18U, 0x01U, 0x10U, 0x02U, 0U, 0U, 0U, 64U,
  0x34U, 0x12U, 0x78U, 0x56U, 0x00U, 0x01U, 0U, 0U, 0U, 1U
};
static const UINT8 mEnumConfigurationDescriptor[] = {
  9U, 0x02U, 18U, 0U, 1U, 1U, 0U, 0x80U, 50U,
  9U, 0x04U, 0U, 0U, 0U, 0xffU, 0U, 0U, 0U
};
static const UINT8 mEnumBosDescriptor[] = {
  5U, 0x0fU, 15U, 0U, 1U,
  10U, 0x10U, 0x03U, 0U, 0x0fU, 0U, 1U, 10U, 0U, 0U
};

static EFI_STATUS EFIAPI
EnumControlTransfer(
  EFI_USB2_HC_PROTOCOL *This,
  UINT8 DeviceAddress,
  UINT8 DeviceSpeed,
  UINTN MaximumPacketLength,
  EFI_USB_DEVICE_REQUEST *Request,
  EFI_USB_DATA_DIRECTION Direction,
  VOID *Data,
  UINTN *DataLength,
  UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  UINT32 *TransferResult
  )
{
  CONST UINT8 *Source = NULL;
  UINTN SourceLength = 0U;
  UINT8 DescriptorType;
  (VOID)Translator;
  CHECK(mUsbEnumFixture != NULL && This == &mUsbEnumFixture->Hc);
  CHECK(DeviceAddress <= 1U && DeviceSpeed == EFI_USB_SPEED_HIGH);
  CHECK((MaximumPacketLength == 8U) || (MaximumPacketLength == 64U));
  CHECK(Request != NULL && DataLength != NULL && TransferResult != NULL);
  CHECK(Timeout == USB_GENERAL_DEVICE_REQUEST_TIMEOUT);
  *TransferResult = 0U;

  if (Request->Request == USB_REQ_GET_DESCRIPTOR) {
    CHECK(Direction == EfiUsbDataIn && Data != NULL);
    DescriptorType = (UINT8)(Request->Value >> 8);
    if (DescriptorType == USB_DESC_TYPE_DEVICE) {
      ++mUsbEnumFixture->DeviceDescriptorRequests;
      Source = mEnumDeviceDescriptor;
      SourceLength = sizeof(mEnumDeviceDescriptor);
    } else if (DescriptorType == USB_DESC_TYPE_CONFIG) {
      ++mUsbEnumFixture->ConfigurationDescriptorRequests;
      Source = mEnumConfigurationDescriptor;
      SourceLength = sizeof(mEnumConfigurationDescriptor);
    } else if (DescriptorType == USB_DESC_TYPE_BOS) {
      ++mUsbEnumFixture->BosDescriptorRequests;
      Source = mEnumBosDescriptor;
      SourceLength = sizeof(mEnumBosDescriptor);
    } else if (DescriptorType == USB_DESC_TYPE_STRING) {
      return EFI_NOT_FOUND;
    } else {
      return EFI_UNSUPPORTED;
    }

    if (*DataLength > SourceLength) *DataLength = SourceLength;
    memcpy(Data, Source, *DataLength);
    return EFI_SUCCESS;
  }

  CHECK(Direction == EfiUsbNoData && *DataLength == 0U);
  if (Request->Request == USB_REQ_SET_ADDRESS) {
    ++mUsbEnumFixture->SetAddressRequests;
    CHECK(Request->Value == 1U);
    return EFI_SUCCESS;
  }
  if (Request->Request == USB_REQ_SET_CONFIG) {
    ++mUsbEnumFixture->SetConfigurationRequests;
    CHECK(Request->Value == 1U);
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
EnumGetPortStatus(
  USB_INTERFACE *HubIf,
  UINT8 Port,
  EFI_USB_PORT_STATUS *PortStatus
  )
{
  CHECK(mUsbEnumFixture != NULL && HubIf == &mUsbEnumFixture->RootIf);
  CHECK(Port == 0U && PortStatus != NULL);
  ++mUsbEnumFixture->PortStatusCount;
  memset(PortStatus, 0, sizeof(*PortStatus));
  PortStatus->PortStatus = USB_PORT_STAT_CONNECTION |
                           USB_PORT_STAT_ENABLE |
                           USB_PORT_STAT_HIGH_SPEED;
  if (mUsbEnumFixture->PortStatusCount == 1U) {
    PortStatus->PortChangeStatus = USB_PORT_STAT_C_CONNECTION;
  }
  return EFI_SUCCESS;
}

static VOID
EnumClearPortChange(USB_INTERFACE *HubIf, UINT8 Port)
{
  CHECK(mUsbEnumFixture != NULL && HubIf == &mUsbEnumFixture->RootIf && Port == 0U);
}

static EFI_STATUS EFIAPI
EnumResetPort(
  USB_INTERFACE *HubIf,
  UINT8 Port
  )
{
  CHECK(mUsbEnumFixture != NULL && HubIf == &mUsbEnumFixture->RootIf);
  CHECK(Port == 0U);
  ++mUsbEnumFixture->PortResetCount;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
EnumInstallUsbIo(
  EFI_HANDLE *Handle,
  ...
  )
{
  va_list Arguments;
  EFI_GUID *Protocol;
  VOID *Interface;
  BOOLEAN SawDevicePath = FALSE;
  BOOLEAN SawUsbIo = FALSE;
  CHECK(mUsbEnumFixture != NULL && Handle != NULL && *Handle == NULL);
  va_start(Arguments, Handle);
  for (;;) {
    Protocol = va_arg(Arguments, EFI_GUID *);
    if (Protocol == NULL) break;
    Interface = va_arg(Arguments, VOID *);
    if (memcmp(Protocol, &gEfiDevicePathProtocolGuid, sizeof(*Protocol)) == 0) {
      CHECK(Interface != NULL);
      SawDevicePath = TRUE;
    } else if (memcmp(Protocol, &gEfiUsbIoProtocolGuid, sizeof(*Protocol)) == 0) {
      CHECK(Interface != NULL);
      SawUsbIo = TRUE;
    } else {
      CHECK(FALSE);
    }
  }
  va_end(Arguments);
  CHECK(SawDevicePath && SawUsbIo);
  ++mUsbEnumFixture->InstallCalls;
  *Handle = (EFI_HANDLE)(UINTN)0x9001U;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
EnumOpenHostProtocol(
  EFI_HANDLE Handle,
  EFI_GUID *Protocol,
  VOID **Interface,
  EFI_HANDLE AgentHandle,
  EFI_HANDLE ControllerHandle,
  UINT32 Attributes
  )
{
  (VOID)AgentHandle;
  CHECK(mUsbEnumFixture != NULL && Handle == (EFI_HANDLE)(UINTN)0x9000U);
  CHECK(Protocol != NULL && Interface != NULL);
  CHECK(memcmp(Protocol, &gEfiUsb2HcProtocolGuid, sizeof(*Protocol)) == 0);
  CHECK(ControllerHandle == (EFI_HANDLE)(UINTN)0x9001U);
  CHECK(Attributes == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER);
  ++mUsbEnumFixture->OpenCalls;
  *Interface = &mUsbEnumFixture->Hc;
  return EFI_SUCCESS;
}

// The bus test links the UsbBus production objects without DevicePathLib.
// These compact helpers model only a root end node plus one USB child node.
UINTN EFIAPI DevicePathNodeLength(CONST VOID *Node)
{
  CONST EFI_DEVICE_PATH_PROTOCOL *Path = (CONST EFI_DEVICE_PATH_PROTOCOL *)Node;
  return (UINTN)Path->Length[0] | ((UINTN)Path->Length[1] << 8);
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode(CONST VOID *Node)
{
  return (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Node + DevicePathNodeLength(Node));
}

BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *Node)
{
  CONST EFI_DEVICE_PATH_PROTOCOL *Path = (CONST EFI_DEVICE_PATH_PROTOCOL *)Node;
  return Path->Type == END_DEVICE_PATH_TYPE &&
         Path->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE;
}

BOOLEAN EFIAPI IsDevicePathEndType(CONST VOID *Node)
{
  return ((CONST EFI_DEVICE_PATH_PROTOCOL *)Node)->Type == END_DEVICE_PATH_TYPE;
}

UINTN EFIAPI GetDevicePathSize(CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath)
{
  CONST EFI_DEVICE_PATH_PROTOCOL *Node = DevicePath;
  UINTN Total = 0U;
  for (UINTN Count = 0U; Node != NULL && Count < 32U; ++Count) {
    UINTN Length = DevicePathNodeLength(Node);
    if (Length < sizeof(*Node)) return 0U;
    Total += Length;
    if (IsDevicePathEnd(Node)) return Total;
    Node = NextDevicePathNode(Node);
  }
  return 0U;
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI DuplicateDevicePath(
  CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath
  )
{
  UINTN Length = GetDevicePathSize(DevicePath);
  EFI_DEVICE_PATH_PROTOCOL *Copy;
  if (Length == 0U) return NULL;
  Copy = AllocateZeroPool(Length);
  if (Copy != NULL) memcpy(Copy, DevicePath, Length);
  return Copy;
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode(
  CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  CONST EFI_DEVICE_PATH_PROTOCOL *DevicePathNode
  )
{
  UINTN PathLength = GetDevicePathSize(DevicePath);
  UINTN NodeLength = DevicePathNodeLength(DevicePathNode);
  UINTN PrefixLength;
  EFI_DEVICE_PATH_PROTOCOL *Result;
  EFI_DEVICE_PATH_PROTOCOL *End;
  if (NodeLength < sizeof(*DevicePathNode)) return NULL;
  PrefixLength = (PathLength >= sizeof(EFI_DEVICE_PATH_PROTOCOL)) ?
                 PathLength - sizeof(EFI_DEVICE_PATH_PROTOCOL) : 0U;
  Result = AllocateZeroPool(PrefixLength + NodeLength + sizeof(*End));
  if (Result == NULL) return NULL;
  if (PrefixLength != 0U) memcpy(Result, DevicePath, PrefixLength);
  memcpy((UINT8 *)Result + PrefixLength, DevicePathNode, NodeLength);
  End = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Result + PrefixLength + NodeLength);
  End->Type = END_DEVICE_PATH_TYPE;
  End->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength(End, sizeof(*End));
  return Result;
}

EFI_GUID gEfiCallerIdGuid = {0};

EFI_STATUS EFIAPI EnumFreePool(VOID *Buffer)
{
  free(Buffer);
  return EFI_SUCCESS;
}
#ifndef main
BOOLEAN EFIAPI ReportProgressCodeEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI ReportErrorCodeEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI ReportDebugCodeEnabled(VOID) { return FALSE; }
EFI_STATUS EFIAPI ReportStatusCode(EFI_STATUS_CODE_TYPE Type,
                                   EFI_STATUS_CODE_VALUE Value)
{
  (VOID)Type;
  (VOID)Value;
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI ReportStatusCodeWithDevicePath(
  EFI_STATUS_CODE_TYPE Type,
  EFI_STATUS_CODE_VALUE Value,
  CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath
  )
{
  (VOID)Type;
  (VOID)Value;
  (VOID)DevicePath;
  return EFI_SUCCESS;
}
#endif
VOID EFIAPI SetDevicePathEndNode(VOID *Node)
{
  EFI_DEVICE_PATH_PROTOCOL *End = (EFI_DEVICE_PATH_PROTOCOL *)Node;
  End->Type = END_DEVICE_PATH_TYPE;
  End->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength(End, sizeof(*End));
}
INTN EFIAPI CompareMem(CONST VOID *Left, CONST VOID *Right, UINTN Length)
{
  return (INTN)memcmp(Left, Right, Length);
}
#ifndef main
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *Head)
{
  Head->ForwardLink = Head;
  Head->BackLink = Head;
  return Head;
}
LIST_ENTRY *EFIAPI InsertTailList(LIST_ENTRY *Head, LIST_ENTRY *Entry)
{
  Entry->BackLink = Head->BackLink;
  Entry->ForwardLink = Head;
  Head->BackLink->ForwardLink = Entry;
  Head->BackLink = Entry;
  return Entry;
}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *Entry)
{
  LIST_ENTRY *Previous = ((LIST_ENTRY *)Entry)->BackLink;
  LIST_ENTRY *Next = ((LIST_ENTRY *)Entry)->ForwardLink;
  Previous->ForwardLink = Next;
  Next->BackLink = Previous;
  return Next;
}
#endif

static void
RunUsb2SsBosEnumeration(EFI_USB2_HC_PROTOCOL *InstalledProtocol,
                        UINT16 HcMajorRevision, UINT32 ExpectedResets,
                        UINT32 ExpectedBosRequests)
{
  USB_ENUM_FIXTURE Fixture = {0};
  USB_DEVICE *Child;
  EFI_DEVICE_PATH_PROTOCOL RootPath = {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0U}
  };

  CHECK(InstalledProtocol != NULL);
  CHECK(InstalledProtocol->MajorRevision == 2U &&
        InstalledProtocol->MinorRevision == 0U);
  mUsbEnumFixture = &Fixture;
  Fixture.Hc = *InstalledProtocol;
  if (HcMajorRevision != InstalledProtocol->MajorRevision) {
    Fixture.Hc.MajorRevision = HcMajorRevision;
  }
  Fixture.Hc.ControlTransfer = EnumControlTransfer;
  Fixture.Bus.Usb2Hc = &Fixture.Hc;
  Fixture.Bus.HostHandle = (EFI_HANDLE)(UINTN)0x9000U;
  Fixture.Bus.MaxDevices = 128U;
  Fixture.Bus.DevicePath = &RootPath;
  Fixture.Bus.Devices[0] = &Fixture.Root;
  Fixture.Bus.WantedUsbIoDPList.ForwardLink = &Fixture.Bus.WantedUsbIoDPList;
  Fixture.Bus.WantedUsbIoDPList.BackLink = &Fixture.Bus.WantedUsbIoDPList;
  Fixture.Root.Bus = &Fixture.Bus;
  Fixture.Root.Address = 0U;
  Fixture.Root.Speed = EFI_USB_SPEED_HIGH;
  Fixture.RootIf.Device = &Fixture.Root;
  Fixture.RootIf.DevicePath = &RootPath;
  Fixture.RootIf.HubApi = &Fixture.HubApi;
  Fixture.RootIf.MaxSpeed = EFI_USB_SPEED_HIGH;
  Fixture.RootIf.NumOfPort = 1U;
  Fixture.HubApi.GetPortStatus = EnumGetPortStatus;
  Fixture.HubApi.ClearPortChange = EnumClearPortChange;
  Fixture.HubApi.ResetPort = EnumResetPort;
  Services.Stall = RootPortStall;
  Services.FreePool = EnumFreePool;
  Services.InstallMultipleProtocolInterfaces = EnumInstallUsbIo;
  Services.OpenProtocol = EnumOpenHostProtocol;

  CHECK(Fixture.Hc.MajorRevision == HcMajorRevision &&
        Fixture.Hc.MinorRevision == 0U);
  UsbRootHubEnumeration(NULL, &Fixture.RootIf);
  CHECK(Fixture.PortResetCount == ExpectedResets);
  CHECK(Fixture.BosDescriptorRequests == ExpectedBosRequests);
  CHECK(Fixture.SetAddressRequests == ExpectedResets);
  CHECK(Fixture.SetConfigurationRequests == 1U);
  CHECK(Fixture.InstallCalls == 1U && Fixture.OpenCalls == 1U);
  Child = Fixture.Bus.Devices[1U];
  CHECK(Child != NULL && Child->Speed == EFI_USB_SPEED_HIGH &&
        Child->MaxPacket0 == 64U && Child->IsSSDev);
  CHECK(Child->DevDesc != NULL && Child->DevDesc->BosDesc != NULL &&
        Child->DevDesc->Desc.BcdUSB == 0x0210U);
  if (Child->Interfaces[0] != NULL) {
    free(Child->Interfaces[0]->DevicePath);
    free(Child->Interfaces[0]);
  }
  Fixture.Bus.Devices[1U] = NULL;
  if (Child->DevDesc != NULL) UsbFreeDevDesc(Child->DevDesc);
  free(Child);
  mUsbEnumFixture = NULL;
  Services.InstallMultipleProtocolInterfaces =
    FakeInstallMultipleProtocolInterfaces;
}

static void
TestUsb2HighSpeedSsCapableBosEnumeration(
  EFI_USB2_HC_PROTOCOL *InstalledProtocol
  )
{
  // Normal enumeration consumes the revision installed by production HCD code.
  RunUsb2SsBosEnumeration(InstalledProtocol,
                         InstalledProtocol->MajorRevision, 1U, 2U);
  // Inject the former 0x0200 value (decimal 512) at the USB bus boundary.
  RunUsb2SsBosEnumeration(InstalledProtocol, 0x0200U, 4U, 8U);
}

int
main(void)
{
  TestRealUsbBusRootReset();
  TestRootPortHotplugWithoutInterrupts();
  TestEdk2HubIdleAndErrorRecovery(TRUE);
  TestAsyncIdleKeepsPolling();
  TestAsyncCancelReregister(FALSE);
  TestAsyncCancelReregister(TRUE);
  TestAsyncOverflowToggleSurvivesCancelReregister();
  TestAsyncSuccessToggleAndExplicitCancel();
  TestAsyncCallbackFailureReleasesSlot();
  TestAsyncCancelCloseFailureDoesNotRunNewSlot();
  TestAsyncStallAndDisconnect();
  TestAsyncValidationAndBusy();
  TestSynchronousTranslatorRoutes();
  TestAsyncTranslatorIsCopiedAndCancelMatchesRoute();
  TestMalformedTranslatorDoesNotReachController();
  TestAsyncDoesNotRetainRegistrationPointer();
  TestHubAndKeyboardPollIndependently();
  TestAsyncCapacityAndControllerStop();
  TestExitBootServicesQuiescesAsync();
  TestInstallFailureCloseFailureKeepsImageResident();
  TestInstallFailureProtocolOwnership();
  TestNotifyCloseFailureKeepsImageResident();
  TestReadyProtocolPowerPaths();
  TestHostReleaseLifecycle();
  TestReadyNotificationSurvivesInitialOrdering();
  TestReadyHostPublishesOperationalAndStopsAtExit();
  fprintf(stderr, "MUSB DXE async checks: %u\n", Checks);
  return 0;
}

