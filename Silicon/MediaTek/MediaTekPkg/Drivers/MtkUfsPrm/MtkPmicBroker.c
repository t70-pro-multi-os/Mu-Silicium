/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
#include <Library/MtkPmicBroker.h>

#define PMIC_TIMEOUT_US 10000U
#define PMIC_MAX_POLLS 4096U
#define WACS_COMMAND 0x880U
#define WACS_WRITE_DATA 0x884U
#define WACS_READ_DATA 0x894U
#define WACS_VALID_CLEAR 0x8A4U
#define WACS_STATUS 0x8A8U
#define PMIC_KEY_SUMMARY 0x19EU
#define PMIC_KEY_STATUS 0x91CU
#define PMIC_TOP_STATUS 0x28U
#define PMIC_KEY_SUMMARY_GROUP2 (1U << 2)
#define PMIC_GAUGE_CTRL 0x0D0AU
#define PMIC_GAUGE_CURRENT 0x0D8AU
#define PMIC_GAUGE_CONTROL_MASK 0x8009U
#define PMIC_AUXADC_REQUEST 0x1108U
#define PMIC_AUXADC_BAT_VOLTAGE 0x1088U
#define PMIC_AUXADC_BAT_TEMP 0x108EU
#define PMIC_AUXADC_CHIP_TEMP 0x1090U
#define PMIC_AUXADC_BIF_VOLTAGE 0x109EU
#define PMIC_AUXADC_BIF_CONTROL 0x0FA8U
#define PMIC_AUXADC_BIF_DISABLE 0x0002U
#define PMIC_AUXADC_DIE_TEMP_TRIM 0x11CEU
#define PMIC_AUXADC_TEMP_REF_TRIM 0x11D0U
#define PMIC_AUXADC_TEMP_SLOPE_TRIM 0x11D2U
#define PMIC_OTP_ROW 0x038AU
#define PMIC_OTP_READ_TRIGGER 0x039AU
#define PMIC_OTP_READ_ENABLE 0x03A0U
#define PMIC_OTP_READ_DATA 0x03A2U
#define PMIC_OTP_READ_BUSY 0x03A4U
#define PMIC_OTP_FUSE_ROW_FIRST 0x0050U
#define PMIC_OTP_FUSE_ROW_LAST 0x0056U
#define PMIC_AUXADC_REQUEST_MASK 0x0819U

enum {
  PhaseIdle=0, PhaseReadStart, PhaseReadWait, PhaseReadClear, PhaseReadDrain,
  PhaseWriteData, PhaseWriteCommand, PhaseWriteWait, PhaseVerifyStart,
  PhaseVerifyWait, PhaseVerifyClear, PhaseVerifyDrain,
  PhaseKeyAckWriteData, PhaseKeyAckWriteCommand, PhaseKeyAckWriteWait,
  PhaseSdPowerDelay
};
enum { KeyLeaseNone=0, KeyLeaseAcquiring, KeyLeaseActive };
enum { KeyAckNone=0, KeyAckReading, KeyAckSiblingPending, KeyAckWriting, KeyAckCompleted };
#define PMIC_KEY_IRQ_MASK 0x5U
#define PMIC_NORMAL_GENERATION_LIMIT (UINT64_MAX-2U)
typedef char RequestIs64Bytes[(sizeof(MTK_PMIC_BROKER_REQUEST)==64)?1:-1];

enum { SdVmchVoltage, SdVmcVoltage, SdVmchEint, SdVmchEnable, SdVmcEnable, SdRegisterCount };
static const struct {
  uint32_t Address,Mask,Enabled;
} SdRegisters[] = {
  {0x1E48U,0x700U,0x500U},  /* VMCH 3.3 V */
  {0x1E4CU,0xFFFU,0xD00U},  /* VMC 3.3 V */
  {0x1D70U,5U,1U},          /* VMCH_EINT_LOW */
  {0x1CD8U,1U,1U},          /* VMCH enable */
  {0x1CC4U,1U,1U}           /* VMC enable */
};
typedef struct { uint8_t Register,DelayAfter; uint16_t Mask; } SD_POWER_STEP;
static const SD_POWER_STEP SdEnableSteps[] = {
  {SdVmchVoltage,0,0x700U}, {SdVmcVoltage,0,0xFFFU}, {SdVmchEint,0,4U},
  {SdVmchEnable,0,1U}, {SdVmchEint,0,1U}, {SdVmcEnable,0,1U}
};
static const SD_POWER_STEP SdReleaseSteps[] = {
  {SdVmcEnable,0,1U}, {SdVmchEnable,1,1U}, {SdVmchEint,0,5U},
  {SdVmcVoltage,0,0xFFFU}, {SdVmchVoltage,0,0x700U}
};

#define CONN_VS2_VOTER       0x1724U
#define CONN_VS2_VOTER_SET   0x1726U
#define CONN_VS2_VOTER_CLEAR 0x1728U
#define CONN_VS2_VOTE        0x10U

/* See MtkConnRails.c for the original WMT power sequences. Each row owns
 * only the stock target field and can restore the value first observed. */
static const struct {
  uint16_t Address,Mask,Target;
} ConnRegisters[] = {
  {0x1C58U,1U,1U}, {0x1C5AU,3U,3U}, {0x1C60U,3U,2U},
  {0x1E8CU,0xF0FU,0xC00U},                   /* VCN18 1.8 V */
  {0x1D1CU,1U,1U}, {0x1D1EU,3U,3U}, {0x1D24U,3U,2U},
  {0x1E28U,0x30FU,0x100U},                   /* VCN33_BT 3.3 V */
  {0x172AU,0xFFFFU,0x46U}, {0x172EU,0xFFFFU,0x4DU}, {0x170AU,0xFFFFU,0x46U},
  {CONN_VS2_VOTER,CONN_VS2_VOTE,CONN_VS2_VOTE}
};
typedef char ConnRegisterCountMatches[
  sizeof(ConnRegisters)/sizeof(*ConnRegisters)==MTK_PMIC_CONN_REG_COUNT?1:-1];

static uint32_t ConnRegisterIndex(uint32_t Address)
{
  uint32_t Index;
  for (Index=0U; Index<MTK_PMIC_CONN_REG_COUNT; ++Index) {
    if (ConnRegisters[Index].Address==Address) break;
  }
  return Index;
}

static int ConnReadable(uint32_t Address)
{
  return Address==0x1C66U || Address==0x1D2CU || Address==0x1D2AU /* Wi-Fi vote */ ||
         Address==0x1720U || ConnRegisterIndex(Address)<MTK_PMIC_CONN_REG_COUNT;
}

static int ConnVoterAlias(uint32_t Address)
{
  return Address==CONN_VS2_VOTER_SET || Address==CONN_VS2_VOTER_CLEAR;
}

static uint32_t ConnWriteIndex(uint32_t Address)
{
  if (Address==CONN_VS2_VOTER) return MTK_PMIC_CONN_REG_COUNT;
  return ConnRegisterIndex(ConnVoterAlias(Address)?CONN_VS2_VOTER:Address);
}

int MtkPmicBrokerConnAddressAllowed(uint32_t Address)
{
  return ConnReadable(Address) || ConnVoterAlias(Address);
}

static int SdPowerOperation(uint32_t Operation)
{
  return Operation==MtkPmicBrokerAcquireSdPower || Operation==MtkPmicBrokerReleaseSdPower;
}

static const SD_POWER_STEP *SdPowerStep(const MTK_PMIC_BROKER *Core)
{
  uint32_t Index=Core->SdStage-SdRegisterCount;
  if (Core->Operation==MtkPmicBrokerAcquireSdPower)
    return Index<sizeof(SdEnableSteps)/sizeof(*SdEnableSteps)?&SdEnableSteps[Index]:NULL;
  return Index<sizeof(SdReleaseSteps)/sizeof(*SdReleaseSteps)?&SdReleaseSteps[Index]:NULL;
}

static int Valid(const MTK_PMIC_BROKER *Core)
{
  return Core!=NULL && Core->Signature==MTK_PMIC_BROKER_SIGNATURE;
}
static uint32_t AllowedMask(uint32_t Address)
{
  if (Address==0x1508U || Address==0x1BC8U) return 1U;
  if (Address==0x1526U || Address==0x1BF4U) return 0x7FU;
  return 0U;
}
static int Readable(uint32_t Address)
{
  /* MT6366 TOPSTATUS is a read-only power-button snapshot. */
  return Address==0x28U || AllowedMask(Address)!=0U || Address==0x1520U ||
         Address==0x1BE4U || Address==0x1BE2U;
}
typedef struct {
  uint16_t Alias,Base,Mask;
  uint8_t Set;
} PMIC_GAUGE_ALIAS;

