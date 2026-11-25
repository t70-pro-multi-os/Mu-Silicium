// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#include <spbcx.h>
#include <reshub.h>
#include <acpiioct.h>
#include <Library/MtkI2cCore.h>
#define HALF 65536U
#define TAG 'I07T'
#define IRQ_RESULT 0x1fU
#define I2C6_BASE 0x1101a000ULL
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY D0Entry;
EVT_WDF_DEVICE_D0_EXIT D0Exit;
EVT_SPB_TARGET_CONNECT TargetConnect;
EVT_SPB_CONTROLLER_READ IoRead;
EVT_SPB_CONTROLLER_WRITE IoWrite;
EVT_SPB_CONTROLLER_SEQUENCE IoSequence;
EVT_WDF_REQUEST_CANCEL RequestCancel;
EVT_WDF_INTERRUPT_ISR InterruptIsr;
EVT_WDF_INTERRUPT_ENABLE InterruptEnable;
EVT_WDF_INTERRUPT_DISABLE InterruptDisable;
typedef struct {
  WDFDEVICE Device;
  PUCHAR Registers, DmaRegisters;
  WDFDMAENABLER Enabler;
  WDFCOMMONBUFFER Buffer;
  WDFWAITLOCK Lock;
  WDFINTERRUPT Interrupt;
  KEVENT TransferEvent;
  volatile LONG IrqOnline;
  MI_CONTROLLER Core;
  SPBREQUEST ActiveRequest;
  BOOLEAN Active, Pinned, I2c6;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT,DeviceContext)
typedef struct { ULONG Address, Speed; } TARGET_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TARGET_CONTEXT,TargetContext)
static VOID CopyAcpiIntegerBytes(PVOID Destination,const __unaligned UCHAR *Source,ULONG Bytes) {
  UCHAR *Output=(UCHAR *)Destination;
  ULONG Index;
  for (Index=0;Index<Bytes;Index++) Output[Index]=Source[Index];
}
static BOOLEAN FirmwareInteger32(PACPI_METHOD_ARGUMENT Argument,PULONG Value) {
  ULONGLONG Wide;
  if (!Argument || !Value || Argument->Type!=ACPI_METHOD_ARGUMENT_INTEGER) return FALSE;
  if (Argument->DataLength==sizeof(ULONG)) {
    CopyAcpiIntegerBytes(Value,Argument->Data,sizeof(*Value)); return TRUE;
  }
  if (Argument->DataLength!=sizeof(ULONGLONG)) return FALSE;
  CopyAcpiIntegerBytes(&Wide,Argument->Data,sizeof(Wide));
  if (Wide>MAXULONG) return FALSE;
  *Value=(ULONG)Wide; return TRUE;
}
typedef struct { volatile LONG Cancelled; KEVENT CancelDone; PRKEVENT Wake; } REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQUEST_CONTEXT,RequestContext)
#pragma pack(push,1)
typedef struct { PNP_SERIAL_BUS_DESCRIPTOR Bus; ULONG Speed; USHORT Address; } I2C_DESCRIPTOR;
#pragma pack(pop)
/* The WDF passive interrupt lock serializes mask/status access with the ISR.
 * Never hold it across a sleep, reset delay or the transfer ownership lock. */
