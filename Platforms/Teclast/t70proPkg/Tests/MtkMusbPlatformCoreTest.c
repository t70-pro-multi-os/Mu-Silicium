// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdio.h>
#include <string.h>
#include <Library/MtkMusbPlatformCore.h>

#define REG_INTRTX       0x002U
#define REG_INTRRX       0x004U
#define REG_INTRTXE      0x006U
#define REG_INTRRXE      0x008U
#define REG_INTRUSB      0x00AU
#define REG_INTRUSBE     0x00BU
#define REG_INDEX        0x00EU
#define REG_POWER        0x001U
#define REG_TXCSR        0x012U
#define REG_RXCSR        0x016U
#define REG_DEVCTL       0x060U
#define REG_SWRST        0x074U
#define REG_TXFIFOSZ     0x062U
#define REG_RXFIFOSZ     0x063U
#define REG_TXFIFOADD    0x064U
#define REG_RXFIFOADD    0x066U
#define REG_HSDMA_INTR   0x200U
#define REG_HSDMA_CTRL0  0x204U
#define REG_QMU_GCSR     0x300U
#define DEVCTL_SESSION   0x01U
#define DEVCTL_HOSTMODE  0x04U
#define DEVCTL_VBUS_VALID 0x18U
#define POWER_ENSUSPEND  0x01U
#define POWER_SUSPENDM   0x02U
#define POWER_RESUME     0x04U
#define POWER_RESET      0x08U
#define POWER_HSENAB     0x20U
#define POWER_SOFTCONN   0x40U
#define DEVCTL_READ_ONLY (DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID | 0x80U)
#define TEST_PHY_DTM1    0x224U
#define TEST_PHY_ACR1    0x4400U
#define TEST_PHY_ACR6    0x50100075U
#define TEST_GPIO_COUNT  MTK_MUSB_PLATFORM_GPIO_COUNT

#define CHECK(Expression) do {   ++Checks;   if (!(Expression)) {     printf("MtkMusbPlatformCoreTest:%u failed: %s\n", (unsigned)__LINE__, #Expression);     return 0;   } } while (0)

typedef struct {
  uint32_t Offset;
  uint32_t Width;
  uint32_t Value;
} WRITE_TRACE;

typedef struct {
  uint8_t Registers[0x10000];
  uint8_t Index;
  uint8_t FifoSize[16][2];
  uint16_t FifoAddress[16][2];
  uint16_t TxCsr[16];
  uint16_t RxCsr[16];
  unsigned ReadCount;
  unsigned WriteCount;
  unsigned FifoWriteCount;
  unsigned Ep0FifoAccess;
  unsigned LockCount;
  int Locked;
  int FailReadOffset;
  int FailWriteOffset;
  int ArmFailReadAfterWriteOffset;
  int FailReadOnce;
  int FailWriteOnce;
  int FailWriteAfterOffset;
  int FailWriteAfterOnce;
  int IgnoreAck;
  unsigned AckWrites;
  WRITE_TRACE IrqTrace[3];
  unsigned IrqWrites, IgnoreIrqWrite;
  int ActivateDmaAfterIrqWrite;
  WRITE_TRACE CsrTrace[24];
  uint8_t CsrEndpoint[24];
  unsigned CsrWrites, IgnoreCsrWrite, FailCsrBefore, FailCsrAfter, FailCsrReadback;
  unsigned ResetWrites;
  WRITE_TRACE ResetTrace;
  WRITE_TRACE ColdTrace[32];
  unsigned ColdTraceCount;
  WRITE_TRACE ColdResetTrace[12];
  unsigned ColdResetTraceCount;
  uint16_t DmaControlAfterClear[8];
  unsigned ResetResponse;
  uint32_t ResetStickyMask;
  int ResetResponseInjected;
} MOCK;

typedef struct {
  uint64_t OwnerToken;
  uint64_t DataRoleToken;
  uint64_t Sequence;
  uint32_t DataRole;
  uint32_t PhyMode;
  uint32_t HostPowerPath;
  uint64_t IdLowRoleToken;
  uint32_t AcpiColdFlags, AcpiClockIdentity, PhyLength;
  uint32_t ChangeClockIdentityOnGuardCall;
  uint32_t PhyState;
  uint32_t PhyPowerCleanupPending;
  uint32_t FailPhase;
  uint32_t ReadyAfterPolls;
  uint32_t ChangeRoleTokenOnGuardCall;
  uint32_t ChangePowerPathOnGuardCall;
  uint32_t ChangeIdLowTokenOnGuardCall;
  unsigned GuardCalls;
  unsigned DelayCalls;
  uint64_t NowUs;
  int ClockFailed;
  unsigned SettleCalls, OtgHoldCalls;
  unsigned FailStartupDelay;
  unsigned SessionSetFailure, Reset32FailAt, Reset32Count, Reset32After;
  unsigned DropSessionBeforeReset, ResetSessionFault;
  unsigned PhyCalls;
  unsigned HostPhyAtRegisterWrite;
  int FailGuardOnce;
  int FailPhyOnce;
  int FailPhyFaultOnce;
  int FailDelay;
  int DeviceIdHigh;
  int HostModeStuck;
  int HostAfterPower;
  /* Replay observed POWER responses independently of host readiness. */
  unsigned ReplayMode, ReplayPowerReads;
  int ReplayPowerWritten;
  uint8_t PowerAfterPhy;
  int PowerAfterPhyValid;
  int DmaAfterPhy;
  int BadTypecOnlyGpio;
  int BadTypecOnlySource;
  int CallbackWhileLocked;
} COLD_MOCK;

static MOCK Bus;
static COLD_MOCK Cold;
static unsigned Checks;
static WRITE_TRACE Trial99Trace[2][3];
static unsigned Trial99PhyAtWrite[2];
typedef struct {
  unsigned StartStatus, StartStep, GuardPhase, Power, Devctl, PowerVerified;
  unsigned StopStatus, StopWrites;
} PHYSICAL_REPLAY;
static PHYSICAL_REPLAY Trial101Replay[2][6];

static uint16_t
ReadRegister16(uint32_t Offset)
{
  return (uint16_t)((uint16_t)Bus.Registers[Offset] |
                    ((uint16_t)Bus.Registers[Offset + 1U] << 8));
}

static void
WriteRegister16(uint32_t Offset, uint16_t Value)
{
  Bus.Registers[Offset] = (uint8_t)(Value & 0xFFU);
  Bus.Registers[Offset + 1U] = (uint8_t)(Value >> 8);
}

static int
FailRead(uint32_t Offset)
{
  if ((Bus.FailReadOffset >= 0) && ((uint32_t)Bus.FailReadOffset == Offset)) {
    if (Bus.FailReadOnce) Bus.FailReadOffset = -1;
    return 1;
  }
  return 0;
}

static int
FailWrite(uint32_t Offset)
{
  if ((Bus.FailWriteOffset >= 0) && ((uint32_t)Bus.FailWriteOffset == Offset)) {
    if (Bus.FailWriteOnce) Bus.FailWriteOffset = -1;
    return 1;
  }
  return 0;
}

static int
FailWriteAfter(uint32_t Offset)
{
  if ((Bus.FailWriteAfterOffset >= 0) &&
      ((uint32_t)Bus.FailWriteAfterOffset == Offset)) {
    if (Bus.FailWriteAfterOnce) Bus.FailWriteAfterOffset = -1;
    return 1;
  }
  return 0;
}

static int
Read8(void *Context, uint32_t Offset, uint8_t *Value)
{
  (void)Context;
  ++Bus.ReadCount;
  if ((Value == NULL) || (Offset >= sizeof(Bus.Registers)) || FailRead(Offset)) return 0;
  if (Offset == REG_POWER && Cold.ReplayPowerWritten) {
    if (Cold.ReplayMode == 2U ||
        (Cold.ReplayMode == 3U && Cold.ReplayPowerReads >= 1U)) {
      Bus.Registers[REG_POWER] |= POWER_SUSPENDM;
    }
    ++Cold.ReplayPowerReads;
  }
  if (Bus.Index == 0U && (Offset == REG_TXFIFOSZ || Offset == REG_RXFIFOSZ)) {
    ++Bus.Ep0FifoAccess; *Value = 0U; return 1;
  }
  if (Offset == REG_INDEX) {
    *Value = Bus.Index;
  } else if (Offset == REG_TXFIFOSZ) {
    *Value = Bus.FifoSize[Bus.Index][0];
  } else if (Offset == REG_RXFIFOSZ) {
    *Value = Bus.FifoSize[Bus.Index][1];
  } else {
    *Value = Bus.Registers[Offset];
  }
  return 1;
}

static int
Read16(void *Context, uint32_t Offset, uint16_t *Value)
{
  (void)Context;
  ++Bus.ReadCount;
  if ((Value == NULL) || (Offset + 1U >= sizeof(Bus.Registers)) || FailRead(Offset)) return 0;
  if (Bus.Index == 0U && (Offset == REG_TXFIFOADD || Offset == REG_RXFIFOADD)) {
    ++Bus.Ep0FifoAccess; *Value = 0U; return 1;
  }
  if (Offset == REG_TXCSR) {
    *Value = Bus.TxCsr[Bus.Index];
  } else if (Offset == REG_RXCSR) {
    *Value = Bus.RxCsr[Bus.Index];
  } else if (Offset == REG_TXFIFOADD) {
    *Value = Bus.FifoAddress[Bus.Index][0];
  } else if (Offset == REG_RXFIFOADD) {
    *Value = Bus.FifoAddress[Bus.Index][1];
  } else {
    *Value = ReadRegister16(Offset);
  }
  return 1;
}

static int
Read32(void *Context, uint32_t Offset, uint32_t *Value)
{
  (void)Context;
  ++Bus.ReadCount;
  if ((Value == NULL) || (Offset + 3U >= sizeof(Bus.Registers)) || FailRead(Offset)) return 0;
  *Value = (uint32_t)Bus.Registers[Offset] |
           ((uint32_t)Bus.Registers[Offset + 1U] << 8) |
           ((uint32_t)Bus.Registers[Offset + 2U] << 16) |
           ((uint32_t)Bus.Registers[Offset + 3U] << 24);
  return 1;
}

static void
RecordColdReset(uint32_t Offset, uint32_t Width, uint32_t Value)
{
  if (Bus.ColdResetTraceCount < 12U)
    Bus.ColdResetTrace[Bus.ColdResetTraceCount++] = (WRITE_TRACE){Offset,Width,Value};
}

static int
Write8(void *Context, uint32_t Offset, uint8_t Value)
{
  int FailAfter;
  (void)Context;
  ++Bus.WriteCount;
  if ((Offset >= sizeof(Bus.Registers)) || FailWrite(Offset)) return 0;
  FailAfter = FailWriteAfter(Offset);
  if (Bus.Index == 0U && (Offset == REG_TXFIFOSZ || Offset == REG_RXFIFOSZ)) {
    ++Bus.Ep0FifoAccess; return 1;
  }
  if (Offset == REG_INDEX) {
    Bus.Index = Value;
  } else if (Offset == REG_TXFIFOSZ) {
    Bus.FifoSize[Bus.Index][0] = Value;
    ++Bus.FifoWriteCount;
  } else if (Offset == REG_RXFIFOSZ) {
    Bus.FifoSize[Bus.Index][1] = Value;
    ++Bus.FifoWriteCount;
  } else if (Offset == REG_HSDMA_INTR) {
    ++Bus.AckWrites;
    if (!Bus.IgnoreAck) Bus.Registers[Offset] &= (uint8_t)~Value;
  } else if (Offset == REG_INTRUSBE) {
    if (Bus.IrqWrites < 3U) Bus.IrqTrace[Bus.IrqWrites] = (WRITE_TRACE){Offset,8U,Value};
    ++Bus.IrqWrites;
    if (Bus.IrqWrites != Bus.IgnoreIrqWrite &&
        !(Bus.ResetResponseInjected && Bus.ResetStickyMask == Offset))
      Bus.Registers[Offset] = Value;
    if (Bus.ActivateDmaAfterIrqWrite) Bus.Registers[REG_HSDMA_CTRL0] = 1U;
  } else if (Offset == REG_DEVCTL) {
    Bus.Registers[Offset] = (uint8_t)((Bus.Registers[Offset] & DEVCTL_READ_ONLY) |
                                      (Value & (uint8_t)~DEVCTL_READ_ONLY));
  } else {
    Bus.Registers[Offset] = Value;
  }
  if (Offset == REG_SWRST) RecordColdReset(Offset,8U,Value);
  if (Offset == REG_POWER && Cold.ReplayMode != 0U) {
    Cold.ReplayPowerWritten = 1;
    Cold.ReplayPowerReads = 0U;
    if (Cold.ReplayMode == 1U) Bus.Registers[REG_POWER] |= POWER_SUSPENDM;
  }
  if (Offset == REG_POWER && Cold.HostAfterPower && !Cold.HostModeStuck &&
      Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST && (Value & 0x6fU) == 0x21U) {
    Bus.Registers[REG_DEVCTL] = (Bus.Registers[REG_DEVCTL] & (uint8_t)~0x80U) |
                               DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID;
  }
  if ((Offset == REG_DEVCTL) || (Offset == REG_POWER)) {
    if (Bus.ColdTraceCount < sizeof(Bus.ColdTrace) / sizeof(Bus.ColdTrace[0])) {
      WRITE_TRACE *Trace = &Bus.ColdTrace[Bus.ColdTraceCount++];
      Trace->Offset = Offset;
      Trace->Width = 8U;
      Trace->Value = Value;
    }
  }
  if ((Bus.ArmFailReadAfterWriteOffset >= 0) &&
      ((uint32_t)Bus.ArmFailReadAfterWriteOffset == Offset)) {
    Bus.ArmFailReadAfterWriteOffset = -1;
    Bus.FailReadOffset = (int)Offset;
    Bus.FailReadOnce = 1;
  }
  if (Offset == REG_DEVCTL && (Value & 1U) && Cold.SessionSetFailure) {
    unsigned Failure=Cold.SessionSetFailure;Cold.SessionSetFailure=0U;
    if (Failure==1U) return 0;
    Bus.FailReadOffset=REG_DEVCTL;Bus.FailReadOnce=1;
  }
  return FailAfter ? 0 : 1;
}

static int
Write16(void *Context, uint32_t Offset, uint16_t Value)
{
  int FailAfter;
  (void)Context;
  ++Bus.WriteCount;
  if ((Offset + 1U >= sizeof(Bus.Registers)) || FailWrite(Offset)) return 0;
  FailAfter = FailWriteAfter(Offset);
  if (Bus.Index == 0U && (Offset == REG_TXFIFOADD || Offset == REG_RXFIFOADD)) {
    ++Bus.Ep0FifoAccess; return 1;
  }
  if (Offset == REG_SWRST || (Offset >= 0x204U && Offset <= 0x274U && ((Offset-0x204U)%16U)==0U))
    RecordColdReset(Offset,16U,Value);
  if (Offset == REG_SWRST) {
    ++Bus.ResetWrites;
    Bus.ResetTrace = (WRITE_TRACE){Offset,16U,Value};
    WriteRegister16(Offset,Value);
    if (Bus.ResetResponse) {
      unsigned Model=Bus.ResetResponse;
      Bus.ResetResponseInjected=1;
      if (Model==1U || Model==3U) {
        Bus.Registers[REG_INTRUSBE]=0x12U;
        WriteRegister16(REG_INTRTXE,0x01ffU);WriteRegister16(REG_INTRRXE,0x01feU);
      }
      if (Model>=2U) {
        WriteRegister16(REG_INTRTX,0x1234U);WriteRegister16(REG_INTRRX,0x42U);
        Bus.Registers[REG_HSDMA_INTR]=0xa5U;
      }
      if (Model==4U) WriteRegister16(REG_HSDMA_CTRL0+7U*16U,0x13U);
      if (Model==5U) Bus.TxCsr[0]=8U;
      if (Model==6U) Bus.Registers[REG_QMU_GCSR+2U]=1U;
      if (Model>=7U && Model<=9U) {
        Bus.ResetStickyMask=Model==7U?REG_INTRUSBE:Model==8U?REG_INTRTXE:REG_INTRRXE;
        if(Model==7U) Bus.Registers[REG_INTRUSBE]=0x12U;
        else WriteRegister16(Bus.ResetStickyMask,Model==8U?0x1ffU:0x1feU);
      }
      if (Model>=10U && Model<=12U) {
        Bus.FailWriteOffset=Model==10U?REG_INTRUSBE:Model==11U?REG_INTRTXE:REG_INTRRXE;
        Bus.FailWriteOnce=1;
      }
    }
  } else if (Offset == REG_TXCSR || Offset == REG_RXCSR) {
    uint16_t *Csr=Offset==REG_TXCSR?&Bus.TxCsr[Bus.Index]:&Bus.RxCsr[Bus.Index];
    uint16_t Wzc=Offset==REG_TXCSR?0x00a6U:0x0045U;
    if (Bus.CsrWrites < 24U) {
      Bus.CsrTrace[Bus.CsrWrites]=(WRITE_TRACE){Offset,16U,Value};
      Bus.CsrEndpoint[Bus.CsrWrites]=Bus.Index;
    }
    ++Bus.CsrWrites;
    if(Bus.FailCsrBefore==Bus.CsrWrites)return 0;
    if(Bus.IgnoreCsrWrite!=Bus.CsrWrites)
      *Csr=(uint16_t)((Value&~Wzc)|(*Csr&Value&Wzc));
    if(Bus.FailCsrReadback==Bus.CsrWrites) {
      Bus.FailReadOffset=(int)Offset;Bus.FailReadOnce=1;
    }
    if(Bus.FailCsrAfter==Bus.CsrWrites)return 0;
  } else if (Offset == REG_INTRTXE || Offset == REG_INTRRXE) {
    if (Bus.IrqWrites < 3U) Bus.IrqTrace[Bus.IrqWrites] = (WRITE_TRACE){Offset,16U,Value};
    ++Bus.IrqWrites;
    if (Bus.IrqWrites != Bus.IgnoreIrqWrite &&
        !(Bus.ResetResponseInjected && Bus.ResetStickyMask == Offset))
      WriteRegister16(Offset,Value);
  } else if (Offset == REG_TXFIFOADD) {
    Bus.FifoAddress[Bus.Index][0] = Value;
    ++Bus.FifoWriteCount;
  } else if (Offset == REG_RXFIFOADD) {
    Bus.FifoAddress[Bus.Index][1] = Value;
    ++Bus.FifoWriteCount;
  } else if ((Offset == REG_INTRTX) || (Offset == REG_INTRRX)) {
    ++Bus.AckWrites;
    if (!Bus.IgnoreAck) WriteRegister16(Offset, (uint16_t)(ReadRegister16(Offset) & ~Value));
  } else if (Offset >= REG_HSDMA_CTRL0 && Offset <= REG_HSDMA_CTRL0 + 7U * 16U &&
             ((Offset - REG_HSDMA_CTRL0) % 16U) == 0U) {
    WriteRegister16(Offset, (uint16_t)(Value | Bus.DmaControlAfterClear[(Offset - REG_HSDMA_CTRL0) / 16U]));
  } else {
    WriteRegister16(Offset, Value);
  }
  if ((Bus.ArmFailReadAfterWriteOffset >= 0) &&
      ((uint32_t)Bus.ArmFailReadAfterWriteOffset == Offset)) {
    Bus.ArmFailReadAfterWriteOffset = -1;
    Bus.FailReadOffset = (int)Offset; Bus.FailReadOnce = 1;
  }
  return FailAfter ? 0 : 1;
}

