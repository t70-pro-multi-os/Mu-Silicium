// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkBtifCore.h>
#include <stdint.h>
#define BTIF_INITIALIZED 0x42544946U
#define TX_MASK (MTK_BTIF_TX_BYTES-1U)
static const unsigned Controls[5]={0x0c,0x6c,0x60,0x4c,0x04};
static int Valid(const MTK_BTIF_CORE *B) {
  return B && B->Initialized==BTIF_INITIALIZED && B->Io.Read &&
    B->Io.Write && B->Io.Barrier && B->Count<=MTK_BTIF_TX_BYTES &&
    B->Head<MTK_BTIF_TX_BYTES && B->Tail<MTK_BTIF_TX_BYTES &&
    ((B->Head+B->Count)&TX_MASK)==B->Tail;
}
static int Overlaps(const void *P,unsigned N,const void *Q,unsigned M) {
  uintptr_t A=(uintptr_t)P,C=(uintptr_t)Q;
  return N && M && (A>=C?A-C<M:C-A<N);
}
static int Aliases(const MTK_BTIF_CORE *B,const void *P,unsigned N) {
  return Overlaps(P,N,B,sizeof(*B));
}
static MTK_BTIF_STATUS Error(MTK_BTIF_CORE *B) {
  if (B->Owned) B->Faulted=1;
  return MtkBtifIoError;
}
static int Read(MTK_BTIF_CORE *B,unsigned Region,unsigned Reg,unsigned *V) {
  return !B->Io.Read(B->Io.Context,Region,Reg,4,V) && *V!=0xffffffffU;
}
static int Write(MTK_BTIF_CORE *B,unsigned Reg,unsigned V,unsigned Stable) {
  unsigned Actual;
  int Failed=B->Io.Write(B->Io.Context,0,Reg,4,V);
  B->Io.Barrier(B->Io.Context);
  return !Failed && (!Stable || (Read(B,0,Reg,&Actual) && Actual==V));
}
static int Modify(MTK_BTIF_CORE *B,unsigned Reg,unsigned Clear,unsigned Set,unsigned Stable) {
  unsigned V;
  return Read(B,0,Reg,&V) && Write(B,Reg,(V&~Clear)|Set,Stable);
}
static MTK_BTIF_STATUS Idle(MTK_BTIF_CORE *B) {
  unsigned I,V;
  for (I=1;I<=2;I++) {
    if (!Read(B,I,8,&V)) return Error(B);
    if (V&1U) return MtkBtifBusy;
  }
  if (!Read(B,0,0x4c,&V)) return Error(B);
  if (V&3U) return MtkBtifBusy;
  if (!Read(B,0,0x14,&V)) return Error(B);
  if (!(V&0x40U) || (V&1U)) return MtkBtifBusy;
  if (!Read(B,0,8,&V)) return Error(B);
  return V&0x44U?MtkBtifBusy:MtkBtifOk;
}
MTK_BTIF_STATUS MtkBtifInit(MTK_BTIF_CORE *B,const MTK_BTIF_IO *Io) {
  unsigned I;
  volatile unsigned char *P;
  if (!B || !Io || !Io->Read || !Io->Write || !Io->Barrier || Aliases(B,Io,sizeof(*Io)))
    return MtkBtifInvalid;
  B->Io=*Io; B->Initialized=BTIF_INITIALIZED;
  B->Owned=B->Running=B->Faulted=B->Uncertain=0;
  B->SavedWake=B->WakeOwned=B->Head=B->Tail=B->Count=0;
  for (I=0;I<5;I++) B->Saved[I]=0;
  P=B->Tx; for (I=0;I<MTK_BTIF_TX_BYTES;I++) P[I]=0;
  return MtkBtifOk;
}
MTK_BTIF_STATUS MtkBtifStart(MTK_BTIF_CORE *B,unsigned TxTrigger,unsigned RxTrigger) {
  unsigned I;
  MTK_BTIF_STATUS S;
  if (!Valid(B) || TxTrigger>15 || RxTrigger>7) return MtkBtifInvalid;
  if (B->Faulted) return MtkBtifFaulted;
  if (B->Owned || B->Running || B->Count) return MtkBtifBusy;
  S=Idle(B); if (S!=MtkBtifOk) return S;
  for (I=0;I<5;I++) if (!Read(B,0,Controls[I],&B->Saved[I])) return Error(B);
  if (B->Saved[4]&3U) return MtkBtifBusy;
  /* Ownership precedes the first write, including an ambiguously failed one. */
  B->Owned=1;
  if (!Write(B,0x0c,0,1) || !Modify(B,0x6c,0,1,1) ||
      !Modify(B,8,0,2,0) || !Modify(B,8,2,0,0) ||
      !Modify(B,8,0,4,0) || !Modify(B,8,4,0,0) ||
      !Write(B,0x60,TxTrigger|(RxTrigger<<4),1) || !Modify(B,0x60,0x80,0,1) ||
      !Modify(B,0x4c,2,0,1) || !Modify(B,0x4c,1,0,1) ||
      !Modify(B,0x4c,0,4,1) || !Modify(B,4,2,0,1) || !Modify(B,4,0,1,1))
    return Error(B);
  B->Running=1; return MtkBtifOk;
}
MTK_BTIF_STATUS MtkBtifRaiseWake(MTK_BTIF_CORE *B,MTK_BTIF_WAIT_US Wait) {
  unsigned V;
  MTK_BTIF_STATUS S;
  if (!Valid(B) || !Wait) return MtkBtifInvalid;
  if (B->Faulted) return MtkBtifFaulted;
  if (!B->Owned || !B->Running) return MtkBtifInvalid;
  if (B->Count) return MtkBtifBusy;
  S=Idle(B); if (S!=MtkBtifOk) return S;
  if (!Read(B,0,0x64,&V)) return Error(B);
  if (!B->WakeOwned) { B->SavedWake=V; B->WakeOwned=1; }
  if (!Write(B,0x64,V&~1U,1) || Wait(B->Io.Context,128,160) ||
      !Modify(B,0x64,0,1,1)) return Error(B);
  return MtkBtifOk;
}
static int ValidTxBuffer(const MTK_BTIF_CORE *B,const unsigned char *Data,
  unsigned Length,unsigned *Written) {
  if (!Valid(B) || !Written || Aliases(B,Written,sizeof(*Written))) return 0;
  if (Data && (Aliases(B,Data,Length) || Overlaps(Data,Length,Written,sizeof(*Written))))
    return 0;
  *Written=0;
  return Data!=0;
}
static MTK_STP_STATUS Enqueue(MTK_BTIF_CORE *B,const unsigned char *Data,
  unsigned Length,unsigned *Written) {
  unsigned I;
  if (B->Faulted || !B->Owned || !B->Running) return MtkStpFaulted;
  if (Length>MTK_BTIF_TX_BYTES-B->Count) return MtkStpFull;
  for (I=0;I<Length;I++) { B->Tx[B->Tail]=Data[I]; B->Tail=(B->Tail+1U)&TX_MASK; }
  B->Count+=Length; *Written=Length;
  B->Io.Barrier(B->Io.Context);
  if (!Modify(B,4,0,2,1)) { Error(B); return MtkStpIoError; }
  return MtkStpOk;
}
MTK_STP_STATUS MtkBtifStpWrite(void *Context,const unsigned char *Data,
  unsigned Length,unsigned *Written) {
  MTK_BTIF_CORE *B=Context;
  unsigned N;
  if (!ValidTxBuffer(B,Data,Length,Written) || Length<7 ||
      Length>MTK_STP_TX_MAX+6U) return MtkStpInvalid;
  N=((unsigned)(Data[1]&15U)<<8)|Data[2];
  if (Data[0]!=0x80 || (Data[1]&0x80U) || Data[3] || N+6U!=Length ||
      Data[Length-1] || Data[Length-2]) return MtkStpMalformed;
  return Enqueue(B,Data,Length,Written);
}
MTK_STP_STATUS MtkBtifFullStpWrite(void *Context,const unsigned char *Data,
  unsigned Length,unsigned *Written) {
  MTK_BTIF_CORE *B=Context;
  const unsigned char *Header=Data;
  unsigned N,Prefix=0;
  if (!ValidTxBuffer(B,Data,Length,Written) || Length<4 ||
      Length>MTK_STP_TX_MAX+10U) return MtkStpInvalid;
  if (Data[0]==0x7fU) {
    if (Length<11 || Data[1]!=0x7fU || Data[2]!=0x7fU || Data[3]!=0x7fU)
      return MtkStpMalformed;
    Prefix=4;
    Header+=Prefix;
  }
  if ((Header[0]&0xc0U)!=0x80U || (Header[1]&0x80U) ||
      Header[3]!=(unsigned char)(Header[0]+Header[1]+Header[2]))
    return MtkStpMalformed;
  N=((unsigned)(Header[1]&15U)<<8)|Header[2];
  if (!N) {
    if (Length!=4 || (Header[0]&0x38U) || Header[1]) return MtkStpMalformed;
  } else if (N>MTK_STP_TX_MAX || N+6U+Prefix!=Length) {
    return MtkStpMalformed;
  }
  return Enqueue(B,Data,Length,Written);
}
MTK_BTIF_STATUS MtkBtifService(MTK_BTIF_CORE *B,unsigned char *Rx,
  unsigned Capacity,MTK_BTIF_SERVICE *R) {
  unsigned Irq,V,Room;
  if (!Valid(B) || !R || !Rx || !Capacity || Capacity>MTK_BTIF_RX_BUDGET ||
      Aliases(B,R,sizeof(*R)) || Aliases(B,Rx,Capacity) ||
      Overlaps(R,sizeof(*R),Rx,Capacity)) return MtkBtifInvalid;
  R->Received=R->Transmitted=R->More=R->Handled=0;
  if (B->Faulted) return MtkBtifFaulted;
  if (!B->Owned || !B->Running) return MtkBtifInvalid;
  if (!Read(B,0,8,&Irq)) return Error(B);
  while ((Irq&0x44U) && R->Received<Capacity) {
    R->Handled=1;
    if (B->Io.Read(B->Io.Context,0,0,1,&V) || V>255) {
      B->Uncertain=1; return Error(B);
    }
    Rx[R->Received++]=(unsigned char)V;
    if (!Read(B,0,8,&Irq)) return Error(B);
  }
  R->More=(Irq&0x44U)!=0;
  if (Irq&2U) {
    R->Handled=1;
    if (!Read(B,0,0x14,&V)) return Error(B);
    Room=(V&0x40U)?16U:((V&0x20U)?8U:0U);
    while (Room && B->Count) {
      int Failed=B->Io.Write(B->Io.Context,0,0,1,B->Tx[B->Head]);
      B->Io.Barrier(B->Io.Context);
      if (Failed) { B->Uncertain=1; return Error(B); }
      ((volatile unsigned char *)B->Tx)[B->Head]=0;
      B->Head=(B->Head+1U)&TX_MASK; --B->Count; --Room; ++R->Transmitted;
    }
    if ((V&0x60U) && !B->Count && !Modify(B,4,2,0,1)) return Error(B);
  }
  R->More|=B->Count!=0;
  if (R->More) return MtkBtifMore;
  return R->Handled?MtkBtifOk:MtkBtifNotOurs;
}
MTK_BTIF_STATUS MtkBtifStop(MTK_BTIF_CORE *B) {
  unsigned I,Live;
  MTK_BTIF_STATUS S;
  if (!Valid(B)) return MtkBtifInvalid;
  if (!B->Owned) return MtkBtifOk;
  if (B->Count || B->Uncertain) return MtkBtifBusy;
  S=Idle(B); if (S!=MtkBtifOk) return S;
  if (!Read(B,0,4,&Live) || !Write(B,4,Live&~3U,1)) return Error(B);
  S=Idle(B);
  if (S!=MtkBtifOk) {
    if (!Write(B,4,Live,1)) return Error(B);
    return S;
  }
  B->Running=0;
  for (I=0;I<5;I++) if (!Write(B,Controls[I],B->Saved[I],1)) return Error(B);
  if (B->WakeOwned && !Write(B,0x64,B->SavedWake,1)) return Error(B);
  B->WakeOwned=B->Owned=B->Faulted=0;
  return MtkBtifOk;
}
