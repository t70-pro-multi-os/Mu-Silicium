// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Library/ClockImplLib.h>
EFI_GUID gMediaTekClockProtocolGuid;
static CONST UINT32 Parents[]={1};
static CONST UINT32 MuxParents[]={0,1};
#define MT6789_PLL_COUNT 14U
#define FIXTURE_PLL_FIRST 11U
#define MT6789_I2C_GATE_COUNT 3U
#define FIXTURE_I2C_GATE_FIRST (FIXTURE_PLL_FIRST + MT6789_PLL_COUNT)
#define FIXTURE_CLOCK_COUNT (FIXTURE_I2C_GATE_FIRST + MT6789_I2C_GATE_COUNT)
#define gClocks mMt6789Clocks
#define gClockCount mMt6789ClockCount
#include "../../../../Silicon/MediaTek/MT6789Pkg/Library/ClockImplLib/ClockImplLib.c"
#undef gClockCount
#undef gClocks

typedef struct {
  UINT32 SourceId;
  UINT32 EnableOffset;
  UINT32 EnableMask;
  UINT32 OutputMask;
  UINT32 ResetBarMask;
  UINT32 TunerEnableOffset;
  UINT32 TunerEnableMask;
} KERNEL_PLL_EXPECTATION;

/* Linux kernel-reference values from modules/clk_mt6789/clk-mt6789.c PLL().
 * The production descriptors below are copied from the MT6789 factory table. */
static CONST KERNEL_PLL_EXPECTATION mKernelPllExpectations[MT6789_PLL_COUNT]={
  {APMIXED_ARMPLL_LL, 0x208U, BIT0, 0U, 0U, 0U, 0U},
  {APMIXED_ARMPLL_BL0,0x218U, BIT0, 0U, 0U, 0U, 0U},
  {APMIXED_CCIPLL,    0x258U, BIT0, 0U, 0U, 0U, 0U},
  {APMIXED_MPLL,      0x390U, BIT0, 0U, 0U, 0U, 0U},
  {APMIXED_MAINPLL,   0x340U, BIT0, 0xFF000000U, BIT23, 0U, 0U},
  {APMIXED_UNIVPLL,   0x308U, BIT0, 0xFF000000U, BIT23, 0U, 0U},
  {APMIXED_MSDCPLL,   0x350U, BIT0, 0U, 0U, 0U, 0U},
  {APMIXED_MMPLL,     0x360U, BIT0, 0xFF000000U, BIT23, 0U, 0U},
  {APMIXED_NPUPLL,    0x3B4U, BIT0, 0xFF000000U, 0U, 0U, 0U},
  {APMIXED_MFGPLL,    0x268U, BIT0, 0U, 0U, 0U, 0U},
  {APMIXED_TVDPLL,    0x380U, BIT0, 0U, 0U, 0U, 0U},
  {APMIXED_APLL1,     0x318U, BIT0, 0U, 0U, 0x00CU, BIT0},
  {APMIXED_APLL2,     0x32CU, BIT0, 0U, 0U, 0x00CU, BIT5},
  {APMIXED_USBPLL,    0x3CCU, BIT2, 0U, BIT23, 0U, 0U}
};

typedef struct {
  UINT32 Offset;
  UINT32 Value;
} PLL_TRACE_WRITE;

#define PLL_TRACE_MAX_WRITES 32U
static PLL_TRACE_WRITE mPllTrace[PLL_TRACE_MAX_WRITES];
static UINTN mPllTraceCount;
static BOOLEAN mCapturePllTrace;
static CONST CHAR8 *mPllTraceNames[MT6789_PLL_COUNT]={
  "armpll_ll", "armpll_bl0", "ccipll", "mpll", "mainpll", "univpll",
  "msdcpll", "mmpll", "npupll", "mfgpll", "tvdpll", "apll1", "apll2",
  "usbpll"
};

