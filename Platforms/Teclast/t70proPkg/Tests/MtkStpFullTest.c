// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkBtifCore.h>
#include <Library/MtkStpFull.h>

#ifdef STP_TEST_NATIVE
#include <stdio.h>
#endif

unsigned MtkStpFullAssertionCount;
#define CHECK(Test) do { ++MtkStpFullAssertionCount; if (!(Test)) return __LINE__; } while (0)
#define RUN(Test) do { unsigned Failure=(Test); if (Failure) return Failure; } while (0)

static MTK_STP_FULL_LINK Link;
static MTK_BTIF_CORE Fifo;
static unsigned char Wire[2U*(MTK_STP_FULL_RX_MAX+6U)];
static unsigned char Body[MTK_STP_FULL_RX_MAX];
static unsigned char Output[MTK_BTIF_TX_BYTES];
static unsigned Length, Writes, Blocked, Partial;

static int Equal(const unsigned char *A,const unsigned char *B,unsigned N) {
  for (unsigned I=0;I<N;I++) if (A[I]!=B[I]) return 0;
  return 1;
}

static MTK_STP_STATUS Write(void *Context,const unsigned char *Data,
  unsigned N,unsigned *Written) {
  if (Context!=Output || N>sizeof(Output)) return MtkStpInvalid;
  *Written=0;
  if (Blocked) return MtkStpFull;
  Writes++;
  Length=Partial?N-1U:N;
  for (unsigned I=0;I<Length;I++) Output[I]=Data[I];
  *Written=Length;
  return MtkStpOk;
}

static unsigned Frame(unsigned char *Buffer,unsigned Seq,unsigned Ack,
  unsigned Channel,unsigned N) {
  static const unsigned CrcNibble[] = {
    0,0xcc01,0xd801,0x1400,0xf001,0x3c00,0x2800,0xe401,
    0xa001,0x6c00,0x7800,0xb401,0x5000,0x9c01,0x8801,0x4400
  };
  unsigned Crc=0;
  Buffer[0]=(unsigned char)(0x80U|Seq<<3|Ack);
  Buffer[1]=(unsigned char)(Channel<<4|N>>8);
  Buffer[2]=(unsigned char)N;
  Buffer[3]=(unsigned char)(Buffer[0]+Buffer[1]+Buffer[2]);
  if (!N) return 4;
  for (unsigned I=0;I<N;I++) {
    Buffer[4+I]=Body[I];
    Crc^=Body[I];
    Crc=(Crc>>4)^CrcNibble[Crc&15U];
    Crc=(Crc>>4)^CrcNibble[Crc&15U];
  }
  Buffer[4+N]=(unsigned char)Crc;
  Buffer[5+N]=(unsigned char)(Crc>>8);
  return N+6U;
}

static unsigned Transfers(void) {
  MTK_STP_FRAME Rx;
  unsigned N, Used, Tx, Ack;
  CHECK(MtkStpFullInit(&Link,Output,Write)==MtkStpOk);
  for (unsigned I=0;I<24;I++) {
    Tx=I&7U;
    Ack=(I+7U)&7U;
    N=Frame(Wire,Tx,Ack,0,I+1U);
    CHECK(MtkStpFullTransmit(&Link,0,Body,I+1U,I*200U)==MtkStpOk);
    CHECK(Writes==2U*I+1U && Length==N && Equal(Output,Wire,N));
    CHECK(MtkStpFullTransmit(&Link,0,Body,1,I*200U)==MtkStpFull);
    N=Frame(Wire,0,Ack,0,0);
    CHECK(MtkStpFullReceive(&Link,Wire,N,&Used,&Rx)==MtkStpNeedMore && Used==N);
    CHECK(Link.TxLength!=0);
    N=Frame(Wire,Tx,Tx,4,9);
    Wire[N-1]^=1;
    CHECK(MtkStpFullReceive(&Link,Wire,N,&Used,&Rx)==MtkStpNeedMore && Used==N);
    CHECK(Link.TxLength && !Link.AckPending);
    N=Frame(Wire,Ack,Tx,4,9);
    CHECK(MtkStpFullReceive(&Link,Wire,N,&Used,&Rx)==MtkStpNeedMore && Used==N);
    CHECK(Link.TxLength && Link.AckPending);
    N=Frame(Wire,Tx,Tx,4,9);
    Frame(Wire+N,0,Tx,0,0);
    CHECK(MtkStpFullReceive(&Link,Wire,N+4,&Used,&Rx)==MtkStpFrameReady && Used==N);
    CHECK(Rx.Channel==4 && Rx.Length==9 && Equal(Rx.Data,Body,9) && !Link.TxLength);
    CHECK(MtkStpFullReceive(&Link,Wire+N,4,&Used,&Rx)==MtkStpFull && !Used);
    CHECK(Equal(Rx.Data,Body,9) && Link.RxNext==Tx);
    CHECK(MtkStpFullAccept(&Link)==MtkStpOk && MtkStpFullAccept(&Link)==MtkStpInvalid);
    Blocked=1;
    CHECK(MtkStpFullService(&Link,I*200U)==MtkStpFull && Link.AckPending);
    Blocked=0;
    CHECK(MtkStpFullService(&Link,I*200U)==MtkStpOk && !Link.AckPending);
    N=Frame(Wire,0,Tx,0,0);
    CHECK(Writes==2U*I+2U && Length==N && Equal(Output,Wire,N));
  }
  return 0;
}

