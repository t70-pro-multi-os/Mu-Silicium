/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Bounded portable implementation of the T70Pro along_cyx DSI lifecycle.
 * The supported profile and register operations are derived from original LK
 * ARM64 SHA-256 05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4,
 * operation 7 at image offset 0x4ae90, recorded in
 * analysis/display-lk-handoff.json. This file derives the configured port
 * shadow from the actual get_params bytes and six preconditioned DSI_config
 * register inputs. It does not establish panel power, PHY lock, or link
 * readiness.
 */
#include <Library/MtkDsiCore.h>

#define DSI_CORE_SIGNATURE 0x44534931U

#define DSI_PORT_OFFSET          0x13000U
#define DSI_GATE_OFFSET_108      0x00108U
#define DSI_GATE_OFFSET_1A8      0x001A8U

#define DSI_MIPI_328             0x00328U
#define DSI_MIPI_LANE0           0x00040U
#define DSI_MIPI_LANE1           0x00044U
#define DSI_MIPI_LANE2           0x00048U
#define DSI_MIPI_LANE3           0x0004CU
#define DSI_MIPI_014             0x00014U
#define DSI_MIPI_03C             0x0003CU
#define DSI_MIPI_00C             0x0000CU
#define DSI_MIPI_244             0x00244U
#define DSI_MIPI_444             0x00444U
#define DSI_MIPI_144             0x00144U
#define DSI_MIPI_544             0x00544U
#define DSI_MIPI_344             0x00344U
#define DSI_MIPI_028             0x00028U
#define DSI_MIPI_02C             0x0002CU
#define DSI_MIPI_030             0x00030U
#define DSI_MIPI_034             0x00034U

static MTK_DSI_U32
ReadLe32 (
  const MTK_DSI_U8 *Bytes,
  MTK_DSI_U32 Offset
  )
{
  return (MTK_DSI_U32)Bytes[Offset] |
         ((MTK_DSI_U32)Bytes[Offset + 1U] << 8) |
         ((MTK_DSI_U32)Bytes[Offset + 2U] << 16) |
         ((MTK_DSI_U32)Bytes[Offset + 3U] << 24);
}

static MTK_DSI_U32
MaxU32 (
  MTK_DSI_U32 Left,
  MTK_DSI_U32 Right
  )
{
  return (Left > Right) ? Left : Right;
}

static MTK_DSI_STATUS
CoreFailure (
  MTK_DSI_CORE *Core,
  MTK_DSI_STATUS Status
  )
{
  Core->LastStatus = Status;
  Core->State = (Core->SideEffectsPossible != 0U)
                  ? MtkDsiStateQuarantined
                  : MtkDsiStateIdle;
  Core->Busy = 0U;
  return Status;
}

static MTK_DSI_STATUS
ReadRegister (
  MTK_DSI_CORE *Core,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 *Value
  )
{
  MTK_DSI_STATUS Status;

  Core->CurrentBank = Bank;
  Core->CurrentOffset = Offset;
  Core->ReadCalls++;
  Status = Core->Io.Read32 (Core->Io.Context, Bank, Offset, Value);
  if (Status != MtkDsiSuccess) {
    return CoreFailure (Core, Status);
  }

  return MtkDsiSuccess;
}

static MTK_DSI_STATUS
WriteRegister (
  MTK_DSI_CORE *Core,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 Value
  )
{
  MTK_DSI_STATUS Status;

  Core->CurrentBank = Bank;
  Core->CurrentOffset = Offset;
  Core->CurrentValue = Value;
  Core->SideEffectsPossible = 1U;
  Core->WriteAttempts++;
  Status = Core->Io.Write32 (Core->Io.Context, Bank, Offset, Value);
  if (Status != MtkDsiSuccess) {
    return CoreFailure (Core, Status);
  }

  Core->WritesAccepted++;
  Core->NextWriteIndex++;
  return MtkDsiSuccess;
}

static MTK_DSI_STATUS
WriteDirect (
  MTK_DSI_CORE *Core,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 Value
  )
{
  return WriteRegister (Core, Bank, Offset, Value);
}

static MTK_DSI_STATUS
ModifyRegister (
  MTK_DSI_CORE *Core,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 ClearMask,
  MTK_DSI_U32 SetMask
  )
{
  MTK_DSI_STATUS Status;
  MTK_DSI_U32 Value;

  Value = 0U;
  Status = ReadRegister (Core, Bank, Offset, &Value);
  if (Status != MtkDsiSuccess) {
    return Status;
  }

  Value = (Value & ~ClearMask) | SetMask;
  return WriteRegister (Core, Bank, Offset, Value);
}

