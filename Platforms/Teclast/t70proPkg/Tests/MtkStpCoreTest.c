// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkStpCore.h>
#ifdef STP_TEST_NATIVE
#include <stdio.h>
#endif
unsigned MtkStpAssertionCount;
#define CHECK(X) do { ++MtkStpAssertionCount; if (!(X)) return __LINE__; } while (0)
static MTK_STP_DECODER Decoder;
static MTK_STP_TRANSMITTER Transmitter;
static unsigned char Payload[MTK_STP_RX_MAX],Wire[MTK_STP_FRAME_MAX+3];
static unsigned char Overlap[MTK_STP_FRAME_MAX+32];
static int Equal(const unsigned char *A,const unsigned char *B,unsigned N) {
  unsigned I; for (I=0;I<N;I++) if (A[I]!=B[I]) return 0; return 1;
}
static void MakeWire(unsigned Channel,unsigned N) {
  unsigned I; Wire[0]=0x80; Wire[1]=(unsigned char)((Channel<<4)|(N>>8));
  Wire[2]=(unsigned char)N; Wire[3]=0;
  for (I=0;I<N;I++) Wire[I+4]=Payload[I];
  for (I=N+4;I<sizeof(Wire);I++) Wire[I]=0;
}
static int ReceiveCase(unsigned Mode,unsigned Channel,unsigned N,unsigned Chunk) {
  unsigned Total=N+6,Offset=0,Count,Part;
  MTK_STP_STATUS S;
  MTK_STP_FRAME F={99,99,0};
  MakeWire(Channel,N);
  if (Mode==MTK_STP_SDIO) {
    while (Total&3U) Wire[Total++]=0xaa; /* Padding has no payload semantics. */
  }
  CHECK(MtkStpDecoderInit(&Decoder,Mode)==MtkStpOk);
  CHECK(MtkStpReceive(&Decoder,0,0,&Count,&F)==MtkStpNeedMore && Count==0);
  while (Offset<Total) {
    Part=Total-Offset; if (Part>Chunk) Part=Chunk;
    S=MtkStpReceive(&Decoder,Wire+Offset,Part,&Count,&F);
    CHECK(Count==Part); Offset+=Count;
    if (Offset<Total) CHECK(S==MtkStpNeedMore && F.Channel==99 && F.Data==0);
    else CHECK(S==MtkStpFrameReady && F.Channel==Channel && F.Length==N);
  }
  CHECK(Equal(F.Data,Payload,N));
  CHECK(Decoder.State==0 && Decoder.Faulted==0);
  return 0;
}
static unsigned Calls,FailWritten;
static MTK_STP_STATUS WriteStatus;
static MTK_STP_STATUS Write(void *Context,const unsigned char *Data,unsigned N,
  unsigned *Written) {
  unsigned I; (void)Context; ++Calls;
  for (I=0;I<N;I++) Wire[I]=Data[I];
  *Written=FailWritten==0xffffffffU?N:FailWritten; return WriteStatus;
}
static int WmtControlTest(void) {
  MTK_WMT_CONTROL C;
  unsigned I,J,Before;
  unsigned char Event[8]={2,6,1,0,0,0,0,0};
  MTK_STP_FRAME F={4,5,Event};
  CHECK(MtkWmtControlInit(0,&Transmitter)==MtkWmtInvalid);
  CHECK(MtkWmtControlInit(&C,0)==MtkWmtInvalid);
  CHECK(MtkWmtControlBegin(0,2,1)==MtkWmtInvalid);
  CHECK(MtkWmtControlEvent(0,&F)==MtkWmtInvalid);
  CHECK(MtkWmtControlExpire(0)==MtkWmtInvalid);
  FailWritten=0xffffffffU; WriteStatus=MtkStpOk; Calls=0;
  CHECK(MtkStpTransmitterInit(&Transmitter,8,0,Write)==MtkStpOk);
  for (I=0;I<256;I++) for (J=0;J<2;J++) {
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtControlBegin(&C,I,J)==MtkWmtPending);
    CHECK(Wire[0]==0x80 && Wire[1]==0x40 && Wire[2]==6 && Wire[3]==0);
    CHECK(Wire[4]==1 && Wire[5]==6 && Wire[6]==2 && Wire[7]==0);
    CHECK(Wire[8]==I && Wire[9]==J && Wire[10]==0 && Wire[11]==0);
    Before=Calls;
    CHECK(MtkWmtControlBegin(&C,I,J)==MtkWmtBusy && Calls==Before);
    CHECK(C.Pending==1 && C.Confirmed==0);
    F.Channel=2;
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtUnrelated && C.Pending==1);
    F.Channel=4; Event[1]=7;
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtUnrelated && C.Pending==1);
    Event[1]=6;
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtOk);
    CHECK(C.Confirmed==1 && C.Pending==0 && C.Faulted==0 && C.Function==I && C.Enable==J);
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtUncertain && C.Faulted==1 && C.Confirmed==0);
    CHECK(MtkWmtControlBegin(&C,I,J)==MtkWmtUncertain && Calls==Before);
  }
  for (I=1;I<256;I++) {
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtPending);
    Event[4]=(unsigned char)I;
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtRejected);
    CHECK(C.FirmwareStatus==I && C.Confirmed==0 && C.Faulted==1 && C.Pending==0);
  }
  Event[4]=0;
  for (I=0;I<8;I++) {
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtPending);
    F.Length=I;
    CHECK(MtkWmtControlEvent(&C,&F)==(I==5?MtkWmtOk:MtkWmtMalformed));
    CHECK(C.Confirmed==(I==5) && C.Faulted==(I!=5));
  }
  F.Length=5;
  for (I=0;I<4;I++) {
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtPending);
    if (I==0) Event[0]=1;
    if (I==1) Event[2]=2;
    if (I==2) Event[3]=1;
    if (I==3) F.Data=0;
    CHECK(MtkWmtControlEvent(&C,&F)==(I==3?MtkWmtInvalid:MtkWmtMalformed));
    Event[0]=2; Event[2]=1; Event[3]=0; F.Data=Event;
  }
  CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
  Before=Calls;
  CHECK(MtkWmtControlBegin(&C,256,1)==MtkWmtInvalid && Calls==Before);
  CHECK(MtkWmtControlBegin(&C,2,2)==MtkWmtInvalid && Calls==Before);
  CHECK(MtkWmtControlExpire(&C)==MtkWmtInvalid);
  CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtPending);
  CHECK(MtkWmtControlExpire(&C)==MtkWmtUncertain && C.Faulted==1 && C.Confirmed==0);
  Before=Calls;
  CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtUncertain && Calls==Before);
  CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtUncertain && C.Confirmed==0);
  CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
  FailWritten=11;
  CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtUncertain && C.Faulted==1 && Transmitter.Faulted==1);
  CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtInvalid);
  CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtUncertain);
  return 0;
}
static void Put32(unsigned char *P,unsigned V) {
  unsigned I; for (I=0;I<4;I++) P[I]=(unsigned char)(V>>(8U*I));
}
static void RegisterEvent(unsigned char *Event,unsigned Write,unsigned Address,unsigned Value) {
  unsigned I; for (I=0;I<17;I++) Event[I]=0;
  Event[0]=2; Event[1]=8; Event[2]=(unsigned char)(Write?4:12);
  /* Reserved response bytes are opaque, not an invented echo requirement. */
  Event[5]=0x5a; Event[6]=0xc3; Event[7]=0x19;
  Put32(Event+8,Address); Put32(Event+12,Value);
}
static int RegisterTest(void) {
  MTK_WMT_CONTROL C;
  unsigned I,J,K,Before,Address=0x80050078U,Value=0xf1e2d3c4U,Mask=0x70000000U;
  unsigned char Event[17];
  MTK_STP_FRAME F={4,16,Event};
  CHECK(MtkWmtRegisterBegin(0,1,Address,Value,Mask)==MtkWmtInvalid);
  CHECK(MtkWmtGpsSyncBegin(0,0x80,1)==MtkWmtInvalid);
  Calls=0; FailWritten=0xffffffffU; WriteStatus=MtkStpOk;
  CHECK(MtkStpTransmitterInit(&Transmitter,8,0,Write)==MtkStpOk);
  CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
  CHECK(MtkWmtRegisterBegin(&C,2,Address,Value,Mask)==MtkWmtInvalid && !Calls);
  CHECK(MtkWmtGpsSyncBegin(&C,256,1)==MtkWmtInvalid && !Calls);
  CHECK(MtkWmtGpsSyncBegin(&C,0x80,2)==MtkWmtInvalid && !Calls);
  for (J=0;J<2;J++) {
    RegisterEvent(Event,J,Address,Value); F.Length=J?8:16;
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtRegisterBegin(&C,J,Address,Value,Mask)==MtkWmtPending);
    CHECK(Wire[0]==0x80 && Wire[1]==0x40 && Wire[2]==20 && Wire[3]==0);
    CHECK(Wire[4]==1 && Wire[5]==8 && Wire[6]==16 && !Wire[7]);
    CHECK(Wire[8]==(J?1:2) && Wire[9]==1 && !Wire[10] && Wire[11]==1);
    for (I=0;I<4;I++) {
      CHECK(Wire[12+I]==(unsigned char)(Address>>(8U*I)));
      CHECK(Wire[16+I]==(unsigned char)(Value>>(8U*I)));
      CHECK(Wire[20+I]==(unsigned char)(Mask>>(8U*I)));
    }
    CHECK(!Wire[24] && !Wire[25] && !C.RegisterValue && !C.Confirmed);
    Before=Calls;
    CHECK(MtkWmtControlBegin(&C,2,1)==MtkWmtBusy && Calls==Before);
    CHECK(MtkWmtRegisterBegin(&C,0,0,0,0)==MtkWmtBusy && Calls==Before);
    CHECK(MtkWmtGpsMcuBegin(&C,Payload,1,1)==MtkWmtBusy && Calls==Before);
    CHECK(MtkWmtGpsSyncBegin(&C,0x80,1)==MtkWmtBusy && Calls==Before);
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtOk && C.Confirmed && !C.Pending && !C.Faulted);
    CHECK(C.RegisterWrite==J && C.RegisterAddress==Address && C.RegisterValue==(J?0:Value));
    CHECK(C.FirmwareStatus==0 && !C.ReplyLength);
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtUncertain && !C.Confirmed && !C.RegisterValue);
    CHECK(MtkWmtRegisterBegin(&C,0,0,0,0)==MtkWmtUncertain && Calls==Before);
    for (I=0;I<=17;I++) {
      CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
      CHECK(MtkWmtRegisterBegin(&C,J,Address,Value,Mask)==MtkWmtPending);
      F.Length=I;
      CHECK(MtkWmtControlEvent(&C,&F)==(I==(J?8U:16U)?MtkWmtOk:MtkWmtMalformed));
      CHECK(C.Confirmed==(I==(J?8U:16U)) && C.RegisterValue==(!J && I==16?Value:0));
    }
    F.Length=J?8:16;
    for (K=0;K<5;K++) for (I=0;I<256;I++) {
      CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
      CHECK(MtkWmtRegisterBegin(&C,J,Address,Value,Mask)==MtkWmtPending);
      RegisterEvent(Event,J,Address,Value); Event[K]=(unsigned char)I;
      if (I==(K==0?2U:K==1?8U:K==2?(J?4U:12U):0U)) {
        CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtOk && C.Confirmed);
      } else if (K==1 && I!=6 && I!=0x32) {
        CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtUnrelated && C.Pending && !C.RegisterValue);
      } else {
        CHECK(MtkWmtControlEvent(&C,&F)==(K==4?MtkWmtRejected:MtkWmtMalformed));
        CHECK(C.Faulted && !C.Pending && !C.Confirmed && !C.RegisterValue);
      }
    }
  }
  for (I=0;I<32;I++) {
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtRegisterBegin(&C,0,Address,0,0xffffffffU)==MtkWmtPending);
    RegisterEvent(Event,0,Address^(1U<<I),Value); F.Length=16;
    CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtMalformed && !C.RegisterValue && C.Faulted);
  }
  for (I=0;I<256;I++) for (J=0;J<2;J++) {
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk); Before=Calls;
    CHECK(MtkWmtGpsSyncBegin(&C,I,J)==(I&0x80?MtkWmtPending:MtkWmtUnsupported));
    if (I&0x80) {
      CHECK(Calls==Before+1 && C.RegisterWrite && C.RegisterAddress==Address);
      CHECK(Wire[19]==(J?0x10:0x50) && Wire[23]==0x70);
      RegisterEvent(Event,1,Address,0); F.Length=8;
      CHECK(MtkWmtControlEvent(&C,&F)==MtkWmtOk && C.Confirmed);
    } else CHECK(Calls==Before && !C.Confirmed && !C.Pending);
  }
  /* A partial frame or a timeout is ambiguous: never replay the register write. */
  for (I=0;I<=26;I++) {
    CHECK(MtkStpTransmitterInit(&Transmitter,8,0,Write)==MtkStpOk);
    CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
    FailWritten=I;
    CHECK(MtkWmtRegisterBegin(&C,1,Address,Value,Mask)==(I==26?MtkWmtPending:MtkWmtUncertain));
    if (I==26) CHECK(MtkWmtControlExpire(&C)==MtkWmtUncertain);
    Before=Calls;
    CHECK(MtkWmtGpsSyncBegin(&C,0x80,0)==MtkWmtUncertain && Calls==Before && !C.RegisterValue);
  }
  FailWritten=0xffffffffU; WriteStatus=MtkStpIoError;
  CHECK(MtkStpTransmitterInit(&Transmitter,8,0,Write)==MtkStpOk);
  CHECK(MtkWmtControlInit(&C,&Transmitter)==MtkWmtOk);
  CHECK(MtkWmtRegisterBegin(&C,1,Address,Value,Mask)==MtkWmtUncertain && C.Faulted && Transmitter.Faulted);
  return 0;
}
static MTK_WMT_CONTROL Mcu;
static unsigned char McuEvent[MTK_STP_RX_MAX+1];
static void MakeMcuEvent(unsigned N) {
  unsigned I;
  McuEvent[0]=2; McuEvent[1]=0x32;
  McuEvent[2]=(unsigned char)N; McuEvent[3]=(unsigned char)(N>>8);
  for (I=0;I<N;I++) McuEvent[I+4]=(unsigned char)(I*57U+N);
}
static int McuTest(void) {
  unsigned I,J,N,Before;
  MTK_STP_FRAME F={4,0,McuEvent};
  CHECK(MtkWmtGpsMcuBegin(0,Payload,1,1)==MtkWmtInvalid);
  CHECK(MtkWmtGpsPowerSaveBegin(0,1,0)==MtkWmtInvalid);
  Calls=0; FailWritten=0xffffffffU; WriteStatus=MtkStpOk;
  CHECK(MtkStpTransmitterInit(&Transmitter,8,0,Write)==MtkStpOk);
  CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,0,1,1)==MtkWmtInvalid);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,0,1)==MtkWmtInvalid);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,1993,1)==MtkWmtInvalid);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,1,0)==MtkWmtInvalid);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,1,1996)==MtkWmtInvalid);
  CHECK(MtkWmtGpsPowerSaveBegin(&Mcu,2,0)==MtkWmtInvalid);
  CHECK(MtkWmtGpsPowerSaveBegin(&Mcu,1,256)==MtkWmtInvalid && Calls==0);
  for (N=1;N<=MTK_WMT_GPS_MCU_TX_MAX;N++) {
    for (I=0;I<N;I++) Payload[I]=(unsigned char)(I*73U+N);
    CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,N,N)==MtkWmtPending);
    CHECK(Wire[0]==0x80 && Wire[1]==(0x40U|((N+4U)>>8)) && Wire[2]==(unsigned char)(N+4U) && Wire[3]==0);
    CHECK(Wire[4]==1 && Wire[5]==0x32 && Wire[6]==(unsigned char)N && Wire[7]==(N>>8));
    for (I=0;I<N;I++) CHECK(Wire[I+8]==Payload[I]);
    CHECK(Wire[N+8]==0 && Wire[N+9]==0 && Mcu.Opcode==0x32 && !Mcu.Confirmed);
    Before=Calls;
    CHECK(MtkWmtControlBegin(&Mcu,2,1)==MtkWmtBusy && Calls==Before);
    CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,1,1)==MtkWmtBusy && Calls==Before);
    MakeMcuEvent(N); F.Length=N+4;
    CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtOk && Mcu.Confirmed && !Mcu.Faulted && !Mcu.Pending);
    CHECK(Mcu.ReplyLength==N && Mcu.FirmwareStatus==255);
    for (I=0;I<N;I++) CHECK(Mcu.Reply[I]==McuEvent[I+4]);
  }
  CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,1,1995)==MtkWmtPending);
  MakeMcuEvent(1995); F.Length=1999;
  CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtOk && Mcu.ReplyLength==1995);
  CHECK(Equal(Mcu.Reply,McuEvent+4,1995));
  CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtUncertain && !Mcu.Confirmed && Mcu.Faulted);
  /* Both directions of caller/TX staging-buffer overlap preserve the command. */
  for (J=0;J<17;J++) {
    for (I=0;I<32;I++) Transmitter.Buffer[J+I]=Payload[I]=(unsigned char)(I*71U+J);
    CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtGpsMcuBegin(&Mcu,Transmitter.Buffer+J,32,3)==MtkWmtPending);
    CHECK(Equal(Wire+8,Payload,32));
  }
  for (I=0;I<2;I++) for (J=0;J<256;J++) {
    CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtGpsPowerSaveBegin(&Mcu,I,J)==MtkWmtPending);
    CHECK(Wire[4]==1 && Wire[5]==0x32 && Wire[6]==2 && Wire[7]==0);
    CHECK(Wire[8]==(I?4:5) && Wire[9]==J && Mcu.ReplyCapacity==3 && Mcu.Enable==I);
    MakeMcuEvent(3); F.Length=7; McuEvent[4]=0;
    CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtOk && Mcu.Confirmed && Mcu.FirmwareStatus==0);
  }
  for (I=1;I<256;I++) {
    CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtGpsPowerSaveBegin(&Mcu,1,2)==MtkWmtPending);
    McuEvent[4]=(unsigned char)I;
    CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtRejected && Mcu.FirmwareStatus==I);
    CHECK(Mcu.Faulted && !Mcu.Pending && !Mcu.Confirmed && Mcu.ReplyLength==3);
  }
  /* Zero status is not enough without the actual declared reply body. */
  for (I=0;I<=8;I++) {
    CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtGpsPowerSaveBegin(&Mcu,1,2)==MtkWmtPending);
    MakeMcuEvent(3); McuEvent[4]=0; F.Length=I;
    CHECK(MtkWmtControlEvent(&Mcu,&F)==(I==7?MtkWmtOk:MtkWmtMalformed));
    CHECK(Mcu.Confirmed==(I==7) && Mcu.Faulted==(I!=7));
  }
  for (I=0;I<=3;I++) {
    CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtGpsPowerSaveBegin(&Mcu,1,0)==MtkWmtPending);
    MakeMcuEvent(I); McuEvent[4]=0; F.Length=I+4;
    CHECK(MtkWmtControlEvent(&Mcu,&F)==(I>=2?MtkWmtOk:MtkWmtMalformed));
  }
  for (I=0;I<5;I++) {
    CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
    CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,2,3)==MtkWmtPending);
    MakeMcuEvent(3); F.Length=7;
    if (I==0) McuEvent[0]=1;
    if (I==1) McuEvent[1]=6;
    if (I==2) McuEvent[2]=4;
    if (I==3) McuEvent[3]=1;
    if (I==4) F.Length=MTK_STP_RX_MAX+1;
    CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtMalformed && Mcu.Faulted && !Mcu.ReplyLength);
  }
  CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,1,2)==MtkWmtPending);
  MakeMcuEvent(3); F.Length=7;
  CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtMalformed && Mcu.Faulted && !Mcu.ReplyLength);
  CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,1,1)==MtkWmtPending);
  MakeMcuEvent(0); F.Length=4;
  CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtOk && Mcu.Confirmed && Mcu.ReplyLength==0);
  CHECK(MtkWmtControlBegin(&Mcu,2,1)==MtkWmtPending);
  MakeMcuEvent(1); F.Length=5;
  CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtMalformed && Mcu.Faulted && !Mcu.Confirmed);
  CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
  for (I=0;I<MTK_WMT_GPS_MCU_RX_MAX;I++) CHECK(Mcu.Reply[I]==0);
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,2,3)==MtkWmtPending);
  McuEvent[1]=7; F.Length=7;
  CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtUnrelated && Mcu.Pending);
  CHECK(MtkWmtControlExpire(&Mcu)==MtkWmtUncertain && Mcu.Faulted && !Mcu.Pending);
  MakeMcuEvent(3);
  CHECK(MtkWmtControlEvent(&Mcu,&F)==MtkWmtUncertain && !Mcu.Confirmed);
  CHECK(MtkWmtControlInit(&Mcu,&Transmitter)==MtkWmtOk);
  FailWritten=11;
  CHECK(MtkWmtGpsPowerSaveBegin(&Mcu,1,0)==MtkWmtUncertain && Transmitter.Faulted && Mcu.Faulted && !Mcu.Pending);
  Before=Calls;
  CHECK(MtkWmtGpsMcuBegin(&Mcu,Payload,2,3)==MtkWmtUncertain && Calls==Before);
  return 0;
}
static MTK_STP_QUEUE Queue;
static unsigned char ReadBuffer[MTK_STP_QUEUE_BYTES];
static int QueueTest(void) {
  unsigned I,J,N,Read,Epoch,OldEpoch,BeforeCount,Seed=0x20f30f39U,Expected=0,Produced=0;
  MTK_STP_FRAME F={2,1999,Payload};
  CHECK(MtkStpQueueInit(0,2)==MtkStpInvalid);
  CHECK(MtkStpQueueInit(&Queue,8)==MtkStpInvalid);
  CHECK(MtkStpQueueInit(&Queue,2)==MtkStpOk); Epoch=Queue.Epoch;
  CHECK(MtkStpQueueRead(&Queue,Epoch,ReadBuffer,16384,&Read)==MtkStpNeedMore && Read==0);
  CHECK(MtkStpQueueRead(&Queue,Epoch,0,0,&Read)==MtkStpOk && Read==0);
  CHECK(MtkStpQueueRead(&Queue,Epoch,0,1,&Read)==MtkStpInvalid && Read==0);
  for (I=0;I<1200;I++) {
    Seed^=Seed<<13; Seed^=Seed>>17; Seed^=Seed<<5;
    N=Seed%1999+1;
    if ((Seed&1U) && N<=MTK_STP_QUEUE_CAPACITY-Queue.Count) {
      for (J=0;J<N;J++) Payload[J]=(unsigned char)((Produced+J)*73U);
      F.Length=N;
      CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpOk); Produced+=N;
    } else {
      BeforeCount=Queue.Count;
      CHECK(MtkStpQueueRead(&Queue,Epoch,ReadBuffer,N,&Read)==(BeforeCount?MtkStpOk:MtkStpNeedMore));
      CHECK(Read==(BeforeCount<N?BeforeCount:N));
      for (J=0;J<Read;J++) CHECK(ReadBuffer[J]==(unsigned char)((Expected+J)*73U));
      Expected+=Read;
    }
    CHECK(Queue.Count==Produced-Expected);
  }
  OldEpoch=Epoch;
  CHECK(MtkStpQueueReset(&Queue,&Epoch)==MtkStpOk && Epoch==OldEpoch+1 && Queue.Count==0);
  for (I=0;I<MTK_STP_QUEUE_BYTES;I++) CHECK(Queue.Data[I]==0);
  CHECK(MtkStpQueuePush(&Queue,OldEpoch,&F)==MtkStpStale && Queue.Count==0);
  CHECK(MtkStpQueueRead(&Queue,OldEpoch,ReadBuffer,1,&Read)==MtkStpStale && Read==0);
  for (I=0;I<8;I++) { F.Length=1999; CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpOk); }
  F.Length=391; CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpOk && Queue.Count==16383);
  F.Length=1;
  CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpFull && Queue.Faulted && Queue.Count==0);
  for (I=0;I<MTK_STP_QUEUE_BYTES;I++) CHECK(Queue.Data[I]==0);
  CHECK(MtkStpQueueRead(&Queue,Epoch,ReadBuffer,1,&Read)==MtkStpFaulted && Read==0);
  CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpFaulted);
  CHECK(MtkStpQueueReset(&Queue,&Epoch)==MtkStpOk);
  F.Channel=4;
  CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpUnsupported && Queue.Count==0);
  F.Channel=2; F.Length=2000;
  CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpInvalid && Queue.Count==0);
  F.Length=1; F.Data=0;
  CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpInvalid);
  F.Data=Payload; Queue.Head=MTK_STP_QUEUE_BYTES;
  CHECK(MtkStpQueueRead(&Queue,Epoch,ReadBuffer,1,&Read)==MtkStpMalformed && Read==0);
  CHECK(MtkStpQueueReset(&Queue,&Epoch)==MtkStpOk);
  CHECK(MtkStpQueuePush(&Queue,Epoch,&F)==MtkStpOk);
  Queue.Epoch=0xffffffffU; Epoch=Queue.Epoch;
  CHECK(MtkStpQueueReset(&Queue,&Epoch)==MtkStpFaulted && Queue.Faulted && Queue.Count==0);
  CHECK(MtkStpQueueRead(&Queue,Epoch,ReadBuffer,1,&Read)==MtkStpFaulted && Read==0);
  return 0;
}
int TestMain(void) {
  static const unsigned Modes[]={2,8,16};
  static const unsigned Sizes[]={0,1,2,3,4,7,15,31,255,256,257,511,1023,1996,1997,1998,1999};
  static const unsigned Chunks[]={1,2,3,4,7,31,257,2008};
  unsigned I,J,K,M,N,Written,Count,Offset,Length;
  MTK_STP_STATUS S;
  MTK_STP_FRAME F={99,99,0};
  int Result;
  for (I=0;I<sizeof(Payload);I++) Payload[I]=(unsigned char)(I*73U+(I>>8));
  CHECK(MtkStpDecoderInit(0,8)==MtkStpInvalid);
  CHECK(MtkStpDecoderInit(&Decoder,1)==MtkStpUnsupported);
  CHECK(MtkStpDecoderInit(&Decoder,4)==MtkStpUnsupported);
  CHECK(MtkStpDecoderInit(&Decoder,10)==MtkStpUnsupported);
  CHECK(MtkStpReceive(0,Wire,1,&Count,&F)==MtkStpInvalid && Count==0);
  CHECK(MtkStpDecoderInit(&Decoder,8)==MtkStpOk);
  CHECK(MtkStpReceive(&Decoder,0,1,&Count,&F)==MtkStpInvalid && Count==0);
  CHECK(MtkStpReceive(&Decoder,Wire,1,0,&F)==MtkStpInvalid);
  for (M=0;M<3;M++) for (K=0;K<sizeof(Sizes)/sizeof(Sizes[0]);K++)
    for (J=0;J<sizeof(Chunks)/sizeof(Chunks[0]);J++) for (I=0;I<8;I++) {
      Result=ReceiveCase(Modes[M],I,Sizes[K],Chunks[J]); CHECK(Result==0);
    }
  for (M=0;M<3;M++) for (N=1;N<=MTK_STP_TX_MAX;N++) {
    I=N&7U; MakeWire(I,N); Written=0;
    CHECK(MtkStpEncode(Modes[M],I,Payload,N,Overlap,sizeof(Overlap),&Written)==MtkStpOk);
    CHECK(Written==N+6 && Equal(Wire,Overlap,Written));
  }
  /* Every split in a frame covers partial headers, payload, tail and padding. */
  for (M=0;M<3;M++) for (N=1;N<=32;N++) {
    MakeWire(2,N); Length=N+6;
    if (Modes[M]==16) while (Length&3U) Wire[Length++]=0;
    for (I=0;I<=Length;I++) {
      CHECK(MtkStpDecoderInit(&Decoder,Modes[M])==MtkStpOk);
      S=MtkStpReceive(&Decoder,Wire,I,&Count,&F); CHECK(Count==I);
      if (I==Length) CHECK(S==MtkStpFrameReady);
      else {
        CHECK(S==MtkStpNeedMore);
        CHECK(MtkStpReceive(&Decoder,Wire+I,Length-I,&Count,&F)==MtkStpFrameReady);
        CHECK(Count==Length-I && F.Length==N && Equal(F.Data,Payload,N));
      }
    }
  }
  /* Multiple frames in one physical read leave an exact cursor for the owner. */
  MakeWire(2,8);
  for (I=0;I<14;I++) Wire[14+I]=Wire[I];
  CHECK(MtkStpDecoderInit(&Decoder,8)==MtkStpOk);
  Offset=0;
  for (I=0;I<2;I++) {
    CHECK(MtkStpReceive(&Decoder,Wire+Offset,28-Offset,&Count,&F)==MtkStpFrameReady);
    CHECK(Count==14 && F.Channel==2 && F.Length==8 && Equal(F.Data,Payload,8));
    Offset+=Count;
  }
  for (J=0;J<6;J++) {
    MakeWire(2,3);
    I=J<4?J:J+3; Wire[I]=J==0?0x81:J==1?0x80:J==2?0xff:1;
    if (J==2) Wire[1]=0x27; /* 2047-byte header exceeds stock receive bound. */
    CHECK(MtkStpDecoderInit(&Decoder,8)==MtkStpOk); F.Channel=99;
    CHECK(MtkStpReceive(&Decoder,Wire,9,&Count,&F)==MtkStpMalformed);
    CHECK(Count==I+1 && F.Channel==99 && Decoder.Faulted==1);
    CHECK(MtkStpReceive(&Decoder,Wire,9,&Count,&F)==MtkStpFaulted && Count==0);
  }
  CHECK(MtkStpDecoderInit(&Decoder,8)==MtkStpOk);
  Decoder.State=8;
  CHECK(MtkStpReceive(&Decoder,Wire,9,&Count,&F)==MtkStpMalformed && Count==0);
  /* Corrupted payload cursors must fail before touching the receive buffer. */
  for (I=0;I<2;I++) {
    CHECK(MtkStpDecoderInit(&Decoder,8)==MtkStpOk);
    Decoder.State=4; Decoder.Length=Decoder.Used=I?MTK_STP_RX_MAX:0;
    CHECK(MtkStpReceive(&Decoder,Wire,1,&Count,&F)==MtkStpMalformed && Count==0);
    CHECK(Decoder.Faulted==1);
  }
  Written=99; MakeWire(2,32);
  CHECK(MtkStpEncode(8,8,Payload,32,Overlap,sizeof(Overlap),&Written)==MtkStpInvalid && Written==99);
  CHECK(MtkStpEncode(8,2,Payload,0,Overlap,sizeof(Overlap),&Written)==MtkStpInvalid);
  CHECK(MtkStpEncode(8,2,Payload,1997,Overlap,sizeof(Overlap),&Written)==MtkStpInvalid);
  CHECK(MtkStpEncode(8,2,Payload,32,Overlap,37,&Written)==MtkStpInvalid);
  CHECK(MtkStpEncode(1,2,Payload,32,Overlap,sizeof(Overlap),&Written)==MtkStpUnsupported);
  CHECK(MtkStpEncode(8,2,0,32,Overlap,sizeof(Overlap),&Written)==MtkStpInvalid);
  CHECK(MtkStpEncode(8,2,Payload,32,0,sizeof(Overlap),&Written)==MtkStpInvalid);
  CHECK(MtkStpEncode(8,2,Payload,32,Overlap,sizeof(Overlap),0)==MtkStpInvalid);
  for (I=0;I<=12;I++) for (J=0;J<=12;J++) {
    for (K=0;K<32;K++) Overlap[J+K]=Payload[K];
    CHECK(MtkStpEncode(8,2,Overlap+J,32,Overlap+I,sizeof(Overlap)-I,&Written)==MtkStpOk);
    CHECK(Written==38 && Equal(Overlap+I,Wire,38));
  }
  CHECK(MtkStpTransmitterInit(0,8,0,Write)==MtkStpInvalid);
  CHECK(MtkStpTransmitterInit(&Transmitter,8,0,0)==MtkStpInvalid);
  CHECK(MtkStpTransmitterInit(&Transmitter,1,0,Write)==MtkStpUnsupported);
  CHECK(MtkStpTransmit(0,2,Payload,1)==MtkStpInvalid);
  for (I=0;I<4;I++) {
    Calls=0; FailWritten=0xffffffffU; WriteStatus=MtkStpOk;
    CHECK(MtkStpTransmitterInit(&Transmitter,8,0,Write)==MtkStpOk);
    CHECK(MtkStpTransmit(&Transmitter,2,Payload,32)==MtkStpOk && Calls==1);
    MakeWire(2,32); CHECK(Equal(Transmitter.Buffer,Wire,38));
    CHECK(MtkStpTransmit(&Transmitter,2,Payload,0)==MtkStpInvalid && Calls==1);
    FailWritten=I==0?0:I==1?37:I==2?39:38;
    WriteStatus=I==3?MtkStpIoError:MtkStpOk;
    CHECK(MtkStpTransmit(&Transmitter,2,Payload,32)==MtkStpIoError && Calls==2);
    CHECK(Transmitter.Faulted==1);
    CHECK(MtkStpTransmit(&Transmitter,2,Payload,32)==MtkStpFaulted && Calls==2);
  }
  Result=WmtControlTest(); CHECK(Result==0);
  Result=McuTest(); CHECK(Result==0);
  Result=RegisterTest(); CHECK(Result==0);
  Result=QueueTest(); CHECK(Result==0);
  return 0;
}
#ifdef STP_TEST_NATIVE
int main(void) {
  int Line=TestMain();
  if (Line) { fprintf(stderr,"STP assertion failed at line %d\n",Line); return 1; }
  printf("STP: %u assertions passed (ASAN/UBSAN)\n",MtkStpAssertionCount); return 0;
}
#endif
