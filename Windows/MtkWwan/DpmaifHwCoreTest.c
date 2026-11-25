/* SPDX-License-Identifier: GPL-2.0-only */
#include "DpmaifHwCore.h"
#include "DpmaifDma.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static T70_DPMAIF_QUEUE_CONFIG ValidConfig(void)
{
  T70_DPMAIF_QUEUE_CONFIG C;
  memset(&C, 0, sizeof(C));
  C.PitAddress = 0x20000U; C.PitEntries = 256U;
  C.RequestBatAddress = 0x30000U; C.RequestBatEntries = 1024U;
  C.RequestBatPostedEntries = 1023U;
  C.FragmentBatAddress = 0x40000U; C.FragmentBatEntries = 1024U;
  C.FragmentBatPostedEntries = 1023U;
  C.FragmentEnabled = 1U;
  for (uint32_t I=0;I<T70_DPMAIF_HW_TX_QUEUES;I++) {
    C.TxDrbAddress[I] = 0x50000U + ((uint64_t)I * 0x4000U);
    C.TxDrbEntries[I] = 2048U;
  }
  return C;
}

typedef struct {
  uint32_t Registers[4][1024];
  uint8_t LastRegion;
  uint32_t LastOffset;
  unsigned Reads, Writes, PollSteps;
  unsigned ClearPeriod;
  uint32_t ClearMask;
  int CancelPoll, FailRead, FailWrite, AutoComplete;
} MOCK_HW;

static int MockRead32(void *Context, uint8_t Region, uint32_t Offset,
                      uint32_t *Value)
{
  MOCK_HW *M=(MOCK_HW *)Context;
  if (M->FailRead || Region>=4U || Offset>=0x1000U || (Offset&3U)!=0U ||
      Value==NULL) return 0;
  M->Reads++;
  M->LastRegion=Region;
  M->LastOffset=Offset;
  *Value=M->Registers[Region][Offset/4U];
  return 1;
}

static int MockWrite32(void *Context, uint8_t Region, uint32_t Offset,
                       uint32_t Value)
{
  MOCK_HW *M=(MOCK_HW *)Context;
  if (M->FailWrite || Region>=4U || Offset>=0x1000U || (Offset&3U)!=0U)
    return 0;
  M->Writes++;
  M->Registers[Region][Offset/4U]=Value;
  return 1;
}

static int MockPollStep(void *Context)
{
  MOCK_HW *M=(MOCK_HW *)Context;
  M->PollSteps++;
  if (M->CancelPoll) return 0;
  if (M->ClearPeriod!=0U && (M->PollSteps%M->ClearPeriod)==0U)
    M->Registers[M->LastRegion][M->LastOffset/4U]&=~M->ClearMask;
  if (M->AutoComplete && M->LastRegion==T70DpmaifHwRegionPdUl) {
    if (M->LastOffset==0x494U) M->Registers[M->LastRegion][M->LastOffset/4U]&=~1U;
    if (M->LastOffset==0x120U || M->LastOffset==0x100U || M->LastOffset==0x104U)
      M->Registers[M->LastRegion][M->LastOffset/4U]&=~(1U<<31);
    if (M->LastOffset==0x490U) M->Registers[M->LastRegion][M->LastOffset/4U]&=~1U;
  }
  return 1;
}

static void TestApplyPlanWithFakeMmio(void)
{
  T70_DPMAIF_QUEUE_CONFIG C=ValidConfig();
  T70_DPMAIF_HW_OP Ops[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count=0U;
  MOCK_HW M;
  T70_DPMAIF_HW_IO Io;
  memset(&M,0,sizeof(M));
  M.ClearPeriod=2U;
  M.ClearMask=1U<<31;
  Io.Context=&M; Io.Read32=MockRead32; Io.Write32=MockWrite32;
  Io.PollStep=MockPollStep;
  CHECK(T70DpmaifBuildQueueInitPlan(&C,Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingSuccess);
  M.Registers[T70DpmaifHwRegionPdUl][0xc00U/4U]=0xa5a500ffU;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,Count,&Io)==T70DpmaifHwExecSuccess);
  CHECK(M.Registers[T70DpmaifHwRegionPdUl][0xc00U/4U]==0x00010440U);
  CHECK(M.PollSteps==6U && M.Reads>0U && M.Writes>0U);

  M.ClearPeriod=0U;
  M.PollSteps=0U; M.Reads=0U; M.Writes=0U;
  Io.PollStep=NULL;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,Count,&Io)==T70DpmaifHwExecInvalid);
}