static MTK_DSI_STATUS
DelayMicroseconds (
  MTK_DSI_CORE *Core
  )
{
  MTK_DSI_STATUS Status;

  Core->DelayAttempts++;
  Status = Core->Io.DelayMicroseconds (
             Core->Io.Context,
             MTK_DSI_LK_DELAY_MICROSECONDS
             );
  if (Status != MtkDsiSuccess) {
    return CoreFailure (Core, Status);
  }

  return MtkDsiSuccess;
}

static MTK_DSI_U32
ComputeTimingWord (
  MTK_DSI_U32 Rate,
  MTK_DSI_U32 *TimingField
  )
{
  MTK_DSI_U32 Shift;
  MTK_DSI_U32 ScaledRate;
  MTK_DSI_U32 Divisor;
  MTK_DSI_U32 Quotient;
  MTK_DSI_U32 Remainder;
  MTK_DSI_U32 Packed;
  MTK_DSI_U32 Byte;
  int Bit;

  /* These threshold choices mirror the original CSEL/compare sequence. The
   * profile validator currently admits only Rate=1466, but keeping the
   * arithmetic explicit makes the source-derived result reviewable. */
  if (Rate > 0x3E7U) {
    Shift = (Rate < 0x7D0U) ? 1U : 2U;
    *TimingField = (Rate < 0x7D0U) ? 0x100U : 0x200U;
  } else {
    Shift = 2U;
    *TimingField = 0x200U;
  }

  if (Rate <= 0x1F3U) {
    Shift = 3U;
    *TimingField = 0x300U;
  }

  if (Rate <= 0xFAU) {
    Shift = 4U;
    *TimingField = 0x400U;
  }

  /* The raw get_params rate fields are 733 and zero, so the original DSI
   * helper derives a rate of 1466 before applying its 0x1a divisor. */
  ScaledRate = Rate << Shift;
  Divisor = 0x1AU;
  Quotient = ScaledRate / Divisor;
  Remainder = ScaledRate % Divisor;
  Packed = (Quotient & 0xFFU) << 24;

  for (Bit = 16; Bit >= 0; Bit -= 8) {
    Remainder <<= 8;
    Byte = Remainder / Divisor;
    Remainder %= Divisor;
    Packed |= (Byte & 0xFFU) << (unsigned)Bit;
  }

  return Packed;
}

