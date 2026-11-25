// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkBtifCore.h>
#ifdef STP_TEST_NATIVE
#include <stdio.h>
#endif
unsigned MtkBtifAssertionCount;
#define CHECK(X) do { ++MtkBtifAssertionCount; if (!(X)) return __LINE__; } while (0)
typedef struct {
  unsigned Reg,Width,Value;
} WRITE;
typedef struct {
  unsigned Words[28],Dma[2],Operations,Fail,Ignore,WriteCount,Barriers,ByteFail;
  unsigned SentCount,RxCount,RxOffset,RxKind,Bad,PendingWrite,Waits,WaitFail;
  unsigned char Sent[8192],Rx[8192];
  WRITE Writes[16384];
} FAKE;
static FAKE F;
static MTK_BTIF_CORE B;
static MTK_BTIF_IO Io;
static MTK_BTIF_SERVICE Result;
static unsigned char Rx[256],Wire[2005],Body[1999];
static int Equal(const unsigned char *A,const unsigned char *C,unsigned N) {
  unsigned I; for (I=0;I<N;I++) if (A[I]!=C[I]) return 0; return 1;
}
static int FakeRead(void *Context,unsigned Region,unsigned Offset,unsigned Width,unsigned *V) {
  FAKE *A=Context;
  if (A->PendingWrite) { ++A->Bad; return 1; }
  if (++A->Operations==A->Fail) return 1;
  if (Region==0 && Width==4 && !(Offset&3U) && Offset<sizeof(A->Words)) {
    if (Offset==8) *V=(A->RxOffset<A->RxCount?A->RxKind:0U)|((A->Words[1]&2U)?2U:0U);
    else if (Offset==0x14) *V=(A->Words[Offset/4]&~1U)|(A->RxOffset<A->RxCount);
    else *V=A->Words[Offset/4];
  } else if (Region==0 && Offset==0 && Width==1 && A->RxOffset<A->RxCount) *V=A->Rx[A->RxOffset++];
  else if (Region>=1 && Region<=2 && Offset==8 && Width==4) *V=A->Dma[Region-1];
  else { ++A->Bad; return 1; }
  return 0;
}
static int FakeWrite(void *Context,unsigned Region,unsigned Offset,unsigned Width,unsigned V) {
  FAKE *A=Context;
  int Failed=++A->Operations==A->Fail;
  if (A->PendingWrite) { ++A->Bad; return 1; }
  A->PendingWrite=1;
  if (Region || A->WriteCount>=16384) { ++A->Bad; return 1; }
  A->Writes[A->WriteCount++]=(WRITE){Offset,Width,V};
  if (Width==4 && !(Offset&3U) && Offset<sizeof(A->Words)) {
    if (A->WriteCount!=A->Ignore && Offset!=8) A->Words[Offset/4]=V;
  } else if (Width==1 && Offset==0 && V<=255 && A->SentCount<sizeof(A->Sent)) {
    A->Sent[A->SentCount++]=(unsigned char)V;
    if (A->SentCount==A->ByteFail) Failed=1;
  } else { ++A->Bad; return 1; }
  return Failed;
}
static void Barrier(void *Context) {
  FAKE *A=Context; ++A->Barriers; A->PendingWrite=0;
}
static int Fresh(void) {
  unsigned I;
  unsigned char *P=(unsigned char *)&F;
  for (I=0;I<sizeof(F);I++) P[I]=0;
  F.Words[3]=0x37; F.Words[0x6c/4]=0x80000002U; F.Words[0x60/4]=0x135a;
  F.Words[0x64/4]=0xa4; F.Words[0x4c/4]=0x80; F.Words[1]=0xa0; F.Words[0x14/4]=0x60; F.RxKind=4;
  Io=(MTK_BTIF_IO){&F,FakeRead,FakeWrite,Barrier};
  CHECK(MtkBtifInit(&B,&Io)==MtkBtifOk && !F.Operations && !B.Owned);
  return 0;
}
static int Start(void) {
  int R=Fresh(); if (R) return R;
  CHECK(MtkBtifStart(&B,8,1)==MtkBtifOk && B.Running && B.Owned && !B.Faulted);
  CHECK(F.WriteCount==13 && F.Barriers==13 && !F.Bad);
  return 0;
}
static int Baseline(void) {
  CHECK(F.Words[3]==0x37 && F.Words[0x6c/4]==0x80000002U && F.Words[0x60/4]==0x135a);
  CHECK(F.Words[0x4c/4]==0x80 && F.Words[1]==0xa0);
  CHECK(F.Words[0x64/4]==0xa4 && !B.WakeOwned);
  CHECK(!B.Owned && !B.Running && !B.Faulted && !F.Bad && F.Barriers>=F.WriteCount && !F.PendingWrite);
  return 0;
}
static unsigned Make(unsigned N);
static int Wait(void *Context,unsigned Minimum,unsigned Maximum) {
  FAKE *A=Context;
  if (Minimum!=128 || Maximum!=160 || A->PendingWrite || (A->Words[0x64/4]&1U)) {
    ++A->Bad; return 1;
  }
  ++A->Waits; return A->WaitFail!=0;
}
static int Wake(void) {
  unsigned I,N,Before,W;
  int R=Start(); if (R) return R;
  CHECK(MtkBtifRaiseWake(&B,0)==MtkBtifInvalid);
  F.Operations=0; CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifOk); N=F.Operations;
  CHECK(F.Waits==1 && F.Words[0x64/4]==0xa5 && B.WakeOwned && B.SavedWake==0xa4);
  CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifOk && B.SavedWake==0xa4 && F.Waits==2);
  CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  for (I=1;I<=N;I++) {
    R=Start(); if (R) return R; F.Operations=0; F.Fail=I;
    CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifIoError && B.Faulted && B.Owned);
    Before=F.Operations; F.Fail=0;
    CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifFaulted && F.Operations==Before);
    CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  }
  R=Start(); if (R) return R; F.WaitFail=1;
  CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifIoError && B.Faulted && B.WakeOwned && F.Words[0x64/4]==0xa4);
  CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  R=Start(); if (R) return R; F.Ignore=F.WriteCount+2;
  CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifIoError && B.Faulted && B.WakeOwned);
  F.Ignore=0; CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  R=Start(); if (R) return R; CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifOk);
  F.Operations=0; CHECK(MtkBtifStop(&B)==MtkBtifOk); N=F.Operations;
  for (I=1;I<=N;I++) {
    R=Start(); if (R) return R; CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifOk);
    F.Operations=0; F.Fail=I;
    CHECK(MtkBtifStop(&B)==MtkBtifIoError && B.Owned && B.Faulted && B.WakeOwned);
    F.Fail=0; CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  }
  R=Start(); if (R) return R;
  I=Make(1); CHECK(MtkBtifStpWrite(&B,Wire,I,&W)==MtkStpOk);
  Before=F.Operations;
  CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifBusy && F.Operations==Before && !F.Waits);
  return 0;
}
static int StartStop(void) {
  unsigned N,I,Before;
  int R=Fresh(); if (R) return R;
  CHECK(MtkBtifStart(&B,16,1)==MtkBtifInvalid && !F.Operations);
  CHECK(MtkBtifStart(&B,8,8)==MtkBtifInvalid && !F.Operations);
  for (I=0;I<7;I++) {
    R=Fresh(); if (R) return R;
    if (I<2) F.Dma[I]=1;
    if (I==2) F.Words[0x4c/4]|=1;
    if (I==3) F.Words[1]|=1;
    if (I==4) F.Words[1]|=2;
    if (I==5) F.Words[0x14/4]=0x20;
    if (I==6) { F.RxCount=1; F.Rx[0]=0x80; }
    CHECK(MtkBtifStart(&B,8,1)==MtkBtifBusy && !F.WriteCount && !B.Owned && !F.RxOffset);
  }
  R=Start(); if (R) return R;
  Before=F.Operations;
  CHECK(MtkBtifStart(&B,8,1)==MtkBtifBusy && F.Operations==Before);
  CHECK(MtkBtifStop(&B)==MtkBtifOk);
  R=Baseline(); if (R) return R;
  Before=F.Operations; CHECK(MtkBtifStop(&B)==MtkBtifOk && F.Operations==Before);
  R=Start(); if (R) return R; N=F.Operations;
  /* Every fallible operation in Start, including a write that took effect. */
  for (I=1;I<=N;I++) {
    R=Fresh(); if (R) return R;
    F.Fail=I; CHECK(MtkBtifStart(&B,8,1)==MtkBtifIoError && !B.Running);
    F.Fail=0;
    if (B.Owned) {
      CHECK(B.Faulted && MtkBtifStart(&B,8,1)==MtkBtifFaulted);
      CHECK(MtkBtifStop(&B)==MtkBtifOk);
      R=Baseline(); if (R) return R;
    } else CHECK(!B.Faulted && !F.WriteCount);
  }
  /* Ignored stable-register writes are detected when they change a value;
   * FCR is a write-only alias of IIR and cannot be checked this way. */
  for (I=1;I<=13;I++) {
    R=Fresh(); if (R) return R; F.Ignore=I;
    CHECK(MtkBtifStart(&B,8,1)==((I==1 || I==2 || I==7 || I==11 || I==13)?MtkBtifIoError:MtkBtifOk));
    F.Ignore=0; CHECK(MtkBtifStop(&B)==MtkBtifOk);
    R=Baseline(); if (R) return R;
  }
  R=Start(); if (R) return R; F.Operations=0;
  CHECK(MtkBtifStop(&B)==MtkBtifOk); N=F.Operations;
  for (I=1;I<=N;I++) {
    R=Start(); if (R) return R; F.Operations=0; F.Fail=I;
    CHECK(MtkBtifStop(&B)==MtkBtifIoError && B.Owned && B.Faulted);
    Before=F.Operations; CHECK(MtkBtifStart(&B,8,1)==MtkBtifFaulted && F.Operations==Before);
    F.Fail=0; CHECK(MtkBtifStop(&B)==MtkBtifOk);
    R=Baseline(); if (R) return R;
  }
  R=Start(); if (R) return R;
  F.Words[0x14/4]=0x20; Before=F.WriteCount;
  CHECK(MtkBtifStop(&B)==MtkBtifBusy && F.WriteCount==Before && B.Running);
  F.Words[0x14/4]=0x60; F.RxCount=1;
  CHECK(MtkBtifStop(&B)==MtkBtifBusy && F.WriteCount==Before && !F.RxOffset);
  return 0;
}
static unsigned Make(unsigned N) {
  unsigned I,W=0;
  for (I=0;I<N;I++) Body[I]=(unsigned char)(I*73U+N);
  if (MtkStpEncode(8,2,Body,N,Wire,sizeof(Wire),&W)!=MtkStpOk) return 0;
  return W;
}
static int Drain(unsigned N,unsigned Room) {
  unsigned Before=F.SentCount,Queued=B.Count,I=0;
  F.Words[0x14/4]=Room==16?0x60U:0x20U;
  while (B.Count) {
    unsigned Old=B.Count;
    CHECK(++I<=512);
    CHECK(MtkBtifService(&B,Rx,sizeof(Rx),&Result)==(Old>Room?MtkBtifMore:MtkBtifOk));
    CHECK(Result.Transmitted==(Old<Room?Old:Room) && !Result.Received && Result.Handled);
  }
  CHECK(F.SentCount-Before==Queued && Queued==N && !(F.Words[1]&2U));
  for (I=0;I<MTK_BTIF_TX_BYTES;I++) CHECK(!B.Tx[I]);
  return 0;
}
static int Transmit(void) {
  static const unsigned Lengths[]={1,2,7,8,9,10,15,16,17,255,256,257,1994,1995,1996};
  unsigned I,J,N,W,Before;
  int R;
  for (J=0;J<2;J++) for (I=0;I<sizeof(Lengths)/sizeof(*Lengths);I++) {
    R=Start(); if (R) return R; N=Make(Lengths[I]);
    CHECK(N==Lengths[I]+6 && MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk && W==N && B.Count==N);
    CHECK(!F.SentCount && (F.Words[1]&2U)); Before=F.Operations;
    CHECK(MtkBtifStop(&B)==MtkBtifBusy && F.Operations==Before);
    F.Words[0x14/4]=0;
    CHECK(MtkBtifService(&B,Rx,sizeof(Rx),&Result)==MtkBtifMore && !Result.Transmitted && B.Count==N);
    R=Drain(N,J?16:8); if (R) return R;
    CHECK(Equal(F.Sent,Wire,N)); F.Words[0x14/4]=0x60;
    CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  }
  R=Start(); if (R) return R; N=Make(1996);
  /* Two whole frames plus a small frame fit; a further one accepts no prefix. */
  for (I=0;I<3;I++) {
    CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk && W==N);
    CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk && W==N);
    Before=F.Operations;
    CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpFull && !W && F.Operations==Before && B.Count==2*N);
    F.SentCount=0; R=Drain(2*N,16); if (R) return R;
    CHECK(Equal(F.Sent,Wire,N) && Equal(F.Sent+N,Wire,N));
  }
  CHECK(B.Head!=0 && B.Head==B.Tail);
  Before=F.Operations;
  Wire[0]=0;
  CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpMalformed && !W && F.Operations==Before);
  Wire[0]=0x80;
  W=77;
  CHECK(MtkBtifStpWrite(&B,Wire,2003,&W)==MtkStpInvalid && !W && F.Operations==Before);
  CHECK(MtkBtifStpWrite(&B,Wire,N,(unsigned *)Wire)==MtkStpInvalid && F.Operations==Before);
  CHECK(MtkBtifService(&B,(unsigned char *)&Result,16,&Result)==MtkBtifInvalid && F.Operations==Before);
  CHECK(MtkBtifService(&B,B.Tx,16,&Result)==MtkBtifInvalid && F.Operations==Before);
  return 0;
}
static int Failures(void) {
  unsigned I,N,W,Before,Sent,Accepted;
  int R=Start(); if (R) return R; N=Make(26);
  CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk);
  F.Operations=0; CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifMore);
  Accepted=F.Operations;
  for (I=1;I<=Accepted;I++) {
    R=Start(); if (R) return R; CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk);
    F.Operations=0; F.Fail=I;
    CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifIoError && B.Faulted);
    CHECK(Result.Transmitted<=16 && B.Count==N-Result.Transmitted);
    Before=F.Operations; Sent=F.SentCount; F.Fail=0;
    CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifFaulted);
    CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpFaulted && !W);
    CHECK(MtkBtifStop(&B)==MtkBtifBusy && F.Operations==Before && F.SentCount==Sent);
  }
  R=Start(); if (R) return R; N=Make(1);
  F.Operations=0; CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk); Accepted=F.Operations;
  for (I=1;I<=Accepted;I++) {
    R=Start(); if (R) return R; F.Operations=0; F.Fail=I;
    CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpIoError && W==N && B.Count==N && B.Faulted);
    Before=F.Operations; CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifFaulted && F.Operations==Before);
  }
  return 0;
}
static int Receive(void) {
  static const unsigned Capacities[]={1,8,16,255,256};
  unsigned I,J,Off,Before,N;
  int R;
  for (J=0;J<2;J++) for (I=0;I<sizeof(Capacities)/sizeof(*Capacities);I++) {
    R=Start(); if (R) return R; F.RxCount=777; F.RxKind=J?0x40U:4U;
    for (N=0;N<F.RxCount;N++) F.Rx[N]=(unsigned char)(N*37U);
    Off=0;
    while (Off<F.RxCount) {
      N=F.RxCount-Off; if (N>Capacities[I]) N=Capacities[I];
      CHECK(MtkBtifService(&B,Rx,Capacities[I],&Result)==(Off+N<F.RxCount?MtkBtifMore:MtkBtifOk));
      CHECK(Result.Received==N && !Result.Transmitted && Result.Handled && Equal(Rx,F.Rx+Off,N));
      Off+=N; CHECK(F.RxOffset==Off);
    }
    CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifNotOurs && !Result.Received && !Result.Handled);
    CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  }
  R=Start(); if (R) return R; F.RxCount=4; F.Operations=0;
  CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifOk); N=F.Operations;
  for (I=1;I<=N;I++) {
    R=Start(); if (R) return R; F.RxCount=4; F.Operations=0; F.Fail=I;
    CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifIoError && B.Faulted && Result.Received<=4);
    Before=F.Operations; F.Fail=0;
    CHECK(MtkBtifService(&B,Rx,256,&Result)==MtkBtifFaulted && F.Operations==Before);
  }
  return 0;
}
static int Duplex(void) {
  unsigned N,W,I,Off=0;
  int R=Start(); if (R) return R;
  N=Make(32); CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk);
  F.RxCount=600;
  for (I=0;I<F.RxCount;I++) F.Rx[I]=(unsigned char)(I*37U);
  while (F.RxOffset<F.RxCount) {
    unsigned Old=B.Count;
    MTK_BTIF_STATUS S=MtkBtifService(&B,Rx,17,&Result);
    CHECK(S==MtkBtifMore || S==MtkBtifOk);
    CHECK(Result.Transmitted==(Old>16?16:Old));
    CHECK(Result.Received<=17 && Equal(Rx,F.Rx+Off,Result.Received)); Off+=Result.Received;
  }
  CHECK(Off==600 && !B.Count && F.SentCount==N && Equal(F.Sent,Wire,N));
  CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  return 0;
}
static MTK_STP_TRANSMITTER T;
static MTK_STP_DECODER Decoder;
static MTK_WMT_CONTROL Wmt;
static MTK_STP_QUEUE Gps;
static int Dispatch(void) {
  unsigned Offset=0,Used;
  MTK_STP_STATUS S;
  MTK_STP_FRAME Frame;
  while (Offset<Result.Received) {
    S=MtkStpReceive(&Decoder,Rx+Offset,Result.Received-Offset,&Used,&Frame);
    CHECK(Used && (S==MtkStpFrameReady || S==MtkStpNeedMore)); Offset+=Used;
    if (S==MtkStpFrameReady) {
      if (Frame.Channel==4) CHECK(MtkWmtControlEvent(&Wmt,&Frame)==MtkWmtOk);
      else CHECK(MtkStpQueuePush(&Gps,Gps.Epoch,&Frame)==MtkStpOk);
    }
  }
  return 0;
}
static int Integration(void) {
  unsigned I,W,N,Read;
  static const unsigned char Reply[]={0x80,0x40,5,0,2,6,1,0,0,0,0};
  int R=Start(); if (R) return R;
  CHECK(MtkBtifRaiseWake(&B,Wait)==MtkBtifOk && F.Waits==1);
  CHECK(MtkStpTransmitterInit(&T,8,&B,MtkBtifStpWrite)==MtkStpOk);
  CHECK(MtkStpDecoderInit(&Decoder,8)==MtkStpOk && MtkStpQueueInit(&Gps,2)==MtkStpOk);
  CHECK(MtkWmtControlInit(&Wmt,&T)==MtkWmtOk);
  CHECK(MtkWmtGpsSyncBegin(&Wmt,0,1)==MtkWmtUnsupported && !B.Count && !F.SentCount);
  for (N=0;N<3;N++) {
    /* Conditional sync on, readback, sync off all cross the real STP->BTIF
     * queue/IRQ path and return through split FIFO/decoder dispatch. */
    if (N==1) CHECK(MtkWmtRegisterBegin(&Wmt,0,0x80050078U,0,0x70000000U)==MtkWmtPending);
    else CHECK(MtkWmtGpsSyncBegin(&Wmt,0x80,N==0)==MtkWmtPending);
    CHECK(B.Count==26 && !Wmt.Confirmed);
    F.SentCount=0; R=Drain(26,8); if (R) return R;
    CHECK(F.Sent[5]==8 && F.Sent[8]==(N==1?2:1));
    CHECK(F.Sent[12]==0x78 && F.Sent[14]==5 && F.Sent[15]==0x80);
    CHECK(F.Sent[19]==(N==0?0x10:N==1?0:0x50) && F.Sent[23]==0x70);
    for (I=0;I<16;I++) Body[I]=0;
    Body[0]=2; Body[1]=8; Body[2]=(unsigned char)(N==1?12:4);
    Body[5]=0x5a; Body[6]=0xc3; Body[7]=0x19;
    if (N==1) { Body[8]=0x78; Body[10]=5; Body[11]=0x80; Body[15]=0x10; }
    CHECK(MtkStpEncode(8,4,Body,N==1?16:8,F.Rx,sizeof(F.Rx),&W)==MtkStpOk);
    F.RxCount=W; F.RxOffset=0;
    while (F.RxOffset<F.RxCount) {
      MTK_BTIF_STATUS S=MtkBtifService(&B,Rx,3,&Result);
      CHECK(S==(F.RxOffset<F.RxCount?MtkBtifMore:MtkBtifOk));
      R=Dispatch(); if (R) return R;
    }
    CHECK(Wmt.Confirmed && !Wmt.Pending && Wmt.RegisterValue==(N==1?0x10000000U:0));
  }
  F.SentCount=0; F.RxCount=F.RxOffset=0;
  CHECK(MtkWmtControlBegin(&Wmt,2,1)==MtkWmtPending && !F.SentCount && B.Count==12);
  R=Drain(12,8); if (R) return R;
  CHECK(F.Sent[4]==1 && F.Sent[5]==6 && F.Sent[8]==2 && F.Sent[9]==1 && !Wmt.Confirmed);
  F.RxCount=sizeof(Reply); for (I=0;I<sizeof(Reply);I++) F.Rx[I]=Reply[I];
  while (F.RxOffset<F.RxCount) {
    MTK_BTIF_STATUS S=MtkBtifService(&B,Rx,3,&Result);
    CHECK(S==(F.RxOffset<F.RxCount?MtkBtifMore:MtkBtifOk));
    R=Dispatch(); if (R) return R;
  }
  CHECK(Wmt.Confirmed && !Wmt.Pending);
  /* The direct FIFO backend carries the full 2002-byte MCU/STP request without
   * the legacy wrapper's 2000-byte software ceiling. */
  for (I=0;I<1992;I++) Body[I]=(unsigned char)(I*51U);
  CHECK(MtkWmtGpsMcuBegin(&Wmt,Body,1992,3)==MtkWmtPending && B.Count==2002);
  F.SentCount=0; R=Drain(2002,16); if (R) return R;
  CHECK(F.Sent[4]==1 && F.Sent[5]==0x32 && F.Sent[6]==0xc8 && F.Sent[7]==7 && Equal(F.Sent+8,Body,1992));
  N=Make(600); F.RxCount=F.RxOffset=0;
  /* A complete MCU reply followed immediately by an opaque GPS byte stream. */
  Body[0]=2; Body[1]=0x32; Body[2]=3; Body[3]=0; Body[4]=0; Body[5]=7; Body[6]=8;
  CHECK(MtkStpEncode(8,4,Body,7,F.Rx,sizeof(F.Rx),&W)==MtkStpOk);
  for (I=0;I<N;I++) { F.Rx[W+I]=Wire[I]; }
  F.RxCount=W+N;
  while (F.RxOffset<F.RxCount) {
    MTK_BTIF_STATUS S=MtkBtifService(&B,Rx,16,&Result);
    CHECK(S==MtkBtifMore || S==MtkBtifOk); R=Dispatch(); if (R) return R;
  }
  CHECK(Wmt.Confirmed && Wmt.ReplyLength==3 && Wmt.Reply[1]==7 && Gps.Count==600);
  CHECK(MtkStpQueueRead(&Gps,Gps.Epoch,Body,sizeof(Body),&Read)==MtkStpOk && Read==600 && Equal(Body,Wire+4,600));
  F.Words[0x14/4]=0x60;
  CHECK(MtkBtifStop(&B)==MtkBtifOk); R=Baseline(); if (R) return R;
  return 0;
}
static unsigned PressureCalls,PressureBytes;
static MTK_STP_STATUS PressureStatus;
static MTK_STP_STATUS PressureWrite(void *Context,const unsigned char *Data,
  unsigned Length,unsigned *Written) {
  (void)Context; (void)Data; (void)Length;
  ++PressureCalls; *Written=PressureBytes; return PressureStatus;
}
static int UncertainPressure(void) {
  unsigned I,Kind,Before;
  for (Kind=0;Kind<4;Kind++) for (I=0;I<4;I++) {
    CHECK(MtkStpTransmitterInit(&T,8,0,PressureWrite)==MtkStpOk);
    CHECK(MtkWmtControlInit(&Wmt,&T)==MtkWmtOk);
    /* Full with a prefix, ambiguous failure with zero/full acceptance, and
     * short successful writes must all remain sticky faults, never Busy. */
    PressureStatus=I==0?MtkStpFull:I==3?MtkStpOk:MtkStpIoError;
    PressureBytes=I==0?1:I==2?4096:0;
    Before=PressureCalls;
    if (Kind==0) CHECK(MtkStpTransmit(&T,2,Body,1)==MtkStpIoError);
    if (Kind==1) CHECK(MtkWmtControlBegin(&Wmt,2,1)==MtkWmtUncertain);
    if (Kind==2) CHECK(MtkWmtRegisterBegin(&Wmt,0,0x1234,0,0)==MtkWmtUncertain);
    if (Kind==3) CHECK(MtkWmtGpsMcuBegin(&Wmt,Body,1,1)==MtkWmtUncertain);
    CHECK(T.Faulted && PressureCalls==Before+1 && !Wmt.Pending);
    CHECK(MtkStpTransmit(&T,2,Body,1)==MtkStpFaulted && PressureCalls==Before+1);
    if (Kind) CHECK(Wmt.Faulted);
  }
  return 0;
}
/* Exercise backpressure through the actual STP/WMT -> BTIF queue, not a
 * backend that merely returns a synthetic Full status. */