static void TestStartupPlans(void)
{
  static const uint32_t ExpectedRemaps[8] = {
    0x08520c81U, 0x08b22487U, 0x09123c8dU, 0x09725493U,
    0x09d26c99U, 0x0a22809fU, 0U, 0U
  };
  T70_DPMAIF_MODEM_MEMORY Memory;
  T70_DPMAIF_HW_OP Ops[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP Before[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count=0U;
  size_t I;

  Memory.ModemMemoryBase=0x80000000U;
  Memory.ModemMemoryBytes=0x20000000U;
  memset(Ops,0x5a,sizeof(Ops));
  CHECK(T70DpmaifBuildCommonInitPlan(&Memory,Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingSuccess);
  CHECK(Count==13U);
  CHECK(Ops[0].Region==T70DpmaifHwRegionPdUl &&
        Ops[0].Kind==T70DpmaifHwUpdate && Ops[0].Offset==0x494U &&
        Ops[0].Mask==1U && Ops[0].Value==1U);
  CHECK(Ops[1].Kind==T70DpmaifHwPollClear &&
        Ops[1].Offset==0x494U && Ops[1].Mask==1U);
  CHECK(Ops[2].Kind==T70DpmaifHwWrite &&
        Ops[2].Offset==0x468U && Ops[2].Value==0x7fU);
  for (I=0U;I<8U;I++) {
    CHECK(Ops[3U+I].Region==T70DpmaifHwRegionPdUl &&
          Ops[3U+I].Kind==T70DpmaifHwWrite &&
          Ops[3U+I].Offset==0xe0cU+(uint32_t)I*4U &&
          Ops[3U+I].Value==ExpectedRemaps[I]);
  }
  CHECK(Ops[11].Region==T70DpmaifHwRegionAoUl &&
        Ops[11].Offset==0x08cU && Ops[11].Value==0x18600U);
  CHECK(Ops[12].Region==T70DpmaifHwRegionPdUl &&
        Ops[12].Offset==0x1a8U && Ops[12].Value==0xffU);

  memcpy(Before,Ops,sizeof(Ops));
  Count=0xaaaU;
  Memory.ModemMemoryBytes=0x1fffffffU;
  CHECK(T70DpmaifBuildCommonInitPlan(&Memory,Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingInvalid);
  CHECK(memcmp(Ops,Before,sizeof(Ops))==0 && Count==0xaaaU);
  Memory.ModemMemoryBytes=0x20000000U;
  CHECK(T70DpmaifBuildCommonInitPlan(&Memory,Ops,1U,&Count)==
        T70DpmaifRingNoSpace);
  CHECK(memcmp(Ops,Before,sizeof(Ops))==0 && Count==0xaaaU);

  CHECK(T70DpmaifBuildInterruptInitPlan(Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingSuccess);
  CHECK(Count==10U);
  CHECK(Ops[0].Region==T70DpmaifHwRegionPdUl &&
        Ops[0].Offset==0x400U && Ops[0].Value==UINT32_MAX);
  CHECK(Ops[1].Region==T70DpmaifHwRegionAoUl &&
        Ops[1].Offset==0x084U && Ops[1].Value==0x01fffc1fU);
  CHECK(Ops[2].Offset==0x088U && Ops[2].Value==~0x01fffc1fU);
  CHECK(Ops[3].Kind==T70DpmaifHwPollMaskedNotEqual &&
        Ops[3].Offset==0x080U && Ops[3].Mask==0x01fffc1fU &&
        Ops[3].Value==0x01fffc1fU);
  CHECK(Ops[5].Offset==0x094U && Ops[5].Value==0x43U);
  CHECK(Ops[6].Offset==0x098U && Ops[6].Value==~0x43U);
  CHECK(Ops[7].Kind==T70DpmaifHwPollMaskedNotEqual &&
        Ops[7].Offset==0x090U && Ops[7].Mask==0x43U);
  CHECK(Ops[8].Region==T70DpmaifHwRegionPdUl &&
        Ops[8].Offset==0x460U && Ops[8].Value==UINT32_MAX);
  CHECK(Ops[9].Region==T70DpmaifHwRegionAoUl &&
        Ops[9].Offset==0x09cU && Ops[9].Value==1U);

  CHECK(T70DpmaifBuildInitDonePlan(Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingSuccess);
  CHECK(Count==4U);
  CHECK(Ops[0].Region==T70DpmaifHwRegionPdUl &&
        Ops[0].Offset==0x490U && Ops[0].Kind==T70DpmaifHwUpdate);
  CHECK(Ops[1].Kind==T70DpmaifHwPollClear &&
        Ops[1].Offset==0x490U && Ops[1].Mask==1U);
  CHECK(Ops[2].Region==T70DpmaifHwRegionAoUl &&
        Ops[2].Offset==0U && Ops[2].Value==1U);
  CHECK(Ops[3].Region==T70DpmaifHwRegionAoUl &&
        Ops[3].Offset==0x400U && Ops[3].Value==1U);
}

static void TestApplyMaskedPoll(void)
{
  MOCK_HW M;
  T70_DPMAIF_HW_IO Io;
  T70_DPMAIF_HW_OP Op;
  memset(&M,0,sizeof(M));
  M.ClearPeriod=1U;
  M.ClearMask=1U;
  Io.Context=&M; Io.Read32=MockRead32; Io.Write32=MockWrite32;
  Io.PollStep=MockPollStep;
  memset(&Op,0,sizeof(Op));
  Op.Region=T70DpmaifHwRegionAoUl;
  Op.Kind=T70DpmaifHwPollMaskedNotEqual;
  Op.Mask=3U; Op.Value=3U; Op.PollLimit=2U;
  M.Registers[T70DpmaifHwRegionAoUl][0]=3U;
  CHECK(T70DpmaifApplyQueueInitPlan(&Op,1U,&Io)==T70DpmaifHwExecSuccess);
  CHECK(M.Reads==2U && M.PollSteps==1U);
}

static void TestApplyPlanFailures(void)
{
  MOCK_HW M;
  T70_DPMAIF_HW_IO Io;
  T70_DPMAIF_HW_OP Ops[2];
  memset(&M,0,sizeof(M));
  Io.Context=&M; Io.Read32=MockRead32; Io.Write32=MockWrite32;
  Io.PollStep=MockPollStep;

  memset(Ops,0,sizeof(Ops));
  Ops[0].Region=T70DpmaifHwRegionAoUl;
  Ops[0].Kind=T70DpmaifHwWrite;
  Ops[0].Mask=UINT32_MAX;
  Ops[0].Value=7U;
  Ops[1]=Ops[0];
  Ops[1].Offset=0x1001U;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,2U,&Io)==T70DpmaifHwExecInvalid);
  CHECK(M.Writes==0U);

  memset(Ops,0,sizeof(Ops));
  Ops[0].Region=T70DpmaifHwRegionPdUl;
  Ops[0].Kind=T70DpmaifHwPollClear;
  Ops[0].Mask=1U<<31;
  Ops[0].PollLimit=T70_DPMAIF_HW_POLL_LIMIT+1U;
  M.Registers[T70DpmaifHwRegionPdUl][0]=1U<<31;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,1U,&Io)==T70DpmaifHwExecInvalid);
  CHECK(M.Reads==0U && M.PollSteps==0U);
  Ops[0].PollLimit=2U;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,1U,&Io)==T70DpmaifHwExecTimeout);
  CHECK(M.Reads==2U && M.PollSteps==1U);

  M.Reads=0U; M.PollSteps=0U; M.CancelPoll=1;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,1U,&Io)==T70DpmaifHwExecCancelled);
  CHECK(M.Reads==1U && M.PollSteps==1U);

  memset(Ops,0,sizeof(Ops));
  Ops[0].Region=T70DpmaifHwRegionAoUl;
  Ops[0].Kind=T70DpmaifHwWrite;
  Ops[0].Mask=UINT32_MAX;
  Ops[0].Value=9U;
  M.FailWrite=1;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,1U,&Io)==T70DpmaifHwExecIoError);

  M.FailWrite=0; M.FailRead=1;
  Ops[0].Kind=T70DpmaifHwUpdate;
  Ops[0].Mask=1U;
  Ops[0].Value=1U;
  CHECK(T70DpmaifApplyQueueInitPlan(Ops,1U,&Io)==T70DpmaifHwExecIoError);
}

