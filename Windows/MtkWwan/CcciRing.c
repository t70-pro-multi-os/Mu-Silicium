/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2016 MediaTek Inc. CCCI ring format. */
#include "CcciRing.h"
#include <string.h>
#if defined(T70_CCCI_HOST_TEST)
#include <stdatomic.h>
#define T70_CCCI_MEMORY_BARRIER() atomic_thread_fence(memory_order_seq_cst)
#else
#include <ntddk.h>
#define T70_CCCI_MEMORY_BARRIER() KeMemoryBarrier()
#endif

#define CCCI_BUFFER_MAGIC 0xEE0000EEU
#define CCCI_BUFFER_END_MAGIC 0xFF0000FFU
#define CCIF_FRAME_MAGIC 0xAABBAABBU
#define CCIF_FRAME_END_MAGIC 0xCCDDEEFFU
#define CCIF_HEADER_SIZE 8U
#define CCIF_FOOTER_SIZE 8U
#define CCCI_HEADER_SIZE 16U

_Static_assert(sizeof(T70_CCCI_SHARED_RING)==24U,"CCCI shared control layout changed");

static uint32_t ReadLe32(const uint8_t *Bytes) {
  return (uint32_t)Bytes[0] | ((uint32_t)Bytes[1]<<8) |
         ((uint32_t)Bytes[2]<<16) | ((uint32_t)Bytes[3]<<24);
}

static void WriteLe32(uint8_t *Bytes,uint32_t Value) {
  Bytes[0]=(uint8_t)Value;Bytes[1]=(uint8_t)(Value>>8);
  Bytes[2]=(uint8_t)(Value>>16);Bytes[3]=(uint8_t)(Value>>24);
}

static uint16_t ReadLe16(const uint8_t *Bytes) {
  return (uint16_t)((uint16_t)Bytes[0]|((uint16_t)Bytes[1]<<8));
}

T70_CCCI_STATUS T70CcciParseMessageHeader(const void *Message,size_t MessageBytes,
                                           T70_CCCI_MESSAGE_HEADER *Header) {
  const uint8_t *Bytes=(const uint8_t *)Message;
  T70_CCCI_MESSAGE_HEADER Parsed;
  if(!Message || !Header)return T70CcciInvalidParameter;
  if(MessageBytes<CCCI_HEADER_SIZE)return T70CcciBufferTooSmall;
  Parsed.Data0=ReadLe32(Bytes);Parsed.Data1=ReadLe32(Bytes+4);
  Parsed.Channel=ReadLe16(Bytes+8);Parsed.SequenceAndAssert=ReadLe16(Bytes+10);
  Parsed.Reserved=ReadLe32(Bytes+12);
  *Header=Parsed;
  return T70CcciSuccess;
}

static int AddSize(size_t Left,size_t Right,size_t *Sum) {
  if(!Sum || Right>SIZE_MAX-Left)return 0;
  *Sum=Left+Right;
  return 1;
}

static int RequiredBufferSize(uint32_t ReceiveBytes,uint32_t TransmitBytes,size_t *Required) {
  size_t Size=T70_CCCI_RING_PREFIX_BYTES;
  return AddSize(Size,sizeof(T70_CCCI_SHARED_RING),&Size) &&
         AddSize(Size,ReceiveBytes,&Size) && AddSize(Size,TransmitBytes,&Size) &&
         AddSize(Size,T70_CCCI_RING_SUFFIX_BYTES,Required);
}

static void CopyIntoRing(volatile uint8_t *Ring,uint32_t Length,uint32_t Offset,
                         const void *Source,size_t Count) {
  const uint8_t *Bytes=(const uint8_t *)Source;
  size_t First=Count,I;
  if(First>(size_t)(Length-Offset))First=Length-Offset;
  for(I=0;I<First;I++)Ring[Offset+(uint32_t)I]=Bytes[I];
  for(I=First;I<Count;I++)Ring[I-First]=Bytes[I];
}

static void CopyFromRing(const volatile uint8_t *Ring,uint32_t Length,uint32_t Offset,
                         void *Destination,size_t Count) {
  uint8_t *Bytes=(uint8_t *)Destination;
  size_t First=Count,I;
  if(First>(size_t)(Length-Offset))First=Length-Offset;
  for(I=0;I<First;I++)Bytes[I]=Ring[Offset+(uint32_t)I];
  for(I=First;I<Count;I++)Bytes[I]=Ring[I-First];
}