MTK_CLOCK_DESC gClocks[ARRAY_SIZE(mMt6789Clocks)]={
 {.Id=0,.Name="root",.Type=ClockTypeFixed,.Fixed=26000000},
 {.Id=1,.Name="pll",.Type=ClockTypePll,.Pll={.Parent=0,.BaseOffset=0,.PowerOffset=4,.EnMask=1}},
 {.Id=2,.Name="gate",.Type=ClockTypeGate,.Gate={.Parent=1,.SetOffset=0x80,.ClearOffset=0x84,.StatusOffset=0x88,.GateShift=3}},
 {.Id=3,.Name="muxgate",.Type=ClockTypeMuxGate,.MuxGate={.Parents=Parents,.ParentCount=1,.MuxWidth=1,.MuxOffset=0x90,.SetOffset=0x94,.ClearOffset=0x98,.GateShift=7}},
 {.Id=4,.Name="factor",.Type=ClockTypeFactors,.Factor={.Parent=1,.Mult=1,.Div=2}},
 {.Id=5,.Name="rmw",.Type=ClockTypeGate,.Gate={.Parent=1,.SetOffset=0xa0,.ClearOffset=0xa0,.StatusOffset=0xa0,.GateShift=2}},
 {.Id=6,.Name="inverted",.Type=ClockTypeGate,.Gate={.Parent=1,.SetOffset=0xa4,.ClearOffset=0xa4,.StatusOffset=0xa4,.GateShift=2,.Inverted=TRUE}},
 {.Id=7,.Name="bit31",.Type=ClockTypeGate,.Gate={.Parent=1,.SetOffset=0xb0,.ClearOffset=0xb4,.StatusOffset=0xb8,.GateShift=31}},
 {.Id=8,.Name="gpu-pll-mux",.Type=ClockTypeMux,.MuxGate={.Parents=MuxParents,.ParentCount=2,.MuxWidth=1,.MuxShift=18,.MuxOffset=0x50,.SetOffset=0x54,.ClearOffset=0x58,.UpdateOffset=MAX_UINT32,.UpdateShift=MAX_UINT8}},
 {.Id=9,.Name="zero-update",.Type=ClockTypeMux,.MuxGate={.Parents=MuxParents,.ParentCount=2,.MuxWidth=1,.MuxOffset=0xc0,.SetOffset=0xc4,.ClearOffset=0xc8,.UpdateOffset=0,.UpdateShift=3}},
 {.Id=10,.Name="invalid-update",.Type=ClockTypeMux,.MuxGate={.Parents=MuxParents,.ParentCount=2,.MuxWidth=1,.MuxOffset=0xd0,.SetOffset=0xd4,.ClearOffset=0xd8,.UpdateOffset=0,.UpdateShift=MAX_UINT8}}
};
UINTN gClockCount=FIXTURE_PLL_FIRST;
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/ClockDxe/ClockDxe.c"
static UINT32 Regs[4096];
static unsigned Writes, PllWrites;
static BOOLEAN mActualUfsClockModel;
static EFI_TPL CurrentTpl=TPL_APPLICATION;
static EFI_TPL EFIAPI RaiseTpl(EFI_TPL New) { EFI_TPL Old=CurrentTpl; CHECK(Old<=New); CurrentTpl=New; return Old; }
static VOID EFIAPI RestoreTpl(EFI_TPL Old) { CHECK(CurrentTpl==TPL_NOTIFY && Old<=CurrentTpl); CurrentTpl=Old; }
UINT32 EFIAPI MmioRead32(UINTN a) { CHECK(a>=0x100000 && a<0x104000); return Regs[(a-0x100000)/4]; }
UINT32 EFIAPI MmioWrite32(UINTN a, UINT32 v) {
  CHECK(a>=0x100000 && a<0x104000);
  CHECK(CurrentTpl==TPL_NOTIFY);
  Writes++;
  if (a<0x100008) PllWrites++;
  if (mCapturePllTrace) {
    CHECK(mPllTraceCount<PLL_TRACE_MAX_WRITES);
    mPllTrace[mPllTraceCount++]=(PLL_TRACE_WRITE){(UINT32)(a-0x100000U),v};
  }
  Regs[(a-0x100000)/4]=v;
  if (mActualUfsClockModel) {
    /* Stock TOPCKGEN/INFRACFG set/clear aliases, not forced ready values. */
    if (a==0x1000D4U) Regs[0xD0U/4U]|=v;
    if (a==0x1000D8U) Regs[0xD0U/4U]&=~v;
    if (a==0x1030A4U) Regs[0x30ACU/4U]|=v;
    if (a==0x1030A8U) Regs[0x30ACU/4U]&=~v;
  }
  return v;
}

static VOID InitActualPllFixtures(VOID) {
  CHECK(mMt6789ClockCount>APMIXED_USBPLL);
  for(UINTN I=0;I<MT6789_PLL_COUNT;++I) {
    CONST KERNEL_PLL_EXPECTATION *Expected=&mKernelPllExpectations[I];
    CONST MTK_CLOCK_DESC *Source=&mMt6789Clocks[Expected->SourceId];
    MTK_CLOCK_DESC *Fixture=&gClocks[FIXTURE_PLL_FIRST+I];
    CHECK(Source->Id==Expected->SourceId && Source->Type==ClockTypePll);
    *Fixture=*Source;
    Fixture->Id=FIXTURE_PLL_FIRST+(UINT32)I;
    Fixture->Pll.Parent=0U;
    CHECK(Fixture->Name==Source->Name && Fixture->Controller==Source->Controller &&
          Fixture->Type==Source->Type && Fixture->Pll.BaseOffset==Source->Pll.BaseOffset &&
          Fixture->Pll.PowerOffset==Source->Pll.PowerOffset &&
          Fixture->Pll.PcwOffset==Source->Pll.PcwOffset &&
          Fixture->Pll.EnMask==Source->Pll.EnMask &&
          Fixture->Pll.ResetBarMask==Source->Pll.ResetBarMask &&
          Fixture->Pll.EnableOffset==Source->Pll.EnableOffset &&
          Fixture->Pll.EnableMask==Source->Pll.EnableMask && Fixture->Pll.Parent==0U);
    CHECK(Fixture->Pll.EnableOffset==Expected->EnableOffset &&
          Fixture->Pll.EnableMask==Expected->EnableMask &&
          Fixture->Pll.EnMask==Expected->OutputMask &&
          Fixture->Pll.ResetBarMask==Expected->ResetBarMask &&
          Fixture->Pll.TunerEnableOffset==Expected->TunerEnableOffset &&
          Fixture->Pll.TunerEnableMask==Expected->TunerEnableMask);
  }
  gClockCount=FIXTURE_CLOCK_COUNT;
}