static const PMIC_GAUGE_ALIAS GaugeAliases[] = {
  {0x012CU,0x012AU,0x0004U,1U}, {0x012EU,0x012AU,0x0004U,0U},
  {0x010EU,0x010CU,0x0010U,1U}, {0x0110U,0x010CU,0x0010U,0U}
};

static const PMIC_GAUGE_ALIAS *GaugeAlias(uint32_t Address)
{
  uint32_t Index;
  for (Index=0U;Index<sizeof(GaugeAliases)/sizeof(*GaugeAliases);++Index)
    if (GaugeAliases[Index].Alias==Address) return &GaugeAliases[Index];
  return NULL;
}

static int OtpFuseRowAllowed(uint32_t Value)
{
  return Value>=PMIC_OTP_FUSE_ROW_FIRST && Value<=PMIC_OTP_FUSE_ROW_LAST &&
         ((Value-PMIC_OTP_FUSE_ROW_FIRST)&1U)==0U;
}

static int GaugeReadable(uint32_t Address)
{
  return Address==PMIC_GAUGE_CTRL || Address==PMIC_GAUGE_CURRENT ||
         Address==PMIC_AUXADC_REQUEST || Address==PMIC_AUXADC_BAT_VOLTAGE ||
         Address==PMIC_AUXADC_BAT_TEMP || Address==PMIC_AUXADC_CHIP_TEMP ||
         Address==PMIC_AUXADC_BIF_VOLTAGE || Address==PMIC_AUXADC_BIF_CONTROL ||
         Address==0x012AU || Address==0x010CU ||
         Address==PMIC_AUXADC_DIE_TEMP_TRIM || Address==PMIC_AUXADC_TEMP_REF_TRIM ||
         Address==PMIC_AUXADC_TEMP_SLOPE_TRIM || Address==PMIC_OTP_ROW ||
         Address==PMIC_OTP_READ_TRIGGER || Address==PMIC_OTP_READ_ENABLE ||
         Address==PMIC_OTP_READ_DATA || Address==PMIC_OTP_READ_BUSY;
}
static int GaugeControlValueAllowed(uint32_t Value)
{
  return Value==0U || Value==1U || Value==8U || Value==0x8008U;
}
static uint32_t GaugeAliasDesired(const MTK_PMIC_BROKER *Core,
                                  const PMIC_GAUGE_ALIAS *Alias)
{
  return (Core->ReadValue&~Alias->Mask)|(Alias->Set?Alias->Mask:0U);
}

static int GaugeWriteAllowed(const MTK_PMIC_BROKER *Core)
{
  const PMIC_GAUGE_ALIAS *Alias;
  if (Core->ProtocolVersion!=MTK_PMIC_BROKER_VERSION_GAUGE || !Core->GaugeLeaseToken)
    return 0;
  Alias=GaugeAlias(Core->Address);
  if (Alias!=NULL)
    return Core->Mask==Alias->Mask && Core->GaugeWriteData==Alias->Mask &&
           Core->RequestedValue==GaugeAliasDesired(Core,Alias);
  if (Core->Address==PMIC_GAUGE_CTRL)
    return Core->Mask==PMIC_GAUGE_CONTROL_MASK &&
           GaugeControlValueAllowed(Core->RequestedValue);
  if (Core->Address==PMIC_AUXADC_BIF_CONTROL)
    return Core->Mask==PMIC_AUXADC_BIF_DISABLE &&
           Core->RequestedValue<=0xFFFFU &&
           ((Core->RequestedValue^Core->ReadValue)&~PMIC_AUXADC_BIF_DISABLE)==0U;
  if (Core->Address==PMIC_OTP_ROW)
    return Core->Mask==0xFFFFU && OtpFuseRowAllowed(Core->RequestedValue);
  if (Core->Address==PMIC_OTP_READ_TRIGGER || Core->Address==PMIC_OTP_READ_ENABLE)
    return Core->Mask==0xFFFFU && Core->RequestedValue<=1U;
  return Core->Address==PMIC_AUXADC_REQUEST && Core->Mask==0xFFFFU &&
         Core->RequestedValue!=0U &&
         (Core->RequestedValue&~PMIC_AUXADC_REQUEST_MASK)==0U &&
         (Core->RequestedValue&(Core->RequestedValue-1U))==0U;
}
static int NextGeneration(MTK_PMIC_BROKER *Core,uint32_t Operation,uint64_t *Generation)
{
  uint64_t SdReserve=(Core->SdPowerToken!=0U || Operation==MtkPmicBrokerAcquireSdPower)?1U:0U;
  uint64_t Limit=PMIC_NORMAL_GENERATION_LIMIT-SdReserve;
  if (Operation==MtkPmicBrokerReleaseKeyLease) Limit=UINT64_MAX-SdReserve;
  else if (Operation==MtkPmicBrokerAckKeySnapshot) Limit=UINT64_MAX-1U-SdReserve;
  else if (Operation==MtkPmicBrokerReleaseSdPower)
    Limit=UINT64_MAX-(Core->KeyLeaseToken!=0U)-(Core->KeySnapshotValid!=0U);
  if (Core->Generation>=Limit) return 0;
  *Generation=++Core->Generation;
  return 1;
}
static MTK_PMIC_BROKER_RESULT Finish(
  const MTK_PMIC_BROKER *Core, MTK_PMIC_BROKER_REQUEST *Request,
  MTK_PMIC_BROKER_RESULT Result)
{
  if (Request!=NULL) {
    Request->State=Valid(Core)?Core->State:MtkPmicBrokerStateFault;
    Request->Generation=Valid(Core)?Core->Generation:0;
    Request->Result=(uint32_t)Result;
    Request->Address=Valid(Core)?Core->Address:0;
    Request->Value=Valid(Core)?Core->Value:0;
  }
  return Result;
}
static MTK_PMIC_BROKER_RESULT Fault(MTK_PMIC_BROKER *Core,MTK_PMIC_BROKER_RESULT Result)
{
  /* Never discard a possibly accepted command, data write or response clear. */
  Core->State=MtkPmicBrokerStateFault;
  Core->FinalResult=(uint32_t)Result;
  return Result;
}
static MTK_PMIC_BROKER_RESULT Done(MTK_PMIC_BROKER *Core,MTK_PMIC_BROKER_RESULT Result)
{
  /* Reached only after the hardware channel is observed idle. The cached
   * result can await Ack while UFS uses WACS2; it cannot issue a new command. */
  Core->State=MtkPmicBrokerStateDone;
  Core->Phase=PhaseIdle;
  Core->FinalResult=(uint32_t)Result;
  if (Result==MtkPmicBrokerResultSuccess &&
      Core->Operation==MtkPmicBrokerAcquireKeyLease) {
    Core->KeyLeaseState=KeyLeaseActive;
    Core->KeyLeaseToken=Core->Generation;
    Core->LastReleasedLeaseToken=0;
    Core->LastReleasedJobGeneration=0;
  } else if (Result==MtkPmicBrokerResultSuccess &&
             Core->Operation==MtkPmicBrokerReleaseKeyLease) {
    Core->LastReleasedLeaseToken=Core->KeyLeaseToken;
    Core->LastReleasedJobGeneration=Core->Generation;
    Core->KeyLeaseState=KeyLeaseNone;
    Core->KeyLeaseToken=0;
    Core->KeyLeaseBaselineBits=0;
    Core->KeyLeaseOwnedMask=0;
  } else if (Result==MtkPmicBrokerResultSuccess &&
             Core->Operation==MtkPmicBrokerBeginKeySnapshot &&
             Core->ProtocolVersion==MTK_PMIC_BROKER_VERSION_KEY_IRQ) {
    Core->KeySnapshotValid=1U;
    Core->KeySnapshotStatus=Core->SnapshotGroupStatus&0xFFFFU;
    Core->KeySnapshotTopStatus=Core->Value&0xFFFFU;
    Core->KeySnapshotToken=Core->Generation;
    Core->KeySnapshotLeaseToken=Core->KeyLeaseToken;
    Core->KeySnapshotAckState=KeyAckNone;
    Core->KeySnapshotAckMask=0U;
    Core->KeySnapshotSiblingStatus=0U;
    Core->KeyAckWriteMayHaveEffect=0U;
    Core->LastAckedSnapshotToken=0U;
    Core->LastKeyAckJobGeneration=0U;
    Core->LastKeyAckValue=0U;
  } else if (Core->Operation==MtkPmicBrokerAckKeySnapshot &&
             Result==MtkPmicBrokerResultSiblingPending) {
    Core->KeySnapshotAckState=KeyAckSiblingPending;
  } else if (Core->Operation==MtkPmicBrokerAckKeySnapshot &&
             Result==MtkPmicBrokerResultSuccess) {
    Core->KeySnapshotAckState=KeyAckCompleted;
    Core->LastAckedSnapshotToken=Core->KeySnapshotToken;
    Core->LastKeyAckJobGeneration=Core->Generation;
    Core->LastKeyAckValue=Core->Value;
  }
  return Result;
}
static MTK_PMIC_BROKER_RESULT Read(
  MTK_PMIC_BROKER *Core,const MTK_PMIC_BROKER_IO *Io,uint32_t Offset,uint32_t *Value)
{
  MTK_PMIC_BROKER_IO_STATUS Result=Io->Read32(Io->Context,Offset,Value);
  if (Result==MtkPmicBrokerIoBusy) return MtkPmicBrokerResultBusy;
  if (Result!=MtkPmicBrokerIoOk) return Fault(Core,MtkPmicBrokerResultIoError);
  return MtkPmicBrokerResultSuccess;
}
static MTK_PMIC_BROKER_RESULT Write(
  MTK_PMIC_BROKER *Core,const MTK_PMIC_BROKER_IO *Io,uint32_t Offset,uint32_t Value)
{
  MTK_PMIC_BROKER_IO_STATUS Status;
  /* Even Busy on a side-effecting write is uncertain. Never replay it. */
  Core->WriteCallbackActive=1U;
  Status=Io->Write32(Io->Context,Offset,Value);
  Core->WriteCallbackActive=0U;
  if (Status!=MtkPmicBrokerIoOk)
    return Fault(Core,MtkPmicBrokerResultIoError);
  return MtkPmicBrokerResultInProgress;
}
static uint32_t ReadCommandAddress(const MTK_PMIC_BROKER *Core)
{
  if (Core->Operation==MtkPmicBrokerAckKeySnapshot) return PMIC_KEY_STATUS;
  if (Core->Operation==MtkPmicBrokerBeginConnWrite && ConnVoterAlias(Core->Address))
    return CONN_VS2_VOTER;
  if (Core->Operation==MtkPmicBrokerBeginGaugeWrite) {
    const PMIC_GAUGE_ALIAS *Alias=GaugeAlias(Core->Address);
    if (Alias!=NULL) return Alias->Base;
  }
  if (Core->Operation!=MtkPmicBrokerBeginKeySnapshot) return Core->Address;
  switch (Core->SnapshotStage) {
    case 0U: return PMIC_KEY_SUMMARY;
    case 1U: return PMIC_KEY_STATUS;
    case 2U: return PMIC_TOP_STATUS;
    default: return UINT32_MAX;
  }
}