static int
Write32(void *Context, uint32_t Offset, uint32_t Value)
{
  (void)Context;
  ++Bus.WriteCount;
  if ((Offset + 3U >= sizeof(Bus.Registers)) || FailWrite(Offset)) return 0;
  int ResetFailure=Offset==0x204U && ++Cold.Reset32Count==Cold.Reset32FailAt;
  if (ResetFailure && !Cold.Reset32After) return 0;
  Bus.Registers[Offset] = (uint8_t)Value;
  Bus.Registers[Offset + 1U] = (uint8_t)(Value >> 8);
  Bus.Registers[Offset + 2U] = (uint8_t)(Value >> 16);
  Bus.Registers[Offset + 3U] = (uint8_t)(Value >> 24);
  if (Offset == 0x204U) RecordColdReset(Offset,32U,Value);
  return (ResetFailure || FailWriteAfter(Offset)) ? 0 : 1;
}

static int
TryLock(void *Context)
{
  (void)Context;
  ++Bus.LockCount;
  if (Bus.Locked) return 0;
  Bus.Locked = 1;
  return 1;
}

static void
Unlock(void *Context)
{
  (void)Context;
  Bus.Locked = 0;
}

static MTK_MUSB_PLATFORM_IO
MakeIo(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  memset(&Io, 0, sizeof(Io));
  Io.Context = &Bus;
  Io.ControllerLength = MTK_MUSB_PLATFORM_CONTROLLER_BYTES;
  Io.Read8 = Read8;
  Io.Read16 = Read16;
  Io.Read32 = Read32;
  Io.Write8 = Write8;
  Io.Write16 = Write16;
  Io.Write32 = Write32;
  Io.TryLock = TryLock;
  Io.Unlock = Unlock;
  return Io;
}

static void
ResetBus(void)
{
  memset(&Bus, 0, sizeof(Bus));
  Bus.Index = 7U;
  Bus.FailReadOffset = -1;
  Bus.FailWriteOffset = -1;
  Bus.ArmFailReadAfterWriteOffset = -1;
  Bus.FailWriteAfterOffset = -1;
}

static void
SetRegister8(uint32_t Offset, uint8_t Value)
{
  Bus.Registers[Offset] = Value;
}

static void
SetRegister16(uint32_t Offset, uint16_t Value)
{
  WriteRegister16(Offset, Value);
}

static MTK_MUSB_PLATFORM_EVIDENCE
MakeEvidence(void)
{
  MTK_MUSB_PLATFORM_EVIDENCE E;
  static const uint32_t Pins[TEST_GPIO_COUNT] = {
    MTK_MUSB_PLATFORM_GPIO_ID_PIN,
    MTK_MUSB_PLATFORM_GPIO_MUX_PIN,
    MTK_MUSB_PLATFORM_GPIO_5V_PIN,
    MTK_MUSB_PLATFORM_GPIO_RESET_PIN,
    MTK_MUSB_PLATFORM_GPIO_OTG_PIN
  };
  uint32_t I;

  memset(&E, 0, sizeof(E));
  E.ControllerBase = MTK_MUSB_PLATFORM_CONTROLLER_BASE;
  E.ControllerLength = MTK_MUSB_PLATFORM_CONTROLLER_BYTES;
  E.PhyBase = MTK_MUSB_PLATFORM_PHY_BASE;
  E.PhyLength = MTK_MUSB_PLATFORM_PHY_BYTES;
  E.ControllerMapped = 1U;
  E.PhyMapped = 1U;
  E.ClockReadMask = MTK_MUSB_PLATFORM_CLOCK_MASK;
  E.ClockEnabledMask = MTK_MUSB_PLATFORM_CLOCK_MASK;
  for (I = 0U; I < 3U; ++I) {
    E.ClockHz[I] = (I == 0U) ? 26000000ULL : 124800000ULL;
    E.ExpectedClockHz[I] = E.ClockHz[I];
  }
  E.RefClockHz = 26000000ULL;
  E.RefClockEnabled = 1U;
  E.TopUsbParent = 3U;
  E.ExpectedTopUsbParent = 3U;
  E.TopUsbFrequencyHz = 124800000ULL;
  E.ExpectedTopUsbFrequencyHz = 124800000ULL;
  for (I = 0U; I < TEST_GPIO_COUNT; ++I) {
    E.Gpio[I].Number = Pins[I];
    E.Gpio[I].Mode = 0U;
    E.Gpio[I].Direction = (I == 0U) ? MTK_MUSB_PLATFORM_GPIO_INPUT : MTK_MUSB_PLATFORM_GPIO_OUTPUT;
    E.Gpio[I].PhysicalInput = (I == 0U) ? 0U : 1U;
  }
  E.PhyState = MTK_MUSB_PLATFORM_PHY_STATE_UNOBSERVED;
  E.PhyReadMask = 0x0FU;
  E.PhyDtm0 = 0U;
  E.PhyDtm1 = TEST_PHY_DTM1;
  E.PhyAcr1 = TEST_PHY_ACR1;
  E.PhyAcr6 = TEST_PHY_ACR6;
  E.Sc8989xBoostReadValid = 1U;
  E.Sc8989xBoostEnabled = MTK_MUSB_PLATFORM_SC8989X_BOOST_ENABLED;
  E.Husb311PowerStatusReadValid = 1U;
  E.Husb311CcStatusReadValid = 1U;
  E.Husb311CcStatus = (uint32_t)MTK_MUSB_PLATFORM_HUSB311_SRC_RD;
  E.Husb311RoleControl = MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
                         MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
                         (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
                          MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT);
  E.Husb311PowerStatus = MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
                         MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT;
  E.InterruptVector = MTK_MUSB_PLATFORM_GSIV;
  E.SupportedEventMask = MTK_MUSB_PLATFORM_EVENTS;
  E.HostPortStatus = 0x0101U;
  E.HostPortChangeStatus = 0x0001U;
  E.HostPortSnapshotValid = 1U;
  return E;
}

static void
SetTypecOnlyGpioEvidence(MTK_MUSB_PLATFORM_EVIDENCE *Evidence)
{
  Evidence->HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY;
  Evidence->Gpio[0].PhysicalInput = 1U;
  Evidence->Gpio[1].PhysicalInput = 0U;
  Evidence->Gpio[2].PhysicalInput = 0U;
  Evidence->Gpio[3].PhysicalInput = 0U;
  Evidence->Gpio[4].PhysicalInput = 1U;
}

static void
ResetColdMock(void)
{
  memset(&Cold, 0, sizeof(Cold));
  Cold.OwnerToken = 0x11U;
  Cold.DataRoleToken = 0x22U;
  Cold.DataRole = MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
  Cold.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC;
  Cold.IdLowRoleToken = 0x33U;
  Cold.PhyMode = MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG;
  Cold.PhyState = MTK_MUSB_PLATFORM_PHY_POWERED;
}

static int
FreshColdGuard(
  void *Context,
  uint32_t Phase,
  MTK_MUSB_PLATFORM_COLD_EVIDENCE *Evidence
  )
{
  COLD_MOCK *Mock = (COLD_MOCK *)Context;
  uint32_t I;

  ++Mock->GuardCalls;
  if (Mock->ReplayMode == 4U && Mock->ReplayPowerWritten &&
      Phase == MtkMusbPlatformColdAfterPower) {
    Bus.Registers[REG_POWER] |= POWER_SUSPENDM;
  }
  if (Mock->DropSessionBeforeReset && Phase == MtkMusbPlatformColdBeforePower) {
    Mock->DropSessionBeforeReset = 0U;
    Bus.Registers[REG_DEVCTL] &= (uint8_t)~DEVCTL_SESSION;
    Bus.Registers[REG_POWER] |= 0x50U; /* supplied D=98/W=71, when initial POWER=21 */
    if (Mock->ResetSessionFault <= 2U) Mock->SessionSetFailure = Mock->ResetSessionFault;
    if (Mock->ResetSessionFault == 3U) Bus.Registers[REG_POWER] |= POWER_RESET;
    if (Mock->ResetSessionFault == 4U) SetRegister16(REG_HSDMA_CTRL0,1U);
  }
  if (Bus.Locked) Mock->CallbackWhileLocked = 1;
  if ((Mock->ChangeRoleTokenOnGuardCall != 0U) &&
      (Mock->GuardCalls == Mock->ChangeRoleTokenOnGuardCall)) {
    ++Mock->DataRoleToken;
  }
  if ((Mock->ChangePowerPathOnGuardCall != 0U) &&
      (Mock->GuardCalls == Mock->ChangePowerPathOnGuardCall)) {
    Mock->HostPowerPath = (Mock->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB)
                            ? MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE
                            : MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  }
  if ((Mock->ChangeIdLowTokenOnGuardCall != 0U) &&
      (Mock->GuardCalls == Mock->ChangeIdLowTokenOnGuardCall)) {
    ++Mock->IdLowRoleToken;
  }
  if ((Mock->FailPhase == Phase) && (Mock->FailGuardOnce != 0)) {
    Mock->FailGuardOnce = 0;
    Mock->FailPhase = 0U;
    return 0;
  }
  ++Mock->Sequence;
  Evidence->ExclusiveOwnerToken = Mock->OwnerToken;
  Evidence->DataRoleToken = Mock->DataRoleToken;
  Evidence->SampleSequence = Mock->Sequence;
  Evidence->UsbDataRole = Mock->DataRole;
  Evidence->PhyUsbMode = Mock->PhyMode;
  Evidence->Platform = MakeEvidence();
  if (Mock->PhyLength != 0U) Evidence->Platform.PhyLength = Mock->PhyLength;
  Evidence->Platform.AcpiColdFlags = Mock->AcpiColdFlags;
  Evidence->Platform.AcpiClockIdentity = Mock->AcpiClockIdentity;
  if (Mock->ChangeClockIdentityOnGuardCall == Mock->GuardCalls)
    ++Evidence->Platform.AcpiClockIdentity;
  Evidence->Platform.HostPowerPath = Mock->HostPowerPath;
  Evidence->Platform.IdLowRoleToken = Mock->IdLowRoleToken;
  Evidence->Platform.PhyState = Mock->PhyState;
  Evidence->Platform.PhyPowerCleanupPending = Mock->PhyPowerCleanupPending;
  Evidence->Platform.PhyDtm1 = (1U << 2) | (1U << 5);
  if (Mock->PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST) {
    Evidence->Platform.PhyDtm1 = 0x3f2dU;
  } else if (Mock->PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID) {
    Evidence->Platform.PhyDtm1 = 0x3f11U;
  }
  if (Mock->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB) {
    Evidence->UsbDataRole = Mock->DataRole;
    Evidence->Platform.Sc8989xBoostEnabled = 0U;
    Evidence->Platform.Husb311PowerStatusReadValid = 0U;
    Evidence->Platform.Husb311CcStatusReadValid = 0U;
    for (I = 1U; I <= 3U; ++I) {
      Evidence->Platform.Gpio[I].PhysicalInput = 1U;
    }
    Evidence->Platform.Gpio[4].PhysicalInput = 0U;
  } else if (Mock->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY) {
    SetTypecOnlyGpioEvidence(&Evidence->Platform);
    if (Mock->BadTypecOnlyGpio) Evidence->Platform.Gpio[1].PhysicalInput = 1U;
    if (Mock->BadTypecOnlySource) Evidence->Platform.Sc8989xBoostEnabled = 0U;
  }
  if (Mock->AcpiColdFlags & MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID) {
    Evidence->Platform.ClockReadMask = 0U;
    Evidence->Platform.ClockEnabledMask = 0U;
    memset(Evidence->Platform.ClockHz, 0, sizeof(Evidence->Platform.ClockHz));
    memset(Evidence->Platform.ExpectedClockHz, 0,
        sizeof(Evidence->Platform.ExpectedClockHz));
    Evidence->Platform.RefClockHz = 0U;
    Evidence->Platform.RefClockEnabled = 0U;
    Evidence->Platform.TopUsbParent = 0U;
    Evidence->Platform.ExpectedTopUsbParent = 0U;
    Evidence->Platform.TopUsbFrequencyHz = 0U;
    Evidence->Platform.ExpectedTopUsbFrequencyHz = 0U;
  }
  if (Mock->AcpiColdFlags & MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID)
    memset(Evidence->Platform.Gpio, 0, sizeof(Evidence->Platform.Gpio));
  if (Mock->AcpiColdFlags & MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE) {
    Evidence->Platform.Sc8989xBoostReadValid = 0U;
    Evidence->Platform.Sc8989xBoostEnabled = 0U;
  }
  if (Mock->AcpiColdFlags & MTK_MUSB_PLATFORM_ACPI_ID_LOW_ROUTE) {
    Evidence->Platform.Sc8989xBoostReadValid = 0U;
    Evidence->Platform.Sc8989xBoostEnabled = 0U;
    memset(Evidence->Platform.Gpio, 0, sizeof(Evidence->Platform.Gpio));
  }
  return 1;
}

static int
SetColdPhyMode(void *Context, uint32_t Mode)
{
  COLD_MOCK *Mock = (COLD_MOCK *)Context;

  ++Mock->PhyCalls;
  if (Mode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST)
    Mock->HostPhyAtRegisterWrite = Bus.ColdTraceCount;
  if (Bus.Locked) Mock->CallbackWhileLocked = 1;
  Mock->PhyMode = Mode;
  if (Mock->FailPhyFaultOnce == (Mode == MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID ? 2 : 1)) {
    Mock->FailPhyFaultOnce = 0;
    Mock->PhyMode = MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN;
    Mock->PhyState = MTK_MUSB_PLATFORM_PHY_STATE_FAULTED;
    Mock->PhyPowerCleanupPending = MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING;
    return 0;
  }
  if (Mock->FailPhyOnce == (Mode == MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID ? 2 : 1)) {
    Mock->FailPhyOnce = 0;
    return 0;
  }
  if (Mode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST) {
    if (Mock->PowerAfterPhyValid) Bus.Registers[REG_POWER] = Mock->PowerAfterPhy;
    if (Mock->DmaAfterPhy) Bus.Registers[REG_HSDMA_CTRL0] = 1U;
  }
  if (Mock->DeviceIdHigh) {
    if (Mode == MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG) {
      Bus.Registers[REG_DEVCTL] = (Bus.Registers[REG_DEVCTL] | 0x80U) & (uint8_t)~DEVCTL_HOSTMODE;
    } else if (Mode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST &&
               !Mock->HostModeStuck && !Mock->HostAfterPower && Mock->ReadyAfterPolls == 0U) {
      Bus.Registers[REG_DEVCTL] = (Bus.Registers[REG_DEVCTL] & (uint8_t)~0x80U) | DEVCTL_HOSTMODE;
    }
  }
  return 1;
}

