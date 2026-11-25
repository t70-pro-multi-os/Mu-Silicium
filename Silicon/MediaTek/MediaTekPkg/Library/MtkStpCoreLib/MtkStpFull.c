// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkStpFull.h>

#define SEQUENCE_MASK 7U
#define HEADER_BYTES 4U
#define SYNC_BYTE 0x7fU

static unsigned char HeaderSum(const unsigned char *Header) {
  return (unsigned char)(Header[0]+Header[1]+Header[2]);
}

static unsigned PayloadLength(const unsigned char *Header) {
  return ((unsigned)(Header[1]&15U)<<8)|Header[2];
}

/* osal_crc16 in the shipped wmt_drv.ko: reflected CRC-16/ARC, initial zero. */
static unsigned PayloadCrc(const unsigned char *Data,unsigned Length) {
  unsigned Crc=0,Bit;
  while (Length--) {
    Crc^=*Data++;
    for (Bit=0;Bit<8;Bit++)
      Crc=(Crc>>1)^((Crc&1U)?0xa001U:0U);
  }
  return Crc;
}

static unsigned RxAck(const MTK_STP_FULL_LINK *Link) {
  return (Link->RxNext-1U)&SEQUENCE_MASK;
}

static MTK_STP_STATUS WriteFrame(MTK_STP_FULL_LINK *Link,
  const unsigned char *Data,unsigned Length) {
  unsigned Written=0;
  MTK_STP_STATUS Status=Link->Write(Link->Context,Data,Length,&Written);
  if (Status==MtkStpFull && !Written) return MtkStpFull;
  if (Status!=MtkStpOk || Written!=Length) {
    Link->Faulted=1;
    return MtkStpIoError;
  }
  Link->AckPending=0;
  return MtkStpOk;
}

MTK_STP_STATUS MtkStpFullInit(MTK_STP_FULL_LINK *Link,void *Context,
  MTK_STP_WRITE Write) {
  unsigned I;
  if (!Link || !Write) return MtkStpInvalid;
  Link->Context=Context;
  Link->Write=Write;
  Link->FirstSendMs=Link->LastSendMs=0;
  Link->TxNext=Link->TxLength=0;
  Link->RxNext=Link->AckPending=Link->Faulted=0;
  Link->HeaderUsed=Link->BodyUsed=Link->Resync=Link->SyncUsed=0;
  Link->OfferedLength=0;
  for (I=0;I<MTK_STP_FULL_SYNC_BYTES;I++) Link->Tx[I]=SYNC_BYTE;
  return MtkStpOk;
}

MTK_STP_STATUS MtkStpFullTransmit(MTK_STP_FULL_LINK *Link,unsigned Channel,
  const unsigned char *Data,unsigned Length,uint64_t NowMs) {
  unsigned char *Wire;
  unsigned I,Crc;
  MTK_STP_STATUS Status;
  if (!Link || !Link->Write || !Data || !Length || Length>MTK_STP_TX_MAX ||
      Channel>7 || Channel==5 || Channel==6) return MtkStpInvalid;
  if (Link->Faulted) return MtkStpFaulted;
  if (Link->TxLength) return MtkStpFull;
  Wire=Link->Tx+MTK_STP_FULL_SYNC_BYTES;
  Wire[0]=(unsigned char)(0x80U|(Link->TxNext<<3)|RxAck(Link));
  Wire[1]=(unsigned char)((Channel<<4)|(Length>>8));
  Wire[2]=(unsigned char)Length;
  Wire[3]=HeaderSum(Wire);
  for (I=0;I<Length;I++) Wire[HEADER_BYTES+I]=Data[I];
  Crc=PayloadCrc(Data,Length);
  Wire[HEADER_BYTES+Length]=(unsigned char)Crc;
  Wire[HEADER_BYTES+Length+1U]=(unsigned char)(Crc>>8);
  Status=WriteFrame(Link,Wire,Length+6U);
  if (Status==MtkStpOk) {
    Link->TxLength=Length+6U;
    Link->FirstSendMs=Link->LastSendMs=NowMs;
  }
  return Status;
}