static MI_U32 ReadRegister(void *P,unsigned Region,unsigned Offset,unsigned Width) {
  DEVICE_CONTEXT *D=P; PUCHAR R=(Region?D->DmaRegisters:D->Registers)+Offset;
  BOOLEAN Irq=D->Interrupt && !Region && (Offset==8 || Offset==12); MI_U32 V;
  if (Irq) WdfInterruptAcquireLock(D->Interrupt);
  if (Width==1) V=READ_REGISTER_UCHAR(R);
  else if (Width==2) V=READ_REGISTER_USHORT((PUSHORT)R);
  else V=READ_REGISTER_ULONG((PULONG)R);
  if (Irq) WdfInterruptReleaseLock(D->Interrupt);
  return V;
}
static void WriteRegister(void *P,unsigned Region,unsigned Offset,unsigned Width,MI_U32 Value) {
  DEVICE_CONTEXT *D=P; PUCHAR R=(Region?D->DmaRegisters:D->Registers)+Offset;
  BOOLEAN Irq=D->Interrupt && !Region && (Offset==8 || Offset==12);
  if (Irq) WdfInterruptAcquireLock(D->Interrupt);
  if (!Region && Offset==8 && !D->IrqOnline) Value=0;
  if (Width==1) WRITE_REGISTER_UCHAR(R,(UCHAR)Value);
  else if (Width==2) WRITE_REGISTER_USHORT((PUSHORT)R,(USHORT)Value);
  else WRITE_REGISTER_ULONG((PULONG)R,Value);
  if (Irq) WdfInterruptReleaseLock(D->Interrupt);
}
static MI_U64 NowUs(void *P) { ULONGLONG T; UNREFERENCED_PARAMETER(P); return KeQueryInterruptTimePrecise(&T)/10; }
static void DelayUs(void *P,unsigned Us) { UNREFERENCED_PARAMETER(P); KeStallExecutionProcessor(Us); }
static void Fence(void *P) { UNREFERENCED_PARAMETER(P); KeMemoryBarrier(); }
static int IsCancelled(void *P) {
  DEVICE_CONTEXT *D=P;
  return !InterlockedCompareExchange(&D->IrqOnline,0,0) ||
    (D->ActiveRequest && InterlockedCompareExchange(&RequestContext(D->ActiveRequest)->Cancelled,0,0)!=0);
}
static void WaitForEvent(void *P,unsigned MaximumUs) {
  DEVICE_CONTEXT *D=P; LARGE_INTEGER Timeout;
  if (!MaximumUs) return;
  if (MaximumUs>5000) MaximumUs=5000;
  Timeout.QuadPart=-(LONGLONG)MaximumUs*10;
  KeWaitForSingleObject(&D->TransferEvent,Executive,KernelMode,FALSE,&Timeout);
}
BOOLEAN InterruptIsr(WDFINTERRUPT Interrupt,ULONG MessageId) {
  DEVICE_CONTEXT *D=DeviceContext(WdfInterruptGetDevice(Interrupt)); ULONG Flags,Mask;
  UNREFERENCED_PARAMETER(MessageId);
  /* WDF holds the passive interrupt lock. Leave W1C status for the owner to
   * decode; masking prevents a level-triggered storm before that thread runs. */
  if (!D->IrqOnline || !D->Registers) return FALSE;
  Mask=READ_REGISTER_ULONG((PULONG)(D->Registers+8));
  Flags=READ_REGISTER_ULONG((PULONG)(D->Registers+12));
  if (!(Flags&Mask&IRQ_RESULT)) return FALSE;
  WRITE_REGISTER_ULONG((PULONG)(D->Registers+8),0);
  KeSetEvent(&D->TransferEvent,IO_NO_INCREMENT,FALSE); return TRUE;
}
NTSTATUS InterruptEnable(WDFINTERRUPT Interrupt,WDFDEVICE Device) {
  DEVICE_CONTEXT *D=DeviceContext(Device); UNREFERENCED_PARAMETER(Interrupt);
  if (!D->Active || !D->Registers) return STATUS_DEVICE_NOT_READY;
  WRITE_REGISTER_ULONG((PULONG)(D->Registers+8),0);
  InterlockedExchange(&D->IrqOnline,1); return STATUS_SUCCESS;
}
NTSTATUS InterruptDisable(WDFINTERRUPT Interrupt,WDFDEVICE Device) {
  DEVICE_CONTEXT *D=DeviceContext(Device); UNREFERENCED_PARAMETER(Interrupt);
  InterlockedExchange(&D->IrqOnline,0);
  if (D->Registers && D->Core.Io.Read) WRITE_REGISTER_ULONG((PULONG)(D->Registers+8),0);
  KeSetEvent(&D->TransferEvent,IO_NO_INCREMENT,FALSE); return STATUS_SUCCESS;
}
static void PinDma(DEVICE_CONTEXT *D) {
  if (!D->Pinned && D->Buffer && D->Enabler) {
    WdfObjectReference(D->Buffer); WdfObjectReference(D->Enabler); D->Pinned=TRUE;
  }
}
static NTSTATUS Convert(MI_STATUS S) {
  switch(S) {
  case MiOk:return STATUS_SUCCESS;
  case MiInvalid:return STATUS_INVALID_PARAMETER;
  case MiUnsupported:return STATUS_NOT_SUPPORTED;
  case MiNack:return STATUS_NO_SUCH_DEVICE;
  case MiArbitration:return STATUS_DEVICE_BUSY;
  case MiTimeout:return STATUS_IO_TIMEOUT;
  case MiCancelled:return STATUS_CANCELLED;
  default:return STATUS_IO_DEVICE_ERROR;
  }
}
static NTSTATUS FirmwareInteger(DEVICE_CONTEXT *D,const char Name[4],PULONG Value) {
  ACPI_EVAL_INPUT_BUFFER In={0};
  UCHAR Storage[sizeof(ACPI_EVAL_OUTPUT_BUFFER)+sizeof(ULONGLONG)]={0};
  PACPI_EVAL_OUTPUT_BUFFER Out=(PACPI_EVAL_OUTPUT_BUFFER)Storage;
  WDF_MEMORY_DESCRIPTOR Input,Output; WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Returned=0; NTSTATUS S;
  In.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE; RtlCopyMemory(In.MethodName,Name,4);
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,&In,sizeof(In));
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output,Storage,sizeof(Storage));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_SEC(2));
  S=WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(D->Device),NULL,
         IOCTL_ACPI_EVAL_METHOD,&Input,&Output,&Options,&Returned);
  if (!NT_SUCCESS(S)) return S;
  if (Returned<sizeof(ACPI_EVAL_OUTPUT_BUFFER) || Returned>sizeof(Storage) || Out->Signature!=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE ||
      Out->Count!=1 || Out->Length>Returned || Out->Length<sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
      Out->Length<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      Returned<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      !FirmwareInteger32(&Out->Argument[0],Value))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  return STATUS_SUCCESS;
}
NTSTATUS TargetConnect(WDFDEVICE Device,SPBTARGET Target) {
  static const char I2c6Path[]="\\_SB.I2C6";
  static const char I2c6PaddedPath[]="\\_SB_.I2C6";
  SPB_CONNECTION_PARAMETERS P; PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Properties;
  I2C_DESCRIPTOR *I; TARGET_CONTEXT *T=TargetContext(Target); ULONG Bytes,SourceLength,J; const UCHAR *Source;
  SPB_CONNECTION_PARAMETERS_INIT(&P); SpbTargetGetConnectionParameters(Target,&P);
  Properties=(PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER)P.ConnectionParameters;
  if (!Properties || Properties->Version!=RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION ||
      Properties->PropertiesLength<sizeof(I2C_DESCRIPTOR)) return STATUS_INVALID_PARAMETER;
  I=(I2C_DESCRIPTOR *)Properties->ConnectionProperties;
  /* Controller-initiated, 7-bit I2C only. */
  if (I->Bus.Tag!=SERIAL_BUS_DESCRIPTOR || I->Bus.RevisionId!=SERIAL_BUS_DESCRIPTOR_REVISION_V2 ||
      I->Bus.ResourceSourceIndex!=0 || I->Bus.SerialBusType!=SERIAL_BUS_I2C_DESCRIPTOR_TYPE ||
      I->Bus.TypeSpecificFlags!=0 || (I->Bus.GeneralFlags!=2 && I->Bus.GeneralFlags!=6) ||
      I->Bus.TypeSpecificRevisionId!=SERIAL_BUS_DESCRIPTOR_REVISION || I->Bus.TypeDataLength!=6 ||
      I->Address<8 || I->Address>0x77 || I->Speed<10000 || I->Speed>400000)
    return STATUS_NOT_SUPPORTED;
  Bytes=(ULONG)I->Bus.Length+3;
  if (Bytes>Properties->PropertiesLength || Bytes<sizeof(*I)+2) return STATUS_INVALID_PARAMETER;
  Source=(const UCHAR *)(I+1); SourceLength=Bytes-sizeof(*I);
  if (!Source[0] || Source[SourceLength-1]) return STATUS_INVALID_PARAMETER;
  for (J=0;J<SourceLength-1;J++) if (!Source[J]) return STATUS_INVALID_PARAMETER;
  // Acpi.sys can share the TCPC and charger with their Windows client.
  // Keep every other target exclusive, including these addresses on other buses.
  if (I->Bus.GeneralFlags==6 &&
      (!DeviceContext(Device)->I2c6 ||
       !((SourceLength==sizeof(I2c6Path) && RtlEqualMemory(Source,I2c6Path,sizeof(I2c6Path))) ||
         (SourceLength==sizeof(I2c6PaddedPath) && RtlEqualMemory(Source,I2c6PaddedPath,sizeof(I2c6PaddedPath)))) ||
       !((I->Address==0x4e && I->Speed==400000) || (I->Address==0x6a && I->Speed==100000))))
    return STATUS_NOT_SUPPORTED;
  T->Address=I->Address; T->Speed=I->Speed; return STATUS_SUCCESS;
}
void RequestCancel(WDFREQUEST Request) {
  REQUEST_CONTEXT *R=RequestContext(Request);
  InterlockedExchange(&R->Cancelled,1);
  /* The transfer owner alone completes the request after DMA is quiescent.
   * It waits for this callback before releasing the request context. */
  if (R->Wake) KeSetEvent(R->Wake,IO_NO_INCREMENT,FALSE);
  KeSetEvent(&R->CancelDone,IO_NO_INCREMENT,FALSE);
}
static NTSTATUS CopyMdl(PMDL Mdl,PUCHAR Buffer,ULONG Length,BOOLEAN ToMdl) {
  ULONG Done=0;
  while (Mdl && Done<Length) {
    ULONG N=MmGetMdlByteCount(Mdl); PUCHAR P;
    if (N>Length-Done) N=Length-Done;
    P=MmGetSystemAddressForMdlSafe(Mdl,NormalPagePriority|MdlMappingNoExecute);
    if (!P) return STATUS_INSUFFICIENT_RESOURCES;
    if (ToMdl) RtlCopyMemory(P,Buffer+Done,N); else RtlCopyMemory(Buffer+Done,P,N);
    Done+=N; Mdl=Mdl->Next;
  }
  return Done==Length?STATUS_SUCCESS:STATUS_INVALID_BUFFER_SIZE;
}
static void Transfer(WDFDEVICE Device,SPBTARGET Target,SPBREQUEST Request,ULONG Count) {
  DEVICE_CONTEXT *D=DeviceContext(Device); TARGET_CONTEXT *T=TargetContext(Target);
  REQUEST_CONTEXT *R=RequestContext(Request); SPB_TRANSFER_DESCRIPTOR X[MI_MAX_MESSAGES];
  PMDL M[MI_MAX_MESSAGES]; MI_MESSAGE Messages[MI_MAX_MESSAGES];
  SPB_REQUEST_PARAMETERS Params; ULONG I,Tx=0,Rx=0,Total=0,Offsets[MI_MAX_MESSAGES],DoneBytes=0;
  unsigned Completed=0; MI_U32 Actual; PUCHAR B=NULL; BOOLEAN Generic=FALSE,CopyFailed=FALSE;
  PHYSICAL_ADDRESS A; NTSTATUS S=STATUS_NOT_SUPPORTED,Unmark; MI_STATUS Engine;
  SPB_REQUEST_PARAMETERS_INIT(&Params); SpbRequestGetParameters(Request,&Params);
  WdfRequestSetInformation((WDFREQUEST)Request,0);
  /* Separate locked requests still need a held-bus owner; a single atomic
   * request supports arbitrary direction changes without intervening STOP. */
  if ((Params.Position!=SpbRequestSequencePositionSingle && Params.Type!=SpbRequestTypeSequence) ||
      Count<1 || Count>MI_MAX_MESSAGES ||
      (Params.Type==SpbRequestTypeSequence && Count!=Params.SequenceTransferCount) ||
      (Params.Type!=SpbRequestTypeSequence && Count!=1)) goto Complete;
  for (I=0;I<Count;I++) {
    SPB_TRANSFER_DESCRIPTOR_INIT(&X[I]); M[I]=NULL;
    SpbRequestGetTransferParameters(Request,I,&X[I],&M[I]);
    if (!X[I].TransferLength || X[I].TransferLength>65535 || X[I].DelayInUs>1000000 ||
        (X[I].Direction!=SpbTransferDirectionToDevice && X[I].Direction!=SpbTransferDirectionFromDevice)) goto Complete;
    Offsets[I]=Total;
    if (X[I].TransferLength>HALF*2-Total) goto Complete;
    Total=((Total+(ULONG)X[I].TransferLength)+3U)&~3U;
    if (Total>HALF*2) goto Complete;
    if (X[I].DelayInUs) Generic=TRUE;
  }
  if (Count>2 || (Count==2 && (X[0].Direction!=SpbTransferDirectionToDevice ||
      X[1].Direction!=SpbTransferDirectionFromDevice))) Generic=TRUE;
  if (!Generic) {
    if (X[0].Direction==SpbTransferDirectionToDevice) Tx=(ULONG)X[0].TransferLength;
    else Rx=(ULONG)X[0].TransferLength;
    if (Count==2) Rx=(ULONG)X[1].TransferLength;
  }
  WdfWaitLockAcquire(D->Lock,NULL);
  if (!D->Active) { S=STATUS_DEVICE_NOT_READY; goto Unlock; }
  S=Convert(MiSetFrequency(&D->Core,T->Speed,&Actual)); if (!NT_SUCCESS(S)) goto Unlock;
  B=WdfCommonBufferGetAlignedVirtualAddress(D->Buffer); A=WdfCommonBufferGetAlignedLogicalAddress(D->Buffer);
  /* Map and validate every MDL and stage all writes before the first target
   * side effect. A malformed later message cannot truncate a hardware sequence. */
  for (I=0;I<Count;I++) {
    ULONG Length=(ULONG)X[I].TransferLength;
    ULONG Offset=Generic?Offsets[I]:(X[I].Direction==SpbTransferDirectionFromDevice?HALF:0);
    PMDL N=M[I]; ULONG Available=0;
    while (N && Available<Length) {
      ULONG Bytes=MmGetMdlByteCount(N);
      if (!MmGetSystemAddressForMdlSafe(N,NormalPagePriority|MdlMappingNoExecute)) { S=STATUS_INSUFFICIENT_RESOURCES; goto Unlock; }
      Available+=Bytes>Length-Available?Length-Available:Bytes; N=N->Next;
    }
    if (Available!=Length) { S=STATUS_INVALID_BUFFER_SIZE; goto Unlock; }
    if (X[I].Direction==SpbTransferDirectionToDevice) {
      S=CopyMdl(M[I],B+Offset,Length,FALSE); if (!NT_SUCCESS(S)) goto Unlock;
    } else RtlZeroMemory(B+Offset,Length);
    Messages[I].Buffer=B+Offset; Messages[I].Length=Length;
    Messages[I].Dma=A.QuadPart+Offset; Messages[I].DelayUs=X[I].DelayInUs;
    Messages[I].Read=X[I].Direction==SpbTransferDirectionFromDevice;
  }
  InterlockedExchange(&R->Cancelled,0); KeInitializeEvent(&R->CancelDone,NotificationEvent,FALSE);
  R->Wake=&D->TransferEvent; KeClearEvent(&D->TransferEvent);
  S=WdfRequestMarkCancelableEx((WDFREQUEST)Request,RequestCancel);
  if (!NT_SUCCESS(S)) { R->Wake=NULL; goto Unlock; }
  D->ActiveRequest=Request;
  if (Generic) Engine=MiSequence(&D->Core,T->Address,Messages,Count,&Completed);
  else {
    Engine=MiTransfer(&D->Core,T->Address,Tx?B:NULL,Tx,A.QuadPart,
                      Rx?B+HALF:NULL,Rx,A.QuadPart+HALF);
    if (Engine==MiOk) Completed=Count;
  }
  S=Convert(Engine); D->ActiveRequest=NULL;
  Unmark=WdfRequestUnmarkCancelable((WDFREQUEST)Request);
  if (Unmark==STATUS_CANCELLED) {
    KeWaitForSingleObject(&R->CancelDone,Executive,KernelMode,FALSE,NULL);
    S=STATUS_CANCELLED; Completed=0;
  }
  R->Wake=NULL;
  if (!D->Core.DmaSafe) {
    D->Active=FALSE; Completed=0;
    /* Do not free/reuse memory that a failed bus master might still own. */
    PinDma(D);
  } else if (S!=STATUS_CANCELLED) {
    if (Completed>Count) { S=STATUS_IO_DEVICE_ERROR; Completed=0; }
    for (I=0;I<Completed;I++) {
      if (Messages[I].Read) {
        NTSTATUS Copy=CopyMdl(M[I],Messages[I].Buffer,Messages[I].Length,TRUE);
        if (!NT_SUCCESS(Copy)) { S=Copy; CopyFailed=TRUE; break; }
      }
      DoneBytes+=Messages[I].Length;
    }
    /* SpbCx sequence requests report completed whole-message bytes on target
     * errors. Validation, cancellation and unrecoverable DMA failures keep their
     * failure status; read/write requests retain ordinary NTSTATUS semantics. */
    if (!CopyFailed && Params.Type==SpbRequestTypeSequence && Engine==MiNack && Completed>0)
      S=STATUS_SUCCESS;
    WdfRequestSetInformation((WDFREQUEST)Request,DoneBytes);
  }
Unlock:
  WdfWaitLockRelease(D->Lock);
Complete:
  SpbRequestComplete(Request,S);
}
void IoRead(WDFDEVICE D,SPBTARGET T,SPBREQUEST R,size_t N) { UNREFERENCED_PARAMETER(N); Transfer(D,T,R,1); }
void IoWrite(WDFDEVICE D,SPBTARGET T,SPBREQUEST R,size_t N) { UNREFERENCED_PARAMETER(N); Transfer(D,T,R,1); }
void IoSequence(WDFDEVICE D,SPBTARGET T,SPBREQUEST R,ULONG N) { Transfer(D,T,R,N); }
/* Validate the complete raw/translated contract before mapping anything.
 * Supported T70Pro controllers share the HID; APDMA/GSI must match the instance. */