static VOID InitActualI2cGateFixtures(VOID) {
  CONST UINT32 SourceIds[MT6789_I2C_GATE_COUNT]={IMPW_AP_CLOCK_I2C0,IMPC_AP_CLOCK_I2C6,IMPW_AP_CLOCK_I2C1};
  CONST MTK_CLOCK_CONTROLLER_TYPE Controllers[MT6789_I2C_GATE_COUNT]={ClkImpIicWrapW,ClkImpIicWrapC,ClkImpIicWrapW};
  CONST UINT32 Shifts[MT6789_I2C_GATE_COUNT]={0U,2U,1U};
  CONST CHAR8 *Names[MT6789_I2C_GATE_COUNT]={"IMPW_AP_CLOCK_I2C0","IMPC_AP_CLOCK_I2C6","IMPW_AP_CLOCK_I2C1"};
  for(UINTN I=0;I<MT6789_I2C_GATE_COUNT;I++) {
    CONST MTK_CLOCK_DESC *Source=&mMt6789Clocks[SourceIds[I]];
    MTK_CLOCK_DESC *Fixture=&gClocks[FIXTURE_I2C_GATE_FIRST+I];
    CHECK(Source->Id==SourceIds[I] && Source->Name && strcmp(Source->Name,Names[I])==0 &&
          Source->Controller==Controllers[I] && Source->Type==ClockTypeGate &&
          Source->Gate.SetOffset==0xE08U && Source->Gate.ClearOffset==0xE04U &&
          Source->Gate.StatusOffset==0xE00U && Source->Gate.GateShift==Shifts[I] &&
          Source->Gate.Parent==TOP_I2C_PSEUDO && !Source->Gate.Inverted);
    *Fixture=*Source;
    Fixture->Id=FIXTURE_I2C_GATE_FIRST+(UINT32)I;
    Fixture->Gate.Parent=0U;
  }
}

static UINT32 PllTraceSeed(UINTN RowId, UINT32 Offset) {
  return 0xA5C39E71U ^ ((UINT32)(RowId+1U)*0x31415927U) ^
         (Offset*0x01020408U);
}

static UINTN FindPllTraceWrite(
  CONST PLL_TRACE_WRITE *Trace,
  UINTN Count,
  UINT32 Offset,
  UINT32 Mask,
  BOOLEAN Set
  )
{
  for (UINTN I=0U;I<Count;++I) {
    if (Trace[I].Offset==Offset &&
        (((Trace[I].Value&Mask)==Mask)==(Set!=FALSE))) return I;
  }
  return MAX_UINTN;
}

static VOID PrintPllTraceRow(
  UINTN Id,
  CONST PLL_TRACE_WRITE *Prepare,
  UINTN PrepareCount,
  CONST PLL_TRACE_WRITE *Unprepare,
  UINTN UnprepareCount
  )
{
  printf("PLL_TRACE_JSON {\"id\":%u,\"pll\":\"%s\",\"prepare_writes\":[",
         (unsigned)Id,mPllTraceNames[Id]);
  for (UINTN I=0U;I<PrepareCount;++I) {
    if (I!=0U) printf(",");
    printf("[%u,%u]",(unsigned)Prepare[I].Offset,(unsigned)Prepare[I].Value);
  }
  printf("],\"unprepare_writes\":[");
  for (UINTN I=0U;I<UnprepareCount;++I) {
    if (I!=0U) printf(",");
    printf("[%u,%u]",(unsigned)Unprepare[I].Offset,(unsigned)Unprepare[I].Value);
  }
  printf("]}\n");
}

static UINT32 PllPcwMask(CONST MTK_PLL_DESC *Pll) {
  CHECK(Pll->PcwBits>0U && Pll->PcwBits<=32U && Pll->PcwShift<32U &&
        (UINT32)Pll->PcwBits+Pll->PcwShift<=32U);
  return Pll->PcwBits==32U ? MAX_UINT32 :
         (((1U<<Pll->PcwBits)-1U)<<Pll->PcwShift);
}