static int
ColdDelayUs(void *Context, uint32_t Microseconds)
{
  COLD_MOCK *Mock = (COLD_MOCK *)Context;

  if (Bus.Locked) Mock->CallbackWhileLocked = 1;
  if (Microseconds == 5000U) {
    if (Mock->SettleCalls == 0U) {
      ++Mock->SettleCalls;
      return Mock->FailStartupDelay != 1U;
    }
    ++Mock->OtgHoldCalls;
    return Mock->FailStartupDelay != 2U &&
           Mock->PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG &&
           (Bus.Registers[REG_DEVCTL] & 1U) == 0U;
  }
  if (Microseconds != MTK_MUSB_PLATFORM_COLD_POLL_US) return 0;
  ++Mock->DelayCalls;
  if (Mock->ReplayMode == 5U && Mock->ReplayPowerWritten)
    Bus.Registers[REG_POWER] |= POWER_SUSPENDM;
  if ((Mock->ReadyAfterPolls != 0U) &&
      (Mock->DelayCalls >= Mock->ReadyAfterPolls)) {
    if (!Mock->DeviceIdHigh ||
        (Mock->PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST && !Mock->HostModeStuck)) {
      Bus.Registers[REG_DEVCTL] |= DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID;
      if (Mock->DeviceIdHigh) Bus.Registers[REG_DEVCTL] &= (uint8_t)~0x80U;
    }
  }
  return Mock->FailDelay ? 0 : 1;
}

static int
ReadColdTime(void *Context, uint64_t *Microseconds)
{
  COLD_MOCK *Mock = Context;
  if (Bus.Locked) Mock->CallbackWhileLocked = 1;
  *Microseconds = Mock->NowUs;
  return !Mock->ClockFailed;
}

static MTK_MUSB_PLATFORM_STATUS
DriveColdStart(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  COLD_MOCK *Mock = Callbacks->Context;
  uint32_t WaitUs;

  for (;;) {
    Status = MtkMusbPlatformAdvanceColdHost(Io, Callbacks, State);
    if (Status != MtkMusbPlatformPending) return Status;
    switch (State->StartStep) {
      case MtkMusbPlatformColdStartSettle: WaitUs = 5000U; break;
      case MtkMusbPlatformColdStartPhySettle: WaitUs = 5000U; break;
      default: WaitUs = MTK_MUSB_PLATFORM_COLD_POLL_US; break;
    }
    Mock->ClockFailed = !ColdDelayUs(Mock, WaitUs);
    Mock->NowUs += WaitUs;
  }
}

static MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS
MakeColdCallbacks(void)
{
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;

  memset(&Callbacks, 0, sizeof(Callbacks));
  Callbacks.Context = &Cold;
  Callbacks.FreshGuard = FreshColdGuard;
  Callbacks.SetPhyMode = SetColdPhyMode;
  Callbacks.GetTimeUs = ReadColdTime;
  return Callbacks;
}

static int
TestFifoProfile(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  uint32_t Cursor;
  uint32_t Endpoint;

  ResetBus();
  Io = MakeIo();
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io) == MtkMusbPlatformSuccess);
  CHECK(Bus.Index == 7U && !Bus.Locked);
  CHECK(Bus.FifoWriteCount == 32U);
  CHECK(Bus.Ep0FifoAccess == 0U);
  CHECK(Bus.FifoSize[0][0] == 0U && Bus.FifoSize[0][1] == 0U);
  CHECK(Bus.FifoAddress[0][0] == 0U && Bus.FifoAddress[0][1] == 0U);
  Cursor = 64U;
  for (Endpoint = 1U; Endpoint <= 8U; ++Endpoint) {
    uint32_t RxBytes;
    CHECK(Bus.FifoSize[Endpoint][0] == 6U);
    CHECK(Bus.FifoAddress[Endpoint][0] == (uint16_t)(Cursor / 8U));
    Cursor += 512U;
    RxBytes = (Endpoint == 8U) ? 64U : 512U;
    CHECK(Bus.FifoSize[Endpoint][1] == ((RxBytes == 64U) ? 3U : 6U));
    CHECK(Bus.FifoAddress[Endpoint][1] == (uint16_t)(Cursor / 8U));
    Cursor += RxBytes;
  }
  CHECK(Cursor == MTK_MUSB_PLATFORM_FIFO_TOTAL_BYTES);
  CHECK(MtkMusbPlatformVerifyHostFifo(&Io) == MtkMusbPlatformSuccess);
  CHECK(Bus.Index == 7U && !Bus.Locked);
  for (Endpoint = 1U; Endpoint <= 8U; ++Endpoint) {
    for (unsigned Direction = 0U; Direction < 2U; ++Direction) {
      Bus.FifoSize[Endpoint][Direction] ^= 1U;
      CHECK(MtkMusbPlatformVerifyHostFifo(&Io) == MtkMusbPlatformProfileMismatch);
      Bus.FifoSize[Endpoint][Direction] ^= 1U;
      Bus.FifoAddress[Endpoint][Direction]++;
      CHECK(MtkMusbPlatformVerifyHostFifo(&Io) == MtkMusbPlatformProfileMismatch);
      Bus.FifoAddress[Endpoint][Direction]--;
      CHECK(Bus.Index == 7U && !Bus.Locked && Bus.Ep0FifoAccess == 0U);
    }
  }
  return 1;
}

static int
TestQuiescenceGates(void)
{
  MTK_MUSB_PLATFORM_IO Io;

  ResetBus(); Io = MakeIo();
  SetRegister8(REG_DEVCTL, DEVCTL_SESSION);
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io) == MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.FifoWriteCount == 0U && Bus.Index == 7U && !Bus.Locked);

  ResetBus(); Io = MakeIo();
  SetRegister16(REG_INTRTXE, 1U);
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 1U) == MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.Index == 7U && !Bus.Locked);

  ResetBus(); Io = MakeIo();
  Bus.TxCsr[3] = 1U;
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 1U) == MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.Index == 7U && !Bus.Locked);

  ResetBus(); Io = MakeIo();
  Bus.TxCsr[0] = 0x0008U;
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 1U) == MtkMusbPlatformQuiescenceFailed);

  ResetBus(); Io = MakeIo();
  Bus.RxCsr[4] = 0x2000U;
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 1U) == MtkMusbPlatformQuiescenceFailed);

  ResetBus(); Io = MakeIo();
  SetRegister16(REG_HSDMA_CTRL0 + 0x20U, 1U);
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 1U) == MtkMusbPlatformQuiescenceFailed);

  ResetBus(); Io = MakeIo();
  Bus.Registers[REG_QMU_GCSR] = 1U;
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 1U) == MtkMusbPlatformQuiescenceFailed);
  return 1;
}

static int
TestFailureRestoresIndex(void)
{
  MTK_MUSB_PLATFORM_IO Io;

  ResetBus(); Io = MakeIo();
  Bus.FailReadOffset = REG_INTRUSBE;
  Bus.FailReadOnce = 1;
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 1U) == MtkMusbPlatformIoError);
  CHECK(Bus.Index == 7U && !Bus.Locked);

  ResetBus(); Io = MakeIo();
  Bus.FailWriteOffset = REG_TXFIFOSZ;
  Bus.FailWriteOnce = 1;
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io) == MtkMusbPlatformIoError);
  CHECK(Bus.Index == 7U && !Bus.Locked);
  CHECK(Bus.FifoWriteCount == 0U);
  return 1;
}

static int
TestReadyEvidence(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_EVIDENCE Evidence;
  MTK_MUSB_PLATFORM_READY_RESULT Result;

  ResetBus(); Io = MakeIo();
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io) == MtkMusbPlatformSuccess);
  SetRegister8(REG_DEVCTL, DEVCTL_SESSION | DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID);
  SetRegister8(REG_INTRUSB, 0x10U);
  Evidence = MakeEvidence();
  Evidence.HostPortSnapshotValid = 0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformSuccess);
  CHECK(Result.Flags == MTK_MUSB_PLATFORM_READY_REQUIRED);
  CHECK(Result.HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  CHECK(Result.MissingFlags == 0U);
  CHECK(Result.DeviceControl == (DEVCTL_SESSION | DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID));
  CHECK(Result.UsbInterruptStatus == 0x10U);
  CHECK(Result.HostPortSnapshotValid == 0U);
  CHECK(Result.HostPortStatus == 0U && Result.HostPortChangeStatus == 0U);
  CHECK(!Bus.Locked && Bus.Index == 7U);

  Evidence.HostPortSnapshotValid = 1U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformSuccess);
  CHECK(Result.HostPortSnapshotValid == 1U);
  CHECK(Result.HostPortStatus == Evidence.HostPortStatus);
  CHECK(Result.HostPortChangeStatus == Evidence.HostPortChangeStatus);

  Evidence.Husb311PowerStatus &= ~MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_VBUS) != 0U);
  CHECK((Result.Flags & MTK_MUSB_PLATFORM_READY_VBUS) == 0U);

  Evidence.Husb311PowerStatus |= MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
                                 MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_VBUS) != 0U);
  CHECK((Result.Flags & MTK_MUSB_PLATFORM_READY_VBUS) == 0U);

  Evidence.Husb311PowerStatus &= ~MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformSuccess);
  CHECK((Result.Flags & MTK_MUSB_PLATFORM_READY_VBUS) != 0U);

  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID|0x80U);
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&Evidence,&Result)==MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags&MTK_MUSB_PLATFORM_READY_HOST)!=0U);
  CHECK((Result.Flags&MTK_MUSB_PLATFORM_READY_VBUS)==0U);
  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID);

  Evidence.Sc8989xBoostReadValid = 0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_VBUS) != 0U);

  Evidence = MakeEvidence();
  Evidence.Husb311CcStatusReadValid = 0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_VBUS) != 0U);
  Evidence = MakeEvidence();
  Evidence.Husb311CcStatus = MTK_MUSB_PLATFORM_HUSB311_CC_LOOKING_FOR_CONNECTION |
                             MTK_MUSB_PLATFORM_HUSB311_SRC_RD;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  Evidence = MakeEvidence();
  Evidence.Husb311CcStatus = MTK_MUSB_PLATFORM_HUSB311_CC_CONNECT_RESULT |
                             MTK_MUSB_PLATFORM_HUSB311_SRC_RD;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);

  Evidence = MakeEvidence();
  SetRegister8(REG_INTRUSBE, MTK_MUSB_PLATFORM_EVENTS);
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_EVENTS) != 0U);
  SetRegister8(REG_INTRUSBE, 0U);

  SetRegister8(REG_DEVCTL, DEVCTL_SESSION | DEVCTL_HOSTMODE);
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_VBUS) != 0U);

  SetRegister8(REG_DEVCTL, DEVCTL_SESSION | DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID);
  Evidence.PhyState = 3U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_PHY) != 0U);

  ResetBus(); Io = MakeIo(); Evidence = MakeEvidence();
  Evidence.ControllerMapped = 0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK(Bus.ReadCount == 0U && Result.Flags == 0U);
  return 1;
}

static int
TestIdLowReadyEvidence(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_EVIDENCE Evidence;
  MTK_MUSB_PLATFORM_READY_RESULT Result;

  ResetBus(); Io = MakeIo(); Evidence = MakeEvidence();
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io) == MtkMusbPlatformSuccess);
  Evidence.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Evidence.IdLowRoleToken = 0x33U;
  Evidence.Sc8989xBoostEnabled = 0U;
  Evidence.Husb311PowerStatusReadValid = 0U;
  Evidence.Husb311CcStatusReadValid = 0U;
  Evidence.Gpio[MTK_MUSB_PLATFORM_GPIO_COUNT - 1U].PhysicalInput = 0U;
  for (uint32_t I = 1U; I <= 3U; ++I) Evidence.Gpio[I].PhysicalInput = 1U;
  SetRegister8(REG_DEVCTL, DEVCTL_SESSION | DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID);
  SetRegister8(REG_INTRUSB, 0x10U);
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformSuccess);
  CHECK(Result.HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);
  CHECK(Result.Flags == MTK_MUSB_PLATFORM_READY_ID_LOW_REQUIRED);
  CHECK(Result.MissingFlags == 0U);
  CHECK((Result.Flags & MTK_MUSB_PLATFORM_READY_VBUS) == 0U);
  CHECK((Result.Flags & MTK_MUSB_PLATFORM_READY_ID_LOW_HUB) != 0U);

  ResetBus(); Io=MakeIo(); Evidence=MakeEvidence();
  SetTypecOnlyGpioEvidence(&Evidence);
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io)==MtkMusbPlatformSuccess);
  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID);
  SetRegister8(REG_INTRUSB,0x10U);
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&Evidence,&Result)==MtkMusbPlatformSuccess);
  CHECK(Result.HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY &&
        Result.Flags==MTK_MUSB_PLATFORM_READY_REQUIRED && Result.MissingFlags==0U);
  Evidence.Gpio[3].PhysicalInput=1U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&Evidence,&Result)==MtkMusbPlatformSuccess);
  Evidence.Gpio[0].PhysicalInput=0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&Evidence,&Result)==MtkMusbPlatformSuccess);
  for (uint32_t Pin=0U;Pin<MTK_MUSB_PLATFORM_GPIO_COUNT;++Pin) {
    MTK_MUSB_PLATFORM_EVIDENCE BadEvidence=Evidence;
    if (Pin==3U) continue;
    BadEvidence.Gpio[Pin].PhysicalInput=Pin==0U?2U:Pin==4U?0U:1U;
    CHECK(MtkMusbPlatformEvaluateReady(&Io,&BadEvidence,&Result)==MtkMusbPlatformNotReady);
    CHECK((Result.MissingFlags&MTK_MUSB_PLATFORM_READY_HOST)!=0U);
  }
  MTK_MUSB_PLATFORM_EVIDENCE BadEvidence=Evidence;
  BadEvidence.Gpio[3].PhysicalInput=2U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&BadEvidence,&Result)==MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags&MTK_MUSB_PLATFORM_READY_HOST)!=0U);
  BadEvidence=Evidence; BadEvidence.Sc8989xBoostEnabled=0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&BadEvidence,&Result)==MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags&MTK_MUSB_PLATFORM_READY_VBUS)!=0U);
  BadEvidence=Evidence; BadEvidence.Husb311CcStatusReadValid=0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&BadEvidence,&Result)==MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags&MTK_MUSB_PLATFORM_READY_VBUS)!=0U);

  Evidence.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Evidence.IdLowRoleToken = 0x33U;
  Evidence.Sc8989xBoostEnabled = 0U;
  Evidence.Husb311PowerStatusReadValid = 0U;
  Evidence.Husb311CcStatusReadValid = 0U;
  Evidence.Gpio[0].PhysicalInput = 0U;
  Evidence.Gpio[4].PhysicalInput = 0U;
  for (uint32_t I=1U; I<=3U; ++I) Evidence.Gpio[I].PhysicalInput=1U;
  Evidence.IdLowRoleToken = 0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_ID_LOW_HUB) != 0U);

  Evidence.IdLowRoleToken = 0x33U;
  Evidence.Sc8989xBoostReadValid = 0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_ID_LOW_HUB) != 0U);

  Evidence.Sc8989xBoostReadValid = 1U;
  Evidence.Sc8989xBoostEnabled = MTK_MUSB_PLATFORM_SC8989X_BOOST_ENABLED;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_ID_LOW_HUB) != 0U);

  Evidence.Sc8989xBoostEnabled = 0U;
  SetRegister8(REG_DEVCTL, DEVCTL_SESSION | DEVCTL_HOSTMODE);
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_ID_LOW_HUB) != 0U);
  CHECK((Result.Flags & MTK_MUSB_PLATFORM_READY_HOST) != 0U);

  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID|0x80U);
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&Evidence,&Result)==MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags&MTK_MUSB_PLATFORM_READY_HOST)!=0U);
  CHECK((Result.MissingFlags&MTK_MUSB_PLATFORM_READY_ID_LOW_HUB)!=0U);

  SetRegister8(REG_DEVCTL, DEVCTL_SESSION | DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID);
  Evidence.Gpio[4].PhysicalInput = 1U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_HOST) != 0U);

  Evidence.Gpio[4].PhysicalInput = 0U;
  Evidence.Gpio[2].PhysicalInput = 0U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_HOST) != 0U);

  Evidence.Gpio[2].PhysicalInput = 1U;
  Evidence.Gpio[2].Direction = MTK_MUSB_PLATFORM_GPIO_INPUT;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_HOST) != 0U);

  Evidence.Gpio[2].Direction = MTK_MUSB_PLATFORM_GPIO_OUTPUT;
  Evidence.Gpio[2].Mode = 1U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_HOST) != 0U);

  Evidence.Gpio[2].Mode = 0U;
  Evidence.Gpio[0].PhysicalInput = 1U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK((Result.MissingFlags & MTK_MUSB_PLATFORM_READY_HOST) != 0U);

  Evidence.Gpio[2].PhysicalInput = 1U;
  Evidence.Gpio[0].PhysicalInput = 0U;
  Evidence.HostPowerPath = 4U;
  CHECK(MtkMusbPlatformEvaluateReady(&Io, &Evidence, &Result) == MtkMusbPlatformNotReady);
  CHECK(Result.Flags == 0U);
  return 1;
}

