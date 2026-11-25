// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkStpCore.h>
#include <stdint.h>

static int ValidMode(unsigned Mode) {
  return Mode==MTK_STP_UART_MANDATORY || Mode==MTK_STP_BTIF_MANDATORY ||
         Mode==MTK_STP_SDIO;
}
MTK_STP_STATUS MtkStpDecoderInit(MTK_STP_DECODER *D,unsigned Mode) {
  if (!D) return MtkStpInvalid;
  if (!ValidMode(Mode)) return MtkStpUnsupported;
  D->Mode=Mode; D->State=D->Length=D->Used=D->Channel=D->Padding=D->Faulted=0;
  return MtkStpOk;
}
MTK_STP_STATUS MtkStpReceive(MTK_STP_DECODER *D,const unsigned char *Data,
  unsigned Length,unsigned *Consumed,MTK_STP_FRAME *Frame) {
  unsigned I=0,Byte;
  if (!Consumed) return MtkStpInvalid;
  *Consumed=0;
  if (!D || !Frame || (!Data && Length)) return MtkStpInvalid;
  if (!ValidMode(D->Mode)) return MtkStpUnsupported;
  if (D->Faulted) return MtkStpFaulted;
  if (D->State>7 || D->Used>D->Length || D->Length>MTK_STP_RX_MAX ||
      D->Channel>7 || D->Padding>3 ||
      (D->State==4 && D->Used>=D->Length)) {
    D->Faulted=1; return MtkStpMalformed;
  }
  while (I<Length) {
    Byte=Data[I++];
    switch (D->State) {
    case 0:
      if (Byte!=0x80) goto Malformed;
      D->Used=D->Length=0; D->State=1; break;
    case 1:
      if (Byte&0x80) goto Malformed;
      D->Channel=Byte>>4; D->Length=(Byte&15U)<<8; D->State=2; break;
    case 2:
      D->Length|=Byte;
      if (D->Length>MTK_STP_RX_MAX) goto Malformed;
      D->State=3; break;
    case 3:
      if (Byte) goto Malformed;
      D->State=D->Length?4U:5U; break;
    case 4:
      D->Payload[D->Used++]=(unsigned char)Byte;
      if (D->Used==D->Length) D->State=5;
      break;
    case 5:
      if (Byte) goto Malformed;
      D->State=6; break;
    case 6:
      if (Byte) goto Malformed;
      D->Padding=D->Mode==MTK_STP_SDIO?((0U-D->Length-6U)&3U):0;
      D->State=7;
      if (D->Padding) break;
      goto Ready;
    case 7:
      if (!D->Padding) goto Malformed;
      if (--D->Padding) break;
      goto Ready;
    }
  }
  *Consumed=I; return MtkStpNeedMore;
Ready:
  Frame->Channel=D->Channel; Frame->Length=D->Length; Frame->Data=D->Payload;
  D->State=0; *Consumed=I; return MtkStpFrameReady;
Malformed:
  D->Faulted=1; *Consumed=I; return MtkStpMalformed;
}
MTK_STP_STATUS MtkStpEncode(unsigned Mode,unsigned Channel,
  const unsigned char *Data,unsigned Length,unsigned char *Output,
  unsigned Capacity,unsigned *Written) {
  unsigned I;
  if (!ValidMode(Mode)) return MtkStpUnsupported;
  if (!Data || !Output || !Written || !Length || Length>MTK_STP_TX_MAX ||
      Channel>7 || Capacity<Length+6U) return MtkStpInvalid;
  /* Move the body first so an in-place caller does not lose its first bytes. */
  if ((uintptr_t)(Output+4)>(uintptr_t)Data) {
    for (I=Length;I;I--) Output[I+3]=Data[I-1];
  } else {
    for (I=0;I<Length;I++) Output[I+4]=Data[I];
  }
  Output[0]=0x80; Output[1]=(unsigned char)((Channel<<4)|(Length>>8));
  Output[2]=(unsigned char)Length; Output[3]=0;
  Output[Length+4]=Output[Length+5]=0;
  *Written=Length+6; return MtkStpOk;
}
MTK_STP_STATUS MtkStpTransmitterInit(MTK_STP_TRANSMITTER *T,unsigned Mode,
  void *Context,MTK_STP_WRITE Write) {
  if (!T || !Write) return MtkStpInvalid;
  if (!ValidMode(Mode)) return MtkStpUnsupported;
  T->Mode=Mode; T->Context=Context; T->Write=Write; T->Faulted=0;
  return MtkStpOk;
}
MTK_STP_STATUS MtkStpTransmit(MTK_STP_TRANSMITTER *T,unsigned Channel,
  const unsigned char *Data,unsigned Length) {
  unsigned Bytes,Written=0;
  MTK_STP_STATUS Status;
  if (!T || !T->Write) return MtkStpInvalid;
  if (T->Faulted) return MtkStpFaulted;
  Status=MtkStpEncode(T->Mode,Channel,Data,Length,T->Buffer,sizeof(T->Buffer),&Bytes);
  if (Status!=MtkStpOk) return Status;
  Status=T->Write(T->Context,T->Buffer,Bytes,&Written);
  /* Full is retryable only when the backend guarantees zero acceptance. */
  if (Status==MtkStpFull && !Written) return MtkStpFull;
  if (Status!=MtkStpOk || Written!=Bytes) {
    T->Faulted=1; return MtkStpIoError;
  }
  return MtkStpOk;
}

