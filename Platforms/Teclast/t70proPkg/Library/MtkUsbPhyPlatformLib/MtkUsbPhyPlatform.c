// SPDX-License-Identifier: BSD-2-Clause-Patent
// T70-specific DT and platform-service adapter for the portable U2 TPHY core.
// No Type-C, VBUS, USB-role selection, or ready publisher is present here.
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/Fdt.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/MtkUsbPhyPlatformLib.h>

#define T70_USB_NODE            "/soc/usb0@11200000"
#define T70_PHY_BUS             "/soc/usb-phy@11f40000"
#define T70_PHY_NODE            "/soc/usb-phy@11f40000/usb-phy@11f40000"
#define T70_CLOCK_NODE          "/clocks/clk26m"
#define T70_NVMEM_CELL_NODE     "/soc/efuse@11c10000/u2_phy_data"
#define T70_NVMEM_PROVIDER_NODE "/soc/efuse@11c10000"
#define T70_USB_BASE            0x11200000ULL
#define T70_USB_BYTES           0x10000ULL
#define T70_PHY_BASE            0x11f40000ULL
#define T70_PHY_BYTES           0x700ULL
#define T70_DEVINFO_BASE        0x11c10000ULL
#define T70_DEVINFO_BYTES       0x10000ULL
#define T70_INTR_CAL_OFFSET     0x1b0U
#define T70_INTR_CAL_BYTES      4U
#define T70_INTR_CAL_MASK       0x1fU
#define T70_REF_CLOCK_NAME      "TOP_CLK26M"
#define T70_REF_CLOCK_HZ        26000000ULL
#define T70_MAX_FDT_BYTES       0x200000U

typedef struct {
  MTK_USB_PHY_CONFIG Config;
  MTK_USB_PHY_HOST_TUNING HostTuning;
  UINT64 PhyBase;
  UINT64 PhyBytes;
  UINT64 NvmemProviderBase;
  UINT64 NvmemProviderBytes;
  UINT32 NvmemCellOffset;
  UINT32 NvmemCellBytes;
  UINT32 NvmemCellMask;
} T70_USB_PHY_DT_CONFIG;

typedef struct {
  BOOLEAN Active;
  CONST VOID *BootFdt;
  UINTN BootFdtBytes;
  MTK_CLOCK_PROTOCOL *Clock;
  MTK_USB_PHY_PLATFORM_NVMEM_READ ReadNvmem;
  VOID *NvmemContext;
  UINT64 PhyBase;
  UINT64 PhyBytes;
  UINT64 NvmemProviderBase;
  UINT64 NvmemProviderBytes;
  UINT32 NvmemCellOffset;
  UINT32 NvmemCellBytes;
  UINT32 NvmemCellMask;
  UINT32 ClockId[MtkUsbPhyClockCount];
  UINT8 ClockPresentMask;
  UINT8 ClockPreparedMask;
  UINT8 ClockEnabledMask;
} T70_USB_PHY_PLATFORM_CONTEXT;

STATIC T70_USB_PHY_PLATFORM_CONTEXT mT70Phy;

STATIC UINT32
ReadBe32 (IN CONST UINT8 *Bytes)
{
  return ((UINT32)Bytes[0] << 24) | ((UINT32)Bytes[1] << 16) |
         ((UINT32)Bytes[2] << 8) | (UINT32)Bytes[3];
}