static int Backpressure(void) {
  unsigned Kind,N,W,Before;
  int R;
  for (Kind=0;Kind<6;Kind++) {
    R=Start(); if (R) return R;
    CHECK(MtkStpTransmitterInit(&T,8,&B,MtkBtifStpWrite)==MtkStpOk);
    CHECK(MtkWmtControlInit(&Wmt,&T)==MtkWmtOk);
    N=Make(1996);
    CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk);
    CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk);
    N=Make(86); CHECK(MtkBtifStpWrite(&B,Wire,N,&W)==MtkStpOk && B.Count==4096);
    Before=F.Operations;
    if (Kind==0) CHECK(MtkStpTransmit(&T,2,Body,1)==MtkStpFull);
    if (Kind==1) CHECK(MtkWmtControlBegin(&Wmt,2,1)==MtkWmtBusy);
    if (Kind==2) CHECK(MtkWmtRegisterBegin(&Wmt,0,0x1234,0,0)==MtkWmtBusy);
    if (Kind==3) CHECK(MtkWmtGpsMcuBegin(&Wmt,Body,1,1)==MtkWmtBusy);
    if (Kind==4) CHECK(MtkWmtGpsSyncBegin(&Wmt,0x80,1)==MtkWmtBusy);
    if (Kind==5) CHECK(MtkWmtGpsPowerSaveBegin(&Wmt,1,1)==MtkWmtBusy);
    CHECK(!T.Faulted && !Wmt.Faulted && !Wmt.Pending && !B.Faulted);
    CHECK(B.Head==0 && B.Tail==0);
    CHECK(B.Count==4096 && F.Operations==Before && !F.SentCount);
    R=Drain(4096,16); if (R) return R;
    if (Kind==0) CHECK(MtkStpTransmit(&T,2,Body,1)==MtkStpOk);
    if (Kind==1) CHECK(MtkWmtControlBegin(&Wmt,2,1)==MtkWmtPending);
    if (Kind==2) CHECK(MtkWmtRegisterBegin(&Wmt,0,0x1234,0,0)==MtkWmtPending);
    if (Kind==3) CHECK(MtkWmtGpsMcuBegin(&Wmt,Body,1,1)==MtkWmtPending);
    if (Kind==4) CHECK(MtkWmtGpsSyncBegin(&Wmt,0x80,1)==MtkWmtPending);
    if (Kind==5) CHECK(MtkWmtGpsPowerSaveBegin(&Wmt,1,1)==MtkWmtPending && Wmt.GpsPowerSave);
    N=Kind==0?7:Kind==1?12:(Kind==2 || Kind==4)?26:Kind==3?11:12;
    CHECK(B.Count==N && !T.Faulted && !Wmt.Faulted);
    R=Drain(N,16); if (R) return R;
    CHECK(F.SentCount==4096+N);
  }
  return 0;
}
int TestMain(void) {
  int R;
  if ((R=StartStop()) || (R=Wake()) || (R=Transmit()) || (R=Failures()) || (R=Receive()) || (R=Duplex()) || (R=Integration()) || (R=Backpressure()) || (R=UncertainPressure())) return R;
  return 0;
}
#ifdef STP_TEST_NATIVE
int main(void) {
  int R=TestMain();
  if (R) { printf("BTIF test failed at line %d after %u assertions\n",R,MtkBtifAssertionCount); return 1; }
  printf("BTIF: %u assertions passed (offline FIFO IO and STP/WMT/GPS integration)\n",MtkBtifAssertionCount); return 0;
}
#endif