static void WmtClearReply(MTK_WMT_CONTROL *C) {
  unsigned I;
  volatile unsigned char *P=C->Reply;
  for (I=0;I<MTK_WMT_GPS_MCU_RX_MAX;I++) P[I]=0;
  C->ReplyLength=0;
  C->RegisterWrite=C->RegisterAddress=C->RegisterValue=0;
}
MTK_WMT_STATUS MtkWmtControlInit(MTK_WMT_CONTROL *C,MTK_STP_TRANSMITTER *T) {
  if (!C || !T || !T->Write || !ValidMode(T->Mode) || T->Faulted)
    return MtkWmtInvalid;
  C->Transmitter=T; C->Pending=C->Faulted=C->Confirmed=0;
  C->Function=C->Enable=C->Opcode=C->ReplyCapacity=C->GpsPowerSave=0;
  C->FirmwareStatus=255; WmtClearReply(C);
  return MtkWmtOk;
}
MTK_WMT_STATUS MtkWmtControlBegin(MTK_WMT_CONTROL *C,unsigned Function,
  unsigned Enable) {
  unsigned char Command[6]={1,6,2,0,0,0};
  MTK_STP_STATUS S;
  if (!C || !C->Transmitter || Function>255 || Enable>1) return MtkWmtInvalid;
  if (C->Faulted || C->Transmitter->Faulted) return MtkWmtUncertain;
  if (C->Pending) return MtkWmtBusy;
  Command[4]=(unsigned char)Function; Command[5]=(unsigned char)Enable;
  C->Function=Function; C->Enable=Enable; C->Pending=1;
  C->Opcode=6; C->ReplyCapacity=C->GpsPowerSave=0; WmtClearReply(C);
  C->Confirmed=0; C->FirmwareStatus=255;
  S=MtkStpTransmit(C->Transmitter,MTK_STP_WMT_CHANNEL,Command,sizeof(Command));
  if (S==MtkStpFull) { C->Pending=0; return MtkWmtBusy; }
  if (S!=MtkStpOk) { C->Pending=0; C->Faulted=1; return MtkWmtUncertain; }
  return MtkWmtPending;
}
static unsigned WmtGet32(const unsigned char *P) {
  return (unsigned)P[0]|((unsigned)P[1]<<8)|((unsigned)P[2]<<16)|
         ((unsigned)P[3]<<24);
}
static void WmtPut32(unsigned char *P,unsigned Value) {
  unsigned I; for (I=0;I<4;I++) P[I]=(unsigned char)(Value>>(I*8U));
}
MTK_WMT_STATUS MtkWmtRegisterBegin(MTK_WMT_CONTROL *C,unsigned Write,
  unsigned Address,unsigned Value,unsigned Mask) {
  MTK_STP_STATUS S;
  unsigned char Command[20]={1,8,16,0,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0};
  if (!C || !C->Transmitter || Write>1) return MtkWmtInvalid;
  if (C->Faulted || C->Transmitter->Faulted) return MtkWmtUncertain;
  if (C->Pending) return MtkWmtBusy;
  if (!C->Transmitter->Write || !ValidMode(C->Transmitter->Mode)) return MtkWmtInvalid;
  Command[4]=(unsigned char)(Write?1:2);
  WmtPut32(Command+8,Address); WmtPut32(Command+12,Value); WmtPut32(Command+16,Mask);
  C->Function=C->Enable=C->ReplyCapacity=C->GpsPowerSave=0; WmtClearReply(C);
  C->RegisterWrite=Write; C->RegisterAddress=Address;
  C->Opcode=MTK_WMT_REGISTER_OPCODE; C->Pending=1; C->Confirmed=0; C->FirmwareStatus=255;
  S=MtkStpTransmit(C->Transmitter,MTK_STP_WMT_CHANNEL,Command,sizeof(Command));
  if (S==MtkStpFull) { C->Pending=0; return MtkWmtBusy; }
  if (S!=MtkStpOk) {
    C->Pending=0; C->Faulted=1; return MtkWmtUncertain;
  }
  return MtkWmtPending;
}
MTK_WMT_STATUS MtkWmtGpsSyncBegin(MTK_WMT_CONTROL *C,unsigned SocPinFlags,
  unsigned Enable) {
  if (!C || !C->Transmitter || SocPinFlags>255 || Enable>1) return MtkWmtInvalid;
  if (C->Faulted || C->Transmitter->Faulted) return MtkWmtUncertain;
  if (C->Pending) return MtkWmtBusy;
  if (!(SocPinFlags&MTK_WMT_SOC_GPS_SYNC_FLAG)) return MtkWmtUnsupported;
  return MtkWmtRegisterBegin(C,1,0x80050078U,Enable?0x10000000U:0x50000000U,0x70000000U);
}
MTK_WMT_STATUS MtkWmtControlEvent(MTK_WMT_CONTROL *C,const MTK_STP_FRAME *F) {
  if (!C || !C->Transmitter || !F || (!F->Data && F->Length)) return MtkWmtInvalid;
  if (C->Faulted || C->Transmitter->Faulted) return MtkWmtUncertain;
  if (F->Channel!=MTK_STP_WMT_CHANNEL) return MtkWmtUnrelated;
  if (F->Length>=2 && F->Data[1]!=6 && F->Data[1]!=MTK_WMT_REGISTER_OPCODE &&
      F->Data[1]!=MTK_WMT_GPS_MCU_OPCODE) return MtkWmtUnrelated;
  if (!C->Pending) {
    /* An unowned reply could otherwise complete the next unrelated command. */
    C->Faulted=1; C->Confirmed=0; C->RegisterValue=0; return MtkWmtUncertain;
  }
  C->Pending=0; C->Faulted=1;
  if (F->Length<2 || F->Data[0]!=2 || F->Data[1]!=C->Opcode)
    return MtkWmtMalformed;
  if (C->Opcode==MTK_WMT_GPS_MCU_OPCODE) {
    unsigned I,Length;
    if (F->Length<4 || F->Length>MTK_STP_RX_MAX ||
        !C->ReplyCapacity || C->ReplyCapacity>MTK_WMT_GPS_MCU_RX_MAX)
      return MtkWmtMalformed;
    Length=(unsigned)F->Data[2]|((unsigned)F->Data[3]<<8);
    if (Length+4U!=F->Length || Length>C->ReplyCapacity ||
        (C->GpsPowerSave && Length<2)) return MtkWmtMalformed;
    for (I=0;I<Length;I++) C->Reply[I]=F->Data[I+4];
    C->ReplyLength=Length;
    if (C->GpsPowerSave) {
      C->FirmwareStatus=C->Reply[0];
      if (C->FirmwareStatus) return MtkWmtRejected;
    }
  } else if (C->Opcode==MTK_WMT_REGISTER_OPCODE) {
    unsigned Length=C->RegisterWrite?8U:16U;
    if (C->RegisterWrite>1 || F->Length!=Length ||
        F->Data[2]!=Length-4U || F->Data[3]!=0) return MtkWmtMalformed;
    C->FirmwareStatus=F->Data[4];
    if (C->FirmwareStatus) return MtkWmtRejected;
    if (!C->RegisterWrite) {
      if (WmtGet32(F->Data+8)!=C->RegisterAddress) return MtkWmtMalformed;
      C->RegisterValue=WmtGet32(F->Data+12);
    }
  } else {
    if (C->Opcode!=6 || F->Length!=5 || F->Data[2]!=1 || F->Data[3]!=0)
      return MtkWmtMalformed;
    C->FirmwareStatus=F->Data[4];
    if (C->FirmwareStatus) return MtkWmtRejected;
  }
  C->Faulted=0; C->Confirmed=1; return MtkWmtOk;
}
MTK_WMT_STATUS MtkWmtControlExpire(MTK_WMT_CONTROL *C) {
  if (!C || !C->Transmitter) return MtkWmtInvalid;
  if (C->Faulted || C->Transmitter->Faulted) return MtkWmtUncertain;
  if (!C->Pending) return MtkWmtInvalid;
  C->Pending=0; C->Faulted=1; C->Confirmed=0; C->RegisterValue=0;
  return MtkWmtUncertain;
}

