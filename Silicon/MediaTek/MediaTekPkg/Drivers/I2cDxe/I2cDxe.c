// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/MtkI2cController.h>
#include <Protocol/MtkI2cExclusive.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DmaLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MtkI2cCore.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/DxeServicesTableLib.h>
#define SIGN SIGNATURE_32('M','I','2','C')
#define HALF 65536U
#define PAGES EFI_SIZE_TO_PAGES(HALF*2)
typedef struct {
  UINT32 Signature;
  EFI_I2C_MASTER_PROTOCOL Master;
  MTK_I2C_EXCLUSIVE_PROTOCOL Exclusive;
  MTK_I2C_CONTROLLER *Board;
  MI_CONTROLLER Core;
  VOID *Buffer, *Mapping;
  PHYSICAL_ADDRESS Dma;
  EFI_EVENT Worker, Exit, Completion;
  EFI_EVENT_NOTIFY ExitObserver;
  VOID *ExitObserverContext;
  EFI_I2C_REQUEST_PACKET *Packet;
  EFI_STATUS *CompletionStatus;
  UINTN Address;
  UINT32 TimeoutUs;
  UINT64 TokenSequence, ActiveToken, ExitObserverToken;
  BOOLEAN Busy, Quiescing, Configured, ExitObserving;
  BOOLEAN LeaseActive, TokensExhausted;
  BOOLEAN ExitObserverRunning, ExitObserverNotified;
} I2C_CONTEXT;
#define CTX(p) (CR(p,I2C_CONTEXT,Master,SIGN))
#define EXCLUSIVE_CTX(p) (CR(p,I2C_CONTEXT,Exclusive,SIGN))
STATIC CONST EFI_I2C_CONTROLLER_CAPABILITIES mCapabilities={
  sizeof(EFI_I2C_CONTROLLER_CAPABILITIES),65535,65535,131070
};
STATIC EFI_STATUS Validate(UINTN Address,EFI_I2C_REQUEST_PACKET *P);
STATIC EFI_STATUS Execute(I2C_CONTEXT *C);
STATIC MI_U32 ReadRegister(VOID *P,unsigned Region,unsigned Offset,unsigned Width) {
  I2C_CONTEXT *C=P; UINTN A=(UINTN)(Region?C->Board->DmaBase:C->Board->ControllerBase)+Offset;
  if (Width==1) return MmioRead8(A); if (Width==2) return MmioRead16(A); return MmioRead32(A);
}
STATIC VOID WriteRegister(VOID *P,unsigned Region,unsigned Offset,unsigned Width,MI_U32 Value) {
  I2C_CONTEXT *C=P; UINTN A=(UINTN)(Region?C->Board->DmaBase:C->Board->ControllerBase)+Offset;
  if (Width==1) MmioWrite8(A,(UINT8)Value); else if (Width==2) MmioWrite16(A,(UINT16)Value); else MmioWrite32(A,Value);
}
STATIC MI_U64 NowUs(VOID *P) { return GetTimeInNanoSecond(GetPerformanceCounter())/1000; }
STATIC VOID DelayUs(VOID *P,unsigned Us) { MicroSecondDelay(Us); }
STATIC VOID Fence(VOID *P) { MemoryFence(); }
STATIC EFI_STATUS Convert(MI_STATUS S) {
  switch(S) {
  case MiOk:return EFI_SUCCESS;
  case MiInvalid:return EFI_INVALID_PARAMETER;
  case MiUnsupported:return EFI_UNSUPPORTED;
  case MiTimeout:return EFI_TIMEOUT;
  case MiCancelled:return EFI_ABORTED;
  default:return EFI_DEVICE_ERROR;
  }
}
STATIC EFI_STATUS Claim(I2C_CONTEXT *C, BOOLEAN ForTransfer) {
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY); EFI_STATUS S=EFI_SUCCESS;
  if (C->Quiescing||C->ExitObserving) S=EFI_NOT_READY;
  else if (C->LeaseActive) S=EFI_ACCESS_DENIED;
  else if (C->Busy) S=EFI_ALREADY_STARTED;
  else if (ForTransfer && (!C->Configured || !C->Core.Ready)) S=EFI_NOT_READY;
  else C->Busy=TRUE;
  gBS->RestoreTPL(T); return S;
}
STATIC VOID Release(I2C_CONTEXT *C) {
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY); C->Busy=FALSE; gBS->RestoreTPL(T);
}
STATIC EFI_STATUS EFIAPI AcquireExclusive(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 *Token) {
  I2C_CONTEXT *C; EFI_TPL T; EFI_STATUS S=EFI_SUCCESS;
  if(!This||!Token) return EFI_INVALID_PARAMETER;
  C=EXCLUSIVE_CTX(This); *Token=0; T=gBS->RaiseTPL(TPL_NOTIFY);
  if(C->Quiescing||(C->ExitObserving&&!C->ExitObserverRunning)) S=EFI_NOT_READY;
  else if(C->LeaseActive) S=EFI_ACCESS_DENIED;
  else if(C->Busy) S=EFI_ALREADY_STARTED;
  else if(!C->Configured||!C->Core.Ready) S=EFI_NOT_READY;
  else if(C->TokensExhausted||C->TokenSequence==MAX_UINT64) {
    C->TokensExhausted=TRUE; S=EFI_OUT_OF_RESOURCES;
  } else {
    C->TokenSequence++; C->ActiveToken=C->TokenSequence;
    C->LeaseActive=TRUE; *Token=C->ActiveToken;
    if(C->ExitObserving) C->ExitObserverToken=C->ActiveToken;
  }
  gBS->RestoreTPL(T); return S;
}
STATIC EFI_STATUS ClaimExclusive(I2C_CONTEXT *C,UINT64 Token) {
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY); EFI_STATUS S=EFI_SUCCESS;
  if(C->Quiescing||(C->ExitObserving&&!C->ExitObserverRunning)) S=EFI_NOT_READY;
  else if(Token==0) S=EFI_INVALID_PARAMETER;
  else if(C->ExitObserving&&C->ExitObserverToken!=Token) S=EFI_ACCESS_DENIED;
  else if(!C->LeaseActive||C->ActiveToken!=Token) S=EFI_ACCESS_DENIED;
  else if(C->Busy) S=EFI_ALREADY_STARTED;
  else if(!C->Configured||!C->Core.Ready) S=EFI_NOT_READY;
  else C->Busy=TRUE;
  gBS->RestoreTPL(T); return S;
}
STATIC EFI_STATUS EFIAPI TransferExclusive(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 Token,
  UINTN Address,EFI_I2C_REQUEST_PACKET *P,EFI_STATUS *I2cStatus) {
  I2C_CONTEXT *C; EFI_STATUS S;
  if(I2cStatus) *I2cStatus=EFI_NOT_READY;
  if(!This) return EFI_INVALID_PARAMETER;
  C=EXCLUSIVE_CTX(This);
  S=Validate(Address,P);
  if(EFI_ERROR(S)) { if(I2cStatus) *I2cStatus=S; return S; }
  S=ClaimExclusive(C,Token);
  if(EFI_ERROR(S)) { if(I2cStatus) *I2cStatus=S; return S; }
  C->Packet=P; C->Address=Address; C->Completion=NULL; C->CompletionStatus=NULL;
  C->TimeoutUs=0;
  S=Execute(C);
  if(I2cStatus) *I2cStatus=S;
  C->Packet=NULL; Release(C); return S;
}
STATIC EFI_STATUS EFIAPI TransferTimed(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 Token,
  UINTN Address,EFI_I2C_REQUEST_PACKET *P,UINT32 TimeoutUs) {
  I2C_CONTEXT *C; EFI_STATUS S;
  if (!This || !TimeoutUs || TimeoutUs>10000) return EFI_INVALID_PARAMETER;
  S=Validate(Address,P);
  if (EFI_ERROR(S)) return S;
  if (P->OperationCount>2 || (P->OperationCount==2 &&
      ((P->Operation[0].Flags&I2C_FLAG_READ) || !(P->Operation[1].Flags&I2C_FLAG_READ))))
    return EFI_UNSUPPORTED;
  C=EXCLUSIVE_CTX(This);
  S=ClaimExclusive(C,Token);
  if (EFI_ERROR(S)) return S;
  C->Packet=P; C->Address=Address; C->TimeoutUs=TimeoutUs;
  S=Execute(C);
  C->Packet=NULL; Release(C);
  return S;
}

