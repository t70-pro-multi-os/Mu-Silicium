/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2016 MediaTek Inc. Stock CCMNI channel assignment. */
#include "CcciNet.h"

typedef struct {
  uint32_t Rx;
  uint32_t Tx;
  uint32_t DlAck;
  uint8_t MultiQueueOnSystem1;
} T70_CCCI_NET_ROUTE;

static const T70_CCCI_NET_ROUTE mRoutes[T70_CCCI_NET_CHANNEL_COUNT] = {
  {20U, 22U, 64U, 1U},
  {24U, 26U, 65U, 1U},
  {28U, 30U, 30U, 1U},
  {69U, 71U, 71U, 1U},
  {74U, 76U, 76U, 1U},
  {79U, 81U, 81U, 1U},
  {84U, 86U, 86U, 1U},
  {89U, 91U, 93U, 1U},
  {T70_CCCI_NET_INVALID_CHANNEL, T70_CCCI_NET_INVALID_CHANNEL,
   T70_CCCI_NET_INVALID_CHANNEL, 0U},
  {103U, 105U, 105U, 1U},
  {108U, 110U, 110U, 1U},
  {113U, 115U, 115U, 1U},
  {118U, 120U, 120U, 1U},
  {123U, 125U, 125U, 1U},
  {128U, 130U, 130U, 1U},
  {133U, 135U, 135U, 1U},
  {138U, 140U, 140U, 1U},
  {143U, 145U, 145U, 1U},
  {148U, 150U, 150U, 1U},
  {153U, 155U, 155U, 1U},
  {158U, 160U, 160U, 1U},
  {96U, 98U, 100U, 0U}
};

T70_CCCI_NET_STATUS
T70CcciGetCcmniChannels(uint32_t Index, uint8_t IsSystem1,
                        T70_CCCI_NET_CHANNELS *Channels)
{
  const T70_CCCI_NET_ROUTE *Route;

  if ((Channels == 0) || (IsSystem1 > 1U)) {
    return T70CcciNetInvalidParameter;
  }
  if (Index >= T70_CCCI_NET_CHANNEL_COUNT) {
    return T70CcciNetNoChannel;
  }

  Route = &mRoutes[Index];
  Channels->Rx = Route->Rx;
  Channels->RxAck = T70_CCCI_NET_NO_ACK_CHANNEL;
  Channels->Tx = Route->Tx;
  Channels->TxAck = T70_CCCI_NET_NO_ACK_CHANNEL;
  Channels->DlAck = Route->DlAck;
  Channels->MultiQueue = (uint8_t)(Route->MultiQueueOnSystem1 && IsSystem1);
  return T70CcciNetSuccess;
}

static T70_CCCI_NET_STATUS
FindCcmniIndex(uint16_t Channel, uint32_t *Index, int Receive)
{
  uint32_t Candidate;

  if (Index == 0) {
    return T70CcciNetInvalidParameter;
  }
  for (Candidate=0U; Candidate<T70_CCCI_NET_CHANNEL_COUNT; ++Candidate) {
    const T70_CCCI_NET_ROUTE *Route=&mRoutes[Candidate];
    uint32_t RouteChannel=Receive?Route->Rx:Route->Tx;
    if (RouteChannel!=T70_CCCI_NET_INVALID_CHANNEL &&
        RouteChannel==(uint32_t)Channel) {
      *Index=Candidate;
      return T70CcciNetSuccess;
    }
  }
  return T70CcciNetNoChannel;
}

T70_CCCI_NET_STATUS
T70CcciFindCcmniIndexByRxChannel(uint16_t Channel, uint32_t *Index)
{
  return FindCcmniIndex(Channel,Index,1);
}

T70_CCCI_NET_STATUS
T70CcciFindCcmniIndexByTxChannel(uint16_t Channel, uint32_t *Index)
{
  return FindCcmniIndex(Channel,Index,0);
}