MTK_WMT_STATUS MtkWmtGpsMcuBegin(MTK_WMT_CONTROL *C,
  const unsigned char *Data,unsigned Length,unsigned ReplyCapacity) {
  unsigned I;
  MTK_STP_TRANSMITTER *T;
  MTK_STP_STATUS S;
  if (!C || !C->Transmitter || !Data || !Length ||
      Length>MTK_WMT_GPS_MCU_TX_MAX || !ReplyCapacity ||
      ReplyCapacity>MTK_WMT_GPS_MCU_RX_MAX) return MtkWmtInvalid;
  T=C->Transmitter;
  if (C->Faulted || T->Faulted) return MtkWmtUncertain;
  if (C->Pending) return MtkWmtBusy;
  if (!T->Write || !ValidMode(T->Mode)) return MtkWmtInvalid;
  /* Stage the WMT body before its header, allowing caller/TX-buffer overlap. */
  if ((uintptr_t)(T->Buffer+4)>(uintptr_t)Data) {
    for (I=Length;I;I--) T->Buffer[I+3]=Data[I-1];
  } else for (I=0;I<Length;I++) T->Buffer[I+4]=Data[I];
  T->Buffer[0]=1; T->Buffer[1]=MTK_WMT_GPS_MCU_OPCODE;
  T->Buffer[2]=(unsigned char)Length; T->Buffer[3]=(unsigned char)(Length>>8);
  C->Opcode=MTK_WMT_GPS_MCU_OPCODE; C->ReplyCapacity=ReplyCapacity;
  C->Function=MTK_WMT_GPS_FUNCTION; C->Enable=C->GpsPowerSave=0;
  C->Pending=1; C->Confirmed=0; C->FirmwareStatus=255; WmtClearReply(C);
  S=MtkStpTransmit(T,MTK_STP_WMT_CHANNEL,T->Buffer,Length+4);
  if (S==MtkStpFull) { C->Pending=0; return MtkWmtBusy; }
  if (S!=MtkStpOk) {
    C->Pending=0; C->Faulted=1; return MtkWmtUncertain;
  }
  return MtkWmtPending;
}
MTK_WMT_STATUS MtkWmtGpsPowerSaveBegin(MTK_WMT_CONTROL *C,
  unsigned Suspend,unsigned FirmwareControl) {
  unsigned char Data[2];
  MTK_WMT_STATUS S;
  if (Suspend>1 || FirmwareControl>255) return MtkWmtInvalid;
  Data[0]=(unsigned char)(Suspend?4:5); Data[1]=(unsigned char)FirmwareControl;
  S=MtkWmtGpsMcuBegin(C,Data,2,3);
  if (S==MtkWmtPending) { C->GpsPowerSave=1; C->Enable=Suspend; }
  return S;
}
static void QueueClear(MTK_STP_QUEUE *Q) {
  unsigned I;
  /* Volatile stores also remove consumed/reset location bytes in release code. */
  volatile unsigned char *P=Q->Data;
  for (I=0;I<MTK_STP_QUEUE_BYTES;I++) P[I]=0;
  Q->Head=Q->Tail=Q->Count=0;
}
static MTK_STP_STATUS QueueState(MTK_STP_QUEUE *Q,unsigned Epoch) {
  if (!Q || !Q->Epoch || Q->Channel>7) return MtkStpInvalid;
  if (Epoch!=Q->Epoch) return MtkStpStale;
  if (Q->Faulted) return MtkStpFaulted;
  if (Q->Head>=MTK_STP_QUEUE_BYTES || Q->Tail>=MTK_STP_QUEUE_BYTES ||
      Q->Count>MTK_STP_QUEUE_CAPACITY ||
      ((Q->Tail-Q->Head)&MTK_STP_QUEUE_CAPACITY)!=Q->Count) {
    Q->Faulted=1; return MtkStpMalformed;
  }
  return MtkStpOk;
}
MTK_STP_STATUS MtkStpQueueInit(MTK_STP_QUEUE *Q,unsigned Channel) {
  if (!Q || Channel>7) return MtkStpInvalid;
  QueueClear(Q); Q->Channel=Channel; Q->Epoch=1; Q->Faulted=0;
  return MtkStpOk;
}
MTK_STP_STATUS MtkStpQueueReset(MTK_STP_QUEUE *Q,unsigned *Epoch) {
  if (!Q || !Epoch || !Q->Epoch || Q->Channel>7) return MtkStpInvalid;
  QueueClear(Q);
  if (Q->Epoch==0xffffffffU) { Q->Faulted=1; return MtkStpFaulted; }
  Q->Epoch++; Q->Faulted=0; *Epoch=Q->Epoch;
  return MtkStpOk;
}
MTK_STP_STATUS MtkStpQueuePush(MTK_STP_QUEUE *Q,unsigned Epoch,
  const MTK_STP_FRAME *F) {
  unsigned I;
  MTK_STP_STATUS S=QueueState(Q,Epoch);
  if (S!=MtkStpOk) return S;
  if (!F || (!F->Data && F->Length) || F->Length>MTK_STP_RX_MAX) return MtkStpInvalid;
  if (F->Channel!=Q->Channel) return MtkStpUnsupported;
  if (F->Length>MTK_STP_QUEUE_CAPACITY-Q->Count) {
    QueueClear(Q); Q->Faulted=1; return MtkStpFull;
  }
  for (I=0;I<F->Length;I++) {
    Q->Data[Q->Tail]=F->Data[I];
    Q->Tail=(Q->Tail+1U)&MTK_STP_QUEUE_CAPACITY;
  }
  Q->Count+=F->Length; return MtkStpOk;
}
MTK_STP_STATUS MtkStpQueueRead(MTK_STP_QUEUE *Q,unsigned Epoch,
  unsigned char *Data,unsigned Capacity,unsigned *Read) {
  unsigned I,Count;
  MTK_STP_STATUS S;
  volatile unsigned char *P;
  if (!Read) return MtkStpInvalid;
  *Read=0; S=QueueState(Q,Epoch);
  if (S!=MtkStpOk) return S;
  if (!Data && Capacity) return MtkStpInvalid;
  if (!Capacity) return MtkStpOk;
  if (!Q->Count) return MtkStpNeedMore;
  Count=Capacity<Q->Count?Capacity:Q->Count; P=Q->Data;
  for (I=0;I<Count;I++) {
    Data[I]=Q->Data[Q->Head]; P[Q->Head]=0;
    Q->Head=(Q->Head+1U)&MTK_STP_QUEUE_CAPACITY;
  }
  Q->Count-=Count; *Read=Count; return MtkStpOk;
}