static NTSTATUS ResourcesValid(WDFCMRESLIST Raw,WDFCMRESLIST Translated) {
  ULONG Pass,I; PCM_PARTIAL_RESOURCE_DESCRIPTOR First;
  ULONGLONG Base,Dma; ULONG Gsi;
  if (!Raw || !Translated || WdfCmResourceListGetCount(Raw)!=3 ||
      WdfCmResourceListGetCount(Translated)!=3) return STATUS_DEVICE_CONFIGURATION_ERROR;
  First=WdfCmResourceListGetDescriptor(Raw,0);
  if (!First || First->Type!=CmResourceTypeMemory) return STATUS_DEVICE_CONFIGURATION_ERROR;
  Base=First->u.Memory.Start.QuadPart;
  if (Base==0x11e00000) { Dma=0x10217080; Gsi=176; }
  else if (Base==0x11e01000) { Dma=0x10217100; Gsi=177; }
  else if (Base==I2C6_BASE) { Dma=0x10217480; Gsi=182; }
  else return STATUS_DEVICE_CONFIGURATION_ERROR;
  for (Pass=0;Pass<2;Pass++) for (I=0;I<3;I++) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Q=WdfCmResourceListGetDescriptor(Pass?Translated:Raw,I);
    if (!Q) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (I<2) {
      if (Q->Type!=CmResourceTypeMemory || Q->u.Memory.Start.QuadPart!=(I?Dma:Base) ||
          Q->u.Memory.Length!=(I?0x80:0x1000) ||
          (Q->Flags&CM_RESOURCE_MEMORY_WRITEABILITY_MASK)!=CM_RESOURCE_MEMORY_READ_WRITE)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    } else {
      if (Q->Type!=CmResourceTypeInterrupt ||
          (Q->ShareDisposition!=CmResourceShareDeviceExclusive && Q->ShareDisposition!=CmResourceShareDriverExclusive) ||
          (Q->Flags&(CM_RESOURCE_INTERRUPT_LATCHED|CM_RESOURCE_INTERRUPT_MESSAGE|CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT)) ||
          (!Pass && Q->u.Interrupt.Vector!=Gsi)) return STATUS_DEVICE_CONFIGURATION_ERROR;
      /* The translated vector/level/affinity are assigned by Windows. */
    }
  }
  return STATUS_SUCCESS;
}
static void ReleaseResources(DEVICE_CONTEXT *D) {
  PUCHAR Registers,DmaRegisters;
  /* PnP drained the ISR. The interrupt stays valid during this callback,
   * then WDF deletes it because it was created in PrepareHardware. */
  if (D->Interrupt) WdfInterruptAcquireLock(D->Interrupt);
  InterlockedExchange(&D->IrqOnline,0); Registers=D->Registers; DmaRegisters=D->DmaRegisters;
  D->Registers=D->DmaRegisters=NULL;
  D->I2c6=FALSE;
  if (D->Interrupt) WdfInterruptReleaseLock(D->Interrupt);
  D->Interrupt=NULL;
  if (Registers) MmUnmapIoSpace(Registers,0x1000);
  if (DmaRegisters) MmUnmapIoSpace(DmaRegisters,0x80);
  RtlZeroMemory(&D->Core,sizeof(D->Core));
  if (!D->Pinned) {
    if (D->Buffer) { WdfObjectDelete(D->Buffer); D->Buffer=NULL; }
    if (D->Enabler) { WdfObjectDelete(D->Enabler); D->Enabler=NULL; }
  }
}
NTSTATUS PrepareHardware(WDFDEVICE Device,WDFCMRESLIST Raw,WDFCMRESLIST Resources) {
  DEVICE_CONTEXT *D=DeviceContext(Device); NTSTATUS S;
  WDF_DMA_ENABLER_CONFIG Config; WDF_INTERRUPT_CONFIG Irq; PHYSICAL_ADDRESS Logical;
  if (D->Pinned) return STATUS_DEVICE_NOT_READY;
  if (D->Interrupt || D->Registers || D->DmaRegisters || D->Enabler || D->Buffer)
    return STATUS_INVALID_DEVICE_STATE;
  S=ResourcesValid(Raw,Resources); if (!NT_SUCCESS(S)) return S;
  KeInitializeEvent(&D->TransferEvent,SynchronizationEvent,FALSE);
  D->Registers=MmMapIoSpaceEx(WdfCmResourceListGetDescriptor(Resources,0)->u.Memory.Start,0x1000,PAGE_READWRITE|PAGE_NOCACHE);
  if (!D->Registers) { S=STATUS_INSUFFICIENT_RESOURCES; goto Failed; }
  D->DmaRegisters=MmMapIoSpaceEx(WdfCmResourceListGetDescriptor(Resources,1)->u.Memory.Start,0x80,PAGE_READWRITE|PAGE_NOCACHE);
  if (!D->DmaRegisters) { S=STATUS_INSUFFICIENT_RESOURCES; goto Failed; }
  WDF_DMA_ENABLER_CONFIG_INIT(&Config,WdfDmaProfileScatterGather64Duplex,HALF*2);
  Config.AddressWidthOverride=36;
  S=WdfDmaEnablerCreate(Device,&Config,WDF_NO_OBJECT_ATTRIBUTES,&D->Enabler); if (!NT_SUCCESS(S)) goto Failed;
  S=WdfCommonBufferCreate(D->Enabler,HALF*2,WDF_NO_OBJECT_ATTRIBUTES,&D->Buffer); if (!NT_SUCCESS(S)) goto Failed;
  Logical=WdfCommonBufferGetAlignedLogicalAddress(D->Buffer);
  if (Logical.QuadPart<=0 || (ULONGLONG)Logical.QuadPart+HALF*2>(1ULL<<36)) { S=STATUS_CONFLICTING_ADDRESSES; goto Failed; }
  /* Last fallible operation: a WDF interrupt cannot be manually deleted. */
  WDF_INTERRUPT_CONFIG_INIT(&Irq,InterruptIsr,NULL);
  Irq.PassiveHandling=TRUE; Irq.AutomaticSerialization=FALSE;
  Irq.InterruptRaw=WdfCmResourceListGetDescriptor(Raw,2);
  Irq.InterruptTranslated=WdfCmResourceListGetDescriptor(Resources,2);
  Irq.EvtInterruptEnable=InterruptEnable; Irq.EvtInterruptDisable=InterruptDisable;
  S=WdfInterruptCreate(Device,&Irq,WDF_NO_OBJECT_ATTRIBUTES,&D->Interrupt);
  if (!NT_SUCCESS(S)) goto Failed;
  D->I2c6=WdfCmResourceListGetDescriptor(Raw,0)->u.Memory.Start.QuadPart==I2C6_BASE;
  return STATUS_SUCCESS;
Failed:
  ReleaseResources(D); return S;
}
NTSTATUS D0Entry(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Previous) {
  DEVICE_CONTEXT *D=DeviceContext(Device); ULONG Clock,Ready; NTSTATUS S;
  MI_IO Io={D,ReadRegister,WriteRegister,NowUs,DelayUs,Fence,IsCancelled,WaitForEvent};
  UNREFERENCED_PARAMETER(Previous);
  /* IRRS checks current pin, parent clock and idle DMA state before it
   * restores this controller's wrapper clock. */
  if (D->Active) return STATUS_INVALID_DEVICE_STATE;
  if (D->Pinned || !D->Registers || !D->DmaRegisters || !D->Buffer || !D->Interrupt)
    return STATUS_DEVICE_NOT_READY;
  S=FirmwareInteger(D,"IRDY",&Ready); if (!NT_SUCCESS(S) || Ready!=1) return STATUS_DEVICE_NOT_READY;
  S=FirmwareInteger(D,"IRCK",&Clock); if (!NT_SUCCESS(S)) return S;
  if (!Clock && (!D->Core.Io.Read || (D->Core.DmaSafe && !D->Core.Ready))) {
    S=FirmwareInteger(D,"IRRS",&Clock);
    if (S==STATUS_OBJECT_NAME_NOT_FOUND) return STATUS_DEVICE_NOT_READY;
    if (!NT_SUCCESS(S)) return S;
  }
  if (Clock<1000000 || Clock>1000000000) return STATUS_DEVICE_NOT_READY;
  S=Convert(MiInitialize(&D->Core,&Io,Clock,400000));
  if (!D->Core.DmaSafe && D->Core.Io.Read) PinDma(D);
  if (NT_SUCCESS(S)) D->Active=TRUE;
  return S;
}
NTSTATUS D0Exit(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Target) {
  DEVICE_CONTEXT *D=DeviceContext(Device); NTSTATUS S=STATUS_SUCCESS;
  UNREFERENCED_PARAMETER(Target); WdfWaitLockAcquire(D->Lock,NULL);
  D->Active=FALSE; if (D->Core.Io.Read) S=Convert(MiStop(&D->Core));
  if (!NT_SUCCESS(S)) PinDma(D);
  WdfWaitLockRelease(D->Lock); return S;
}
NTSTATUS ReleaseHardware(WDFDEVICE Device,WDFCMRESLIST Resources) {
  DEVICE_CONTEXT *D=DeviceContext(Device); UNREFERENCED_PARAMETER(Resources);
  /* Hardware access ends at D0Exit; an unexpected active release retains DMA
   * allocations instead of trying to stop an inaccessible controller here. */
  if (D->Active || (D->Core.Io.Read && !D->Core.DmaSafe)) PinDma(D);
  D->Active=FALSE;
  ReleaseResources(D);
  return STATUS_SUCCESS;
}
NTSTATUS DeviceAdd(WDFDRIVER Driver,PWDFDEVICE_INIT Init) {
  WDF_OBJECT_ATTRIBUTES A; WDF_PNPPOWER_EVENT_CALLBACKS P; SPB_CONTROLLER_CONFIG C;
  WDFDEVICE Device; DEVICE_CONTEXT *D; NTSTATUS S;
  UNREFERENCED_PARAMETER(Driver);
  S=SpbDeviceInitConfig(Init); if (!NT_SUCCESS(S)) return S;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&P); P.EvtDevicePrepareHardware=PrepareHardware;
  P.EvtDeviceReleaseHardware=ReleaseHardware; P.EvtDeviceD0Entry=D0Entry; P.EvtDeviceD0Exit=D0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init,&P);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,DEVICE_CONTEXT);
  A.ExecutionLevel=WdfExecutionLevelPassive; A.SynchronizationScope=WdfSynchronizationScopeNone;
  S=WdfDeviceCreate(&Init,&A,&Device); if (!NT_SUCCESS(S)) return S;
  D=DeviceContext(Device); D->Device=Device;
  WDF_OBJECT_ATTRIBUTES_INIT(&A); A.ParentObject=Device;
  S=WdfWaitLockCreate(&A,&D->Lock); if (!NT_SUCCESS(S)) return S;
  SPB_CONTROLLER_CONFIG_INIT(&C); C.EvtSpbTargetConnect=TargetConnect;
  C.EvtSpbIoRead=IoRead; C.EvtSpbIoWrite=IoWrite; C.EvtSpbIoSequence=IoSequence;
  S=SpbDeviceInitialize(Device,&C); if (!NT_SUCCESS(S)) return S;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,TARGET_CONTEXT); SpbControllerSetTargetAttributes(Device,&A);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,REQUEST_CONTEXT); SpbControllerSetRequestAttributes(Device,&A);
  return STATUS_SUCCESS;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT Object,PUNICODE_STRING Path) {
  WDF_DRIVER_CONFIG C; WDF_DRIVER_CONFIG_INIT(&C,DeviceAdd);
  return WdfDriverCreate(Object,Path,WDF_NO_OBJECT_ATTRIBUTES,&C,WDF_NO_HANDLE);
}