static VOID TestActualPlls(VOID) {
  for(UINTN I=0;I<MT6789_PLL_COUNT;++I) {
    CONST KERNEL_PLL_EXPECTATION *Expected=&mKernelPllExpectations[I];
    CONST MTK_CLOCK_DESC *Clock=&gClocks[FIXTURE_PLL_FIRST+I];
    CONST MTK_PLL_DESC *Pll=&Clock->Pll;
    PLL_TRACE_WRITE PrepareTrace[PLL_TRACE_MAX_WRITES];
    PLL_TRACE_WRITE UnprepareTrace[PLL_TRACE_MAX_WRITES];
    UINTN PrepareCount,UnprepareCount;
    UINT32 BaseBefore,PowerBefore,PcwBefore,EnableBefore,TunerBefore=0U;
    UINT32 BaseOwnedMask,EnableOwnedMask,PowerOwnedMask,PcwMask;
    UINTN BaseIndex=Pll->BaseOffset/4U;
    UINTN PowerIndex=Pll->PowerOffset/4U;
    UINTN EnableIndex=Pll->EnableOffset/4U;
    UINTN PcwIndex=Pll->PcwOffset/4U;
    UINTN TunerIndex=Pll->TunerEnableOffset/4U;
    BOOLEAN Enabled=TRUE;

    CHECK(Clock->Id==FIXTURE_PLL_FIRST+I && Clock->Type==ClockTypePll &&
          Pll->Parent==0U && Pll->EnableMask!=0U);
    PcwMask=PllPcwMask(Pll);
    BaseOwnedMask=Pll->ResetBarMask;
    if(Pll->EnableOffset==Pll->BaseOffset)
      BaseOwnedMask|=Pll->EnableMask|Pll->EnMask;
    EnableOwnedMask=Pll->EnableMask|Pll->EnMask;
    if(Pll->EnableOffset==Pll->BaseOffset)
      EnableOwnedMask|=Pll->ResetBarMask;
    if(Pll->EnableOffset==Pll->PowerOffset)
      EnableOwnedMask|=BIT0|BIT1;
    PowerOwnedMask=BIT0|BIT1;
    if(Pll->EnableOffset==Pll->PowerOffset)
      PowerOwnedMask|=Pll->EnableMask|Pll->EnMask;

    Regs[BaseIndex]=PllTraceSeed(I,Pll->BaseOffset);
    Regs[PowerIndex]=PllTraceSeed(I,Pll->PowerOffset);
    Regs[EnableIndex]=PllTraceSeed(I,Pll->EnableOffset);
    Regs[PcwIndex]=PllTraceSeed(I,Pll->PcwOffset);
    if(Pll->TunerEnableMask!=0U)
      Regs[TunerIndex]=PllTraceSeed(I,Pll->TunerEnableOffset);
    Regs[BaseIndex]&=~Pll->ResetBarMask;
    Regs[EnableIndex]&=~(Pll->EnableMask|Pll->EnMask);
    Regs[PowerIndex]=(Regs[PowerIndex]|BIT1)&~BIT0;
    if(Pll->TunerEnableMask!=0U)
      Regs[TunerIndex]&=~Pll->TunerEnableMask;
    BaseBefore=Regs[BaseIndex];
    PowerBefore=Regs[PowerIndex];
    PcwBefore=Regs[PcwIndex];
    EnableBefore=Regs[EnableIndex];
    if(Pll->TunerEnableMask!=0U) TunerBefore=Regs[TunerIndex];

    CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS && !Enabled);
    if(Expected->SourceId==APMIXED_USBPLL) {
      CHECK((Regs[BaseIndex]&BIT0)!=0U);
      CHECK((Regs[EnableIndex]&Pll->EnableMask)==0U);
      CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS && !Enabled);
    }

    Writes=0U;
    mPllTraceCount=0U; mCapturePllTrace=TRUE;
    CHECK(ClockSetEnable(Clock->Id,TRUE)==EFI_SUCCESS);
    mCapturePllTrace=FALSE;
    PrepareCount=mPllTraceCount;
    CHECK(PrepareCount<=ARRAY_SIZE(PrepareTrace));
    for(UINTN J=0U;J<PrepareCount;++J) PrepareTrace[J]=mPllTrace[J];
    CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS && Enabled);
    CHECK((Regs[EnableIndex]&Pll->EnableMask)==Pll->EnableMask);
    CHECK((Regs[EnableIndex]&Pll->EnMask)==Pll->EnMask);
    CHECK((Regs[PowerIndex]&BIT0)!=0U && (Regs[PowerIndex]&BIT1)==0U);
    CHECK((Regs[BaseIndex]&Pll->ResetBarMask)==Pll->ResetBarMask);
    CHECK(((Regs[BaseIndex]^BaseBefore)&~BaseOwnedMask)==0U);
    CHECK(((Regs[EnableIndex]^EnableBefore)&~EnableOwnedMask)==0U);
    CHECK(((Regs[PowerIndex]^PowerBefore)&~PowerOwnedMask)==0U);
    if(Pll->TunerEnableMask!=0U) {
      CHECK((Regs[TunerIndex]&Pll->TunerEnableMask)==Pll->TunerEnableMask);
      CHECK(((Regs[TunerIndex]^TunerBefore)&~Pll->TunerEnableMask)==0U);
      CHECK(FindPllTraceWrite(PrepareTrace,PrepareCount,Pll->EnableOffset,
                              Pll->EnableMask,TRUE)<
            FindPllTraceWrite(PrepareTrace,PrepareCount,Pll->TunerEnableOffset,
                              Pll->TunerEnableMask,TRUE));
    }
    if(Pll->PcwOffset==Pll->BaseOffset)
      CHECK((Regs[PcwIndex]&PcwMask)==(BaseBefore&PcwMask));
    else
      CHECK(Regs[PcwIndex]==PcwBefore);

    Writes=0U;
    CHECK(ClockSetEnable(Clock->Id,TRUE)==EFI_SUCCESS && Writes==0U);
    CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS && Enabled);

    mPllTraceCount=0U; mCapturePllTrace=TRUE;
    CHECK(ClockSetEnable(Clock->Id,FALSE)==EFI_SUCCESS);
    mCapturePllTrace=FALSE;
    UnprepareCount=mPllTraceCount;
    CHECK(UnprepareCount<=ARRAY_SIZE(UnprepareTrace));
    for(UINTN J=0U;J<UnprepareCount;++J) UnprepareTrace[J]=mPllTrace[J];
    CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS && !Enabled);
    CHECK((Regs[EnableIndex]&Pll->EnableMask)==0U);
    CHECK((Regs[EnableIndex]&Pll->EnMask)==0U);
    CHECK((Regs[BaseIndex]&Pll->ResetBarMask)==0U);
    CHECK((Regs[PowerIndex]&BIT0)==0U && (Regs[PowerIndex]&BIT1)!=0U);
    CHECK(((Regs[BaseIndex]^BaseBefore)&~BaseOwnedMask)==0U);
    CHECK(((Regs[EnableIndex]^EnableBefore)&~EnableOwnedMask)==0U);
    CHECK(((Regs[PowerIndex]^PowerBefore)&~PowerOwnedMask)==0U);
    if(Pll->TunerEnableMask!=0U) {
      CHECK((Regs[TunerIndex]&Pll->TunerEnableMask)==0U);
      CHECK(((Regs[TunerIndex]^TunerBefore)&~Pll->TunerEnableMask)==0U);
      CHECK(FindPllTraceWrite(UnprepareTrace,UnprepareCount,
                              Pll->TunerEnableOffset,Pll->TunerEnableMask,FALSE)<
            FindPllTraceWrite(UnprepareTrace,UnprepareCount,
                              Pll->EnableOffset,Pll->EnableMask,FALSE));
    }
    if(Pll->PcwOffset==Pll->BaseOffset)
      CHECK((Regs[PcwIndex]&PcwMask)==(BaseBefore&PcwMask));
    else
      CHECK(Regs[PcwIndex]==PcwBefore);
    CHECK(ClockSetEnable(Clock->Id,FALSE)==EFI_SUCCESS);
    CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS && !Enabled);
    PrintPllTraceRow(I,PrepareTrace,PrepareCount,UnprepareTrace,UnprepareCount);
  }
}