static MTK_DSI_STATUS
BuildPortTiming (
  const MTK_DSI_U8 *Parameters,
  MTK_DSI_PORT_SHADOW *Shadow
  )
{
  MTK_DSI_U32 Rate;
  MTK_DSI_U32 Divider;
  MTK_DSI_U32 RateStep;
  MTK_DSI_U32 V13;
  MTK_DSI_U32 V14;
  MTK_DSI_U32 V15;
  MTK_DSI_U32 V16;
  MTK_DSI_U32 V17;
  MTK_DSI_U32 V3;
  MTK_DSI_U32 V4;
  MTK_DSI_U32 V5;
  MTK_DSI_U32 V8;
  MTK_DSI_U32 V9;
  MTK_DSI_U32 HsPrepare;
  MTK_DSI_U32 V10;
  MTK_DSI_U32 V11;
  MTK_DSI_U32 V12;
  MTK_DSI_U32 V1;
  MTK_DSI_U32 V2;
  MTK_DSI_U32 Div75;
  MTK_DSI_U32 Div64;
  MTK_DSI_U32 Div80;
  MTK_DSI_U32 Div125;
  MTK_DSI_U32 Div350;
  MTK_DSI_U32 FiveStep;
  MTK_DSI_U32 FourStep;
  MTK_DSI_U32 ThreeStep;
  MTK_DSI_U32 Packed114;
  MTK_DSI_U32 Packed118;

  Rate = ReadLe32 (Parameters, 0xC4U);
  if (Rate == 0U) {
    Rate = ReadLe32 (Parameters, 0xC0U);
    if ((Rate == 0U) || (Rate > 0x7FFFFFFFU)) {
      return MtkDsiInvalid;
    }
    Rate <<= 1;
  }

  if ((Rate == 0U) || ((Rate & 0xFFFFU) == 0U)) {
    return MtkDsiInvalid;
  }

  Divider = 8000U / Rate;
  if (Divider == 0U) {
    return MtkDsiInvalid;
  }

  RateStep = (Rate > 0x3E8U)
               ? 1U
               : 1000U / (Rate & 0xFFFFU);
  if (RateStep == 0U) {
    return MtkDsiInvalid;
  }

  /* Transcription of the arithmetic and byte overrides in LK 0x483e4.
   * The source receives the get_params fragment beginning at +0x170. */
  V5 = RateStep * 52U + 90U;
  Div75 = 75U / Divider;
  FiveStep = RateStep + (RateStep << 2);
  V11 = (FiveStep + 64U) / Divider;
  V17 = (FiveStep + 80U) / Divider;
  V15 = V11 + 1U;
  /* LK 0x484e8 selects (initial V11 + 1) when byte +0x8e is zero. */
  HsPrepare = (Parameters[0x8EU] != 0U) ? Parameters[0x8EU] : V15;
  V14 = (RateStep * 10U + 200U) / Divider;
  V4 = (V15 >= V14) ? 0U : V15;
  V14 -= V4;
  V16 = (RateStep + (RateStep << 3)) / Divider;
  Div64 = 64U / Divider;
  Div350 = 350U / Divider;
  V1 = Div350 - ((Div350 > Div64) ? Div64 : 0U);
  V3 = Div75 + 1U;
  FourStep = V3 << 2;
  FiveStep = FourStep + V3;
  ThreeStep = (V3 + (V3 << 1)) >> 1;
  Div125 = 125U / Divider;
  Div80 = 80U / Divider;

  V13 = (Parameters[0x8FU] != 0U)
          ? Parameters[0x8FU]
          : (Div75 + 1U);
  V13 += (V13 & 1U);
  V14 = (Parameters[0x8DU] == 0U)
          ? V14
          : Parameters[0x8DU];
  V11 = (Parameters[0x8CU] == 0U)
          ? MaxU32 (V16, V17)
          : Parameters[0x8CU];
  V12 = (Parameters[0x91U] == 0U)
          ? FiveStep
          : Parameters[0x91U];
  V17 = (V5 & 0xFFFEU) / Divider;
  V3 = (Parameters[0x92U] == 0U)
         ? ThreeStep
         : Parameters[0x92U];
  V16 = (Parameters[0x93U] == 0U)
          ? FourStep
          : Parameters[0x93U];
  V8 = (Parameters[0x9AU] != 0U)
         ? Parameters[0x9AU]
         : (Div125 + 1U);
  V15 = (Parameters[0x95U] == 0U)
          ? V1
          : Parameters[0x95U];
  V4 = (Parameters[0x94U] == 0U)
         ? Div80
         : Parameters[0x94U];
  V10 = (Parameters[0x98U] == 0U)
          ? Div64
          : Parameters[0x98U];
  V1 = (Parameters[0x9BU] == 0U)
         ? (Div125 + 1U)
         : Parameters[0x9BU];
  V17 = (Parameters[0x99U] == 0U)
          ? V17
          : Parameters[0x99U];
  V9 = HsPrepare + (HsPrepare & 1U);
  V2 = (V9 > 6U) ? V9 : 6U;

  Shadow->Reg110 = (V13 & 0xFFU) |
                   ((V2 & 0xFFU) << 8) |
                   ((V14 & 0xFFU) << 16) |
                   ((V11 & 0xFFU) << 24);
  Packed114 = (V16 & 0xFFU) |
              ((V3 & 0xFFU) << 8) |
              ((V12 & 0xFFU) << 16) |
              (((V8 | 1U) & 0xFFU) << 24);
  Shadow->Reg114 = Packed114;
  Packed118 = 0x103U |
              ((V15 & 0xFFU) << 16) |
              ((V4 & 0xFFU) << 24);
  Shadow->Reg118 = Packed118;
  Shadow->Reg11C = (V10 & 0xFFU) |
                   ((V17 & 0xFFU) << 8) |
                   ((V1 & 0xFFU) << 16);
  return MtkDsiSuccess;
}