static uint32_t ConnDesired(const MTK_PMIC_BROKER *Core)
{
  return Core->Address==CONN_VS2_VOTER_CLEAR?0U:Core->RequestedValue&Core->Mask;
}

static MTK_PMIC_BROKER_RESULT ReadGaugeAliasWrite(
  MTK_PMIC_BROKER *Core,int Verify)
{
  const PMIC_GAUGE_ALIAS *Alias=GaugeAlias(Core->Address);
  if (Alias==NULL || Core->Mask!=Alias->Mask || Core->GaugeWriteData!=Alias->Mask)
    return Done(Core,MtkPmicBrokerResultInvalid);
  if (!Verify) {
    Core->RequestedValue=GaugeAliasDesired(Core,Alias);
    Core->Value=Core->GaugeWriteData;
    if (Core->ReadValue==Core->RequestedValue)
      return Done(Core,MtkPmicBrokerResultSuccess);
    Core->Phase=PhaseWriteData;
    return MtkPmicBrokerResultInProgress;
  }
  Core->Value=Core->GaugeWriteData;
  return Done(Core,Core->ReadValue==Core->RequestedValue?
      MtkPmicBrokerResultSuccess:MtkPmicBrokerResultIoError);
}

static MTK_PMIC_BROKER_RESULT ReadConnWrite(
  MTK_PMIC_BROKER *Core,int Verify)
{
  uint32_t Index=ConnWriteIndex(Core->Address);
  if (Index>=MTK_PMIC_CONN_REG_COUNT) return Fault(Core,MtkPmicBrokerResultIoError);
  uint32_t Bit=1U<<Index, Live=Core->ReadValue&Core->Mask, Wanted=ConnDesired(Core);
  uint32_t Saved=(Core->ConnCaptured&Bit)?Core->ConnBaseline[Index]:Live;

  if (!Verify) {
    if ((!ConnVoterAlias(Core->Address) &&
         ((Core->ReadValue^Core->RequestedValue)&~Core->Mask)!=0U) ||
        (Wanted!=Saved && Wanted!=ConnRegisters[Index].Target))
      return Done(Core,MtkPmicBrokerResultInvalid);
    Core->ConnBaseline[Index]=(uint16_t)Saved;
    Core->ConnCaptured|=Bit;
    if (Live!=Wanted) {
      /* Preserve the original field even when the write response is lost. */
      Core->ConnDirty|=Bit;
      Core->Phase=PhaseWriteData;
      return MtkPmicBrokerResultInProgress;
    }
  }
  if (Live==Saved) Core->ConnDirty&=~Bit;
  else Core->ConnDirty|=Bit;
  return Done(Core,Live==Wanted?MtkPmicBrokerResultSuccess:MtkPmicBrokerResultIoError);
}

static MTK_PMIC_BROKER_RESULT NextSdPowerStage(
  MTK_PMIC_BROKER *Core,const MTK_PMIC_BROKER_IO *Io)
{
  ++Core->SdStage;
  if (Core->SdStage<SdRegisterCount) {
    Core->Address=SdRegisters[Core->SdStage].Address;
  } else {
    const SD_POWER_STEP *Step=SdPowerStep(Core);
    if (Step==NULL) {
      if (Core->Operation==MtkPmicBrokerAcquireSdPower) {
        Core->SdPowerToken=Core->Generation;
        Core->Value=1U;
      } else {
        Core->SdReleasedToken=Core->SdPowerToken;
        Core->SdReleaseGeneration=Core->Generation;
        Core->SdPowerToken=0U;
      }
      Core->Address=0U;
      return Done(Core,MtkPmicBrokerResultSuccess);
    }
    Core->Address=SdRegisters[Step->Register].Address;
  }
  Core->StartedUs=Io->NowUs(Io->Context);
  Core->PollCount=0U;
  Core->Phase=PhaseReadStart;
  return MtkPmicBrokerResultInProgress;
}

static MTK_PMIC_BROKER_RESULT ReadSdPower(
  MTK_PMIC_BROKER *Core,const MTK_PMIC_BROKER_IO *Io)
{
  int Acquire=Core->Operation==MtkPmicBrokerAcquireSdPower;
  uint32_t Index=Core->SdStage;
  if (Index<SdRegisterCount) {
    uint32_t Bits=Core->ReadValue&SdRegisters[Index].Mask;
    if (Acquire) {
      Core->SdBaseline[Index]=Bits;
      if (Index==SdVmcEnable) {
        uint32_t *Saved=Core->SdBaseline;
        if ((Saved[SdVmchEint]&1U)!=Saved[SdVmchEnable] ||
            (Saved[SdVmchEnable] && (Saved[SdVmchEint]!=1U || Saved[SdVmchVoltage]!=0x500U)) ||
            (Saved[SdVmcEnable] && Saved[SdVmcVoltage]!=0xD00U))
          return Done(Core,MtkPmicBrokerResultWrongState);
      }
    } else if (Bits!=SdRegisters[Index].Enabled) {
      Core->SdPowerFault=1U;
      return Done(Core,MtkPmicBrokerResultIoError);
    }
    return NextSdPowerStage(Core,Io);
  }
  const SD_POWER_STEP *Step=SdPowerStep(Core);
  if (Step==NULL) return Fault(Core,MtkPmicBrokerResultIoError);
  Core->Mask=Step->Mask;
  uint32_t Wanted=Acquire?SdRegisters[Step->Register].Enabled:Core->SdBaseline[Step->Register];
  Core->RequestedValue=(Core->ReadValue&~Core->Mask)|(Wanted&Core->Mask);
  if (Core->RequestedValue!=Core->ReadValue) {
    if ((Step->Register==SdVmchVoltage && Core->SdBaseline[SdVmchEnable]) ||
        (Step->Register==SdVmcVoltage && Core->SdBaseline[SdVmcEnable])) {
      Core->SdPowerFault=1U;
      return Done(Core,MtkPmicBrokerResultIoError);
    }
    Core->Phase=PhaseWriteData;
    return MtkPmicBrokerResultInProgress;
  }
  return NextSdPowerStage(Core,Io);
}