MTK_STP_STATUS MtkStpFullService(MTK_STP_FULL_LINK *Link,uint64_t NowMs) {
  unsigned char *Wire;
  unsigned char Ack[HEADER_BYTES];
  MTK_STP_STATUS Status;
  if (!Link || !Link->Write) return MtkStpInvalid;
  if (Link->Faulted) return MtkStpFaulted;
  /* Queue pressure uses the same absolute limit as a silent peer. */
  if (Link->TxLength &&
      NowMs-Link->FirstSendMs>=MTK_STP_FULL_ACK_MS*(MTK_STP_FULL_RETRIES+1U)) {
    Link->Faulted=1;
    return MtkStpFaulted;
  }
  if (Link->TxLength && NowMs-Link->LastSendMs>=MTK_STP_FULL_ACK_MS) {
    Wire=Link->Tx+MTK_STP_FULL_SYNC_BYTES;
    Wire[0]=(unsigned char)((Wire[0]&~SEQUENCE_MASK)|RxAck(Link));
    Wire[3]=HeaderSum(Wire);
    Status=WriteFrame(Link,Link->Tx,MTK_STP_FULL_SYNC_BYTES+Link->TxLength);
    if (Status==MtkStpOk) Link->LastSendMs=NowMs;
    return Status;
  }
  if (!Link->AckPending) return MtkStpOk;
  Ack[0]=Ack[3]=(unsigned char)(0x80U|RxAck(Link));
  Ack[1]=Ack[2]=0;
  return WriteFrame(Link,Ack,sizeof(Ack));
}

static void ConsumeAck(MTK_STP_FULL_LINK *Link) {
  if (Link->TxLength && (Link->Header[0]&SEQUENCE_MASK)==Link->TxNext) {
    Link->TxLength=0;
    Link->TxNext=(Link->TxNext+1U)&SEQUENCE_MASK;
  }
}

MTK_STP_STATUS MtkStpFullReceive(MTK_STP_FULL_LINK *Link,
  const unsigned char *Data,unsigned Length,unsigned *Consumed,
  MTK_STP_FRAME *Frame) {
  unsigned Position,Byte,Size,Channel,ReceivedCrc,I;
  if (!Consumed) return MtkStpInvalid;
  *Consumed=0;
  if (!Link || !Link->Write || (!Data && Length) || !Frame) return MtkStpInvalid;
  if (Link->Faulted) return MtkStpFaulted;
  if (Link->OfferedLength) return MtkStpFull;
  for (Position=0;Position<Length;Position++) {
    Byte=Data[Position];
    if (Link->HeaderUsed<HEADER_BYTES) {
      if (Link->Resync || (!Link->HeaderUsed && Byte==SYNC_BYTE)) {
        Link->Resync=1;
        Link->SyncUsed=Byte==SYNC_BYTE?Link->SyncUsed+1U:0;
        if (Link->SyncUsed==MTK_STP_FULL_SYNC_BYTES)
          Link->Resync=Link->SyncUsed=0;
        continue;
      }
      if (!Link->HeaderUsed && (Byte&0xc0U)!=0x80U) {
        Link->Resync=1;
        continue;
      }
      Link->Header[Link->HeaderUsed++]=(unsigned char)Byte;
      if (Link->HeaderUsed<HEADER_BYTES) continue;
      Size=PayloadLength(Link->Header);
      if (Size>MTK_STP_FULL_RX_MAX || HeaderSum(Link->Header)!=Byte) {
        Link->HeaderUsed=0;
        Link->Resync=1;
        /* A retry marker can begin inside a truncated header. */
        for (I=HEADER_BYTES;I && Link->Header[I-1U]==SYNC_BYTE;I--)
          Link->SyncUsed++;
        continue;
      }
      Channel=(Link->Header[1]>>4)&7U;
      /* Channels 5/6 carry firmware management and assertion data. */
      if (Channel==5 || Channel==6 || (!Size && Channel)) {
        *Consumed=Position+1U;
        Link->Faulted=1;
        return MtkStpFaulted;
      }
      if (!Size) {
        ConsumeAck(Link);
        Link->HeaderUsed=0;
      }
      continue;
    }

    Size=PayloadLength(Link->Header);
    Link->Body[Link->BodyUsed++]=(unsigned char)Byte;
    if (Link->BodyUsed<Size+2U) continue;
    Link->HeaderUsed=Link->BodyUsed=0;
    ReceivedCrc=Link->Body[Size]|((unsigned)Link->Body[Size+1U]<<8);
    if (ReceivedCrc!=PayloadCrc(Link->Body,Size)) continue;
    if (((Link->Header[0]>>3)&SEQUENCE_MASK)!=Link->RxNext) {
      Link->AckPending=1;
      continue;
    }
    ConsumeAck(Link);
    Link->OfferedLength=Size;
    Frame->Channel=(Link->Header[1]>>4)&7U;
    Frame->Length=Size;
    Frame->Data=Link->Body;
    *Consumed=Position+1U;
    return MtkStpFrameReady;
  }
  *Consumed=Position;
  return MtkStpNeedMore;
}

