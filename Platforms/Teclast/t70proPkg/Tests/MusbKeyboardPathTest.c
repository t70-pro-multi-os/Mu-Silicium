// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the real UsbBusDxe -> MUSB -> UsbKbDxe -> SimpleTextIn path.
#define main MusbDxeFixtureMain
#include "MusbHostDxeTest.c"
#undef main
#include "KeyBoard.h"

extern USB_KEYBOARD_LAYOUT_PACK_BIN mUsbKeyboardLayoutBin;
extern UINT8 EfiKeyToUsbKeyCodeConvertionTable[];
EFI_RUNTIME_SERVICES *gRT; // An unexpected reset path fails instead of rebooting.

VOID *EFIAPI AllocatePool(UINTN Size) { return malloc(Size); }
BOOLEAN EFIAPI ReportProgressCodeEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI ReportErrorCodeEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI ReportDebugCodeEnabled(VOID) { return FALSE; }
EFI_STATUS EFIAPI ReportStatusCodeWithDevicePath(
  EFI_STATUS_CODE_TYPE Type, EFI_STATUS_CODE_VALUE Value,
  CONST EFI_DEVICE_PATH_PROTOCOL *Path)
{ (VOID)Type; (VOID)Value; (VOID)Path; return EFI_SUCCESS; }

static void
KeyboardReport(EFI_EVENT Event, UINT8 Code)
{
  memset(Bus.RxResponse, 0, 8U);
  Bus.RxResponse[2] = Code;
  Bus.RxResponseLength = 8U;
  Bus.RxSuccess = 1U;
  Trigger(Event);
}

static void
CheckKey(USB_KB_DEV *Keyboard, UINT16 Scan, CHAR16 Character)
{
  EFI_INPUT_KEY Key;
  USBKeyboardTimerHandler(NULL, Keyboard);
  CHECK(USBKeyboardReadKeyStroke(&Keyboard->SimpleInput, &Key) == EFI_SUCCESS);
  CHECK(Key.ScanCode == Scan && Key.UnicodeChar == Character);
  CHECK(USBKeyboardReadKeyStroke(&Keyboard->SimpleInput, &Key) == EFI_NOT_READY);
}