static MTK_PMIC_BROKER_RESULT Step(
  MTK_PMIC_BROKER *Core,const MTK_PMIC_BROKER_IO *Io)
{
  uint32_t Raw,Fsm;
  uint64_t Now=Io->NowUs(Io->Context);
  MTK_PMIC_BROKER_RESULT Result;
  if (Core->Phase==PhaseSdPowerDelay) {
    if (Now<Core->StartedUs) {
      Core->SdPowerFault=1U;
      return Done(Core,MtkPmicBrokerResultIoError);
    }
    return Now-Core->StartedUs>=1500U?NextSdPowerStage(Core,Io):MtkPmicBrokerResultInProgress;
  }
  if (Now<Core->StartedUs || Now-Core->StartedUs>PMIC_TIMEOUT_US ||
      ++Core->PollCount>PMIC_MAX_POLLS) return Fault(Core,MtkPmicBrokerResultTimeout);

  if (Core->Phase==PhaseReadStart || Core->Phase==PhaseVerifyStart) {
    uint32_t ReadAddress=ReadCommandAddress(Core);
    if (ReadAddress==UINT32_MAX)
      return Fault(Core,MtkPmicBrokerResultIoError);
    Result=Read(Core,Io,0U,&Raw);
    if (Result!=MtkPmicBrokerResultSuccess) return Result;
    if (Raw!=1U) return Fault(Core,MtkPmicBrokerResultIoError);
    Result=Read(Core,Io,WACS_STATUS,&Raw);
    if (Result!=MtkPmicBrokerResultSuccess) return Result;
    if (Raw==UINT32_MAX) return Fault(Core,MtkPmicBrokerResultIoError);
    /* An unread response belongs to an unknown client. Never clear it. */
    if (((Raw>>1)&7U)!=0U) return MtkPmicBrokerResultBusy;
    Core->Phase=Core->Phase==PhaseReadStart?PhaseReadWait:PhaseVerifyWait;
    return Write(Core,Io,WACS_COMMAND,ReadAddress);
  }
  if (Core->Phase==PhaseWriteData) {
    uint32_t WriteValue=(Core->Operation==MtkPmicBrokerBeginGaugeWrite &&
                         GaugeAlias(Core->Address)!=NULL)?
                        Core->GaugeWriteData:Core->RequestedValue;
    Core->Phase=PhaseWriteCommand;
    return Write(Core,Io,WACS_WRITE_DATA,WriteValue);
  }
  if (Core->Phase==PhaseWriteCommand) {
    Core->Phase=PhaseWriteWait;
    return Write(Core,Io,WACS_COMMAND,0x20000000U|Core->Address);
  }
  if (Core->Phase==PhaseKeyAckWriteData) {
    Core->Phase=PhaseKeyAckWriteCommand;
    return Write(Core,Io,WACS_WRITE_DATA,Core->KeySnapshotAckMask);
  }
  if (Core->Phase==PhaseKeyAckWriteCommand) {
    /* A failed command write may have acknowledged the latched event. Keep
     * the journal faulted and never issue this W1C a second time. */
    Core->KeyAckWriteMayHaveEffect=1U;
    Core->Phase=PhaseKeyAckWriteWait;
    return Write(Core,Io,WACS_COMMAND,0x20000000U|PMIC_KEY_STATUS);
  }
  if (Core->Phase==PhaseReadClear || Core->Phase==PhaseVerifyClear) {
    Core->Phase=Core->Phase==PhaseReadClear?PhaseReadDrain:PhaseVerifyDrain;
    return Write(Core,Io,WACS_VALID_CLEAR,1U);
  }
  Result=Read(Core,Io,WACS_STATUS,&Raw);
  if (Result!=MtkPmicBrokerResultSuccess) return Result;
  if (Raw==UINT32_MAX) return Fault(Core,MtkPmicBrokerResultIoError);
  Fsm=(Raw>>1)&7U;
  if (Core->Phase==PhaseReadWait || Core->Phase==PhaseVerifyWait) {
    if (Fsm==0U) return Fault(Core,MtkPmicBrokerResultIoError);
    if (Fsm!=6U) return MtkPmicBrokerResultInProgress;
    Result=Read(Core,Io,WACS_READ_DATA,&Core->ReadValue);
    if (Result!=MtkPmicBrokerResultSuccess) return Result;
    Core->ReadValue&=0xFFFFU;
    Core->Phase=Core->Phase==PhaseReadWait?PhaseReadClear:PhaseVerifyClear;
    return MtkPmicBrokerResultInProgress;
  }
  if (Core->Phase==PhaseReadDrain) {
    if (Fsm!=0U) return MtkPmicBrokerResultInProgress;
    if (SdPowerOperation(Core->Operation)) return ReadSdPower(Core,Io);
    if (Core->Operation==MtkPmicBrokerBeginConnWrite) return ReadConnWrite(Core,0);
    if (Core->Operation==MtkPmicBrokerBeginGaugeWrite && GaugeAlias(Core->Address)!=NULL)
      return ReadGaugeAliasWrite(Core,0);
    if (Core->Operation==MtkPmicBrokerBeginKeySnapshot) {
      if (Core->SnapshotStage==0U) {
        if ((Core->ReadValue&PMIC_KEY_SUMMARY_GROUP2)!=0U) {
          Core->SnapshotStage=1U;
        } else {
          Core->SnapshotGroupStatus=0U;
          Core->SnapshotStage=2U;
        }
        Core->Phase=PhaseReadStart;
        return MtkPmicBrokerResultInProgress;
      }
      if (Core->SnapshotStage==1U) {
        Core->SnapshotGroupStatus=Core->ReadValue&0xFFFFU;
        Core->SnapshotStage=2U;
        Core->Phase=PhaseReadStart;
        return MtkPmicBrokerResultInProgress;
      }
      if (Core->SnapshotStage==2U) {
        Core->Value=((Core->SnapshotGroupStatus&0xFFFFU)<<16)|
                    (Core->ReadValue&0xFFFFU);
        return Done(Core,MtkPmicBrokerResultSuccess);
      }
      return Fault(Core,MtkPmicBrokerResultIoError);
    }
    if (Core->Operation==MtkPmicBrokerAckKeySnapshot) {
      uint32_t SiblingBits=Core->ReadValue&~PMIC_KEY_IRQ_MASK;
      if (SiblingBits!=0U) {
        Core->KeySnapshotSiblingStatus=SiblingBits;
        return Done(Core,MtkPmicBrokerResultSiblingPending);
      }
      Core->KeySnapshotAckMask=Core->KeySnapshotStatus&PMIC_KEY_IRQ_MASK;
      Core->KeySnapshotAckState=KeyAckWriting;
      Core->KeyAckWriteMayHaveEffect=0U;
      if (Core->KeySnapshotAckMask==0U)
        return Done(Core,MtkPmicBrokerResultSuccess);
      Core->Phase=PhaseKeyAckWriteData;
      return MtkPmicBrokerResultInProgress;
    }
    if (Core->Operation==MtkPmicBrokerBeginRead || Core->Operation==MtkPmicBrokerBeginConnRead ||
        Core->Operation==MtkPmicBrokerBeginGaugeRead) {
      Core->Value=Core->ReadValue;
      return Done(Core,MtkPmicBrokerResultSuccess);
    }
    if (Core->Operation==MtkPmicBrokerAcquireKeyLease) {
      Core->KeyLeaseBaselineBits=Core->ReadValue&PMIC_KEY_IRQ_MASK;
      Core->KeyLeaseOwnedMask=(~Core->ReadValue)&PMIC_KEY_IRQ_MASK;
      Core->RequestedValue=Core->ReadValue|PMIC_KEY_IRQ_MASK;
      Core->Mask=PMIC_KEY_IRQ_MASK;
      if (Core->RequestedValue==Core->ReadValue)
        return Done(Core,MtkPmicBrokerResultSuccess);
      Core->Phase=PhaseWriteData;
      return MtkPmicBrokerResultInProgress;
    }
    if (Core->Operation==MtkPmicBrokerReleaseKeyLease) {
      Core->RequestedValue=Core->ReadValue&~Core->KeyLeaseOwnedMask;
      Core->Mask=Core->KeyLeaseOwnedMask;
      if (Core->RequestedValue==Core->ReadValue)
        return Done(Core,MtkPmicBrokerResultSuccess);
      Core->Phase=PhaseWriteData;
      return MtkPmicBrokerResultInProgress;
    }
    if (((Core->RequestedValue^Core->ReadValue)&~Core->Mask)!=0U)
      return Done(Core,MtkPmicBrokerResultInvalid);
    if (Core->RequestedValue==Core->ReadValue)
      return Done(Core,MtkPmicBrokerResultSuccess);
    Core->Phase=PhaseWriteData;
    return MtkPmicBrokerResultInProgress;
  }
  if (Core->Phase==PhaseWriteWait) {
    if (Fsm!=0U) return MtkPmicBrokerResultInProgress;
    Core->Phase=PhaseVerifyStart;
    return MtkPmicBrokerResultInProgress;
  }
  if (Core->Phase==PhaseKeyAckWriteWait) {
    if (Fsm!=0U) return MtkPmicBrokerResultInProgress;
    /* W1C has no safe readback: a same-bit event can coalesce with the
     * captured edge while the bit is set. The captured mask is sent once. */
    return Done(Core,MtkPmicBrokerResultSuccess);
  }
  if (Core->Phase==PhaseVerifyDrain) {
    if (Fsm!=0U) return MtkPmicBrokerResultInProgress;
    if (Core->Operation==MtkPmicBrokerBeginConnWrite) return ReadConnWrite(Core,1);
    if (Core->Operation==MtkPmicBrokerBeginGaugeWrite && GaugeAlias(Core->Address)!=NULL)
      return ReadGaugeAliasWrite(Core,1);
    if (Core->Operation==MtkPmicBrokerBeginGaugeWrite)
      return Done(Core,((Core->ReadValue^Core->RequestedValue)&~0x8000U)==0U?
        MtkPmicBrokerResultSuccess:MtkPmicBrokerResultIoError);
    if (SdPowerOperation(Core->Operation)) {
      const SD_POWER_STEP *PowerStep=SdPowerStep(Core);
      if (PowerStep==NULL || ((Core->ReadValue^Core->RequestedValue)&Core->Mask)!=0U) {
        /* The channel is idle. Preserve UFS access and quarantine this lease. */
        Core->SdPowerFault=1U;
        return Done(Core,MtkPmicBrokerResultIoError);
      }
      if (PowerStep->DelayAfter) {
        Core->StartedUs=Now;
        Core->PollCount=0U;
        Core->Phase=PhaseSdPowerDelay;
        return MtkPmicBrokerResultInProgress;
      }
      return NextSdPowerStage(Core,Io);
    }
    if (Core->Operation==MtkPmicBrokerAcquireKeyLease) {
      if ((Core->ReadValue&PMIC_KEY_IRQ_MASK)!=PMIC_KEY_IRQ_MASK)
        return Fault(Core,MtkPmicBrokerResultIoError);
      return Done(Core,MtkPmicBrokerResultSuccess);
    }
    if (Core->Operation==MtkPmicBrokerReleaseKeyLease) {
      if ((Core->ReadValue&Core->KeyLeaseOwnedMask)!=0U)
        return Fault(Core,MtkPmicBrokerResultIoError);
      return Done(Core,MtkPmicBrokerResultSuccess);
    }
    return Done(Core,Core->ReadValue==Core->RequestedValue?
      MtkPmicBrokerResultSuccess:MtkPmicBrokerResultIoError);
  }
  return Fault(Core,MtkPmicBrokerResultIoError);
}
void MtkPmicBrokerInitialize(MTK_PMIC_BROKER *Core)
{
  if (Core==NULL) return;
  *Core=(MTK_PMIC_BROKER){0};
  Core->Signature=MTK_PMIC_BROKER_SIGNATURE;
}
int MtkPmicBrokerBlocksUfs(const MTK_PMIC_BROKER *Core)
{
  return !Valid(Core) || Core->Phase!=PhaseIdle || Core->GaugeLeaseToken!=0U ||
         (Core->State!=MtkPmicBrokerStateIdle && Core->State!=MtkPmicBrokerStateDone);
}

