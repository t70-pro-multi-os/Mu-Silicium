// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/MtkDisplayCore.h>

#define MTK_PAGE_MASK (EFI_PAGE_SIZE - 1)

STATIC
UINT32
ReadLe32 (
  IN CONST UINT8 *Bytes
  )
{
  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

EFI_STATUS
EFIAPI
MtkDisplayDecodeLkOvl (
  IN UINT32 ControllerBase,
  IN CONST VOID *RegisterPage,
  IN UINTN RegisterPageSize,
  OUT MTK_DISPLAY_OVL_PLANE *Plane
  )
{
  CONST UINT8 *Bytes = RegisterPage;
  MTK_DISPLAY_OVL_PLANE Decoded;
  UINT32 LayerCount;
  UINT32 Source;
  UINT32 Roi;
  UINT32 Layer;
  UINT32 Offset;
  UINT32 Control;
  UINT32 Pitch;
  UINT64 Size;

  if ((RegisterPage == NULL) || (Plane == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (RegisterPageSize != 0x1000) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (ControllerBase == 0x14005000) {
    LayerCount = 4;
  } else if (ControllerBase == 0x14006000) {
    LayerCount = 2;
  } else {
    return EFI_UNSUPPORTED;
  }
  if ((ReadLe32 (Bytes + 0x0C) & 1U) == 0) {
    return EFI_NOT_READY;
  }
  Source = ReadLe32 (Bytes + 0x2C);
  if (Source == 0) {
    return EFI_NOT_READY;
  }
  if ((Source >= (1U << LayerCount)) || ((Source & (Source - 1U)) != 0) ||
      ((ReadLe32 (Bytes + 0x24) & 0xF0U) != 0) ||
      (ReadLe32 (Bytes + 0x324) != 0)) {
    return EFI_UNSUPPORTED;
  }
  for (Layer = 0; (Source & (1U << Layer)) == 0; ++Layer) {
  }
  Offset = Layer * 0x20;
  Control = ReadLe32 (Bytes + 0x30 + Offset);
  Pitch = ReadLe32 (Bytes + 0x44 + Offset);
  // Exact LK 0x50D0C writes format 0x01002002 as L_CON format 2, no swap.
  if (((Control & ~0x1FFU) != 0x2000U) ||
      (ReadLe32 (Bytes + 0x3C + Offset) != 0) ||
      (ReadLe32 (Bytes + 0x4C + Offset) != 0) ||
      // +0x40 is PITCH_MSB, not the source key (+0x34). On this exact
      // linear LK plane bit20 reads as one; retain that reset field while
      // rejecting every other unimplemented high-pitch/subbuffer setting.
      ((ReadLe32 (Bytes + 0x40 + Offset) & ~0x00100000U) != 0) ||
      ((ReadLe32 (Bytes + 0x2D0) & (0xFU << (Layer * 4))) != 0) ||
      ((Pitch & ~0x1000FFFFU) != 0)) {
    return EFI_UNSUPPORTED;
  }
  // RDMA_CTRL bit0 enables the layer. Upper readback includes FIFO used:
  // shipping DRM mtk_ovl_analysis +0x339BC extracts bits27:16 separately
  // and +0x339CC tests bit0. Live LK reads 0x005A0001, not literal 1.
  if ((ReadLe32 (Bytes + 0xC0 + Offset) & 1U) == 0) {
    return EFI_NOT_READY;
  }
  Roi = ReadLe32 (Bytes + 0x20);
  if (((Roi & 0xF000F000U) != 0) ||
      (ReadLe32 (Bytes + 0x38 + Offset) != Roi)) {
    return EFI_COMPROMISED_DATA;
  }
  ZeroMem (&Decoded, sizeof (Decoded));
  Decoded.DmaAddress = ReadLe32 (Bytes + 0xF40 + Offset);
  Decoded.Width = Roi & 0xFFFU;
  Decoded.Height = Roi >> 16;
  Decoded.StrideBytes = Pitch & 0xFFFFU;
  Decoded.LayerIndex = Layer;
  Decoded.AlphaControl = Control & 0x1FFU;
  Decoded.Opaque = (Decoded.AlphaControl == 0x1FFU) &&
                   ((Pitch & 0x10000000U) != 0);
  Pitch = Decoded.StrideBytes;
  Size = (UINT64)Pitch * Decoded.Height;
  if ((Decoded.DmaAddress == 0) || ((Decoded.DmaAddress & 3U) != 0) ||
      (Decoded.Width == 0) || (Decoded.Height == 0) ||
      ((Pitch & 3U) != 0) || (Pitch < Decoded.Width * 4U) ||
      (Size > 0x100000000ULL - Decoded.DmaAddress)) {
    return EFI_COMPROMISED_DATA;
  }
  CopyMem (Plane, &Decoded, sizeof (*Plane));
  return EFI_SUCCESS;
}

STATIC
UINT64
ReadCells (
  IN CONST UINT8 *Data,
  IN INT32 Cells
  )
{
  UINT64 Value = 0;
  INT32 Index;
  for (Index = 0; Index < Cells * 4; ++Index) {
    Value = (Value << 8) | Data[Index];
  }
  return Value;
}

STATIC
BOOLEAN
RangeContains (
  IN UINT64 OuterBase,
  IN UINT64 OuterSize,
  IN UINT64 InnerBase,
  IN UINT64 InnerSize
  )
{
  if ((OuterSize == 0) || (InnerSize == 0) ||
      (OuterBase > MAX_UINT64 - OuterSize) ||
      (InnerBase > MAX_UINT64 - InnerSize) ||
      (InnerBase < OuterBase)) {
    return FALSE;
  }
  return (InnerBase - OuterBase <= OuterSize) &&
         (InnerSize <= OuterSize - (InnerBase - OuterBase));
}

STATIC
BOOLEAN
RangesOverlap (
  IN UINT64 ABase,
  IN UINT64 ASize,
  IN UINT64 BBase,
  IN UINT64 BSize
  )
{
  if ((ASize == 0) || (BSize == 0) ||
      (ABase > MAX_UINT64 - ASize) ||
      (BBase > MAX_UINT64 - BSize)) {
    return FALSE;
  }
  return (ABase < BBase + BSize) && (BBase < ABase + ASize);
}

STATIC
EFI_STATUS
ReadReg (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  IN INT32 Parent,
  OUT UINT64 *Base,
  OUT UINT64 *Size
  )
{
  INT32 AddressCells;
  INT32 SizeCells;
  INT32 Length;
  INT32 TupleBytes;
  CONST UINT8 *Reg;

  AddressCells = FdtAddressCells (Fdt, Parent);
  SizeCells = FdtSizeCells (Fdt, Parent);
  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 1) || (SizeCells > 2)) {
    return EFI_UNSUPPORTED;
  }

  TupleBytes = (AddressCells + SizeCells) * sizeof (UINT32);
  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if ((Reg == NULL) || (Length != TupleBytes)) {
    return EFI_COMPROMISED_DATA;
  }

  *Base = ReadCells (Reg, AddressCells);
  *Size = ReadCells (Reg + AddressCells * sizeof (UINT32), SizeCells);
  if ((*Size == 0) || (*Base > MAX_UINT64 - *Size)) {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

// A reserved-memory pool may describe several disjoint physical ranges.
// Validate every tuple before accepting one that contains the whole scanout.
STATIC
EFI_STATUS
CheckRegContains (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  IN INT32 Parent,
  IN UINT64 Base,
  IN UINT64 Size
  )
{
  INT32 AddressCells = FdtAddressCells (Fdt, Parent);
  INT32 SizeCells = FdtSizeCells (Fdt, Parent);
  INT32 Length;
  INT32 TupleBytes;
  CONST UINT8 *Reg;
  BOOLEAN Contains = FALSE;

  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 1) || (SizeCells > 2)) {
    return EFI_UNSUPPORTED;
  }
  TupleBytes = (AddressCells + SizeCells) * sizeof (UINT32);
  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if ((Reg == NULL) || (Length <= 0) || (Length % TupleBytes != 0)) {
    return EFI_COMPROMISED_DATA;
  }
  for (INT32 Offset = 0; Offset < Length; Offset += TupleBytes) {
    UINT64 ReserveBase = ReadCells (Reg + Offset, AddressCells);
    UINT64 ReserveSize = ReadCells (Reg + Offset + AddressCells * sizeof (UINT32), SizeCells);
    if ((ReserveSize == 0) || (ReserveBase > MAX_UINT64 - ReserveSize)) {
      return EFI_COMPROMISED_DATA;
    }
    Contains = Contains || RangeContains (ReserveBase, ReserveSize, Base, Size);
  }
  return Contains ? EFI_SUCCESS : EFI_ACCESS_DENIED;
}

STATIC
EFI_STATUS
IsNodeEnabled (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  OUT BOOLEAN *Enabled
  )
{
  INT32 Length;
  CONST CHAR8 *Status;

  *Enabled = TRUE;
  Status = FdtGetProp (Fdt, Node, "status", &Length);
  if (Status == NULL) {
    return EFI_SUCCESS;
  }
  if ((Length == sizeof ("okay")) && (CompareMem (Status, "okay", Length) == 0)) {
    return EFI_SUCCESS;
  }
  if ((Length == sizeof ("ok")) && (CompareMem (Status, "ok", Length) == 0)) {
    return EFI_SUCCESS;
  }
  if ((Length == sizeof ("disabled")) && (CompareMem (Status, "disabled", Length) == 0)) {
    *Enabled = FALSE;
    return EFI_SUCCESS;
  }
  return EFI_COMPROMISED_DATA;
}

STATIC
EFI_STATUS
CheckReservedRange (
  IN CONST VOID *Fdt,
  IN UINT64 Base,
  IN UINT64 Size
  )
{
  INT32 Entries;
  INT32 Index;
  INT32 Root;
  INT32 Length;
  INT32 Node;
  UINT64 ReserveBase;
  UINT64 ReserveSize;
  CONST VOID *Ranges;
  BOOLEAN Enabled;
  EFI_STATUS Status;

  Entries = FdtGetNumberOfReserveMapEntries (Fdt);
  if (Entries < 0) {
    return EFI_COMPROMISED_DATA;
  }
  for (Index = 0; Index < Entries; ++Index) {
    if (FdtGetReserveMapEntry (Fdt, Index, &ReserveBase, &ReserveSize) != 0) {
      return EFI_COMPROMISED_DATA;
    }
    if (RangeContains (ReserveBase, ReserveSize, Base, Size)) {
      return EFI_SUCCESS;
    }
  }

  Root = FdtPathOffset (Fdt, "/reserved-memory");
  if (Root < 0) {
    return EFI_ACCESS_DENIED;
  }
  Status = IsNodeEnabled (Fdt, Root, &Enabled);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (!Enabled) {
    return EFI_ACCESS_DENIED;
  }
  Ranges = FdtGetProp (Fdt, Root, "ranges", &Length);
  if ((Ranges == NULL) || (Length != 0)) {
    return EFI_UNSUPPORTED;
  }

  FdtForEachSubnode (Node, Fdt, Root) {
    Status = IsNodeEnabled (Fdt, Node, &Enabled);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (!Enabled) {
      continue;
    }
    if (FdtGetProp (Fdt, Node, "reg", &Length) == NULL) {
      INT32 SizeCells = FdtSizeCells (Fdt, Root);
      CONST UINT8 *Request = FdtGetProp (Fdt, Node, "size", &Length);
      if ((SizeCells < 1) || (SizeCells > 2)) {
        return EFI_UNSUPPORTED;
      }
      if ((Request == NULL) || (Length != SizeCells * sizeof (UINT32)) ||
          (ReadCells (Request, SizeCells) == 0)) {
        return EFI_COMPROMISED_DATA;
      }
      // An OS allocation request cannot establish a physical framebuffer range.
      continue;
    }
    Status = CheckRegContains (Fdt, Node, Root, Base, Size);
    if (Status != EFI_ACCESS_DENIED) {
      return Status;
    }
  }
  return EFI_ACCESS_DENIED;
}

STATIC
EFI_STATUS
FindSimpleFramebuffer (
  IN CONST VOID *Fdt,
  OUT INT32 *FramebufferNode,
  OUT INT32 *ChosenNode
  )
{
  INT32 Chosen;
  INT32 Node;
  INT32 Length;
  INT32 Found = -1;
  CONST CHAR8 *Compatible;
  BOOLEAN Enabled;
  EFI_STATUS Status;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    return EFI_NOT_FOUND;
  }
  FdtForEachSubnode (Node, Fdt, Chosen) {
    Compatible = FdtGetProp (Fdt, Node, "compatible", &Length);
    if ((Compatible == NULL) ||
        (FdtStringListContains (Compatible, Length, "simple-framebuffer") != 1)) {
      continue;
    }
    Status = IsNodeEnabled (Fdt, Node, &Enabled);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (!Enabled) {
      continue;
    }
    if (Found >= 0) {
      return EFI_UNSUPPORTED;
    }
    Found = Node;
  }
  if (Found < 0) {
    return EFI_NOT_FOUND;
  }
  *FramebufferNode = Found;
  *ChosenNode = Chosen;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReadFormat (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  OUT EFI_GRAPHICS_PIXEL_FORMAT *Format
  )
{
  INT32 Length;
  CONST CHAR8 *Value;

  Value = FdtGetProp (Fdt, Node, "format", &Length);
  if (Value == NULL) {
    return EFI_COMPROMISED_DATA;
  }
  /* B,G,R,X bytes on little-endian ARM64 map directly to UEFI BLT pixels. */
  if ((Length != sizeof ("x8r8g8b8")) ||
      (CompareMem (Value, "x8r8g8b8", sizeof ("x8r8g8b8")) != 0)) {
    return EFI_UNSUPPORTED;
  }
  *Format = PixelBlueGreenRedReserved8BitPerColor;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReadU32 (
  IN CONST VOID *Fdt,
  IN INT32 Node,
  IN CONST CHAR8 *Name,
  OUT UINT32 *Value
  )
{
  INT32 Length;
  CONST UINT8 *Property = FdtGetProp (Fdt, Node, Name, &Length);
  if ((Property == NULL) || (Length != sizeof (UINT32))) {
    return EFI_COMPROMISED_DATA;
  }
  *Value = (UINT32)ReadCells (Property, 1);
  return EFI_SUCCESS;
}

// Exact stock DT resources; never use a DT-supplied arbitrary MMIO address.
STATIC
EFI_STATUS
CheckLkHardwareNodes (IN CONST VOID *Fdt)
{
  STATIC CONST struct { CONST CHAR8 *Compatible; UINT32 Base; } Nodes[] = {
    {"mediatek,mt6789-scpsys", 0x10006000},
    {"mediatek,mt6789-infracfg_ao", 0x10001000},
    {"mediatek,mt6789-mmsys", 0x14000000},
    {"mediatek,smi_larb0", 0x14003000},
    {"mediatek,smi_larb1", 0x14004000},
    {"mediatek,disp_ovl0", 0x14005000},
    {"mediatek,disp_ovl0_2l", 0x14006000},
    {"mediatek,disp_rdma0", 0x14007000},
    {"mediatek,dsi0", 0x14013000}
  };
  INT32 Ancestors[32];
  INT32 Depth = -1;
  INT32 Node, Length, CompatibleLength, Match;
  UINT32 Found = 0;
  CONST VOID *Compatible, *Ranges;
  UINT64 Base, Size;
  BOOLEAN Enabled;
  EFI_STATUS Status;

  // Parent lookups rescan the whole tree. Keep the current ancestor path
  // during one traversal so the full shipping DTB fits the early boot budget.
  for (Node = FdtNextNode (Fdt, -1, &Depth); Node >= 0 && Depth >= 0;
       Node = FdtNextNode (Fdt, Node, &Depth)) {
    if (Depth >= (INT32)ARRAY_SIZE (Ancestors)) return EFI_UNSUPPORTED;
    Ancestors[Depth] = Node;
    Compatible = FdtGetProp (Fdt, Node, "compatible", &CompatibleLength);
    if (Compatible == NULL) continue;
    for (UINTN Index = 0; Index < ARRAY_SIZE (Nodes); Index++) {
      Match = FdtStringListContains (Compatible, CompatibleLength, Nodes[Index].Compatible);
      if (Match < 0) return EFI_COMPROMISED_DATA;
      if (Match == 0) continue;
      if (Depth == 0 || (Found & (1U << Index)) != 0) return EFI_UNSUPPORTED;
      Status = ReadReg (Fdt, Node, Ancestors[Depth - 1], &Base, &Size);
      if (EFI_ERROR (Status) || Base != Nodes[Index].Base || Size != 0x1000) {
        return EFI_UNSUPPORTED;
      }
      for (INT32 Level = Depth; Level > 0; Level--) {
        Status = IsNodeEnabled (Fdt, Ancestors[Level], &Enabled);
        if (EFI_ERROR (Status) || !Enabled) return EFI_NOT_READY;
        if (Level < Depth) {
          Ranges = FdtGetProp (Fdt, Ancestors[Level], "ranges", &Length);
          if (Ranges == NULL || Length != 0) return EFI_UNSUPPORTED;
        }
      }
      Found |= 1U << Index;
    }
  }
  return Found == (1U << ARRAY_SIZE (Nodes)) - 1 ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
ReadLkAccessState (
  IN MTK_DISPLAY_READ32 Read32,
  IN VOID *Context,
  OUT UINT32 *Controller
  )
{
  // These SPM/INFRA registers remain accessible while DISP is powered down.
  STATIC CONST struct { UINTN Address; UINT32 Mask; UINT32 Value; } Guards[] = {
    {0x1000616C, 0x200000, 0x200000},
    {0x10006170, 0x200000, 0x200000},
    {0x10006354, 0x111F, 0x000D},
    {0x100012D0, 0x1400, 0}, {0x100012EC, 0x1400, 0},
    {0x10001220, 0x800040, 0}, {0x10001228, 0x800040, 0},
    // Exact clk-mt6789-mmsys gates: APB, RDMA, SMI and DSI domains.
    {0x14000100, 0x014A080A, 0}, {0x140001A0, 1, 0},
    {0x14007010, 3, 1}, // RDMA enabled, direct-link (not memory) mode.
    {0x14013000, 1, 1}, // DSI video stream started by LK.
  };
  UINT32 Value;
  UINT32 Ovl0;
  UINT32 Ovl2l;
  UINT32 Input;
  UINT32 Sout;
  UINT32 Dsi;
  UINT32 Dither;
  UINT32 Clock;
  UINT32 Larb;
  UINT32 Port;
  EFI_STATUS Status;

  for (UINTN Index = 0; Index < ARRAY_SIZE (Guards); ++Index) {
    Status = Read32 (Context, Guards[Index].Address, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    if ((Value & Guards[Index].Mask) != Guards[Index].Value) {
      return EFI_NOT_READY;
    }
  }
  Status = Read32 (Context, 0x14013014, &Value);
  if (EFI_ERROR (Status)) { return Status; }
  if ((Value & 3U) == 0) { return EFI_UNSUPPORTED; } // No command-mode refresh ownership.

  // Exact LK ddp_path tables/connect code: select one unscaled OVL -> RDMA.
  Status = Read32 (Context, 0x14000F18, &Ovl0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Read32 (Context, 0x14000F14, &Ovl2l);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Read32 (Context, 0x14000F28, &Input);
  if (EFI_ERROR (Status)) { return Status; }
  if (((Ovl0 & 7U) == 1) && (Input == 0)) {
    *Controller = 0x14005000; Larb = 0x14003000; Port = 1;
  } else if (((Ovl2l & 7U) == 1) && ((Ovl0 & 7U) == 0) && (Input == 2)) {
    *Controller = 0x14006000; Larb = 0x14004000; Port = 0;
  } else {
    return EFI_UNSUPPORTED;
  }
  Status = Read32 (Context, 0x14000100, &Clock);
  if (EFI_ERROR (Status)) { return Status; }
  if ((Clock & (*Controller == 0x14005000 ? 4U : 0x10U)) != 0) {
    return EFI_NOT_READY;
  }
  Status = Read32 (Context, 0x14000F0C, &Sout);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Read32 (Context, 0x14000F30, &Dsi);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Read32 (Context, 0x14000F20, &Dither);
  if (EFI_ERROR (Status)) { return Status; }
  if (!(((Sout == 0) && (Dsi == 0)) ||
        ((Sout == 1) && (Dsi == 1) && ((Dither & 7U) == 1)))) {
    return EFI_UNSUPPORTED;
  }
  if ((Sout == 1) && ((Clock & 0x16700U) != 0)) {
    return EFI_NOT_READY;
  }
  // Both the pixel and header port must be physical. No IOVA is guessed.
  for (UINTN Index = 0; Index < 2; ++Index) {
    Status = Read32 (Context, Larb + 0x380 + (Port + Index) * 4, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    if ((Value & 1U) != 0) { return EFI_UNSUPPORTED; }
  }
  return EFI_SUCCESS;
}

// Original LK DSI_config (0x4ab34) programs these controls/timings before
// start. The handoff accepts only its four-lane, uncompressed RGB888 stream.
// Read controls, not status counters, so two equal snapshots can be compared.
typedef struct {
  UINT32 Reg[12];
} LK_DSI_STATE;

STATIC
EFI_STATUS
ReadLkDsiState (
  IN MTK_DISPLAY_READ32 Read32,
  IN VOID *Context,
  OUT LK_DSI_STATE *State
  )
{
  STATIC CONST UINT32 Offset[] = {
    0x14, 0x18, 0x1C, 0x20, 0x24, 0x28,
    0x2C, 0x38, 0x50, 0x54, 0x58, 0x5C
  };
  EFI_STATUS Status;

  for (UINTN Index = 0; Index < ARRAY_SIZE (Offset); ++Index) {
    Status = Read32 (Context, 0x14013000 + Offset[Index], &State->Reg[Index]);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidateLkDsiState (
  IN CONST LK_DSI_STATE *State,
  IN UINT32 Width,
  IN UINT32 Height
  )
{
  // LK PSCTRL at +0x1c uses bits 14:0 for byte count and 19:16 for packing;
  // its original RGB888 configuration writes packing 3 and width * 3 bytes.
  if (((State->Reg[0] & 3U) == 0) ||
      ((State->Reg[1] & 0x3CU) != 0x3CU) ||
      ((State->Reg[2] & 0xF0000U) != 0x30000U)) {
    return EFI_UNSUPPORTED;
  }
  if ((Width == 0) || (Height == 0) || (Width > 0xFFFFU) ||
      (Height > 0xFFFFU) || (Width * 3U > 0x7FFFU) ||
      ((State->Reg[2] & 0x7FFFU) != Width * 3U) ||
      (State->Reg[6] != Height) ||
      (State->Reg[7] != ((Height << 16) | Width))) {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReadLkOvlPage (
  IN MTK_DISPLAY_READ32 Read32,
  IN VOID *Context,
  IN UINT32 Base,
  OUT UINT32 *Page
  )
{
  STATIC CONST UINT32 Header[] = {0x0C, 0x20, 0x24, 0x2C, 0x2D0, 0x324};
  STATIC CONST UINT32 LayerRegs[] = {0x30, 0x38, 0x3C, 0x40, 0x44, 0x4C, 0xC0, 0xF40};
  UINT32 Source;
  UINT32 Layer;
  EFI_STATUS Status;
  ZeroMem (Page, 0x1000);
  for (UINTN Index = 0; Index < ARRAY_SIZE (Header); ++Index) {
    Status = Read32 (Context, Base + Header[Index], &Page[Header[Index] / 4]);
    if (EFI_ERROR (Status)) { return Status; }
  }
  Source = Page[0x2C / 4];
  if ((Source == 0) || (Source >= (Base == 0x14005000 ? 16U : 4U)) ||
      ((Source & (Source - 1U)) != 0)) {
    return EFI_UNSUPPORTED;
  }
  for (Layer = 0; (Source & (1U << Layer)) == 0; ++Layer) {
  }
  for (UINTN Index = 0; Index < ARRAY_SIZE (LayerRegs); ++Index) {
    UINT32 Offset = LayerRegs[Index] + Layer * 0x20;
    Status = Read32 (Context, Base + Offset, &Page[Offset / 4]);
    if (EFI_ERROR (Status)) { return Status; }
    if (LayerRegs[Index] == 0xC0) {
      // The exact shipping DRM dump calls bits27:16 fifo_used. Only this
      // volatile count is omitted from stability comparisons; retain all
      // enable/configuration bits so a changing plane still fails closed.
      Page[Offset / 4] &= ~0x0FFF0000U;
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
AcquireLkHandoff (
  IN CONST VOID *Fdt,
  IN UINTN FdtSize,
  IN MTK_DISPLAY_READ32 Read32,
  IN MTK_DISPLAY_WRITE32 Write32 OPTIONAL,
  IN VOID *Context,
  IN BOOLEAN RequireConstantAlpha,
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  )
{
  STATIC CONST CHAR8 *VideoNames[] = {
    "atag,videolfb-fb_base_h", "atag,videolfb-fb_base_l", "atag,videolfb-vramSize",
    "atag,videolfb-islcmfound", "atag,videolfb-islcm_inited"
  };
  UINT32 Tags[ARRAY_SIZE (VideoNames)];
  UINT32 First[0x400];
  UINT32 Second[0x400];
  LK_DSI_STATE FirstDsi;
  LK_DSI_STATE SecondDsi;
  UINT32 Controller;
  UINT32 Again;
  UINT32 Width;
  UINT32 Height;
  UINT32 OriginalPitch;
  UINT32 PitchAddress;
  UINT32 ReadBack;
  UINT64 VideoBase;
  UINT64 VideoSize;
  INT32 Chosen;
  INT32 Node;
  INT32 Length;
  BOOLEAN InRam = FALSE;
  CONST VOID *Property;
  MTK_DISPLAY_OVL_PLANE Plane;
  MTK_DISPLAY_FRAMEBUFFER Result;
  EFI_STATUS Status;

  if ((Fdt == NULL) || (Read32 == NULL) || (FrameBuffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((FdtSize < sizeof (FDT_HEADER)) || (FdtTotalSize (Fdt) != FdtSize) ||
      (FdtCheckFull (Fdt, FdtSize) != 0)) { return EFI_COMPROMISED_DATA; }
  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) { return EFI_NOT_FOUND; }
  UINTN Present = 0;
  for (UINTN Index = 0; Index < ARRAY_SIZE (VideoNames); ++Index) {
    if (FdtGetProp (Fdt, Chosen, VideoNames[Index], &Length) != NULL) { ++Present; }
  }
  if (Present == 0) { return EFI_NOT_FOUND; }
  if (Present != ARRAY_SIZE (VideoNames)) { return EFI_COMPROMISED_DATA; }
  for (UINTN Index = 0; Index < ARRAY_SIZE (VideoNames); ++Index) {
    Status = ReadU32 (Fdt, Chosen, VideoNames[Index], &Tags[Index]);
    if (EFI_ERROR (Status)) { return Status; }
  }
  // Shipping LK +0x5454C updates only base/size, not islcm_inited. The stock
  // DT leaves that boolean at zero even after LK has started scanout. Accept
  // either well-formed boolean; the power/clock/path/plane/DSI checks below
  // prove the live state before any framebuffer is exposed or changed.
  if (Tags[4] > 1) { return EFI_COMPROMISED_DATA; }
  if (Tags[3] != 1) { return EFI_NOT_READY; }
  VideoBase = ((UINT64)Tags[0] << 32) | Tags[1];
  VideoSize = Tags[2];
  if ((VideoBase < 0x48090000ULL) || (VideoBase >= 0x100000000ULL) ||
      ((VideoBase & MTK_PAGE_MASK) != 0) || (VideoSize == 0) ||
      (VideoSize > 0x100000000ULL - VideoBase)) { return EFI_COMPROMISED_DATA; }
  // Before touching display registers, prove the entire live VRAM lies in RAM.
  FdtForEachSubnode (Node, Fdt, 0) {
    Property = FdtGetProp (Fdt, Node, "device_type", &Length);
    if ((Property == NULL) || (Length != 7) || (CompareMem (Property, "memory", 7) != 0)) {
      continue;
    }
    BOOLEAN Enabled;
    Status = IsNodeEnabled (Fdt, Node, &Enabled);
    if (EFI_ERROR (Status) || !Enabled) { continue; }
    Status = CheckRegContains (Fdt, Node, 0, VideoBase, VideoSize);
    if (!EFI_ERROR (Status)) { InRam = TRUE; }
    else if (Status != EFI_ACCESS_DENIED) { return Status; }
  }
  if (!InRam) { return EFI_ACCESS_DENIED; }
  Status = CheckLkHardwareNodes (Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ReadLkAccessState (Read32, Context, &Controller);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ReadLkOvlPage (Read32, Context, Controller, First);
  if (EFI_ERROR (Status)) { return Status; }
  Status = MtkDisplayDecodeLkOvl (Controller, First, sizeof (First), &Plane);
  if (EFI_ERROR (Status)) { return Status; }
  // LK's start enables shadow bypass. Global alpha must be fully opaque;
  // constant pixel alpha is prepared only after all bounds/stability checks.
  if (((First[0x0C / 4] & 0x400000U) == 0) || (Plane.AlphaControl != 0x1FFU)) {
    return EFI_UNSUPPORTED;
  }
  Status = Read32 (Context, 0x14007014, &Width);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Read32 (Context, 0x14007018, &Height);
  if (EFI_ERROR (Status)) { return Status; }
  if (((Width & 0x1FFFU) != Plane.Width) || ((Height & 0xFFFFFU) != Plane.Height)) {
    return EFI_COMPROMISED_DATA;
  }
  Status = ReadLkDsiState (Read32, Context, &FirstDsi);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ValidateLkDsiState (&FirstDsi, Plane.Width, Plane.Height);
  if (EFI_ERROR (Status)) { return Status; }
  ZeroMem (&Result, sizeof (Result));
  Result.FrameBufferBase = Plane.DmaAddress;
  Result.FrameBufferRegionSize = (UINT64)Plane.StrideBytes * Plane.Height;
  Result.Width = Plane.Width; Result.Height = Plane.Height;
  Result.StrideBytes = Plane.StrideBytes;
  Result.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  Status = MtkDisplayValidateLayout (&Result);
  if (EFI_ERROR (Status)) { return Status; }
  if (!RangeContains (VideoBase, VideoSize, Result.FrameBufferBase, Result.ProtectedSize)) {
    return EFI_ACCESS_DENIED;
  }
  // Recheck the access path and plane so a changing/shadow configuration cannot
  // publish a mixture of addresses, formats and geometry.
  Status = ReadLkAccessState (Read32, Context, &Again);
  if (EFI_ERROR (Status)) { return Status; }
  if (Again != Controller) { return EFI_NOT_READY; }
  Status = ReadLkOvlPage (Read32, Context, Controller, Second);
  if (EFI_ERROR (Status)) { return Status; }
  if (CompareMem (First, Second, sizeof (First)) != 0) { return EFI_NOT_READY; }
  Status = ReadLkDsiState (Read32, Context, &SecondDsi);
  if (EFI_ERROR (Status)) { return Status; }
  if (CompareMem (&FirstDsi, &SecondDsi, sizeof (FirstDsi)) != 0) { return EFI_NOT_READY; }
  if (RequireConstantAlpha && !Plane.Opaque) {
    if (Write32 == NULL) { return EFI_UNSUPPORTED; }
    // Exact shipping DRM layer_config +0x3878C writes bit28 at layer*0x20+0x44.
    // With LK shadow bypass enabled, only this existing plane flag is changed.
    PitchAddress = Controller + 0x44 + Plane.LayerIndex * 0x20;
    OriginalPitch = First[(PitchAddress - Controller) / 4];
    Status = Write32 (Context, PitchAddress, OriginalPitch | 0x10000000U);
    if (!EFI_ERROR (Status)) {
      Status = Read32 (Context, PitchAddress, &ReadBack);
      if (!EFI_ERROR (Status) && (ReadBack != (OriginalPitch | 0x10000000U))) {
        Status = EFI_DEVICE_ERROR;
      }
    }
    if (!EFI_ERROR (Status)) {
      First[(PitchAddress - Controller) / 4] = OriginalPitch | 0x10000000U;
      Status = ReadLkOvlPage (Read32, Context, Controller, Second);
      if (!EFI_ERROR (Status) && (CompareMem (First, Second, sizeof (First)) != 0)) {
        Status = EFI_NOT_READY;
      }
    }
    if (!EFI_ERROR (Status)) {
      Status = ReadLkDsiState (Read32, Context, &SecondDsi);
      if (!EFI_ERROR (Status) &&
          (CompareMem (&FirstDsi, &SecondDsi, sizeof (FirstDsi)) != 0)) {
        Status = EFI_NOT_READY;
      }
    }
    if (EFI_ERROR (Status)) {
      // Never attempt a restore into a powered-down or rerouted display.
      if (!EFI_ERROR (ReadLkAccessState (Read32, Context, &Again)) && (Again == Controller)) {
        EFI_STATUS ReadStatus = Read32 (Context, PitchAddress, &ReadBack);
        if (EFI_ERROR (ReadStatus)) {
          return EFI_DEVICE_ERROR;
        }
        if (ReadBack == (OriginalPitch | 0x10000000U)) {
          EFI_STATUS RestoreStatus = Write32 (Context, PitchAddress, OriginalPitch);
          ReadStatus = Read32 (Context, PitchAddress, &ReadBack);
          if (EFI_ERROR (RestoreStatus) || EFI_ERROR (ReadStatus) ||
              (ReadBack != OriginalPitch)) {
            return EFI_DEVICE_ERROR;
          }
        }
      }
      return Status;
    }
  }
  CopyMem (FrameBuffer, &Result, sizeof (Result));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MtkDisplayAcquireLkHandoff (
  IN CONST VOID *Fdt,
  IN UINTN FdtSize,
  IN MTK_DISPLAY_READ32 Read32,
  IN MTK_DISPLAY_WRITE32 Write32 OPTIONAL,
  IN VOID *Context,
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  )
{
  return AcquireLkHandoff (Fdt, FdtSize, Read32, Write32, Context, TRUE, FrameBuffer);
}

EFI_STATUS
EFIAPI
MtkDisplayInspectLkHandoff (
  IN CONST VOID *Fdt,
  IN UINTN FdtSize,
  IN MTK_DISPLAY_READ32 Read32,
  IN VOID *Context,
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  )
{
  return AcquireLkHandoff (Fdt, FdtSize, Read32, NULL, Context, FALSE, FrameBuffer);
}

EFI_STATUS
EFIAPI
MtkDisplayValidateLayout (
  IN OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  )
{
  UINT64 RequiredSize;
  UINT64 Rounded;

  if (FrameBuffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (FrameBuffer->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) {
    return EFI_UNSUPPORTED;
  }
  if ((FrameBuffer->FrameBufferBase == 0) ||
      (FrameBuffer->FrameBufferRegionSize == 0) ||
      (FrameBuffer->Width == 0) || (FrameBuffer->Height == 0) ||
      (FrameBuffer->Width > MAX_UINT32 / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) ||
      (FrameBuffer->StrideBytes < FrameBuffer->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) ||
      ((FrameBuffer->StrideBytes & 3U) != 0) ||
      (FrameBuffer->FrameBufferBase & MTK_PAGE_MASK) != 0 ||
      (FrameBuffer->FrameBufferRegionSize > MAX_UINT64 - FrameBuffer->FrameBufferBase) ||
      (FrameBuffer->FrameBufferRegionSize > MAX_UINT64 - MTK_PAGE_MASK)) {
    return EFI_COMPROMISED_DATA;
  }

  RequiredSize = (UINT64)FrameBuffer->StrideBytes * FrameBuffer->Height;
  if ((RequiredSize == 0) || (RequiredSize > FrameBuffer->FrameBufferRegionSize) ||
      (RequiredSize > MAX_UINTN)) {
    return EFI_COMPROMISED_DATA;
  }
  Rounded = (FrameBuffer->FrameBufferRegionSize + MTK_PAGE_MASK) &
            ~((UINT64)MTK_PAGE_MASK);
  if ((Rounded == 0) || (Rounded > MAX_UINT64 - FrameBuffer->FrameBufferBase)) {
    return EFI_COMPROMISED_DATA;
  }

  FrameBuffer->ProtectedSize = Rounded;
  FrameBuffer->FrameBufferSize = (UINTN)RequiredSize;
  FrameBuffer->PixelsPerScanLine =
    FrameBuffer->StrideBytes / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MtkDisplayParseHandoff (
  IN CONST VOID *Fdt,
  IN UINTN FdtSize,
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  )
{
  INT32 Node;
  INT32 Chosen;
  INT32 Length;
  INT32 ReservedRoot;
  UINT64 Base;
  UINT64 RegionSize;
  UINT32 Width;
  UINT32 Height;
  UINT32 Stride;
  UINT32 Phandle;
  CONST UINT8 *MemoryRegion;
  EFI_GRAPHICS_PIXEL_FORMAT Format;
  EFI_STATUS Status;

  if ((Fdt == NULL) || (FrameBuffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((FdtSize < sizeof (FDT_HEADER)) ||
      (FdtTotalSize (Fdt) != FdtSize) ||
      (FdtCheckFull (Fdt, FdtSize) != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = FindSimpleFramebuffer (Fdt, &Node, &Chosen);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = ReadFormat (Fdt, Node, &Format);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = ReadU32 (Fdt, Node, "width", &Width);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = ReadU32 (Fdt, Node, "height", &Height);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = ReadU32 (Fdt, Node, "stride", &Stride);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = ReadReg (Fdt, Node, Chosen, &Base, &RegionSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MemoryRegion = FdtGetProp (Fdt, Node, "memory-region", &Length);
  if (MemoryRegion != NULL) {
    if (Length != sizeof (UINT32)) {
      return EFI_COMPROMISED_DATA;
    }
    Phandle = (UINT32)ReadCells (MemoryRegion, 1);
    ReservedRoot = FdtPathOffset (Fdt, "/reserved-memory");
    Node = FdtNodeOffsetByPhandle (Fdt, Phandle);
    if ((Phandle == 0) || (Node < 0) || (ReservedRoot < 0) ||
        (FdtParentOffset (Fdt, Node) != ReservedRoot)) {
      return EFI_COMPROMISED_DATA;
    }
    Status = CheckRegContains (Fdt, Node, ReservedRoot, Base, RegionSize);
    if (EFI_ERROR (Status)) {
      return Status == EFI_ACCESS_DENIED ? EFI_COMPROMISED_DATA : Status;
    }
  }

  ZeroMem (FrameBuffer, sizeof (*FrameBuffer));
  FrameBuffer->FrameBufferBase = Base;
  FrameBuffer->FrameBufferRegionSize = RegionSize;
  FrameBuffer->Width = Width;
  FrameBuffer->Height = Height;
  FrameBuffer->StrideBytes = Stride;
  FrameBuffer->PixelFormat = Format;

  Status = MtkDisplayValidateLayout (FrameBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return CheckReservedRange (Fdt, Base, FrameBuffer->ProtectedSize);
}

EFI_STATUS
EFIAPI
MtkDisplayValidateMemoryMap (
  IN CONST MTK_DISPLAY_FRAMEBUFFER *FrameBuffer,
  IN CONST VOID *MemoryMap,
  IN UINTN MemoryMapSize,
  IN UINTN DescriptorSize
  )
{
  EFI_MEMORY_DESCRIPTOR Descriptor;
  MTK_DISPLAY_FRAMEBUFFER Checked;
  UINTN Offset;
  UINT64 RangeEnd;
  UINT64 DescriptorBytes;
  UINT64 DescriptorEnd;
  UINT64 Cursor;
  UINT64 BestEnd;

  if ((FrameBuffer == NULL) || (MemoryMap == NULL) ||
      (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
      (MemoryMapSize == 0) || ((MemoryMapSize % DescriptorSize) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Checked = *FrameBuffer;
  if (EFI_ERROR (MtkDisplayValidateLayout (&Checked)) ||
      (FrameBuffer->ProtectedSize != Checked.ProtectedSize)) {
    return EFI_COMPROMISED_DATA;
  }
  RangeEnd = Checked.FrameBufferBase + Checked.ProtectedSize;

  for (Offset = 0; Offset < MemoryMapSize; Offset += DescriptorSize) {
    CopyMem (&Descriptor, (CONST UINT8 *)MemoryMap + Offset, sizeof (Descriptor));
    if (Descriptor.NumberOfPages > MAX_UINT64 / EFI_PAGE_SIZE) {
      return EFI_COMPROMISED_DATA;
    }
    DescriptorBytes = Descriptor.NumberOfPages * EFI_PAGE_SIZE;
    if ((DescriptorBytes != 0) &&
        (Descriptor.PhysicalStart > MAX_UINT64 - DescriptorBytes)) {
      return EFI_COMPROMISED_DATA;
    }
    DescriptorEnd = Descriptor.PhysicalStart + DescriptorBytes;
    if (RangesOverlap (Checked.FrameBufferBase, Checked.ProtectedSize,
                       Descriptor.PhysicalStart, DescriptorBytes) &&
        (Descriptor.Type != EfiReservedMemoryType) &&
        (Descriptor.Type != EfiMemoryMappedIO)) {
      return EFI_ACCESS_DENIED;
    }
  }

  Cursor = Checked.FrameBufferBase;
  while (Cursor < RangeEnd) {
    BestEnd = Cursor;
    for (Offset = 0; Offset < MemoryMapSize; Offset += DescriptorSize) {
      CopyMem (&Descriptor, (CONST UINT8 *)MemoryMap + Offset, sizeof (Descriptor));
      DescriptorBytes = Descriptor.NumberOfPages * EFI_PAGE_SIZE;
      DescriptorEnd = Descriptor.PhysicalStart + DescriptorBytes;
      if (((Descriptor.Type == EfiReservedMemoryType) ||
           (Descriptor.Type == EfiMemoryMappedIO)) &&
          (Descriptor.PhysicalStart <= Cursor) && (DescriptorEnd > BestEnd)) {
        BestEnd = DescriptorEnd;
      }
    }
    if (BestEnd == Cursor) {
      return EFI_ACCESS_DENIED;
    }
    Cursor = (BestEnd < RangeEnd) ? BestEnd : RangeEnd;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CheckRect (
  IN UINTN X,
  IN UINTN Y,
  IN UINTN Width,
  IN UINTN Height,
  IN UINTN MaxWidth,
  IN UINTN MaxHeight
  )
{
  if ((Width == 0) || (Height == 0) ||
      (X > MaxWidth) || (Y > MaxHeight) ||
      (Width > MaxWidth - X) || (Height > MaxHeight - Y)) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GetBufferRegion (
  IN UINTN X,
  IN UINTN Y,
  IN UINTN Width,
  IN UINTN Height,
  IN UINTN Delta,
  IN UINTN BufferAddress,
  OUT UINTN *Offset,
  OUT UINTN *RowPitch,
  OUT UINTN *RowBytes
  )
{
  UINTN LastPixel;
  UINTN XBytes;
  UINTN CopyBytes;
  UINTN LastRowOffset;

  if ((Width == 0) || (Height == 0) ||
      (Width > MAX_UINTN / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) ||
      (X > MAX_UINTN - Width)) {
    return EFI_INVALID_PARAMETER;
  }
  CopyBytes = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  LastPixel = X + Width;
  if (LastPixel > MAX_UINTN / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) {
    return EFI_INVALID_PARAMETER;
  }
  XBytes = X * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  if (Delta == 0) {
    if ((X != 0) || (Y != 0)) {
      return EFI_INVALID_PARAMETER;
    }
    *RowPitch = CopyBytes;
  } else {
    if (((Delta & 3) != 0) ||
        (Delta < LastPixel * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))) {
      return EFI_INVALID_PARAMETER;
    }
    *RowPitch = Delta;
  }
  if ((Y != 0) && (*RowPitch > (MAX_UINTN - XBytes) / Y)) {
    return EFI_INVALID_PARAMETER;
  }
  *Offset = Y * *RowPitch + XBytes;
  if (*Offset > MAX_UINTN - CopyBytes) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Height - 1) != 0 &&
      (*RowPitch > (MAX_UINTN - *Offset - CopyBytes) / (Height - 1))) {
    return EFI_INVALID_PARAMETER;
  }
  LastRowOffset = *Offset + (Height - 1) * *RowPitch;
  if (BufferAddress > MAX_UINTN - LastRowOffset ||
      BufferAddress + LastRowOffset > MAX_UINTN - CopyBytes) {
    return EFI_INVALID_PARAMETER;
  }
  *RowBytes = CopyBytes;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MtkDisplayBlt (
  IN CONST MTK_DISPLAY_FRAMEBUFFER *FrameBuffer,
  IN OUT VOID *LinearFrameBuffer,
  IN UINTN LinearFrameBufferSize,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation,
  IN UINTN SourceX,
  IN UINTN SourceY,
  IN UINTN DestinationX,
  IN UINTN DestinationY,
  IN UINTN Width,
  IN UINTN Height,
  IN UINTN Delta
  )
{
  MTK_DISPLAY_FRAMEBUFFER Checked;
  UINTN SourceOffset = 0;
  UINTN DestinationOffset = 0;
  UINTN SourcePitch = 0;
  UINTN DestinationPitch = 0;
  UINTN SourceRowBytes = 0;
  UINTN DestinationRowBytes = 0;
  UINTN SourceRow;
  UINTN DestinationRow;
  UINTN Row;
  UINTN RowStart;
  UINTN Pixel;
  UINTN CopyBytes;
  UINT8 *Frame;
  UINT8 *Buffer;
  UINT8 *Source;
  UINT8 *Destination;
  UINT32 Fill;
  EFI_STATUS Status;

  if ((FrameBuffer == NULL) || (LinearFrameBuffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Checked = *FrameBuffer;
  Status = MtkDisplayValidateLayout (&Checked);
  if (EFI_ERROR (Status) ||
      (Checked.FrameBufferSize != FrameBuffer->FrameBufferSize) ||
      (LinearFrameBufferSize < Checked.FrameBufferSize)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Operation < EfiBltVideoFill) || (Operation >= EfiGraphicsOutputBltOperationMax)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Operation != EfiBltVideoToVideo) && (BltBuffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Operation == EfiBltVideoFill) ||
      (Operation == EfiBltBufferToVideo) ||
      (Operation == EfiBltVideoToVideo)) {
    Status = CheckRect (DestinationX, DestinationY, Width, Height,
                        Checked.Width, Checked.Height);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  if ((Operation == EfiBltVideoToBltBuffer) ||
      (Operation == EfiBltVideoToVideo)) {
    Status = CheckRect (SourceX, SourceY, Width, Height,
                        Checked.Width, Checked.Height);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  CopyBytes = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  Frame = LinearFrameBuffer;
  Buffer = (UINT8 *)BltBuffer;
  if (Operation == EfiBltBufferToVideo) {
    Status = GetBufferRegion (SourceX, SourceY, Width, Height, Delta,
                              (UINTN)Buffer, &SourceOffset, &SourcePitch, &SourceRowBytes);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  } else if (Operation == EfiBltVideoToBltBuffer) {
    Status = GetBufferRegion (DestinationX, DestinationY, Width, Height, Delta,
                              (UINTN)Buffer, &DestinationOffset,
                              &DestinationPitch, &DestinationRowBytes);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (Operation == EfiBltVideoFill) {
    CopyMem (&Fill, BltBuffer, sizeof (Fill));
    for (Row = 0; Row < Height; ++Row) {
      RowStart = (DestinationY + Row) * Checked.StrideBytes +
                 DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      if (((UINTN)(Frame + RowStart) & (sizeof (Fill) - 1U)) == 0U) {
        SetMem32 (Frame + RowStart, CopyBytes, Fill);
      } else {
        // Keep the byte-aligned caller path without unaligned word stores.
        for (Pixel = 0; Pixel < Width; ++Pixel) {
          CopyMem (Frame + RowStart + Pixel * sizeof (Fill), &Fill, sizeof (Fill));
        }
      }
    }
    return EFI_SUCCESS;
  }

  if (Operation == EfiBltBufferToVideo) {
    for (Row = 0; Row < Height; ++Row) {
      Source = Buffer + SourceOffset + Row * SourcePitch;
      Destination = Frame + (DestinationY + Row) * Checked.StrideBytes +
                    DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      CopyMem (Destination, Source, SourceRowBytes);
    }
    return EFI_SUCCESS;
  }

  if (Operation == EfiBltVideoToBltBuffer) {
    for (Row = 0; Row < Height; ++Row) {
      Source = Frame + (SourceY + Row) * Checked.StrideBytes +
               SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      Destination = Buffer + DestinationOffset + Row * DestinationPitch;
      CopyMem (Destination, Source, DestinationRowBytes);
    }
    return EFI_SUCCESS;
  }

  if ((DestinationY > SourceY) && (DestinationY - SourceY < Height)) {
    for (Row = Height; Row != 0; --Row) {
      SourceRow = SourceY + Row - 1;
      DestinationRow = DestinationY + Row - 1;
      Source = Frame + SourceRow * Checked.StrideBytes +
               SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      Destination = Frame + DestinationRow * Checked.StrideBytes +
                    DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      if (((UINTN)Destination > (UINTN)Source) &&
          ((UINTN)Destination - (UINTN)Source < CopyBytes)) {
        for (Pixel = Width; Pixel != 0; --Pixel) {
          CopyMem (
            Destination + (Pixel - 1) * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
            Source + (Pixel - 1) * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
            sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
            );
        }
      } else {
        CopyMem (Destination, Source, CopyBytes);
      }
    }
  } else {
    for (Row = 0; Row < Height; ++Row) {
      SourceRow = SourceY + Row;
      DestinationRow = DestinationY + Row;
      Source = Frame + SourceRow * Checked.StrideBytes +
               SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      Destination = Frame + DestinationRow * Checked.StrideBytes +
                    DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      if (((UINTN)Destination > (UINTN)Source) &&
          ((UINTN)Destination - (UINTN)Source < CopyBytes)) {
        for (Pixel = Width; Pixel != 0; --Pixel) {
          CopyMem (
            Destination + (Pixel - 1) * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
            Source + (Pixel - 1) * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
            sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
            );
        }
      } else {
        CopyMem (Destination, Source, CopyBytes);
      }
    }
  }
  return EFI_SUCCESS;
}