static void ZeroRing(volatile uint8_t *Ring,uint32_t Length,uint32_t Offset,size_t Count) {
  size_t First=Count,I;
  if(First>(size_t)(Length-Offset))First=Length-Offset;
  for(I=0;I<First;I++)Ring[Offset+(uint32_t)I]=0U;
  for(I=First;I<Count;I++)Ring[I-First]=0U;
}

static T70_CCCI_STATUS PacketFrameBytes(uint32_t MessageBytes,size_t *FrameBytes) {
  uint64_t Aligned;
  if(!FrameBytes || MessageBytes<CCCI_HEADER_SIZE)return T70CcciInvalidParameter;
  Aligned=((uint64_t)MessageBytes+7U)&~UINT64_C(7);
  if(Aligned>SIZE_MAX-CCIF_HEADER_SIZE-CCIF_FOOTER_SIZE)
    return T70CcciPacketTooLarge;
  *FrameBytes=(size_t)Aligned+CCIF_HEADER_SIZE+CCIF_FOOTER_SIZE;
  return T70CcciSuccess;
}

typedef struct {
  uint32_t ReceiveRead,ReceiveWrite,ReceiveLength;
  uint32_t TransmitRead,TransmitWrite,TransmitLength;
  size_t Required;
  volatile uint8_t *ReceiveBuffer,*TransmitBuffer;
} T70_CCCI_RING_SNAPSHOT;

static uint32_t ReadSharedLe32(const volatile uint8_t *Bytes) {
  return (uint32_t)Bytes[0] | ((uint32_t)Bytes[1]<<8) |
         ((uint32_t)Bytes[2]<<16) | ((uint32_t)Bytes[3]<<24);
}

static T70_CCCI_STATUS SnapshotView(const T70_CCCI_RING_VIEW *View,
                                    T70_CCCI_RING_SNAPSHOT *Snapshot) {
  const volatile T70_CCCI_SHARED_RING *Ring;
  const volatile uint8_t *Base,*Tail;
  if(!View || !Snapshot || !View->Base || !View->Shared ||
     ((uintptr_t)View->Base&3U) ||
     View->Capacity<T70_CCCI_RING_PREFIX_BYTES+sizeof(T70_CCCI_SHARED_RING)+T70_CCCI_RING_SUFFIX_BYTES)
    return T70CcciInvalidParameter;
  if(View->Shared!=(T70_CCCI_SHARED_RING *)(View->Base+T70_CCCI_RING_PREFIX_BYTES))
    return T70CcciCorruptState;
  Ring=(const volatile T70_CCCI_SHARED_RING *)View->Shared;
  T70_CCCI_MEMORY_BARRIER();
  Snapshot->ReceiveRead=Ring->Receive.Read;
  Snapshot->ReceiveWrite=Ring->Receive.Write;
  Snapshot->ReceiveLength=Ring->Receive.Length;
  Snapshot->TransmitRead=Ring->Transmit.Read;
  Snapshot->TransmitWrite=Ring->Transmit.Write;
  Snapshot->TransmitLength=Ring->Transmit.Length;
  T70_CCCI_MEMORY_BARRIER();
  if(Snapshot->ReceiveLength<T70_CCCI_PACKET_MIN_BYTES ||
     Snapshot->TransmitLength<T70_CCCI_PACKET_MIN_BYTES ||
     !RequiredBufferSize(Snapshot->ReceiveLength,Snapshot->TransmitLength,&Snapshot->Required) ||
     Snapshot->Required>View->Capacity ||
     Snapshot->ReceiveRead>=Snapshot->ReceiveLength ||
     Snapshot->ReceiveWrite>=Snapshot->ReceiveLength ||
     Snapshot->TransmitRead>=Snapshot->TransmitLength ||
     Snapshot->TransmitWrite>=Snapshot->TransmitLength)
    return T70CcciCorruptState;
  Base=(const volatile uint8_t *)View->Base;
  Tail=Base+Snapshot->Required-T70_CCCI_RING_SUFFIX_BYTES;
  if(ReadSharedLe32(Base)!=CCCI_BUFFER_MAGIC ||
     ReadSharedLe32(Base+4)!=CCCI_BUFFER_MAGIC ||
     ReadSharedLe32(Tail)!=CCCI_BUFFER_END_MAGIC ||
     ReadSharedLe32(Tail+4)!=CCCI_BUFFER_END_MAGIC)
    return T70CcciCorruptState;
  Snapshot->ReceiveBuffer=(volatile uint8_t *)Ring->Buffer;
  Snapshot->TransmitBuffer=(volatile uint8_t *)(Ring->Buffer+Snapshot->ReceiveLength);
  return T70CcciSuccess;
}