static int WireOperationSupported(uint32_t Version,uint32_t Operation)
{
  switch (Version) {
    case MTK_PMIC_BROKER_VERSION: return Operation<=MtkPmicBrokerBeginKeySnapshot;
    case MTK_PMIC_BROKER_VERSION_KEY_IRQ: return Operation<=MtkPmicBrokerAckKeySnapshot;
    case MTK_PMIC_BROKER_VERSION_SD_POWER:
      return SdPowerOperation(Operation) || Operation==MtkPmicBrokerQuery ||
             Operation==MtkPmicBrokerStep || Operation==MtkPmicBrokerAck;
    case MTK_PMIC_BROKER_VERSION_CONN_RAILS:
      return (Operation>=MtkPmicBrokerAcquireConnRails && Operation<=MtkPmicBrokerBeginConnWrite) ||
             Operation==MtkPmicBrokerQuery || Operation==MtkPmicBrokerStep || Operation==MtkPmicBrokerAck;
    case MTK_PMIC_BROKER_VERSION_GAUGE:
      return (Operation>=MtkPmicBrokerAcquireConnRails && Operation<=MtkPmicBrokerBeginGaugeWrite) ||
             Operation==MtkPmicBrokerQuery || Operation==MtkPmicBrokerStep || Operation==MtkPmicBrokerAck;
    default: return 0;
  }
}

