/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "CcciRing.h"
#include "CcciNet.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks,Failures;
#define CHECK(X) do {++Checks;if(!(X)&&!Failures)Failures=__LINE__;} while(0)

typedef union { uint64_t Align; uint8_t Bytes[512]; } ALIGNED_MEMORY;

static void PutRing(uint8_t *Ring,uint32_t Length,uint32_t Offset,
                    const void *Source,size_t Bytes) {
  size_t First=Bytes;
  if(First>(size_t)(Length-Offset))First=Length-Offset;
  if(First)memcpy(Ring+Offset,Source,First);
  if(Bytes>First)memcpy(Ring,(const uint8_t *)Source+First,Bytes-First);
}
static void PutLe32(uint8_t *p,uint32_t v) {
  p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
static void PublishRx(T70_CCCI_RING_VIEW *View,uint32_t Offset,
                      const uint8_t *Message,uint32_t MessageBytes) {
  uint8_t Header[8],Footer[8],Pad[8]={0};
  uint32_t Length=View->Shared->Receive.Length;
  size_t Aligned=((size_t)MessageBytes+7U)&~(size_t)7U;
  size_t Padding=Aligned-MessageBytes;
  PutLe32(Header,T70_CCIF_PACKET_HEADER);PutLe32(Header+4,MessageBytes);
  PutLe32(Footer,T70_CCIF_PACKET_FOOTER);PutLe32(Footer+4,T70_CCIF_PACKET_FOOTER);
  PutRing(View->Shared->Buffer,Length,Offset,Header,sizeof(Header));
  Offset=(uint32_t)(((uint64_t)Offset+sizeof(Header))%Length);
  PutRing(View->Shared->Buffer,Length,Offset,Message,MessageBytes);
  Offset=(uint32_t)(((uint64_t)Offset+MessageBytes)%Length);
  PutRing(View->Shared->Buffer,Length,Offset,Pad,Padding);
  Offset=(uint32_t)(((uint64_t)Offset+Padding)%Length);
  PutRing(View->Shared->Buffer,Length,Offset,Footer,sizeof(Footer));
  size_t Frame=16U+Aligned;
  View->Shared->Receive.Write=(uint32_t)(((uint64_t)View->Shared->Receive.Read+Frame)%Length);
}
static uint32_t GetLe32(const uint8_t *p) {
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void TestInitialize(void) {
  ALIGNED_MEMORY Storage;T70_CCCI_RING_VIEW View={0};
  CHECK(T70CcciInitialize(Storage.Bytes,167,64,64,&View)==T70CcciBufferTooSmall);
  CHECK(T70CcciInitialize(Storage.Bytes,sizeof(Storage.Bytes),0,64,&View)==T70CcciInvalidParameter);
  CHECK(T70CcciInitialize(Storage.Bytes,sizeof(Storage.Bytes),64,64,&View)==T70CcciSuccess);
  CHECK(View.Shared && View.Shared->Receive.Length==64 && View.Shared->Transmit.Length==64);
  CHECK(GetLe32(Storage.Bytes)==T70_CCCI_RBF_HEADER && GetLe32(Storage.Bytes+4)==T70_CCCI_RBF_HEADER);
  CHECK(GetLe32(Storage.Bytes+160)==T70_CCCI_RBF_FOOTER && GetLe32(Storage.Bytes+164)==T70_CCCI_RBF_FOOTER);
  uint32_t FreeBytes=0;
  CHECK(T70CcciTransmitFreeBytes(&View,&FreeBytes)==T70CcciSuccess && FreeBytes==63U);
  CHECK(T70CcciReset(&View,(T70_CCCI_DIRECTION)99)==T70CcciInvalidParameter);
  T70_CCCI_RING_VIEW ShortView=View;ShortView.Capacity=4;
  uint32_t Length=0;
  CHECK(T70CcciPeekReceive(&ShortView,&Length)==T70CcciInvalidParameter);
}
static void TestTransmitWrapAndBackpressure(void) {
  ALIGNED_MEMORY Storage;T70_CCCI_RING_VIEW View={0};uint8_t Message[40];uint32_t Free;
  for(unsigned i=0;i<sizeof(Message);++i)Message[i]=(uint8_t)(i+1U);
  CHECK(T70CcciInitialize(Storage.Bytes,sizeof(Storage.Bytes),64,64,&View)==T70CcciSuccess);
  View.Shared->Transmit.Read=56;View.Shared->Transmit.Write=56;
  CHECK(T70CcciWrite(&View,Message,17)==T70CcciSuccess);
  CHECK(View.Shared->Transmit.Write==32);
  uint8_t Wrapped[40];
  for(unsigned i=0;i<sizeof(Wrapped);++i)Wrapped[i]=Storage.Bytes[32+64+((56U+i)%64U)];
  CHECK(GetLe32(Wrapped)==T70_CCIF_PACKET_HEADER && GetLe32(Wrapped+4)==17U);
  CHECK(memcmp(Wrapped+8,Message,17)==0);
  for(unsigned i=25;i<32;++i)CHECK(Wrapped[i]==0U);
  CHECK(GetLe32(Wrapped+32)==T70_CCIF_PACKET_FOOTER &&
        GetLe32(Wrapped+36)==T70_CCIF_PACKET_FOOTER);
  CHECK(T70CcciWrite(&View,Message,40)==T70CcciNotEnoughSpace);
  View.Shared->Transmit.Read=View.Shared->Transmit.Write;
  CHECK(T70CcciTransmitFreeBytes(&View,&Free)==T70CcciSuccess && Free==63U);
  CHECK(T70CcciWrite(&View,Message,40)==T70CcciSuccess);
  CHECK(T70CcciWrite(&View,Message,16)==T70CcciNotEnoughSpace);
  CHECK(T70CcciReset(&View,T70CcciTransmit)==T70CcciSuccess &&
        View.Shared->Transmit.Read==0 && View.Shared->Transmit.Write==0);
  CHECK(T70CcciWrite(&View,Message,15)==T70CcciInvalidParameter);
}
static void TestReceiveWrapAndConsume(void) {
  ALIGNED_MEMORY Storage;T70_CCCI_RING_VIEW View={0};uint8_t Source[20],Out[20],Small[19];
  uint32_t MessageBytes=0xA5A5U;
  for(unsigned i=0;i<sizeof(Source);++i)Source[i]=(uint8_t)(0x80U+i);
  memset(Out,0xCC,sizeof(Out));memset(Small,0xCC,sizeof(Small));
  CHECK(T70CcciInitialize(Storage.Bytes,sizeof(Storage.Bytes),64,64,&View)==T70CcciSuccess);
  View.Shared->Receive.Read=56;View.Shared->Receive.Write=56;
  PublishRx(&View,56,Source,sizeof(Source));
  CHECK(View.Shared->Receive.Write==32U);
  CHECK(T70CcciPeekReceive(&View,&MessageBytes)==T70CcciSuccess && MessageBytes==20U);
  CHECK(T70CcciReadReceive(&View,Small,sizeof(Small),&MessageBytes)==T70CcciBufferTooSmall &&
        MessageBytes==20U && Small[0]==0xCCU && View.Shared->Receive.Read==56U);
  MessageBytes=0;
  CHECK(T70CcciReadReceive(&View,Out,sizeof(Out),&MessageBytes)==T70CcciSuccess &&
        MessageBytes==sizeof(Source) && memcmp(Out,Source,sizeof(Source))==0 &&
        View.Shared->Receive.Read==56U);
  CHECK(T70CcciConsumeReceive(&View,19U)==T70CcciInvalidParameter && View.Shared->Receive.Read==56U);
  CHECK(T70CcciConsumeReceive(&View,20U)==T70CcciSuccess && View.Shared->Receive.Read==32U);
  CHECK(T70CcciPeekReceive(&View,&MessageBytes)==T70CcciEmpty);
}
static void TestMessageHeader(void) {
  uint8_t Bytes[16];T70_CCCI_MESSAGE_HEADER Header={0};
  PutLe32(Bytes,0x11223344U);PutLe32(Bytes+4,0xAABBCCDDU);
  Bytes[8]=20U;Bytes[9]=0U;
  Bytes[10]=0x34U;Bytes[11]=0x92U; /* sequence 0x1234 with assert bit */
  PutLe32(Bytes+12,0xDEADBEEFU);
  CHECK(T70CcciParseMessageHeader(Bytes,15,&Header)==T70CcciBufferTooSmall && Header.Data0==0U);
  CHECK(T70CcciParseMessageHeader(Bytes,sizeof(Bytes),&Header)==T70CcciSuccess &&
        Header.Data0==0x11223344U && Header.Data1==0xAABBCCDDU &&
        Header.Channel==20U && (Header.SequenceAndAssert&T70_CCCI_SEQUENCE_MASK)==0x1234U &&
        (Header.SequenceAndAssert&T70_CCCI_ASSERT_MASK)!=0U && Header.Reserved==0xDEADBEEFU);
  CHECK(T70CcciParseMessageHeader(NULL,sizeof(Bytes),&Header)==T70CcciInvalidParameter);
}
static void TestSequenceChecks(void) {
  uint16_t Next=0xA55AU;
  CHECK(T70CcciAdvanceRxSequence(0U,7U,1U,&Next)==T70CcciSuccess && Next==7U);
  CHECK(T70CcciAdvanceRxSequence(7U,8U,1U,&Next)==T70CcciSuccess && Next==8U);
  Next=0xA55AU;
  CHECK(T70CcciAdvanceRxSequence(8U,10U,1U,&Next)==T70CcciSequenceGap && Next==0xA55AU);
  CHECK(T70CcciAdvanceRxSequence(8U,10U,0U,&Next)==T70CcciSuccess && Next==10U);
  CHECK(T70CcciAdvanceRxSequence(0x7FFFU,0U,1U,&Next)==T70CcciSuccess && Next==0U);
  Next=0xA55AU;
  CHECK(T70CcciAdvanceRxSequence(0x8000U,0U,1U,&Next)==T70CcciInvalidParameter && Next==0xA55AU);
  CHECK(T70CcciAdvanceRxSequence(0U,0U,2U,&Next)==T70CcciInvalidParameter && Next==0xA55AU);
}
static void TestMalformedFramesAndState(void) {
  ALIGNED_MEMORY Storage;T70_CCCI_RING_VIEW View={0};uint32_t Length=0xA5A5U;
  CHECK(T70CcciInitialize(Storage.Bytes,sizeof(Storage.Bytes),64,64,&View)==T70CcciSuccess);
  View.Shared->Receive.Write=8;
  CHECK(T70CcciPeekReceive(&View,&Length)==T70CcciIncomplete && Length==0xA5A5U);
  View.Shared->Receive.Read=0;View.Shared->Receive.Write=32;
  memset(View.Shared->Buffer,0,32);PutLe32(View.Shared->Buffer,1U);
  CHECK(T70CcciPeekReceive(&View,&Length)==T70CcciBadHeader);
  View.Shared->Receive.Read=0;View.Shared->Receive.Write=32;
  PutLe32(View.Shared->Buffer,T70_CCIF_PACKET_HEADER);PutLe32(View.Shared->Buffer+4,8U);
  CHECK(T70CcciPeekReceive(&View,&Length)==T70CcciBadHeader);
  PutLe32(View.Shared->Buffer+4,16U);
  PutLe32(View.Shared->Buffer+24,0U);PutLe32(View.Shared->Buffer+28,0U);
  CHECK(T70CcciPeekReceive(&View,&Length)==T70CcciBadFooter);
  PutLe32(View.Shared->Buffer+24,T70_CCIF_PACKET_FOOTER);PutLe32(View.Shared->Buffer+28,T70_CCIF_PACKET_FOOTER);
  PutLe32(View.Shared->Buffer+4,0xFFFFFFF8U);
  CHECK(T70CcciPeekReceive(&View,&Length)==T70CcciPacketTooLarge);
  View.Shared->Receive.Read=64;
  CHECK(T70CcciPeekReceive(&View,&Length)==T70CcciCorruptState);
  View.Shared->Receive.Read=0;Storage.Bytes[0]^=1U;
  CHECK(T70CcciPeekReceive(&View,&Length)==T70CcciCorruptState);
}
static void TestNetworkChannels(void) {
  static const uint32_t Expected[22][3] = {
    {20U,22U,64U},{24U,26U,65U},{28U,30U,30U},{69U,71U,71U},
    {74U,76U,76U},{79U,81U,81U},{84U,86U,86U},{89U,91U,93U},
    {T70_CCCI_NET_INVALID_CHANNEL,T70_CCCI_NET_INVALID_CHANNEL,T70_CCCI_NET_INVALID_CHANNEL},
    {103U,105U,105U},{108U,110U,110U},{113U,115U,115U},{118U,120U,120U},
    {123U,125U,125U},{128U,130U,130U},{133U,135U,135U},{138U,140U,140U},
    {143U,145U,145U},{148U,150U,150U},{153U,155U,155U},{158U,160U,160U},
    {96U,98U,100U}
  };
  T70_CCCI_NET_CHANNELS Channels;
  for (uint32_t i=0;i<T70_CCCI_NET_CHANNEL_COUNT;++i) {
    CHECK(T70CcciGetCcmniChannels(i,1U,&Channels)==T70CcciNetSuccess);
    CHECK(Channels.Rx==Expected[i][0] && Channels.Tx==Expected[i][1] &&
          Channels.DlAck==Expected[i][2]);
    CHECK(Channels.RxAck==T70_CCCI_NET_NO_ACK_CHANNEL &&
          Channels.TxAck==T70_CCCI_NET_NO_ACK_CHANNEL);
    CHECK(Channels.MultiQueue==(uint8_t)(i!=8U && i!=21U));
    CHECK(T70CcciGetCcmniChannels(i,0U,&Channels)==T70CcciNetSuccess);
    CHECK(Channels.MultiQueue==0U);
    if (i!=8U) {
      uint32_t Found=UINT32_C(0xA5A5A5A5);
      CHECK(T70CcciFindCcmniIndexByRxChannel((uint16_t)Expected[i][0],&Found)==T70CcciNetSuccess &&
            Found==i);
      Found=UINT32_C(0xA5A5A5A5);
      CHECK(T70CcciFindCcmniIndexByTxChannel((uint16_t)Expected[i][1],&Found)==T70CcciNetSuccess &&
            Found==i);
    }
  }
  uint32_t Found=UINT32_C(0xA5A5A5A5);
  CHECK(T70CcciFindCcmniIndexByRxChannel(UINT16_MAX,&Found)==T70CcciNetNoChannel &&
        Found==UINT32_C(0xA5A5A5A5));
  CHECK(T70CcciFindCcmniIndexByTxChannel(UINT16_MAX,&Found)==T70CcciNetNoChannel &&
        Found==UINT32_C(0xA5A5A5A5));
  CHECK(T70CcciFindCcmniIndexByRxChannel(20U,NULL)==T70CcciNetInvalidParameter);
  CHECK(T70CcciFindCcmniIndexByTxChannel(22U,NULL)==T70CcciNetInvalidParameter);
  memset(&Channels,0xA5,sizeof(Channels));
  CHECK(T70CcciGetCcmniChannels(22U,1U,&Channels)==T70CcciNetNoChannel);
  CHECK(Channels.Rx==UINT32_C(0xA5A5A5A5));
  CHECK(T70CcciGetCcmniChannels(0U,2U,&Channels)==T70CcciNetInvalidParameter);
  CHECK(T70CcciGetCcmniChannels(0U,1U,NULL)==T70CcciNetInvalidParameter);
}

int main(void) {
  TestInitialize();TestTransmitWrapAndBackpressure();TestReceiveWrapAndConsume();
  TestMessageHeader();TestSequenceChecks();TestMalformedFramesAndState();
  TestNetworkChannels();
  if(Failures){printf("CcciRingAndNetTest failed at line %u, checks=%u\n",Failures,Checks);return 1;}
  printf("CcciRingAndNetTest passed, checks=%u\n",Checks);return 0;
}