MTK_DSI_STATUS
MtkDsiCoreBuildAlongCyxConfigShadow (
  const MTK_DSI_OP7_PROFILE *Profile,
  const MTK_DSI_PORT_INITIAL *Initial,
  MTK_DSI_PORT_SHADOW *Output
  )
{
  static const MTK_DSI_U32 TimingSelectTable[2] = { 0x10000U, 0x30000U };
  static const MTK_DSI_U32 TimingModeTable[4] = { 4U, 0xCU, 0x1CU, 0x3CU };
  const MTK_DSI_U8 *Raw;
  const MTK_DSI_U8 *Parameters;
  MTK_DSI_PORT_SHADOW Shadow;
  MTK_DSI_U32 Selector;
  MTK_DSI_U32 Rows;
  MTK_DSI_U32 Reg50Source;
  MTK_DSI_U32 Reg54Source;
  MTK_DSI_U32 Reg58Source;
  MTK_DSI_U32 Reg5CSource;
  MTK_DSI_STATUS Status;
  unsigned Index;

  if ((Profile == 0) || (Initial == 0) || (Output == 0) ||
      (Profile->GetParams == 0) ||
      (Profile->GetParamsSize != MTK_DSI_GET_PARAMS_SIZE) ||
      (Profile->ModuleId != MTK_DSI_LK_MODULE_ID) ||
      (Profile->Mode != 0U) ||
      (Profile->ConfigWidth != MTK_DSI_LK_WIDTH) ||
      (Profile->ConfigHeight != MTK_DSI_LK_HEIGHT)) {
    return MtkDsiInvalid;
  }

  Raw = Profile->GetParams;
  Parameters = Raw + MTK_DSI_GET_PARAMS_FRAGMENT_OFFSET;
  if ((ReadLe32 (Raw, 0x00U) != MTK_DSI_LK_PANEL_TYPE) ||
      (ReadLe32 (Raw, 0x18U) != Profile->ConfigWidth) ||
      (ReadLe32 (Raw, 0x1CU) != Profile->ConfigHeight) ||
      (ReadLe32 (Raw, 0x170U) != 1U) ||
      (ReadLe32 (Raw, 0x184U) != 4U) ||
      (ReadLe32 (Raw, 0x194U) != 2U) ||
      (ReadLe32 (Raw, 0x1A0U) != 2U) ||
      (ReadLe32 (Raw, 0x1A4U) != 0x1A40U) ||
      (ReadLe32 (Raw, 0x1A8U) != 0x100U) ||
      (ReadLe32 (Raw, 0x1B0U) != 8U) ||
      (ReadLe32 (Raw, 0x1B4U) != 32U) ||
      (ReadLe32 (Raw, 0x1B8U) != 230U) ||
      (ReadLe32 (Raw, 0x1C0U) != MTK_DSI_LK_HEIGHT) ||
      (ReadLe32 (Raw, 0x1C4U) != 8U) ||
      (ReadLe32 (Raw, 0x1C8U) != 32U) ||
      (ReadLe32 (Raw, 0x1CCU) != 40U) ||
      (ReadLe32 (Raw, 0x1D8U) != 0U) ||
      (ReadLe32 (Raw, 0x1D4U) != MTK_DSI_LK_WIDTH) ||
      (ReadLe32 (Raw, 0x230U) != MTK_DSI_LK_RATE_FIELD) ||
      (ReadLe32 (Raw, 0x234U) != 0U) ||
      (ReadLe32 (Raw, 0x238U) != 0U) ||
      (ReadLe32 (Raw, 0x240U) != 1U) ||
      (ReadLe32 (Raw, 0x25CU) != 1U) ||
      (ReadLe32 (Raw, 0x5E8U) != 1U) ||
      (ReadLe32 (Raw, 0x624U) != 0U) ||
      (ReadLe32 (Raw, 0x6D0U) != 0U) ||
      (ReadLe32 (Raw, 0x6D4U) != 0U) ||
      (ReadLe32 (Raw, 0x6D8U) != 0U)) {
    return MtkDsiInvalid;
  }

  for (Index = 0U; Index < 6U; ++Index) {
    if (ReadLe32 (Raw, 0x628U + (Index * 4U)) != 0U) {
      return MtkDsiInvalid;
    }
  }

  Shadow.Reg08 = Initial->Reg08;
  Shadow.Reg14 = Initial->Reg14;
  Shadow.Reg18 = (Initial->Reg18 & ~0x0001F0C3U) | 0x3CU;
  Shadow.Reg64 = Initial->Reg64;
  Shadow.Reg200 = Initial->Reg200 | 0x21U;

  Selector = ReadLe32 (Parameters, 0x30U);
  if ((Selector == 0U) || (Selector > 2U)) {
    return MtkDsiInvalid;
  }
  Shadow.Reg1C = (Initial->Reg1C & ~0x000F7FFFU) |
                 TimingSelectTable[Selector - 1U] |
                 ((Profile->ConfigWidth * 3U) & 0x7FFFU);

  Selector = ReadLe32 (Parameters, 0x14U);
  if ((Selector == 0U) || (Selector > 4U)) {
    return MtkDsiInvalid;
  }
  Shadow.Reg90 = TimingModeTable[Selector - 1U];

  Rows = 2U + ((ReadLe32 (Parameters, 0x24U) != 0U) ? 1U : 0U);
  Shadow.Reg20 = ReadLe32 (Parameters, 0x40U);
  Shadow.Reg24 = ReadLe32 (Parameters, 0x44U);
  Shadow.Reg28 = ReadLe32 (Parameters, 0x48U);
  Shadow.Reg2C = ReadLe32 (Parameters, 0x50U);
  Reg50Source = ReadLe32 (Parameters, 0x54U) * Rows;
  Reg54Source = ReadLe32 (Parameters, 0x58U) * Rows;
  Reg58Source = ReadLe32 (Parameters, 0x5CU) * Rows;
  Reg5CSource = ReadLe32 (Parameters, 0x68U) * Rows;
  if ((Reg50Source <= 9U) || (Reg54Source <= 9U) ||
      (Reg58Source <= 11U)) {
    return MtkDsiInvalid;
  }
  Shadow.Reg50 = (Reg50Source - 10U + 3U) & ~3U;
  Shadow.Reg54 = (Reg54Source - 7U) & ~3U;
  Shadow.Reg58 = (Reg58Source - 9U) & ~3U;
  Shadow.Reg5C = (Reg5CSource + 3U) & ~3U;

  Status = BuildPortTiming (Parameters, &Shadow);
  if (Status != MtkDsiSuccess) {
    return Status;
  }

  *Output = Shadow;
  return MtkDsiSuccess;
}