MTK_PMIC_BROKER_RESULT MtkPmicBrokerHandle(
  MTK_PMIC_BROKER *Core,const MTK_PMIC_BROKER_IO *Io,int OtherOwned,
  MTK_PMIC_BROKER_REQUEST *Request)
{
  uint32_t Operation,Version;
  uint64_t InputGeneration,NewGeneration;
  MTK_PMIC_BROKER_RESULT Result;
  if (!Valid(Core) || Io==NULL || Io->Read32==NULL || Io->Write32==NULL ||
      Io->NowUs==NULL || Request==NULL)
    return Finish(Core,Request,MtkPmicBrokerResultInvalid);
  Operation=Request->Operation;
  Version=Request->Version;
  InputGeneration=Request->Generation;
  if (Request->Size!=sizeof(*Request) || !WireOperationSupported(Version,Operation) ||
      Request->Flags || Request->State || Request->Result ||
      Request->Reserved0 || Request->Reserved[0] || Request->Reserved[1] || Request->Reserved[2])
    return Finish(Core,Request,MtkPmicBrokerResultInvalid);
  if (Operation==MtkPmicBrokerQuery && Version!=MTK_PMIC_BROKER_VERSION) {
    /* This is a wire-capability probe only. It never samples hardware or
     * changes the current journal. */
    if (InputGeneration || Request->Address || Request->Value)
      return Finish(Core,Request,MtkPmicBrokerResultInvalid);
    Request->State=MtkPmicBrokerStateIdle;
    Request->Generation=0U;
    Request->Result=MtkPmicBrokerResultSuccess;
    Request->Address=0U;
    Request->Value=0U;
    if (Version==MTK_PMIC_BROKER_VERSION_KEY_IRQ)
      Request->Flags=MTK_PMIC_BROKER_CAP_KEY_IRQ_LEASE | MTK_PMIC_BROKER_CAP_CAPTURED_KEY_ACK;
    else if (Version==MTK_PMIC_BROKER_VERSION_SD_POWER)
      Request->Flags=MTK_PMIC_BROKER_CAP_SD_POWER_LEASE;
    else if (Version==MTK_PMIC_BROKER_VERSION_CONN_RAILS)
      Request->Flags=MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE;
    else
      Request->Flags=MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE|MTK_PMIC_BROKER_CAP_GAUGE_LEASE;
    return (MTK_PMIC_BROKER_RESULT)Request->Result;
  }
  if (Core->State>MtkPmicBrokerStateFault ||
      ((Core->State==MtkPmicBrokerStateIdle || Core->State==MtkPmicBrokerStateDone) &&
       Core->Phase!=PhaseIdle))
    return Finish(Core,Request,Fault(Core,MtkPmicBrokerResultIoError));
  if (Operation==MtkPmicBrokerQuery) {
    if (InputGeneration || Request->Address || Request->Value)
      return Finish(Core,Request,MtkPmicBrokerResultInvalid);
    return Finish(Core,Request,(MTK_PMIC_BROKER_RESULT)Core->FinalResult);
  }
  if (Core->GaugeLeaseToken!=0U &&
      Operation!=MtkPmicBrokerStep && Operation!=MtkPmicBrokerAck &&
      Operation!=MtkPmicBrokerReleaseGaugeLease &&
      Operation!=MtkPmicBrokerBeginGaugeRead && Operation!=MtkPmicBrokerBeginGaugeWrite) {
    Request->State=MtkPmicBrokerStateIdle;
    Request->Generation=InputGeneration;
    Request->Result=MtkPmicBrokerResultBusy;
    return MtkPmicBrokerResultBusy;
  }
  if (Operation==MtkPmicBrokerReleaseGaugeLease) {
    if (Version!=MTK_PMIC_BROKER_VERSION_GAUGE || !InputGeneration ||
        Request->Address || Request->Value)
      return Finish(Core,Request,MtkPmicBrokerResultInvalid);
    if (Core->State!=MtkPmicBrokerStateIdle)
      return Finish(Core,Request,MtkPmicBrokerResultBusy);
    if (OtherOwned) {
      Request->State=MtkPmicBrokerStateIdle;
      Request->Generation=InputGeneration;
      Request->Result=MtkPmicBrokerResultBusy;
      Request->Address=0U;
      Request->Value=0U;
      return MtkPmicBrokerResultBusy;
    }
    if (!Core->GaugeLeaseToken && InputGeneration==Core->GaugeReleasedToken &&
        Core->GaugeReleasedToken!=0U) {
      Request->State=MtkPmicBrokerStateIdle;
      Request->Generation=Core->Generation;
      Request->Result=MtkPmicBrokerResultSuccess;
      Request->Address=0U;
      Request->Value=0U;
      return MtkPmicBrokerResultSuccess;
    }
    if (!Core->GaugeLeaseToken)
      return Finish(Core,Request,MtkPmicBrokerResultWrongState);
    if (InputGeneration!=Core->GaugeLeaseToken)
      return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
    Core->GaugeReleasedToken=Core->GaugeLeaseToken;
    Core->GaugeLeaseToken=0U;
    Request->State=MtkPmicBrokerStateIdle;
    Request->Generation=Core->Generation;
    Request->Result=MtkPmicBrokerResultSuccess;
    Request->Address=0U;
    Request->Value=0U;
    return MtkPmicBrokerResultSuccess;
  }
  if (Operation==MtkPmicBrokerReleaseConnRails) {
    if (!InputGeneration || Request->Address || Request->Value)
      return Finish(Core,Request,MtkPmicBrokerResultInvalid);
    if (Core->State!=MtkPmicBrokerStateIdle)
      return Finish(Core,Request,MtkPmicBrokerResultBusy);
    if (!Core->ConnLeaseToken && InputGeneration==Core->ConnReleasedToken)
      return MtkPmicBrokerResultSuccess;
    if (InputGeneration!=Core->ConnLeaseToken)
      return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
    if (Core->ConnDirty) return Finish(Core,Request,MtkPmicBrokerResultWrongState);
    Core->ConnReleasedToken=Core->ConnLeaseToken;
    Core->ConnLeaseToken=0U;
    Core->ConnCaptured=0U;
    return MtkPmicBrokerResultSuccess;
  }
  if (Operation==MtkPmicBrokerBeginRead || Operation==MtkPmicBrokerBeginWrite ||
      Operation==MtkPmicBrokerBeginKeySnapshot ||
      Operation==MtkPmicBrokerAcquireKeyLease ||
      Operation==MtkPmicBrokerReleaseKeyLease ||
      Operation==MtkPmicBrokerAckKeySnapshot ||
      Operation==MtkPmicBrokerAcquireConnRails ||
      Operation==MtkPmicBrokerBeginConnRead || Operation==MtkPmicBrokerBeginConnWrite ||
      Operation==MtkPmicBrokerAcquireGaugeLease ||
      Operation==MtkPmicBrokerBeginGaugeRead || Operation==MtkPmicBrokerBeginGaugeWrite ||
      SdPowerOperation(Operation)) {
    uint32_t Address=0U,Value=0U,Mask=0U;
    int StartsHardware=1;
    if (Request->Address || Request->Value) {
      if (Operation!=MtkPmicBrokerBeginRead && Operation!=MtkPmicBrokerBeginWrite &&
          Operation!=MtkPmicBrokerBeginConnRead && Operation!=MtkPmicBrokerBeginConnWrite &&
          Operation!=MtkPmicBrokerBeginGaugeRead && Operation!=MtkPmicBrokerBeginGaugeWrite)
        return Finish(Core,Request,MtkPmicBrokerResultInvalid);
    }
    if (Core->State!=MtkPmicBrokerStateIdle)
      return Finish(Core,Request,MtkPmicBrokerResultBusy);
    if (SdPowerOperation(Operation)) {
      if (Core->SdPowerFault) return Finish(Core,Request,MtkPmicBrokerResultWrongState);
      if (Operation==MtkPmicBrokerAcquireSdPower) {
        if (InputGeneration) return Finish(Core,Request,MtkPmicBrokerResultInvalid);
        if (Core->SdPowerToken) return Finish(Core,Request,MtkPmicBrokerResultBusy);
      } else {
        if (InputGeneration && InputGeneration==Core->SdReleasedToken && !Core->SdPowerToken) {
          Request->State=MtkPmicBrokerStateIdle;
          Request->Generation=Core->SdReleaseGeneration;
          Request->Result=MtkPmicBrokerResultSuccess;
          return MtkPmicBrokerResultSuccess;
        }
        if (!Core->SdPowerToken) return Finish(Core,Request,MtkPmicBrokerResultWrongState);
        if (InputGeneration!=Core->SdPowerToken)
          return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
      }
      Address=SdRegisters[0].Address;
    } else if (Operation==MtkPmicBrokerAcquireConnRails) {
      if (InputGeneration) return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      if (Core->ConnLeaseToken) return Finish(Core,Request,MtkPmicBrokerResultBusy);
      StartsHardware=0;
    } else if (Operation==MtkPmicBrokerBeginConnRead || Operation==MtkPmicBrokerBeginConnWrite) {
      if (!Core->ConnLeaseToken) return Finish(Core,Request,MtkPmicBrokerResultWrongState);
      if (InputGeneration!=Core->ConnLeaseToken)
        return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
      Address=Request->Address;
      if (Operation==MtkPmicBrokerBeginConnRead) {
        if (Request->Value || !ConnReadable(Address))
          return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      } else {
        uint32_t Index=ConnWriteIndex(Address);
        Value=Request->Value;
        if (Index>=MTK_PMIC_CONN_REG_COUNT || Value>0xFFFFU ||
            (ConnVoterAlias(Address) && Value!=CONN_VS2_VOTE))
          return Finish(Core,Request,MtkPmicBrokerResultInvalid);
        Mask=ConnRegisters[Index].Mask;
      }
    } else if (Operation==MtkPmicBrokerAcquireGaugeLease) {
      if (Version!=MTK_PMIC_BROKER_VERSION_GAUGE || InputGeneration ||
          Request->Address || Request->Value)
        return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      if (Core->GaugeLeaseToken || Core->ConnLeaseToken || Core->SdPowerToken ||
          Core->KeyLeaseState!=KeyLeaseNone || Core->KeySnapshotValid)
        return Finish(Core,Request,MtkPmicBrokerResultBusy);
      StartsHardware=0;
    } else if (Operation==MtkPmicBrokerBeginGaugeRead ||
               Operation==MtkPmicBrokerBeginGaugeWrite) {
      if (Version!=MTK_PMIC_BROKER_VERSION_GAUGE || !Core->GaugeLeaseToken)
        return Finish(Core,Request,MtkPmicBrokerResultWrongState);
      if (InputGeneration!=Core->GaugeLeaseToken)
        return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
      Address=Request->Address;
      Value=Request->Value;
      if (Operation==MtkPmicBrokerBeginGaugeRead) {
        if (Value || !GaugeReadable(Address))
          return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      } else {
        if (Address==PMIC_GAUGE_CTRL && GaugeControlValueAllowed(Value))
          Mask=PMIC_GAUGE_CONTROL_MASK;
        else if (Address==PMIC_AUXADC_REQUEST && Value!=0U &&
                 (Value&~PMIC_AUXADC_REQUEST_MASK)==0U && (Value&(Value-1U))==0U)
          Mask=0xFFFFU;
        else if (Address==PMIC_AUXADC_BIF_CONTROL && Value<=0xFFFFU)
          Mask=PMIC_AUXADC_BIF_DISABLE;
        else if (Address==PMIC_OTP_ROW && OtpFuseRowAllowed(Value))
          Mask=0xFFFFU;
        else if ((Address==PMIC_OTP_READ_TRIGGER || Address==PMIC_OTP_READ_ENABLE) && Value<=1U)
          Mask=0xFFFFU;
        else {
          const PMIC_GAUGE_ALIAS *Alias=GaugeAlias(Address);
          if (Alias==NULL || Value!=Alias->Mask)
            return Finish(Core,Request,MtkPmicBrokerResultInvalid);
          Mask=Alias->Mask;
        }
        /* Keep the alias's write-one payload separate from the preserved
         * base-register image used for readback. */
        Core->GaugeWriteData=Value;
        if (Mask==0U)
          return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      }
    } else if (Operation==MtkPmicBrokerBeginRead || Operation==MtkPmicBrokerBeginWrite) {
      if (InputGeneration || !Readable(Request->Address) || Request->Value>0xFFFFU ||
          (Operation==MtkPmicBrokerBeginRead && Request->Value) ||
          (Operation==MtkPmicBrokerBeginWrite && !AllowedMask(Request->Address)))
        return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      Address=Request->Address;
      Value=Operation==MtkPmicBrokerBeginWrite?Request->Value:0U;
      Mask=AllowedMask(Address);
    } else if (Operation==MtkPmicBrokerBeginKeySnapshot) {
      if (Request->Address || Request->Value)
        return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      if (Version==MTK_PMIC_BROKER_VERSION) {
        if (InputGeneration) return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      } else {
        if (Core->KeyLeaseState!=KeyLeaseActive || Core->KeyLeaseToken==0U)
          return Finish(Core,Request,MtkPmicBrokerResultWrongState);
        if (InputGeneration!=Core->KeyLeaseToken)
          return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
        if (Core->KeySnapshotValid)
          return Finish(Core,Request,MtkPmicBrokerResultBusy);
      }
      Address=PMIC_KEY_STATUS;
    } else if (Operation==MtkPmicBrokerAcquireKeyLease) {
      if (Version!=MTK_PMIC_BROKER_VERSION_KEY_IRQ || InputGeneration ||
          Request->Address || Request->Value)
        return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      if (Core->KeyLeaseState!=KeyLeaseNone)
        return Finish(Core,Request,MtkPmicBrokerResultBusy);
      Address=0x910U;
      Value=PMIC_KEY_IRQ_MASK;
      Mask=PMIC_KEY_IRQ_MASK;
    } else if (Operation==MtkPmicBrokerReleaseKeyLease) {
      if (Version!=MTK_PMIC_BROKER_VERSION_KEY_IRQ || !InputGeneration ||
          Request->Address || Request->Value)
        return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      if (Core->KeyLeaseState==KeyLeaseNone &&
          InputGeneration==Core->LastReleasedLeaseToken &&
          Core->LastReleasedLeaseToken!=0U) {
        /* A lost successful response can be retried without touching PMIC. */
        Request->State=MtkPmicBrokerStateIdle;
        Request->Generation=Core->LastReleasedJobGeneration;
        Request->Result=MtkPmicBrokerResultSuccess;
        Request->Address=0x910U;
        Request->Value=PMIC_KEY_IRQ_MASK;
        return MtkPmicBrokerResultSuccess;
      }
      if (Core->KeyLeaseState!=KeyLeaseActive)
        return Finish(Core,Request,MtkPmicBrokerResultWrongState);
      if (InputGeneration!=Core->KeyLeaseToken)
        return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
      if (Core->KeySnapshotValid)
        return Finish(Core,Request,MtkPmicBrokerResultWrongState);
      Address=0x910U;
      Value=PMIC_KEY_IRQ_MASK;
      Mask=Core->KeyLeaseOwnedMask;
      if (Mask==0U) StartsHardware=0;
    } else {
      if (Version!=MTK_PMIC_BROKER_VERSION_KEY_IRQ || !InputGeneration ||
          Request->Address || Request->Value)
        return Finish(Core,Request,MtkPmicBrokerResultInvalid);
      if (!Core->KeySnapshotValid) {
        if (InputGeneration==Core->LastAckedSnapshotToken && InputGeneration!=0U) {
          Request->State=MtkPmicBrokerStateIdle;
          Request->Generation=Core->LastKeyAckJobGeneration;
          Request->Result=MtkPmicBrokerResultSuccess;
          Request->Address=PMIC_KEY_STATUS;
          Request->Value=Core->LastKeyAckValue;
          return MtkPmicBrokerResultSuccess;
        }
        return Finish(Core,Request,MtkPmicBrokerResultWrongState);
      }
      if (InputGeneration!=Core->KeySnapshotToken)
        return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
      if (Core->KeyLeaseState!=KeyLeaseActive ||
          Core->KeySnapshotLeaseToken!=Core->KeyLeaseToken)
        return Finish(Core,Request,MtkPmicBrokerResultWrongState);
      if (Core->KeySnapshotAckState==KeyAckCompleted)
        return Finish(Core,Request,MtkPmicBrokerResultSuccess);
      Address=PMIC_KEY_STATUS;
      Value=Core->Value;
    }
    if (OtherOwned) {
      if (Operation==MtkPmicBrokerAcquireGaugeLease ||
          Operation==MtkPmicBrokerBeginGaugeRead ||
          Operation==MtkPmicBrokerBeginGaugeWrite) {
        Request->State=MtkPmicBrokerStateIdle;
        Request->Generation=InputGeneration;
        Request->Result=MtkPmicBrokerResultBusy;
        return MtkPmicBrokerResultBusy;
      }
      return Finish(Core,Request,MtkPmicBrokerResultBusy);
    }
    if (!NextGeneration(Core,Operation,&NewGeneration))
      return Finish(Core,Request,MtkPmicBrokerResultExhausted);
    Core->ProtocolVersion=Version;
    Core->Operation=Operation;
    Core->Address=Address;
    Core->RequestedValue=Value;
    Core->Value=Value;
    Core->Mask=Mask;
    Core->ReadValue=0U;
    Core->PollCount=0U;
    Core->SdStage=0U;
    Core->SnapshotStage=0U;
    Core->SnapshotGroupStatus=0U;
    Core->StartedUs=Io->NowUs(Io->Context);
    Core->FinalResult=MtkPmicBrokerResultInProgress;
    if (Operation==MtkPmicBrokerAcquireConnRails) {
      Core->ConnLeaseToken=NewGeneration;
      Core->ConnReleasedToken=0U;
    } else if (Operation==MtkPmicBrokerAcquireGaugeLease) {
      Core->GaugeLeaseToken=NewGeneration;
      Core->GaugeReleasedToken=0U;
    } else if (Operation==MtkPmicBrokerAcquireKeyLease) {
      Core->KeyLeaseState=KeyLeaseAcquiring;
      Core->KeyLeaseBaselineBits=0U;
      Core->KeyLeaseOwnedMask=0U;
      Core->KeyLeaseToken=0U;
      Core->LastReleasedLeaseToken=0U;
      Core->LastReleasedJobGeneration=0U;
      Core->LastAckedSnapshotToken=0U;
      Core->LastKeyAckJobGeneration=0U;
      Core->LastKeyAckValue=0U;
    } else if (Operation==MtkPmicBrokerBeginKeySnapshot &&
               Version==MTK_PMIC_BROKER_VERSION_KEY_IRQ) {
      Core->LastAckedSnapshotToken=0U;
      Core->LastKeyAckJobGeneration=0U;
      Core->LastKeyAckValue=0U;
    } else if (Operation==MtkPmicBrokerAckKeySnapshot) {
      Core->KeySnapshotAckMask=0U;
      Core->KeySnapshotSiblingStatus=0U;
      Core->KeySnapshotAckState=KeyAckReading;
    }
    if (Operation==MtkPmicBrokerAckKeySnapshot)
      Core->Value=Core->KeySnapshotStatus<<16 | Core->KeySnapshotTopStatus;
    if (StartsHardware) {
      Core->Phase=PhaseReadStart;
      Core->State=MtkPmicBrokerStatePending;
      return Finish(Core,Request,MtkPmicBrokerResultInProgress);
    }
    Core->Phase=PhaseIdle;
    Core->State=MtkPmicBrokerStateDone;
    Result=Done(Core,MtkPmicBrokerResultSuccess);
    return Finish(Core,Request,Result);
  }
  if (Request->Address || Request->Value)
    return Finish(Core,Request,MtkPmicBrokerResultInvalid);
  if (!InputGeneration)
    return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
  if (Version!=Core->ProtocolVersion)
    return Finish(Core,Request,MtkPmicBrokerResultWrongState);
  if (Operation==MtkPmicBrokerAck) {
    if (Core->State==MtkPmicBrokerStateIdle && InputGeneration==Core->LastAckGeneration)
      return Finish(Core,Request,MtkPmicBrokerResultSuccess);
    if (InputGeneration!=Core->Generation)
      return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
    if (Core->State!=MtkPmicBrokerStateDone)
      return Finish(Core,Request,MtkPmicBrokerResultWrongState);
    Core->LastAckGeneration=InputGeneration;
    if (Core->Operation==MtkPmicBrokerAckKeySnapshot) {
      if (Core->FinalResult==MtkPmicBrokerResultSuccess &&
          Core->KeySnapshotAckState==KeyAckCompleted) {
        Core->KeySnapshotValid=0U;
        Core->KeySnapshotStatus=0U;
        Core->KeySnapshotTopStatus=0U;
        Core->KeySnapshotToken=0U;
        Core->KeySnapshotLeaseToken=0U;
        Core->KeySnapshotAckMask=0U;
        Core->KeySnapshotSiblingStatus=0U;
        Core->KeySnapshotAckState=KeyAckNone;
      } else if (Core->FinalResult==MtkPmicBrokerResultSiblingPending) {
        Core->KeySnapshotAckState=KeyAckNone;
      }
    }
    Core->State=MtkPmicBrokerStateIdle;
    Core->FinalResult=MtkPmicBrokerResultSuccess;
    return Finish(Core,Request,MtkPmicBrokerResultSuccess);
  }
  if (Operation!=MtkPmicBrokerStep)
    return Finish(Core,Request,MtkPmicBrokerResultInvalid);
  if (InputGeneration!=Core->Generation)
    return Finish(Core,Request,MtkPmicBrokerResultStaleGeneration);
  if (Core->State==MtkPmicBrokerStateDone || Core->State==MtkPmicBrokerStateFault)
    return Finish(Core,Request,(MTK_PMIC_BROKER_RESULT)Core->FinalResult);
  if (Core->State!=MtkPmicBrokerStatePending)
    return Finish(Core,Request,MtkPmicBrokerResultWrongState);
  if (OtherOwned) return Finish(Core,Request,MtkPmicBrokerResultBusy);
  return Finish(Core,Request,Step(Core,Io));
}