static VOID TestActualUsbPllRate(VOID) {
  CONST MTK_CLOCK_DESC *Clock=&gClocks[FIXTURE_PLL_FIRST+MT6789_PLL_COUNT-1U];
  CONST MTK_PLL_DESC *Pll=&Clock->Pll;
  UINT64 Hz;
  BOOLEAN Enabled;
  CHECK(strcmp(Clock->Name,"APMIXED_USBPLL")==0&&Pll->PcwOffset==Pll->BaseOffset&&
        Pll->PostDivOffset==Pll->BaseOffset&&Pll->PostDivShift==24U&&
        Pll->PcwChgOffset==0x3C8U&&
        Pll->PcwBits==22U&&Pll->PcwiBits==8U&&Pll->PcwShift==0U);
  // Synthetic encoding of trial87's rate, not a physical register capture.
  Regs[Pll->BaseOffset/4U]=0x00180000U;
  Regs[Pll->PowerOffset/4U]=BIT1;
  Writes=0U;
  CHECK(ClockGetFrequency(Clock->Id,&Hz)==EFI_SUCCESS&&Hz==2496000000ULL&&Writes==0U);
  CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS&&!Enabled);
  CHECK(ClockSetEnable(Clock->Id,TRUE)==EFI_SUCCESS);
  CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS&&Enabled);
  CHECK((Regs[Pll->BaseOffset/4U]&~Pll->ResetBarMask)==0x00180000U);
  CHECK(ClockGetFrequency(Clock->Id,&Hz)==EFI_SUCCESS&&Hz==2496000000ULL);
  CHECK(ClockSetEnable(Clock->Id,FALSE)==EFI_SUCCESS);
  CHECK(ClockIsEnabled(Clock->Id,&Enabled)==EFI_SUCCESS&&!Enabled);
  CHECK(Regs[Pll->BaseOffset/4U]==0x00180000U);
  CHECK(ClockGetFrequency(Clock->Id,&Hz)==EFI_SUCCESS&&Hz==2496000000ULL);

  Writes=0;
  const UINT64 Lowest = (Pll->FMin+15)/16;
  CHECK(ClockSetFrequency(Clock->Id,Lowest-1)==EFI_UNSUPPORTED && !Writes);
  UINT32 SavedParent = gClocks[Pll->Parent].Fixed;
  gClocks[Pll->Parent].Fixed=0;
  CHECK(ClockSetFrequency(Clock->Id,Lowest)==EFI_NOT_READY && !Writes);
  gClocks[Pll->Parent].Fixed=SavedParent;
  Regs[Pll->BaseOffset/4U]|=BIT22;
  Regs[Pll->PcwChgOffset/4U]=0x00340012U;
  CHECK(ClockSetFrequency(Clock->Id,Lowest)==EFI_SUCCESS && Writes==2);
  CHECK((Regs[Pll->BaseOffset/4U]&(BIT31|BIT22|GENMASK(26,24)))==(BIT22|(4U<<24)));
  CHECK(Regs[Pll->PcwChgOffset/4U]==(0x00340012U|BIT31));
  CHECK((Regs[Pll->PcwOffset/4U]&PllPcwMask(Pll))==((Lowest*16U)<<14)/SavedParent);
  // PCW quantization puts the actual rate just below the 93.75 MHz boundary.
  CHECK(ClockGetFrequency(Clock->Id,&Hz)==EFI_SUCCESS && Hz==93700000ULL);

  const UINT32 Shift = 1;
  gClocks[Clock->Id].Pll.PcwShift=Shift;
  CHECK(ClockSetFrequency(Clock->Id,2496000000ULL)==EFI_SUCCESS);
  CHECK((Regs[Pll->PcwOffset/4U]&PllPcwMask(Pll))==(0x00180000U<<Shift));
  CHECK(ClockGetFrequency(Clock->Id,&Hz)==EFI_SUCCESS && Hz==2496000000ULL);
  gClocks[Clock->Id].Pll.PcwShift=0;
  CHECK(mMt6789Clocks[TOP_MMPLL_D5_D2].Id==TOP_MMPLL_D5_D2 &&
        mMt6789Clocks[TOP_MMPLL_D5_D2].Factor.Div==10);
}