static MTK_DSI_STATUS
CopyPortShadow (
  MTK_DSI_CORE *Core,
  const MTK_DSI_PORT_SHADOW *Port
  )
{
  MTK_DSI_STATUS Status;
  MTK_DSI_U32 Base;

  Base = DSI_PORT_OFFSET;
#define PORT_WRITE(Reg, Value) do { \
  Status = WriteDirect (Core, MtkDsiBankDsi0, Base + (Reg), (Value)); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)

  PORT_WRITE (0x008U, Port->Reg08);
  PORT_WRITE (0x014U, Port->Reg14);
  PORT_WRITE (0x018U, Port->Reg18 & 0xFFFFFFC3U);
  PORT_WRITE (0x01CU, Port->Reg1C);
  PORT_WRITE (0x020U, Port->Reg20);
  PORT_WRITE (0x024U, Port->Reg24);
  PORT_WRITE (0x028U, Port->Reg28);
  PORT_WRITE (0x02CU, Port->Reg2C);
  PORT_WRITE (0x050U, Port->Reg50);
  PORT_WRITE (0x054U, Port->Reg54);
  PORT_WRITE (0x058U, Port->Reg58);
  PORT_WRITE (0x05CU, Port->Reg5C);
  PORT_WRITE (0x064U, Port->Reg64);
  PORT_WRITE (0x090U, Port->Reg90);
  PORT_WRITE (0x110U, Port->Reg110);
  PORT_WRITE (0x114U, Port->Reg114);
  PORT_WRITE (0x118U, Port->Reg118);
  PORT_WRITE (0x11CU, Port->Reg11C);
  PORT_WRITE (0x200U, Port->Reg200);

#undef PORT_WRITE
  return MtkDsiSuccess;
}

MTK_DSI_STATUS
MtkDsiCoreInitialize (
  MTK_DSI_CORE *Core,
  const MTK_DSI_CORE_IO *Io
  )
{
  if ((Core == 0) || (Io == 0) ||
      (Io->Read32 == 0) || (Io->Write32 == 0) ||
      (Io->DelayMicroseconds == 0)) {
    return MtkDsiInvalid;
  }

  /* This object is a one-shot journal. Reinitializing a completed or
   * quarantined operation could replay a reset pulse without external proof. */
  if (Core->Signature != 0U) {
    return (Core->Signature == DSI_CORE_SIGNATURE && Core->Busy != 0U)
             ? MtkDsiBusy
             : MtkDsiWrongState;
  }

  Core->Io = *Io;
  Core->Signature = DSI_CORE_SIGNATURE;
  Core->State = MtkDsiStateIdle;
  Core->LastStatus = MtkDsiSuccess;
  Core->CurrentBank = MtkDsiBankDsi0;
  Core->CurrentOffset = 0U;
  Core->CurrentValue = 0U;
  Core->ReadCalls = 0U;
  Core->WriteAttempts = 0U;
  Core->WritesAccepted = 0U;
  Core->DelayAttempts = 0U;
  Core->NextWriteIndex = 0U;
  Core->Busy = 0U;
  Core->SideEffectsPossible = 0U;
  Core->Reserved[0] = 0U;
  Core->Reserved[1] = 0U;
  Core->SavedPort = (MTK_DSI_PORT_SHADOW){ 0 };
  return MtkDsiSuccess;
}