static int ConnWriteAllowed(const MTK_PMIC_BROKER *Core)
{
  uint32_t Index=ConnWriteIndex(Core->Address);
  return Core->ProtocolVersion==MTK_PMIC_BROKER_VERSION_CONN_RAILS && Core->ConnLeaseToken &&
         Index<MTK_PMIC_CONN_REG_COUNT && Core->Mask==ConnRegisters[Index].Mask &&
         (Core->ConnCaptured&Core->ConnDirty&(1U<<Index))!=0U &&
         (ConnDesired(Core)==Core->ConnBaseline[Index] ||
          ConnDesired(Core)==ConnRegisters[Index].Target) &&
         (ConnVoterAlias(Core->Address)?Core->RequestedValue==CONN_VS2_VOTE:
          ((Core->RequestedValue^Core->ReadValue)&~Core->Mask)==0U);
}

int MtkPmicBrokerWacsWriteAllowed(
  const MTK_PMIC_BROKER *Core,uint32_t Offset,uint32_t Value)
{
  uint32_t Address;
  if (!Valid(Core) || Core->WriteCallbackActive!=1U) return 0;
  if (Offset==WACS_VALID_CLEAR)
    return Value==1U &&
      (Core->Phase==PhaseReadDrain || Core->Phase==PhaseVerifyDrain);
  if (Offset==WACS_WRITE_DATA) {
    if (Core->Phase==PhaseWriteCommand &&
        Value==((Core->Operation==MtkPmicBrokerBeginGaugeWrite &&
                 GaugeAlias(Core->Address)!=NULL)?Core->GaugeWriteData:Core->RequestedValue))
      return Core->Operation==MtkPmicBrokerBeginWrite ||
             (Core->Operation==MtkPmicBrokerBeginGaugeWrite && GaugeWriteAllowed(Core)) ||
             (Core->Operation==MtkPmicBrokerBeginConnWrite && ConnWriteAllowed(Core)) ||
             Core->Operation==MtkPmicBrokerAcquireKeyLease ||
             Core->Operation==MtkPmicBrokerReleaseKeyLease ||
             (SdPowerOperation(Core->Operation) &&
              Core->ProtocolVersion==MTK_PMIC_BROKER_VERSION_SD_POWER);
    return Core->Phase==PhaseKeyAckWriteCommand &&
           Core->Operation==MtkPmicBrokerAckKeySnapshot &&
           Value==Core->KeySnapshotAckMask && Value!=0U &&
           (Value&~PMIC_KEY_IRQ_MASK)==0U;
  }
  if (Offset!=WACS_COMMAND || (Value&~0x2000FFFFU)!=0U)
    return 0;
  Address=Value&0xFFFFU;
  if ((Value&0x20000000U)==0U) {
    if ((Core->Operation==MtkPmicBrokerBeginConnRead ||
         Core->Operation==MtkPmicBrokerBeginConnWrite) &&
        (Core->ProtocolVersion!=MTK_PMIC_BROKER_VERSION_CONN_RAILS ||
         !Core->ConnLeaseToken || !ConnReadable(Address))) return 0;
    if ((Core->Operation==MtkPmicBrokerBeginGaugeRead ||
         Core->Operation==MtkPmicBrokerBeginGaugeWrite) &&
        (Core->ProtocolVersion!=MTK_PMIC_BROKER_VERSION_GAUGE ||
         !Core->GaugeLeaseToken || !GaugeReadable(Address))) return 0;
    return (Core->Phase==PhaseReadWait || Core->Phase==PhaseVerifyWait) &&
           Address==ReadCommandAddress(Core);
  }
  if (Core->Phase==PhaseWriteWait && Address==Core->Address &&
      Value==(0x20000000U|Core->Address)) {
    if (Core->Operation==MtkPmicBrokerBeginWrite)
      return AllowedMask(Address)!=0U;
    if (Core->Operation==MtkPmicBrokerBeginConnWrite)
      return ConnWriteAllowed(Core);
    if (Core->Operation==MtkPmicBrokerBeginGaugeWrite)
      return GaugeWriteAllowed(Core);
    if (Core->Operation==MtkPmicBrokerAcquireKeyLease ||
        Core->Operation==MtkPmicBrokerReleaseKeyLease)
      return Address==0x910U && Core->ProtocolVersion==MTK_PMIC_BROKER_VERSION_KEY_IRQ;
    if (SdPowerOperation(Core->Operation) && Core->ProtocolVersion==MTK_PMIC_BROKER_VERSION_SD_POWER) {
      const SD_POWER_STEP *PowerStep=SdPowerStep(Core);
      return PowerStep!=NULL && Address==SdRegisters[PowerStep->Register].Address &&
             Core->Mask==PowerStep->Mask && ((Core->RequestedValue^Core->ReadValue)&~Core->Mask)==0U;
    }
  }
  return Core->Phase==PhaseKeyAckWriteWait &&
         Core->Operation==MtkPmicBrokerAckKeySnapshot &&
         Address==PMIC_KEY_STATUS && Value==(0x20000000U|PMIC_KEY_STATUS) &&
         Core->KeySnapshotAckMask!=0U &&
         (Core->KeySnapshotAckMask&~PMIC_KEY_IRQ_MASK)==0U;
}