static int
TestHusb311SourceAttach(void)
{
  /* DRP currently presenting Rp, with the sink Rd on CC1. */
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 1);
  /* Source attachment with cable Ra on the inactive CC2 line is valid. */
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD |
            (MTK_MUSB_PLATFORM_HUSB311_SRC_RA << MTK_MUSB_PLATFORM_HUSB311_CC2_SHIFT),
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 1);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_OPEN |
            (MTK_MUSB_PLATFORM_HUSB311_SRC_RD << MTK_MUSB_PLATFORM_HUSB311_CC2_SHIFT),
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RD <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC1_SHIFT) |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RD <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 1);
  /* Fixed source mode must set Rp on both pins. */
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 1);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_OPEN |
            (MTK_MUSB_PLATFORM_HUSB311_SRC_RD << MTK_MUSB_PLATFORM_HUSB311_CC2_SHIFT),
          MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 1);
  /* Fixed source mode with CC2 Rd or Open is invalid even if CC1 says Rp. */
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RD <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_OPEN <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  /* Current source role must agree with ConnectResult in fixed mode. */
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_CC_CONNECT_RESULT |
            MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  /* Reject looking/toggling, disconnected, sink, reserved, and dual-Rd data. */
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_CC_LOOKING_FOR_CONNECTION |
            MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_CC_CONNECT_RESULT |
            MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          0U,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          0x03U,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD |
            (MTK_MUSB_PLATFORM_HUSB311_SRC_RD << MTK_MUSB_PLATFORM_HUSB311_CC2_SHIFT),
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  /* DRP configuration must be symmetric; RpValue 11 is reserved. */
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RD <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_RESERVED <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_SHIFT) |
            MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT)) == 0);
  CHECK(MtkMusbPlatformHusb311SourceAttached(
          MTK_MUSB_PLATFORM_HUSB311_SRC_RD,
          MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT) |
            (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_RESERVED <<
             MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_SHIFT)) == 0);
  return 1;
}

static int
TestInvalidAndBusyInputs(void)
{
  MTK_MUSB_PLATFORM_IO Io;

  ResetBus(); Io = MakeIo();
  CHECK(MtkMusbPlatformCheckQuiescent(NULL, 0U) == MtkMusbPlatformInvalid);
  Io.TryLock = NULL;
  CHECK(MtkMusbPlatformCheckQuiescent(&Io, 0U) == MtkMusbPlatformInvalid);

  Io = MakeIo();
  Bus.Locked = 1;
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io) == MtkMusbPlatformBusy);
  CHECK(Bus.FifoWriteCount == 0U && Bus.Locked);
  return 1;
}

static int
TestIdleDiagnostic(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC D;
  const uint32_t Offset[] = {REG_INTRUSBE, REG_INTRTXE, REG_INTRRXE,
    REG_TXCSR, REG_TXCSR, REG_RXCSR, REG_HSDMA_CTRL0 + 0x70U, REG_QMU_GCSR};
  const uint32_t Value[] = {0xf7U, 1U, 0x8000U, 2U, 1U, 0x20U, 1U, 0x10000U};
  const uint32_t Mask[] = {0xffU, 0xffffU, 0xffffU, 0x6bU, 0x9403U, 0xe823U, 1U, 0xff00ffU};
  const uint32_t Endpoint[] = {0xffU, 0xffU, 0xffU, 0U, 5U, 8U, 7U, 0xffU};
  unsigned i;
  for (i = 0U; i < sizeof(Offset)/sizeof(Offset[0]); ++i) {
    ResetBus(); Io = MakeIo(); Bus.Index = 7U;
    WriteRegister16(REG_INTRTX, 0x1234U);
    WriteRegister16(REG_INTRRX, 0x0042U);
    Bus.Registers[REG_HSDMA_INTR] = 0xa5U;
    if (i == 3U || i == 4U) Bus.TxCsr[Endpoint[i]] = (uint16_t)Value[i];
    else if (i == 5U) Bus.RxCsr[Endpoint[i]] = (uint16_t)Value[i];
    else if (i == 0U) Bus.Registers[Offset[i]] = (uint8_t)Value[i];
    else if (i == 7U) { WriteRegister16(Offset[i], (uint16_t)Value[i]); WriteRegister16(Offset[i]+2U, (uint16_t)(Value[i]>>16)); }
    else WriteRegister16(Offset[i], (uint16_t)Value[i]);
    CHECK(MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(&Io, &D) ==
          MtkMusbPlatformQuiescenceFailed);
    CHECK(D.Offset == Offset[i] && D.Value == Value[i] &&
          D.Mask == Mask[i] && D.Endpoint == Endpoint[i]);
    CHECK(Bus.AckWrites == 0U && Bus.FifoWriteCount == 0U &&
          Bus.Index == 7U && !Bus.Locked);
    CHECK(ReadRegister16(REG_INTRTX) == 0x1234U &&
          ReadRegister16(REG_INTRRX) == 0x0042U && Bus.Registers[REG_HSDMA_INTR] == 0xa5U);
  }
  ResetBus(); Io = MakeIo(); Bus.Index = 7U;
  WriteRegister16(REG_INTRTX, 0x1234U); Bus.IgnoreAck = 1;
  CHECK(MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(&Io, &D) ==
        MtkMusbPlatformQuiescenceFailed);
  CHECK(D.Offset == REG_INTRTX && D.Value == 0x1234U && D.Mask == 0xffffU);
  CHECK(Bus.AckWrites == 1U && Bus.Index == 7U && !Bus.Locked);
  Bus.IgnoreAck = 0;
  CHECK(MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(&Io, &D) ==
        MtkMusbPlatformSuccess);
  CHECK(D.Offset == 0xffffffffU && Bus.Index == 7U && !Bus.Locked);
  Bus.FailReadOffset = REG_INTRUSBE; Bus.FailReadOnce = 1;
  CHECK(MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(&Io, &D) ==
        MtkMusbPlatformIoError);
  CHECK(D.Offset == 0xffffffffU && Bus.Index == 7U && !Bus.Locked);
  D.Offset = 0U;
  CHECK(MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(NULL, &D) ==
        MtkMusbPlatformInvalid);
  CHECK(D.Offset == 0xffffffffU);
  return 1;
}

static void
IdleDeviceFixture(void)
{
  ResetBus();
  Bus.Registers[REG_DEVCTL] = 0x99U; Bus.Registers[REG_POWER] = 0x40U;
  Bus.Registers[REG_INTRUSBE] = 0x27U;
  WriteRegister16(REG_INTRTXE,0x1ffU); WriteRegister16(REG_INTRRXE,0x1feU);
  WriteRegister16(REG_INTRTX,0x1234U); WriteRegister16(REG_INTRRX,0x42U);
  Bus.Registers[REG_HSDMA_INTR] = 0xa5U; Bus.Registers[REG_HSDMA_INTR+1U] = 0x5aU;
  Bus.Registers[REG_INTRUSB] = 0x30U;
}

static MTK_MUSB_PLATFORM_IO
DisconnectedStatusFixture(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC D;
  IdleDeviceFixture(); Io=MakeIo(); Bus.TxCsr[0]=8U;
  if (MtkMusbPlatformRetireDeviceForDisconnect(&Io,&D)!=MtkMusbPlatformSuccess)
    return (MTK_MUSB_PLATFORM_IO){0};
  Bus.Registers[REG_POWER]=0U; Bus.Registers[REG_DEVCTL]=0x98U;
  return Io;
}

static int
TestPendingDeviceStatusDisconnect(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC D;
  const uint32_t ResetValues[]={0U,2U,0x4000U,0x8000U,0xfffcU};
  const uint32_t Offsets[]={REG_DEVCTL,REG_DEVCTL,REG_DEVCTL,REG_POWER,REG_POWER,
    REG_POWER,REG_POWER,REG_INTRUSBE,REG_INTRTXE,REG_INTRRXE,REG_TXCSR,
    REG_TXCSR,REG_TXCSR,REG_TXCSR,REG_RXCSR,REG_HSDMA_CTRL0,
    REG_HSDMA_CTRL0+0x70U,REG_QMU_GCSR,REG_SWRST};
  const uint32_t Values[]={0x99U,0x9cU,0x18U,0x40U,2U,4U,8U,1U,1U,2U,
    9U,10U,0x18U,1U,1U,1U,1U,0x10000U,1U};
  const uint32_t Endpoint[]={0xffU,0xffU,0xffU,0xffU,0xffU,0xffU,0xffU,
    0xffU,0xffU,0xffU,0U,0U,0U,5U,8U,0xffU,0xffU,0xffU,0xffU};
  const uint32_t ReadFaults[]={REG_INDEX,REG_DEVCTL,REG_POWER,REG_INTRUSBE,
    REG_INTRTXE,REG_INTRRXE,REG_INTRTX,REG_INTRRX,REG_TXCSR,REG_RXCSR,
    REG_HSDMA_INTR,REG_HSDMA_CTRL0,REG_HSDMA_CTRL0+0x70U,REG_QMU_GCSR,REG_SWRST};
  unsigned I,Fail;
  IdleDeviceFixture();Io=MakeIo();Bus.TxCsr[0]=8U;
  CHECK(MtkMusbPlatformCheckIdleDevice(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
  CHECK(D.Offset==REG_TXCSR&&D.Value==8U&&D.Mask==0x6bU&&D.Endpoint==0U);
  CHECK(MtkMusbPlatformCheckDeviceForDisconnect(&Io,&D)==MtkMusbPlatformSuccess);
  CHECK(Bus.TxCsr[0]==8U&&Bus.IrqWrites==0U&&Bus.CsrWrites==0U&&Bus.ResetWrites==0U);
  CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
  CHECK(MtkMusbPlatformRetireDeviceForDisconnect(&Io,&D)==MtkMusbPlatformSuccess);
  CHECK(Bus.TxCsr[0]==8U&&Bus.CsrWrites==0U&&Bus.ResetWrites==0U&&Bus.IrqWrites==3U);
  CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io)==MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.AckWrites==0U&&Bus.Index==7U&&!Bus.Locked);
  for(I=0U;I<sizeof(ResetValues)/sizeof(ResetValues[0]);++I) {
    Io=DisconnectedStatusFixture();WriteRegister16(REG_SWRST,(uint16_t)ResetValues[I]);
    CHECK(MtkMusbPlatformRequestDisconnectedDeviceReset(&Io,&D)==MtkMusbPlatformSuccess);
    CHECK(Bus.ResetWrites==1U&&Bus.ResetTrace.Offset==REG_SWRST&&
      Bus.ResetTrace.Width==16U&&Bus.ResetTrace.Value==(ResetValues[I]|3U));
    // Request success does not establish hardware completion or host readiness.
    CHECK(Bus.TxCsr[0]==8U&&Bus.AckWrites==0U&&Bus.FifoWriteCount==0U&&
      Bus.CsrWrites==0U&&Bus.Index==7U&&!Bus.Locked);
    CHECK(MtkMusbPlatformCheckIdleDevice(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
    Bus.TxCsr[0]=0U;
    CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io)==MtkMusbPlatformSuccess);
    CHECK(Bus.AckWrites==3U&&Bus.Registers[REG_HSDMA_INTR+1U]==0x5aU);
  }
  for(I=0U;I<sizeof(Offsets)/sizeof(Offsets[0]);++I) {
    Io=DisconnectedStatusFixture();
    if(Endpoint[I]!=0xffU) {
      if(Offsets[I]==REG_TXCSR)Bus.TxCsr[Endpoint[I]]=(uint16_t)Values[I];
      else Bus.RxCsr[Endpoint[I]]=(uint16_t)Values[I];
    } else if(Offsets[I]==REG_QMU_GCSR) Write32(NULL,Offsets[I],Values[I]);
    else if(Offsets[I]==REG_POWER||Offsets[I]==REG_DEVCTL||Offsets[I]==REG_INTRUSBE)
      Bus.Registers[Offsets[I]]=(uint8_t)Values[I];
    else WriteRegister16(Offsets[I],(uint16_t)Values[I]);
    CHECK(MtkMusbPlatformRequestDisconnectedDeviceReset(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
    CHECK(Bus.ResetWrites==0U&&Bus.AckWrites==0U&&Bus.CsrWrites==0U&&
      Bus.FifoWriteCount==0U&&Bus.Index==7U&&!Bus.Locked);
    CHECK(D.Offset==Offsets[I]);
  }
  // Pending DATAEND cannot hide a real packet, SETUPEND or DMA/QMU operation
  // at either the initial eligibility check or setup retirement.
  for(I=10U;I<=17U;++I) {
    Io=DisconnectedStatusFixture();
    if(Endpoint[I]!=0xffU) {
      if(Offsets[I]==REG_TXCSR)Bus.TxCsr[Endpoint[I]]=(uint16_t)Values[I];
      else Bus.RxCsr[Endpoint[I]]=(uint16_t)Values[I];
    } else if(Offsets[I]==REG_QMU_GCSR)Write32(NULL,Offsets[I],Values[I]);
    else WriteRegister16(Offsets[I],(uint16_t)Values[I]);
    CHECK(MtkMusbPlatformCheckDeviceForDisconnect(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
    CHECK(MtkMusbPlatformRetireDeviceForDisconnect(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
    CHECK(Bus.ResetWrites==0U&&Bus.CsrWrites==0U&&Bus.IrqWrites==3U&&!Bus.Locked);
  }
  for(I=0U;I<sizeof(ReadFaults)/sizeof(ReadFaults[0]);++I) {
    Io=DisconnectedStatusFixture();Bus.FailReadOffset=(int)ReadFaults[I];Bus.FailReadOnce=1;
    CHECK(MtkMusbPlatformRequestDisconnectedDeviceReset(&Io,&D)==MtkMusbPlatformIoError);
    CHECK(Bus.ResetWrites==0U&&Bus.Index==7U&&!Bus.Locked);
  }
  for(Fail=0U;Fail<2U;++Fail) {
    Io=DisconnectedStatusFixture();
    if(!Fail)Bus.FailWriteOffset=REG_SWRST;
    else Bus.FailWriteAfterOffset=REG_SWRST;
    CHECK(MtkMusbPlatformRequestDisconnectedDeviceReset(&Io,&D)==MtkMusbPlatformIoError);
    CHECK(Bus.ResetWrites==Fail&&Bus.TxCsr[0]==8U&&Bus.Index==7U&&!Bus.Locked);
  }
  CHECK(MtkMusbPlatformCheckDeviceForDisconnect(NULL,&D)==MtkMusbPlatformInvalid);
  CHECK(MtkMusbPlatformRetireDeviceForDisconnect(NULL,&D)==MtkMusbPlatformInvalid);
  CHECK(MtkMusbPlatformRequestDisconnectedDeviceReset(NULL,&D)==MtkMusbPlatformInvalid);
  Io=DisconnectedStatusFixture();Bus.Locked=1;
  CHECK(MtkMusbPlatformRequestDisconnectedDeviceReset(&Io,&D)==MtkMusbPlatformBusy);
  CHECK(Bus.ResetWrites==0U&&Bus.Locked);
  Bus.Locked=0;Io.Write16=NULL;
  CHECK(MtkMusbPlatformRequestDisconnectedDeviceReset(&Io,&D)==MtkMusbPlatformUnsupported);
  return 1;
}

static int
TestIdleDeviceInterruptRetirement(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC D;
  const uint32_t Offsets[] = {REG_INTRUSBE,REG_INTRTXE,REG_INTRRXE};
  const uint32_t Hazards[] = {REG_INTRTXE,REG_INTRRXE,REG_TXCSR,REG_TXCSR,
    REG_RXCSR,REG_HSDMA_CTRL0+0x70U,REG_QMU_GCSR,REG_POWER,REG_TXCSR,REG_DEVCTL};
  const uint32_t Values[] = {0x8000U,0x8000U,2U,1U,0x20U,1U,0x10000U,0x42U,0x10U,0x1dU};
  const uint32_t Endpoints[] = {0xffU,0xffU,0U,5U,8U,7U,0xffU,0xffU,0U,0xffU};
  unsigned I,J,Failure;
  IdleDeviceFixture(); Io=MakeIo();
  CHECK(MtkMusbPlatformCheckIdleDevice(&Io,&D)==MtkMusbPlatformSuccess);
  CHECK(D.Offset==0xffffffffU && Bus.IrqWrites==0U && Bus.AckWrites==0U &&
        Bus.FifoWriteCount==0U && Bus.Index==7U && !Bus.Locked);
  CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformSuccess);
  CHECK(Bus.IrqWrites==3U && D.Offset==0xffffffffU && Bus.AckWrites==0U);
  for(I=0U;I<3U;++I)
    CHECK(Bus.IrqTrace[I].Offset==Offsets[I] && Bus.IrqTrace[I].Width==(I==0U?8U:16U) &&
          Bus.IrqTrace[I].Value==0U);
  CHECK(Bus.Registers[REG_INTRUSBE]==0U && ReadRegister16(REG_INTRTXE)==0U &&
        ReadRegister16(REG_INTRRXE)==0U && ReadRegister16(REG_INTRTX)==0x1234U &&
        ReadRegister16(REG_INTRRX)==0x42U && Bus.Registers[REG_HSDMA_INTR]==0xa5U);
  CHECK(Bus.Registers[REG_POWER]==0x40U && Bus.Registers[REG_DEVCTL]==0x99U &&
        Bus.Registers[REG_INTRUSB]==0x30U && Bus.Registers[REG_HSDMA_INTR+1U]==0x5aU &&
        Bus.Index==7U && !Bus.Locked && Bus.FifoWriteCount==0U);
  CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io)==MtkMusbPlatformSuccess);
  CHECK(Bus.AckWrites==3U && Bus.Registers[REG_INTRUSB]==0x30U);
  for(I=0U;I<sizeof(Hazards)/sizeof(Hazards[0]);++I) {
    IdleDeviceFixture(); Io=MakeIo();
    if(I==2U||I==3U||I==8U)Bus.TxCsr[Endpoints[I]]=(uint16_t)Values[I];
    else if(I==4U)Bus.RxCsr[Endpoints[I]]=(uint16_t)Values[I];
    else if(I==7U||I==9U)Bus.Registers[Hazards[I]]=(uint8_t)Values[I];
    else {
      WriteRegister16(Hazards[I],(uint16_t)Values[I]);
      if(I==6U)WriteRegister16(Hazards[I]+2U,(uint16_t)(Values[I]>>16));
    }
    CHECK(MtkMusbPlatformCheckIdleDevice(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
    CHECK(D.Offset==Hazards[I] && D.Value==Values[I] && D.Endpoint==Endpoints[I]);
    CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
    CHECK(Bus.IrqWrites==0U && Bus.AckWrites==0U && Bus.Index==7U && !Bus.Locked);
  }
  // Failures before/after a write, ignored writes and failed confirmation reads
  // retain already-cleared masks and leave all later masks untouched.
  for(Failure=0U;Failure<4U;++Failure)for(I=0U;I<3U;++I) {
    IdleDeviceFixture(); Io=MakeIo();
    if(Failure==0U)Bus.FailWriteOffset=(int)Offsets[I];
    if(Failure==1U)Bus.FailWriteAfterOffset=(int)Offsets[I];
    if(Failure==2U)Bus.IgnoreIrqWrite=I+1U;
    if(Failure==3U)Bus.ArmFailReadAfterWriteOffset=(int)Offsets[I];
    CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==
          (Failure==2U?MtkMusbPlatformQuiescenceFailed:MtkMusbPlatformIoError));
    CHECK(Bus.IrqWrites==I+(Failure==0U?0U:1U) && Bus.AckWrites==0U &&
          Bus.Index==7U && !Bus.Locked && Bus.FifoWriteCount==0U);
    for(J=0U;J<3U;++J) {
      uint32_t Value=J==0U?Bus.Registers[REG_INTRUSBE]:ReadRegister16(Offsets[J]);
      CHECK(Value==((J<I||(J==I&&(Failure==1U||Failure==3U)))?0U:
                    (J==0U?0x27U:J==1U?0x1ffU:0x1feU)));
    }
  }
  IdleDeviceFixture();Io=MakeIo();Bus.ActivateDmaAfterIrqWrite=1;
  CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.IrqWrites==1U && D.Offset==REG_HSDMA_CTRL0 &&
        ReadRegister16(REG_INTRTXE)==0x1ffU && ReadRegister16(REG_INTRRXE)==0x1feU &&
        Bus.Index==7U && !Bus.Locked && Bus.AckWrites==0U);
  for(I=0U;I<3U;++I) {
    IdleDeviceFixture();Io=MakeIo();Bus.FailReadOffset=(int)Offsets[I];
    CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformIoError);
    CHECK(Bus.IrqWrites==0U && Bus.AckWrites==0U && Bus.Index==7U && !Bus.Locked);
  }
  CHECK(MtkMusbPlatformCheckIdleDevice(NULL,&D)==MtkMusbPlatformInvalid);
  CHECK(MtkMusbPlatformRetireIdleDevice(NULL,&D)==MtkMusbPlatformInvalid);
  IdleDeviceFixture();Io=MakeIo();Bus.Locked=1;
  CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformBusy);
  CHECK(Bus.IrqWrites==0U && Bus.Locked);
  return 1;
}

static void
IdleDeviceDmaFixture(void)
{
  IdleDeviceFixture();
  Bus.TxCsr[1]=0x1004U; // physical trial74: DMA request enable + UNDERRUN
  Bus.RxCsr[2]=0xa804U;
  Bus.TxCsr[3]=0x9404U;
}

static int
TestIdleDeviceDmaRequestRetirement(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC D;
  const uint32_t Offset[]={REG_TXCSR,REG_RXCSR,REG_TXCSR,REG_TXCSR};
  const uint32_t Endpoint[]={1U,2U,3U,3U};
  const uint32_t Values[]={0x00a6U,0x0045U,0x04a6U,0x00a6U};
  const uint32_t Hazards[]={REG_TXCSR,REG_RXCSR,REG_TXCSR,REG_TXCSR,
                           REG_RXCSR,REG_RXCSR,REG_HSDMA_CTRL0,REG_QMU_GCSR};
  const uint32_t Unsafe[]={1U,2U,8U,0x40U,0x10U,0x80U,1U,1U};
  unsigned I,J,Failure;
  IdleDeviceDmaFixture();Io=MakeIo();
  CHECK(MtkMusbPlatformCheckIdleDevice(&Io,&D)==MtkMusbPlatformSuccess);
  CHECK(Bus.CsrWrites==0U&&Bus.IrqWrites==0U&&Bus.Index==7U&&!Bus.Locked);
  CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformSuccess);
  CHECK(Bus.CsrWrites==4U&&Bus.IrqWrites==3U&&Bus.AckWrites==0U);
  for(I=0U;I<4U;++I)
    CHECK(Bus.CsrTrace[I].Offset==Offset[I]&&Bus.CsrTrace[I].Width==16U&&
          Bus.CsrTrace[I].Value==Values[I]&&Bus.CsrEndpoint[I]==Endpoint[I]);
  CHECK(Bus.TxCsr[1]==4U&&Bus.RxCsr[2]==4U&&Bus.TxCsr[3]==4U);
  CHECK(Bus.Registers[REG_POWER]==0x40U&&Bus.Registers[REG_DEVCTL]==0x99U&&
        Bus.Registers[REG_INTRUSB]==0x30U&&Bus.Registers[REG_HSDMA_INTR]==0xa5U&&
        ReadRegister16(REG_INTRTX)==0x1234U&&ReadRegister16(REG_INTRRX)==0x42U&&
        Bus.FifoWriteCount==0U&&Bus.ColdTraceCount==0U&&Bus.Index==7U&&!Bus.Locked);
  CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io)==MtkMusbPlatformSuccess);
  CHECK(Bus.AckWrites==3U);
  for(I=0U;I<sizeof(Hazards)/sizeof(Hazards[0]);++I) {
    IdleDeviceDmaFixture();Io=MakeIo();
    if(I==0U||I==2U||I==3U)Bus.TxCsr[5]|=(uint16_t)Unsafe[I];
    else if(I==1U||I==4U||I==5U)Bus.RxCsr[5]|=(uint16_t)Unsafe[I];
    else WriteRegister16(Hazards[I],(uint16_t)Unsafe[I]);
    CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==MtkMusbPlatformQuiescenceFailed);
    CHECK(D.Offset==Hazards[I]&&Bus.CsrWrites==0U&&Bus.IrqWrites==0U&&
          Bus.AckWrites==0U&&Bus.FifoWriteCount==0U&&Bus.Index==7U&&!Bus.Locked);
  }
  for(Failure=0U;Failure<4U;++Failure)for(I=1U;I<=4U;++I) {
    IdleDeviceDmaFixture();Io=MakeIo();
    if(Failure==0U)Bus.FailCsrBefore=I;
    if(Failure==1U)Bus.FailCsrAfter=I;
    if(Failure==2U)Bus.IgnoreCsrWrite=I;
    if(Failure==3U)Bus.FailCsrReadback=I;
    CHECK(MtkMusbPlatformRetireIdleDevice(&Io,&D)==
          (Failure==2U?MtkMusbPlatformQuiescenceFailed:MtkMusbPlatformIoError));
    CHECK(Bus.CsrWrites==I&&Bus.IrqWrites==3U&&Bus.AckWrites==0U&&
          Bus.FifoWriteCount==0U&&Bus.ColdTraceCount==0U&&Bus.Index==7U&&!Bus.Locked);
    for(J=0U;J<I;++J)
      CHECK(Bus.CsrTrace[J].Value==Values[J]&&Bus.CsrEndpoint[J]==Endpoint[J]);
    CHECK(Bus.Registers[REG_INTRUSBE]==0U&&ReadRegister16(REG_INTRTXE)==0U&&
          ReadRegister16(REG_INTRRXE)==0U&&ReadRegister16(REG_HSDMA_CTRL0)==0U&&
          Bus.Registers[REG_POWER]==0x40U&&Bus.Registers[REG_DEVCTL]==0x99U);
  }
  return 1;
}