static MTK_DSI_STATUS
RunOp7Registers (
  MTK_DSI_CORE *Core,
  const MTK_DSI_PORT_SHADOW *PortShadow,
  MTK_DSI_U8 Resume
  )
{
  MTK_DSI_STATUS Status;
  MTK_DSI_U32 TimingField;
  MTK_DSI_U32 TimingWord;

  TimingWord = ComputeTimingWord (MTK_DSI_LK_RATE_FIELD << 1, &TimingField);

#define APPLY_RMW(BankValue, OffsetValue, ClearValue, SetValue) do { \
  Status = ModifyRegister (Core, (BankValue), (OffsetValue), \
                           (ClearValue), (SetValue)); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)
#define APPLY_WRITE(BankValue, OffsetValue, ValueValue) do { \
  Status = WriteDirect (Core, (BankValue), (OffsetValue), (ValueValue)); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)
#define APPLY_DELAY() do { \
  Status = DelayMicroseconds (Core); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)

  /* Stock module-0x10 switch writes followed by the supported MIPI setup. */
  if (Resume == 0U) {
    APPLY_RMW (MtkDsiBankDsi0, DSI_GATE_OFFSET_108, 0U, 0x00080000U);
    APPLY_RMW (MtkDsiBankDsi0, DSI_GATE_OFFSET_1A8, 0U, 1U);
  }
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_328, 0U, 1U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_LANE0, 0x65432100U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_LANE1, 0x24232987U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_LANE2, 0x76543210U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_LANE3, 8U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_014, 0U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_03C, 0x00FF12E0U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_00C, 0x3FFF0180U);
  APPLY_DELAY ();
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_00C, 0x3FFF0080U);
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_244, 1U, 0U);
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_444, 1U, 0U);
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_144, 1U, 0U);
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_544, 1U, 0U);
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_344, 1U, 0U);
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_028, 0U, 1U);
  APPLY_DELAY ();
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_028, 2U, 0U);
  APPLY_WRITE (MtkDsiBankMipiTx, DSI_MIPI_02C, TimingWord);
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_030, 0x700U, TimingField);
  APPLY_DELAY ();
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_030, 0U, 0x10U);
  APPLY_DELAY ();
  APPLY_RMW (MtkDsiBankMipiTx, DSI_MIPI_034, 2U, 0U);

  Status = CopyPortShadow (Core, PortShadow);
  if (Status != MtkDsiSuccess) {
    return Status;
  }

  if (Resume != 0U) {
    /* LK 0x4b0b4..0x4b11c: only subsequent operation 7 sets this
     * word, before the same reset pulse clears bit20 again. */
    APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x014U, 0U, 0x00100000U);
    APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x0A0U, 0xFFFFU,
               (((MTK_DSI_LK_RATE_FIELD * 2U * 1000U) >> 13) + 1U) & 0xFFFFU);
  }

  /* Original first-use DSI reset pulse and bit cleanup. A callback error at
   * any point above or inside this pulse leaves the core quarantined. */
  APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET, 4U, 0U);
  APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET, 0U, 4U);
  APPLY_DELAY ();
  APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET, 4U, 0U);
  APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x014U, 0x00100000U, 0U);
  APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x104U, 1U, 0U);
  APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x010U, 0U, 1U);
  APPLY_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x010U, 1U, 0U);

#undef APPLY_RMW
#undef APPLY_WRITE
#undef APPLY_DELAY

  Core->State = MtkDsiStateOp7Complete;
  Core->LastStatus = MtkDsiSuccess;
  Core->Busy = 0U;
  return MtkDsiSuccess;
}

MTK_DSI_STATUS
MtkDsiCoreRunAlongCyxOp7 (
  MTK_DSI_CORE *Core,
  const MTK_DSI_OP7_PROFILE *Profile
  )
{
  MTK_DSI_STATUS Status;
  MTK_DSI_OP7_PROFILE ProfileCopy;
  MTK_DSI_PORT_SHADOW PortShadow;

  if ((Core == 0) || (Profile == 0) ||
      (Core->Signature != DSI_CORE_SIGNATURE)) {
    return MtkDsiInvalid;
  }

  if (Core->Busy != 0U) {
    return MtkDsiBusy;
  }

  if (Core->State == MtkDsiStateQuarantined) {
    return MtkDsiQuarantined;
  }

  if (Core->State != MtkDsiStateIdle) {
    return MtkDsiWrongState;
  }

  Core->Busy = 1U;
  Core->State = MtkDsiStateRunningOp7;
  ProfileCopy = *Profile;
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &ProfileCopy,
             &ProfileCopy.Initial,
             &PortShadow
             );
  if (Status != MtkDsiSuccess) {
    return CoreFailure (Core, Status);
  }

  Status = RunOp7Registers (Core, &PortShadow, 0U);
  if (Status == MtkDsiSuccess) {
    Core->SavedPort = PortShadow;
  }
  return Status;
}