static unsigned Fragmentation(void) {
  MTK_STP_FRAME Rx;
  unsigned N,Used;
  for (unsigned I=0;I<sizeof(Body);I++) Body[I]=0x7f;
  N=Frame(Wire,0,7,7,sizeof(Body));
  for (unsigned Cut=0;Cut<N;Cut++) {
    CHECK(MtkStpFullInit(&Link,Output,Write)==MtkStpOk);
    CHECK(MtkStpFullReceive(&Link,Wire,Cut,&Used,&Rx)==MtkStpNeedMore && Used==Cut);
    CHECK(MtkStpFullReceive(&Link,Wire+Cut,N-Cut,&Used,&Rx)==MtkStpFrameReady && Used==N-Cut);
    CHECK(Rx.Channel==7 && Rx.Length==sizeof(Body) && Equal(Rx.Data,Body,sizeof(Body)));
    CHECK(MtkStpFullAccept(&Link)==MtkStpOk);
  }
  N=Frame(Wire+4,0,7,0,1);
  for (unsigned I=0;I<4;I++) Wire[I]=0x7f;
  for (unsigned Cut=0;Cut<=4;Cut++) {
    static const unsigned char Truncated[]={0x87,0x20};
    CHECK(MtkStpFullInit(&Link,Output,Write)==MtkStpOk);
    CHECK(MtkStpFullReceive(&Link,Truncated,2,&Used,&Rx)==MtkStpNeedMore && Used==2);
    CHECK(MtkStpFullReceive(&Link,Wire,Cut,&Used,&Rx)==MtkStpNeedMore && Used==Cut);
    CHECK(MtkStpFullReceive(&Link,Wire+Cut,N+4-Cut,&Used,&Rx)==MtkStpFrameReady);
    CHECK(Used==N+4-Cut && Rx.Channel==0 && Rx.Length==1 && Rx.Data[0]==0x7f);
  }
  N=Frame(Wire,0,7,6,1);
  CHECK(MtkStpFullInit(&Link,Output,Write)==MtkStpOk);
  CHECK(MtkStpFullReceive(&Link,Wire,N,&Used,&Rx)==MtkStpFaulted && Used==4);
  CHECK(MtkStpFullReceive(&Link,Wire,N,&Used,&Rx)==MtkStpFaulted && Used==0);
  return 0;
}

static unsigned Timers(void) {
  MTK_STP_FRAME Rx;
  unsigned N,Used,Start=1000,At;
  const unsigned Deadline=MTK_STP_FULL_ACK_MS*(MTK_STP_FULL_RETRIES+1U);
  CHECK(MtkStpFullInit(&Link,Output,Write)==MtkStpOk);
  for (unsigned I=0;I<sizeof(Body);I++) Body[I]=(unsigned char)(I*41U);
  Writes=0;
  Blocked=1;
  CHECK(MtkStpFullTransmit(&Link,3,Body,1,Start)==MtkStpFull && !Link.TxLength);
  Blocked=0;
  CHECK(MtkStpFullTransmit(&Link,3,Body,MTK_STP_TX_MAX+1U,Start)==MtkStpInvalid);
  CHECK(MtkStpFullTransmit(&Link,3,Body,MTK_STP_TX_MAX,Start)==MtkStpOk);
  N=Frame(Wire,0,7,3,MTK_STP_TX_MAX);
  CHECK(Length==N && Equal(Output,Wire,N));
  N=Frame(Wire,0,7,0,2);
  CHECK(MtkStpFullReceive(&Link,Wire,N,&Used,&Rx)==MtkStpFrameReady);
  CHECK(MtkStpFullAccept(&Link)==MtkStpOk && Link.TxLength);
  N=Frame(Wire+4,0,0,3,MTK_STP_TX_MAX);
  for (unsigned I=0;I<4;I++) Wire[I]=0x7f;
  CHECK(MtkStpFullService(&Link,Start+MTK_STP_FULL_ACK_MS-1U)==MtkStpOk && Writes==2);
  for (unsigned I=1;I<=MTK_STP_FULL_RETRIES;I++) {
    At=Start+I*MTK_STP_FULL_ACK_MS;
    CHECK(MtkStpFullService(&Link,At)==MtkStpOk && Writes==I+2U);
    CHECK(Length==N+4U && Equal(Output,Wire,Length));
  }
  CHECK(MtkStpFullService(&Link,Start+Deadline)==MtkStpFaulted && Writes==12);
  CHECK(MtkStpFullTransmit(&Link,3,Body,1,Start+Deadline)==MtkStpFaulted);
  CHECK(MtkStpFullInit(&Link,Output,Write)==MtkStpOk);
  CHECK(MtkStpFullTransmit(&Link,3,Body,1,Start)==MtkStpOk);
  Blocked=1;
  CHECK(MtkStpFullService(&Link,Start+MTK_STP_FULL_ACK_MS)==MtkStpFull);
  Blocked=0;
  CHECK(MtkStpFullService(&Link,Start+Deadline-1U)==MtkStpOk);
  CHECK(MtkStpFullService(&Link,Start+Deadline)==MtkStpFaulted);
  CHECK(MtkStpFullInit(&Link,Output,Write)==MtkStpOk);
  CHECK(MtkStpFullTransmit(&Link,3,Body,1,Start)==MtkStpOk);
  N=Frame(Wire,0,0,0,0);
  CHECK(MtkStpFullReceive(&Link,Wire,N,&Used,&Rx)==MtkStpNeedMore && !Link.TxLength);
  CHECK(MtkStpFullService(&Link,Start+Deadline)==MtkStpOk);
  Partial=1;
  CHECK(MtkStpFullTransmit(&Link,3,Body,1,Start+Deadline)==MtkStpIoError);
  CHECK(MtkStpFullService(&Link,Start+Deadline)==MtkStpFaulted);
  Partial=0;
  return 0;
}