static int
TestRetainedHostReset(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_STATUS Status;
  const uint32_t Offset[] = {REG_DEVCTL,REG_POWER,REG_INTRUSBE,REG_INTRTX,
                            REG_HSDMA_CTRL0+7U*16U,REG_QMU_GCSR+2U};
  const uint8_t Value[] = {0x05U,0x23U,0x12U,1U,1U,1U};

  ResetBus(); ResetColdMock(); Io=MakeIo();
  SetRegister8(REG_DEVCTL,0x5dU); SetRegister8(REG_POWER,0x21U);
  SetRegister16(REG_SWRST,0x6108U);
  Bus.ResetResponse=3U;
  CHECK(MtkMusbPlatformResetRetainedHost(&Io)==MtkMusbPlatformSuccess);
  CHECK(Bus.ColdResetTraceCount==11U && Bus.ResetWrites==1U && Bus.Index==7U && !Bus.Locked);
  for(unsigned I=0;I<8U;++I) {
    WRITE_TRACE T=Bus.ColdResetTrace[I];
    CHECK(T.Offset==REG_HSDMA_CTRL0+16U*I && T.Width==16U && T.Value==0U);
  }
  CHECK(Bus.ColdResetTrace[8].Offset==REG_HSDMA_CTRL0 &&
        Bus.ColdResetTrace[8].Width==32U && Bus.ColdResetTrace[8].Value==0x600U);
  CHECK(Bus.ColdResetTrace[9].Offset==REG_HSDMA_CTRL0 &&
        Bus.ColdResetTrace[9].Width==32U && Bus.ColdResetTrace[9].Value==0x4600U);
  CHECK(Bus.ResetTrace.Width==16U && Bus.ResetTrace.Value==0x610bU);
  CHECK(Bus.ColdTraceCount==0U && Bus.FifoWriteCount==0U && Bus.AckWrites==0U);
  CHECK(Bus.IrqWrites==3U && !ReadRegister16(REG_INTRTXE) &&
        !ReadRegister16(REG_INTRRXE) && !Bus.Registers[REG_INTRUSBE]);
  CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io)==MtkMusbPlatformSuccess && Bus.AckWrites==3U);

  for(unsigned I=0;I<sizeof(Value);++I) {
    ResetBus(); Io=MakeIo();
    SetRegister8(REG_DEVCTL,0x5dU); SetRegister8(REG_POWER,0x21U);
    SetRegister8(Offset[I],Value[I]);
    Status=MtkMusbPlatformResetRetainedHost(&Io);
    CHECK(Status==(I<2U?MtkMusbPlatformNotReady:MtkMusbPlatformQuiescenceFailed));
    CHECK(Bus.ColdResetTraceCount==0U && Bus.ColdTraceCount==0U &&
          Bus.Index==7U && !Bus.Locked);
  }
  ResetBus(); Io=MakeIo();
  SetRegister8(REG_DEVCTL,0x5dU); SetRegister8(REG_POWER,0x21U);
  Bus.TxCsr[1]=1U;
  CHECK(MtkMusbPlatformResetRetainedHost(&Io)==MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.ColdResetTraceCount==0U && Bus.Index==7U && !Bus.Locked);
  Bus.TxCsr[1]=0U;
  Bus.FailWriteOffset=REG_SWRST;
  CHECK(MtkMusbPlatformResetRetainedHost(&Io)==MtkMusbPlatformIoError);
  CHECK(Bus.ResetWrites==0U && Bus.ColdTraceCount==0U && !Bus.Locked);
  Bus.FailWriteOffset=-1;
  Bus.ResetResponse=7U;
  CHECK(MtkMusbPlatformResetRetainedHost(&Io)==MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.ResetWrites==1U && Bus.ColdTraceCount==0U && !Bus.Locked);
  return 1;
}

static int
TestIdleInterruptAcknowledge(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  unsigned i;
  const uint32_t Offsets[] = {REG_INTRRX, REG_INTRTX, REG_HSDMA_INTR};

  ResetBus(); Io = MakeIo();
  Bus.Index = 5U;
  WriteRegister16(REG_INTRTX, 0x1234U); WriteRegister16(REG_INTRRX, 0x0042U);
  Bus.Registers[REG_HSDMA_INTR] = 0xa5U;
  Bus.Registers[REG_HSDMA_INTR+1U] = 0x5aU; // never write reserved adjacent byte
  Bus.Registers[REG_INTRUSB] = 0x30U;
  CHECK(MtkMusbPlatformCheckQuiescent(&Io,0U) == MtkMusbPlatformQuiescenceFailed);
  CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io) == MtkMusbPlatformSuccess);
  CHECK(Bus.AckWrites == 3U && Bus.Index == 5U && !Bus.Locked);
  CHECK(ReadRegister16(REG_INTRTX) == 0U && ReadRegister16(REG_INTRRX) == 0U);
  CHECK(Bus.Registers[REG_HSDMA_INTR] == 0U && Bus.Registers[REG_HSDMA_INTR+1U] == 0x5aU);
  CHECK(Bus.Registers[REG_INTRUSB] == 0x30U && Bus.FifoWriteCount == 0U);

  ResetBus(); Io = MakeIo(); WriteRegister16(REG_INTRTX, 1U);
  Bus.TxCsr[1] = 1U;
  CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io) == MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.AckWrites == 0U && ReadRegister16(REG_INTRTX) == 1U && !Bus.Locked);

  ResetBus(); Io = MakeIo(); WriteRegister16(REG_INTRRX, 8U); Bus.IgnoreAck = 1;
  CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io) == MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.AckWrites == 1U && ReadRegister16(REG_INTRRX) == 8U && !Bus.Locked);

  for (i = 0U; i < sizeof(Offsets)/sizeof(Offsets[0]); ++i) {
    ResetBus(); Io = MakeIo(); Bus.Index = 7U;
    WriteRegister16(REG_INTRTX, 1U); WriteRegister16(REG_INTRRX, 2U);
    Bus.Registers[REG_HSDMA_INTR] = 4U;
    Bus.FailWriteOffset = (int)Offsets[i];
    CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io) == MtkMusbPlatformIoError);
    CHECK(Bus.Index == 7U && !Bus.Locked && Bus.FifoWriteCount == 0U);
  }
  return 1;
}

static int
TestInheritedFifoPreparation(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_EVIDENCE E;
  MTK_MUSB_PLATFORM_READY_RESULT R;
  unsigned Bad;
  ResetBus(); Io=MakeIo(); E=MakeEvidence();
  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID);
  CHECK(MtkMusbPlatformConfigureHostFifo(&Io)==MtkMusbPlatformQuiescenceFailed);
  CHECK(Bus.FifoWriteCount==0);
  CHECK(MtkMusbPlatformConfigureInheritedHostFifo(&Io,&E)==MtkMusbPlatformSuccess);
  CHECK(Bus.FifoWriteCount==32 && Bus.Index==7 && !Bus.Locked && Bus.Ep0FifoAccess==0U);
  CHECK(Bus.Registers[REG_DEVCTL]==(DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID));
  CHECK(MtkMusbPlatformEvaluateReady(&Io,&E,&R)==MtkMusbPlatformSuccess);
  CHECK(MtkMusbPlatformConfigureInheritedHostFifo(&Io,&E)==MtkMusbPlatformSuccess);
  CHECK(Bus.FifoWriteCount==32);

  ResetBus(); Io=MakeIo(); E=MakeEvidence();
  SetTypecOnlyGpioEvidence(&E);
  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID);
  CHECK(MtkMusbPlatformConfigureInheritedHostFifo(&Io,&E)==MtkMusbPlatformSuccess);
  CHECK(Bus.FifoWriteCount==32 && Bus.Index==7 && !Bus.Locked && Bus.Ep0FifoAccess==0U);

  ResetBus(); Io=MakeIo(); E=MakeEvidence();
  E.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  E.IdLowRoleToken=0x33U; E.Sc8989xBoostEnabled=0U;
  E.Gpio[4].PhysicalInput=0U;
  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID);
  CHECK(MtkMusbPlatformConfigureInheritedHostFifo(&Io,&E)==MtkMusbPlatformSuccess);
  CHECK(Bus.FifoWriteCount==32 && Bus.Index==7 && !Bus.Locked && Bus.Ep0FifoAccess==0U);

  for(Bad=0;Bad<8;++Bad) {
    ResetBus(); Io=MakeIo(); E=MakeEvidence();
    SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID);
    switch(Bad) {
      case 0:E.Sc8989xBoostReadValid=0;break;
      case 1:E.PhyDtm1=0;break;
      case 2:E.ClockEnabledMask=0;break;
      case 3:E.Gpio[0].PhysicalInput=2;break;
      case 4:Bus.TxCsr[1]=1;break;
      case 5:Bus.Registers[REG_INTRUSBE]=1;break;
      case 6:Bus.Registers[REG_DEVCTL]=DEVCTL_SESSION|DEVCTL_HOSTMODE;break;
      case 7:Bus.Registers[REG_HSDMA_CTRL0]=1;break;
    }
    CHECK(MtkMusbPlatformConfigureInheritedHostFifo(&Io,&E)==MtkMusbPlatformNotReady);
    CHECK(Bus.FifoWriteCount==0 && Bus.Index==7 && !Bus.Locked);
  }
  ResetBus(); Io=MakeIo(); E=MakeEvidence();
  SetRegister8(REG_DEVCTL,DEVCTL_SESSION|DEVCTL_HOSTMODE|DEVCTL_VBUS_VALID);
  Bus.FailWriteOffset=REG_TXFIFOSZ; Bus.FailWriteOnce=1;
  CHECK(MtkMusbPlatformConfigureInheritedHostFifo(&Io,&E)==MtkMusbPlatformIoError);
  CHECK(Bus.Index==7 && !Bus.Locked && Bus.FifoWriteCount==0);
  CHECK(MtkMusbPlatformConfigureInheritedHostFifo(&Io,NULL)==MtkMusbPlatformInvalid);
  return 1;
}