STATIC EFI_STATUS EFIAPI ReleaseExclusive(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 Token) {
  I2C_CONTEXT *C; EFI_TPL T; EFI_STATUS S=EFI_SUCCESS;
  if(!This||Token==0) return EFI_INVALID_PARAMETER;
  C=EXCLUSIVE_CTX(This); T=gBS->RaiseTPL(TPL_NOTIFY);
  if(C->Quiescing||(C->ExitObserving&&!C->ExitObserverRunning)) S=EFI_NOT_READY;
  else if(C->ExitObserving&&C->ExitObserverToken!=Token) S=EFI_ACCESS_DENIED;
  else if(!C->LeaseActive||C->ActiveToken!=Token) S=EFI_ACCESS_DENIED;
  else if(C->Busy) S=EFI_ALREADY_STARTED;
  else {
    C->LeaseActive=FALSE; C->ActiveToken=0;
    if(C->ExitObserving) C->ExitObserverToken=0;
  }
  gBS->RestoreTPL(T); return S;
}
STATIC EFI_STATUS EFIAPI RegisterExitObserver(MTK_I2C_EXCLUSIVE_PROTOCOL *This,
  EFI_EVENT_NOTIFY Observer,VOID *Context) {
  I2C_CONTEXT *C; EFI_TPL T; EFI_STATUS S=EFI_SUCCESS;
  if(!This||!Observer) return EFI_INVALID_PARAMETER;
  C=EXCLUSIVE_CTX(This); T=gBS->RaiseTPL(TPL_NOTIFY);
  if(C->Quiescing||C->ExitObserving) S=EFI_NOT_READY;
  else if(C->ExitObserver) {
    if(C->ExitObserver==Observer&&C->ExitObserverContext==Context) S=EFI_ALREADY_STARTED;
    else S=EFI_ACCESS_DENIED;
  } else if(C->LeaseActive) S=EFI_ACCESS_DENIED;
  else if(C->Busy) S=EFI_ALREADY_STARTED;
  else { C->ExitObserver=Observer; C->ExitObserverContext=Context; }
  gBS->RestoreTPL(T); return S;
}
STATIC EFI_STATUS EFIAPI UnregisterExitObserver(MTK_I2C_EXCLUSIVE_PROTOCOL *This,
  EFI_EVENT_NOTIFY Observer,VOID *Context) {
  I2C_CONTEXT *C; EFI_TPL T; EFI_STATUS S=EFI_SUCCESS;
  if(!This||!Observer) return EFI_INVALID_PARAMETER;
  C=EXCLUSIVE_CTX(This); T=gBS->RaiseTPL(TPL_NOTIFY);
  if(!C->ExitObserver) S=EFI_NOT_FOUND;
  else if(C->ExitObserver!=Observer||C->ExitObserverContext!=Context) S=EFI_ACCESS_DENIED;
  else if(C->Quiescing||C->ExitObserving) S=EFI_NOT_READY;
  else if(C->LeaseActive) S=EFI_ACCESS_DENIED;
  else if(C->Busy) S=EFI_ALREADY_STARTED;
  else { C->ExitObserver=NULL; C->ExitObserverContext=NULL; }
  gBS->RestoreTPL(T); return S;
}
STATIC EFI_STATUS EFIAPI Frequency(CONST EFI_I2C_MASTER_PROTOCOL *This,UINTN *Hz) {
  I2C_CONTEXT *C; EFI_STATUS S; MI_U32 Actual;
  if (!This || !Hz || *Hz>MAX_UINT32) return EFI_INVALID_PARAMETER;
  C=CTX(This);
  S=Claim(C,FALSE); if (EFI_ERROR(S)) return S;
  S=Convert(MiSetFrequency(&C->Core,(MI_U32)*Hz,&Actual));
  if (!EFI_ERROR(S)) { *Hz=Actual; C->Configured=TRUE; }
  Release(C); return S;
}
STATIC EFI_STATUS EFIAPI Reset(CONST EFI_I2C_MASTER_PROTOCOL *This) {
  I2C_CONTEXT *C; EFI_STATUS S; MI_IO Io;
  if (!This) return EFI_INVALID_PARAMETER;
  C=CTX(This);
  S=Claim(C,FALSE); if (EFI_ERROR(S)) return S;
  Io=C->Core.Io;
  S=Convert(MiInitialize(&C->Core,&Io,C->Board->SourceClockHz,C->Core.RequestedHz));
  C->Configured=FALSE; Release(C); return S;
}
STATIC EFI_STATUS Validate(UINTN Address,EFI_I2C_REQUEST_PACKET *P) {
  UINTN I,Total=0;
  if (!P) return EFI_INVALID_PARAMETER;
  if (Address<8 || Address>0x77) return EFI_NOT_FOUND;
  if (P->OperationCount<1 || P->OperationCount>MI_MAX_MESSAGES) return EFI_UNSUPPORTED;
  for (I=0;I<P->OperationCount;I++) {
    if (P->Operation[I].Flags&~I2C_FLAG_READ) return EFI_UNSUPPORTED;
    if (!P->Operation[I].LengthInBytes || !P->Operation[I].Buffer) return EFI_INVALID_PARAMETER;
    if (P->Operation[I].LengthInBytes>65535 || P->Operation[I].LengthInBytes>HALF*2-Total) return EFI_BAD_BUFFER_SIZE;
    Total=(Total+P->Operation[I].LengthInBytes+3)&~(UINTN)3;
    if (Total>HALF*2) return EFI_BAD_BUFFER_SIZE;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Execute(I2C_CONTEXT *C) {
  EFI_I2C_REQUEST_PACKET *P=C->Packet;
  UINT32 Tx=0,Rx=0; UINT8 *B=C->Buffer; EFI_STATUS S;
  if (P->OperationCount>2 || (P->OperationCount==2 &&
      ((P->Operation[0].Flags&I2C_FLAG_READ) || !(P->Operation[1].Flags&I2C_FLAG_READ)))) {
    MI_MESSAGE M[MI_MAX_MESSAGES]; unsigned Done=0; UINTN I,Offset=0;
    for (I=0;I<P->OperationCount;I++) {
      M[I].Buffer=B+Offset; M[I].Length=P->Operation[I].LengthInBytes;
      M[I].Read=(P->Operation[I].Flags&I2C_FLAG_READ)!=0;
      M[I].Dma=C->Dma+Offset; M[I].DelayUs=0;
      if (M[I].Read) ZeroMem(M[I].Buffer,M[I].Length);
      else CopyMem(M[I].Buffer,P->Operation[I].Buffer,M[I].Length);
      Offset=(Offset+M[I].Length+3)&~(UINTN)3;
    }
    S=Convert(MiSequence(&C->Core,(unsigned)C->Address,M,(unsigned)P->OperationCount,&Done));
    if (!EFI_ERROR(S)) {
      if (Done!=P->OperationCount) return EFI_DEVICE_ERROR;
      for (I=0;I<P->OperationCount;I++) if (M[I].Read) CopyMem(P->Operation[I].Buffer,M[I].Buffer,M[I].Length);
    }
    return S;
  }
  if (!(P->Operation[0].Flags&I2C_FLAG_READ)) Tx=P->Operation[0].LengthInBytes;
  if (P->Operation[P->OperationCount-1].Flags&I2C_FLAG_READ) Rx=P->Operation[P->OperationCount-1].LengthInBytes;
  if (Tx) CopyMem(B,P->Operation[0].Buffer,Tx);
  if (Rx) ZeroMem(B+HALF,Rx);
  if (C->TimeoutUs)
    S=Convert(MiTransferTimed(&C->Core,(unsigned)C->Address,Tx?B:NULL,Tx,C->Dma,
                              Rx?B+HALF:NULL,Rx,C->Dma+HALF,C->TimeoutUs));
  else
    S=Convert(MiTransfer(&C->Core,(unsigned)C->Address,Tx?B:NULL,Tx,C->Dma,
                         Rx?B+HALF:NULL,Rx,C->Dma+HALF));
  if (!EFI_ERROR(S) && Rx) CopyMem(P->Operation[P->OperationCount-1].Buffer,B+HALF,Rx);
  return S;
}
STATIC VOID EFIAPI Worker(EFI_EVENT Event,VOID *Context) {
  I2C_CONTEXT *C=Context; EFI_EVENT Completion; EFI_STATUS S;
  if (!C->Busy || !C->Packet) return;
  Completion=C->Completion; S=C->Quiescing?EFI_ABORTED:Execute(C);
  if (C->CompletionStatus) *C->CompletionStatus=S;
  C->Packet=NULL; C->Completion=NULL; C->CompletionStatus=NULL; Release(C);
  if (Completion) gBS->SignalEvent(Completion);
}
STATIC EFI_STATUS EFIAPI StartRequest(CONST EFI_I2C_MASTER_PROTOCOL *This,UINTN Address,
  EFI_I2C_REQUEST_PACKET *P,EFI_EVENT Event,EFI_STATUS *I2cStatus) {
  I2C_CONTEXT *C; EFI_STATUS S;
  if (!This) return EFI_INVALID_PARAMETER;
  S=Validate(Address,P);
  if (EFI_ERROR(S)) return S;
  C=CTX(This);
  S=Claim(C,TRUE); if (EFI_ERROR(S)) return S;
  C->Packet=P; C->Address=Address; C->Completion=Event; C->CompletionStatus=I2cStatus;
  C->TimeoutUs=0;
  if (Event) {
    if (I2cStatus) *I2cStatus=EFI_NOT_READY;
    S=gBS->SetTimer(C->Worker,TimerRelative,1);
    if (EFI_ERROR(S)) { C->Packet=NULL; C->Completion=NULL; C->CompletionStatus=NULL; Release(C); }
    return S;
  }
  S=Execute(C); if (I2cStatus) *I2cStatus=S;
  C->Packet=NULL; C->CompletionStatus=NULL; Release(C); return S;
}
STATIC VOID EFIAPI ExitBootServices(EFI_EVENT Event,VOID *Context) {
  I2C_CONTEXT *C=Context; EFI_TPL T; EFI_EVENT_NOTIFY Observer; VOID *ObserverContext;
  EFI_STATUS Status;
  T=gBS->RaiseTPL(TPL_NOTIFY);
  if(C->ExitObserverNotified||C->ExitObserving||C->Quiescing) {
    gBS->RestoreTPL(T); return;
  }
  C->ExitObserving=TRUE; C->ExitObserverNotified=TRUE;
  if (C->Worker) (VOID)gBS->SetTimer(C->Worker,TimerCancel,0);
  if (C->Busy && C->CompletionStatus) *C->CompletionStatus=EFI_ABORTED;
  C->Busy=FALSE; C->Packet=NULL; C->Completion=NULL; C->CompletionStatus=NULL;
  C->LeaseActive=FALSE; C->ActiveToken=0;
  Observer=C->ExitObserver; ObserverContext=C->ExitObserverContext;
  C->ExitObserverRunning=(Observer!=NULL);
  gBS->RestoreTPL(T);

  // Let the sole board observer sample power while the controller is usable.
  // Public I2C is blocked during this window; only its callback-scoped lease works.
  if(Observer) Observer(Event,ObserverContext);

  T=gBS->RaiseTPL(TPL_NOTIFY);
  C->ExitObserverRunning=FALSE; C->ExitObserverToken=0;
  C->Quiescing=TRUE; C->Configured=FALSE;
  C->LeaseActive=FALSE; C->ActiveToken=0;
  gBS->RestoreTPL(T);
  // DMA memory is RuntimeServicesData, retained even after a failed stop. Never
  // change the memory map here. A hardware failure is recorded, not ignored.
  Status=C->Core.Io.Read?Convert(MiStop(&C->Core)):EFI_NOT_READY;
  if (C->Board && C->Board->Revision==MTK_I2C_CONTROLLER_REVISION) {
    C->Board->DriverStartPhase=MTK_I2C_DRIVER_HANDOFF;
    C->Board->DriverStartStatus=Status;
  }
  if (EFI_ERROR(Status)) DEBUG((DEBUG_ERROR,"T70Pro I2C DMA failed to stop at OS handoff\n"));
}
// Keep context and DMA storage resident while any callback can still run.
// A failed unbind stays quiescent and can be retried through the same protocol.
STATIC EFI_STATUS CloseEvents(I2C_CONTEXT *C) {
  EFI_STATUS S,Result=EFI_SUCCESS;
  C->Quiescing=TRUE; C->Configured=FALSE;
  if (C->Worker) {
    (VOID)gBS->SetTimer(C->Worker,TimerCancel,0);
    S=gBS->CloseEvent(C->Worker);
    if (EFI_ERROR(S)) Result=S; else C->Worker=NULL;
  }
  if (C->Exit) {
    S=gBS->CloseEvent(C->Exit);
    if (EFI_ERROR(S)) Result=S; else C->Exit=NULL;
  }
  return Result;
}
STATIC EFI_STATUS BoardValid(MTK_I2C_CONTROLLER *B) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D; EFI_STATUS S; UINTN I;
  if (!B || B->Revision!=MTK_I2C_CONTROLLER_REVISION || B->Flags!=MTK_I2C_REQUIRED_FLAGS ||
      B->SourceClockHz<1000000 || B->SourceClockHz>1000000000 ||
      B->IommuBase || B->DmaId ||
      B->ControllerSize<0x1000 || B->DmaSize<0x80 ||
      (B->ControllerBase&0xfff) || (B->DmaBase&0x7f)) return EFI_UNSUPPORTED;
  for (I=0;I<2;I++) {
    EFI_PHYSICAL_ADDRESS Base=I?B->DmaBase:B->ControllerBase; UINT64 Size=I?0x80:0x1000;
    S=gDS->GetMemorySpaceDescriptor(Base,&D);
    if (EFI_ERROR(S) || D.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo ||
        Base<D.BaseAddress || Base-D.BaseAddress>D.Length || Size>D.Length-(Base-D.BaseAddress) ||
        !(D.Attributes&EFI_MEMORY_UC)||
        (D.Attributes&(EFI_MEMORY_RO|EFI_MEMORY_RP|EFI_MEMORY_WP))) return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}
STATIC VOID ReportStart(MTK_I2C_CONTROLLER *B,UINT32 Phase,EFI_STATUS Status) {
  if(B&&B->Revision==MTK_I2C_CONTROLLER_REVISION) {
    B->DriverStartPhase=Phase;B->DriverStartStatus=Status;
  }
}
STATIC EFI_STATUS EFIAPI Supported(EFI_DRIVER_BINDING_PROTOCOL *This,EFI_HANDLE Handle,EFI_DEVICE_PATH_PROTOCOL *Path) {
  MTK_I2C_CONTROLLER *B; EFI_STATUS S;
  S=gBS->OpenProtocol(Handle,&gMediaTekI2cControllerProtocolGuid,(VOID **)&B,This->DriverBindingHandle,Handle,EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR(S)) return S;
  S=BoardValid(B);
  if(B&&B->Revision==MTK_I2C_CONTROLLER_REVISION&&B->DriverStartPhase<0x10U)ReportStart(B,1U,S);
  gBS->CloseProtocol(Handle,&gMediaTekI2cControllerProtocolGuid,This->DriverBindingHandle,Handle);return S;
}
STATIC EFI_STATUS EFIAPI Start(EFI_DRIVER_BINDING_PROTOCOL *This,EFI_HANDLE Handle,EFI_DEVICE_PATH_PROTOCOL *Path) {
  I2C_CONTEXT *C; MTK_I2C_CONTROLLER *B; EFI_STATUS S; UINTN Bytes=HALF*2;
  MI_IO Io;
  S=gBS->OpenProtocol(Handle,&gMediaTekI2cControllerProtocolGuid,(VOID **)&B,This->DriverBindingHandle,Handle,EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR(S)) return S;
  C=AllocateZeroPool(sizeof(*C));
  ReportStart(B,0x11U,C?EFI_SUCCESS:EFI_OUT_OF_RESOURCES);
  if(!C){S=EFI_OUT_OF_RESOURCES;goto Close;}
  C->Signature=SIGN; C->Board=B;
  S=BoardValid(B);ReportStart(B,0x12U,S);if(EFI_ERROR(S))goto Free;
  S=DmaAllocateBuffer(EfiRuntimeServicesData,PAGES,&C->Buffer);
  ReportStart(B,0x13U,S);if(EFI_ERROR(S))goto Free;
  S=DmaMap(MapOperationBusMasterCommonBuffer,C->Buffer,&Bytes,B->IommuBase,B->DmaId,&C->Dma,&C->Mapping);
  ReportStart(B,0x14U,S);if(EFI_ERROR(S))goto Free;
  if(Bytes!=HALF*2||!C->Dma||C->Dma>=(1ULL<<36)||HALF*2>(1ULL<<36)-C->Dma) {
    S=EFI_UNSUPPORTED;ReportStart(B,0x15U,S);goto Free;
  }
  S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Worker,C,&C->Worker);
  ReportStart(B,0x16U,S);if(EFI_ERROR(S))goto Free;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBootServices,C,&gEfiEventExitBootServicesGuid,&C->Exit);
  ReportStart(B,0x17U,S);if(EFI_ERROR(S))goto Free;
  Io=(MI_IO){C,ReadRegister,WriteRegister,NowUs,DelayUs,Fence,NULL,NULL};
  S=Convert(MiInitialize(&C->Core,&Io,B->SourceClockHz,400000));
  ReportStart(B,0x18U,S);if(EFI_ERROR(S))goto Free;
  C->Configured=TRUE;
  C->Master=(EFI_I2C_MASTER_PROTOCOL){Frequency,Reset,StartRequest,&mCapabilities};
  C->Exclusive=(MTK_I2C_EXCLUSIVE_PROTOCOL){
    MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION,AcquireExclusive,TransferExclusive,ReleaseExclusive,
    RegisterExitObserver,UnregisterExitObserver,TransferTimed};
  S=gBS->InstallMultipleProtocolInterfaces(&Handle,
     &gEfiI2cMasterProtocolGuid,&C->Master,
     &gMediaTekI2cExclusiveProtocolGuid,&C->Exclusive,NULL);
  ReportStart(B,EFI_ERROR(S)?0x19U:0x1aU,S);
  if(!EFI_ERROR(S))return S;
Free:
  if (C->Core.Io.Read && MiStop(&C->Core)!=MiOk) {
    // Retain the allocation and exit event while a bus master may own memory.
    C->Quiescing=TRUE; goto Close;
  }
  if (EFI_ERROR(CloseEvents(C))) goto Close;
  if (C->Mapping) DmaUnmap(C->Mapping);
  if (C->Buffer) DmaFreeBuffer(PAGES,C->Buffer);
  FreePool(C);
Close:
  gBS->CloseProtocol(Handle,&gMediaTekI2cControllerProtocolGuid,This->DriverBindingHandle,Handle); return S;
}
STATIC EFI_STATUS EFIAPI Stop(EFI_DRIVER_BINDING_PROTOCOL *This,EFI_HANDLE Handle,UINTN Children,EFI_HANDLE *ChildHandles) {
  EFI_I2C_MASTER_PROTOCOL *Master; I2C_CONTEXT *C; EFI_STATUS S; EFI_TPL T;
  if (Children) return EFI_UNSUPPORTED;
  S=gBS->HandleProtocol(Handle,&gEfiI2cMasterProtocolGuid,(VOID **)&Master); if (EFI_ERROR(S)) return S;
  C=CTX(Master); T=gBS->RaiseTPL(TPL_NOTIFY);
  if(C->Busy||C->LeaseActive||C->ExitObserver||C->ExitObserverRunning||C->ExitObserving) {
    gBS->RestoreTPL(T); return EFI_NOT_READY;
  }
  C->Quiescing=TRUE; C->Configured=FALSE; gBS->RestoreTPL(T);
  S=Convert(MiStop(&C->Core)); if (EFI_ERROR(S)) return S;
  S=CloseEvents(C); if (EFI_ERROR(S)) return S;
  S=gBS->UninstallMultipleProtocolInterfaces(Handle,
     &gEfiI2cMasterProtocolGuid,Master,
     &gMediaTekI2cExclusiveProtocolGuid,&C->Exclusive,NULL);
  if (EFI_ERROR(S)) return S;
  DmaUnmap(C->Mapping); DmaFreeBuffer(PAGES,C->Buffer);
  gBS->CloseProtocol(Handle,&gMediaTekI2cControllerProtocolGuid,This->DriverBindingHandle,Handle);
  FreePool(C); return EFI_SUCCESS;
}
STATIC EFI_DRIVER_BINDING_PROTOCOL mBinding={Supported,Start,Stop,0x10,NULL,NULL};
EFI_STATUS EFIAPI I2cEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  return EfiLibInstallDriverBindingComponentName2(ImageHandle,SystemTable,&mBinding,ImageHandle,NULL,NULL);
}
