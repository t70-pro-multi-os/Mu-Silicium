/* SPDX-License-Identifier: GPL-2.0-only */
#include "DpmaifLayout.h"
#include "DpmaifDma.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(Expression) do { \
  ++Checks; \
  if (!(Expression) && Failure == 0U) Failure = __LINE__; \
} while (0)

static T70_DPMAIF_QUEUE_COUNTS
StockCounts(void)
{
  T70_DPMAIF_QUEUE_COUNTS C;
  memset(&C, 0, sizeof(C));
  /* T70 DT omits dl_bat_entry_size: stock v3 default 16384; PIT doubles it. */
  C.PitEntries = 32768U;
  C.RequestBatEntries = 16384U;
  C.FragmentBatEntries = 16384U;
  C.FragmentEnabled = 1U;
  for (unsigned I=0U; I<T70_DPMAIF_HW_TX_QUEUES; ++I)
    C.TxDrbEntries[I] = 2048U;
  return C;
}

static T70_DPMAIF_QUEUE_ADDRESSES
StockAddresses(void)
{
  T70_DPMAIF_QUEUE_ADDRESSES A;
  memset(&A, 0, sizeof(A));
  A.PitAddress=UINT64_C(0x80000000);
  A.RequestBatAddress=UINT64_C(0x81000000);
  A.FragmentBatAddress=UINT64_C(0x82000000);
  for (unsigned I=0U; I<T70_DPMAIF_HW_TX_QUEUES; ++I)
    A.TxDrbAddress[I]=UINT64_C(0x83000000)+(uint64_t)I*0x4000U;
  return A;
}

static void
TestStockLayout(void)
{
  T70_DPMAIF_QUEUE_COUNTS C=StockCounts();
  T70_DPMAIF_QUEUE_ADDRESSES A=StockAddresses();
  T70_DPMAIF_QUEUE_MEMORY_SIZES Sizes, SizesBefore;
  T70_DPMAIF_QUEUE_LAYOUT Layout, Before;
  T70_DPMAIF_HW_OP Ops[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count=0U;

  CHECK(T70DpmaifQueueMemorySizes(&C,&Sizes)==T70DpmaifRingSuccess);
  CHECK(Sizes.PitBytes==524288U);
  CHECK(Sizes.RequestBatBytes==131072U);
  CHECK(Sizes.FragmentBatBytes==131072U);
  for (unsigned I=0U; I<T70_DPMAIF_HW_TX_QUEUES; ++I)
    CHECK(Sizes.TxDrbBytes[I]==16384U);
  CHECK(Sizes.TotalBytes==851968U);

  memset(&Layout,0xa5,sizeof(Layout));
  CHECK(T70DpmaifBuildQueueLayout(&C,&A,&Layout)==T70DpmaifRingSuccess);
  CHECK(Layout.BytesUsed==Sizes.TotalBytes);
  CHECK(Layout.Queues.PitAddress==A.PitAddress &&
        Layout.Queues.RequestBatAddress==A.RequestBatAddress &&
        Layout.Queues.FragmentBatAddress==A.FragmentBatAddress);
  for (unsigned I=0U; I<T70_DPMAIF_HW_TX_QUEUES; ++I)
    CHECK(Layout.Queues.TxDrbAddress[I]==A.TxDrbAddress[I]);
  CHECK(T70DpmaifBuildQueueInitPlan(&Layout.Queues,Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingSuccess);

  Before=Layout;
  A.RequestBatAddress=A.PitAddress+64U;
  CHECK(T70DpmaifBuildQueueLayout(&C,&A,&Layout)==T70DpmaifRingInvalid);
  CHECK(memcmp(&Layout,&Before,sizeof(Layout))==0);
  A=StockAddresses();
  A.PitAddress++;
  CHECK(T70DpmaifBuildQueueLayout(&C,&A,&Layout)==T70DpmaifRingInvalid);
  CHECK(memcmp(&Layout,&Before,sizeof(Layout))==0);
  A=StockAddresses();
  A.TxDrbAddress[3]=T70_DPMAIF_DMA_MASK_LIMIT-Sizes.TxDrbBytes[3]+64U;
  CHECK(T70DpmaifBuildQueueLayout(&C,&A,&Layout)==T70DpmaifRingInvalid);
  CHECK(memcmp(&Layout,&Before,sizeof(Layout))==0);

  SizesBefore=Sizes;
  C.PitEntries=0U;
  CHECK(T70DpmaifQueueMemorySizes(&C,&Sizes)==T70DpmaifRingInvalid);
  CHECK(memcmp(&Sizes,&SizesBefore,sizeof(Sizes))==0);
  C=StockCounts();
  C.TxDrbEntries[3]=UINT32_MAX;
  CHECK(T70DpmaifQueueMemorySizes(&C,&Sizes)==T70DpmaifRingInvalid);
  CHECK(memcmp(&Sizes,&SizesBefore,sizeof(Sizes))==0);
}

static void
TestOptionalFragment(void)
{
  T70_DPMAIF_QUEUE_COUNTS C=StockCounts();
  T70_DPMAIF_QUEUE_ADDRESSES A=StockAddresses();
  T70_DPMAIF_QUEUE_LAYOUT Layout;
  T70_DPMAIF_QUEUE_MEMORY_SIZES Sizes;

  C.FragmentEnabled=0U;
  C.FragmentBatEntries=0U;
  A.FragmentBatAddress=0U;
  CHECK(T70DpmaifQueueMemorySizes(&C,&Sizes)==T70DpmaifRingSuccess);
  CHECK(Sizes.FragmentBatBytes==0U && Sizes.TotalBytes==720896U);
  CHECK(T70DpmaifBuildQueueLayout(&C,&A,&Layout)==T70DpmaifRingSuccess);
  CHECK(Layout.Queues.FragmentBatAddress==0U &&
        Layout.Queues.FragmentBatEntries==0U &&
        Layout.Queues.FragmentEnabled==0U);
  A.FragmentBatAddress=UINT64_C(0x82000000);
  CHECK(T70DpmaifBuildQueueLayout(&C,&A,&Layout)==T70DpmaifRingInvalid);
}

int
main(void)
{
  TestStockLayout();
  TestOptionalFragment();
  if (Failure != 0U) {
    fprintf(stderr,"DpmaifLayoutTest failed at line %u after %u checks\n",
        Failure,Checks);
    return (int)Failure;
  }
  printf("T70 DPMAIF table sizes and separate DMA-buffer ranges: %u checks passed\n",
      Checks);
  return 0;
}
