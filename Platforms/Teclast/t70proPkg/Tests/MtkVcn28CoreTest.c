// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkVcn28Core.h>
#ifdef STP_TEST_NATIVE
#include <stdio.h>
#endif
unsigned MtkVcn28AssertionCount;
#define CHECK(X) do { ++MtkVcn28AssertionCount; if (!(X)) return __LINE__; } while (0)
typedef struct {
  unsigned Reg[4],Calls,Writes,Delays,DelayTotal,Authorized;
  unsigned FailAt,FailKind,AutoReady,BadRead,StatusReads,LastDelayCall;
} MODEL;
static MODEL Model;
static MTK_VCN28 Rail;
static unsigned Index(unsigned A) {
  switch (A) {
    case MTK_VCN28_ENABLE:return 0;
    case MTK_VCN28_OP_ENABLE:return 1;
    case MTK_VCN28_OP_CONFIG:return 2;
    case MTK_VCN28_STATUS:return 3;
    default:return 4;
  }
}
static MTK_VCN28_STATUS_CODE Read16(void *Context,unsigned A,unsigned *V) {
  MODEL *M=Context; unsigned I=Index(A); M->Calls++;
  if (I>3 || (M->Calls==M->FailAt && M->FailKind==1)) return MtkVcn28IoError;
  if (I==3) M->StatusReads++;
  *V=M->BadRead?0x10000U:M->Reg[I]; return MtkVcn28Ok;
}
static MTK_VCN28_STATUS_CODE Write16(void *Context,unsigned A,unsigned V) {
  MODEL *M=Context; unsigned I=Index(A); M->Calls++; M->Writes++;
  if (I>2 || V>0xffffU) return MtkVcn28Invalid;
  if (M->Calls==M->FailAt && M->FailKind==1) return MtkVcn28IoError;
  if (!(M->Calls==M->FailAt && M->FailKind==3)) {
    M->Reg[I]=V;
    if (!I && M->AutoReady) M->Reg[3]=(M->Reg[3]&~0x8000U)|((V&1U)<<15);
  }
  return M->Calls==M->FailAt && M->FailKind==2?MtkVcn28IoError:MtkVcn28Ok;
}
static MTK_VCN28_STATUS_CODE DelayUs(void *Context,unsigned Us) {
  MODEL *M=Context; M->Calls++; M->Delays++; M->LastDelayCall=M->Calls;
  if (M->Calls==M->FailAt) return MtkVcn28IoError;
  M->DelayTotal+=Us; return MtkVcn28Ok;
}
static unsigned Authorized(void *Context) { return ((MODEL *)Context)->Authorized; }
static MTK_VCN28_IO Io={&Model,Read16,Write16,DelayUs,Authorized};
static void Reset(unsigned Enable,unsigned Mode,unsigned Select) {
  MTK_VCN28 D={0}; MODEL M={0}; Rail=D; Model=M;
  Model.Reg[0]=Enable; Model.Reg[1]=Mode; Model.Reg[2]=Select;
  Model.Reg[3]=0x39U|((Enable&1U)<<15);
  Model.Authorized=Model.AutoReady=1;
}
static int SharedTest(void) {
  unsigned E,U,N,Before,Writes;
  MTK_VCN28_IO NoIo={&Model,0,0,0,Authorized};
  Reset(0,0,0);
  CHECK(MtkVcn28Initialize(0,&Io,0x6631)==MtkVcn28Invalid);
  CHECK(MtkVcn28Initialize(&Rail,0,0x6631)==MtkVcn28Invalid);
  CHECK(MtkVcn28Initialize(&Rail,&Io,0)==MtkVcn28Invalid);
  CHECK(MtkVcn28Initialize(&Rail,&Io,0xffff)==MtkVcn28Invalid);
  CHECK(MtkVcn28Initialize(&Rail,&Io,0x10000)==MtkVcn28Invalid);
  CHECK(MtkVcn28Initialize(&Rail,&NoIo,0x6631)==MtkVcn28Invalid);
  CHECK(MtkVcn28Set(0,1,1)==MtkVcn28Invalid);
  CHECK(MtkVcn28Set(&Rail,1,1)==MtkVcn28Invalid);
  CHECK(!Model.Calls && !Model.Writes);
  CHECK(MtkVcn28Initialize(&Rail,&NoIo,0x6635)==MtkVcn28Ok);
  CHECK(!Model.Calls);
  for (N=0;N<32;N++) {
    U=(N&1U)?1U:2U;
    CHECK(MtkVcn28Set(&Rail,U,1)==MtkVcn28Ok);
    CHECK(MtkVcn28Set(&Rail,3U^U,1)==MtkVcn28Ok && Rail.Users==3);
    CHECK(MtkVcn28Set(&Rail,U,0)==MtkVcn28Ok && Rail.Users==(3U^U));
    CHECK(MtkVcn28Set(&Rail,3U^U,0)==MtkVcn28Ok && !Rail.Users);
  }
  CHECK(!Model.Calls && !Model.Writes && !Rail.Captured);
  /* Exercise every enable-register bit, including inherited always-on power. */
  for (E=0;E<0x10000U;E+=127U) for (U=1;U<=2;U++) {
    unsigned SavedOpEnable=(E*7U)&0xffffU,SavedOpConfig=(E*11U)&0xffffU;
    Reset(E,SavedOpEnable,SavedOpConfig);
    CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok && !Model.Calls);
    CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Busy && !Model.Calls);
    CHECK(MtkVcn28Set(&Rail,U,1)==MtkVcn28Ok);
    CHECK(Rail.Users==U && Rail.Captured && !Rail.Faulted);
    CHECK(Model.Reg[0]==(E|1U) && Model.Reg[1]==3 && Model.Reg[2]==0);
    CHECK(Model.DelayTotal==((E&1U)?0U:270U));
    Writes=Model.Writes;
    CHECK(MtkVcn28Set(&Rail,U,1)==MtkVcn28Ok && Model.Writes==Writes);
    CHECK(MtkVcn28Set(&Rail,3U^U,1)==MtkVcn28Ok && Rail.Users==3 && Model.Writes==Writes);
    Before=Model.Calls;
    CHECK(MtkVcn28Set(&Rail,U,0)==MtkVcn28Ok && Model.Calls==Before && Rail.Users==(3U^U));
    CHECK(MtkVcn28Set(&Rail,U,0)==MtkVcn28Ok && Model.Calls==Before);
    CHECK(MtkVcn28Set(&Rail,3U^U,0)==MtkVcn28Ok);
    CHECK(!Rail.Users && !Rail.Captured && !Rail.Faulted && !Rail.DelayPending);
    CHECK(Model.Reg[0]==(E|1U) && Model.Reg[1]==SavedOpEnable && Model.Reg[2]==SavedOpConfig);
    CHECK(!Rail.SavedEnable && !Rail.SavedOpEnable && !Rail.SavedOpConfig);
    CHECK(Model.DelayTotal==((E&1U)?0U:270U));
    Before=Model.Calls; Model.Authorized=0;
    CHECK(MtkVcn28Set(&Rail,U,1)==MtkVcn28Offline && Model.Calls==Before && !Rail.Users);
    Model.Authorized=1;
    CHECK(MtkVcn28Set(&Rail,0,1)==MtkVcn28Invalid && Model.Calls==Before);
    CHECK(MtkVcn28Set(&Rail,3,1)==MtkVcn28Invalid && Model.Calls==Before);
    CHECK(MtkVcn28Set(&Rail,1,2)==MtkVcn28Invalid && Model.Calls==Before);
  }
  return 0;
}
static int FailureTest(void) {
  unsigned Point,Kind,AcquireCalls,ReleaseCalls,Before,DelayCall;
  MTK_VCN28_STATUS_CODE R;
  Reset(0xa4,0x42,0x84); CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
  CHECK(MtkVcn28Set(&Rail,2,1)==MtkVcn28Ok); AcquireCalls=Model.Calls; DelayCall=Model.LastDelayCall;
  Model.Reg[0]&=~1U; Model.Reg[3]&=~0x8000U;
  CHECK(MtkVcn28Set(&Rail,2,0)==MtkVcn28Ok && Model.DelayTotal==540); ReleaseCalls=Model.Calls-AcquireCalls;
  /* Inject failure before every IO, after each write, and ignored writes. */
  for (Kind=1;Kind<=3;Kind++) for (Point=1;Point<=AcquireCalls;Point++) {
    Reset(0xa4,0x42,0x84); Model.FailAt=Point; Model.FailKind=Kind;
    CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
    R=MtkVcn28Set(&Rail,2,1);
    if (R==MtkVcn28Ok) { CHECK(MtkVcn28Set(&Rail,2,0)==MtkVcn28Ok); continue; }
    CHECK(R==MtkVcn28IoError);
    if (!Rail.Captured) {
      CHECK(!Rail.Users && !Rail.Faulted && !Model.Writes);
    } else {
      CHECK(Rail.Users==2 && Rail.Faulted);
      Before=Model.Calls;
      CHECK(MtkVcn28Set(&Rail,1,1)==MtkVcn28Uncertain && Model.Calls==Before);
      CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Busy);
      Model.FailAt=0;
      CHECK(MtkVcn28Set(&Rail,2,0)==MtkVcn28Ok && !Rail.Users && !Rail.Faulted);
      CHECK(Model.Reg[0]==0xa5 && Model.Reg[1]==0x42 && Model.Reg[2]==0x84);
    }
  }
  for (Kind=1;Kind<=3;Kind++) for (Point=1;Point<=ReleaseCalls;Point++) {
    Reset(0xa4,0x42,0x84); CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
    CHECK(MtkVcn28Set(&Rail,1,1)==MtkVcn28Ok);
    Model.Reg[0]&=~1U; Model.Reg[3]&=~0x8000U;
    Model.FailAt=Model.Calls+Point; Model.FailKind=Kind;
    R=MtkVcn28Set(&Rail,1,0);
    if (R==MtkVcn28Ok) continue;
    CHECK(R==MtkVcn28IoError && Rail.Users==1 && Rail.Captured && Rail.Faulted);
    Model.FailAt=0;
    CHECK(MtkVcn28Set(&Rail,1,0)==MtkVcn28Ok && !Rail.Users && !Rail.Faulted);
    CHECK(Model.Reg[0]==0xa5 && Model.Reg[1]==0x42 && Model.Reg[2]==0x84);
  }
  Reset(0xa4,0x42,0x84); Model.FailAt=DelayCall;
  CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
  CHECK(MtkVcn28Set(&Rail,2,1)==MtkVcn28IoError && Rail.DelayPending && !Model.DelayTotal);
  Model.FailAt=0;
  CHECK(MtkVcn28Set(&Rail,2,0)==MtkVcn28Ok && !Rail.DelayPending && Model.DelayTotal==270);
  Reset(0,0,0); Model.AutoReady=0;
  CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
  CHECK(MtkVcn28Set(&Rail,2,1)==MtkVcn28Timeout && Rail.Faulted && Rail.Users==2);
  CHECK(Model.StatusReads==100 && Model.DelayTotal==1270);
  CHECK(MtkVcn28Set(&Rail,2,0)==MtkVcn28Timeout && Rail.Users==2);
  Model.Reg[3]|=0x8000;
  CHECK(MtkVcn28Set(&Rail,2,0)==MtkVcn28Ok && !Rail.Users);
  Reset(1,0,0); CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
  CHECK(MtkVcn28Set(&Rail,1,1)==MtkVcn28Ok);
  Model.Reg[1]=4; Before=Model.Writes;
  CHECK(MtkVcn28Set(&Rail,2,1)==MtkVcn28Busy && Rail.Users==1 && Rail.Faulted && Model.Writes==Before);
  CHECK(MtkVcn28Set(&Rail,1,0)==MtkVcn28Ok && Model.Reg[1]==0);
  Reset(1,0,0); Model.BadRead=1;
  CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
  CHECK(MtkVcn28Set(&Rail,1,1)==MtkVcn28IoError && !Rail.Users && !Model.Writes);
  Reset(1,0,0); CHECK(MtkVcn28Initialize(&Rail,&Io,0x6631)==MtkVcn28Ok);
  CHECK(MtkVcn28Set(&Rail,1,1)==MtkVcn28Ok);
  CHECK(MtkVcn28Set(&Rail,2,1)==MtkVcn28Ok);
  Before=Model.Calls; Model.Authorized=0;
  CHECK(MtkVcn28Set(&Rail,1,0)==MtkVcn28Offline && Rail.Users==3 && Model.Calls==Before);
  Model.Authorized=1; Model.BadRead=1;
  CHECK(MtkVcn28Set(&Rail,1,1)==MtkVcn28IoError && Rail.Faulted && Rail.Users==3);
  CHECK(MtkVcn28Set(&Rail,1,0)==MtkVcn28Ok && Rail.Users==2);
  Model.BadRead=0;
  CHECK(MtkVcn28Set(&Rail,2,0)==MtkVcn28Ok && !Rail.Users && !Rail.Faulted);
  return 0;
}
int TestMain(void) { int R=SharedTest(); if (!R) R=FailureTest(); return R; }
#ifdef STP_TEST_NATIVE
int main(void) {
  int R=TestMain();
  if (R) printf("VCN28 failed at line %d (%u assertions)\n",R,MtkVcn28AssertionCount);
  else printf("VCN28: %u assertions passed\n",MtkVcn28AssertionCount);
  return R?1:0;
}
#endif
