/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "CcciCcifResources.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static void TestValidAndUnorderedResources(void)
{
  T70_CCCI_CCIF_RESOURCE R[4]={
    {T70CcciCcifResourceInterrupt,259U,0U,1U,1U,0U},
    {T70CcciCcifResourceMemory,UINT64_C(0x1020a000),0x1000U,0U,0U,0U},
    {T70CcciCcifResourceInterrupt,258U,0U,1U,1U,0U},
    {T70CcciCcifResourceMemory,UINT64_C(0x10209000),0x1000U,0U,0U,0U}
  };
  T70_CCCI_CCIF_RESOURCE_MAP Map={0};
  CHECK(T70CcciCcifMapResources(R,4U,&Map)==T70CcciCcifResourceSuccess);
  CHECK(Map.ApBase==UINT64_C(0x10209000));
  CHECK(Map.ModemBase==UINT64_C(0x1020a000));
  CHECK(Map.DataInterrupt==258U && Map.ExceptionInterrupt==259U);
}

static void TestRejectsMalformedResourcesAtomically(void)
{
  T70_CCCI_CCIF_RESOURCE R[4]={
    {T70CcciCcifResourceMemory,UINT64_C(0x10209000),0x1000U,0U,0U,0U},
    {T70CcciCcifResourceMemory,UINT64_C(0x1020a000),0x1000U,0U,0U,0U},
    {T70CcciCcifResourceInterrupt,258U,0U,1U,1U,0U},
    {T70CcciCcifResourceInterrupt,259U,0U,1U,1U,0U}
  };
  T70_CCCI_CCIF_RESOURCE_MAP Map,Before;
  memset(&Map,0xa5,sizeof(Map)); Before=Map;
  CHECK(T70CcciCcifMapResources(NULL,4U,&Map)==T70CcciCcifResourceInvalid);
  CHECK(memcmp(&Map,&Before,sizeof(Map))==0);
  CHECK(T70CcciCcifMapResources(R,3U,&Map)==T70CcciCcifResourceMismatch);
  CHECK(memcmp(&Map,&Before,sizeof(Map))==0);

  R[1].Start=R[0].Start;
  CHECK(T70CcciCcifMapResources(R,4U,&Map)==T70CcciCcifResourceMismatch);
  CHECK(memcmp(&Map,&Before,sizeof(Map))==0);
  R[1].Start=UINT64_C(0x1020a000); R[1].Length=0x2000U;
  CHECK(T70CcciCcifMapResources(R,4U,&Map)==T70CcciCcifResourceMismatch);
  R[1].Length=0x1000U; R[0].Shared=1U;
  CHECK(T70CcciCcifMapResources(R,4U,&Map)==T70CcciCcifResourceMismatch);
  R[0].Shared=0U; R[2].LevelTriggered=0U;
  CHECK(T70CcciCcifMapResources(R,4U,&Map)==T70CcciCcifResourceMismatch);
  R[2].LevelTriggered=1U; R[2].ActiveHigh=0U;
  CHECK(T70CcciCcifMapResources(R,4U,&Map)==T70CcciCcifResourceMismatch);
  R[2].ActiveHigh=1U; R[3].Start=260U;
  CHECK(T70CcciCcifMapResources(R,4U,&Map)==T70CcciCcifResourceMismatch);
  CHECK(memcmp(&Map,&Before,sizeof(Map))==0);
}

int main(void)
{
  TestValidAndUnorderedResources();
  TestRejectsMalformedResourcesAtomically();
  if (Failure) {
    fprintf(stderr,"CcciCcifResourcesTest failed at line %u after %u checks\n",
            Failure,Checks);
    return (int)Failure;
  }
  printf("T70 HANA6798 CCIF resource map: %u checks passed\n",Checks);
  return 0;
}
