/* SPDX-License-Identifier: GPL-2.0-only */
/* DPMAIF v3 descriptor views from the T70 kernel reference. */
#ifndef T70_DPMAIF_DESCRIPTOR_H
#define T70_DPMAIF_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    T70DpmaifSuccess = 0,
    T70DpmaifInvalidParameter,
    T70DpmaifBufferSize,
    T70DpmaifWrongType,
    T70DpmaifSequenceError
} T70_DPMAIF_STATUS;

#define T70_DPMAIF_DATA_PIT_BYTES 16U
#define T70_DPMAIF_MESSAGE_PIT_BYTES 16U
#define T70_DPMAIF_BAT_BYTES 8U
#define T70_DPMAIF_TX_DATA_BYTES 8U
#define T70_DPMAIF_TX_MESSAGE_BYTES 8U
#define T70_DPMAIF_DMA_ADDRESS_LIMIT (UINT64_C(1) << 40)

typedef struct {
    uint8_t PacketType, Continuation, BufferType;
    uint16_t BufferId, CombinedBufferId, DataLength;
    uint64_t DataAddress;
    uint8_t PitSequence, HighBufferId, Reserved2, Ignore, BufferInfo;
    uint8_t HeaderOffset, UlqDone, DlqDone;
} T70_DPMAIF_DATA_PIT;

typedef struct {
    uint8_t PacketType, Continuation, Checksum, Error, SourceQueue;
    uint8_t HardwareChecksumIndex, Reserved0, ChannelId, NetworkType;
    uint8_t Reserved1, DataPath;
    uint16_t CountLow;
    uint8_t Flow, Reserved2, Command, HighPriorityIndex, Reserved3;
    uint16_t VirtualBufferId;
    uint8_t Protocol, Reserved4, Hash, PitSequence, HighBufferId, Reserved5;
    uint8_t Ignore, Reserved6, MulticastRoute, Reserved7, IpChecksum;
    uint8_t UlqDone, DlqDone;
} T70_DPMAIF_MESSAGE_PIT;

typedef struct {
    uint64_t BufferAddress;
    uint32_t Reserved;
} T70_DPMAIF_BAT;

typedef struct {
    uint8_t Type, Continuation, Reserved, AddressExtension;
    uint16_t DataLength;
    uint32_t AddressLow;
    uint64_t DataAddress;
} T70_DPMAIF_TX_DATA;

typedef struct {
    uint8_t Type, Continuation;
    uint16_t Reserved0, PacketLength;
    uint16_t CountLow;
    uint8_t ChannelId, NetworkType, Reserved1, Ipv4Checksum, L4Checksum, Reserved2;
} T70_DPMAIF_TX_MESSAGE;

typedef enum {
    T70DpmaifRxMessage = 1,
    T70DpmaifRxData = 2
} T70_DPMAIF_RX_KIND;

typedef struct {
    uint8_t HaveMessage, ChannelId, Checksum, DataPath;
    uint32_t PacketSegments, PacketBytes;
} T70_DPMAIF_RX_STATE;

typedef struct {
    uint8_t Kind, HaveMessage, ChannelId, Checksum, DataPath, PacketComplete;
    uint32_t PacketSegments, PacketBytes;
    T70_DPMAIF_DATA_PIT Data;
    T70_DPMAIF_MESSAGE_PIT Message;
} T70_DPMAIF_RX_EVENT;

/* Decode and encode the exact little-endian PIT/BAT/DRB fields from
 * ccci_hif_dpmaif_v3.h. Reserved fields are preserved by decode. Descriptor
 * encode validates required zero fields and commits output only on success. */
T70_DPMAIF_STATUS T70DpmaifDecodeDataPit(const uint8_t *Entry, size_t Bytes,
                                         T70_DPMAIF_DATA_PIT *Pit);
T70_DPMAIF_STATUS T70DpmaifDecodeMessagePit(const uint8_t *Entry, size_t Bytes,
                                            T70_DPMAIF_MESSAGE_PIT *Pit);
T70_DPMAIF_STATUS T70DpmaifDecodeBat(const uint8_t *Entry, size_t Bytes,
                                     T70_DPMAIF_BAT *Bat);
T70_DPMAIF_STATUS T70DpmaifEncodeBat(const T70_DPMAIF_BAT *Bat,
                                     uint8_t *Entry, size_t Bytes);
T70_DPMAIF_STATUS T70DpmaifDecodeTxData(const uint8_t *Entry, size_t Bytes,
                                        T70_DPMAIF_TX_DATA *Descriptor);
T70_DPMAIF_STATUS T70DpmaifDecodeTxMessage(const uint8_t *Entry, size_t Bytes,
                                           T70_DPMAIF_TX_MESSAGE *Descriptor);
T70_DPMAIF_STATUS T70DpmaifEncodeTxData(const T70_DPMAIF_TX_DATA *Descriptor,
                                        uint8_t *Entry, size_t Bytes);
T70_DPMAIF_STATUS T70DpmaifEncodeTxMessage(const T70_DPMAIF_TX_MESSAGE *Descriptor,
                                           uint8_t *Entry, size_t Bytes);
T70_DPMAIF_STATUS T70DpmaifRxWalkInit(T70_DPMAIF_RX_STATE *State);
T70_DPMAIF_STATUS T70DpmaifRxWalkStep(T70_DPMAIF_RX_STATE *State,
                                      const uint8_t *Entry, size_t Bytes,
                                      T70_DPMAIF_RX_EVENT *Event);

#endif