static int
TestRetainedFifoRestoration(void)
{
  const uint8_t Host = DEVCTL_SESSION | DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID;
  MTK_MUSB_PLATFORM_IO Io;
  unsigned Fault;

  ResetBus(); Io = MakeIo();
  SetRegister8(REG_DEVCTL, Host);
  SetRegister8(REG_POWER, 0x31U);
  SetRegister8(REG_INTRUSB, 0x30U);
  CHECK(MtkMusbPlatformVerifyHostFifo(&Io) == MtkMusbPlatformProfileMismatch);
  CHECK(MtkMusbPlatformRestoreRetainedHostFifo(&Io) == MtkMusbPlatformSuccess);
  CHECK(MtkMusbPlatformVerifyHostFifo(&Io) == MtkMusbPlatformSuccess);
  CHECK(Bus.FifoWriteCount == 32U && Bus.Index == 7U && !Bus.Locked);
  CHECK(Bus.Registers[REG_INTRUSB] == 0x30U && Bus.Ep0FifoAccess == 0U);
  CHECK(Bus.Registers[REG_DEVCTL] == Host && Bus.Registers[REG_POWER] == 0x31U);

  for (Fault = 0; Fault < 7U; ++Fault) {
    ResetBus(); Io = MakeIo();
    SetRegister8(REG_DEVCTL, Host);
    switch (Fault) {
      case 0: SetRegister8(REG_DEVCTL, Host & ~DEVCTL_SESSION); break;
      case 1: SetRegister8(REG_DEVCTL, Host | 0x80U); break;
      case 2: SetRegister8(REG_DEVCTL, Host & ~DEVCTL_VBUS_VALID); break;
      case 3: SetRegister8(REG_POWER, POWER_SOFTCONN); break;
      case 4: SetRegister8(REG_POWER, POWER_SUSPENDM); break;
      case 5: Bus.TxCsr[1] = 1U; break;
      case 6: SetRegister8(REG_HSDMA_CTRL0, 1U); break;
    }
    CHECK(MtkMusbPlatformRestoreRetainedHostFifo(&Io) ==
          (Fault < 5U ? MtkMusbPlatformNotReady : MtkMusbPlatformQuiescenceFailed));
    CHECK(Bus.FifoWriteCount == 0U && Bus.Index == 7U && !Bus.Locked);
  }

  ResetBus(); Io = MakeIo();
  SetRegister8(REG_DEVCTL, Host);
  Bus.FailWriteOffset = REG_RXFIFOSZ; Bus.FailWriteOnce = 1;
  CHECK(MtkMusbPlatformRestoreRetainedHostFifo(&Io) == MtkMusbPlatformIoError);
  CHECK(Bus.FifoWriteCount == 2U && Bus.Index == 7U && !Bus.Locked);
  return 1;
}

static void
PrintColdRegisterTrace(void)
{
  unsigned I;
  int First;

  printf("MUSB_COLD_TRACE_JSON {\"session_set_register_writes\":[");
  First = 1;
  for (I = 0U; I < Bus.ColdTraceCount; ++I) {
    const WRITE_TRACE *Trace = &Bus.ColdTrace[I];
    if ((Trace->Offset == REG_DEVCTL) && ((Trace->Value & DEVCTL_SESSION) != 0U)) {
      printf("%s[%u,%u,%u]", First ? "" : ",", Trace->Offset, Trace->Width, Trace->Value);
      First = 0;
    }
  }
  printf("],\"power_register_writes\":[");
  First = 1;
  for (I = 0U; I < Bus.ColdTraceCount; ++I) {
    const WRITE_TRACE *Trace = &Bus.ColdTrace[I];
    if (Trace->Offset == REG_POWER) {
      printf("%s[%u,%u,%u]", First ? "" : ",", Trace->Offset, Trace->Width, Trace->Value);
      First = 0;
    }
  }
  printf("],\"trial99_cases\":[");
  for (I=0U; I<2U; ++I) {
    printf("%s{\"host_power_path\":%u,\"initial_devctl\":152,\"initial_power\":33,"
           "\"post_phy_devctl\":153,\"post_phy_power\":99,"
           "\"readiness_model\":\"withheld_until_POWER_initialization\","
           "\"host_phy_after_register_writes\":%u,\"register_writes\":["
           "[%u,%u,%u],[%u,%u,%u],[%u,%u,%u]]}",
           I ? "," : "", I+1U, Trial99PhyAtWrite[I],
           Trial99Trace[I][0].Offset, Trial99Trace[I][0].Width, Trial99Trace[I][0].Value,
           Trial99Trace[I][1].Offset, Trial99Trace[I][1].Width, Trial99Trace[I][1].Value,
           Trial99Trace[I][2].Offset, Trial99Trace[I][2].Width, Trial99Trace[I][2].Value);
  }
  printf("],\"trial101_physical_response_replays\":[");
  static const char *Modes[]={"sticky_suspend","reassert_first_read","reassert_second_read",
                             "reassert_after_power_guard","reassert_on_poll","no_hostmode"};
  for (unsigned Path=0U;Path<2U;++Path) for(unsigned Mode=0U;Mode<6U;++Mode) {
    const PHYSICAL_REPLAY *R=&Trial101Replay[Path][Mode];
    printf("%s{\"host_power_path\":%u,\"model\":\"%s\",\"start_status\":%u,"
           "\"start_step\":%u,\"guard_phase\":%u,\"power\":%u,\"devctl\":%u,"
           "\"power_verified\":%u,\"ready\":false,\"stop_status_in_fault\":%u,"
           "\"stop_register_writes_in_fault\":%u,\"physical_recovery_proven\":false}",
           Path||Mode?",":"",Path+1U,Modes[Mode],R->StartStatus,R->StartStep,
           R->GuardPhase,R->Power,R->Devctl,R->PowerVerified,R->StopStatus,R->StopWrites);
  }
  printf("]}\n");
}

static int
TestColdStartFailureDiagnostics(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.FailPhase=MtkMusbPlatformColdBeforeSession; Cold.FailGuardOnce=1;
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.Signature==0U && State.OwnedMask==0U &&
        State.LastStatus==MtkMusbPlatformNotReady &&
        State.StartStep==MtkMusbPlatformColdStartGuard &&
        State.GuardPhase==MtkMusbPlatformColdBeforeSession &&
        State.GuardReason==MtkMusbPlatformColdGuardFresh);
  CHECK(Bus.ColdTraceCount==0U && !Bus.Locked && !Cold.CallbackWhileLocked);

  // An unowned rejection preserves its reason but permits a clean new start.
  SetRegister8(REG_DEVCTL,0x1cU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  CHECK(State.LastStatus==MtkMusbPlatformSuccess &&
        State.StartStep==MtkMusbPlatformColdStartReady &&
        State.GuardReason==MtkMusbPlatformColdGuardPassed);
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_UNKNOWN;
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.Signature==0U && State.StartStep==MtkMusbPlatformColdStartGuard &&
        State.GuardReason==MtkMusbPlatformColdGuardSource &&
        State.LastStatus==MtkMusbPlatformNotReady && Bus.ColdTraceCount==0U);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  SetRegister8(REG_DEVCTL,0x1cU);
  Cold.FailPhase=MtkMusbPlatformColdAfterPower; Cold.FailGuardOnce=1;
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.StartStep==MtkMusbPlatformColdStartAfterPower &&
        State.GuardPhase==MtkMusbPlatformColdAfterPower &&
        State.GuardReason==MtkMusbPlatformColdGuardFresh &&
        State.LastStatus==MtkMusbPlatformNotReady && State.OwnedMask==7U);
  MTK_MUSB_PLATFORM_COLD_HOST_STATE Failure=State;
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  CHECK(State.Signature==0U && State.StartStep==0U && State.GuardReason==0U &&
        Failure.StartStep==MtkMusbPlatformColdStartAfterPower &&
        Failure.GuardReason==MtkMusbPlatformColdGuardFresh &&
        Failure.LastStatus==MtkMusbPlatformNotReady);
  CHECK(!Bus.Locked && !Cold.CallbackWhileLocked && Bus.Index==7U);
  return 1;
}

static int
TestStockColdResetAndPartialStart(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};
  ResetBus();ResetColdMock();Io=MakeIo();Callbacks=MakeColdCallbacks();
  Io.Write32=NULL;
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformUnsupported);
  CHECK(!State.Signature && !Bus.WriteCount && !Cold.GuardCalls);
  ResetBus();ResetColdMock();Io=MakeIo();Callbacks=MakeColdCallbacks();
  SetRegister8(REG_DEVCTL,0x1cU);SetRegister16(REG_SWRST,0x614bU);
  Bus.Registers[0x207U]=0xa0U;
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  CHECK(Bus.ColdResetTraceCount==12U);
  for (unsigned I=0U;I<8U;++I)
    CHECK(Bus.ColdResetTrace[I].Offset==0x204U+16U*I &&
          Bus.ColdResetTrace[I].Width==16U && !Bus.ColdResetTrace[I].Value);
  CHECK(Bus.ColdResetTrace[8].Width==32U && Bus.ColdResetTrace[8].Value==0xa0000600U);
  CHECK(Bus.ColdResetTrace[9].Width==32U && Bus.ColdResetTrace[9].Value==0xa0004600U);
  CHECK(Bus.ColdResetTrace[10].Offset==0x74U && Bus.ColdResetTrace[10].Width==16U &&
        Bus.ColdResetTrace[10].Value==0x614bU);
  CHECK(Bus.ColdResetTrace[11].Offset==0x74U && Bus.ColdResetTrace[11].Width==8U &&
        Bus.ColdResetTrace[11].Value==0x8bU);
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);

  for (unsigned Failure=0U;Failure<3U;++Failure) {
    ResetBus();ResetColdMock();Io=MakeIo();Callbacks=MakeColdCallbacks();
    State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};SetRegister8(REG_DEVCTL,0x1cU);
    if (Failure<2U) Cold.FailStartupDelay=Failure+1U;
    else Cold.FailPhyOnce=1;
    CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformIoError);
    CHECK(!Bus.Registers[REG_POWER]);
    CHECK(!Bus.ColdResetTraceCount);
    if (Failure==0U) CHECK(!State.Signature && !State.OwnedMask && !Cold.PhyCalls);
    else if (Failure==1U) {
      CHECK(State.Kind==MtkMusbPlatformColdStateNeedsStop &&
            State.OwnedMask==MTK_MUSB_PLATFORM_COLD_OWNS_SESSION &&
            !State.PhyInvalidAttempted && !State.PhyHostAttempted &&
            (Bus.Registers[REG_DEVCTL]&1U)==0U);
      CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
      CHECK(!State.Signature && !State.OwnedMask);
    } else {
      CHECK(State.Kind==MtkMusbPlatformColdStateNeedsStop &&
            State.OwnedMask==(MTK_MUSB_PLATFORM_COLD_OWNS_SESSION |
                             MTK_MUSB_PLATFORM_COLD_OWNS_PHY) &&
            !State.PhyInvalidAttempted && State.PhyHostAttempted &&
            (Bus.Registers[REG_DEVCTL]&1U)!=0U);
      CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
      CHECK(!State.Signature && !State.OwnedMask);
    }
    CHECK(!Bus.Locked && !Cold.CallbackWhileLocked);
  }
  for (unsigned After=0U;After<2U;++After) for (unsigned I=0U;I<9U;++I) {
    ResetBus();ResetColdMock();Io=MakeIo();Callbacks=MakeColdCallbacks();
    State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};SetRegister8(REG_DEVCTL,0x1cU);
    uint32_t Offset=I<8U ? 0x204U+16U*I : 0x74U;
    if (After) { Bus.FailWriteAfterOffset=(int)Offset;Bus.FailWriteAfterOnce=1; }
    else { Bus.FailWriteOffset=(int)Offset;Bus.FailWriteOnce=1; }
    CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformIoError);
    CHECK(State.StartStep==MtkMusbPlatformColdStartReset && !State.PowerAttempted);
    CHECK(State.Kind==MtkMusbPlatformColdStateNeedsStop && State.OwnedMask==5U);
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(!State.Signature && !(Bus.Registers[REG_DEVCTL]&1U) && !Bus.Registers[REG_POWER]);
  }
  for (unsigned Mode=1U;Mode<=2U;++Mode) {
    ResetBus();ResetColdMock();Io=MakeIo();Callbacks=MakeColdCallbacks();
    State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};SetRegister8(REG_DEVCTL,0x1cU);
    Cold.SessionSetFailure=Mode;
    CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformIoError);
    CHECK(!State.PhyInvalidAttempted && !State.PhyHostAttempted &&
          State.OwnedMask==MTK_MUSB_PLATFORM_COLD_OWNS_SESSION &&
          (Bus.Registers[REG_DEVCTL]&1U));
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(!State.Signature && !(Bus.Registers[REG_DEVCTL]&1U) &&
          Cold.PhyMode==MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG);
  }
  for (unsigned At=1U;At<=2U;++At) for (unsigned After=0U;After<2U;++After) {
    ResetBus();ResetColdMock();Io=MakeIo();Callbacks=MakeColdCallbacks();
    State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};SetRegister8(REG_DEVCTL,0x1cU);
    Cold.Reset32FailAt=At;Cold.Reset32After=After;
    CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformIoError);
    CHECK(State.StartStep==MtkMusbPlatformColdStartReset && !State.PowerAttempted &&
          Cold.Reset32Count==At && State.OwnedMask==5U);
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(!State.Signature && !(Bus.Registers[REG_DEVCTL]&1U));
  }
  return 1;
}

static int
TestInactiveDmaConfigurationAfterClear(void)
{
  /* Trial107 retained direction/endpoint bits (0x12), with DMA disabled.
   * A separate injected enable bit must still stop reset on every channel. */
  for (unsigned Path=0U;Path<2U;++Path) for (unsigned Fault=0U;Fault<=8U;++Fault) {
    MTK_MUSB_PLATFORM_COLD_HOST_STATE State={0};
    ResetBus();ResetColdMock();
    MTK_MUSB_PLATFORM_IO Io=MakeIo();
    MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks=MakeColdCallbacks();
    SetRegister8(REG_DEVCTL,0x98U);SetRegister8(REG_POWER,0x21U);
    Cold.DeviceIdHigh=Cold.HostAfterPower=Cold.DropSessionBeforeReset=1;
    if (Path) {
      Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
      Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
    }
    for (unsigned I=0U;I<8U;++I) {
      SetRegister16(REG_HSDMA_CTRL0+16U*I,0x12U);
      Bus.DmaControlAfterClear[I]=(uint16_t)(Fault==I+1U?0x13U:0x12U);
    }
    MTK_MUSB_PLATFORM_STATUS Status=DriveColdStart(&Io,&Callbacks,&State);
    if (!Fault) {
      CHECK(Status==MtkMusbPlatformSuccess && State.ResetCheck==MtkMusbPlatformResetComplete);
      CHECK(State.PowerVerified && Bus.ResetWrites==1U && Bus.ColdResetTraceCount==12U);
      for (unsigned I=0U;I<8U;++I) {
        CHECK(Bus.ColdResetTrace[I].Offset==REG_HSDMA_CTRL0+16U*I &&
              Bus.ColdResetTrace[I].Width==16U && Bus.ColdResetTrace[I].Value==0U);
        CHECK(ReadRegister16(REG_HSDMA_CTRL0+16U*I)==(I?0x12U:0x4612U));
      }
      CHECK(Bus.ColdResetTrace[8].Value==0x612U && Bus.ColdResetTrace[9].Value==0x4612U);
    } else {
      CHECK(Status==MtkMusbPlatformConflict && State.StartStep==MtkMusbPlatformColdStartReset);
      CHECK(State.ResetCheck==MtkMusbPlatformResetDmaClear &&
            State.ResetOffset==REG_HSDMA_CTRL0+16U*(Fault-1U) && State.ResetValue==0x13U);
      CHECK(!State.PowerAttempted && !Bus.ResetWrites && Bus.ColdResetTraceCount==Fault);
      SetRegister16(State.ResetOffset,0x12U);
      /* End the injected peripheral response before testing owned cleanup. */
      Bus.Registers[REG_POWER] &= (uint8_t)~POWER_SOFTCONN;
    }
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(!State.Signature && !(Bus.Registers[REG_DEVCTL]&1U) &&
          (Bus.Registers[REG_POWER]&0x6fU)==0x21U && !Bus.Locked && !Cold.CallbackWhileLocked);
  }
  return 1;
}