static MTK_DSI_STATUS
BeginTransition (
  MTK_DSI_CORE *Core,
  MTK_DSI_CORE_STATE Expected,
  MTK_DSI_CORE_STATE Running
  )
{
  if ((Core == 0) || (Core->Signature != DSI_CORE_SIGNATURE)) {
    return MtkDsiInvalid;
  }
  if (Core->Busy != 0U) {
    return MtkDsiBusy;
  }
  if (Core->State == MtkDsiStateQuarantined) {
    return MtkDsiQuarantined;
  }
  if (Core->State != Expected) {
    return MtkDsiWrongState;
  }
  Core->Busy = 1U;
  Core->State = Running;
  return MtkDsiSuccess;
}

MTK_DSI_STATUS
MtkDsiCoreStartVideo (
  MTK_DSI_CORE *Core
  )
{
  MTK_DSI_STATUS Status;

  Status = BeginTransition (Core, MtkDsiStateOp7Complete, MtkDsiStateStarting);
  if (Status != MtkDsiSuccess) {
    return Status;
  }

#define VIDEO_RMW(Reg, Clear, Set) do { \
  Status = ModifyRegister (Core, MtkDsiBankDsi0, DSI_PORT_OFFSET + (Reg), (Clear), (Set)); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)

  /* DSI_config's TXRX setup (0x474c0) must follow PHY operation 7:
   * that operation deliberately copies the shadow with data lanes disabled.
   * Reapply the validated four-lane along_cyx profile before starting video.
   * Fragment +0xec == 1, +0x560/+0x564 == 0 are validated from get_params. */
  VIDEO_RMW (0x018U, 3U, 0U);
  VIDEO_RMW (0x018U, 0x40U, 0U);
  VIDEO_RMW (0x018U, 0x80U, 0U);
  VIDEO_RMW (0x018U, 0xF000U, 0U);
  VIDEO_RMW (0x018U, 0x10000U, 0U);
  VIDEO_RMW (0x018U, 0x3CU, 0x3CU);
  Status = WriteDirect (Core, MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x090U, 0x3CU);
  if (Status != MtkDsiSuccess) {
    return Status;
  }
  VIDEO_RMW (0x018U, 0x200U, 0U);

  /* Registered operation 3, 0x4b78c, module 0x10, video mode 1. */
  VIDEO_RMW (0x190U, 0U, 2U);
  VIDEO_RMW (0x190U, 0U, 4U);
  VIDEO_RMW (0x014U, 3U, 1U);
  Status = WriteDirect (Core, MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x100U, 0xAAU);
  if (Status != MtkDsiSuccess) {
    return Status;
  }
  VIDEO_RMW (0x104U, 0U, 1U);

  /* Registered operation 4, 0x4b910: a start edge, not just setting bit0. */
  VIDEO_RMW (0x000U, 1U, 0U);
  VIDEO_RMW (0x000U, 0U, 1U);
#undef VIDEO_RMW

  Core->State = MtkDsiStateStreaming;
  Core->LastStatus = MtkDsiSuccess;
  Core->Busy = 0U;
  return MtkDsiSuccess;
}

static MTK_DSI_STATUS
SavePort (
  MTK_DSI_CORE *Core
  )
{
  static const MTK_DSI_U32 Offsets[19] = {
    0x008U, 0x014U, 0x018U, 0x01CU, 0x020U, 0x024U, 0x028U,
    0x02CU, 0x050U, 0x054U, 0x058U, 0x05CU, 0x064U, 0x090U,
    0x110U, 0x114U, 0x118U, 0x11CU, 0x200U
  };
  MTK_DSI_PORT_SHADOW Saved;
  MTK_DSI_U32 *Words[19] = {
    &Saved.Reg08, &Saved.Reg14, &Saved.Reg18, &Saved.Reg1C,
    &Saved.Reg20, &Saved.Reg24, &Saved.Reg28, &Saved.Reg2C,
    &Saved.Reg50, &Saved.Reg54, &Saved.Reg58, &Saved.Reg5C,
    &Saved.Reg64, &Saved.Reg90, &Saved.Reg110, &Saved.Reg114,
    &Saved.Reg118, &Saved.Reg11C, &Saved.Reg200
  };
  MTK_DSI_STATUS Status;
  unsigned Index;

  /* Original 0x468ec captures these before changing the PHY. Commit the
   * saved context only when all reads succeeded. */
  for (Index = 0U; Index < 19U; ++Index) {
    Status = ReadRegister (Core, MtkDsiBankDsi0,
                           DSI_PORT_OFFSET + Offsets[Index], Words[Index]);
    if (Status != MtkDsiSuccess) {
      return Status;
    }
  }
  Core->SavedPort = Saved;
  return MtkDsiSuccess;
}