static T70_CCCI_STATUS RingUsed(uint32_t Read,uint32_t Write,uint32_t Length,
                                uint32_t *Used) {
  if(!Used || !Length || Read>=Length || Write>=Length)return T70CcciCorruptState;
  *Used=Write>=Read?Write-Read:Length-Read+Write;
  if(*Used>=Length)return T70CcciCorruptState;
  return T70CcciSuccess;
}

T70_CCCI_STATUS T70CcciAdvanceRxSequence(uint16_t Previous,uint16_t Current,
                                          uint8_t AssertBit,uint16_t *NextSequence) {
  if(!NextSequence || Previous>0x7FFFU || Current>0x7FFFU || AssertBit>1U)
    return T70CcciInvalidParameter;
  if(AssertBit && Previous!=0U && ((uint16_t)(Current-Previous)&0x7FFFU)!=1U)
    return T70CcciSequenceGap;
  *NextSequence=Current;
  return T70CcciSuccess;
}

T70_CCCI_STATUS T70CcciInitialize(void *Memory,size_t Capacity,
                                   uint32_t ReceiveBytes,uint32_t TransmitBytes,
                                   T70_CCCI_RING_VIEW *View) {
  size_t Required;
  uint8_t *Base=(uint8_t *)Memory;
  T70_CCCI_SHARED_RING *Ring;
  T70_CCCI_RING_VIEW Candidate;
  if(!Memory || !View || ((uintptr_t)Memory&3U) ||
     ReceiveBytes<T70_CCCI_PACKET_MIN_BYTES || TransmitBytes<T70_CCCI_PACKET_MIN_BYTES)
    return T70CcciInvalidParameter;
  if(!RequiredBufferSize(ReceiveBytes,TransmitBytes,&Required))
    return T70CcciInvalidParameter;
  if(Capacity<Required)return T70CcciBufferTooSmall;
  memset(Base,0,Required);
  WriteLe32(Base,CCCI_BUFFER_MAGIC);WriteLe32(Base+4,CCCI_BUFFER_MAGIC);
  Ring=(T70_CCCI_SHARED_RING *)(Base+T70_CCCI_RING_PREFIX_BYTES);
  Ring->Receive.Length=ReceiveBytes;
  Ring->Transmit.Length=TransmitBytes;
  WriteLe32(Base+Required-T70_CCCI_RING_SUFFIX_BYTES,CCCI_BUFFER_END_MAGIC);
  WriteLe32(Base+Required-4U,CCCI_BUFFER_END_MAGIC);
  Candidate.Base=Base;Candidate.Capacity=Capacity;Candidate.Shared=Ring;
  T70_CCCI_MEMORY_BARRIER();
  *View=Candidate;
  return T70CcciSuccess;
}

T70_CCCI_STATUS T70CcciTransmitFreeBytes(const T70_CCCI_RING_VIEW *View,
                                          uint32_t *FreeBytes) {
  T70_CCCI_RING_SNAPSHOT Snapshot;
  uint32_t Used;
  T70_CCCI_STATUS Status=SnapshotView(View,&Snapshot);
  if(Status!=T70CcciSuccess)return Status;
  if(!FreeBytes)return T70CcciInvalidParameter;
  Status=RingUsed(Snapshot.TransmitRead,Snapshot.TransmitWrite,
      Snapshot.TransmitLength,&Used);
  if(Status==T70CcciSuccess)*FreeBytes=Snapshot.TransmitLength-1U-Used;
  return Status;
}