static int
TestPostResetInterruptState(void)
{
  /* These declared peripheral responses exercise the stock post-reset IRQ
   * shutdown gap. Trial109 did not record its exact busy register. */
  for (unsigned Path=0U;Path<2U;++Path) for (unsigned Model=1U;Model<=12U;++Model) {
    MTK_MUSB_PLATFORM_COLD_HOST_STATE State={0};
    ResetBus();ResetColdMock();
    MTK_MUSB_PLATFORM_IO Io=MakeIo();
    MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks=MakeColdCallbacks();
    SetRegister8(REG_DEVCTL,0x98U);SetRegister8(REG_POWER,0x21U);
    Cold.DeviceIdHigh=Cold.HostAfterPower=Cold.DropSessionBeforeReset=1;
    if(Path) {
      Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
      Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
    }
    Bus.ResetResponse=Model;Bus.Registers[REG_INTRUSB]=0x30U;
    MTK_MUSB_PLATFORM_STATUS Status=DriveColdStart(&Io,&Callbacks,&State);
    if(Model<=3U && Status!=MtkMusbPlatformSuccess)
      printf("post-reset model=%u path=%u status=%u step=%u reset=%u offset=%x value=%x\n",
             Model,Path,Status,State.StartStep,State.ResetCheck,State.ResetOffset,State.ResetValue);
    CHECK(Bus.ResetResponseInjected && Bus.ResetWrites==1U);
    if(Model<=3U) {
      CHECK(Status==MtkMusbPlatformSuccess && State.ResetCheck==MtkMusbPlatformResetComplete);
      CHECK(State.PowerVerified && Bus.Registers[REG_INTRUSBE]==0U &&
            ReadRegister16(REG_INTRTXE)==0U && ReadRegister16(REG_INTRRXE)==0U);
      CHECK(ReadRegister16(REG_INTRTX)==0U && ReadRegister16(REG_INTRRX)==0U &&
            Bus.Registers[REG_HSDMA_INTR]==0U && Bus.AckWrites==(Model>=2U?3U:0U));
      CHECK(Bus.IrqWrites==3U && Bus.IrqTrace[0].Offset==REG_INTRUSBE &&
            Bus.IrqTrace[1].Offset==REG_INTRTXE && Bus.IrqTrace[2].Offset==REG_INTRRXE);
    } else {
      const uint32_t Offsets[]={0U,0U,0U,0U,REG_HSDMA_CTRL0+7U*16U,REG_TXCSR,
        REG_QMU_GCSR,REG_INTRUSBE,REG_INTRTXE,REG_INTRRXE,REG_INTRUSBE,REG_INTRTXE,REG_INTRRXE};
      const uint32_t Values[]={0U,0U,0U,0U,0x13U,8U,0x10000U,0x12U,0x1ffU,0x1feU,0U,0U,0U};
      CHECK(Status==(Model>=10U?MtkMusbPlatformIoError:MtkMusbPlatformQuiescenceFailed));
      CHECK(State.StartStep==MtkMusbPlatformColdStartReset &&
            State.ResetCheck==MtkMusbPlatformResetFinalIdle && !State.PowerAttempted);
      CHECK(State.ResetOffset==Offsets[Model] && State.ResetValue==Values[Model]);
      CHECK(Bus.AckWrites==0U && ReadRegister16(REG_INTRTX)==0x1234U &&
            ReadRegister16(REG_INTRRX)==0x42U && Bus.Registers[REG_HSDMA_INTR]==0xa5U);
      /* End injected busy/sticky states before separately checking cleanup. */
      Bus.ResetStickyMask=0U;Bus.Registers[REG_INTRUSBE]=0U;
      WriteRegister16(REG_INTRTXE,0U);WriteRegister16(REG_INTRRXE,0U);
      WriteRegister16(REG_HSDMA_CTRL0+7U*16U,0x12U);Bus.TxCsr[0]=0U;
      Bus.Registers[REG_QMU_GCSR+2U]=0U;Bus.Registers[REG_POWER]&=(uint8_t)~POWER_SOFTCONN;
      CHECK(MtkMusbPlatformAcknowledgeIdleInterrupts(&Io)==MtkMusbPlatformSuccess);
    }
    CHECK(Bus.Registers[REG_INTRUSB]==0x30U && Bus.Index==7U && !Bus.Locked);
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(!State.Signature && !(Bus.Registers[REG_DEVCTL]&1U) && !Cold.CallbackWhileLocked);
  }
  return 1;
}

static int
TestSessionLostBeforeControllerReset(void)
{
  /* The supplied screen fixes the values, not the instant SESSION cleared.
   * Inject a one-time clear while the fresh board guard runs. HOSTMODE only
   * follows a later real POWER write in this declared MMIO model. */
  for (unsigned Path=0U;Path<2U;++Path) for (unsigned Fault=0U;Fault<5U;++Fault) {
    MTK_MUSB_PLATFORM_COLD_HOST_STATE State={0};
    ResetBus();ResetColdMock();
    MTK_MUSB_PLATFORM_IO Io=MakeIo();
    MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks=MakeColdCallbacks();
    SetRegister8(REG_DEVCTL,0x98U);SetRegister8(REG_POWER,0x21U);
    Cold.DeviceIdHigh=Cold.HostAfterPower=1;
    if (Path) {
      Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
      Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
    }
    Cold.DropSessionBeforeReset=1U;Cold.ResetSessionFault=Fault;
    MTK_MUSB_PLATFORM_STATUS Status=DriveColdStart(&Io,&Callbacks,&State);
    if (!Fault) {
      CHECK(Status==MtkMusbPlatformSuccess);
      CHECK(State.ResetCheck==MtkMusbPlatformResetComplete);
      CHECK(State.Kind==MtkMusbPlatformColdStateStarted && State.PowerVerified &&
            Bus.Registers[REG_DEVCTL]==0x1dU && Bus.Registers[REG_POWER]==0x31U);
      CHECK(Bus.ColdTraceCount==4U && Bus.ColdTrace[2].Offset==REG_DEVCTL &&
            Bus.ColdTrace[2].Value==0x99U && Bus.ColdResetTraceCount==12U);
    } else {
      CHECK(Status==(Fault<=2U?MtkMusbPlatformIoError:
                     Fault==3U?MtkMusbPlatformConflict:MtkMusbPlatformQuiescenceFailed));
      CHECK(State.StartStep==MtkMusbPlatformColdStartReset && !State.PowerAttempted &&
            !Bus.ColdResetTraceCount);
      if (Fault<=2U) CHECK(State.ResetCheck==MtkMusbPlatformResetSession &&
                           State.ResetOffset==REG_DEVCTL);
      if (Fault==3U) {
        CHECK(State.ResetCheck==MtkMusbPlatformResetStartPower &&
              State.ResetOffset==REG_POWER && State.ResetValue==0x79U);
        Bus.Registers[REG_POWER] &= (uint8_t)~POWER_RESET;
      }
      if (Fault==4U) SetRegister16(REG_HSDMA_CTRL0,0U);
      /* Leave only the modeled inherited read-only HSMODE and original bits. */
      Bus.Registers[REG_POWER] &= (uint8_t)~POWER_SOFTCONN;
    }
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(!State.Signature && !(Bus.Registers[REG_DEVCTL]&DEVCTL_SESSION) &&
          (Bus.Registers[REG_POWER]&0x6fU)==0x21U && !Bus.Locked && !Cold.CallbackWhileLocked);
  }
  return 1;
}

static int
TestColdHostStartStopAndWriteTrace(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus();
  ResetColdMock();
  Io = MakeIo();
  Callbacks = MakeColdCallbacks();
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(State.Kind == MtkMusbPlatformColdStateStarted);
  CHECK(State.OwnedMask == (MTK_MUSB_PLATFORM_COLD_OWNS_SESSION |
                            MTK_MUSB_PLATFORM_COLD_OWNS_POWER |
                            MTK_MUSB_PLATFORM_COLD_OWNS_PHY));
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1DU);
  CHECK(Bus.Registers[REG_POWER] == (POWER_ENSUSPEND | POWER_HSENAB));
  CHECK(Bus.ColdTraceCount == 3U);
  CHECK(Bus.ColdTrace[0].Offset == REG_DEVCTL &&
        Bus.ColdTrace[0].Width == 8U && Bus.ColdTrace[0].Value == 0x1CU);
  CHECK(Bus.ColdTrace[1].Offset == REG_DEVCTL &&
        Bus.ColdTrace[1].Width == 8U && Bus.ColdTrace[1].Value == 0x1DU);
  CHECK(Bus.ColdTrace[2].Offset == REG_POWER &&
        Bus.ColdTrace[2].Width == 8U &&
        Bus.ColdTrace[2].Value == (POWER_ENSUSPEND | POWER_HSENAB));
  CHECK(Cold.SettleCalls == 1U && Cold.OtgHoldCalls == 1U);
  CHECK(!State.PhyInvalidAttempted && State.PhyHostVerified &&
        Cold.PhyCalls == 1U && Bus.ResetWrites == 1U);
  CHECK(ReadRegister16(REG_SWRST) == 0x83U);
  CHECK(ReadRegister16(REG_HSDMA_CTRL0) == 0x4600U);
  CHECK(Bus.FifoWriteCount == 0U && Bus.AckWrites == 0U);
  CHECK(!Cold.CallbackWhileLocked && !Bus.Locked && Bus.Index == 7U);

  /* This trace is captured from the actual C core writes for the raw
   * DEVCTL=0x1c/POWER=0 canonical cold-start fixture. */
  PrintColdRegisterTrace();
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(State.Signature == 0U && State.OwnedMask == 0U);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && Bus.Registers[REG_POWER] == 0U);
  CHECK(Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG);
  CHECK(!Cold.CallbackWhileLocked && !Bus.Locked && Bus.Index == 7U);
  return 1;
}

static int
TestColdHostDelayedReadinessAndPreservation(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus();
  ResetColdMock();
  Io = MakeIo();
  Callbacks = MakeColdCallbacks();
  Cold.ReadyAfterPolls = 2U;
  SetRegister8(REG_POWER, 0x80U);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Cold.DelayCalls == 2U && !Cold.CallbackWhileLocked);
  CHECK(Bus.Registers[REG_DEVCTL] ==
        (DEVCTL_SESSION | DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID));
  CHECK(Bus.Registers[REG_POWER] ==
        (0x80U | POWER_ENSUSPEND | POWER_HSENAB));
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == (DEVCTL_HOSTMODE | DEVCTL_VBUS_VALID));
  CHECK(Bus.Registers[REG_POWER] == 0x80U);
  return 1;
}

/* Trial99 froze DEVCTL=99, POWER=63 after the successful HOST PHY call.
 * This model deliberately withholds host readiness until the original-style
 * host POWER initialization; it does not claim to emulate the physical PHY. */
static int
TestColdHostPowerBeforeRoleReadback(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State;
  for (unsigned Path=0U; Path<2U; ++Path) {
    ResetBus(); ResetColdMock(); State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
    Io=MakeIo(); Callbacks=MakeColdCallbacks();
    Cold.DeviceIdHigh=Cold.HostAfterPower=1;
    Cold.PowerAfterPhy=0x63U; Cold.PowerAfterPhyValid=1;
    if (Path) {
      Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
      Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
    }
    SetRegister8(REG_DEVCTL,0x98U); SetRegister8(REG_POWER,0x21U);
    CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(Bus.ColdTraceCount==3U && Bus.ColdTrace[0].Offset==REG_DEVCTL &&
          Bus.ColdTrace[0].Value==0x98U && Bus.ColdTrace[1].Offset==REG_DEVCTL &&
          Bus.ColdTrace[1].Value==0x99U && Bus.ColdTrace[2].Offset==REG_POWER &&
          Bus.ColdTrace[2].Value==0x21U);
    CHECK(Bus.Registers[REG_DEVCTL]==0x1dU && Bus.Registers[REG_POWER]==0x21U);
    CHECK(Cold.HostPhyAtRegisterWrite==2U);
    Trial99Trace[Path][0]=Bus.ColdTrace[0];Trial99Trace[Path][1]=Bus.ColdTrace[1];Trial99Trace[Path][2]=Bus.ColdTrace[2];
    Trial99PhyAtWrite[Path]=Cold.HostPhyAtRegisterWrite;
    CHECK(!Bus.Locked && !Cold.CallbackWhileLocked && Bus.Index==7U);
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(Bus.Registers[REG_POWER]==0x21U && Bus.Registers[REG_DEVCTL]==0x98U);
  }
  /* Exhaust every post-PHY POWER byte. Only SUSPENDM/SOFTCONN may be
   * retired; RESET/RESUME or changed enable bits must prevent the write.
   * The unrelated ISOUPDATE/HSMODE bits are preserved. */
  for (unsigned Path=0U; Path<2U; ++Path) for (unsigned Value=0U; Value<256U; ++Value) {
    ResetBus(); ResetColdMock(); State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
    Io=MakeIo(); Callbacks=MakeColdCallbacks();
    Cold.DeviceIdHigh=Cold.HostAfterPower=Cold.PowerAfterPhyValid=1;
    Cold.PowerAfterPhy=(uint8_t)Value;
    if (Path) {
      Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
      Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
    }
    SetRegister8(REG_DEVCTL,0x98U); SetRegister8(REG_POWER,0x21U);
    MTK_MUSB_PLATFORM_STATUS Status=DriveColdStart(&Io,&Callbacks,&State);
    if ((Value & 0x2dU)==0x21U) {
      CHECK(Status==MtkMusbPlatformSuccess);
      CHECK((Bus.Registers[REG_POWER]&0x63U)==0x21U &&
            (Bus.Registers[REG_POWER]&0x90U)==(Value&0x90U));
      CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
      CHECK(Bus.Registers[REG_POWER]==((Value&0x90U)|0x21U));
    } else {
      CHECK(Status!=MtkMusbPlatformSuccess && Bus.ColdTraceCount==2U);
      CHECK(State.Kind==MtkMusbPlatformColdStateNeedsStop);
    }
    CHECK(!Bus.Locked && !Cold.CallbackWhileLocked && Bus.Index==7U);
  }
  /* RESET, RESUME and new DMA activity still prohibit host POWER writes. */
  for (unsigned Unsafe=0U; Unsafe<3U; ++Unsafe) {
    ResetBus(); ResetColdMock(); State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
    Io=MakeIo(); Callbacks=MakeColdCallbacks();
    Cold.DeviceIdHigh=Cold.HostAfterPower=1;
    Cold.PowerAfterPhy=Unsafe==0U ? 0x67U : Unsafe==1U ? 0x6bU : 0x63U;
    Cold.PowerAfterPhyValid=1;
    Cold.DmaAfterPhy=Unsafe==2U;
    SetRegister8(REG_DEVCTL,0x98U); SetRegister8(REG_POWER,0x21U);
    CHECK(DriveColdStart(&Io,&Callbacks,&State)!=MtkMusbPlatformSuccess);
    CHECK(Bus.ColdTraceCount==2U && Bus.ColdTrace[0].Offset==REG_DEVCTL);
    CHECK(!Bus.Locked && !Cold.CallbackWhileLocked && Bus.Index==7U);
  }
  return 1;
}

/* A passing replay means the physical failure is detected, never that the
 * candidate can start a host. No write or callback synthesizes HOSTMODE. */
static int
TestColdHostPhysicalPowerResponses(void)
{
  for (unsigned Path=0U; Path<2U; ++Path) for (unsigned Mode=1U; Mode<=6U; ++Mode) {
    MTK_MUSB_PLATFORM_COLD_HOST_STATE State={0};
    ResetBus();ResetColdMock();
    MTK_MUSB_PLATFORM_IO Io=MakeIo();
    MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks=MakeColdCallbacks();
    Cold.DeviceIdHigh=Cold.HostModeStuck=1;
    Cold.PowerAfterPhyValid=1;Cold.PowerAfterPhy=0x63U;Cold.ReplayMode=Mode;
    if (Path) {
      Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
      Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
    }
    SetRegister8(REG_DEVCTL,0x98U);SetRegister8(REG_POWER,0x21U);
    MTK_MUSB_PLATFORM_STATUS Status=DriveColdStart(&Io,&Callbacks,&State);
    PHYSICAL_REPLAY *Replay=&Trial101Replay[Path][Mode-1U];
    Replay->StartStatus=Status;Replay->StartStep=State.StartStep;
    Replay->GuardPhase=State.GuardPhase;Replay->Power=Bus.Registers[REG_POWER];
    Replay->Devctl=Bus.Registers[REG_DEVCTL];Replay->PowerVerified=State.PowerVerified;
    CHECK(Status!=MtkMusbPlatformSuccess && State.Kind==MtkMusbPlatformColdStateNeedsStop);
    CHECK(Replay->Devctl==0x99U && Bus.ColdTraceCount==3U &&
          Bus.ColdTrace[2].Offset==REG_POWER && Bus.ColdTrace[2].Value==0x21U);
    if (Mode<=2U) {
      CHECK(Status==MtkMusbPlatformConflict && Replay->Power==0x23U &&
            State.StartStep==MtkMusbPlatformColdStartPower &&
            State.GuardPhase==MtkMusbPlatformColdBeforePower && !State.PowerVerified);
    } else if (Mode<=5U) {
      CHECK(Status==MtkMusbPlatformQuiescenceFailed && Replay->Power==0x23U &&
            State.StartStep==MtkMusbPlatformColdStartWaitHost && State.PowerVerified &&
            State.GuardPhase==MtkMusbPlatformColdAfterPower);
    } else {
      CHECK(Status==MtkMusbPlatformTimeout && Replay->Power==0x21U &&
            State.StartStep==MtkMusbPlatformColdStartWaitHost && State.PowerVerified &&
            Cold.DelayCalls==MTK_MUSB_PLATFORM_COLD_TIMEOUT_MS);
    }
    unsigned Before=Bus.ColdTraceCount;
    Status=MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State);
    Replay->StopStatus=Status;Replay->StopWrites=Bus.ColdTraceCount-Before;
    if (Mode<=5U) {
      CHECK(Status==MtkMusbPlatformQuiescenceFailed && Replay->StopWrites==0U &&
            State.Kind==MtkMusbPlatformColdStateNeedsStop &&
            Bus.Registers[REG_DEVCTL]==0x99U);
      // Removal is an explicit external model action, not driver recovery.
      Cold.ReplayMode=0U;Bus.Registers[REG_POWER]&=(uint8_t)~POWER_SUSPENDM;
      CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    } else CHECK(Status==MtkMusbPlatformSuccess);
    CHECK(State.Signature==0U && Bus.Registers[REG_DEVCTL]==0x98U &&
          Bus.Registers[REG_POWER]==0x21U && !Bus.Locked && !Cold.CallbackWhileLocked);
  }
  return 1;
}

