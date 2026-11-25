#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/SynchronizationLib.h>

#include <Protocol/MtkPmicWrapper.h>
#include <Protocol/MtkPmic.h>

#define MT6358_TOPSTATUS 0x28
#define MT6358_PWRKEY_MASK BIT1
#define MT6358_HOMEKEY_MASK BIT3

#define MT6358_CON0_ENABLE_MASK BIT0

typedef enum {
  Ldo,
  FixedLdo,
  Buck
} MTK_REGULATOR_TYPE;

typedef struct {
  UINT32 Voltage;
  UINT16 Mask;
} MTK_VOSEL;

typedef struct {
  UINT16 Con0Reg;
  UINT16 AnaReg;

  UINT16 VoselShift;
  UINT16 VoselMask;

  CONST MTK_VOSEL *Ranges;
  UINT16           RangesLen;
} MTK_LDO_DESC;

typedef struct {
  UINT16 Con0Reg;
  UINT32 Voltage;
} MTK_FIXED_LDO_DESC;

typedef struct {
  UINT16 Con0Reg;
  UINT16 VSelReg;
  UINT16 VSelMask;

  UINT32 Step;
  UINT32 Min;
  UINT32 Max;
} MTK_BUCK_DESC;

typedef struct {
  CONST CHAR8 *Name;
  MTK_REGULATOR_TYPE Type;
  union {
    MTK_LDO_DESC Ldo;
    MTK_FIXED_LDO_DESC FixedLdo;
    MTK_BUCK_DESC Buck;
  };
} MTK_REGULATOR_DESC;

STATIC CONST MTK_VOSEL mVdram2Ranges[] = {
  {600000, 0x0},
  {1800000, 0xC},
};

STATIC CONST MTK_VOSEL mVsimRanges[] = {
  {1700000, 0x3},
  {1800000, 0x4},
  {2700000, 0x8},
  {3000000, 0xB},
  {3100000, 0xC},
};

STATIC CONST MTK_VOSEL mVibrRanges[] = {
  {1200000, 0x0},
  {1300000, 0x1},
  {1500000, 0x2},
  {1800000, 0x4},
  {2000000, 0x5},
  {2800000, 0x9},
  {3000000, 0xB},
  {3300000, 0xD},
};

STATIC CONST MTK_VOSEL mVusbRanges[] = {
  {3000000, 0x3},
  {3100000, 0x4},
};

STATIC CONST MTK_VOSEL mVcamdRanges[] = {
  {900000, 0x3},
  {1000000, 0x4},
  {1100000, 0x5},
  {1200000, 0x6},
  {1300000, 0x7},
  {1500000, 0x9},
  {1800000, 0xC},
};

STATIC CONST MTK_VOSEL mVefuseRanges[] = {
  {1700000, 0xB},
  {1800000, 0xC},
  {1900000, 0xD},
};

STATIC CONST MTK_VOSEL mVmchVemcRanges[] = {
  {2900000, 0x2},
  {3000000, 0x3},
  {3300000, 0x5},
};

STATIC CONST MTK_VOSEL mVcamaRanges[] = {
  {1800000, 0x0},
  {2500000, 0x7},
  {2700000, 0x9},
  {2800000, 0xA},
  {2900000, 0xB},
  {3000000, 0xC},
};

STATIC CONST MTK_VOSEL mVcn33Ranges[] = {
  {3300000, 0x1},
  {3400000, 0x2},
  {3500000, 0x3},
};

STATIC CONST MTK_VOSEL mVmcRanges[] = {
  {1800000, 0x400},
  {1810000, 0x401},
  {1820000, 0x402},
  {1830000, 0x403},
  {1840000, 0x404},
  {1850000, 0x405},
  {1860000, 0x406},
  {1870000, 0x407},
  {1880000, 0x408},
  {1890000, 0x409},
  {1900000, 0x40A},
  {2900000, 0xA00},
  {2910000, 0xA01},
  {2920000, 0xA02},
  {2930000, 0xA03},
  {2940000, 0xA04},
  {2950000, 0xA05},
  {2960000, 0xA06},
  {2970000, 0xA07},
  {2980000, 0xA08},
  {2990000, 0xA09},
  {3000000, 0xB00},
  {3010000, 0xB01},
  {3020000, 0xB02},
  {3030000, 0xB03},
  {3040000, 0xB04},
  {3050000, 0xB05},
  {3060000, 0xB06},
  {3070000, 0xB07},
  {3080000, 0xB08},
  {3090000, 0xB09},
  {3100000, 0xB0A},
  {3300000, 0xD00},
};