static VOID TestClockControllerMapping(VOID) {
  UINTN SavedCount=gClockCount;
  MTK_CLOCK_CONTROLLER_TYPE Controllers[]={ClkImpIicWrapW,ClkImpIicWrapC};
  CONST CHAR8 *Names[]={"I2C Wrap W","I2C Wrap C"};

  // A platform with no descriptor using the I2C wrapper must not require its
  // memory-map entry, preserving the shared ClockDxe on older SoCs.
  gClockCount=FIXTURE_I2C_GATE_FIRST;
  LocateStatus=EFI_NOT_FOUND;
  for(UINTN I=0;I<ARRAY_SIZE(Controllers);I++) {
    CHECK(!ClockControllerRequired(Controllers[I]));
    CHECK(LocateOptionalClockControllerRegion(Names[I],Controllers[I])==EFI_SUCCESS);
    CHECK(mClockControllersRegions[Controllers[I]].Address==0U);
  }

  // Once a clock descriptor uses a wrapper, a missing region is a hard error.
  gClockCount=FIXTURE_CLOCK_COUNT;
  for(UINTN I=0;I<ARRAY_SIZE(Controllers);I++) {
    CHECK(ClockControllerRequired(Controllers[I]));
    CHECK(LocateOptionalClockControllerRegion(Names[I],Controllers[I])==EFI_NOT_FOUND);
  }
  LocateStatus=EFI_SUCCESS;
  gClockCount=SavedCount;
}