T70_CCCI_STATUS T70CcciWrite(const T70_CCCI_RING_VIEW *View,
                              const void *Message,uint32_t MessageBytes) {
  T70_CCCI_RING_SNAPSHOT Snapshot;
  uint32_t Used,Next;
  size_t FrameBytes,AlignedBytes,Padding;
  uint8_t Header[CCIF_HEADER_SIZE],Footer[CCIF_FOOTER_SIZE];
  T70_CCCI_STATUS Status=SnapshotView(View,&Snapshot);
  if(Status!=T70CcciSuccess)return Status;
  if(!Message)return T70CcciInvalidParameter;
  Status=PacketFrameBytes(MessageBytes,&FrameBytes);
  if(Status!=T70CcciSuccess)return Status;
  if(FrameBytes>Snapshot.TransmitLength-1U)return T70CcciPacketTooLarge;
  Status=RingUsed(Snapshot.TransmitRead,Snapshot.TransmitWrite,
      Snapshot.TransmitLength,&Used);
  if(Status!=T70CcciSuccess)return Status;
  if(FrameBytes>Snapshot.TransmitLength-1U-Used)return T70CcciNotEnoughSpace;
  WriteLe32(Header,CCIF_FRAME_MAGIC);WriteLe32(Header+4,MessageBytes);
  WriteLe32(Footer,CCIF_FRAME_END_MAGIC);WriteLe32(Footer+4,CCIF_FRAME_END_MAGIC);
  AlignedBytes=FrameBytes-CCIF_HEADER_SIZE-CCIF_FOOTER_SIZE;
  Padding=AlignedBytes-MessageBytes;
  CopyIntoRing(Snapshot.TransmitBuffer,Snapshot.TransmitLength,
      Snapshot.TransmitWrite,Header,sizeof(Header));
  Next=(uint32_t)(((uint64_t)Snapshot.TransmitWrite+sizeof(Header))%Snapshot.TransmitLength);
  CopyIntoRing(Snapshot.TransmitBuffer,Snapshot.TransmitLength,Next,Message,MessageBytes);
  Next=(uint32_t)(((uint64_t)Next+MessageBytes)%Snapshot.TransmitLength);
  ZeroRing(Snapshot.TransmitBuffer,Snapshot.TransmitLength,Next,Padding);
  Next=(uint32_t)(((uint64_t)Next+Padding)%Snapshot.TransmitLength);
  CopyIntoRing(Snapshot.TransmitBuffer,Snapshot.TransmitLength,Next,Footer,sizeof(Footer));
  Next=(uint32_t)(((uint64_t)Snapshot.TransmitWrite+FrameBytes)%Snapshot.TransmitLength);
  T70_CCCI_MEMORY_BARRIER();
  View->Shared->Transmit.Write=Next;
  return T70CcciSuccess;
}

static T70_CCCI_STATUS PeekReceiveSnapshot(const T70_CCCI_RING_SNAPSHOT *Snapshot,
                                           uint32_t *MessageBytes) {
  uint32_t Used,Header[2],Footer[2],FooterPos,Payload;
  size_t FrameBytes;
  uint8_t Bytes[CCIF_HEADER_SIZE];
  T70_CCCI_STATUS Status;
  if(!Snapshot || !MessageBytes)return T70CcciInvalidParameter;
  Status=RingUsed(Snapshot->ReceiveRead,Snapshot->ReceiveWrite,
      Snapshot->ReceiveLength,&Used);
  if(Status!=T70CcciSuccess)return Status;
  if(Used==0U)return T70CcciEmpty;
  if(Used<T70_CCCI_PACKET_MIN_BYTES)return T70CcciIncomplete;
  CopyFromRing(Snapshot->ReceiveBuffer,Snapshot->ReceiveLength,
      Snapshot->ReceiveRead,Bytes,sizeof(Bytes));
  Header[0]=ReadLe32(Bytes);Header[1]=ReadLe32(Bytes+4);
  if(Header[0]!=CCIF_FRAME_MAGIC)return T70CcciBadHeader;
  Payload=Header[1];
  if(Payload<CCCI_HEADER_SIZE)return T70CcciBadHeader;
  Status=PacketFrameBytes(Payload,&FrameBytes);
  if(Status!=T70CcciSuccess)return T70CcciBadHeader;
  if(FrameBytes>Snapshot->ReceiveLength-1U)return T70CcciPacketTooLarge;
  if(FrameBytes>Used)return T70CcciIncomplete;
  FooterPos=(uint32_t)(((uint64_t)Snapshot->ReceiveRead+FrameBytes-CCIF_FOOTER_SIZE)%Snapshot->ReceiveLength);
  CopyFromRing(Snapshot->ReceiveBuffer,Snapshot->ReceiveLength,FooterPos,Bytes,CCIF_FOOTER_SIZE);
  Footer[0]=ReadLe32(Bytes);Footer[1]=ReadLe32(Bytes+4);
  if(Footer[0]!=CCIF_FRAME_END_MAGIC || Footer[1]!=CCIF_FRAME_END_MAGIC)
    return T70CcciBadFooter;
  *MessageBytes=Payload;
  return T70CcciSuccess;
}

