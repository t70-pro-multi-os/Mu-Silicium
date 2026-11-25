/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "CcciCcifCore.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static void TestSendPlans(void)
{
  T70_CCCI_CCIF_WRITE Ops[T70_CCCI_CCIF_MAX_SEND_OPS];
  T70_CCCI_CCIF_WRITE Before[T70_CCCI_CCIF_MAX_SEND_OPS];
  size_t Count=0xaaaU;

  memset(Ops,0x5a,sizeof(Ops));
  memcpy(Before,Ops,sizeof(Ops));
  CHECK(T70CcciCcifBuildSendPlan(24U,0U,Ops,
      T70_CCCI_CCIF_MAX_SEND_OPS,&Count)==T70CcciCcifInvalid);
  CHECK(memcmp(Ops,Before,sizeof(Ops))==0 && Count==0xaaaU);
  CHECK(T70CcciCcifBuildSendPlan(23U,1U<<23,Ops,
      T70_CCCI_CCIF_MAX_SEND_OPS,&Count)==T70CcciCcifBusy);
  CHECK(memcmp(Ops,Before,sizeof(Ops))==0 && Count==0xaaaU);
  CHECK(T70CcciCcifBuildSendPlan(23U,0U,Ops,1U,&Count)==
        T70CcciCcifNoSpace);
  CHECK(memcmp(Ops,Before,sizeof(Ops))==0 && Count==0xaaaU);
  CHECK(T70CcciCcifBuildSendPlan(23U,0U,Ops,
      T70_CCCI_CCIF_MAX_SEND_OPS,&Count)==T70CcciCcifSuccess);
  CHECK(Count==2U && Ops[0].Offset==0x04U && Ops[0].Value==(1U<<23));
  CHECK(Ops[1].Offset==0x0cU && Ops[1].Value==23U);
}

static void TestReceiveAcknowledgeMasks(void)
{
  T70_CCCI_CCIF_RX Result, Before;
  uint32_t Channels=(1U<<0)|(1U<<15)|(1U<<16)|(1U<<23);
  memset(&Result,0xa5,sizeof(Result));
  Before=Result;
  CHECK(T70CcciCcifDecodeInterrupt(Channels,T70CcciCcifDataInterrupt,
      &Result)==T70CcciCcifSuccess);
  CHECK(Result.ReceivedChannels==Channels && Result.AckMask==0x00008001U);
  CHECK(T70CcciCcifDecodeInterrupt(Channels,T70CcciCcifExceptionInterrupt,
      &Result)==T70CcciCcifSuccess);
  CHECK(Result.ReceivedChannels==Channels &&
      Result.AckMask==((1U<<15)|(1U<<16)|(1U<<23)));
  CHECK(T70CcciCcifDecodeInterrupt(Channels|0xff000000U,
      T70CcciCcifExceptionInterrupt,&Result)==T70CcciCcifSuccess);
  CHECK(Result.ReceivedChannels==Channels &&
      Result.AckMask==((1U<<15)|(1U<<16)|(1U<<23)));
  Before=Result;
  CHECK(T70CcciCcifDecodeInterrupt(0U,T70CcciCcifDataInterrupt,
      &Result)==T70CcciCcifEmpty);
  CHECK(memcmp(&Result,&Before,sizeof(Result))==0);
  CHECK(T70CcciCcifDecodeInterrupt(Channels,(T70_CCCI_CCIF_INTERRUPT_KIND)2,
      &Result)==T70CcciCcifInvalid);
  CHECK(memcmp(&Result,&Before,sizeof(Result))==0);
}

int main(void)
{
  TestSendPlans();
  TestReceiveAcknowledgeMasks();
  if (Failure) {
    fprintf(stderr,"CcciCcifCoreTest failed at line %u after %u checks\n",
            Failure,Checks);
    return (int)Failure;
  }
  printf("T70 CCIF send/ack source checks passed: %u\n",Checks);
  return 0;
}