MTK_DSI_STATUS
MtkDsiCoreSuspend (
  MTK_DSI_CORE *Core,
  MTK_DSI_U32 MaxPolls
  )
{
  MTK_DSI_STATUS Status;
  MTK_DSI_U32 Value;
  MTK_DSI_U32 Poll;

  if ((MaxPolls == 0U) || (MaxPolls > 1000U)) {
    return MtkDsiInvalid;
  }
  Status = BeginTransition (Core, MtkDsiStateStreaming, MtkDsiStateSuspending);
  if (Status != MtkDsiSuccess) {
    return Status;
  }

#define SUSPEND_RMW(Bank, Reg, Clear, Set) do { \
  Status = ModifyRegister (Core, (Bank), (Reg), (Clear), (Set)); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)
#define SUSPEND_WRITE(Bank, Reg, Val) do { \
  Status = WriteDirect (Core, (Bank), (Reg), (Val)); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)
#define SUSPEND_DELAY() do { \
  Status = DelayMicroseconds (Core); \
  if (Status != MtkDsiSuccess) return Status; \
} while (0)

  /* Operation 5 first inspects the low byte, then uses a full-word RMW.
   * Read32 is safe for this register; its unrelated bits remain untouched. */
  Status = ReadRegister (Core, MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x014U, &Value);
  if (Status != MtkDsiSuccess) {
    return Status;
  }
  if ((Value & 3U) != 0U) {
    SUSPEND_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x014U, 3U, 0U);
  }

  /* Original operation 9 tests signed bit31 of INTSTA (+0x0c). Never
   * power down the PHY while it still reports a busy transfer. */
  for (Poll = 0U; Poll < MaxPolls; ++Poll) {
    Status = ReadRegister (Core, MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x00CU, &Value);
    if (Status != MtkDsiSuccess) {
      return Status;
    }
    if ((Value & 0x80000000U) == 0U) {
      break;
    }
    if (Poll + 1U < MaxPolls) {
      SUSPEND_DELAY ();
    }
  }
  if (Poll == MaxPolls) {
    return CoreFailure (Core, MtkDsiTimeout);
  }
  Status = SavePort (Core);
  if (Status != MtkDsiSuccess) {
    return Status;
  }

  /* Operation 8 after our completed operation 7 (LK gate flag already 1).
   * This profile uses the D-PHY branch, fragment+0x568 == 0. */
  SUSPEND_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x104U, 1U, 0U);
  SUSPEND_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x108U, 1U, 0U);
  SUSPEND_DELAY ();
  SUSPEND_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x108U, 2U, 0U);
  SUSPEND_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x108U, 0U, 2U);
  SUSPEND_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x104U, 2U, 0U);
  SUSPEND_RMW (MtkDsiBankDsi0, DSI_PORT_OFFSET + 0x104U, 0U, 2U);
  SUSPEND_DELAY ();
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_030, 0x10U, 0U);
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_028, 0U, 2U);
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_028, 1U, 0U);
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_244, 0U, 1U);
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_444, 0U, 1U);
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_144, 0U, 1U);
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_544, 0U, 1U);
  SUSPEND_RMW (MtkDsiBankMipiTx, DSI_MIPI_344, 0U, 1U);
  SUSPEND_WRITE (MtkDsiBankMipiTx, DSI_MIPI_00C, 0x3FFF0180U);
  SUSPEND_WRITE (MtkDsiBankMipiTx, DSI_MIPI_00C, 0x3FFF0100U);

#undef SUSPEND_RMW
#undef SUSPEND_WRITE
#undef SUSPEND_DELAY

  Core->State = MtkDsiStateSuspended;
  Core->LastStatus = MtkDsiSuccess;
  Core->Busy = 0U;
  return MtkDsiSuccess;
}

MTK_DSI_STATUS
MtkDsiCoreResume (
  MTK_DSI_CORE *Core
  )
{
  MTK_DSI_STATUS Status;

  Status = BeginTransition (Core, MtkDsiStateSuspended, MtkDsiStateResuming);
  if (Status != MtkDsiSuccess) {
    return Status;
  }
  return RunOp7Registers (Core, &Core->SavedPort, 1U);
}
