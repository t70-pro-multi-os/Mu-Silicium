/* SPDX-License-Identifier: GPL-2.0-only */
/* T70 CCCI network channel mapping derived from the stock MediaTek port. */
#ifndef T70_CCCI_NET_H
#define T70_CCCI_NET_H
#include <stdint.h>

#define T70_CCCI_NET_INVALID_CHANNEL UINT32_C(0xFFFFFFFF)
#define T70_CCCI_NET_NO_ACK_CHANNEL UINT32_C(0xFF)
#define T70_CCCI_NET_CHANNEL_COUNT 22U

typedef enum {
  T70CcciNetSuccess=0,
  T70CcciNetInvalidParameter,
  T70CcciNetNoChannel
} T70_CCCI_NET_STATUS;

typedef struct {
  uint32_t Rx;
  uint32_t RxAck;
  uint32_t Tx;
  uint32_t TxAck;
  uint32_t DlAck;
  uint8_t MultiQueue;
} T70_CCCI_NET_CHANNELS;

/* Index 8 is the stock LAN replica placeholder and succeeds with invalid IDs.
   IsSystem1 controls the stock multi-queue flag used for modem MD_SYS1. */
T70_CCCI_NET_STATUS T70CcciGetCcmniChannels(uint32_t Index,
                                             uint8_t IsSystem1,
                                             T70_CCCI_NET_CHANNELS *Channels);
T70_CCCI_NET_STATUS T70CcciFindCcmniIndexByRxChannel(uint16_t Channel,
                                                       uint32_t *Index);
T70_CCCI_NET_STATUS T70CcciFindCcmniIndexByTxChannel(uint16_t Channel,
                                                       uint32_t *Index);

#endif
