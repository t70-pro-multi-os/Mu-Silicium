// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production BOT, UsbBus bulk and MUSB; device/control replies and MMIO are modeled.
#define main MusbDxeFixtureMain
#include "MusbHostDxeTest.c"
#undef main
#include "UsbMass.h"
#ifdef T70PRO_RAM_DIAGNOSTICS
#include <Protocol/T70UsbStorageDiagnostics.h>
EFI_STATUS UsbConnectDriver(USB_INTERFACE *UsbIf);
#endif

#ifdef T70PRO_RAM_DIAGNOSTICS
// Storage diagnostics are optional. The fixture checks command responses.
VOID T70UsbStorageDiagnostic(EFI_HANDLE Controller, UINT32 Stage,
  EFI_STATUS Status, UINT8 Opcode, UINT8 Lun, UINT32 Result,
  UINT8 Key, UINT8 Asc, UINT8 Ascq) {
  (VOID)Controller; (VOID)Stage; (VOID)Status; (VOID)Opcode; (VOID)Lun;
  (VOID)Result; (VOID)Key; (VOID)Asc; (VOID)Ascq;
}
#endif

EFI_STATUS UsbBootExecCmd(USB_MASS_DEVICE *, VOID *, UINT8,
  EFI_USB_DATA_DIRECTION, VOID *, UINT32, UINT32);

EFI_STATUS UsbBootExecCmdWithRetry(USB_MASS_DEVICE *, VOID *, UINT8,
  EFI_USB_DATA_DIRECTION, VOID *, UINT32, UINT32);

EFI_STATUS UsbBotSendCommand(USB_BOT_PROTOCOL *, UINT8 *, UINT8,
  EFI_USB_DATA_DIRECTION, UINT32, UINT8);
typedef struct {
  UINT8 Tx[512], Data[8192];
  UINTN TxLength, DataLength, DataPosition;
  USB_BOT_CBW Cbw;
  USB_BOT_CSW Csw;
  UINTN MaxPacket;
  UINT32 Commands, Reads, Clears, Resets, TtClears, NakEvery;
  EFI_USB_DEVICE_REQUEST LastTt;
  UINT16 TtValues[16];
  UINT8 Fault, ActiveFault;
  UINT8 CheckConditions, SenseKey, SenseAsc;
  UINT8 CbwStalls, CswStalls, CswErrors, ClearFailures;
  UINT8 ClearFailureEndpoints;
  UINT8 RecoveryEvents[32], RecoveryEventCount;
  UINT32 StatusAttempts;
  UINTN TransferDelayMs;
  UINT8 MaxLun;
  BOOLEAN MaxLunStalls;
  BOOLEAN StatusSent;
} STORAGE_MODEL;
static STORAGE_MODEL Storage;

static void CaptureTx(UINT32 Value, UINTN Width) {
  CHECK(Storage.TxLength + Width <= sizeof(Storage.Tx));
  for (UINTN I = 0; I < Width; ++I) {
    Storage.Tx[Storage.TxLength++] = (UINT8)(Value >> (I * 8U));
  }
}
static UINT8 StorageByte(UINTN Offset) {
  return (UINT8)((Offset * 37U + (Offset >> 8) * 13U + (Offset >> 16) * 7U) ^ 0x5aU);
}
static void BeginCommand(void) {
  CHECK(Storage.TxLength == sizeof(USB_BOT_CBW));
  memcpy(&Storage.Cbw, Storage.Tx, sizeof(Storage.Cbw));
  Storage.TxLength = 0;
  CHECK(Storage.Cbw.Signature == USB_BOT_CBW_SIGNATURE);
  CHECK(Storage.Cbw.DataLen <= sizeof(Storage.Data));
  CHECK(Storage.Cbw.CmdBlock[0] == 0x25U || Storage.Cbw.CmdBlock[0] == 0x28U ||
        Storage.Cbw.CmdBlock[0] == 0x88U || Storage.Cbw.CmdBlock[0] == 0x9eU ||
        Storage.Cbw.CmdBlock[0] == 0x12U || Storage.Cbw.CmdBlock[0] == 0x03U ||
        Storage.Cbw.CmdBlock[0] == 0x00U);
  Storage.ActiveFault = (Storage.Cbw.CmdBlock[0] == 0x03U && Storage.Fault < 7U) ? 0 : Storage.Fault;
  ++Storage.Commands;
  Storage.DataLength = Storage.Cbw.DataLen;
  Storage.DataPosition = 0;
  Storage.StatusSent = FALSE;
  for (UINTN I = 0; I < Storage.DataLength; ++I) {
    Storage.Data[I] = StorageByte(I);
  }
  if (Storage.Cbw.CmdBlock[0] == 0x25U) {
    const UINT8 Capacity[] = {0,0,0,63,0,0,2,0};
    CHECK(Storage.DataLength == sizeof(Capacity));
    memcpy(Storage.Data, Capacity, sizeof(Capacity));
  }
  if (Storage.Cbw.CmdBlock[0] == 0x03U) {
    memset(Storage.Data, 0, Storage.DataLength);
    Storage.Data[0] = 0x70; Storage.Data[7] = 10;
    Storage.Data[2] = Storage.SenseKey; Storage.Data[12] = Storage.SenseAsc;
  }
  Storage.Csw = (USB_BOT_CSW){USB_BOT_CSW_SIGNATURE, Storage.Cbw.Tag, 0, 0};
  if (Storage.ActiveFault == 1U) { ++Storage.Csw.Tag; }
  if (Storage.ActiveFault == 2U) { Storage.Csw.DataResidue = Storage.Cbw.DataLen + 1U; }
  if (Storage.ActiveFault == 6U) { Storage.Csw.CmdStatus = USB_BOT_COMMAND_FAILED; }
  if (Storage.Cbw.CmdBlock[0] != USB_BOOT_REQUEST_SENSE_OPCODE && Storage.CheckConditions != 0) {
    --Storage.CheckConditions;
    Storage.Csw.CmdStatus = USB_BOT_COMMAND_FAILED;
  }
  if (Storage.ActiveFault == 4U) { --Storage.DataLength; }
  if (Storage.ActiveFault == 7U || Storage.ActiveFault == 9U) { Storage.Csw.DataResidue=1; }
  if (Storage.ActiveFault == 8U) { Storage.Csw.DataResidue=Storage.Cbw.DataLen; }
  if (Storage.ActiveFault == 9U) { --Storage.DataLength; }
}
static int StorageWrite8(void *Context, uint32_t Offset, uint8_t Value) {
  if (Offset == TEST_FIFO_BASE + 4U) { CaptureTx(Value, 1); return 1; }
  return AsyncFakeWrite8(Context, Offset, Value);
}
static int StorageWrite32(void *Context, uint32_t Offset, uint32_t Value) {
  if (Offset == TEST_FIFO_BASE + 4U) { CaptureTx(Value, 4); return 1; }
  return AsyncFakeWrite32(Context, Offset, Value);
}
static int StorageWrite16(void *Context, uint32_t Offset, uint16_t Value) {
  if (Offset == TEST_FIFO_BASE + 4U) { CaptureTx(Value, 2); return 1; }
  if (Bus.Index == 1U && Offset == TEST_EP_BASE + 2U && (Value & 1U)) {
    BeginCommand();
  }
  if (Bus.Index == 1U && Offset == TEST_EP_BASE + TEST_RXCSR &&
      (Value & TEST_H_REQPKT)) {
    if ((Bus.Indexed[1][TEST_RXCSR] & TEST_RXPKTRDY) &&
        Bus.RxPosition < Bus.RxLength) {
      printf("unread RX discarded: offset=%lu length=%u read=%u\n",
        (unsigned long)Storage.DataPosition, Bus.RxLength, Bus.RxPosition);
      CHECK(FALSE);
    }
    ++Storage.Reads;
    if (Storage.NakEvery && Storage.Reads % Storage.NakEvery == 0) {
      Bus.RxSuccess = 0;
      return AsyncFakeWrite16(Context, Offset, Value);
    }
    Bus.RxSuccess = 1;
    if (Storage.DataPosition < Storage.DataLength) {
      UINTN Length = MIN(Storage.MaxPacket, Storage.DataLength - Storage.DataPosition);
      memcpy(Bus.RxResponse, Storage.Data + Storage.DataPosition, Length);
      Bus.RxResponseLength = (UINT16)Length;
      Storage.DataPosition += Length;
      if (Storage.ActiveFault == 5U) {
        Bus.RxStall = 1;
        Storage.DataPosition = Storage.DataLength;
      }
    } else {
      CHECK(!Storage.StatusSent);
      Storage.StatusSent = TRUE;
      memcpy(Bus.RxResponse, &Storage.Csw, sizeof(Storage.Csw));
      Bus.RxResponseLength = (UINT16)(sizeof(Storage.Csw) - (Storage.ActiveFault == 3U));
    }
  }
  return AsyncFakeWrite16(Context, Offset, Value);
}
static BOOLEAN StorageTimesOut(UINTN Timeout, UINT32 *Result) {
  Bus.TimeUs += MIN(Timeout, Storage.TransferDelayMs) * 1000U;
  if (Timeout >= Storage.TransferDelayMs) return FALSE;
  *Result = EFI_USB_ERR_TIMEOUT;
  return TRUE;
}