static unsigned Regs[0x70/4];

static int FifoRead(void *Context,unsigned Region,unsigned Reg,unsigned Width,unsigned *Value) {
  if (Context!=Regs || Width!=4 || Reg>=sizeof(Regs) || Reg%4U) return 1;
  *Value=Region?0:Reg==8?(Regs[1]&2U):Reg==0x14?0x60:Regs[Reg/4U];
  return 0;
}

static int FifoWrite(void *Context,unsigned Region,unsigned Reg,unsigned Width,unsigned Value) {
  if (Context!=Regs || Region || Reg>=sizeof(Regs) || Reg%4U) return 1;
  if (Width==1 && !Reg && Length<sizeof(Output)) Output[Length++]=(unsigned char)Value;
  else if (Width==4) Regs[Reg/4U]=Value;
  else return 1;
  return 0;
}

static void FifoBarrier(void *Context) { (void)Context; }

static unsigned FifoTransfer(void) {
  MTK_BTIF_IO Io={Regs,FifoRead,FifoWrite,FifoBarrier};
  MTK_BTIF_SERVICE Service;
  unsigned char Rx[1];
  unsigned N,Written;
  CHECK(MtkBtifInit(&Fifo,&Io)==MtkBtifOk);
  CHECK(MtkStpFullInit(&Link,&Fifo,MtkBtifFullStpWrite)==MtkStpOk);
  CHECK(MtkBtifStart(&Fifo,MTK_BTIF_TX_TRIGGER,MTK_BTIF_RX_TRIGGER)==MtkBtifOk);
  N=Frame(Wire,0,7,0,MTK_STP_TX_MAX);
  for (unsigned I=0;I<4;I++) Wire[N+I]=0x7f;
  Frame(Wire+N+4,0,7,0,MTK_STP_TX_MAX);
  Wire[3]^=1;
  CHECK(MtkBtifFullStpWrite(&Fifo,Wire,N,&Written)==MtkStpMalformed && !Written);
  Wire[3]^=1;
  CHECK(MtkBtifStpWrite(&Fifo,Wire,N,&Written)==MtkStpMalformed && !Written);
  CHECK(MtkStpFullTransmit(&Link,0,Body,MTK_STP_TX_MAX,0)==MtkStpOk && Fifo.Count==N);
  CHECK(MtkStpFullService(&Link,MTK_STP_FULL_ACK_MS)==MtkStpOk && Fifo.Count==N*2U+4U);
  CHECK(MtkStpFullService(&Link,MTK_STP_FULL_ACK_MS*2U)==MtkStpFull && Fifo.Count==N*2U+4U);
  Length=0;
  while (Fifo.Count) {
    MTK_BTIF_STATUS Status=MtkBtifService(&Fifo,Rx,sizeof(Rx),&Service);
    CHECK(Status==MtkBtifMore || Status==MtkBtifOk);
    CHECK(Service.Transmitted>0 && Service.Transmitted<=16 && Length<=N*2U+4U);
  }
  CHECK(Length==N*2U+4U && Equal(Output,Wire,Length));
  CHECK(MtkBtifStop(&Fifo)==MtkBtifOk && !Fifo.Owned);
  return 0;
}

unsigned TestMain(void) {
  for (unsigned I=0;I<sizeof(Body);I++) Body[I]=(unsigned char)(I*37U);
  RUN(Transfers());
  RUN(Fragmentation());
  RUN(Timers());
  RUN(FifoTransfer());
  return 0;
}

#ifdef STP_TEST_NATIVE
int main(void) {
  unsigned Failure=TestMain();
  if (Failure) fprintf(stderr,"MtkStpFullTest.c:%u\n",Failure);
  else printf("STP full mode passed %u assertions\n",MtkStpFullAssertionCount);
  return Failure!=0;
}
#endif