MTK_STP_STATUS MtkStpFullAccept(MTK_STP_FULL_LINK *Link) {
  if (!Link || !Link->Write) return MtkStpInvalid;
  if (Link->Faulted) return MtkStpFaulted;
  if (!Link->OfferedLength) return MtkStpInvalid;
  Link->OfferedLength=0;
  Link->RxNext=(Link->RxNext+1U)&SEQUENCE_MASK;
  Link->AckPending=1;
  return MtkStpOk;
}


MTK_WMT_STATUS MtkWmtFullControlInit(MTK_WMT_FULL_CONTROL *Control,
  MTK_STP_FULL_LINK *Link) {
  if (!Control || !Link || !Link->Write) return MtkWmtInvalid;
  Control->Link=Link; Control->SinceMs=0; Control->Pending=0;
  Control->Faulted=0; Control->Confirmed=0; Control->Function=0;
  Control->Enable=0; Control->FirmwareStatus=255;
  return MtkWmtOk;
}

MTK_WMT_STATUS MtkWmtFullControlBegin(MTK_WMT_FULL_CONTROL *Control,
  unsigned Function,unsigned Enable,uint64_t NowMs) {
  unsigned char Command[6]={1,6,2,0,0,0};
  MTK_STP_STATUS Status;
  if (!Control || !Control->Link || !Control->Link->Write ||
      Function>255U || Enable>1U) return MtkWmtInvalid;
  if (Control->Faulted || Control->Link->Faulted) return MtkWmtUncertain;
  if (Control->Pending) return MtkWmtBusy;
  Command[4]=(unsigned char)Function; Command[5]=(unsigned char)Enable;
  Control->Function=Function; Control->Enable=Enable; Control->Pending=1;
  Control->Confirmed=0; Control->FirmwareStatus=255; Control->SinceMs=NowMs;
  Status=MtkStpFullTransmit(Control->Link,MTK_STP_WMT_CHANNEL,Command,
                            sizeof(Command),NowMs);
  if (Status==MtkStpFull) { Control->Pending=0; return MtkWmtBusy; }
  if (Status!=MtkStpOk) {
    Control->Pending=0; Control->Faulted=1; return MtkWmtUncertain;
  }
  return MtkWmtPending;
}

MTK_WMT_STATUS MtkWmtFullControlEvent(MTK_WMT_FULL_CONTROL *Control,
  const MTK_STP_FRAME *Frame) {
  if (!Control || !Control->Link || !Frame || (!Frame->Data && Frame->Length))
    return MtkWmtInvalid;
  if (Control->Faulted || Control->Link->Faulted) return MtkWmtUncertain;
  if (Frame->Channel!=MTK_STP_WMT_CHANNEL) return MtkWmtUnrelated;
  if (Frame->Length>=2U && Frame->Data[1]!=6U) return MtkWmtUnrelated;
  if (!Control->Pending) {
    Control->Faulted=1; Control->Confirmed=0;
    return MtkWmtUncertain;
  }
  Control->Pending=0; Control->Faulted=1; Control->Confirmed=0;
  if (Frame->Length!=5U || Frame->Data[0]!=2U || Frame->Data[1]!=6U ||
      Frame->Data[2]!=1U || Frame->Data[3]!=0U) return MtkWmtMalformed;
  Control->FirmwareStatus=Frame->Data[4];
  if (Control->FirmwareStatus) return MtkWmtRejected;
  Control->Faulted=0; Control->Confirmed=1;
  return MtkWmtOk;
}

MTK_WMT_STATUS MtkWmtFullControlExpire(MTK_WMT_FULL_CONTROL *Control,
  uint64_t NowMs) {
  if (!Control || !Control->Link || !Control->Link->Write) return MtkWmtInvalid;
  if (Control->Faulted || Control->Link->Faulted) return MtkWmtUncertain;
  if (!Control->Pending) return MtkWmtInvalid;
  if (NowMs<Control->SinceMs) {
    Control->Pending=0; Control->Faulted=1; Control->Confirmed=0;
    return MtkWmtUncertain;
  }
  if (NowMs-Control->SinceMs<MTK_WMT_FULL_CONTROL_TIMEOUT_MS) return MtkWmtPending;
  Control->Pending=0; Control->Faulted=1; Control->Confirmed=0;
  return MtkWmtUncertain;
}