STATIC UINT32
ReadLe32 (IN CONST UINT8 *Bytes)
{
  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

/* Shipped nvmem_mtk-devinfo.ko: devinfo_parse_dt -> mtk_reg_read.
 * The provider uses the little-endian atag payload, not devinfo MMIO/SMC.
 * Only the exact copied boot FDT can supply a calibration value. */
STATIC MTK_USB_PHY_STATUS
ReadBootIntrCal (IN CONST VOID *Fdt, IN UINTN FdtBytes, OUT UINT32 *RawValue)
{
  VOID *Published = NULL;
  CONST UINT8 *Tag;
  INT32 Chosen, Length;
  UINT32 Count;
  if (RawValue == NULL) return MtkUsbPhyInvalid;
  *RawValue = 0;
  if (Fdt == NULL || FdtBytes < sizeof(FDT_HEADER) || FdtBytes > T70_MAX_FDT_BYTES ||
      EFI_ERROR(EfiGetSystemConfigurationTable(&gFdtTableGuid,&Published)) || Published != Fdt ||
      MmioRead64(0x43F7F000) != 0x0000000148303754ULL ||
      MmioRead64(0x43F7F048) != (UINT64)(UINTN)Fdt ||
      MmioRead64(0x43F7F050) != FdtBytes || MmioRead64(0x43F7F058) != EFI_SUCCESS)
    return MtkUsbPhyNotReady;
  if (FdtCheckFull(Fdt,FdtBytes) != 0 || FdtTotalSize(Fdt) != FdtBytes)
    return MtkUsbPhyNvmemError;
  Chosen = FdtPathOffset(Fdt,"/chosen");
  if (Chosen < 0) Chosen = FdtPathOffset(Fdt,"/chosen@0");
  if (Chosen < 0) return MtkUsbPhyNotReady;
  Tag = FdtGetProp(Fdt,Chosen,"atag,devinfo",&Length);
  if (Tag == NULL) return MtkUsbPhyNotReady;
  if (Length < 4) return MtkUsbPhyNvmemError;
  Count = ReadLe32(Tag);
  // Stock accepts at most 400 words; unlike stock, check the property extent
  // before reading index 108. No raw value is retained or logged here.
  if (Count <= T70_INTR_CAL_OFFSET/4U || Count > 400U ||
      (UINT32)Length-4U < Count*4U) return MtkUsbPhyNvmemError;
  *RawValue = ReadLe32(Tag+4U+T70_INTR_CAL_OFFSET);
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
ReadStockNvmem (IN VOID *Context, IN CONST CHAR8 *Name, IN UINT32 Mask, OUT UINT32 *RawValue)
{
  if (RawValue != NULL) *RawValue = 0;
  if (Context != &mT70Phy || !mT70Phy.Active || Name == NULL || RawValue == NULL ||
      AsciiStrCmp(Name,"intr_cal") != 0 || Mask != T70_INTR_CAL_MASK)
    return MtkUsbPhyInvalid;
  return ReadBootIntrCal(mT70Phy.BootFdt,mT70Phy.BootFdtBytes,RawValue);
}

STATIC UINT64
ReadBeCells (IN CONST UINT8 *Bytes, IN UINT32 CellCount)
{
  UINT64 Value = 0;
  UINT32 Index;
  for (Index = 0; Index < CellCount; ++Index) {
    Value = (Value << 32) | ReadBe32 (Bytes + Index * sizeof (UINT32));
  }
  return Value;
}

STATIC BOOLEAN
NodeEnabled (IN CONST VOID *Fdt, IN INT32 Node)
{
  UINTN Depth;
  for (Depth = 0; (Node >= 0) && (Depth < 128U); ++Depth) {
    INT32 Length;
    CONST CHAR8 *Status = FdtGetProp (Fdt, Node, "status", &Length);
    if ((Status != NULL) &&
        !(((Length == 5) && (CompareMem (Status, "okay", 5) == 0)) ||
          ((Length == 3) && (CompareMem (Status, "ok", 3) == 0)))) return FALSE;
    if (Node == 0) return TRUE;
    Node = FdtParentOffset (Fdt, Node);
  }
  return FALSE;
}

STATIC BOOLEAN
NodeHasCompatible (IN CONST VOID *Fdt, IN INT32 Node, IN CONST CHAR8 *Compatible)
{
  INT32 Length;
  CONST CHAR8 *Property = FdtGetProp (Fdt, Node, "compatible", &Length);
  UINTN Wanted = AsciiStrLen (Compatible);
  UINTN Offset = 0;
  BOOLEAN Match = FALSE;
  if ((Property == NULL) || (Length <= 0)) return FALSE;
  while (Offset < (UINTN)Length) {
    UINTN End = Offset;
    while ((End < (UINTN)Length) && (Property[End] != '\0')) ++End;
    // Validate every list member, including the tail after a match.
    if ((End == (UINTN)Length) || (End == Offset)) return FALSE;
    if (((End - Offset) == Wanted) &&
        (CompareMem (Property + Offset, Compatible, Wanted) == 0)) Match = TRUE;
    Offset = End + 1;
  }
  return Match;
}

STATIC EFI_STATUS
ReadU32Property (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  IN CONST CHAR8 *Name,
  OUT UINT32 *Value
  )
{
  INT32 Length;
  CONST UINT8 *Property;
  if ((Fdt == NULL) || (Node < 0) || (Name == NULL) || (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Property = FdtGetProp (Fdt, Node, Name, &Length);
  if ((Property == NULL) || (Length != sizeof (UINT32))) return EFI_UNSUPPORTED;
  *Value = ReadBe32 (Property);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ReadSingleString (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  IN CONST CHAR8 *Name,
  IN CONST CHAR8 *Expected
  )
{
  INT32 Length;
  CONST CHAR8 *Property = FdtGetProp (Fdt, Node, Name, &Length);
  UINTN ExpectedLength = AsciiStrLen (Expected);
  if ((Property == NULL) || (Length != (INT32)(ExpectedLength + 1)) ||
      (CompareMem (Property, Expected, ExpectedLength) != 0) ||
      (Property[ExpectedLength] != '\0')) return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ReadBooleanProperty (IN CONST VOID *Fdt, IN INT32 Node, IN CONST CHAR8 *Name)
{
  INT32 Length;
  CONST VOID *Property = FdtGetProp (Fdt, Node, Name, &Length);
  return ((Property != NULL) && (Length == 0)) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
ReadReg (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  IN INT32 Parent,
  IN UINT32 TupleIndex,
  OUT UINT64 *Base,
  OUT UINT64 *Bytes
  )
{
  UINT32 AddressCells, SizeCells;
  UINTN TupleBytes, Offset;
  INT32 Length;
  CONST UINT8 *Reg;
  EFI_STATUS Status;
  if ((Base == NULL) || (Bytes == NULL)) return EFI_INVALID_PARAMETER;
  Status = ReadU32Property (Fdt, Parent, "#address-cells", &AddressCells);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, Parent, "#size-cells", &SizeCells);
  if (EFI_ERROR (Status)) return Status;
  if ((AddressCells == 0) || (AddressCells > 2) ||
      (SizeCells == 0) || (SizeCells > 2)) return EFI_UNSUPPORTED;
  TupleBytes = (AddressCells + SizeCells) * sizeof (UINT32);
  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if ((Reg == NULL) || (Length <= 0) || ((UINTN)Length % TupleBytes != 0) ||
      (TupleIndex >= ((UINTN)Length / TupleBytes))) return EFI_UNSUPPORTED;
  Offset = TupleIndex * TupleBytes;
  *Base = ReadBeCells (Reg + Offset, AddressCells);
  *Bytes = ReadBeCells (Reg + Offset + AddressCells * sizeof (UINT32), SizeCells);
  if ((*Base == 0) || (*Bytes == 0) || (*Bytes > MAX_UINT64 - *Base)) {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ReadExactReg (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  IN INT32 Parent,
  IN UINT32 TupleIndex,
  IN UINT64 ExpectedBase,
  IN UINT64 ExpectedBytes
  )
{
  UINT64 Base, Bytes;
  EFI_STATUS Status = ReadReg (Fdt, Node, Parent, TupleIndex, &Base, &Bytes);
  if (EFI_ERROR (Status)) return Status;
  return ((Base == ExpectedBase) && (Bytes == ExpectedBytes)) ?
         EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
ValidateClockTree (
  IN CONST VOID *Fdt,
  IN INT32 PhyNode,
  OUT UINT32 *ClockPhandle
  )
{
  INT32 ClockNode, Length;
  UINT32 Phandle, ClockCells, Frequency;
  CONST UINT8 *Clocks;
  CONST VOID *Names;
  EFI_STATUS Status;
  if (ClockPhandle == NULL) return EFI_INVALID_PARAMETER;
  Clocks = FdtGetProp (Fdt, PhyNode, "clocks", &Length);
  if ((Clocks == NULL) || (Length != sizeof (UINT32))) return EFI_UNSUPPORTED;
  Phandle = ReadBe32 (Clocks);
  ClockNode = FdtNodeOffsetByPhandle (Fdt, Phandle);
  if ((ClockNode < 0) || (ClockNode != FdtPathOffset (Fdt, T70_CLOCK_NODE)) ||
      !NodeEnabled (Fdt, ClockNode) ||
      !NodeHasCompatible (Fdt, ClockNode, "fixed-clock")) return EFI_UNSUPPORTED;
  Status = ReadU32Property (Fdt, ClockNode, "#clock-cells", &ClockCells);
  if (EFI_ERROR (Status) || (ClockCells != 0)) return EFI_UNSUPPORTED;
  Status = ReadU32Property (Fdt, ClockNode, "clock-frequency", &Frequency);
  if (EFI_ERROR (Status) || (Frequency != T70_REF_CLOCK_HZ)) return EFI_UNSUPPORTED;
  Names = FdtGetProp (Fdt, PhyNode, "clock-names", &Length);
  if ((Names == NULL) || (Length != sizeof ("ref")) ||
      (CompareMem (Names, "ref", sizeof ("ref")) != 0)) {
    // The optional da_ref slot is absent in T70. Do not infer or add it.
    return EFI_UNSUPPORTED;
  }
  *ClockPhandle = Phandle;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ValidateNvmemTree (
  IN CONST VOID *Fdt,
  IN INT32 PhyNode,
  OUT T70_USB_PHY_DT_CONFIG *Config
  )
{
  INT32 CellNode, ProviderNode, Length;
  UINT32 Phandle, CellCount;
  CONST UINT8 *Property;
  UINT64 Base, Bytes;
  EFI_STATUS Status;
  Property = FdtGetProp (Fdt, PhyNode, "nvmem-cells", &Length);
  if ((Property == NULL) || (Length != sizeof (UINT32))) return EFI_UNSUPPORTED;
  Phandle = ReadBe32 (Property);
  CellNode = FdtNodeOffsetByPhandle (Fdt, Phandle);
  if ((CellNode < 0) || !NodeEnabled (Fdt, CellNode) ||
      (CellNode != FdtPathOffset (Fdt, T70_NVMEM_CELL_NODE))) {
    return EFI_UNSUPPORTED;
  }
  Status = ReadSingleString (Fdt, PhyNode, "nvmem-cell-names", "intr_cal");
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "nvmem-cell-masks", &Config->NvmemCellMask);
  if (EFI_ERROR (Status) || (Config->NvmemCellMask != T70_INTR_CAL_MASK)) {
    return EFI_UNSUPPORTED;
  }
  Status = ReadBooleanProperty (Fdt, PhyNode, "mediatek,set-efuse-v1");
  if (EFI_ERROR (Status)) return Status;
  ProviderNode = FdtParentOffset (Fdt, CellNode);
  if ((ProviderNode < 0) ||
      (ProviderNode != FdtPathOffset (Fdt, T70_NVMEM_PROVIDER_NODE)) ||
      !NodeEnabled (Fdt, ProviderNode) ||
      !NodeHasCompatible (Fdt, ProviderNode, "mediatek,devinfo")) {
    return EFI_UNSUPPORTED;
  }
  Status = ReadExactReg (Fdt, ProviderNode, FdtParentOffset (Fdt, ProviderNode),
                         0, T70_DEVINFO_BASE, T70_DEVINFO_BYTES);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, ProviderNode, "#address-cells", &CellCount);
  if (EFI_ERROR (Status) || (CellCount != 1)) return EFI_UNSUPPORTED;
  Status = ReadU32Property (Fdt, ProviderNode, "#size-cells", &CellCount);
  if (EFI_ERROR (Status) || (CellCount != 1)) return EFI_UNSUPPORTED;
  Status = ReadReg (Fdt, CellNode, ProviderNode, 0, &Base, &Bytes);
  if (EFI_ERROR (Status) || (Base != T70_INTR_CAL_OFFSET) ||
      (Bytes != T70_INTR_CAL_BYTES)) return EFI_UNSUPPORTED;

  // These are DT-derived coordinates only. The external callback performs any
  // actual NVMEM read; this code never reads devinfo MMIO or logs raw values.
  Config->NvmemProviderBase = T70_DEVINFO_BASE;
  Config->NvmemProviderBytes = T70_DEVINFO_BYTES;
  Config->NvmemCellOffset = (UINT32)Base;
  Config->NvmemCellBytes = (UINT32)Bytes;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ValidatePhyNode (
  IN CONST VOID *Fdt,
  IN INT32 PhyNode,
  IN INT32 ExpectedParent,
  OUT T70_USB_PHY_DT_CONFIG *Config,
  OUT UINT32 *ClockPhandle
  )
{
  INT32 Parent;
  UINT32 Value;
  EFI_STATUS Status;
  Parent = FdtParentOffset (Fdt, PhyNode);
  if ((Parent < 0) || (Parent != ExpectedParent) ||
      !NodeEnabled (Fdt, Parent) ||
      !NodeHasCompatible (Fdt, Parent, "mediatek,generic-tphy-v2") ||
      !NodeEnabled (Fdt, PhyNode)) {
    return EFI_UNSUPPORTED;
  }
  Status = ReadExactReg (Fdt, PhyNode, Parent, 0, T70_PHY_BASE, T70_PHY_BYTES);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "#phy-cells", &Value);
  if (EFI_ERROR (Status) || (Value != 1)) return EFI_UNSUPPORTED;
  Status = ValidateClockTree (Fdt, PhyNode, ClockPhandle);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "mediatek,eye-vrt", &Config->Config.EyeVrt);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "mediatek,eye-term", &Config->Config.EyeTerm);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "mediatek,discth", &Config->Config.DiscTh);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "mediatek,rx-sqth", &Config->Config.RxSqth);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "mediatek,rev4", &Config->Config.Rev4);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadU32Property (Fdt, PhyNode, "mediatek,disc-host",
                            &Config->HostTuning.DisconnectThreshold);
  if (EFI_ERROR (Status)) return Status;
  Config->Config.TuningPresent = MTK_USB_TPHY_TUNING_T70_REQUIRED;
  Config->Config.Revision = MTK_USB_PHY_CORE_REVISION;
  Config->Config.PhyType = MTK_USB_TPHY_T70_U2_TYPE;
  Config->Config.TphyVersion = MTK_USB_TPHY_VERSION_V2;
  Config->Config.SetEfuseV1 = 1;
  Config->PhyBase = T70_PHY_BASE;
  Config->PhyBytes = T70_PHY_BYTES;
  if ((Config->Config.EyeVrt > 7U) ||
      (Config->Config.EyeTerm > 7U) ||
      (Config->Config.DiscTh > 15U) ||
      (Config->Config.RxSqth > 15U) ||
      (Config->Config.Rev4 > 1U) ||
      (Config->HostTuning.DisconnectThreshold > 15U)) {
    return EFI_UNSUPPORTED;
  }
  return ValidateNvmemTree (Fdt, PhyNode, Config);
}

STATIC EFI_STATUS
ValidateT70Tree (
  IN CONST VOID *Fdt,
  IN UINTN FdtBytes,
  OUT T70_USB_PHY_DT_CONFIG *Config,
  OUT UINT32 *RefClockPhandle
  )
{
  INT32 Soc, Usb, UsbParent, PhyBus, Phy, ResolvedPhy, Length;
  UINT32 AddressCells, SizeCells, PhyPhandle;
  CONST UINT8 *Phys;
  T70_USB_PHY_DT_CONFIG LocalConfig;
  EFI_STATUS Status;
  if ((Fdt == NULL) || (Config == NULL) || (RefClockPhandle == NULL) ||
      (FdtBytes < sizeof (FDT_HEADER)) || (FdtBytes > T70_MAX_FDT_BYTES) ||
      (FdtCheckFull (Fdt, FdtBytes) != 0) || (FdtTotalSize (Fdt) != FdtBytes)) {
    return EFI_COMPROMISED_DATA;
  }
  if (!NodeHasCompatible (Fdt, 0, "mediatek,MT6789")) return EFI_UNSUPPORTED;
  Soc = FdtPathOffset (Fdt, "/soc");
  Usb = FdtPathOffset (Fdt, T70_USB_NODE);
  if ((Soc < 0) || (Usb < 0) || !NodeEnabled (Fdt, Soc) ||
      !NodeEnabled (Fdt, Usb) ||
      !NodeHasCompatible (Fdt, Usb, "mediatek,mt6789-usb20")) return EFI_UNSUPPORTED;
  Status = ReadU32Property (Fdt, Soc, "#address-cells", &AddressCells);
  if (EFI_ERROR (Status) || (AddressCells != 2)) return EFI_UNSUPPORTED;
  Status = ReadU32Property (Fdt, Soc, "#size-cells", &SizeCells);
  if (EFI_ERROR (Status) || (SizeCells != 2)) return EFI_UNSUPPORTED;
  Status = ReadExactReg (Fdt, Usb, Soc, 0, T70_USB_BASE, T70_USB_BYTES);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadExactReg (Fdt, Usb, Soc, 1, T70_PHY_BASE, 0x10000ULL);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadSingleString (Fdt, Usb, "dr_mode", "otg");
  if (EFI_ERROR (Status)) return Status;
  Phys = FdtGetProp (Fdt, Usb, "phys", &Length);
  if ((Phys == NULL) || (Length != (2 * (INT32)sizeof (UINT32)))) {
    return EFI_UNSUPPORTED;
  }
  PhyPhandle = ReadBe32 (Phys);
  if (ReadBe32 (Phys + sizeof (UINT32)) != MTK_USB_TPHY_T70_U2_TYPE) {
    return EFI_UNSUPPORTED;
  }
  // Use canonical unit-address paths. libfdt also accepts an omitted unit
  // address; that spelling resolves to this same node, not a second bus.
  PhyBus = FdtPathOffset (Fdt, T70_PHY_BUS);
  Phy = FdtPathOffset (Fdt, T70_PHY_NODE);
  if ((PhyBus < 0) || (Phy < 0) ||
      (FdtParentOffset (Fdt, PhyBus) != Soc) ||
      !NodeEnabled (Fdt, PhyBus) ||
      !NodeHasCompatible (Fdt, PhyBus, "mediatek,generic-tphy-v2")) {
    return EFI_UNSUPPORTED;
  }
  Status = ReadU32Property (Fdt, PhyBus, "#address-cells", &AddressCells);
  if (EFI_ERROR (Status) || (AddressCells != 2)) return EFI_UNSUPPORTED;
  Status = ReadU32Property (Fdt, PhyBus, "#size-cells", &SizeCells);
  if (EFI_ERROR (Status) || (SizeCells != 2)) return EFI_UNSUPPORTED;
  if ((FdtGetProp (Fdt, PhyBus, "ranges", &Length) == NULL) || (Length != 0)) {
    return EFI_UNSUPPORTED;
  }
  ResolvedPhy = FdtNodeOffsetByPhandle (Fdt, PhyPhandle);
  if (ResolvedPhy != Phy) return EFI_UNSUPPORTED;
  ZeroMem (&LocalConfig, sizeof (LocalConfig));
  Status = ValidatePhyNode (Fdt, Phy, PhyBus, &LocalConfig, RefClockPhandle);
  if (EFI_ERROR (Status)) return Status;
  LocalConfig.HostTuning.SlewControl = 0;
  LocalConfig.HostTuning.EyeVrt = 0;
  LocalConfig.HostTuning.EyeTerm = 0;
  LocalConfig.HostTuning.Rev6 = 0;
  UsbParent = FdtParentOffset (Fdt, Usb);
  if (UsbParent != Soc) return EFI_UNSUPPORTED;
  *Config = LocalConfig;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ValidatePhyGcdMapping (IN UINT64 Base, IN UINT64 Bytes)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  UINT64 Offset;
  EFI_STATUS Status;
  if (gDS == NULL) return EFI_NOT_READY;
  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) return Status;
  if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
      (Base < Descriptor.BaseAddress)) return EFI_UNSUPPORTED;
  Offset = Base - Descriptor.BaseAddress;
  if ((Offset > Descriptor.Length) || (Bytes > Descriptor.Length - Offset) ||
      ((Descriptor.Attributes & EFI_MEMORY_UC) == 0) ||
      ((Descriptor.Attributes & (EFI_MEMORY_RO | EFI_MEMORY_RP)) != 0)) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC UINT8
ClockBit (IN MTK_USB_PHY_CLOCK_SLOT Slot)
{
  return (UINT8)(1U << (UINT32)Slot);
}

STATIC MTK_USB_PHY_STATUS
ResolveClock (IN VOID *Context, IN MTK_USB_PHY_CLOCK_SLOT Slot, OUT UINT8 *Present)
{
  if ((Context != &mT70Phy) || !mT70Phy.Active || (Present == NULL) ||
      ((UINT32)Slot >= (UINT32)MtkUsbPhyClockCount)) return MtkUsbPhyInvalid;
  *Present = ((mT70Phy.ClockPresentMask & ClockBit (Slot)) != 0) ? 1U : 0U;
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
PrepareClock (IN VOID *Context, IN MTK_USB_PHY_CLOCK_SLOT Slot)
{
  UINT8 Bit;
  if ((Context != &mT70Phy) || !mT70Phy.Active ||
      ((UINT32)Slot >= (UINT32)MtkUsbPhyClockCount)) return MtkUsbPhyInvalid;
  Bit = ClockBit (Slot);
  if ((mT70Phy.ClockPresentMask & Bit) == 0) return MtkUsbPhyNotReady;
  if ((mT70Phy.ClockPreparedMask & Bit) != 0) return MtkUsbPhyBusy;
  // MTK_CLOCK_PROTOCOL has no prepare API; the fixed DT ref clock is always on.
  mT70Phy.ClockPreparedMask |= Bit;
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
EnableClock (IN VOID *Context, IN MTK_USB_PHY_CLOCK_SLOT Slot)
{
  UINT8 Bit;
  EFI_STATUS Status;
  UINT64 Frequency;
  BOOLEAN Enabled;
  if ((Context != &mT70Phy) || !mT70Phy.Active ||
      ((UINT32)Slot >= (UINT32)MtkUsbPhyClockCount)) return MtkUsbPhyInvalid;
  Bit = ClockBit (Slot);
  if (((mT70Phy.ClockPresentMask & Bit) == 0) ||
      ((mT70Phy.ClockPreparedMask & Bit) == 0) ||
      ((mT70Phy.ClockEnabledMask & Bit) != 0)) return MtkUsbPhyClockError;
  // The sole DT clock is the inherited, always-on TOP_CLK26M. Acquire a
  // logical reference only; never gate the parent's clock during USB cleanup.
  Status = mT70Phy.Clock->GetFrequency (mT70Phy.ClockId[Slot], &Frequency);
  if (EFI_ERROR (Status) || (Frequency != T70_REF_CLOCK_HZ))
    return MtkUsbPhyClockError;
  Status = mT70Phy.Clock->IsEnabled (mT70Phy.ClockId[Slot], &Enabled);
  if (EFI_ERROR (Status) || !Enabled) return MtkUsbPhyClockError;
  mT70Phy.ClockEnabledMask |= Bit;
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
DisableClock (IN VOID *Context, IN MTK_USB_PHY_CLOCK_SLOT Slot)
{
  UINT8 Bit;
  if ((Context != &mT70Phy) || !mT70Phy.Active ||
      ((UINT32)Slot >= (UINT32)MtkUsbPhyClockCount)) return MtkUsbPhyInvalid;
  Bit = ClockBit (Slot);
  if (((mT70Phy.ClockPresentMask & Bit) == 0) ||
      ((mT70Phy.ClockEnabledMask & Bit) == 0)) return MtkUsbPhyClockError;
  // Drop this consumer reference without changing the inherited clock.
  mT70Phy.ClockEnabledMask &= (UINT8)~Bit;
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
UnprepareClock (IN VOID *Context, IN MTK_USB_PHY_CLOCK_SLOT Slot)
{
  UINT8 Bit;
  if ((Context != &mT70Phy) || !mT70Phy.Active ||
      ((UINT32)Slot >= (UINT32)MtkUsbPhyClockCount)) return MtkUsbPhyInvalid;
  Bit = ClockBit (Slot);
  if (((mT70Phy.ClockPresentMask & Bit) == 0) ||
      ((mT70Phy.ClockPreparedMask & Bit) == 0) ||
      ((mT70Phy.ClockEnabledMask & Bit) != 0)) return MtkUsbPhyClockError;
  mT70Phy.ClockPreparedMask &= (UINT8)~Bit;
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
ReadPhy32 (IN VOID *Context, IN UINT32 Offset, OUT UINT32 *Value)
{
  if ((Context != &mT70Phy) || !mT70Phy.Active || (Value == NULL) ||
      ((Offset & 3U) != 0) || ((UINT64)Offset + sizeof (UINT32) > mT70Phy.PhyBytes)) {
    return MtkUsbPhyIoError;
  }
  *Value = MmioRead32 ((UINTN)(mT70Phy.PhyBase + Offset));
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
WritePhy32 (IN VOID *Context, IN UINT32 Offset, IN UINT32 Value)
{
  if ((Context != &mT70Phy) || !mT70Phy.Active ||
      ((Offset & 3U) != 0) || ((UINT64)Offset + sizeof (UINT32) > mT70Phy.PhyBytes)) {
    return MtkUsbPhyIoError;
  }
  MmioWrite32 ((UINTN)(mT70Phy.PhyBase + Offset), Value);
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
ReadPhyNvmemCell (
  IN VOID *Context,
  IN CONST CHAR8 *Name,
  IN UINT32 Mask,
  OUT UINT32 *RawValue
  )
{
  MTK_USB_PHY_STATUS Status;
  if ((Context != &mT70Phy) || !mT70Phy.Active ||
      (Name == NULL) || (RawValue == NULL)) return MtkUsbPhyInvalid;
  *RawValue = 0;
  if ((AsciiStrCmp (Name, "intr_cal") != 0) ||
      (Mask != mT70Phy.NvmemCellMask)) return MtkUsbPhyUnsupported;
  if (mT70Phy.ReadNvmem == NULL) return MtkUsbPhyNotReady;
  Status = mT70Phy.ReadNvmem (
             mT70Phy.NvmemContext, "intr_cal",
             mT70Phy.NvmemCellMask, RawValue);
  if (Status != MtkUsbPhySuccess) *RawValue = 0;
  return Status;
}

STATIC MTK_USB_PHY_STATUS
NowNs (IN VOID *Context, OUT uint64_t *Value)
{
  if ((Context != &mT70Phy) || !mT70Phy.Active || (Value == NULL)) {
    return MtkUsbPhyInvalid;
  }
  *Value = GetTimeInNanoSecond (GetPerformanceCounter ());
  return MtkUsbPhySuccess;
}

STATIC MTK_USB_PHY_STATUS
DelayUs (IN VOID *Context, IN UINT32 Microseconds)
{
  if ((Context != &mT70Phy) || !mT70Phy.Active) return MtkUsbPhyInvalid;
  MicroSecondDelay (Microseconds);
  return MtkUsbPhySuccess;
}

STATIC EFI_STATUS
ValidateFixedRefClock (IN MTK_CLOCK_PROTOCOL *Clock, OUT UINT32 *ClockId)
{
  CONST CHAR8 *Name;
  UINT64 Frequency;
  BOOLEAN Enabled;
  EFI_STATUS Status;
  if ((Clock == NULL) || (ClockId == NULL) ||
      (Clock->GetId == NULL) || (Clock->GetName == NULL) ||
      (Clock->IsEnabled == NULL) ||
      (Clock->GetFrequency == NULL)) return EFI_NOT_READY;
  Status = Clock->GetId (T70_REF_CLOCK_NAME, ClockId);
  if (EFI_ERROR (Status)) return Status;
  Status = Clock->GetName (*ClockId, &Name);
  if (EFI_ERROR (Status)) return Status;
  if ((Name == NULL) || (AsciiStrCmp (Name, T70_REF_CLOCK_NAME) != 0)) {
    return EFI_UNSUPPORTED;
  }
  Status = Clock->GetFrequency (*ClockId, &Frequency);
  if (EFI_ERROR (Status)) return Status;
  if (Frequency != T70_REF_CLOCK_HZ) return EFI_UNSUPPORTED;
  Status = Clock->IsEnabled (*ClockId, &Enabled);
  if (EFI_ERROR (Status)) return Status;
  return Enabled ? EFI_SUCCESS : EFI_NOT_READY;
}

EFI_STATUS
MtkUsbPhyPlatformBind (
  IN CONST VOID *Fdt,
  IN UINTN FdtBytes,
  IN MTK_CLOCK_PROTOCOL *Clock,
  IN MTK_USB_PHY_PLATFORM_NVMEM_READ ReadNvmem,
  IN VOID *NvmemContext,
  OUT MTK_USB_PHY_IO *Io,
  OUT MTK_USB_PHY_CONFIG *Config,
  OUT MTK_USB_PHY_HOST_TUNING *HostTuning
  )
{
  T70_USB_PHY_DT_CONFIG Dt;
  MTK_USB_PHY_IO LocalIo;
  UINT32 RefClockPhandle, ClockId, ProbeValue;
  MTK_USB_PHY_STATUS NvmemResult;
  BOOLEAN UseStockNvmem;
  EFI_STATUS Status;
  if ((Io == NULL) || (Config == NULL) || (HostTuning == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Io, sizeof (*Io));
  ZeroMem (Config, sizeof (*Config));
  ZeroMem (HostTuning, sizeof (*HostTuning));
  if (mT70Phy.Active) return EFI_ALREADY_STARTED;
  ZeroMem (&Dt, sizeof (Dt));
  Status = ValidateT70Tree (Fdt, FdtBytes, &Dt, &RefClockPhandle);
  if (EFI_ERROR (Status)) return Status;
  UseStockNvmem = (BOOLEAN)(ReadNvmem == NULL);
  if (UseStockNvmem) {
    NvmemResult = ReadBootIntrCal(Fdt,FdtBytes,&ProbeValue);
    if (NvmemResult != MtkUsbPhySuccess)
      return NvmemResult == MtkUsbPhyNotReady ? EFI_NOT_READY : EFI_COMPROMISED_DATA;
    ReadNvmem = ReadStockNvmem;
    NvmemContext = &mT70Phy;
  }
  Status = ValidateFixedRefClock (Clock, &ClockId);
  if (EFI_ERROR (Status)) return Status;
  Status = ValidatePhyGcdMapping (Dt.PhyBase, Dt.PhyBytes);
  if (EFI_ERROR (Status)) return Status;

  ZeroMem (&mT70Phy, sizeof (mT70Phy));
  mT70Phy.BootFdt = UseStockNvmem ? Fdt : NULL;
  mT70Phy.BootFdtBytes = UseStockNvmem ? FdtBytes : 0;
  mT70Phy.Clock = Clock;
  mT70Phy.ReadNvmem = ReadNvmem;
  mT70Phy.NvmemContext = NvmemContext;
  mT70Phy.PhyBase = Dt.PhyBase;
  mT70Phy.PhyBytes = Dt.PhyBytes;
  mT70Phy.NvmemProviderBase = Dt.NvmemProviderBase;
  mT70Phy.NvmemProviderBytes = Dt.NvmemProviderBytes;
  mT70Phy.NvmemCellOffset = Dt.NvmemCellOffset;
  mT70Phy.NvmemCellBytes = Dt.NvmemCellBytes;
  mT70Phy.NvmemCellMask = Dt.NvmemCellMask;
  mT70Phy.ClockId[MtkUsbPhyClockRef] = ClockId;
  mT70Phy.ClockPresentMask = (UINT8)(1U << MtkUsbPhyClockRef);
  mT70Phy.Active = TRUE;

  ZeroMem (&LocalIo, sizeof (LocalIo));
  LocalIo.Context = &mT70Phy;
  LocalIo.PhyBase = Dt.PhyBase;
  LocalIo.PhyLength = Dt.PhyBytes;
  LocalIo.Read32 = ReadPhy32;
  LocalIo.Write32 = WritePhy32;
  LocalIo.NowNs = NowNs;
  LocalIo.DelayUs = DelayUs;
  LocalIo.ResolveClock = ResolveClock;
  LocalIo.ClockPrepare = PrepareClock;
  LocalIo.ClockEnable = EnableClock;
  LocalIo.ClockDisable = DisableClock;
  LocalIo.ClockUnprepare = UnprepareClock;
  LocalIo.ReadNvmemCell = ReadPhyNvmemCell;
  *Io = LocalIo;
  *Config = Dt.Config;
  *HostTuning = Dt.HostTuning;
  (VOID)RefClockPhandle;
  return EFI_SUCCESS;
}

/*
 * Caller invokes Unbind only after successful MtkUsbPhyCoreExit. It does not
 * modify clock or PHY hardware and refuses to discard callback ownership.
 */
EFI_STATUS
MtkUsbPhyPlatformUnbind (VOID)
{
  if (!mT70Phy.Active) return EFI_NOT_STARTED;
  if ((mT70Phy.ClockPreparedMask != 0) ||
      (mT70Phy.ClockEnabledMask != 0)) return EFI_NOT_READY;
  ZeroMem (&mT70Phy, sizeof (mT70Phy));
  return EFI_SUCCESS;
}