T70_CCCI_STATUS T70CcciPeekReceive(const T70_CCCI_RING_VIEW *View,
                                    uint32_t *MessageBytes) {
  T70_CCCI_RING_SNAPSHOT Snapshot;
  T70_CCCI_STATUS Status=SnapshotView(View,&Snapshot);
  return Status==T70CcciSuccess?PeekReceiveSnapshot(&Snapshot,MessageBytes):Status;
}

T70_CCCI_STATUS T70CcciReadReceive(const T70_CCCI_RING_VIEW *View,
                                    void *Message,size_t Capacity,
                                    uint32_t *MessageBytes) {
  T70_CCCI_RING_SNAPSHOT Snapshot;
  uint32_t Payload,Read;
  T70_CCCI_STATUS Status;
  if(!Message || !MessageBytes)return T70CcciInvalidParameter;
  Status=SnapshotView(View,&Snapshot);
  if(Status!=T70CcciSuccess)return Status;
  Status=PeekReceiveSnapshot(&Snapshot,&Payload);
  if(Status!=T70CcciSuccess)return Status;
  if(Payload>Capacity)return T70CcciBufferTooSmall;
  Read=(uint32_t)(((uint64_t)Snapshot.ReceiveRead+CCIF_HEADER_SIZE)%Snapshot.ReceiveLength);
  CopyFromRing(Snapshot.ReceiveBuffer,Snapshot.ReceiveLength,Read,Message,Payload);
  *MessageBytes=Payload;
  return T70CcciSuccess;
}

T70_CCCI_STATUS T70CcciConsumeReceive(const T70_CCCI_RING_VIEW *View,
                                       uint32_t MessageBytes) {
  T70_CCCI_RING_SNAPSHOT Snapshot;
  uint32_t Payload,Next;
  size_t FrameBytes;
  T70_CCCI_STATUS Status;
  if(!View || !View->Shared)return T70CcciInvalidParameter;
  Status=SnapshotView(View,&Snapshot);
  if(Status!=T70CcciSuccess)return Status;
  Status=PeekReceiveSnapshot(&Snapshot,&Payload);
  if(Status!=T70CcciSuccess)return Status;
  if(Payload!=MessageBytes)return T70CcciInvalidParameter;
  Status=PacketFrameBytes(Payload,&FrameBytes);
  if(Status!=T70CcciSuccess)return Status;
  Next=(uint32_t)(((uint64_t)Snapshot.ReceiveRead+FrameBytes)%Snapshot.ReceiveLength);
  T70_CCCI_MEMORY_BARRIER();
  if(View->Shared->Receive.Read!=Snapshot.ReceiveRead)return T70CcciCorruptState;
  View->Shared->Receive.Read=Next;
  return T70CcciSuccess;
}

T70_CCCI_STATUS T70CcciReset(const T70_CCCI_RING_VIEW *View,
                               T70_CCCI_DIRECTION Direction) {
  T70_CCCI_RING_SNAPSHOT Snapshot;
  T70_CCCI_STATUS Status=SnapshotView(View,&Snapshot);
  if(Status!=T70CcciSuccess)return Status;
  T70_CCCI_MEMORY_BARRIER();
  if(Direction==T70CcciReceive) {
    View->Shared->Receive.Read=0;View->Shared->Receive.Write=0;
  } else if(Direction==T70CcciTransmit) {
    View->Shared->Transmit.Read=0;View->Shared->Transmit.Write=0;
  } else {
    return T70CcciInvalidParameter;
  }
  T70_CCCI_MEMORY_BARRIER();
  return T70CcciSuccess;
}
