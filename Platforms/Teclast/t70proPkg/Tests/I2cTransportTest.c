// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual DXE adapter and core; only the controller and I2C target are modeled.
#include "MediaTekTest.h"
#include <Library/MtkI2cCore.h>
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/I2cDxe/I2cDxe.c"
EFI_GUID gMediaTekI2cControllerProtocolGuid={1},gEfiI2cMasterProtocolGuid={2},
         gMediaTekI2cExclusiveProtocolGuid={4},gEfiEventExitBootServicesGuid={3};
static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS=&DxeServices;
#define BASE 0x1101a000U
#define DMA  0x11000300U
static UINT32 Registers[0x1000/4],DmaRegisters[0x80/4];
static UINT8 Target[256],TxFifo[16],RxFifo[16];
static unsigned TxCount,RxCount,RxOffset,Starts,AppliedWrites,Fault,IgnoreWrite;
static unsigned Resets,Pending,CompletionUs,Frames;
static UINT64 CompleteAt;
static UINT8 FrameBytes[3];
static UINT32 FrameControl,FrameAddress,FrameLength,FrameAux,FrameCount;
static VOID Complete(VOID) {
  if(!Pending || TimeNs<CompleteAt || Fault==2U)return;
  Pending=0;Registers[0x24/4]&=~1U;
  Registers[0x0c/4]|=Fault==1U?3U:1U;
  if(Fault)return;
  if(FrameControl&0x10U) {
    CHECK(TxCount==1U&&FrameLength==1U&&FrameCount==2U&&FrameAux<=2U);
    CHECK(FrameAddress==0x9cU&&FrameControl==0x3aU);
    RxCount=FrameAux;RxOffset=0;
    for(unsigned I=0;I<RxCount;I++)RxFifo[I]=Target[(UINT8)(TxFifo[0]+I)];
  } else {
    CHECK(FrameAddress==0x9cU&&FrameControl==0x28U);
    CHECK(FrameLength==2U&&FrameCount==1U&&TxCount==2U);
    if(!IgnoreWrite) {Target[TxFifo[0]]=TxFifo[1];AppliedWrites++;}
  }
}
VOID EFIAPI MemoryFence(VOID) {}
UINT8 EFIAPI MmioRead8(UINTN A) {
  CHECK(A==BASE);Complete();CHECK(RxOffset<RxCount);return RxFifo[RxOffset++];
}
UINT16 EFIAPI MmioRead16(UINTN A) {(void)A;CHECK(0);return 0;}
UINT32 EFIAPI MmioRead32(UINTN A) {
  Complete();
  if(A>=DMA&&A<DMA+0x80U) {
    UINTN I=(A-DMA)/4;
    if(I==3U)DmaRegisters[I]&=~1U;
    return DmaRegisters[I];
  }
  CHECK(A>BASE&&A<BASE+0x1000U&&!(A&3U));return Registers[(A-BASE)/4];
}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V) {
  CHECK(A==BASE&&TxCount<sizeof(TxFifo));TxFifo[TxCount++]=V;return V;
}
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V) {(void)A;CHECK(0);return V;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) {
  if(A>=DMA&&A<DMA+0x80U) {DmaRegisters[(A-DMA)/4]=V;return V;}
  CHECK(A>BASE&&A<BASE+0x1000U&&!(A&3U));UINTN O=A-BASE;
  if(O==0x0cU) {Registers[O/4]&=~V;return V;}
  Registers[O/4]=V;
  if(O==0x38U) {TxCount=0;RxCount=RxOffset=0;}
  if(O==0x50U&&V==0x21U) {Resets++;Pending=0;Registers[0x24/4]=0;}
  if(O==0x24U&&(V&1U)) {
    CHECK(!Pending);Starts++;Frames++;
    FrameControl=Registers[0x10/4];FrameAddress=Registers[0x94/4];
    FrameLength=Registers[0x14/4];FrameAux=Registers[0x44/4];FrameCount=Registers[0x18/4];
    CHECK(TxCount<=sizeof(FrameBytes));memcpy(FrameBytes,TxFifo,TxCount);
    CHECK(V==((FrameControl&0x10U)?1U:0x4001U));
    Pending=1;CompleteAt=TimeNs+(UINT64)CompletionUs*1000U;
  }
  return V;
}
VOID *EFIAPI AllocateZeroPool(UINTN N) {return calloc(1,N);}
EFI_STATUS EFIAPI DmaAllocateBuffer(EFI_MEMORY_TYPE T,UINTN P,VOID **B) {(void)T;(void)P;(void)B;CHECK(0);return EFI_UNSUPPORTED;}
EFI_STATUS EFIAPI DmaMap(DMA_MAP_OPERATION O,VOID *B,UINTN *N,UINT64 I,UINT32 Id,PHYSICAL_ADDRESS *A,VOID **M) {(void)O;(void)B;(void)N;(void)I;(void)Id;(void)A;(void)M;CHECK(0);return EFI_UNSUPPORTED;}
EFI_STATUS EFIAPI DmaUnmap(VOID *M) {(void)M;CHECK(0);return EFI_UNSUPPORTED;}
EFI_STATUS EFIAPI DmaFreeBuffer(UINTN P,VOID *B) {(void)P;(void)B;CHECK(0);return EFI_UNSUPPORTED;}
static EFI_TPL CurrentTpl=TPL_APPLICATION;
static EFI_TPL EFIAPI Raise(EFI_TPL T) {EFI_TPL Old=CurrentTpl;CHECK(T>=Old);CurrentTpl=T;return Old;}
static VOID EFIAPI Restore(EFI_TPL T) {CurrentTpl=T;}
static I2C_CONTEXT C;
static MTK_I2C_CONTROLLER Board={.ControllerBase=BASE,.DmaBase=DMA,.SourceClockHz=136000000};
static UINT8 Buffer[HALF*2];
static UINT64 Token;
static VOID Init(UINT8 Control) {
  memset(&C,0,sizeof(C));memset(Registers,0,sizeof(Registers));memset(DmaRegisters,0,sizeof(DmaRegisters));
  memset(Target,0,sizeof(Target));Target[0x1c]=Control;Target[0x1d]=0x10;Target[0x1e]=(Control&1U)?2U:0U;
  TxCount=RxCount=RxOffset=Starts=AppliedWrites=Fault=IgnoreWrite=Resets=Pending=Frames=0;
  TimeNs=0;CompletionUs=100;
  C.Signature=SIGN;C.Board=&Board;C.Buffer=Buffer;C.Dma=0x10000;
  C.Exclusive=(MTK_I2C_EXCLUSIVE_PROTOCOL){MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION,
    AcquireExclusive,TransferExclusive,ReleaseExclusive,RegisterExitObserver,UnregisterExitObserver};
  MI_IO Io={&C,ReadRegister,WriteRegister,NowUs,DelayUs,Fence,NULL,NULL};
  Registers[0x90/4]=0xa5a5U;Registers[0x0c/4]=0x200U; // Inherited timing/status.
  CHECK(MiInitialize(&C.Core,&Io,136000000,400000)==MiOk);C.Configured=TRUE;
  CHECK(Registers[0x90/4]==0U&&Registers[0x0c/4]==0U&&C.Core.Timing.ActualHz<=400000U);
  CHECK(C.Exclusive.Acquire(&C.Exclusive,&Token)==EFI_SUCCESS&&Token);
}
static EFI_STATUS Transfer(UINTN N,EFI_I2C_OPERATION *Ops) {
  struct {UINTN OperationCount;EFI_I2C_OPERATION Operation[2];} Request;
  EFI_STATUS Completion=EFI_NOT_READY;Request.OperationCount=N;
  memcpy(Request.Operation,Ops,N*sizeof(*Ops));
  EFI_STATUS S=C.Exclusive.Transfer(&C.Exclusive,Token,0x4e,(VOID *)&Request,&Completion);
  CHECK(S==Completion&&!C.Busy&&CurrentTpl==TPL_APPLICATION);return S;
}
static EFI_STATUS Read(UINT8 Reg,UINT8 *Out,UINTN N) {
  EFI_I2C_OPERATION Ops[]={{0,1,&Reg},{I2C_FLAG_READ,(UINT32)N,Out}};
  return Transfer(2,Ops);
}
static EFI_STATUS Write(UINT8 Reg,UINT8 Value) {
  UINT8 Pair[]={Reg,Value};EFI_I2C_OPERATION Op={0,sizeof(Pair),Pair};return Transfer(1,&Op);
}
int main(void) {
  Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;
  for(unsigned V=0;V<256U;V++) {
    UINT8 Live=0xffU;Init((UINT8)V);unsigned Before=Resets;
    CHECK(Read(0x1c,&Live,1)==EFI_SUCCESS&&Live==V);
    CHECK(Write(0x1c,(UINT8)(V&~1U))==EFI_SUCCESS);
    CHECK(FrameBytes[0]==0x1cU&&FrameBytes[1]==(V&~1U)&&AppliedWrites==1U);
    CHECK(Read(0x1c,&Live,1)==EFI_SUCCESS&&Live==(V&~1U));
    CHECK(Starts==3U&&Frames==3U&&Resets==Before);
    CHECK(C.Exclusive.Release(&C.Exclusive,Token)==EFI_SUCCESS);
  }
  UINT8 Live=0;Init(1U);IgnoreWrite=1;
  CHECK(Write(0x1c,0)==EFI_SUCCESS&&AppliedWrites==0U);
  CHECK(Read(0x1c,&Live,1)==EFI_SUCCESS&&Live==1U);
  CHECK(Live!=0U); // Transport completion alone must not confirm the target write.
  Init(1U);Fault=1;Registers[0x90/4]=0x5a5aU;
  CHECK(Write(0x1c,0)==EFI_DEVICE_ERROR&&AppliedWrites==0U&&Target[0x1c]==1U);
  CHECK(Registers[0x90/4]==0U);
  Fault=0;CHECK(Read(0x1c,&Live,1)==EFI_SUCCESS&&Live==1U);
  Init(1U);Fault=2;
  CHECK(Write(0x1c,0)==EFI_TIMEOUT&&AppliedWrites==0U&&Target[0x1c]==1U);
  CHECK(TimeNs<100000000ULL&&Resets==2U);
  Fault=0;CHECK(Read(0x1c,&Live,1)==EFI_SUCCESS&&Live==1U);
  Init(1U);Registers[0x0c/4]=0x201U;
  CHECK(Write(0x1c,0)==EFI_SUCCESS&&AppliedWrites==1U&&TimeNs>=110000U);
  CHECK(Registers[0x0c/4]==0U);
  Init(1U);UINT32 Actual=0;Registers[0x90/4]=0x1234U;
  CHECK(MiSetFrequency(&C.Core,400000,&Actual)==MiOk&&Actual<=400000U);
  CHECK(Registers[0x90/4]==0U);
  puts("I2C transport: actual DXE+core, effective board DT 400k default, inherited timing cleanup, 256 register values, ignored-write reproduction, NACK, timeout and stale completion PASS");
  printf("I2C transport checks: %u\n",Checks);return 0;
}