static VOID TestActualUfsClockChain(VOID) {
  /* Preserve the actual MT6789 IDs and parent edges: no fixed-on root or
   * rewritten parent IDs in this check. Only MMIO is a register model. */
  CONST UINT32 Parents[]={TOP_TCK_26M_MX9,TOP_MAINPLL_D4_D4,TOP_MAINPLL_D4_D8,
    TOP_UNIVPLL_D4_D4,TOP_MAINPLL_D6_D2,TOP_MAINPLL_D5_D2,TOP_MSDCPLL_D2};
  CONST UINT32 Gates[]={IFRAO_UFS,IFRAO_UFS_SAP_BCLK,IFRAO_UNIPRO_TICK,
    IFRAO_UNIPRO_SYSCLK,IFRAO_UFS_AES};
  CONST UINT32 GateBits[]={28U,13U,12U,11U,29U};
  CONST UINT32 Plls[]={APMIXED_MAINPLL,APMIXED_UNIVPLL,APMIXED_MSDCPLL};
  UINT32 AllGates=BIT28|BIT13|BIT12|BIT11|BIT29;
  BOOLEAN On;
  UINT64 Hz,BeforeHz;
  CopyMem(gClocks,mMt6789Clocks,sizeof(mMt6789Clocks));
  gClockCount=ARRAY_SIZE(mMt6789Clocks);
  mClockControllersRegions[ClkTopCkGen].Address=0x100000U;
  mClockControllersRegions[ClkApMixed].Address=0x101000U;
  mClockControllersRegions[ClkInfraCfgAo].Address=0x103000U;
  mActualUfsClockModel=TRUE;
  CHECK(gClocks[TOP_UFS].Type==ClockTypeFactors && gClocks[TOP_UFS].Factor.Parent==TOP_UFS_SEL);
  CHECK(gClocks[TOP_UFS_SEL].MuxGate.ParentCount==ARRAY_SIZE(Parents));
  for(UINTN I=0;I<ARRAY_SIZE(Parents);I++) {
    UINT32 MuxWord=(0x58AA5533U&~(7U<<24))|((UINT32)I<<24)|BIT31;
    UINT32 Parent;
    ZeroMem(Regs,sizeof(Regs));
    Regs[0xD0U/4U]=MuxWord;
    Regs[0x30ACU/4U]=AllGates;
    for(UINTN J=0;J<ARRAY_SIZE(Plls);J++) {
      CONST MTK_PLL_DESC *Pll=&gClocks[Plls[J]].Pll;
      Regs[(0x1000U+Pll->PcwOffset)/4U]=1U<<(Pll->PcwBits-Pll->PcwiBits+5U);
      Regs[(0x1000U+Pll->PowerOffset)/4U]=BIT1;
    }
    CHECK(ClockGetParent(TOP_UFS_SEL,&Parent)==EFI_SUCCESS && Parent==Parents[I]);
    CHECK(ClockGetFrequency(TOP_UFS,&BeforeHz)==EFI_SUCCESS && BeforeHz>0 && BeforeHz<=1000000000ULL);
    CHECK(ClockIsEnabled(TOP_UFS,&On)==EFI_SUCCESS && !On);
    CHECK(ClockSetEnable(TOP_UFS,TRUE)==EFI_SUCCESS);
    CHECK(ClockIsEnabled(TOP_UFS,&On)==EFI_SUCCESS && On);
    CHECK(Regs[0xD0U/4U]==(MuxWord&~BIT31));
    CHECK(ClockGetParent(TOP_UFS_SEL,&Parent)==EFI_SUCCESS && Parent==Parents[I]);
    CHECK(ClockGetFrequency(TOP_UFS,&Hz)==EFI_SUCCESS && Hz==BeforeHz);
    if(I!=0U) {
      UINT32 Pll=I==3U ? APMIXED_UNIVPLL : I==6U ? APMIXED_MSDCPLL : APMIXED_MAINPLL;
      CHECK(ClockIsEnabled(Pll,&On)==EFI_SUCCESS && On);
    }
    for(UINTN J=0;J<ARRAY_SIZE(Gates);J++) {
      CHECK(gClocks[Gates[J]].Gate.GateShift==GateBits[J]);
      CHECK(ClockIsEnabled(Gates[J],&On)==EFI_SUCCESS && !On);
      CHECK(ClockSetEnable(Gates[J],TRUE)==EFI_SUCCESS);
      CHECK(ClockIsEnabled(Gates[J],&On)==EFI_SUCCESS && On);
    }
    CHECK((Regs[0x30ACU/4U]&AllGates)==0U);
    for(UINTN J=ARRAY_SIZE(Gates);J>0;J--) CHECK(ClockSetEnable(Gates[J-1U],FALSE)==EFI_SUCCESS);
    CHECK((Regs[0x30ACU/4U]&AllGates)==AllGates);
    CHECK(ClockIsEnabled(TOP_UFS,&On)==EFI_SUCCESS && On);
    Writes=0;
    CHECK(ClockSetEnable(TOP_UFS,FALSE)==EFI_UNSUPPORTED && Writes==0U);
    CHECK(ClockGetFrequency(TOP_UFS,&Hz)==EFI_SUCCESS && Hz==BeforeHz);
  }
  /* Selector 7 is reserved: do not silently choose a different PLL. */
  Regs[0xD0U/4U]=BIT31|(7U<<24);Writes=0;
  CHECK(ClockSetEnable(TOP_UFS,TRUE)==EFI_DEVICE_ERROR && Writes==0U);
  mActualUfsClockModel=FALSE;
}