static EFI_STATUS EFIAPI StorageControl(
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Speed, UINTN Packet,
  EFI_USB_DEVICE_REQUEST *Request, EFI_USB_DATA_DIRECTION Direction,
  VOID *Data, UINTN *Length, UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result) {
  (VOID)This; (VOID)Address; (VOID)Speed; (VOID)Packet; (VOID)Direction;
  (VOID)Translator;
  *Result = EFI_USB_NOERROR;
  if (StorageTimesOut(Timeout, Result)) return EFI_TIMEOUT;
  if (Request->RequestType == 0xa1U && Request->Request == USB_BOT_GETLUN_REQUEST) {
    CHECK(*Length == 1U && Request->Length == 1U);
    if (Storage.MaxLunStalls) {
      *Result = EFI_USB_ERR_STALL;
      return EFI_DEVICE_ERROR;
    }
    *(UINT8 *)Data = Storage.MaxLun;
  } else if (Request->RequestType == 0x23U && Request->Request == 8U) {
    CHECK(Storage.TtClears < ARRAY_SIZE(Storage.TtValues));
    Storage.TtValues[Storage.TtClears] = Request->Value;
    Storage.LastTt = *Request;
    ++Storage.TtClears;
  } else if (Request->RequestType == 2U && Request->Request == USB_REQ_CLEAR_FEATURE) {
    ++Storage.Clears;
    CHECK(Storage.RecoveryEventCount<ARRAY_SIZE(Storage.RecoveryEvents));
    Storage.RecoveryEvents[Storage.RecoveryEventCount++]=Request->Index==0x81?1:2;
    if (Storage.ClearFailures) { --Storage.ClearFailures; return EFI_ACCESS_DENIED; }
    UINT8 EndpointBit = Request->Index == 0x81 ? 1 : 2;
    if (Storage.ClearFailureEndpoints & EndpointBit) {
      Storage.ClearFailureEndpoints &= (UINT8)~EndpointBit;
      return EFI_ACCESS_DENIED;
    }
  } else if (Request->RequestType == 0x21U && Request->Request == USB_BOT_RESET_REQUEST) {
    ++Storage.Resets;
    CHECK(Storage.RecoveryEventCount<ARRAY_SIZE(Storage.RecoveryEvents));
    Storage.RecoveryEvents[Storage.RecoveryEventCount++]=0;
  } else { CHECK(FALSE); }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StorageIoControl(
  EFI_USB_IO_PROTOCOL *This, EFI_USB_DEVICE_REQUEST *Request,
  EFI_USB_DATA_DIRECTION Direction, UINT32 Timeout, VOID *Data, UINTN Length,
  UINT32 *Result) {
  USB_INTERFACE *Interface = USB_INTERFACE_FROM_USBIO(This);
  USB_DEVICE *Device = Interface->Device;
  EFI_STATUS Status = StorageControl(Device->Bus->Usb2Hc, Device->Address,
    Device->Speed, Device->MaxPacket0, Request, Direction, Data, &Length,
    Timeout, &Device->Translator, Result);
  if (!EFI_ERROR(Status) && Request->RequestType == 2U && Request->Request == USB_REQ_CLEAR_FEATURE) {
    USB_ENDPOINT_DESC *Ep = UsbGetEndpointDesc(Interface, (UINT8)Request->Index);
    CHECK(Ep != NULL); Ep->Toggle = 0;
  }
  return Status;
}
static EFI_STATUS EFIAPI RecoveryBulk(EFI_USB_IO_PROTOCOL *This, UINT8 Endpoint,
  VOID *Data, UINTN *Length, UINTN Timeout, UINT32 *Result) {
  if (StorageTimesOut(Timeout, Result)) {
    *Length = 0U;
    return EFI_TIMEOUT;
  }
  if (!(Endpoint&0x80) && *Length==sizeof(USB_BOT_CBW) && Storage.CbwStalls) {
    --Storage.CbwStalls; *Length=0; *Result=EFI_USB_ERR_STALL; return EFI_DEVICE_ERROR;
  }
  if ((Endpoint&0x80) && *Length==sizeof(USB_BOT_CSW)) {
    ++Storage.StatusAttempts;
    if (Storage.CswStalls || Storage.CswErrors) {
      *Length=0;
      if (Storage.CswStalls) { --Storage.CswStalls; *Result=EFI_USB_ERR_STALL; }
      else { --Storage.CswErrors; *Result=EFI_USB_ERR_TIMEOUT; }
      return EFI_DEVICE_ERROR;
    }
  }
  return UsbIoBulkTransfer(This,Endpoint,Data,Length,Timeout,Result);
}
static EFI_STATUS EFIAPI StorageStall(UINTN Us) { Bus.TimeUs += Us; return EFI_SUCCESS; }

typedef struct {
  ASYNC_FIXTURE Host;
  USB_BUS Bus;
  USB_DEVICE Device, Hub;
  USB_INTERFACE Interface, HubInterface;
  USB_INTERFACE_SETTING Setting, HubSetting;
  USB_ENDPOINT_DESC In, Out;
  USB_ENDPOINT_DESC *Endpoints[2];
  VOID *Bot;
} STORAGE_FIXTURE;
static void PrepareStorage(STORAGE_FIXTURE *F, UINT8 Speed, BOOLEAN BehindHub) {
  memset(F, 0, sizeof(*F));
  memset(&Storage, 0, sizeof(Storage));
  PrepareFixture(&F->Host);
  EnableFixturePort(&F->Host);
  Storage.MaxPacket = Speed == EFI_USB_SPEED_HIGH ? 512U : 64U;
  F->Host.Host->Host.Io.Write8 = StorageWrite8;
  F->Host.Host->Host.Io.Write16 = StorageWrite16;
  F->Host.Host->Host.Io.Write32 = StorageWrite32;
  F->Host.Host->UsbHc.BulkTransfer = MusbBulkTransfer;
  F->Host.Host->UsbHc.ControlTransfer = StorageControl;
  F->Bus.Usb2Hc = &F->Host.Host->UsbHc;
  F->Bus.MaxDevices = ARRAY_SIZE(F->Bus.Devices);
  F->Bus.Devices[2] = &F->Hub;
  F->Hub.Bus = &F->Bus; F->Hub.Address = 2;
  F->Hub.Speed = EFI_USB_SPEED_HIGH; F->Hub.MaxPacket0 = 64;
  F->Hub.NumOfInterface = 1; F->Hub.Interfaces[0] = &F->HubInterface;
  F->HubInterface.Device = &F->Hub; F->HubInterface.IfSetting = &F->HubSetting;
  F->HubSetting.Desc.InterfaceClass = USB_HUB_CLASS_CODE;
  F->HubSetting.Desc.InterfaceProtocol = 2;
  F->Device.Bus = &F->Bus; F->Device.Address = 1; F->Device.Speed = Speed;
  F->Device.Connected = TRUE; F->Device.MaxPacket0 = 64;
  if (BehindHub) {
    Bus.Registers[1] = 0x10U; // High-speed upstream link for the real TT routing check.
    F->Device.Translator.TranslatorHubAddress = 2;
    F->Device.Translator.TranslatorPortNumber = 3;
  }
  F->Interface.Signature = USB_INTERFACE_SIGNATURE;
  F->Interface.Device = &F->Device; F->Interface.IfSetting = &F->Setting;
  F->Interface.UsbIo.UsbBulkTransfer = RecoveryBulk;
  F->Interface.UsbIo.UsbControlTransfer = StorageIoControl;
  F->Interface.UsbIo.UsbGetInterfaceDescriptor = UsbIoGetInterfaceDescriptor;
  F->Interface.UsbIo.UsbGetEndpointDescriptor = UsbIoGetEndpointDescriptor;
  F->Setting.Desc.NumEndpoints = 2;
  F->Setting.Desc.InterfaceProtocol = USB_MASS_STORE_BOT;
  F->Setting.Desc.InterfaceClass = 8;
  F->Setting.Desc.InterfaceSubClass = 6;
  F->Setting.Endpoints = F->Endpoints;
  F->Endpoints[0] = &F->In; F->Endpoints[1] = &F->Out;
  F->In.Desc.EndpointAddress = 0x81; F->Out.Desc.EndpointAddress = 0x02;
  F->In.Desc.Attributes = F->Out.Desc.Attributes = USB_ENDPOINT_BULK;
  F->In.Desc.MaxPacketSize = F->Out.Desc.MaxPacketSize = (UINT16)Storage.MaxPacket;
  Services.RaiseTPL = HubRaiseTpl; Services.RestoreTPL = HubRestoreTpl;
  Services.Stall = StorageStall;
  CHECK(UsbBotInit(&F->Interface.UsbIo, &F->Bot) == EFI_SUCCESS);
}
static void FinishStorage(STORAGE_FIXTURE *F) {
  UsbBotCleanUp(F->Bot); CleanupFixture(&F->Host);
}
static EFI_STATUS Capacity(STORAGE_FIXTURE *F, UINT32 *Result) {
  UINT8 Cmd[10] = {0x25}, Data[8] = {0};
  EFI_STATUS Status = UsbBotExecCommand(F->Bot, Cmd, sizeof(Cmd),
    EfiUsbDataIn, Data, sizeof(Data), 0, 1000000, Result);
  if (!EFI_ERROR(Status) && *Result == USB_MASS_CMD_SUCCESS && Storage.ActiveFault == 0) {
    CHECK(Data[3] == 63 && Data[6] == 2 && Data[7] == 0);
  }
  return Status;
}
static void SuccessPaths(void) {
  for (UINTN Mode = 0; Mode < 3; ++Mode) {
    STORAGE_FIXTURE F; UINT32 Result;
    PrepareStorage(&F, Mode == 0 ? EFI_USB_SPEED_HIGH : EFI_USB_SPEED_FULL, Mode == 2);
    CHECK(Capacity(&F, &Result) == EFI_SUCCESS && Result == USB_MASS_CMD_SUCCESS);
    const UINT8 Counts[] = {1, 2, 3, 7, 8, 16};
    Storage.NakEvery = Mode == 2 ? 3 : 0;
    for (UINTN Count = 0; Count < ARRAY_SIZE(Counts); ++Count) {
      UINT8 Cmd[10] = {0x28,0,0,0,0,1,0,0,Counts[Count],0}, Data[8192] = {0};
      UINT32 Length = (UINT32)Counts[Count] * 512U;
      CHECK(UsbBotExecCommand(F.Bot, Cmd, sizeof(Cmd), EfiUsbDataIn, Data,
        Length, 0, 1000000, &Result) == EFI_SUCCESS);
      CHECK(Result == USB_MASS_CMD_SUCCESS);
      for (UINTN I = 0; I < Length; ++I) { CHECK(Data[I] == StorageByte(I)); }
    }
    CHECK(Storage.Commands == 1 + ARRAY_SIZE(Counts));
    F.Device.Connected = FALSE;
    UINT32 Commands = Storage.Commands;
    CHECK(EFI_ERROR(Capacity(&F, &Result)) && Result != USB_MASS_CMD_SUCCESS);
    CHECK(Storage.Commands == Commands);
    F.Device.Connected = TRUE;
    F.In.Toggle = F.Out.Toggle = 0;
    CHECK(Capacity(&F, &Result) == EFI_SUCCESS && Result == USB_MASS_CMD_SUCCESS);
    FinishStorage(&F);
  }
}
static void RootPortTransitions(void) {
  STORAGE_FIXTURE F; EFI_USB_PORT_STATUS Port; UINT32 Result;
  USB_DEVICE Root = {0}; USB_INTERFACE RootIf = {0};
  PrepareStorage(&F, EFI_USB_SPEED_FULL, FALSE);
  F.Host.Host->UsbHc.GetRootHubPortStatus = MusbGetRootHubPortStatus;
  F.Host.Host->UsbHc.SetRootHubPortFeature = MusbSetRootHubPortFeature;
  F.Host.Host->UsbHc.ClearRootHubPortFeature = MusbClearRootHubPortFeature;
  Root.Bus = &F.Bus; RootIf.Device = &Root; RootIf.MaxSpeed = EFI_USB_SPEED_HIGH;
  for (UINTN Cycle = 0; Cycle < 64; ++Cycle) {
    CHECK(MusbGetRootHubPortStatus(&F.Host.Host->UsbHc, 0, &Port) == EFI_SUCCESS);
    CHECK(Port.PortStatus & MUSB_PORT_CONNECTION);
    CHECK(MusbClearRootHubPortFeature(&F.Host.Host->UsbHc, 0, EfiUsbPortConnectChange) == EFI_SUCCESS);
    Bus.Registers[TEST_DEVCTL] = 0x1dU;
    Bus.Registers[TEST_INTRUSB] = 0x20U;
    CHECK(MusbGetRootHubPortStatus(&F.Host.Host->UsbHc, 0, &Port) == EFI_SUCCESS);
    CHECK(!(Port.PortStatus & MUSB_PORT_CONNECTION));
    CHECK(Port.PortChangeStatus & MUSB_PORT_CHANGE_CONNECTION);
    F.Device.Connected = FALSE;
    CHECK(EFI_ERROR(Capacity(&F, &Result)));
    CHECK(MusbClearRootHubPortFeature(&F.Host.Host->UsbHc, 0, EfiUsbPortConnectChange) == EFI_SUCCESS);
    Bus.Registers[TEST_DEVCTL] = 0x5dU;
    Bus.Registers[TEST_INTRUSB] = 0x10U;
    CHECK(MusbGetRootHubPortStatus(&F.Host.Host->UsbHc, 0, &Port) == EFI_SUCCESS);
    CHECK(Port.PortStatus & MUSB_PORT_CONNECTION);
    CHECK(Port.PortChangeStatus & MUSB_PORT_CHANGE_CONNECTION);
    CHECK(UsbRootHubResetPort(&RootIf, 0) == EFI_SUCCESS);
    F.Device.Connected = TRUE; F.In.Toggle = F.Out.Toggle = 0;
    CHECK(Capacity(&F, &Result) == EFI_SUCCESS && Result == USB_MASS_CMD_SUCCESS);
  }
  FinishStorage(&F);
  printf("64 modeled root-port disconnect/reset/reconnect/read cycles passed\n");
}
static void TtRecovery(void) {
  STORAGE_FIXTURE F; UINT32 Result;
  PrepareStorage(&F, EFI_USB_SPEED_FULL, TRUE);
  CHECK(UsbHubCtrlClearTTBuffer(&F.Hub, 3, 1, 0x81, USB_ENDPOINT_BULK) == EFI_SUCCESS);
  printf("TT direct actual=%04x port=%u expected=9011/3\n", Storage.LastTt.Value, Storage.LastTt.Index);
  CHECK(Storage.LastTt.Value == 0x9011U && Storage.LastTt.Index == 3);
  Storage.Fault = 5;
  (VOID)Capacity(&F, &Result);
  printf("TT recovery actual=%04x port=%u expected=9011/3\n", Storage.LastTt.Value, Storage.LastTt.Index);
  CHECK(Storage.TtClears >= 2);
  CHECK(Storage.LastTt.Value == 0x9011U && Storage.LastTt.Index == 3);
  F.HubSetting.Desc.InterfaceProtocol = 1;
  CHECK(UsbHubCtrlClearTTBuffer(&F.Hub, 3, 1, 0x02, USB_ENDPOINT_BULK) == EFI_SUCCESS);
  printf("TT single actual=%04x port=%u expected=1012/1\n", Storage.LastTt.Value, Storage.LastTt.Index);
  CHECK(Storage.LastTt.Value == 0x1012 && Storage.LastTt.Index == 1);
  UINT32 Before = Storage.TtClears;
  CHECK(UsbHubCtrlClearTTBuffer(&F.Hub, 3, 1, 0, USB_ENDPOINT_CONTROL) == EFI_SUCCESS);
  CHECK(Storage.TtClears == Before + 2);
  CHECK(Storage.TtValues[Before] == 0x8010 && Storage.TtValues[Before + 1] == 0x0010);
  CHECK(Storage.LastTt.Index == 1);
  FinishStorage(&F);
}
static void FaultPaths(void) {
  for (UINT8 Fault = 1; Fault <= 5; ++Fault) {
    STORAGE_FIXTURE F; UINT32 Result;
    PrepareStorage(&F, EFI_USB_SPEED_HIGH, FALSE);
    Storage.Fault = Fault;
    EFI_STATUS Status = Capacity(&F, &Result);
    printf("BOT fault=%u status=%llx result=%u\n", Fault, (unsigned long long)Status, Result);
    CHECK(EFI_ERROR(Status) && Result != USB_MASS_CMD_SUCCESS);
    FinishStorage(&F);
  }
}
static void BootClassFaultPaths(void) {
  for (UINT8 Fault = 1; Fault <= 6; ++Fault) {
    STORAGE_FIXTURE F; USB_MASS_DEVICE Mass = {0};
    UINT8 Cmd[10] = {0x25}, Data[8] = {0};
    PrepareStorage(&F, EFI_USB_SPEED_HIGH, FALSE);
    Mass.Transport = &mUsbBotTransport; Mass.Context = F.Bot;
    Storage.Fault = Fault;
    EFI_STATUS Status = UsbBootExecCmd(&Mass, Cmd, sizeof(Cmd),
      EfiUsbDataIn, Data, sizeof(Data), 1000000);
    printf("USB storage class fault=%u status=%llx last_opcode=%02x\n",
      Fault, (unsigned long long)Status, Storage.Cbw.CmdBlock[0]);
    CHECK(Storage.Cbw.CmdBlock[0] == USB_BOOT_REQUEST_SENSE_OPCODE);
    CHECK(EFI_ERROR(Status));
    Storage.Fault = 0;
    CHECK(UsbBootExecCmd(&Mass, Cmd, sizeof(Cmd), EfiUsbDataIn,
      Data, sizeof(Data), 1000000) == EFI_SUCCESS);
    FinishStorage(&F);
  }
}
static void ResiduePaths(void) {
  const UINT8 Opcodes[]={0x25,0x28,0x88,0x9e};
  for (UINTN I=0;I<ARRAY_SIZE(Opcodes);I++) {
    for (UINT8 Fault=7;Fault<=9;Fault++) {
      STORAGE_FIXTURE F; UINT32 Result; UINT8 Cmd[16]={0}, Data[512]={0};
      UINT8 CmdLen=(Opcodes[I]==0x88 || Opcodes[I]==0x9e)?16:10;
      UINT32 Length=Opcodes[I]==0x25?8:Opcodes[I]==0x9e?32:512;
      PrepareStorage(&F,EFI_USB_SPEED_HIGH,FALSE); Cmd[0]=Opcodes[I];
      if (Cmd[0]==0x9e) Cmd[1]=0x10;
      Storage.Fault=Fault;
      CHECK(EFI_ERROR(UsbBotExecCommand(F.Bot,Cmd,CmdLen,EfiUsbDataIn,Data,Length,0,1000000,&Result)));
      CHECK(Result!=USB_MASS_CMD_SUCCESS);
      // The following normal command still works after the malformed result.
      Storage.Fault=0;
      CHECK(Capacity(&F,&Result)==EFI_SUCCESS && Result==USB_MASS_CMD_SUCCESS);
      FinishStorage(&F);
    }
  }
  // Normal shorter INQUIRY and REQUEST SENSE data remain supported.
  const UINT8 Variable[]={0x12,0x03};
  for (UINTN I=0;I<ARRAY_SIZE(Variable);I++) {
    STORAGE_FIXTURE F; UINT32 Result; UINT8 Cmd[6]={0}, Data[36]={0};
    PrepareStorage(&F,EFI_USB_SPEED_HIGH,FALSE); Cmd[0]=Variable[I]; Cmd[4]=sizeof(Data);
    Storage.Fault=9;
    CHECK(UsbBotExecCommand(F.Bot,Cmd,sizeof(Cmd),EfiUsbDataIn,Data,sizeof(Data),0,1000000,&Result)==EFI_SUCCESS);
    CHECK(Result==USB_MASS_CMD_SUCCESS && Storage.Csw.DataResidue==1);
    CHECK(Storage.DataLength==sizeof(Data)-1 && Data[sizeof(Data)-1]==0);
    FinishStorage(&F);
  }
}
static void RecoveryPaths(void) {
  const EFI_USB_DATA_DIRECTION Directions[]={EfiUsbDataIn,EfiUsbNoData,EfiUsbDataOut};
  for (UINTN I=0;I<ARRAY_SIZE(Directions);I++) {
    STORAGE_FIXTURE F; UINT8 Cmd[10]={0x28};
    PrepareStorage(&F,EFI_USB_SPEED_HIGH,FALSE); Storage.CbwStalls=1;
    CHECK(EFI_ERROR(UsbBotSendCommand(F.Bot,Cmd,sizeof(Cmd),Directions[I],0,0)));
    CHECK(Storage.Resets==1 && Storage.Clears==2 && Storage.RecoveryEventCount==3);
    CHECK(Storage.RecoveryEvents[0]==0 && Storage.RecoveryEvents[1]==1 && Storage.RecoveryEvents[2]==2);
    FinishStorage(&F);
  }
  for (UINT8 Stalls=1;Stalls<=2;Stalls++) {
    STORAGE_FIXTURE F; UINT32 Result;
    PrepareStorage(&F,EFI_USB_SPEED_HIGH,FALSE); Storage.CswStalls=Stalls;
    EFI_STATUS Status=Capacity(&F,&Result);
    CHECK(Storage.StatusAttempts==2);
    if (Stalls==1) {
      CHECK(Status==EFI_SUCCESS && Result==USB_MASS_CMD_SUCCESS);
      CHECK(Storage.Resets==0 && Storage.Clears==1);
    } else {
      CHECK(EFI_ERROR(Status) && Result!=USB_MASS_CMD_SUCCESS);
      CHECK(Storage.Resets==1 && Storage.Clears==3 && Storage.RecoveryEventCount==4);
      CHECK(Storage.RecoveryEvents[0]==1 && Storage.RecoveryEvents[1]==0 &&
            Storage.RecoveryEvents[2]==1 && Storage.RecoveryEvents[3]==2);
    }
    CHECK(Capacity(&F,&Result)==EFI_SUCCESS && Result==USB_MASS_CMD_SUCCESS);
    FinishStorage(&F);
  }
  STORAGE_FIXTURE F; UINT32 Result;
  PrepareStorage(&F,EFI_USB_SPEED_HIGH,FALSE); Storage.CswErrors=1;
  CHECK(Capacity(&F,&Result)==EFI_SUCCESS && Result==USB_MASS_CMD_SUCCESS);
  CHECK(Storage.StatusAttempts==2 && Storage.Resets==0 && Storage.Clears==1);
  FinishStorage(&F);
  PrepareStorage(&F,EFI_USB_SPEED_HIGH,FALSE); Storage.CswStalls=1; Storage.ClearFailures=1;
  CHECK(EFI_ERROR(Capacity(&F,&Result)) && Result!=USB_MASS_CMD_SUCCESS);
  CHECK(Storage.StatusAttempts==1 && Storage.Resets==1 && Storage.Clears==3);
  FinishStorage(&F);
}
static void ResetClearFailures(void) {
  for (UINT8 FailMask=1;FailMask<=3;FailMask++) {
    STORAGE_FIXTURE F; UINT32 Result;
    PrepareStorage(&F,EFI_USB_SPEED_HIGH,FALSE);
    F.In.Toggle=F.Out.Toggle=1;
    Storage.ClearFailureEndpoints=FailMask;
    CHECK(UsbBotResetDevice(F.Bot,FALSE)==EFI_ACCESS_DENIED);
    CHECK(Storage.Resets==1 && Storage.Clears==2 && Storage.RecoveryEventCount==3);
    CHECK(Storage.RecoveryEvents[0]==0 && Storage.RecoveryEvents[1]==1 && Storage.RecoveryEvents[2]==2);
    CHECK(F.In.Toggle==((FailMask&1)!=0) && F.Out.Toggle==((FailMask&2)!=0));
    CHECK(UsbBotResetDevice(F.Bot,FALSE)==EFI_SUCCESS);
    CHECK(Storage.Resets==2 && Storage.Clears==4 && Storage.RecoveryEventCount==6);
    CHECK(F.In.Toggle==0 && F.Out.Toggle==0);
    CHECK(Capacity(&F,&Result)==EFI_SUCCESS && Result==USB_MASS_CMD_SUCCESS);
    FinishStorage(&F);
  }
}
#ifdef T70PRO_RAM_DIAGNOSTICS
static EFI_USB_PORT_STATUS HubPortState[2];
static UINTN HubPortResets[2];
static UINTN HubPortReads[2];

static EFI_STATUS EFIAPI
HubPortControl(EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Speed,
               UINTN Packet, EFI_USB_DEVICE_REQUEST *Request,
               EFI_USB_DATA_DIRECTION Direction, VOID *Data, UINTN *Bytes,
               UINTN Timeout, EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
               UINT32 *Result)
{
  (VOID)This; (VOID)Packet; (VOID)Direction; (VOID)Timeout; (VOID)Translator;
  CHECK(Address == 1 && Speed == EFI_USB_SPEED_HIGH);
  *Result = EFI_USB_NOERROR;
  if (Request->RequestType == 0xa0 && Request->Request == USB_HUB_REQ_GET_STATUS) {
    CHECK(*Bytes == sizeof(EFI_USB_PORT_STATUS));
    ZeroMem(Data, *Bytes);
    return EFI_SUCCESS;
  }

  CHECK(Request->Index >= 1 && Request->Index <= ARRAY_SIZE(HubPortState));
  UINTN Port = Request->Index - 1;
  EFI_USB_PORT_STATUS *State = &HubPortState[Port];
  if (Request->Request == USB_HUB_REQ_GET_STATUS) {
    CHECK(*Bytes == sizeof(*State));
    CopyMem(Data, State, *Bytes);
    ++HubPortReads[Port];
  } else if (Request->Request == USB_HUB_REQ_SET_FEATURE && Request->Value == USB_HUB_PORT_RESET) {
    State->PortStatus &= ~USB_PORT_STAT_ENABLE;
    State->PortStatus |= USB_PORT_STAT_RESET;
    ++HubPortResets[Port];
  } else {
    CHECK(Request->Request == USB_HUB_REQ_CLEAR_FEATURE && *Bytes == 0);
    switch (Request->Value) {
      case EfiUsbPortEnable:
        State->PortStatus &= ~(USB_PORT_STAT_ENABLE | USB_PORT_STAT_RESET);
        break;
      case EfiUsbPortConnectChange:
        State->PortChangeStatus &= ~USB_PORT_STAT_C_CONNECTION;
        break;
      case EfiUsbPortEnableChange:
        State->PortChangeStatus &= ~USB_PORT_STAT_C_ENABLE;
        break;
      case EfiUsbPortResetChange:
        State->PortChangeStatus &= ~USB_PORT_STAT_C_RESET;
        break;
      default:
        CHECK(FALSE);
    }
  }

  return EFI_SUCCESS;
}

static void NotifyHubPorts(USB_INTERFACE *Hub, UINT8 Changes)
{
  Hub->ChangeMap = AllocateZeroPool(1);
  CHECK(Hub->ChangeMap != NULL);
  *Hub->ChangeMap = Changes;
  UsbHubEnumeration(NULL, Hub);
}

static void DeferredHubPortReset(void)
{
  EFI_USB2_HC_PROTOCOL Hc = {0};
  USB_BUS UsbBus = {0};
  USB_DEVICE HubDevice = {0};
  USB_INTERFACE Hub = {0};

  ResetEvents();
  Services.FreePool = EnumFreePool;
  Services.Stall = RootPortStall;
  Hc.ControlTransfer = HubPortControl;
  UsbBus.Usb2Hc = &Hc;
  UsbBus.MaxDevices = ARRAY_SIZE(UsbBus.Devices);
  HubDevice.Bus = &UsbBus;
  HubDevice.Address = 1;
  HubDevice.Speed = EFI_USB_SPEED_HIGH;
  HubDevice.MaxPacket0 = 64;
  Hub.Device = &HubDevice;
  Hub.HubApi = &mUsbHubApi;
  Hub.NumOfPort = ARRAY_SIZE(HubPortState);
  for (UINTN Port = 0; Port < Hub.NumOfPort; ++Port) {
    HubPortState[Port] = (EFI_USB_PORT_STATUS){ USB_PORT_STAT_CONNECTION, USB_PORT_STAT_C_CONNECTION };
  }

  NotifyHubPorts(&Hub, BIT1 | BIT2);
  EFI_EVENT First = Hub.PortWork[0].Event;
  EFI_EVENT Second = Hub.PortWork[1].Event;
  UINT64 Before = Bus.TimeUs;
  Trigger(First);
  CHECK(UsbBus.ResetOwner == &Hub.PortWork[0] && HubPortResets[0] == 1);
  CHECK(((FAKE_EVENT *)First)->TriggerTime == USB_SET_PORT_RESET_STALL * 10ULL);
  Trigger(Second);
  CHECK(HubPortResets[1] == 0 && !((FAKE_EVENT *)Second)->Closed);
  UINTN Reads = HubPortReads[0];
  Trigger(First);
  CHECK(HubPortReads[0] == Reads + 1 && Bus.TimeUs == Before);

  HubPortState[0] = (EFI_USB_PORT_STATUS){ USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE,
                                        USB_PORT_STAT_C_RESET | USB_PORT_STAT_C_ENABLE };
  NotifyHubPorts(&Hub, BIT1);
  CHECK(Hub.PortWork[0].Event == First && UsbBus.ResetOwner == &Hub.PortWork[0]);
  Trigger(First);
  CHECK(HubPortState[0].PortChangeStatus == 0);
  CHECK(((FAKE_EVENT *)First)->TriggerTime == USB_SET_PORT_RECOVERY_STALL * 10ULL);

  HubPortState[0] = (EFI_USB_PORT_STATUS){ 0, USB_PORT_STAT_C_CONNECTION };
  Trigger(First);
  CHECK(((FAKE_EVENT *)First)->Closed && UsbBus.ResetOwner == NULL);
  Trigger(Second);
  CHECK(HubPortResets[1] == 1 && UsbBus.ResetOwner == &Hub.PortWork[1]);
  UINTN Polls = 0;
  while ((UsbBus.ResetOwner != NULL) && (++Polls <= 60)) {
    Trigger(Second);
  }
  CHECK(Polls <= 60 && Bus.TimeUs == Before);
  CHECK(UsbBus.ResetOwner == NULL && Hub.PortWork[1].Retries == 2);
  CHECK(!(HubPortState[1].PortStatus & USB_PORT_STAT_ENABLE));
  Trigger(Second);
  CHECK(HubPortResets[1] == 2 && UsbBus.ResetOwner == &Hub.PortWork[1]);
  HubPortState[1] = (EFI_USB_PORT_STATUS){ USB_PORT_STAT_CONNECTION, USB_PORT_STAT_C_CONNECTION };
  NotifyHubPorts(&Hub, BIT2);
  EFI_EVENT Replacement = Hub.PortWork[1].Event;
  CHECK(Replacement != NULL && Replacement != Second && ((FAKE_EVENT *)Second)->Closed);
  CHECK(UsbBus.ResetOwner == NULL);
  UsbCancelPortWork(&Hub);
  CHECK(((FAKE_EVENT *)Replacement)->Closed && UsbBus.ResetOwner == NULL);
}

static T70_USB_STORAGE_DIAGNOSTICS BindDiagnostics;
static EFI_STATUS BindResult;
static BOOLEAN UpdateBindRecord;

static EFI_TPL EFIAPI StorageCallbackTpl(EFI_TPL NewTpl) {
  CHECK(NewTpl == TPL_HIGH_LEVEL || NewTpl == TPL_CALLBACK);
  return TPL_CALLBACK;
}

static EFI_STATUS EFIAPI LocateStorageDiagnostics(EFI_GUID *Guid, VOID *Registration,
                                                 VOID **Interface) {
  EFI_GUID Expected = T70_USB_STORAGE_DIAGNOSTICS_GUID;
  CHECK(Registration == NULL && !memcmp(Guid, &Expected, sizeof(Expected)));
  *Interface = &BindDiagnostics;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI BindStorage(EFI_HANDLE Controller, EFI_HANDLE *Driver,
                                      EFI_DEVICE_PATH_PROTOCOL *Remaining, BOOLEAN Recursive) {
  CHECK(Controller != NULL && Driver == NULL && Remaining == NULL && Recursive);
  if (UpdateBindRecord) {
    BindDiagnostics.Records[0].Stage = 5;
    BindDiagnostics.Records[0].Controller = Controller;
    BindDiagnostics.Records[0].Sequence = ++BindDiagnostics.Sequence;
    BindDiagnostics.Records[0].BindStatus = BindResult;
    BindDiagnostics.Count = 1;
  }
  // Match DXE Core, which does not propagate a driver Start() error.
  return EFI_ERROR(BindResult) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

static void StorageBindRetry(void) {
  const EFI_STATUS Errors[] = { EFI_NOT_READY, EFI_TIMEOUT, EFI_UNSUPPORTED };
  for (UINTN Index = 0U; Index < ARRAY_SIZE(Errors); ++Index) {
    STORAGE_FIXTURE F;
    PrepareStorage(&F, EFI_USB_SPEED_HIGH, FALSE);
    F.Bus.Signature = USB_BUS_SIGNATURE;
    F.Device.Connected = TRUE;
    F.Interface.Handle = &F.Interface;
    InitializeListHead(&F.Bus.WantedUsbIoDPList);
    CHECK(UsbBusAddWantedUsbIoDP(&F.Bus.BusId, NULL) == EFI_SUCCESS);
    Services.RaiseTPL = StorageCallbackTpl;
    Services.LocateProtocol = LocateStorageDiagnostics;
    Services.ConnectController = BindStorage;
    BindDiagnostics = (T70_USB_STORAGE_DIAGNOSTICS){ .Version = T70_USB_DIAG_VERSION };
    BindResult = Errors[Index];
    UpdateBindRecord = TRUE;
    CHECK(UsbConnectDriver(&F.Interface) == EFI_SUCCESS);

    EFI_EVENT Event = F.Interface.StorageConnectEvent;
    CHECK(Event != NULL);
    Trigger(Event);
    if (BindResult == EFI_NOT_READY || BindResult == EFI_TIMEOUT) {
      CHECK(F.Interface.StorageConnectEvent == Event &&
            ((FAKE_EVENT *)Event)->TriggerTime == EFI_TIMER_PERIOD_MILLISECONDS(200U));
      if (BindResult == EFI_NOT_READY) {
        BindResult = EFI_SUCCESS;
      } else {
        // An old TIMEOUT record cannot make a new NOT_FOUND retryable.
        UpdateBindRecord = FALSE;
      }
      Trigger(Event);
    }
    CHECK(F.Interface.StorageConnectEvent == NULL && ((FAKE_EVENT *)Event)->Closed);
    CHECK(F.Interface.IsManaged == (Errors[Index] == EFI_NOT_READY));
    CHECK(F.Interface.StorageConnectAttempts == (Errors[Index] == EFI_UNSUPPORTED ? 1U : 2U));
    CHECK(UsbBusFreeUsbDPList(&F.Bus.WantedUsbIoDPList) == EFI_SUCCESS);
    Services.RaiseTPL = HubRaiseTpl;
    FinishStorage(&F);
  }
}

static void SlowMediaProbe(void) {
  STORAGE_FIXTURE F;
  UINT8 UnitReady[6] = {0};
  UINT8 MaxLun = 0;
  UINT32 Result;

  PrepareStorage(&F, EFI_USB_SPEED_HIGH, FALSE);
  ((USB_BOT_PROTOCOL *)F.Bot)->MediaProbe = TRUE;
  Storage.TransferDelayMs = 300U;
  Storage.MaxLun = 2U;
  CHECK(UsbBotGetMaxLun(F.Bot, &MaxLun) == EFI_SUCCESS && MaxLun == 2U);
  CHECK(UsbBotExecCommand(F.Bot, UnitReady, sizeof(UnitReady), EfiUsbNoData,
    NULL, 0U, 0U, 1000000U, &Result) == EFI_SUCCESS && Result == USB_MASS_CMD_SUCCESS);
  CHECK(Capacity(&F, &Result) == EFI_SUCCESS && Result == USB_MASS_CMD_SUCCESS);
  CHECK(Storage.Commands == 2U && Storage.Resets == 0U);

  CHECK(UsbBotResetDevice(F.Bot, FALSE) == EFI_SUCCESS);
  CHECK(Storage.Resets == 1U && Storage.Clears == 2U);
  Storage.MaxLunStalls = TRUE;
  CHECK(UsbBotGetMaxLun(F.Bot, &MaxLun) == EFI_SUCCESS && MaxLun == 0U);
  Storage.MaxLunStalls = FALSE;
  Storage.TransferDelayMs = 4000U;
  CHECK(UsbBotGetMaxLun(F.Bot, &MaxLun) == EFI_TIMEOUT && MaxLun == 0U);
  FinishStorage(&F);
}

static void MediaProbeReadiness(void) {
  STORAGE_FIXTURE F;
  USB_MASS_DEVICE Mass = {0};
  UINT8 Cmd[10] = {0x25}, Data[8] = {0};

  PrepareStorage(&F, EFI_USB_SPEED_HIGH, FALSE);
  ((USB_BOT_PROTOCOL *)F.Bot)->MediaProbe = TRUE;
  Mass.Transport = &mUsbBotTransport;
  Mass.Context = F.Bot;

  Storage.CheckConditions = 1;
  Storage.SenseKey = 2;
  Storage.SenseAsc = 4;
  UINT64 Start = Bus.TimeUs;
  CHECK(UsbBootExecCmdWithRetry(&Mass, Cmd, sizeof(Cmd), EfiUsbDataIn,
    Data, sizeof(Data), 1000000) == EFI_SUCCESS);
  CHECK(Storage.Commands == 3 && Bus.TimeUs - Start < 100000);

  Storage.CheckConditions = 1;
  Storage.SenseKey = 6;
  Storage.SenseAsc = EFI_SCSI_ASC_RESET;
  CHECK(UsbBootExecCmdWithRetry(&Mass, Cmd, sizeof(Cmd), EfiUsbDataIn,
    Data, sizeof(Data), 1000000) == EFI_SUCCESS);
  CHECK(Storage.Commands == 6 && Mass.BlockIoMedia.MediaId == 0);

  Storage.CheckConditions = 1;
  Storage.SenseAsc = 0x28;
  CHECK(UsbBootExecCmdWithRetry(&Mass, Cmd, sizeof(Cmd), EfiUsbDataIn,
    Data, sizeof(Data), 1000000) == EFI_SUCCESS);
  CHECK(Storage.Commands == 9 && Mass.BlockIoMedia.MediaId == 1);

  Storage.CheckConditions = 3;
  CHECK(UsbBootExecCmdWithRetry(&Mass, Cmd, sizeof(Cmd), EfiUsbDataIn,
    Data, sizeof(Data), 1000000) == EFI_NOT_READY);
  CHECK(Storage.Commands == 13 && Storage.CheckConditions == 1);
  FinishStorage(&F);
}
#endif

int main(void) {
  const char *Probe = getenv("T70_STORAGE_PROBE");
  setbuf(stdout, NULL);
  if (!Probe || !strcmp(Probe, "success")) { SuccessPaths(); RootPortTransitions(); }
  if (!Probe || !strcmp(Probe, "tt")) { TtRecovery(); }
  if (!Probe || !strcmp(Probe, "fault")) { FaultPaths(); }
  if (!Probe || !strcmp(Probe, "residue")) { ResiduePaths(); }
  if (!Probe || !strcmp(Probe, "recovery")) { RecoveryPaths(); }
  if (!Probe || !strcmp(Probe, "reset-clear")) { ResetClearFailures(); }
  if (!Probe || !strcmp(Probe, "class")) { BootClassFaultPaths(); }
#ifdef T70PRO_RAM_DIAGNOSTICS
  if (!Probe || !strcmp(Probe, "media-probe")) { MediaProbeReadiness(); SlowMediaProbe(); }
  if (!Probe || !strcmp(Probe, "hub-reset")) { DeferredHubPortReset(); }
  if (!Probe || !strcmp(Probe, "storage-retry")) { StorageBindRetry(); }
#endif
  printf("MusbStoragePathTest: %u checks passed\n", Checks);
  return 0;
}