static void TestRemapSizeClasses(void)
{
  static const uint64_t Sizes[]={0x04000000U,0x08000000U,0x10000000U,0x20000000U};
  T70_DPMAIF_MODEM_MEMORY Memory={0x80000000U,0U};
  T70_DPMAIF_HW_OP Ops[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t BadCount=0U;
  for(size_t S=0;S<sizeof(Sizes)/sizeof(Sizes[0]);++S) {
    size_t Count=0U;
    Memory.ModemMemoryBytes=Sizes[S];
    CHECK(T70DpmaifBuildCommonInitPlan(&Memory,Ops,T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingSuccess);
    CHECK(Count==13U);
    for(size_t Group=0;Group<8U;++Group) {
      uint64_t Offset=(uint64_t)Group*UINT64_C(0x06000000);
      uint64_t Remaining=Sizes[S]>Offset?Sizes[S]-Offset:0U;
      uint32_t Banks=(uint32_t)((Remaining+UINT64_C(0x01ffffff))/UINT64_C(0x02000000));
      uint32_t Expected=Banks==0U?0U:(1U<<((Banks>3U)?3U:Banks))-1U;
      uint32_t Value=Ops[3U+Group].Value;
      uint32_t Encoded=(Value&1U)|((Value>>9)&2U)|((Value>>18)&4U);
      CHECK(Encoded==Expected);
    }
  }
  Memory.ModemMemoryBase=UINT64_C(0x3ffffe000);Memory.ModemMemoryBytes=0x02000000U;
  CHECK(T70DpmaifBuildCommonInitPlan(&Memory,Ops,T70_DPMAIF_HW_MAX_PLAN_OPS,&BadCount)==T70DpmaifRingInvalid);
}

static void TestPlanProfile(void)
{
  T70_DPMAIF_QUEUE_CONFIG C = ValidConfig();
  T70_DPMAIF_HW_OP Ops[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count = 0U;
  size_t I;
  unsigned PitPoll=0U, BatPoll=0U, FragPoll=0U, TxHigh=0U, TxLow=0U, TxArb=0U;

  memset(Ops, 0xa5, sizeof(Ops));
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS, &Count) == T70DpmaifRingSuccess);
  CHECK(Count > 40U && Count < T70_DPMAIF_HW_MAX_PLAN_OPS);
  CHECK(Ops[0].Region == T70DpmaifHwRegionPdUl &&
        Ops[0].Offset == 0xc00U && Ops[0].Kind == T70DpmaifHwUpdate &&
        Ops[0].Mask == 0x0000ff00U && Ops[0].Value == 0x00000400U);
  CHECK(Ops[3].Kind == T70DpmaifHwWrite && Ops[3].Offset == 0xc04U &&
        Ops[3].Value == 3080U);
  CHECK(Ops[8].Kind == T70DpmaifHwPollClear && Ops[8].Offset == 0x120U);
  CHECK(Ops[9].Kind == T70DpmaifHwWrite && Ops[9].Offset == 0x128U &&
        Ops[9].Value == (uint32_t)C.PitAddress);
  CHECK(Ops[10].Kind == T70DpmaifHwUpdate && Ops[10].Offset == 0x12cU &&
        Ops[10].Mask == (0xff000000U | 0x3ffffU) &&
        Ops[10].Value == ((uint32_t)((C.PitAddress >> 8) & 0xff000000U) |
                           C.PitEntries));
  for (I=0U;I<Count;I++) {
    if (Ops[I].Kind == T70DpmaifHwPollClear &&
        (Ops[I].Offset == 0x120U || Ops[I].Offset == 0x100U)) {
      if (Ops[I].Offset == 0x120U) {
        PitPoll++;
      } else if (BatPoll < 2U) {
        BatPoll++;
      } else {
        FragPoll++;
      }
      CHECK(Ops[I].Mask == 0x80000000U &&
            Ops[I].PollLimit == T70_DPMAIF_HW_POLL_LIMIT);
    }
    if (Ops[I].Region == T70DpmaifHwRegionPdUl &&
        Ops[I].Offset >= 0xd14U && Ops[I].Offset <= 0xd44U &&
        Ops[I].Kind == T70DpmaifHwUpdate && Ops[I].Mask == 0xff000000U)
      TxHigh++;
    if (Ops[I].Region == T70DpmaifHwRegionPdUl &&
        Ops[I].Kind == T70DpmaifHwWrite && Ops[I].Offset >= 0xd10U &&
        Ops[I].Offset <= 0xd40U && ((Ops[I].Offset - 0xd10U) % 0x10U) == 0U)
      TxLow++;
    if (Ops[I].Region == T70DpmaifHwRegionPdUl &&
        Ops[I].Kind == T70DpmaifHwUpdate && Ops[I].Mask == 0x0000ffffU &&
        Ops[I].Offset >= 0xd14U && Ops[I].Offset <= 0xd44U &&
        ((Ops[I].Offset - 0xd14U) % 0x10U) == 0U)
      CHECK(Ops[I].Value == 4096U);
    if (Ops[I].Region == T70DpmaifHwRegionAoUl && Ops[I].Offset == 0x1cU &&
        Ops[I].Kind == T70DpmaifHwUpdate && (Ops[I].Mask & 0x00000f00U))
      TxArb++;
  }
  CHECK(PitPoll == 2U && BatPoll == 2U && FragPoll == 2U);
  CHECK(TxHigh == 4U && TxLow == 4U && TxArb == 4U);
}

static void TestAtomicFailuresAndOptionalFragment(void)
{
  T70_DPMAIF_QUEUE_CONFIG C = ValidConfig();
  T70_DPMAIF_HW_OP Ops[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP Before[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count = 0xaaaU;

  memset(Ops, 0x5a, sizeof(Ops));
  memcpy(Before, Ops, sizeof(Ops));
  C.PitAddress = T70_DPMAIF_DMA_MASK_LIMIT - 0x100U;
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS, &Count) == T70DpmaifRingInvalid);
  CHECK(memcmp(Ops, Before, sizeof(Ops)) == 0 && Count == 0xaaaU);

  C = ValidConfig();
  C.PitEntries = 0x40000U;
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS, &Count) == T70DpmaifRingInvalid);
  CHECK(memcmp(Ops, Before, sizeof(Ops)) == 0 && Count == 0xaaaU);

  C = ValidConfig();
  C.RequestBatEntries = 0x10000U;
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS, &Count) == T70DpmaifRingInvalid);
  CHECK(memcmp(Ops, Before, sizeof(Ops)) == 0 && Count == 0xaaaU);

  C = ValidConfig();
  C.TxDrbEntries[2] = 32768U;
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS, &Count) == T70DpmaifRingInvalid);
  CHECK(memcmp(Ops, Before, sizeof(Ops)) == 0 && Count == 0xaaaU);

  C = ValidConfig();
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops, 1U, &Count) ==
        T70DpmaifRingNoSpace);
  CHECK(memcmp(Ops, Before, sizeof(Ops)) == 0 && Count == 0xaaaU);

  C = ValidConfig();
  C.FragmentEnabled = 0U;
  C.FragmentBatAddress = C.FragmentBatEntries = 0U;
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS, &Count) == T70DpmaifRingSuccess);
  for (size_t I=0U;I<Count;I++)
    CHECK(!(Ops[I].Region == T70DpmaifHwRegionPdUl &&
        (Ops[I].Value & (1U<<16)) && Ops[I].Offset == 0x100U));

  C.FragmentBatAddress = 0x60000U;
  CHECK(T70DpmaifBuildQueueInitPlan(&C, Ops,
      T70_DPMAIF_HW_MAX_PLAN_OPS, &Count) == T70DpmaifRingInvalid);
}