STATIC CONST MTK_VOSEL mVldo28Ranges[] = {
  {2800000, 0x1},
  {3000000, 0x3},
};

STATIC CONST MTK_REGULATOR_DESC mRegulators[] = {
  {
    .Name = "buck_vdram1",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1608,
      .VSelReg  = 0x1626,
      .VSelMask = 0x7f,
      .Step     = 12500,
      .Min      = 500000,
      .Max      = 2087500,
    },
  },
  {
    .Name = "buck_vcore",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1488,
      .VSelReg  = 0x14aa,
      .VSelMask = 0x7f,
      .Step     = 6250,
      .Min      = 500000,
      .Max      = 1293750,
    },
  },
  {
    .Name = "buck_vpa",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1788,
      .VSelReg  = 0x178a,
      .VSelMask = 0x3f,
      .Step     = 50000,
      .Min      = 500000,
      .Max      = 3650000,
    },
  },
  {
    .Name = "buck_vproc11",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1388,
      .VSelReg  = 0x13a6,
      .VSelMask = 0x7f,
      .Step     = 6250,
      .Min      = 500000,
      .Max      = 1293750,
    },
  },
  {
    .Name = "buck_vproc12",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1408,
      .VSelReg  = 0x1426,
      .VSelMask = 0x7f,
      .Step     = 6250,
      .Min      = 500000,
      .Max      = 1293750,
    },
  },
  {
    .Name = "buck_vgpu",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1508,
      .VSelReg  = 0x1526,
      .VSelMask = 0x7f,
      .Step     = 6250,
      .Min      = 500000,
      .Max      = 1293750,
    },
  },
  {
    // LDO with a linear 6.25 mV selector, so it uses the buck description:
    // enable LDO_VSRAM_GPU_CON0[0], selector LDO_VSRAM_CON3[6:0]
    // (mt6358-regulator.c MT6358_LDO1 "ldo_vsram_gpu", also used for MT6366).
    .Name = "ldo_vsram_gpu",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1bc8,
      .VSelReg  = 0x1bf4,
      .VSelMask = 0x7f,
      .Step     = 6250,
      .Min      = 500000,
      .Max      = 1293750,
    },
  },
  {
    .Name = "buck_vs2",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1708,
      .VSelReg  = 0x172e,
      .VSelMask = 0x7f,
      .Step     = 12500,
      .Min      = 500000,
      .Max      = 2087500,
    },
  },
  {
    .Name = "buck_vmodem",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1588,
      .VSelReg  = 0x15a6,
      .VSelMask = 0x7f,
      .Step     = 6250,
      .Min      = 500000,
      .Max      = 1293750,
    },
  },
  {
    .Name = "buck_vs1",
    .Type = Buck,
    .Buck =
    {
      .Con0Reg  = 0x1688,
      .VSelReg  = 0x16ae,
      .VSelMask = 0x7f,
      .Step     = 12500,
      .Min      = 1000000,
      .Max      = 2587500,
    },
  },
  {
    .Name     = "ldo_vrf12",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1c30,
      .Voltage = 1200000,
    },
  },
  {
    .Name     = "ldo_vio18",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1aec,
      .Voltage = 1800000,
    },
  },
  {
    .Name     = "ldo_vcamio",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1cb0,
      .Voltage = 1800000,
    },
  },
  {
    .Name     = "ldo_vcn18",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1c58,
      .Voltage = 1800000,
    },
  },
  {
    .Name     = "ldo_vfe28",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1c08,
      .Voltage = 2800000,
    },
  },
  {
    .Name     = "ldo_vcn28",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1d88,
      .Voltage = 2800000,
    },
  },
  {
    .Name     = "ldo_vxo22",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1a88,
      .Voltage = 2200000,
    },
  },
  {
    .Name     = "ldo_vaux18",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1ab0,
      .Voltage = 1800000,
    },
  },
  {
    .Name     = "ldo_vbif28",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1d9e,
      .Voltage = 2800000,
    },
  },
  {
    .Name     = "ldo_vio28",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1ad8,
      .Voltage = 2800000,
    },
  },
  {
    .Name     = "ldo_va12",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1a9c,
      .Voltage = 1200000,
    },
  },
  {
    .Name     = "ldo_vrf18",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1c1c,
      .Voltage = 1800000,
    },
  },
  {
    .Name     = "ldo_vaud28",
    .Type     = FixedLdo,
    .FixedLdo =
    {
      .Con0Reg = 0x1ac4,
      .Voltage = 2800000,
    },
  },
  {
    .Name = "ldo_vdram2",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1b08,
      .AnaReg     = 0x1eaa,
      .VoselShift = 0,
      .VoselMask  = 0xf,
      .Ranges     = mVdram2Ranges,
      .RangesLen  = ARRAY_SIZE (mVdram2Ranges),
    },
  },
  {
    .Name = "ldo_vsim1",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1d48,
      .AnaReg     = 0x1e2c,
      .VoselShift = 8,
      .VoselMask  = 0xf,
      .Ranges     = mVsimRanges,
      .RangesLen  = ARRAY_SIZE (mVsimRanges),
    },
  },
  {
    .Name = "ldo_vibr",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1d08,
      .AnaReg     = 0x1e44,
      .VoselShift = 8,
      .VoselMask  = 0xf,
      .Ranges     = mVibrRanges,
      .RangesLen  = ARRAY_SIZE (mVibrRanges),
    },
  },
  {
    .Name = "ldo_vusb",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1b30,
      .AnaReg     = 0x1e34,
      .VoselShift = 8,
      .VoselMask  = 0x7,
      .Ranges     = mVusbRanges,
      .RangesLen  = ARRAY_SIZE (mVusbRanges),
    },
  },
  {
    .Name = "ldo_vcamd",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1c9c,
      .AnaReg     = 0x1eae,
      .VoselShift = 8,
      .VoselMask  = 0xf,
      .Ranges     = mVcamdRanges,
      .RangesLen  = ARRAY_SIZE (mVcamdRanges),
    },
  },
  {
    .Name = "ldo_vefuse",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1c44,
      .AnaReg     = 0x1e98,
      .VoselShift = 8,
      .VoselMask  = 0xf,
      .Ranges     = mVefuseRanges,
      .RangesLen  = ARRAY_SIZE (mVefuseRanges),
    },
  },
  {
    .Name = "ldo_vmch",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1cd8,
      .AnaReg     = 0x1e48,
      .VoselShift = 8,
      .VoselMask  = 0x7,
      .Ranges     = mVmchVemcRanges,
      .RangesLen  = ARRAY_SIZE (mVmchVemcRanges),
    },
  },
  {
    .Name = "ldo_vcama1",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1c6c,
      .AnaReg     = 0x1e08,
      .VoselShift = 8,
      .VoselMask  = 0xf,
      .Ranges     = mVcamaRanges,
      .RangesLen  = ARRAY_SIZE (mVcamaRanges),
    },
  },
  {
    .Name = "ldo_vemc",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1b1c,
      .AnaReg     = 0x1e38,
      .VoselShift = 8,
      .VoselMask  = 0x7,
      .Ranges     = mVmchVemcRanges,
      .RangesLen  = ARRAY_SIZE (mVmchVemcRanges),
    },
  },
  {
    .Name = "ldo_vcn33",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1d1c,
      .AnaReg     = 0x1e28,
      .VoselShift = 8,
      .VoselMask  = 0x3,
      .Ranges     = mVcn33Ranges,
      .RangesLen  = ARRAY_SIZE (mVcn33Ranges),
    },
  },
  {
    .Name = "ldo_vcama2",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1c88,
      .AnaReg     = 0x1e0c,
      .VoselShift = 8,
      .VoselMask  = 0xf,
      .Ranges     = mVcamaRanges,
      .RangesLen  = ARRAY_SIZE (mVcamaRanges),
    },
  },
  {
    .Name = "ldo_vmc",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1cc4,
      .AnaReg     = 0x1e4c,
      .VoselShift = 0,
      .VoselMask  = 0xfff,
      .Ranges     = mVmcRanges,
      .RangesLen  = ARRAY_SIZE (mVmcRanges),
    },
  },
  {
    .Name = "ldo_vldo28",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1d32,
      .AnaReg     = 0x1e3c,
      .VoselShift = 8,
      .VoselMask  = 0x3,
      .Ranges     = mVldo28Ranges,
      .RangesLen  = ARRAY_SIZE (mVldo28Ranges),
    },
  },
  {
    .Name = "ldo_vsim2",
    .Type = Ldo,
    .Ldo  =
    {
      .Con0Reg    = 0x1d5c,
      .AnaReg     = 0x1e30,
      .VoselShift = 8,
      .VoselMask  = 0xf,
      .Ranges     = mVsimRanges,
      .RangesLen  = ARRAY_SIZE (mVsimRanges),
    },
  },
};