int main(void)
{
  ASYNC_FIXTURE Fixture;
  USB_BUS UsbBus = {0};
  USB_DEVICE RootDevice = {0};
  USB_INTERFACE RootInterface = {0};
  USB_DEVICE Device = {0};
  USB_INTERFACE Interface = {0};
  USB_INTERFACE_SETTING Setting = {0};
  USB_ENDPOINT_DESC Endpoint = {0};
  USB_ENDPOINT_DESC *Endpoints[] = {&Endpoint};
  USB_KB_DEV Keyboard = {0};
  EFI_KEY_DESCRIPTOR Layout[NUMBER_OF_VALID_NON_MODIFIER_USB_KEYCODE + 8U] = {0};
  FAKE_EVENT Repeat = {0}, Recovery = {0};
  EFI_EVENT OldEvent, NewEvent;
  EFI_INPUT_KEY Key;
  const UINT8 Codes[] = {0x52U, 0x51U, 0x50U, 0x4fU, 0x28U, 0x29U, 0x43U};
  const UINT16 Scans[] = {SCAN_UP, SCAN_DOWN, SCAN_LEFT, SCAN_RIGHT, SCAN_NULL, SCAN_ESC, SCAN_F10};

  PrepareFixture(&Fixture);
  Bus.Registers[1U] = 0x10U; // High-speed hub upstream, full-speed keyboard on its TT.
  Bus.Registers[TEST_DEVCTL] = 0x5dU;
  Bus.Registers[TEST_INTRUSB] = 0U; // Cold preparation already consumed CONNECT.
  EFI_USB_PORT_STATUS Port;
  CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
  CHECK(Port.PortStatus == (MUSB_PORT_POWER | MUSB_PORT_CONNECTION));
  CHECK(Port.PortChangeStatus == MUSB_PORT_CHANGE_CONNECTION);
  Services.Stall = RootPortStall;
  Fixture.Host->UsbHc.GetRootHubPortStatus = MusbGetRootHubPortStatus;
  Fixture.Host->UsbHc.SetRootHubPortFeature = MusbSetRootHubPortFeature;
  Fixture.Host->UsbHc.ClearRootHubPortFeature = MusbClearRootHubPortFeature;
  UsbBus.Usb2Hc = &Fixture.Host->UsbHc;
  RootDevice.Bus = &UsbBus;RootInterface.Device = &RootDevice;
  RootInterface.MaxSpeed = EFI_USB_SPEED_HIGH;
  CHECK(UsbRootHubResetPort(&RootInterface, 0U) == EFI_SUCCESS);
  CHECK(MusbGetRootHubPortStatus(&Fixture.Host->UsbHc, 0U, &Port) == EFI_SUCCESS);
  CHECK((Port.PortStatus & (MUSB_PORT_ENABLE | MUSB_PORT_HIGH_SPEED)) ==
        (MUSB_PORT_ENABLE | MUSB_PORT_HIGH_SPEED));
  Services.RaiseTPL = HubRaiseTpl;
  Services.RestoreTPL = HubRestoreTpl;
  HubHaltClears = 0U;
  UsbBus.Usb2Hc = &Fixture.Host->UsbHc;
  Device.Bus = &UsbBus;
  Device.Connected = TRUE;
  Device.Address = 1U;
  Device.Speed = EFI_USB_SPEED_FULL;
  Device.Translator.TranslatorHubAddress = 2U;
  Device.Translator.TranslatorPortNumber = 3U;
  Interface.Signature = USB_INTERFACE_SIGNATURE;
  Interface.Device = &Device;
  Interface.IfSetting = &Setting;
  Interface.HubEp = &Endpoint; // clear-halt control-response fixture
  Interface.UsbIo.UsbAsyncInterruptTransfer = UsbIoAsyncInterruptTransfer;
  Interface.UsbIo.UsbControlTransfer = HubClearHalt;
  Interface.UsbIo.UsbGetInterfaceDescriptor = UsbIoGetInterfaceDescriptor;
  Interface.UsbIo.UsbGetEndpointDescriptor = UsbIoGetEndpointDescriptor;
  Setting.Desc.NumEndpoints = 1U;
  Setting.Endpoints = Endpoints;
  Endpoint.Desc.EndpointAddress = 0x81U;
  Endpoint.Desc.Attributes = USB_ENDPOINT_INTERRUPT;
  Endpoint.Desc.MaxPacketSize = 8U;
  Endpoint.Desc.Interval = 10U;

  Keyboard.Signature = USB_KB_DEV_SIGNATURE;
  Keyboard.UsbIo = &Interface.UsbIo;
  Keyboard.IntEndpointDescriptor = Endpoint.Desc;
  Keyboard.RepeatTimer = &Repeat;
  Keyboard.DelayedRecoveryEvent = &Recovery;
  Recovery.Notify = USBKeyboardRecoveryHandler;
  Recovery.NotifyContext = &Keyboard;
  Keyboard.KeyConvertionTable = Layout;
  InitializeListHead(&Keyboard.NotifyList);
  InitializeListHead(&Keyboard.NsKeyList);
  InitQueue(&Keyboard.UsbKeyQueue, sizeof(USB_KEY));
  InitQueue(&Keyboard.EfiKeyQueue, sizeof(EFI_KEY_DATA));
  InitQueue(&Keyboard.EfiKeyQueueForNotify, sizeof(EFI_KEY_DATA));
  for (UINTN Index = 0U; Index < USB_KEYBOARD_KEY_COUNT; ++Index) {
    EFI_KEY_DESCRIPTOR Descriptor;
    UINT8 Code;
    UINTN Position;
    CopyMem(&Descriptor, &mUsbKeyboardLayoutBin.KeyDescriptor[Index], sizeof(Descriptor));
    Code = EfiKeyToUsbKeyCodeConvertionTable[Descriptor.Key];
    Position = Code >= 0xe0U ?
      Code - 0xe0U + NUMBER_OF_VALID_NON_MODIFIER_USB_KEYCODE : Code - 4U;
    CHECK(Position < ARRAY_SIZE(Layout));
    Layout[Position] = Descriptor;
  }
  USBKeyboardRecoveryHandler(&Recovery, &Keyboard);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
  OldEvent = Fixture.Host->AsyncSlots[0].AsyncEvent;
  Bus.RxSuccess = 0U;
  for (UINTN Index = 0U; Index < 32U; ++Index) {
    Trigger(OldEvent);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == OldEvent);
    CHECK(!((FAKE_EVENT *)OldEvent)->Closed);
    CHECK(Recovery.TriggerTime == 0U);
    CHECK(USBKeyboardReadKeyStroke(&Keyboard.SimpleInput, &Key) == EFI_NOT_READY);
  }
  // A full-speed report through the hub may cross a frame boundary.
  Bus.RxDelayUs = 1250U;
  for (UINTN Index = 0U; Index < ARRAY_SIZE(Codes); ++Index) {
    KeyboardReport(OldEvent, Codes[Index]);
    CheckKey(&Keyboard, Scans[Index], Index == 4U ? CHAR_CARRIAGE_RETURN : CHAR_NULL);
    KeyboardReport(OldEvent, 0U);
    USBKeyboardTimerHandler(NULL, &Keyboard);
    CHECK(USBKeyboardReadKeyStroke(&Keyboard.SimpleInput, &Key) == EFI_NOT_READY);
    Bus.RxSuccess = 0U;
    Trigger(OldEvent);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncActive);
    CHECK(Fixture.Host->AsyncSlots[0].AsyncEvent == OldEvent);
  }
  KeyboardReport(OldEvent, Codes[0]);
  CheckKey(&Keyboard, SCAN_UP, CHAR_NULL);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 1U);
  Bus.RxSuccess = 0U;
  Bus.RxStall = 1U;
  Trigger(OldEvent);
  CHECK(HubHaltClears == 1U);
  CHECK(((FAKE_EVENT *)OldEvent)->Closed);
  CHECK(!Fixture.Host->AsyncSlots[0].AsyncActive);
  CHECK(Endpoint.Toggle == 0U);
  CHECK(Recovery.TimerType == TimerRelative && Recovery.TriggerTime == EFI_USB_INTERRUPT_DELAY);
  Trigger(&Recovery);
  NewEvent = Fixture.Host->AsyncSlots[0].AsyncEvent;
  CHECK(Fixture.Host->AsyncSlots[0].AsyncActive && NewEvent != OldEvent);
  CHECK(Fixture.Host->AsyncSlots[0].AsyncToggle == 0U);
  KeyboardReport(NewEvent, Codes[1]);
  CheckKey(&Keyboard, SCAN_DOWN, CHAR_NULL);
  CHECK(UsbIoAsyncInterruptTransfer(&Interface.UsbIo, 0x81U, FALSE,
          0U, 0U, NULL, NULL) == EFI_SUCCESS);
  DestroyQueue(&Keyboard.UsbKeyQueue);
  DestroyQueue(&Keyboard.EfiKeyQueue);
  DestroyQueue(&Keyboard.EfiKeyQueueForNotify);
  CleanupFixture(&Fixture);
  printf("MUSB real keyboard path: %u checks PASS (32 idle polls, delayed reports through a hub, four arrows, Enter, Esc, F10, key release, delayed STALL recovery, DATA0 restart)\n", Checks);
  return 0;
}
