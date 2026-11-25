/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2016 MediaTek Inc. CCCI ring format. */
#ifndef T70_CCCI_RING_H
#define T70_CCCI_RING_H
#include <stddef.h>
#include <stdint.h>

#define T70_CCCI_RBF_HEADER 0xEE0000EEU
#define T70_CCCI_RBF_FOOTER 0xFF0000FFU
#define T70_CCIF_PACKET_HEADER 0xAABBAABBU
#define T70_CCIF_PACKET_FOOTER 0xCCDDEEFFU
#define T70_CCIF_PACKET_HEADER_BYTES 8U
#define T70_CCIF_PACKET_FOOTER_BYTES 8U
#define T70_CCCI_MESSAGE_HEADER_BYTES 16U
#define T70_CCCI_RING_PREFIX_BYTES 8U
#define T70_CCCI_RING_SUFFIX_BYTES 8U
#define T70_CCCI_PACKET_MIN_BYTES (T70_CCIF_PACKET_HEADER_BYTES + T70_CCCI_MESSAGE_HEADER_BYTES + T70_CCIF_PACKET_FOOTER_BYTES)

typedef enum {
  T70CcciSuccess=0,
  T70CcciInvalidParameter,
  T70CcciCorruptState,
  T70CcciNotEnoughSpace,
  T70CcciEmpty,
  T70CcciIncomplete,
  T70CcciBadHeader,
  T70CcciBadFooter,
  T70CcciPacketTooLarge,
  T70CcciBufferTooSmall,
  T70CcciSequenceGap
} T70_CCCI_STATUS;

typedef enum { T70CcciReceive=0, T70CcciTransmit=1 } T70_CCCI_DIRECTION;

typedef struct {
  volatile uint32_t Read;
  volatile uint32_t Write;
  volatile uint32_t Length;
} T70_CCCI_RING_CONTROL;

typedef struct {
  T70_CCCI_RING_CONTROL Receive;
  T70_CCCI_RING_CONTROL Transmit;
  uint8_t Buffer[];
} T70_CCCI_SHARED_RING;

typedef struct {
  uint8_t *Base;
  size_t Capacity;
  T70_CCCI_SHARED_RING *Shared;
} T70_CCCI_RING_VIEW;

typedef struct {
  uint32_t Data0;
  uint32_t Data1; /* Do not treat this as a receive length. */
  uint16_t Channel;
  uint16_t SequenceAndAssert;
  uint32_t Reserved;
} T70_CCCI_MESSAGE_HEADER;
#define T70_CCCI_SEQUENCE_MASK 0x7FFFU
#define T70_CCCI_ASSERT_MASK 0x8000U

T70_CCCI_STATUS T70CcciParseMessageHeader(const void *Message,size_t MessageBytes,
                                           T70_CCCI_MESSAGE_HEADER *Header);
/* Mirrors the stock 15-bit per-channel receive-sequence check. A gap leaves
   NextSequence untouched; the caller owns recovery/assert policy. */
T70_CCCI_STATUS T70CcciAdvanceRxSequence(uint16_t Previous,uint16_t Current,
                                          uint8_t AssertBit,uint16_t *NextSequence);

/* Shared control indices and frame bytes are sampled once through volatile
   accesses. Serialize host operations per direction; cache maintenance and DMA
   ownership remain the caller's responsibility. */
T70_CCCI_STATUS T70CcciInitialize(void *Memory,size_t Capacity,
                                   uint32_t ReceiveBytes,uint32_t TransmitBytes,
                                   T70_CCCI_RING_VIEW *View);
T70_CCCI_STATUS T70CcciTransmitFreeBytes(const T70_CCCI_RING_VIEW *View,
                                          uint32_t *FreeBytes);
T70_CCCI_STATUS T70CcciWrite(const T70_CCCI_RING_VIEW *View,
                              const void *Message,uint32_t MessageBytes);
T70_CCCI_STATUS T70CcciPeekReceive(const T70_CCCI_RING_VIEW *View,
                                    uint32_t *MessageBytes);
T70_CCCI_STATUS T70CcciReadReceive(const T70_CCCI_RING_VIEW *View,
                                    void *Message,size_t Capacity,
                                    uint32_t *MessageBytes);
T70_CCCI_STATUS T70CcciConsumeReceive(const T70_CCCI_RING_VIEW *View,
                                       uint32_t MessageBytes);
T70_CCCI_STATUS T70CcciReset(const T70_CCCI_RING_VIEW *View,
                               T70_CCCI_DIRECTION Direction);
#endif