STATIC MTK_PMIC_WRAPPER_PROTOCOL *mPmicWrapper = NULL;

STATIC
CONST
MTK_REGULATOR_DESC*
GetRegulatorByName(
  IN CONST CHAR8 *Name)
{
  if (Name == NULL) {
    return NULL;
  }

  for (UINTN Index = 0; Index < ARRAY_SIZE(mRegulators); Index++) {
    if (AsciiStrCmp(mRegulators[Index].Name, Name) == 0) {
      return &mRegulators[Index];
    }
  }

  return NULL;
}

VOID
PowerButtonPressed (
  OUT BOOLEAN *Pressed)
{
  UINT16 Value;

  if (Pressed == NULL) { return; }
  *Pressed = FALSE;
  if (EFI_ERROR (mPmicWrapper->Read (MT6358_TOPSTATUS, &Value))) { return; }
  Value &= MT6358_PWRKEY_MASK;

  *Pressed = (Value == 0) ? TRUE : FALSE;
}

VOID
HomeButtonPressed (
  OUT BOOLEAN *Pressed)
{
  UINT16 Value;

  if (Pressed == NULL) { return; }
  *Pressed = FALSE;
  if (EFI_ERROR (mPmicWrapper->Read (MT6358_TOPSTATUS, &Value))) { return; }
  Value &= MT6358_HOMEKEY_MASK;

  *Pressed = (Value == 0) ? TRUE : FALSE;
}