static int
TestColdHostInheritedDeviceRole(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  /* The physical Type-C ID remains high until the PHY is forced HOST.
   * Only a previously stopped device session is eligible here. */
  const unsigned ReadyPolls[] = {0U, 2U, 180U, MTK_MUSB_PLATFORM_COLD_TIMEOUT_MS};
  for (unsigned Index = 0U; Index < sizeof(ReadyPolls) / sizeof(ReadyPolls[0]); ++Index) {
    ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
    Cold.DeviceIdHigh=1; Cold.ReadyAfterPolls=ReadyPolls[Index];
    SetRegister8(REG_DEVCTL,0x98U);
    CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(Cold.PhyCalls==1U && Cold.DelayCalls==ReadyPolls[Index]);
    CHECK(Bus.ColdTraceCount==3U && Bus.ColdTrace[0].Value==0x98U &&
          Bus.ColdTrace[1].Value==0x99U && Bus.ColdTrace[2].Offset==REG_POWER && Bus.Registers[REG_DEVCTL]==0x1dU);
    CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
    CHECK(Bus.Registers[REG_DEVCTL]==0x98U && Bus.Registers[REG_POWER]==0U &&
          State.Signature==0U && !Cold.CallbackWhileLocked && Bus.Index==7U);
  }
  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.DeviceIdHigh=Cold.HostModeStuck=1; SetRegister8(REG_DEVCTL,0x98U);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformTimeout);
  CHECK(Cold.PhyCalls==1U && Cold.DelayCalls==MTK_MUSB_PLATFORM_COLD_TIMEOUT_MS &&
        Bus.ColdTraceCount==3U && Bus.Registers[REG_POWER]==0x21U);
  CHECK(State.StartStep==MtkMusbPlatformColdStartWaitHost &&
        State.LastStatus==MtkMusbPlatformTimeout &&
        State.GuardPhase==MtkMusbPlatformColdAfterPower &&
        State.GuardReason==MtkMusbPlatformColdGuardPassed);
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL]==0x98U && State.Signature==0U);

  for (unsigned Unsafe=0U; Unsafe<3U; ++Unsafe) {
    ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
    Cold.DeviceIdHigh=1; SetRegister8(REG_DEVCTL,Unsafe==0U ? 0x99U : 0x98U);
    if (Unsafe==1U) Cold.DataRoleToken=0U;
    if (Unsafe==2U) SetRegister16(REG_HSDMA_CTRL0,1U);
    CHECK(DriveColdStart(&Io,&Callbacks,&State)!=MtkMusbPlatformSuccess);
    CHECK(State.Signature==0U && Bus.ColdTraceCount==0U && Cold.PhyCalls==0U);
  }
  return 1;
}

static int
TestColdHostIdLowPathAndStopWithoutRouteEvidence(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  CHECK(State.HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);
  CHECK(State.IdLowRoleToken==Cold.IdLowRoleToken && State.DataRoleToken==0U);
  CHECK(Bus.Registers[REG_DEVCTL]==0x1DU);

  /* Cleanup retains controller ownership even if the external ID route/token
   * has disappeared; the stop callback proves only clock, PHY and owner. */
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC;
  Cold.IdLowRoleToken=0U;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_UNKNOWN;
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  CHECK(State.Signature==0U && Bus.Registers[REG_DEVCTL]==0x1CU &&
        Bus.Registers[REG_POWER]==0U);
  return 1;
}

static int
TestColdHostTypecOnlyPath(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  CHECK(State.HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY &&
        State.DataRoleToken==Cold.DataRoleToken);
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
  Cold.BadTypecOnlyGpio=1;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.Signature==0U && State.GuardReason==MtkMusbPlatformColdGuardSource &&
        Bus.ColdTraceCount==0U);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
  Cold.BadTypecOnlySource=1;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.Signature==0U && State.GuardReason==MtkMusbPlatformColdGuardSource &&
        Bus.ColdTraceCount==0U);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
  Cold.DataRoleToken=0U;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.Signature==0U && State.GuardReason==MtkMusbPlatformColdGuardSource &&
        Bus.ColdTraceCount==0U);
  return 1;
}

static int
TestColdHostAcpiEvidenceContracts(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  Cold.AcpiColdFlags = MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID |
      MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
      MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE;
  Cold.AcpiClockIdentity = 0x12345678U;
  Cold.PhyLength = MTK_MUSB_PLATFORM_PHY_REG_BYTES;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(State.ClockIdentity == 0x12345678U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) ==
        MtkMusbPlatformSuccess);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  Cold.AcpiColdFlags = MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID |
      MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
      MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE;
  Cold.AcpiClockIdentity = 0x12345678U;
  Cold.PhyLength = MTK_MUSB_PLATFORM_PHY_REG_BYTES - 1U;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Signature == 0U && State.GuardReason == MtkMusbPlatformColdGuardClockPhy &&
        Cold.PhyCalls == 0U && Bus.ColdTraceCount == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  Cold.AcpiColdFlags = MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID |
      MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE;
  Cold.AcpiClockIdentity = 0x12345678U;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Kind == MtkMusbPlatformColdStateUninitialized &&
        Cold.PhyCalls == 0U && Bus.Registers[REG_DEVCTL] == 0x1CU);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  Cold.AcpiColdFlags = MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID |
      MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
      MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE;
  Cold.AcpiClockIdentity = 0x12345678U;
  Cold.ChangeClockIdentityOnGuardCall = 2U;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformConflict);
  CHECK(State.Kind == MtkMusbPlatformColdStateUninitialized &&
        Cold.PhyCalls == 0U && Bus.Registers[REG_DEVCTL] == 0x1CU);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Cold.DataRole = MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
  Cold.AcpiColdFlags = MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID |
      MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
      MTK_MUSB_PLATFORM_ACPI_ID_LOW_ROUTE;
  Cold.AcpiClockIdentity = 0x12345678U;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(State.ClockIdentity == 0x12345678U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) ==
        MtkMusbPlatformSuccess);
  return 1;
}

static int
TestColdHostIdLowRejectsDriftAndBadRole(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.Signature==0U && Bus.ColdTraceCount==0U);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
  Cold.IdLowRoleToken=0U;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
  CHECK(State.Signature==0U && Bus.ColdTraceCount==0U);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks(); State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
  Cold.ChangePowerPathOnGuardCall=3U;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformConflict);
  CHECK(State.Kind==MtkMusbPlatformColdStateNeedsStop &&
        State.HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC;
  Cold.IdLowRoleToken=0U;
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);

  ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks(); State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
  Cold.DataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW;
  Cold.ChangeIdLowTokenOnGuardCall=3U;
  SetRegister8(REG_DEVCTL,0x1CU);
  CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformConflict);
  CHECK(State.Kind==MtkMusbPlatformColdStateNeedsStop &&
        State.IdLowRoleToken==0x33U);
  Cold.HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC;
  Cold.IdLowRoleToken=0U;
  CHECK(MtkMusbPlatformStopColdHost(&Io,&Callbacks,&State)==MtkMusbPlatformSuccess);
  return 1;
}

static int
TestColdHostRejectsUnqualifiedAndBusyStarts(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  Cold.DataRole = MTK_MUSB_PLATFORM_USB_DATA_ROLE_UFP;
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Signature == 0U && Bus.ColdTraceCount == 0U);

  Cold.PhyPowerCleanupPending = MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Signature == 0U && Bus.ColdTraceCount == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  Cold.DataRoleToken = 0U;
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Signature == 0U && Bus.ColdTraceCount == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  Cold.FailPhase = MtkMusbPlatformColdBeforeSession;
  Cold.FailGuardOnce = 1;
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Signature == 0U && Bus.ColdTraceCount == 0U);

  for (unsigned I=0U; I<4U; ++I) {
    static const uint8_t UnsafePower[]={POWER_RESET,POWER_RESUME,POWER_SUSPENDM,POWER_SOFTCONN};
    ResetBus(); ResetColdMock(); Io=MakeIo(); Callbacks=MakeColdCallbacks();
    State=(MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
    SetRegister8(REG_DEVCTL,0x1CU); SetRegister8(REG_POWER,UnsafePower[I]);
    CHECK(DriveColdStart(&Io,&Callbacks,&State)==MtkMusbPlatformNotReady);
    CHECK(State.Signature==0U && Bus.ColdTraceCount==0U && Cold.PhyCalls==0U);
  }

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  SetRegister16(REG_HSDMA_CTRL0 + 0x20U, 1U);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformQuiescenceFailed);
  CHECK(State.Signature == 0U && Bus.ColdTraceCount == 0U);
  return 1;
}

static int
TestColdHostTimeoutAndCleanupRetry(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  Cold.ReadyAfterPolls = 0xFFFFFFFFU;
  SetRegister8(REG_DEVCTL, 0U);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformTimeout);
  CHECK(Cold.DelayCalls == MTK_MUSB_PLATFORM_COLD_TIMEOUT_MS);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK((State.OwnedMask & MTK_MUSB_PLATFORM_COLD_OWNS_SESSION) != 0U);
  CHECK((Bus.Registers[REG_DEVCTL] & DEVCTL_SESSION) != 0U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(State.Signature == 0U && Bus.Registers[REG_DEVCTL] == 0U);
  CHECK(!Cold.CallbackWhileLocked && !Bus.Locked);
  return 1;
}

static int
TestColdHostFaultedPhyCleanupRetry(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  Cold.PhyState = MTK_MUSB_PLATFORM_PHY_STATE_FAULTED;
  Cold.PhyMode = MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN;
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Signature == 0U && Bus.ColdTraceCount == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.FailPhyFaultOnce = 1;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK(State.PhyHostAttempted == 1U && State.PhyOtgAttempted == 0U);
  CHECK(Cold.PhyState == MTK_MUSB_PLATFORM_PHY_STATE_FAULTED &&
        Cold.PhyPowerCleanupPending == MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1DU && Bus.Registers[REG_POWER] == 0U);

  /* A failed controller restore keeps the fault classification for retry. */
  Bus.FailWriteOffset = REG_DEVCTL; Bus.FailWriteOnce = 1;
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop && State.PhyFaultedCleanup == 1U);
  CHECK(Cold.PhyCalls == 1U && Bus.Registers[REG_DEVCTL] == 0x1DU);

  /* Do not accept a silently changed PHY state while controller ownership remains. */
  Cold.PhyState = MTK_MUSB_PLATFORM_PHY_POWERED;
  Cold.PhyPowerCleanupPending = 0U;
  Cold.PhyMode = MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG;
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformConflict);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop && Bus.Registers[REG_DEVCTL] == 0x1DU);

  Cold.PhyState = MTK_MUSB_PLATFORM_PHY_STATE_FAULTED;
  Cold.PhyPowerCleanupPending = MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING;
  Cold.PhyMode = MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN;
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(State.Signature == 0U && State.OwnedMask == 0U);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && Bus.Registers[REG_POWER] == 0U);
  CHECK(Cold.PhyCalls == 1U && Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN);
  CHECK(!Cold.CallbackWhileLocked && !Bus.Locked && Bus.Index == 7U);
  return 1;
}

static int
TestColdHostPartialWritesAndGuardFailureCleanup(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  SetRegister8(REG_DEVCTL, 0x1CU);
  Bus.FailWriteAfterOffset = REG_DEVCTL;
  Bus.FailWriteAfterOnce = 1;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK((Bus.Registers[REG_DEVCTL] & DEVCTL_SESSION) == 0U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && State.Signature == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Bus.ArmFailReadAfterWriteOffset = REG_DEVCTL;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK((Bus.Registers[REG_DEVCTL] & DEVCTL_SESSION) == 0U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && State.Signature == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Bus.FailWriteAfterOffset = REG_POWER;
  Bus.FailWriteAfterOnce = 1;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK(Bus.Registers[REG_POWER] == (POWER_ENSUSPEND | POWER_HSENAB));
  CHECK(Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && Bus.Registers[REG_POWER] == 0U);
  CHECK(Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  Cold.FailPhase = MtkMusbPlatformColdAfterSession;
  Cold.FailGuardOnce = 1;
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformNotReady);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && State.Signature == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.ChangeRoleTokenOnGuardCall = 4U;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformConflict);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK((Bus.Registers[REG_DEVCTL] & DEVCTL_SESSION) != 0U);
  CHECK(Cold.PhyCalls==0U && Bus.Registers[REG_POWER]==0U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && State.Signature == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0U);
  Cold.FailDelay = 1;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK((Bus.Registers[REG_DEVCTL] & DEVCTL_SESSION) != 0U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == 0U && State.Signature == 0U);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  Cold.FailPhyOnce = 1;
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK(Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && Bus.Registers[REG_POWER] == 0U);
  CHECK(Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG);
  return 1;
}

static int
TestColdHostDriftAndStopRetry(void)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State = {0};

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks();
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);

  Bus.Registers[REG_POWER] |= POWER_SUSPENDM;
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformQuiescenceFailed);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK((Bus.Registers[REG_POWER] & (POWER_ENSUSPEND | POWER_HSENAB)) ==
        (POWER_ENSUSPEND | POWER_HSENAB));
  Bus.Registers[REG_POWER] &= (uint8_t)~POWER_SUSPENDM;
  Cold.FailPhyOnce = 1;
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformIoError);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  CHECK(Bus.Registers[REG_DEVCTL] == 0x1CU && Bus.Registers[REG_POWER] == 0U);
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  CHECK(State.Signature == 0U && Cold.PhyMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG);

  ResetBus(); ResetColdMock(); Io = MakeIo(); Callbacks = MakeColdCallbacks(); State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
  SetRegister8(REG_DEVCTL, 0x1CU);
  CHECK(DriveColdStart(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  Cold.OwnerToken++;
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformConflict);
  CHECK(State.Kind == MtkMusbPlatformColdStateNeedsStop);
  Cold.OwnerToken--;
  CHECK(MtkMusbPlatformStopColdHost(&Io, &Callbacks, &State) == MtkMusbPlatformSuccess);
  return 1;
}

int
main(void)
{
  if (!TestFifoProfile() || !TestQuiescenceGates() ||
      !TestFailureRestoresIndex() || !TestReadyEvidence() || !TestIdLowReadyEvidence() ||
      !TestHusb311SourceAttach() ||
      !TestInvalidAndBusyInputs() || !TestIdleDiagnostic() ||
      !TestIdleDeviceInterruptRetirement() || !TestIdleDeviceDmaRequestRetirement() ||
      !TestPendingDeviceStatusDisconnect() ||
      !TestIdleInterruptAcknowledge() || !TestRetainedHostReset() ||
      !TestInheritedFifoPreparation() || !TestRetainedFifoRestoration() ||
      !TestColdHostPowerBeforeRoleReadback() ||
      !TestColdHostPhysicalPowerResponses() ||
      !TestColdStartFailureDiagnostics() ||
      !TestStockColdResetAndPartialStart() ||
      !TestInactiveDmaConfigurationAfterClear() ||
      !TestPostResetInterruptState() ||
      !TestSessionLostBeforeControllerReset() ||
      !TestColdHostStartStopAndWriteTrace() ||
      !TestColdHostDelayedReadinessAndPreservation() ||
      !TestColdHostInheritedDeviceRole() ||
      !TestColdHostIdLowPathAndStopWithoutRouteEvidence() ||
      !TestColdHostTypecOnlyPath() ||
      !TestColdHostAcpiEvidenceContracts() ||
      !TestColdHostIdLowRejectsDriftAndBadRole() ||
      !TestColdHostRejectsUnqualifiedAndBusyStarts() ||
      !TestColdHostTimeoutAndCleanupRetry() ||
      !TestColdHostFaultedPhyCleanupRetry() ||
      !TestColdHostPartialWritesAndGuardFailureCleanup() ||
      !TestColdHostDriftAndStopRetry()) return 1;
  printf("MUSB platform core: %u checks passed (stock FIFO profile, idle gates, raw readiness evidence, inherited PHY, no device access)\n", Checks);
  return 0;
}
