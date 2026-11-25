// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Library/MtkI2cCore.h>
static unsigned Checks;
#define CHECK(x) do { Checks++; assert(x); } while(0)
typedef struct {
  unsigned Reg[0x1000], Dma[32], Writes, Starts, Status, FifoRead, FifoWrite;
  unsigned LastControl, LastAddress, LastTxLength, LastRxLength, LastCount, LastStart;
  unsigned SequenceMode, SequenceFlags[MI_MAX_MESSAGES], StartValues[MI_MAX_MESSAGES], ControlValues[MI_MAX_MESSAGES], Resets, AutoDma, CancelMessage;
  unsigned LastDmaCon, LastTxHigh, LastRxHigh;
  int Active, Stuck, WarmStuck, FrozenTimer, Cancel, CancelAfterStart;
  unsigned Waits, CompleteAfterWait, CancelAfterWait, MaxWait, Armed, LastMask, StopMasks;
  MI_U64 Time;
  /* Explicit fault model: completion can precede the final STOP. A controller
   * reset before STOP discards the target's pending register write. This tests
   * transport ordering, not a measured HUSB311 timing constant. */
  unsigned StopDelayUs, StopPending, StopAborts, StopStuck, StopCancelUs;
  unsigned TargetRegister, TargetValue, TargetWrites, FifoBytes[16];
  MI_U64 StopAt;
} MOCK;
static MI_U32 Read(void *P,unsigned Region,unsigned Off,unsigned Width) {
  MOCK *M=P; CHECK(Width==(Region?4:(Off?4:1)));
  if (Region) {
    if (Off==0x0c && !M->WarmStuck) M->Dma[Off/4]&=~1U;
    return M->Dma[Off/4];
  }
  if (!Off) return (MI_U8)(0xa0+M->FifoRead++);
  if (Off==0x24 && M->StopPending && !M->StopStuck && M->Time>=M->StopAt) {
    M->Reg[0x24/2]&=~1U; M->StopPending=0;
    M->TargetRegister=M->TargetValue; M->TargetWrites++;
  }
  if (Off==0x0c && M->Active) {
    if (M->SequenceMode) {
      unsigned Flags=M->SequenceFlags[M->Starts-1];
      if ((Flags&17) && M->AutoDma && !M->Stuck) M->Dma[0x08/4]=0;
      if (Flags&17) {
        M->Active=0;
        if (Flags&1) M->Reg[0x24/2]&=~1U;
      }
      return Flags;
    }
    if ((M->Status&1) && M->AutoDma && !M->Stuck) M->Dma[0x08/4]=0;
    if (M->Status&1) {
      M->Active=0;
      if (!M->StopPending) M->Reg[0x24/2]&=~1U;
    }
    return M->Status;
  }
  return M->Reg[Off/2];
}
static void Write(void *P,unsigned Region,unsigned Off,unsigned Width,MI_U32 V) {
  MOCK *M=P; M->Writes++; CHECK(Width==(Region?4:(Off?4:1)));
  if (Region) {
    M->Dma[Off/4]=V;
    if (Off==0x0c && (V==5 || V==6) && !M->Stuck) M->Dma[0x08/4]=0;
    return;
  }
  if (!Off) {
    if (M->FifoWrite<16) M->FifoBytes[M->FifoWrite]=V;
    M->FifoWrite++; return;
  }
  if ((Off==0x24 && !V) || (Off==0x50 && V==0x21)) {
    if (M->StopPending) { M->StopPending=0; M->StopAborts++; }
  }
  if (Off==0x0c) {
    M->Reg[Off/2]&=~V;
    if (M->SequenceMode && M->Starts && !M->Active && M->CancelMessage==M->Starts) M->Cancel=1;
    return;
  }
  if (Off==8) {
    if (V) { CHECK((V==15 || V==31) && !M->Active); M->Armed++; }
    else if (M->Active) M->StopMasks++;
  }
  M->Reg[Off/2]=V;
  if (Off==0x50 && V==0x21) M->Resets++;
  if (Off==0x24) {
    M->Active=(V&1)!=0;
    if (M->Active) {
      M->LastMask=M->Reg[8/2];
      M->LastStart=V;
      if (M->SequenceMode) { CHECK(M->Starts<MI_MAX_MESSAGES); M->StartValues[M->Starts]=V; M->ControlValues[M->Starts]=M->Reg[0x10/2]; }
      M->Starts++; M->LastControl=M->Reg[0x10/2]; M->LastAddress=M->Reg[0x94/2];
      M->LastTxLength=M->Reg[0x14/2]; M->LastRxLength=M->Reg[0x44/2];
      M->LastCount=M->Reg[0x18/2]; M->LastDmaCon=M->Dma[0x18/4];
      M->LastTxHigh=M->Dma[0x54/4]; M->LastRxHigh=M->Dma[0x58/4];
      if (M->StopDelayUs || M->StopStuck) {
        CHECK(M->LastAddress==0x9c && M->LastTxLength==2 &&
              M->FifoWrite==2 && M->FifoBytes[0]==0x1c);
        M->TargetValue=M->FifoBytes[1];
        M->StopPending=1; M->StopAt=M->Time+M->StopDelayUs;
      }
      if (M->CancelAfterStart) M->Cancel=1;
    }
  }
}
static MI_U64 Now(void *P) { return ((MOCK *)P)->Time; }
static void Delay(void *P,unsigned Us) { MOCK *M=P; if (!M->FrozenTimer) M->Time+=Us; }
static void Fence(void *P) { (void)P; }
static int Cancel(void *P) {
  MOCK *M=P;
  return M->Cancel || (M->StopCancelUs && M->StopPending &&
                      M->Time+M->StopDelayUs>=M->StopAt+M->StopCancelUs);
}
static void Wait(void *P,unsigned Us) {
  MOCK *M=P; CHECK(M->Active && (M->LastMask==15 || M->LastMask==31)); CHECK(Us && Us<=5000);
  M->Waits++; if (Us>M->MaxWait) M->MaxWait=Us;
  if (!M->FrozenTimer) M->Time+=Us;
  /* A real ISR masks but leaves raw status for the transfer owner. */
  if (M->CompleteAfterWait && M->Waits>=M->CompleteAfterWait) { M->Status=1; M->Reg[4]=0; }
  if (M->CancelAfterWait && M->Waits>=M->CancelAfterWait) M->Cancel=1;
}
static void Init(MOCK *M,MI_CONTROLLER *C) {
  MI_IO Io={M,Read,Write,Now,Delay,Fence,Cancel,NULL}; memset(M,0,sizeof(*M));
  memset(C,0,sizeof(*C)); M->Status=1; M->AutoDma=1;
  CHECK(MiInitialize(C,&Io,136000000,400000)==MiOk); CHECK(C->Ready&&C->DmaSafe);
}
int main(void) {
  MOCK M; MI_CONTROLLER C; MI_TIMING T; MI_U8 Tx[32]={0},Rx[32]={0}; unsigned W,I,J;
  const unsigned Clocks[]={1000000,13000000,26000000,136000000,273000000,1000000000};
  const unsigned Rates[]={10000,50000,100000,100001,200000,333333,400000};
  for (I=0;I<sizeof(Clocks)/sizeof(*Clocks);I++) for (J=0;J<sizeof(Rates)/sizeof(*Rates);J++) {
    MI_STATUS S=MiCalculateTiming(Clocks[I],Rates[J],&T);
    CHECK(S==MiOk || S==MiUnsupported);
    if (S==MiOk) {
      unsigned Div=(T.ClockDiv&255)+1, Sample=(T.Timing>>8)+1;
      unsigned H=T.Timing&63,L=T.LowTiming&63;
      MI_U64 Den=(MI_U64)Div*Sample*(H+L+2);
      CHECK((MI_U64)Clocks[I]<=(MI_U64)Rates[J]*Den);
      CHECK(T.ActualHz==Clocks[I]/Den&&T.ActualHz<=Rates[J]);
      CHECK(((T.LowTiming>>6)&7)==Sample-1);
      CHECK(T.Ext&1); CHECK(T.ClockDiv>>8==(T.ClockDiv&255));
      CHECK((MI_U64)(L+1)*Div*Sample*1000000000ULL>=
            (MI_U64)(Rates[J]<=100000?5200:1450)*Clocks[I]);
      CHECK((MI_U64)(T.Ext>>8)*Div*1000000000ULL>=
            (MI_U64)(Rates[J]<=100000?5200:750)*Clocks[I]);
    }
  }
  CHECK(MiCalculateTiming(0,400000,&T)==MiUnsupported);
  CHECK(MiCalculateTiming(26000000,1000000,&T)==MiUnsupported);
  CHECK(MiCalculateTiming(26000000,400000,0)==MiUnsupported);
  Init(&M,&C); M.Reg[0x0c/2]=2; /* stale NACK must be acknowledged */
  CHECK(MiTransfer(&C,0x48,Tx,1,0,Rx,8,0)==MiOk);
  CHECK(M.Starts==1&&M.LastAddress==0x90&&M.LastCount==2&&M.LastStart==1);
  CHECK(M.LastTxLength==1&&M.LastRxLength==8&&(M.LastControl&0x12)==0x12);
  CHECK(!(M.LastControl&0x304)&&M.FifoWrite==1&&M.FifoRead==8);
  CHECK(Rx[0]==0xa0&&Rx[7]==0xa7&&C.DmaSafe);
  Init(&M,&C); CHECK(MiTransfer(&C,0x6a,Tx,32,0xf12345678ULL,Rx,32,0xe12345678ULL)==MiOk);
  CHECK(M.LastDmaCon==0x214&&M.LastTxHigh==15&&M.LastRxHigh==14);
  CHECK((M.LastControl&0x304)==0x304&&C.DmaSafe&&!M.Dma[0x08/4]);
  Init(&M,&C); CHECK(MiTransfer(&C,0x48,0,0,0,Rx,32,0x1000)==MiOk);
  CHECK(M.LastAddress==0x91&&M.LastDmaCon==0x15&&M.LastCount==1);
  Init(&M,&C); W=M.Writes;
  CHECK(MiTransfer(&C,0x48,Tx,32,0xfffffffffULL,0,0,0)==MiInvalid);
  CHECK(MiTransfer(&C,0x48,Tx,32,0,0,0,0)==MiInvalid);
  CHECK(MiTransfer(&C,0,Tx,1,0,0,0,0)==MiInvalid);
  CHECK(MiTransfer(&C,0x78,Tx,1,0,0,0,0)==MiInvalid);
  CHECK(MiTransfer(&C,0x48,Tx,65536,0x1000,0,0,0)==MiInvalid);
  CHECK(MiTransfer(&C,0x48,0,1,0,0,0,0)==MiInvalid);
  CHECK(MiTransfer(&C,0x48,0,0,0,0,0,0)==MiInvalid); CHECK(M.Writes==W);
  M.Cancel=1; CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiCancelled); CHECK(M.Writes==W);
  Init(&M,&C); M.AutoDma=0; memset(Rx,0x55,sizeof(Rx));
  CHECK(MiTransfer(&C,0x48,0,0,0,Rx,32,0x1000)==MiDmaStuck);
  CHECK(C.DmaSafe && C.Ready && Rx[0]==0x55);
  Init(&M,&C); M.Status=3; CHECK(MiTransfer(&C,0x48,Tx,32,0x1000,0,0,0)==MiNack);
  CHECK(M.Starts==1&&C.Ready&&C.DmaSafe);
  Init(&M,&C); M.Status=9; CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiArbitration);
  Init(&M,&C); M.Status=0; CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiTimeout); CHECK(M.Time<60000);
  Init(&M,&C); M.Status=0;
  MI_U64 Before=M.Time;
  CHECK(MiTransferTimed(&C,0x48,Tx,1,0x1000,Rx,32,0x2000,5000)==MiTimeout);
  CHECK(M.Time-Before>=5000 && M.Time-Before<=6000 && C.Ready && C.DmaSafe);
  Init(&M,&C); M.Status=0; M.FrozenTimer=1;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiTimeout);
  Init(&M,&C); M.CancelAfterStart=1;
  CHECK(MiTransfer(&C,0x48,Tx,32,0x1000,0,0,0)==MiCancelled); CHECK(C.DmaSafe);
  Init(&M,&C); M.Stuck=1;
  CHECK(MiTransfer(&C,0x48,Tx,32,0x1000,0,0,0)==MiDmaStuck);
  CHECK(!C.Ready&&!C.DmaSafe); W=M.Writes;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiInvalid); CHECK(W==M.Writes);
  Init(&M,&C); W=M.Writes; I=123;
  CHECK(MiSetFrequency(&C,1000000,&I)==MiUnsupported); CHECK(W==M.Writes&&I==123);
  CHECK(MiSetFrequency(&C,100000,&I)==MiOk); CHECK(I<=100000&&I>0);
  CHECK(MiStop(&C)==MiOk&&!C.Ready&&C.DmaSafe);
  CHECK(MiInitialize(0,&C.Io,26000000,100000)==MiInvalid);
  /* Polling clients never enable interrupt masks. Event clients always decode
   * raw status, including completions observed without a delivered interrupt. */
  Init(&M,&C); CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiOk);
  CHECK(!M.Armed && !M.Waits && !M.LastMask && !M.Reg[4]);
  Init(&M,&C); C.Io.WaitForEvent=Wait;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,Rx,8,0)==MiOk);
  CHECK(M.Armed==1 && !M.Waits && M.LastMask==15 && M.StopMasks==0 && !M.Reg[4]);
  Init(&M,&C); C.Io.WaitForEvent=Wait; M.Status=0; M.CompleteAfterWait=2;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,Rx,8,0)==MiOk);
  CHECK(M.Waits==2 && M.MaxWait==5000 && C.DmaSafe && !M.Reg[4]);
  Init(&M,&C); C.Io.WaitForEvent=Wait; M.Status=0; M.CancelAfterWait=1; memset(Rx,0x55,sizeof(Rx));
  CHECK(MiTransfer(&C,0x48,Tx,32,0x1000,Rx,32,0x2000)==MiCancelled);
  CHECK(M.Waits==1 && M.Starts==1 && C.DmaSafe && !M.Reg[4] && Rx[0]==0x55);
  Init(&M,&C); C.Io.WaitForEvent=Wait; M.Status=0;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiTimeout);
  CHECK(M.Waits<=12 && M.Time<60000 && C.DmaSafe && !M.Reg[4]);
  Init(&M,&C); C.Io.WaitForEvent=Wait; M.Status=0; M.FrozenTimer=1;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiTimeout);
  CHECK(M.Waits<=13 && !M.Reg[4]);
  Init(&M,&C); C.Io.WaitForEvent=Wait; M.Status=9;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiArbitration);
  CHECK(!M.Waits && M.Starts==1 && !M.Reg[4]);
  Init(&M,&C); C.Io.WaitForEvent=Wait; M.Status=3;
  CHECK(MiTransfer(&C,0x48,Tx,1,0,0,0,0)==MiNack); CHECK(!M.Reg[4]);
  Init(&M,&C); C.Io.WaitForEvent=Wait; M.Stuck=1;
  CHECK(MiTransfer(&C,0x48,Tx,32,0x1000,0,0,0)==MiDmaStuck);
  CHECK(!C.Ready && !C.DmaSafe && !M.Reg[4]);
  {
    MI_MESSAGE Messages[MI_MAX_MESSAGES]; unsigned Done,K,Resets;
    for (K=0;K<8;K++) {
      Init(&M,&C); M.SequenceMode=1;
      for (I=0;I<3;I++) {
        Messages[I]=(MI_MESSAGE){Rx,1,0,0,(K>>I)&1U};
        M.SequenceFlags[I]=I<2?16:1;
      }
      Resets=M.Resets;
      CHECK(MiSequence(&C,0x48,Messages,3,&Done)==MiOk && Done==3);
      CHECK(M.Starts==3 && M.Resets==Resets && !C.SequenceHeld && C.Ready && C.DmaSafe);
      CHECK(M.StartValues[0]==0xc001 && M.StartValues[1]==0xc001 && M.StartValues[2]==0x4001);
      CHECK((M.ControlValues[0]&2) && (M.ControlValues[1]&2) && !(M.ControlValues[2]&2));
    }
    Init(&M,&C); M.SequenceMode=1; M.AutoDma=1; Resets=M.Resets;
    Messages[0]=(MI_MESSAGE){Tx,32,0,0x1000,0}; Messages[1]=(MI_MESSAGE){Rx,32,10,0x2000,1};
    M.SequenceFlags[0]=16; M.SequenceFlags[1]=1;
    CHECK(MiSequence(&C,0x48,Messages,2,&Done)==MiOk && Done==2 && C.DmaSafe && M.Resets==Resets);
    for (I=0;I<3;I++) Messages[I]=(MI_MESSAGE){Rx,1,0,0,I&1U};
    Init(&M,&C); M.SequenceMode=1; M.SequenceFlags[0]=16; M.SequenceFlags[1]=2; Resets=M.Resets;
    CHECK(MiSequence(&C,0x48,Messages,3,&Done)==MiNack && Done==1 && M.Starts==2);
    CHECK(!C.SequenceHeld && C.Ready && C.DmaSafe && M.Resets==Resets+1);
    Init(&M,&C); M.SequenceMode=1; M.SequenceFlags[0]=1;
    CHECK(MiSequence(&C,0x48,Messages,3,&Done)==MiBusError && !Done && M.Starts==1);
    Init(&M,&C); M.SequenceMode=1; M.SequenceFlags[0]=16; M.CancelMessage=1;
    CHECK(MiSequence(&C,0x48,Messages,3,&Done)==MiCancelled && Done==1 && M.Starts==1 && !C.SequenceHeld);
    Init(&M,&C); M.SequenceMode=1; M.AutoDma=0; M.SequenceFlags[0]=16; M.SequenceFlags[1]=1; Resets=M.Resets;
    Messages[0]=(MI_MESSAGE){Tx,32,0,0x1000,0}; Messages[1]=(MI_MESSAGE){Rx,1,0,0,1};
    CHECK(MiSequence(&C,0x48,Messages,2,&Done)==MiDmaStuck && !Done && M.Starts==1);
    CHECK(C.DmaSafe && C.Ready && !C.SequenceHeld && M.Resets==Resets+1);
    Init(&M,&C); M.SequenceMode=1; M.WarmStuck=1; M.SequenceFlags[0]=16;
    Messages[0]=(MI_MESSAGE){Tx,1,0,0,0}; Messages[1]=(MI_MESSAGE){Rx,1,0,0,1}; Resets=M.Resets;
    CHECK(MiSequence(&C,0x48,Messages,2,&Done)==MiTimeout && Done==1 && M.Starts==1);
    CHECK(C.DmaSafe && C.Ready && !C.SequenceHeld && M.Resets==Resets+1);
    Init(&M,&C); M.SequenceMode=1; M.FrozenTimer=1; M.SequenceFlags[0]=0; C.Io.WaitForEvent=Wait;
    Messages[0]=(MI_MESSAGE){Tx,1,0,0,0};
    CHECK(MiSequence(&C,0x48,Messages,1,&Done)==MiTimeout && !Done && M.Waits<=13);
    Init(&M,&C); W=M.Writes; Messages[0]=(MI_MESSAGE){Tx,1,0,0,0}; Messages[1]=Messages[0]; Messages[2]=Messages[0];
    Messages[2].Buffer=0;
    CHECK(MiSequence(&C,0x48,Messages,3,&Done)==MiInvalid && !Done && W==M.Writes);
    CHECK(MiSequence(&C,0x48,Messages,0,&Done)==MiInvalid && W==M.Writes);
    CHECK(MiSequence(&C,0x48,Messages,MI_MAX_MESSAGES+1,&Done)==MiInvalid && W==M.Writes);
    CHECK(MiSequence(&C,0x48,Messages,1,0)==MiInvalid && W==M.Writes);
    Messages[0].DelayUs=1000001;
    CHECK(MiSequence(&C,0x48,Messages,1,&Done)==MiInvalid && W==M.Writes);
    Messages[0]=(MI_MESSAGE){Tx,65535,0,0x1000,0}; C.Timing.ActualHz=10000;
    CHECK(MiSequence(&C,0x48,Messages,1,&Done)==MiUnsupported && W==M.Writes);
    Init(&M,&C); M.SequenceMode=1;
    for (I=0;I<MI_MAX_MESSAGES;I++) { Messages[I]=(MI_MESSAGE){Rx,1,0,0,I&1}; M.SequenceFlags[I]=I+1<MI_MAX_MESSAGES?16:1; }
    CHECK(MiSequence(&C,0x48,Messages,MI_MAX_MESSAGES,&Done)==MiOk && Done==MI_MAX_MESSAGES && M.Starts==MI_MAX_MESSAGES);
  }
  /* A completed IRQ does not authorize resetting a still-started controller.
   * Reproduce an ACKed 1c=00 write which is lost if cleanup cuts off STOP. */
  {
    const unsigned Delays[]={1,10,80,1000,9990,10000};
    MI_U8 Pair[2]={0x1c,0};
    for (I=0;I<sizeof(Delays)/sizeof(*Delays);I++) for (J=0;J<2;J++) {
      Init(&M,&C); M.StopDelayUs=Delays[I];
      M.TargetRegister=J?3:1; Pair[1]=(MI_U8)(M.TargetRegister&~1U);
      CHECK(MiTransfer(&C,0x4e,Pair,2,0,0,0,0)==MiOk);
      if (M.TargetRegister!=Pair[1])
        fprintf(stderr,"ACKed target write lost: delay=%u expected=%02x live=%02x aborts=%u\n",
                Delays[I],Pair[1],M.TargetRegister,M.StopAborts);
      CHECK(M.TargetRegister==Pair[1] && M.TargetWrites==1 && !M.StopAborts);
      CHECK(M.Starts==1 && M.Resets==1 && C.Ready && C.DmaSafe && !C.SequenceHeld);
    }
    Init(&M,&C); M.StopStuck=1; M.TargetRegister=1; Pair[1]=0;
    CHECK(MiTransfer(&C,0x4e,Pair,2,0,0,0,0)==MiTimeout);
    CHECK(M.TargetRegister==1 && !M.TargetWrites && M.StopAborts==1 &&
          C.Ready && C.DmaSafe && M.Time<=10030);
    Init(&M,&C); M.StopDelayUs=80; M.StopCancelUs=20; M.TargetRegister=1;
    CHECK(MiTransfer(&C,0x4e,Pair,2,0,0,0,0)==MiCancelled);
    CHECK(M.TargetRegister==1 && !M.TargetWrites && M.StopAborts==1 && C.DmaSafe);
    Init(&M,&C); M.StopStuck=1; M.FrozenTimer=1; M.TargetRegister=1;
    CHECK(MiTransfer(&C,0x4e,Pair,2,0,0,0,0)==MiTimeout);
    CHECK(M.TargetRegister==1 && !M.TargetWrites && M.StopAborts==1 && C.DmaSafe);
  }
  printf("I2C core: %u assertions passed\n",Checks); return 0;
}