STATIC
EFI_STATUS
UpdatePmicBits (UINT16 Address, UINT16 Mask, UINT16 Bits)
{
  EFI_STATUS Status;
  UINT16 Value;

  Status = mPmicWrapper->Read (Address, &Value);
  if (EFI_ERROR (Status)) { return Status; }
  return mPmicWrapper->Write (Address, (Value & ~Mask) | (Bits & Mask));
}

STATIC
BOOLEAN
IsVmchEint (CONST CHAR8 *Name)
{
  return (Name != NULL) &&
    ((AsciiStrCmp (Name, "vmch_eint_low") == 0) ||
     (AsciiStrCmp (Name, "vmch_eint_high") == 0));
}

STATIC
EFI_STATUS
RegulatorSetEnableUnlocked(
  IN CONST CHAR8 *Name,
  IN BOOLEAN Enable)
{
  EFI_STATUS Status;
  UINT16 Value;
  CONST MTK_REGULATOR_DESC *Regulator = GetRegulatorByName(IsVmchEint (Name) ? "ldo_vmch" : Name);
  if (Regulator == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid regulator name \"%a\" is provided\n", Name));
    return EFI_INVALID_PARAMETER;
  }

  if (IsVmchEint (Name)) {
    UINT16 OldEint, OldLdo;
    EFI_STATUS RestoreEint, RestoreLdo;
    // Snapshot both registers before any write. Preserve unrelated PMIC bits.
    Status = mPmicWrapper->Read (0x1D70, &OldEint);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mPmicWrapper->Read (0x1CD8, &OldLdo);
    if (EFI_ERROR (Status)) { return Status; }
    if ((((OldLdo | OldEint) & BIT0) != 0) &&
        (((OldEint & BIT2) != 0) != (AsciiStrCmp (Name, "vmch_eint_high") == 0))) {
      return EFI_ACCESS_DENIED;
    }
    if (Enable) {
      Status = UpdatePmicBits (0x1D70, BIT2,
                  AsciiStrCmp (Name, "vmch_eint_high") == 0 ? BIT2 : 0);
      if (!EFI_ERROR (Status)) { Status = UpdatePmicBits (0x1CD8, BIT0, BIT0); }
      if (!EFI_ERROR (Status)) { Status = UpdatePmicBits (0x1D70, BIT0, BIT0); }
    } else {
      Status = UpdatePmicBits (0x1CD8, BIT0, 0);
      if (!EFI_ERROR (Status)) {
        MicroSecondDelay (1500);
        Status = UpdatePmicBits (0x1D70, BIT0, 0);
      }
    }
    if (!EFI_ERROR (Status)) { return EFI_SUCCESS; }
    // A failed transfer may already have reached the PMIC. Restore even that
    // register; report a distinct device error if restoration cannot finish.
    RestoreLdo = UpdatePmicBits (0x1CD8, BIT0, OldLdo);
    if ((OldLdo & BIT0) == 0) MicroSecondDelay (1500);
    RestoreEint = UpdatePmicBits (0x1D70, BIT0 | BIT2, OldEint);
    return EFI_ERROR (RestoreEint) || EFI_ERROR (RestoreLdo) ? EFI_DEVICE_ERROR : Status;
  }

  switch (Regulator->Type) {
  case Ldo:
    // Write enable bit to CON0 register
    Status = mPmicWrapper->Read (Regulator->Ldo.Con0Reg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    if (Enable) {
      Value |= MT6358_CON0_ENABLE_MASK;
    } else {
      Value &= ~MT6358_CON0_ENABLE_MASK;
    }
    return mPmicWrapper->Write (Regulator->Ldo.Con0Reg, Value);
  case FixedLdo:
    // Write enable bit to CON0 register
    Status = mPmicWrapper->Read (Regulator->FixedLdo.Con0Reg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    if (Enable) {
      Value |= MT6358_CON0_ENABLE_MASK;
    } else {
      Value &= ~MT6358_CON0_ENABLE_MASK;
    }
    return mPmicWrapper->Write (Regulator->FixedLdo.Con0Reg, Value);
  case Buck:
    // Write enable bit to CON0 register
    Status = mPmicWrapper->Read (Regulator->Buck.Con0Reg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    if (Enable) {
      Value |= MT6358_CON0_ENABLE_MASK;
    } else {
      Value &= ~MT6358_CON0_ENABLE_MASK;
    }
    return mPmicWrapper->Write (Regulator->Buck.Con0Reg, Value);
  default:
    DEBUG ((EFI_D_ERROR, "Invalid regulator type %d\n", Regulator->Type));
    return EFI_INVALID_PARAMETER;
  }
}

STATIC
EFI_STATUS
RegulatorIsEnabledUnlocked(
  IN  CONST CHAR8 *Name,
  OUT BOOLEAN *Enabled)
{
  EFI_STATUS Status;
  UINT16 Value;
  CONST MTK_REGULATOR_DESC *Regulator = GetRegulatorByName(IsVmchEint (Name) ? "ldo_vmch" : Name);
  if (Regulator == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid regulator name \"%a\" is provided\n", Name));
    return EFI_INVALID_PARAMETER;
  }

  if (Enabled == NULL) { return EFI_INVALID_PARAMETER; }

  if (IsVmchEint (Name)) {
    UINT16 Ldo;
    Status = mPmicWrapper->Read (0x1D70, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mPmicWrapper->Read (0x1CD8, &Ldo);
    if (EFI_ERROR (Status)) { return Status; }
    if ((Value & BIT0) != (Ldo & BIT0)) { return EFI_DEVICE_ERROR; }
    if (((Value & BIT0) != 0) &&
        (((Value & BIT2) != 0) != (AsciiStrCmp (Name, "vmch_eint_high") == 0))) {
      return EFI_ACCESS_DENIED;
    }
    *Enabled = (Value & BIT0) != 0;
    return EFI_SUCCESS;
  }

  switch (Regulator->Type) {
  case Ldo:
    // Read enable bit from CON0 register
    Status = mPmicWrapper->Read (Regulator->Ldo.Con0Reg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    *Enabled = Value & MT6358_CON0_ENABLE_MASK;
    return EFI_SUCCESS;
  case FixedLdo:
    // Read enable bit from CON0 register
    Status = mPmicWrapper->Read (Regulator->FixedLdo.Con0Reg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    *Enabled = Value & MT6358_CON0_ENABLE_MASK;
    return EFI_SUCCESS;
  case Buck:
    // Read enable bit from CON0 register
    Status = mPmicWrapper->Read (Regulator->Buck.Con0Reg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    *Enabled = Value & MT6358_CON0_ENABLE_MASK;
    return EFI_SUCCESS;
  default:
    DEBUG ((EFI_D_ERROR, "Invalid regulator type %d\n", Regulator->Type));
    return EFI_INVALID_PARAMETER;
  }
}

STATIC
EFI_STATUS
RegulatorSetVoltageUnlocked(
  IN CONST CHAR8 *Name,
  IN UINT32 Voltage)
{
  EFI_STATUS Status;
  UINT16 Value;
  CONST MTK_REGULATOR_DESC *Regulator = GetRegulatorByName(IsVmchEint (Name) ? "ldo_vmch" : Name);
  if (Regulator == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid regulator name \"%a\" is provided\n", Name));
    return EFI_INVALID_PARAMETER;
  }

  switch (Regulator->Type) {
  case Ldo: {
    UINTN Index;
    UINT16 Vosel;

    // Map specified voltage to voltage selection
    for (Index = 0; Index < Regulator->Ldo.RangesLen; Index++) {
      if (Voltage == Regulator->Ldo.Ranges[Index].Voltage) {
        Vosel = Regulator->Ldo.Ranges[Index].Mask;
        break;
      }
    }

    // If voltage is not found in the ranges, return error
    if (Index == Regulator->Ldo.RangesLen) {
      return EFI_INVALID_PARAMETER;
    }

    // Write voltage selection to analog register
    Status = mPmicWrapper->Read (Regulator->Ldo.AnaReg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    Value &= ~(Regulator->Ldo.VoselMask << Regulator->Ldo.VoselShift);
    Value |= Vosel << Regulator->Ldo.VoselShift;
    return mPmicWrapper->Write (Regulator->Ldo.AnaReg, Value);
  }
  case FixedLdo:
    // Voltage of Fixed LDO cannot be changed
    return EFI_UNSUPPORTED;
  case Buck:
    if ((Voltage < Regulator->Buck.Min) || (Voltage > Regulator->Buck.Max) ||
        ((Voltage - Regulator->Buck.Min) % Regulator->Buck.Step != 0)) {
      return EFI_INVALID_PARAMETER;
    }

    // Write voltage to VSel register
    Status = mPmicWrapper->Read (Regulator->Buck.VSelReg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    Value &= ~Regulator->Buck.VSelMask;
    Value |= ((Voltage - Regulator->Buck.Min) / Regulator->Buck.Step) & Regulator->Buck.VSelMask;
    return mPmicWrapper->Write (Regulator->Buck.VSelReg, Value);
  default:
    DEBUG ((EFI_D_ERROR, "Invalid regulator type %d\n", Regulator->Type));
    return EFI_INVALID_PARAMETER;
  }
}

STATIC
EFI_STATUS
RegulatorGetVoltageUnlocked(
  IN  CONST CHAR8 *Name,
  OUT UINT32 *Voltage)
{
  EFI_STATUS Status;
  UINT16 Value;

  // Find regulator by name
  CONST MTK_REGULATOR_DESC *Regulator = GetRegulatorByName(IsVmchEint (Name) ? "ldo_vmch" : Name);
  if (Regulator == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid regulator name \"%a\" is provided\n", Name));
    return EFI_INVALID_PARAMETER;
  }

  if (Voltage == NULL) { return EFI_INVALID_PARAMETER; }

  switch (Regulator->Type) {
  case Ldo:
    // Read voltage selection from Analog register
    Status = mPmicWrapper->Read (Regulator->Ldo.AnaReg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    Value = (Value >> Regulator->Ldo.VoselShift) & Regulator->Ldo.VoselMask;

    // Map voltage selection to voltage
    for (UINTN Index = 0; Index < Regulator->Ldo.RangesLen; Index++) {
      if (Value == Regulator->Ldo.Ranges[Index].Mask) {
        *Voltage = Regulator->Ldo.Ranges[Index].Voltage;
        return EFI_SUCCESS;
      }
    }

    return EFI_DEVICE_ERROR;
  case FixedLdo:
    // Return fixed voltage
    *Voltage = Regulator->FixedLdo.Voltage;
    return EFI_SUCCESS;
  case Buck:
    // Read voltage selection from VSel register
    Status = mPmicWrapper->Read (Regulator->Buck.VSelReg, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    Value &= Regulator->Buck.VSelMask;

    // Calculate voltage based on selection
    *Voltage = Regulator->Buck.Min + (Value * Regulator->Buck.Step);
    return EFI_SUCCESS;
  default:
    DEBUG ((EFI_D_ERROR, "Invalid regulator type %d\n", Regulator->Type));
    return EFI_INVALID_PARAMETER;
  }
}

// Serialize the complete regulator transaction, including snapshots and rollback.
// Do not spin: a higher-TPL caller may have interrupted the current owner.
// This is not a lease for clients using the raw PMIC wrapper directly.
STATIC volatile UINT32 mRegulatorBusy;

EFI_STATUS
RegulatorSetEnable (IN CONST CHAR8 *Name, BOOLEAN Enable)
{
  EFI_STATUS Status;
  if (InterlockedCompareExchange32 (&mRegulatorBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  Status = RegulatorSetEnableUnlocked (Name, Enable);
  InterlockedCompareExchange32 (&mRegulatorBusy, 1, 0);
  return Status;
}

EFI_STATUS
RegulatorIsEnabled (IN CONST CHAR8 *Name, BOOLEAN * Enabled)
{
  EFI_STATUS Status;
  if (InterlockedCompareExchange32 (&mRegulatorBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  Status = RegulatorIsEnabledUnlocked (Name, Enabled);
  InterlockedCompareExchange32 (&mRegulatorBusy, 1, 0);
  return Status;
}

EFI_STATUS
RegulatorSetVoltage (IN CONST CHAR8 *Name, UINT32 Voltage)
{
  EFI_STATUS Status;
  if (InterlockedCompareExchange32 (&mRegulatorBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  Status = RegulatorSetVoltageUnlocked (Name, Voltage);
  InterlockedCompareExchange32 (&mRegulatorBusy, 1, 0);
  return Status;
}

EFI_STATUS
RegulatorGetVoltage (IN CONST CHAR8 *Name, UINT32 * Voltage)
{
  EFI_STATUS Status;
  if (InterlockedCompareExchange32 (&mRegulatorBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  Status = RegulatorGetVoltageUnlocked (Name, Voltage);
  InterlockedCompareExchange32 (&mRegulatorBusy, 1, 0);
  return Status;
}

STATIC MTK_PMIC_PROTOCOL mPmic = {
  PowerButtonPressed,
  HomeButtonPressed,
  RegulatorSetEnable,
  RegulatorIsEnabled,
  RegulatorSetVoltage,
  RegulatorGetVoltage
};

EFI_STATUS
EFIAPI
InitPmic (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  // Locate PMIC Wrapper Protocol
  Status = gBS->LocateProtocol (&gMediaTekPmicWrapperProtocolGuid, NULL, (VOID **)&mPmicWrapper);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate PMIC Wrapper Protocol! Status = %r\n", Status));
    return Status;
  }

  // Register MT6358 PMIC Protocol
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gMediaTekPmicProtocolGuid, &mPmic, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register MT6358 PMIC Protocol! Status = %r\n", Status));
    return Status;
  }

  return Status;
}