int main(void) {
  BOOLEAN on;
  Services.RaiseTPL=RaiseTpl;
  Services.RestoreTPL=RestoreTpl;
  InitActualPllFixtures();
  InitActualI2cGateFixtures();
  TestClockControllerMapping();
  for (unsigned n=0;n<ClkMax;n++) mClockControllersRegions[n].Address=0x100000;
  mClockControllersRegions[ClkImpIicWrapW].Address=0x101000;
  mClockControllersRegions[ClkImpIicWrapC].Address=0x102000;
  Regs[0]=1;
  CHECK(ClockSetEnable(2,FALSE)==EFI_SUCCESS && PllWrites==0 && Regs[0x80/4]==BIT3);
  CHECK(ClockSetEnable(3,FALSE)==EFI_SUCCESS && PllWrites==0 && Regs[0x94/4]==BIT7);
  CHECK(ClockSetEnable(4,FALSE)==EFI_UNSUPPORTED && PllWrites==0);
  Writes=0; CHECK(ClockSetEnable(1,TRUE)==EFI_SUCCESS && Writes==0);
  CHECK(ClockSetEnable(2,TRUE)==EFI_SUCCESS && PllWrites==0 && Regs[0x84/4]==BIT3);
  Regs[0xa0/4]=0xff;
  CHECK(ClockSetEnable(5,TRUE)==EFI_SUCCESS && Regs[0xa0/4]==0xfb);
  CHECK(ClockIsEnabled(5,&on)==EFI_SUCCESS && on);
  CHECK(ClockSetEnable(5,FALSE)==EFI_SUCCESS && Regs[0xa0/4]==0xff);
  CHECK(ClockIsEnabled(5,&on)==EFI_SUCCESS && !on);
  Regs[0xa4/4]=0;
  CHECK(ClockSetEnable(6,TRUE)==EFI_SUCCESS && Regs[0xa4/4]==BIT2);
  CHECK(ClockIsEnabled(6,&on)==EFI_SUCCESS && on);
  CHECK(ClockSetEnable(6,FALSE)==EFI_SUCCESS && Regs[0xa4/4]==0);
  CHECK(ClockSetEnable(7,TRUE)==EFI_SUCCESS && Regs[0xb4/4]==0x80000000U);
  Regs[0xb8/4]=0x80000000U;CHECK(ClockIsEnabled(7,&on)==EFI_SUCCESS && !on);
  Regs[0xb8/4]=0;CHECK(ClockIsEnabled(7,&on)==EFI_SUCCESS && on);
  CHECK(ClockSetEnable(7,FALSE)==EFI_SUCCESS && Regs[0xb0/4]==0x80000000U);
  CHECK(ClockSetEnable(99,TRUE)==EFI_INVALID_PARAMETER);
  CHECK(ClockSetEnable(1,FALSE)==EFI_SUCCESS && Regs[0]==0 && !(Regs[1]&BIT0));
  Writes=0;Regs[0x50/4]=0;
  CHECK(ClockSetParent(8,1)==EFI_SUCCESS && Writes==2 && Regs[0x54/4]==BIT18 && Regs[0x58/4]==BIT18);
  Regs[0x50/4]=BIT18;Writes=0;
  CHECK(ClockSetParent(8,1)==EFI_SUCCESS && Writes==0);
  CHECK(ClockSetParent(8,0)==EFI_SUCCESS && Writes==2 && Regs[0x54/4]==0 && Regs[0x58/4]==BIT18);
  Writes=0;Regs[0xc0/4]=0;
  CHECK(ClockSetParent(9,1)==EFI_SUCCESS && Writes==3 && Regs[0]==BIT3);
  Writes=0;CHECK(ClockSetParent(10,1)==EFI_INVALID_PARAMETER && Writes==0);
  CHECK(ClockSetParent(8,99)==EFI_INVALID_PARAMETER && Writes==0);
  const UINT32 GateIds[MT6789_I2C_GATE_COUNT]={FIXTURE_I2C_GATE_FIRST,FIXTURE_I2C_GATE_FIRST+1U,FIXTURE_I2C_GATE_FIRST+2U};
  const UINT32 GateBases[MT6789_I2C_GATE_COUNT]={0x101000U,0x102000U,0x101000U};
  const UINT32 GateMasks[MT6789_I2C_GATE_COUNT]={BIT0,BIT2,BIT1};
  for(UINTN I=0;I<MT6789_I2C_GATE_COUNT;I++) {
    UINT32 StatusIndex=(GateBases[I]+0xE00U-0x100000U)/4U;
    UINT32 EnableIndex=(GateBases[I]+0xE04U-0x100000U)/4U;
    UINT32 DisableIndex=(GateBases[I]+0xE08U-0x100000U)/4U;
    Writes=0U;
    CHECK(ClockSetEnable(GateIds[I],TRUE)==EFI_SUCCESS && Writes==1U && Regs[EnableIndex]==GateMasks[I]);
    Regs[StatusIndex]=0U;
    CHECK(ClockIsEnabled(GateIds[I],&on)==EFI_SUCCESS && on);
    Regs[StatusIndex]=GateMasks[I];
    CHECK(ClockIsEnabled(GateIds[I],&on)==EFI_SUCCESS && !on);
    CHECK(ClockSetEnable(GateIds[I],FALSE)==EFI_SUCCESS && Writes==2U && Regs[DisableIndex]==GateMasks[I]);
    Regs[StatusIndex]=GateMasks[I];
    CHECK(ClockIsEnabled(GateIds[I],&on)==EFI_SUCCESS && !on);
    Regs[StatusIndex]=0U;
    CHECK(ClockIsEnabled(GateIds[I],&on)==EFI_SUCCESS && on);
  }
  TestActualPlls();
  TestActualUsbPllRate();
  TestActualUfsClockChain();
  printf("CLOCK: %u checks passed (legacy clocks, I2C0/I2C6/I2C1 wrapper gates, 14 MT6789 PLL gates, retained 2496MHz USBPLL, and seven UFS parents)\n",Checks);
}