static void TestBuildStartupPlan(void)
{
  T70_DPMAIF_MODEM_MEMORY Memory={0x80000000U,0x20000000U};
  T70_DPMAIF_QUEUE_CONFIG C=ValidConfig();
  T70_DPMAIF_HW_OP Common[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP Interrupts[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP Queues[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP BatStart[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP Done[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP Startup[T70_DPMAIF_HW_MAX_PLAN_OPS];
  T70_DPMAIF_HW_OP Before[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t CommonCount=0U, InterruptCount=0U, QueueCount=0U, BatStartCount=0U, DoneCount=0U;
  size_t Count=0U, Expected;

  CHECK(T70DpmaifBuildCommonInitPlan(&Memory,Common,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&CommonCount)==T70DpmaifRingSuccess);
  CHECK(T70DpmaifBuildInterruptInitPlan(Interrupts,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&InterruptCount)==T70DpmaifRingSuccess);
  CHECK(T70DpmaifBuildQueueInitPlan(&C,Queues,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&QueueCount)==T70DpmaifRingSuccess);
  CHECK(T70DpmaifBuildBatStartPlan(&C,BatStart,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&BatStartCount)==T70DpmaifRingSuccess);
  CHECK(T70DpmaifBuildInitDonePlan(Done,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&DoneCount)==T70DpmaifRingSuccess);
  Expected=CommonCount+InterruptCount+QueueCount+BatStartCount+DoneCount;
  CHECK(Expected>64U && Expected<=T70_DPMAIF_HW_MAX_PLAN_OPS);
  memset(Startup,0x5a,sizeof(Startup));
  CHECK(T70DpmaifBuildStartupPlan(&Memory,&C,Startup,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingSuccess);
  CHECK(Count==Expected);
  CHECK(memcmp(Startup,Common,CommonCount*sizeof(Startup[0]))==0);
  CHECK(memcmp(Startup+CommonCount,Interrupts,
      InterruptCount*sizeof(Startup[0]))==0);
  CHECK(memcmp(Startup+CommonCount+InterruptCount,Queues,
      QueueCount*sizeof(Startup[0]))==0);
  CHECK(memcmp(Startup+CommonCount+InterruptCount+QueueCount,BatStart,
      BatStartCount*sizeof(Startup[0]))==0);
  CHECK(memcmp(Startup+CommonCount+InterruptCount+QueueCount+BatStartCount,Done,
      DoneCount*sizeof(Startup[0]))==0);
  MOCK_HW M;
  T70_DPMAIF_HW_IO Io;
  memset(&M,0,sizeof(M));
  M.AutoComplete=1;
  Io.Context=&M; Io.Read32=MockRead32; Io.Write32=MockWrite32;
  Io.PollStep=MockPollStep;
  CHECK(T70DpmaifApplyQueueInitPlan(Startup,Count,&Io)==
        T70DpmaifHwExecSuccess);
  CHECK(M.Writes>0U && M.PollSteps>=4U);

  memcpy(Before,Startup,sizeof(Before));
  Count=0xaaaU;
  Memory.ModemMemoryBytes=0x1fffffffU;
  CHECK(T70DpmaifBuildStartupPlan(&Memory,&C,Startup,
      T70_DPMAIF_HW_MAX_PLAN_OPS,&Count)==T70DpmaifRingInvalid);
  CHECK(Count==0xaaaU && memcmp(Startup,Before,sizeof(Before))==0);
  Memory.ModemMemoryBytes=0x20000000U;
  CHECK(T70DpmaifBuildStartupPlan(&Memory,&C,Startup,Expected-1U,&Count)==
      T70DpmaifRingNoSpace);
  CHECK(Count==0xaaaU && memcmp(Startup,Before,sizeof(Before))==0);
}

int main(void)
{
  TestPlanProfile();
  TestRemapSizeClasses();
  TestAtomicFailuresAndOptionalFragment();
  TestApplyPlanWithFakeMmio();
  TestApplyMaskedPoll();
  TestApplyPlanFailures();
  TestStartupPlans();
  TestBuildStartupPlan();
  if (Failure) {
    fprintf(stderr, "DpmaifHwCoreTest failed at line %u after %u checks\n",
            Failure, Checks);
    return (int)Failure;
  }
  printf("T70 DPMAIF startup register plans: %u source, fake-MMIO, and boundary checks passed\n",
         Checks);
  return 0;
}
