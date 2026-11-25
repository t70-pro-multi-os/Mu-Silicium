// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MemoryMapLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/ArmLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MtkDisplayCore.h>
#include <Library/MtkCcciMemoryHandoff.h>

// The bootstrap map uses only the stock preloader's mb_kernel reservation.
// No EMI-based RAM discovery or other board's reserved ranges are used.
STATIC CONST EFI_MEMORY_REGION_DESCRIPTOR mBootstrap[] = {
  {"Kernel payload", 0x40000000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"Bootstrap RAM",  0x40800000, 0x0377F000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Boot handoff",   0x43F7F000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"UEFI Stack",     0x43F80000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD",        0x44000000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",       0x44400000, 0x03880000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Bootloader DTB", 0x47C80000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"GIC Distributor",    0x0C000000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GIC Redistributors", 0x0C040000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"WatchDog Timer",     0x10007000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  // Peripheral MMIO ranges checked against the exact effective stock DTB.
  {"Top Ck Gen", 0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Infra Cfg AO", 0x10001000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"EINT", 0x1000B000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Keypad Controller", 0x10010000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Pinctrl", 0x10005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"IOCFG RB", 0x11C30000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"IOCFG BM", 0x11D10000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"IOCFG BR", 0x11D40000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"IOCFG LM", 0x11E20000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"IOCFG BL", 0x11E60000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"IOCFG RT", 0x11EA0000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"AP Mixed", 0x1000C000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Display SPM", 0x10006000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Display MMSYS", 0x14000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Display SMI/OVL/RDMA", 0x14003000, 0x00005000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Display DSI", 0x14013000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Display MIPI TX", 0x11F60000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PMIC Wrapper", 0x10026000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"APDMA", 0x10217000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"I2C-0", 0x11E00000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"I2C-1", 0x11E01000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"I2C-6", 0x1101A000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"I2C Wrap W", 0x11E02000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"I2C Wrap C", 0x1101B000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MSDC-1", 0x11240000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MSDC Top-1", 0x11EF0000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"UFS", 0x11270000, 0x00003000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB0", 0x11200000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB PHY", 0x11F40000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
};

STATIC BOOLEAN mRuntimeTraceActive;
STATIC BOOLEAN mCcciMemoryValid;
typedef struct {
  UINT64 Base;
  UINT32 Size;
  UINT32 MdGeneration;
  UINT32 MdSubVersion;
  UINT32 ExpectedBootStatus;
  UINT32 BootStatus0;
  UINT32 BootStatus1;
  BOOLEAN ImageLoadReportedOk;
  BOOLEAN BootStatusMatched;
} CCCI_MD_MEMORY;
STATIC CCCI_MD_MEMORY mCcciMemory;
STATIC UINT32 mCcciExpectedBootStatus;
STATIC BOOLEAN mCcciHandoffReused;
STATIC BOOLEAN mCcciTagReadAllowed = TRUE;
STATIC UINT32 mRuntimePhase;
STATIC EFI_STATUS mDisplayAdoptionStatus;
STATIC UINT32 mRuntimeLastAddress, mRuntimeLastValue, mRuntimeLastOperation;

// GetMemoryMap runs before the MMU/cache is enabled. Reuse the fixed own
// snapshot so SEC progress survives even when DXE never starts.
STATIC VOID RecordRuntimeProgress (EFI_STATUS Status, UINT32 Additional) {
  if (!mRuntimeTraceActive) return;
  MmioWrite64 (0x43F7F800, 0);
  MmioWrite32 (0x43F7F808, 0x43F7F060);
  MmioWrite32 (0x43F7F80C, 0);
  MmioWrite32 (0x43F7F810, mRuntimePhase);
  MmioWrite32 (0x43F7F814, Additional);
  MmioWrite64 (0x43F7F818, Status);
  MmioWrite64 (0x43F7F820, mDisplayAdoptionStatus);
  MmioWrite32 (0x43F7F828, mRuntimeLastAddress);
  MmioWrite32 (0x43F7F82C, mRuntimeLastValue);
  MmioWrite32 (0x43F7F830, mRuntimeLastOperation);
  for (UINTN Address = 0x43F7F834; Address < 0x43F7F848; Address += 4) MmioWrite32 (Address, 0);
  MemoryFence ();
  MmioWrite64 (0x43F7F800, 0x000000044C443754ULL);
}

STATIC VOID RuntimePhase (UINT32 Phase) {
  mRuntimePhase = Phase;
  RecordRuntimeProgress (EFI_NOT_READY, 0);
}

STATIC EFI_STATUS EFIAPI ReadDisplay32 (VOID *Context, UINTN Address, UINT32 *Value) {
  (VOID)Context;
  mRuntimeLastAddress = (UINT32)Address; mRuntimeLastOperation = 1;
  RecordRuntimeProgress (EFI_NOT_READY, 0);
  *Value = MmioRead32 (Address);
  mRuntimeLastValue = *Value; mRuntimeLastOperation = 2;
  RecordRuntimeProgress (EFI_NOT_READY, 0);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI WriteDisplay32 (VOID *Context, UINTN Address, UINT32 Value) {
  (VOID)Context;
  mRuntimeLastAddress = (UINT32)Address; mRuntimeLastValue = Value; mRuntimeLastOperation = 3;
  RecordRuntimeProgress (EFI_NOT_READY, 0);
  MmioWrite32 (Address, Value);
  mRuntimeLastOperation = 4;
  RecordRuntimeProgress (EFI_NOT_READY, 0);
  return EFI_SUCCESS;
}

// The parser uses bounded static storage and no allocator before the MMU/HOBs.
#define MAX_RANGES 64
#define DTB_END 0x48080000ULL
#define PAGE_MASK 0xFFFULL
#define PA_LIMIT (1ULL << 36)
#define RANGE_OWNER_FIXED (-1)
#define RANGE_OWNER_VIDEO (-2)
#define RANGE_OWNER_MEMRESERVE (-3) // Decreasing values encode the original reserve-map index.
#define RANGE_OWNER_CCCI_METADATA (-4)
#define RANGE_OWNER_MODEM_MEMORY (-5)
typedef struct { UINT64 Start, End; EFI_MEMORY_TYPE Type; INT32 Owner; } RAM_RANGE;
STATIC EFI_MEMORY_REGION_DESCRIPTOR mRegions[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
STATIC BOOLEAN mMapCached;
STATIC UINT8 mCachedCount;
STATIC BOOLEAN mOverlapValid;
STATIC UINT64 mOverlapDetail[5];

STATIC UINT64 ReadCells (CONST UINT8 *Data, INT32 Cells) {
  UINT64 Value = 0;
  for (INT32 Index = 0; Index < Cells * 4; Index++) { Value = (Value << 8) | Data[Index]; }
  return Value;
}

STATIC UINT64 ReadLe (CONST UINT8 *Data, UINTN Bytes) {
  UINT64 Value = 0;
  for (UINTN Index = 0; Index < Bytes; Index++) { Value |= (UINT64)Data[Index] << (Index * 8); }
  return Value;
}

STATIC EFI_STATUS AddRange (RAM_RANGE *Ranges, UINTN *Count, UINT64 Start, UINT64 Size, EFI_MEMORY_TYPE Type) {
  UINT64 End;
  if ((Size == 0) || (Start >= PA_LIMIT) || (Size > PA_LIMIT - Start)) { return EFI_COMPROMISED_DATA; }
  End = Start + Size;
  if (Type != EfiConventionalMemory) {
    Start &= ~PAGE_MASK;
    End = (End + PAGE_MASK) & ~PAGE_MASK;
  } else {
    Start = (Start + PAGE_MASK) & ~PAGE_MASK;
    End &= ~PAGE_MASK;
  }
  if (End <= Start) { return EFI_SUCCESS; }
  if (*Count >= MAX_RANGES) { return EFI_OUT_OF_RESOURCES; }
  Ranges[*Count].Start = Start;
  Ranges[*Count].End = End;
  Ranges[*Count].Type = Type;
  Ranges[*Count].Owner = RANGE_OWNER_FIXED;
  (*Count)++;
  return EFI_SUCCESS;
}

STATIC BOOLEAN NodeEnabled (CONST VOID *Fdt, INT32 Node) {
  INT32 Length;
  CONST CHAR8 *Status = FdtGetProp (Fdt, Node, "status", &Length);
  return Status == NULL ||
    ((Length == 5) && (CompareMem (Status, "okay", 5) == 0)) ||
    ((Length == 3) && (CompareMem (Status, "ok", 3) == 0));
}

STATIC EFI_STATUS ReadReg (CONST VOID *Fdt, INT32 Node, INT32 AddressCells, INT32 SizeCells,
                           RAM_RANGE *Ranges, UINTN *Count, EFI_MEMORY_TYPE Type) {
  INT32 Length;
  INT32 Tuple = (AddressCells + SizeCells) * 4;
  CONST UINT8 *Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  EFI_STATUS Status;
  if ((Reg == NULL) || (Length <= 0) || (Length % Tuple != 0)) { return EFI_COMPROMISED_DATA; }
  for (INT32 Offset = 0; Offset < Length; Offset += Tuple) {
    UINTN PreviousCount = *Count;
    Status = AddRange (Ranges, Count, ReadCells (Reg + Offset, AddressCells),
                      ReadCells (Reg + Offset + AddressCells * 4, SizeCells), Type);
    if (EFI_ERROR (Status)) { return Status; }
    if (*Count > PreviousCount) { Ranges[*Count - 1].Owner = Node; }
  }
  return EFI_SUCCESS;
}

// DTSpec 3.5.4: a size-only pool is allocated by the OS after boot services.
// Validate the request, but never invent a physical address or reserve its window.
STATIC EFI_STATUS ReadReserved (CONST VOID *Fdt, INT32 Node, INT32 AddressCells,
                                INT32 SizeCells, RAM_RANGE *Ranges, UINTN *Count) {
  INT32 Length, RegLength, NoMapLength, ReusableLength;
  CONST VOID *NoMap = FdtGetProp (Fdt, Node, "no-map", &NoMapLength);
  CONST VOID *Reusable = FdtGetProp (Fdt, Node, "reusable", &ReusableLength);
  CONST UINT8 *Value;
  UINT64 Bytes, Alignment;
  if (((NoMap != NULL) && (NoMapLength != 0)) ||
      ((Reusable != NULL) && (ReusableLength != 0)) ||
      ((NoMap != NULL) && (Reusable != NULL))) { return EFI_COMPROMISED_DATA; }
  Value = FdtGetProp (Fdt, Node, "reg", &RegLength);
  // reg takes precedence, including when LK left the original size request.
  if (Value != NULL) {
    return ReadReg (Fdt, Node, AddressCells, SizeCells, Ranges, Count,
                    NoMap != NULL ? EfiReservedMemoryType : EfiBootServicesData);
  }
  Value = FdtGetProp (Fdt, Node, "size", &Length);
  if ((Value == NULL) || (Length != SizeCells * 4)) { return EFI_COMPROMISED_DATA; }
  Bytes = ReadCells (Value, SizeCells);
  if ((Bytes == 0) || (Bytes > PA_LIMIT)) { return EFI_COMPROMISED_DATA; }
  Value = FdtGetProp (Fdt, Node, "alignment", &Length);
  if (Value != NULL) {
    if (Length != SizeCells * 4) { return EFI_COMPROMISED_DATA; }
    Alignment = ReadCells (Value, SizeCells);
    if ((Alignment == 0) || (Alignment > PA_LIMIT) ||
        ((Alignment & (Alignment - 1)) != 0)) { return EFI_COMPROMISED_DATA; }
  }
  Value = FdtGetProp (Fdt, Node, "alloc-ranges", &Length);
  if (Value != NULL) {
    INT32 Tuple = (AddressCells + SizeCells) * 4;
    if ((Length <= 0) || (Length % Tuple != 0) ||
        (Length / Tuple > MAX_RANGES)) { return EFI_COMPROMISED_DATA; }
    for (INT32 Offset = 0; Offset < Length; Offset += Tuple) {
      UINT64 Base = ReadCells (Value + Offset, AddressCells);
      UINT64 Size = ReadCells (Value + Offset + AddressCells * 4, SizeCells);
      if ((Size == 0) || (Base >= PA_LIMIT) || (Size > PA_LIMIT - Base)) {
        return EFI_COMPROMISED_DATA;
      }
    }
  }
  return EFI_SUCCESS;
}

// The exact LK writes these three DT cells independently of reserved-memory.
// They protect VRAM even when it supplies no dimensions/format for GOP.
// Exact shipping LK: +0x16128 names nodes "mblock-%d-%s", +0x16200
// sets "mediatek,%s"; display +0x543D4 selects the "framebuffer" allocation
// but +0x5454C publishes its independently calculated, possibly smaller VRAM.
STATIC BOOLEAN IsLkFramebufferContainer (CONST VOID *Fdt, INT32 Node,
                                         UINT64 VideoBase, UINT64 VideoSize) {
  INT32 Parent, Length, NameLength, Found;
  CONST CHAR8 *Name;
  CONST VOID *Value;
  UINT32 Index = 0;
  UINT64 Base, Size;
  if (Node < 0) return FALSE;
  Value = FdtGetProp (Fdt, Node, "compatible", &Length);
  if (Value == NULL || Length != sizeof ("mediatek,framebuffer") ||
      CompareMem (Value, "mediatek,framebuffer", sizeof ("mediatek,framebuffer")) != 0) return FALSE;
  // Reject unrelated pools before the libfdt parent lookup rescans the tree.
  Parent = FdtParentOffset (Fdt, Node);
  if (Parent != FdtPathOffset (Fdt, "/reserved-memory") ||
      FdtAddressCells (Fdt, Parent) != 2 || FdtSizeCells (Fdt, Parent) != 2) return FALSE;
  Found = FdtNodeOffsetByCompatible (Fdt, -1, "mediatek,framebuffer");
  if (Found != Node || FdtNodeOffsetByCompatible (Fdt, Node, "mediatek,framebuffer") >= 0) return FALSE;
  Name = FdtGetName (Fdt, Node, &NameLength);
  if (Name == NULL || NameLength < 20 || NameLength > 22 ||
      CompareMem (Name, "mblock-", 7) != 0 || Name[7] == '0' ||
      CompareMem (Name + NameLength - 12, "-framebuffer", 12) != 0) return FALSE;
  for (INT32 Pos = 7; Pos < NameLength - 12; Pos++) {
    if (Name[Pos] < '0' || Name[Pos] > '9') return FALSE;
    Index = Index * 10 + (UINT32)(Name[Pos] - '0');
  }
  if (Index == 0 || Index > 128) return FALSE;
  Value = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Value == NULL || Length != 16) return FALSE;
  Base = ReadCells (Value, 2); Size = ReadCells ((CONST UINT8 *)Value + 8, 2);
  return Base == VideoBase && Size >= VideoSize && Size <= 64U*1024U*1024U &&
         VideoSize != 0 && Size <= PA_LIMIT - Base;
}

STATIC EFI_STATUS ReserveLkVideo (CONST VOID *Fdt, RAM_RANGE *Reserved,
                                  UINTN *ReservedCount, RAM_RANGE *Video) {
  STATIC CONST CHAR8 *Names[] = {
    "atag,videolfb-fb_base_h", "atag,videolfb-fb_base_l", "atag,videolfb-vramSize"
  };
  CONST UINT8 *Values[ARRAY_SIZE (Names)];
  INT32 Lengths[ARRAY_SIZE (Names)];
  INT32 Chosen = FdtPathOffset (Fdt, "/chosen");
  UINTN Present = 0;
  UINT64 Base, Bytes;
  EFI_STATUS Status;
  if (Chosen < 0) { return EFI_SUCCESS; }
  for (UINTN Index = 0; Index < ARRAY_SIZE (Names); Index++) {
    Values[Index] = FdtGetProp (Fdt, Chosen, Names[Index], &Lengths[Index]);
    if (Values[Index] != NULL) { Present++; }
  }
  if (Present == 0) { return EFI_SUCCESS; }
  if (Present != ARRAY_SIZE (Names)) { return EFI_COMPROMISED_DATA; }
  for (UINTN Index = 0; Index < ARRAY_SIZE (Names); Index++) {
    if (Lengths[Index] != sizeof (UINT32)) { return EFI_COMPROMISED_DATA; }
  }
  Base = (ReadCells (Values[0], 1) << 32) | ReadCells (Values[1], 1);
  Bytes = ReadCells (Values[2], 1);
  if ((Base == 0) || (Bytes == 0)) { return EFI_NOT_READY; }
  if (Base < DTB_END + 0x10000) { return EFI_ACCESS_DENIED; }
  Status = AddRange (Reserved, ReservedCount, Base, Bytes, EfiReservedMemoryType);
  if (!EFI_ERROR (Status)) {
    Reserved[*ReservedCount - 1].Owner = RANGE_OWNER_VIDEO;
    *Video = Reserved[*ReservedCount - 1];
    // A byte-identical DT memreserve entry is the same LK VRAM reservation.
    // Only that source is reclassified; CCCI, other fixed pools and partial
    // overlaps retain their independent ownership and still block adoption.
    for (UINTN Index = 0; Index + 1 < *ReservedCount; Index++) {
      if (IsLkFramebufferContainer (Fdt, Reserved[Index].Owner, Base, Bytes)) {
        // Preserve every byte of the allocation; only the validated live
        // plane receives an uncached CPU mapping later.
        Reserved[Index].Owner = RANGE_OWNER_VIDEO;
        Reserved[Index].Type = EfiReservedMemoryType;
        continue;
      }
      if (Reserved[Index].Owner <= RANGE_OWNER_MEMRESERVE &&
          Reserved[Index].Start == Video->Start && Reserved[Index].End == Video->End) {
        UINT64 OriginalBase, OriginalSize;
        INT32 ReserveIndex = RANGE_OWNER_MEMRESERVE - Reserved[Index].Owner;
        if (FdtGetReserveMapEntry (Fdt, ReserveIndex, &OriginalBase, &OriginalSize) == 0 &&
            OriginalBase == Base && OriginalSize == Bytes) {
          Reserved[Index].Owner = RANGE_OWNER_VIDEO;
        }
      }
    }
  }
  return Status;
}

// Only a pool explicitly referenced by the standard framebuffer may supply
// scanout. Other fixed pools, memreserve and external LK buffers keep ownership.

typedef EFI_STATUS (EFIAPI *CCCI_TAG_READER)(
  UINT64 Base, UINTN Bytes, VOID *Buffer);


STATIC EFI_STATUS EFIAPI ReadCcciTagBlockPhysical(
  UINT64 Base, UINTN Bytes, VOID *Buffer)
{
  if (Buffer == NULL || Base >= PA_LIMIT || Bytes == 0U ||
      Bytes > 0x10000U || Bytes > PA_LIMIT - Base) {
    return EFI_INVALID_PARAMETER;
  }
  if (ArmMmuEnabled ()) return EFI_ACCESS_DENIED;
  CopyMem(Buffer, (CONST VOID *)(UINTN)Base, Bytes);
  return EFI_SUCCESS;
}

STATIC CCCI_TAG_READER mCcciTagReader = ReadCcciTagBlockPhysical;
STATIC UINT8 mCcciTagBuffer[0x10000];

STATIC BOOLEAN RangeInRam(
  CONST RAM_RANGE *Ram, UINTN RamCount, UINT64 Base, UINT64 Bytes);

STATIC BOOLEAN RangeInRamPageAligned(
  CONST RAM_RANGE *Ram, UINTN RamCount, UINT64 Base, UINT64 Bytes)
{
  UINT64 End;
  if (Bytes == 0U || Base >= PA_LIMIT || Bytes > PA_LIMIT - Base) return FALSE;
  End = Base + Bytes;
  if (End > PA_LIMIT - PAGE_MASK) return FALSE;
  return RangeInRam (Ram, RamCount, Base & ~PAGE_MASK,
                     ((End + PAGE_MASK) & ~PAGE_MASK) - (Base & ~PAGE_MASK));
}

STATIC BOOLEAN RangeInRam(
  CONST RAM_RANGE *Ram, UINTN RamCount, UINT64 Base, UINT64 Bytes)
{
  UINT64 End;
  if (Ram == NULL || Bytes == 0U || Base >= PA_LIMIT ||
      Bytes > PA_LIMIT - Base) {
    return FALSE;
  }
  End = Base + Bytes;
  for (UINTN Index = 0U; Index < RamCount; ++Index) {
    if (Base >= Ram[Index].Start && End <= Ram[Index].End) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC BOOLEAN OverlapsFixedReservation(
  CONST RAM_RANGE *Ranges, UINTN Count, UINT64 Base, UINT64 Bytes)
{
  UINT64 End;
  if (Ranges == NULL || Bytes == 0U || Base >= PA_LIMIT ||
      Bytes > PA_LIMIT - Base) {
    return TRUE;
  }
  End = Base + Bytes;
  for (UINTN Index = 0U; Index < Count; ++Index) {
    if (Ranges[Index].Owner != RANGE_OWNER_FIXED &&
        Ranges[Index].Owner != RANGE_OWNER_VIDEO) {
      continue;
    }
    if (Base < Ranges[Index].End && Ranges[Index].Start < End) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC BOOLEAN CcciTagNameMatches(
  CONST UINT8 *Name, UINTN NameBytes, CONST CHAR8 *Expected)
{
  UINTN ExpectedBytes = AsciiStrLen(Expected);
  for (UINTN Index = 0U; Index < NameBytes; ++Index) {
    if (Name[Index] == 0U) {
      return Index == ExpectedBytes &&
          CompareMem(Name, Expected, ExpectedBytes) == 0;
    }
  }
  return FALSE;
}

/* Read only hdr_count/hdr_tbl_inf from the LK CCCI tag block. Firmware
 * image headers, MPU policy, calibration and payload tags are never copied. */
STATIC EFI_STATUS ParseCcciMdMemory(
  CONST UINT8 *Property,
  UINTN PropertyBytes,
  BOOLEAN Version2Property,
  CONST UINT8 *TagBlock,
  UINTN TagBlockBytes,
  CCCI_MD_MEMORY *Memory)
{
  UINT32 TagCount, InfoBytes, TagVersion, GlobalError = 0U;
  UINTN NameBytes, HeaderBytes, Cursor = 0U;
  UINT32 HeaderCount = 0U, HeaderTableOffset = 0U, HeaderTableBytes = 0U;
  BOOLEAN HaveHeaderCount = FALSE, HaveHeaderTable = FALSE;
  UINT64 MdBase = 0U;
  UINT32 MdSize = 0U;
  UINT8 MdError = 0xffU;
  BOOLEAN HaveMd1 = FALSE;

  if (Property == NULL || TagBlock == NULL || Memory == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Memory, sizeof (*Memory));
  if (PropertyBytes < (Version2Property ? 24U : 16U)) {
    return EFI_COMPROMISED_DATA;
  }
  InfoBytes = (UINT32)ReadLe(Property + 8U, 4U);
  if (InfoBytes == 0U || InfoBytes > 0x10000U ||
      InfoBytes > TagBlockBytes) {
    return EFI_COMPROMISED_DATA;
  }
  if (Version2Property) {
    GlobalError = (UINT32)ReadLe(Property + 12U, 4U);
    TagVersion = (UINT32)ReadLe(Property + 16U, 4U);
    TagCount = (UINT32)ReadLe(Property + 20U, 4U);
  } else {
    TagVersion = 1U;
    TagCount = (UINT32)ReadLe(Property + 12U, 4U);
  }
  NameBytes = (TagVersion >= 2U) ? 64U : 16U;
  HeaderBytes = NameBytes + 12U;
  if (TagCount == 0U) {
    return EFI_NOT_FOUND;
  }
  if (TagCount > InfoBytes / HeaderBytes) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINT32 Index = 0U; Index < TagCount; ++Index) {
    UINT32 DataOffset, DataSize, NextOffset;
    CONST UINT8 *Tag;
    if (Cursor > InfoBytes || HeaderBytes > InfoBytes - Cursor) {
      return EFI_COMPROMISED_DATA;
    }
    Tag = TagBlock + Cursor;
    BOOLEAN Terminated = FALSE;
    for (UINTN NameIndex = 0U; NameIndex < NameBytes; ++NameIndex) {
      if (Tag[NameIndex] == 0U) { Terminated = TRUE; break; }
    }
    if (!Terminated) { return EFI_COMPROMISED_DATA; }
    DataOffset = (UINT32)ReadLe(Tag + NameBytes, 4U);
    DataSize = (UINT32)ReadLe(Tag + NameBytes + 4U, 4U);
    NextOffset = (UINT32)ReadLe(Tag + NameBytes + 8U, 4U);
    if (DataOffset > InfoBytes || DataSize > InfoBytes - DataOffset) {
      return EFI_COMPROMISED_DATA;
    }
    if (CcciTagNameMatches(Tag, NameBytes, "hdr_count")) {
      if (HaveHeaderCount || DataSize < sizeof(UINT32)) {
        return EFI_COMPROMISED_DATA;
      }
      HeaderCount = (UINT32)ReadLe(TagBlock + DataOffset, sizeof(UINT32));
      HaveHeaderCount = TRUE;
    } else if (CcciTagNameMatches(Tag, NameBytes, "hdr_tbl_inf")) {
      if (HaveHeaderTable) { return EFI_COMPROMISED_DATA; }
      HeaderTableOffset = DataOffset;
      HeaderTableBytes = DataSize;
      HaveHeaderTable = TRUE;
    }
    if (Index + 1U < TagCount) {
      if (NextOffset < Cursor + HeaderBytes || NextOffset >= InfoBytes) {
        return EFI_COMPROMISED_DATA;
      }
      Cursor = NextOffset;
    }
  }
  if (!HaveHeaderCount || !HaveHeaderTable) { return EFI_NOT_FOUND; }
  if (HeaderCount == 0U || HeaderCount > 4U ||
      HeaderTableBytes < HeaderCount * 24U) {
    return EFI_COMPROMISED_DATA;
  }
  for (UINT32 Index = 0U; Index < HeaderCount; ++Index) {
    CONST UINT8 *Entry = TagBlock + HeaderTableOffset + (Index * 24U);
    if (Entry[12U] == 0U) {
      if (HaveMd1) { return EFI_COMPROMISED_DATA; }
      MdBase = ReadLe(Entry, 8U);
      MdSize = (UINT32)ReadLe(Entry + 8U, 4U);
      MdError = Entry[13U];
      HaveMd1 = TRUE;
    }
  }
  if (!HaveMd1 || MdBase == 0U || MdSize == 0U ||
      MdBase >= PA_LIMIT || (UINT64)MdSize > PA_LIMIT - MdBase) {
    return EFI_NOT_FOUND;
  }
  Memory->Base = MdBase;
  Memory->Size = MdSize;
  Memory->ImageLoadReportedOk = (BOOLEAN)(GlobalError == 0U && MdError == 0U);
  return EFI_SUCCESS;
}

STATIC INT32 FrameBufferPool (CONST VOID *Fdt) {
  INT32 Chosen = FdtPathOffset (Fdt, "/chosen");
  INT32 Node, Length;
  CONST VOID *Value;
  if (Chosen < 0) { return -1; }
  FdtForEachSubnode (Node, Fdt, Chosen) {
    if (!NodeEnabled (Fdt, Node)) { continue; }
    Value = FdtGetProp (Fdt, Node, "compatible", &Length);
    if ((Value == NULL) || (FdtStringListContains (Value, Length, "simple-framebuffer") != 1)) {
      continue;
    }
    Value = FdtGetProp (Fdt, Node, "memory-region", &Length);
    if ((Value == NULL) || (Length != sizeof (UINT32))) { return -1; }
    return FdtNodeOffsetByPhandle (Fdt, (UINT32)ReadCells (Value, 1));
  }
  return -1;
}

STATIC BOOLEAN ProtectedOverlap (CONST RAM_RANGE *Reserved, UINTN Count,
                                  UINT64 Start, UINT64 End, INT32 AllowedPool) {
  for (UINTN Index = 0; Index < Count; ++Index) {
    if ((Reserved[Index].Owner == RANGE_OWNER_VIDEO) ||
        ((AllowedPool >= 0) && (Reserved[Index].Owner == AllowedPool))) {
      continue;
    }
    if ((Start < Reserved[Index].End) && (Reserved[Index].Start < End)) {
      mOverlapDetail[0] = Start; mOverlapDetail[1] = End;
      mOverlapDetail[2] = Reserved[Index].Start; mOverlapDetail[3] = Reserved[Index].End;
      mOverlapDetail[4] = ((UINT64)(UINT32)Reserved[Index].Owner << 32) | Index;
      mOverlapValid = TRUE;
      return TRUE;
    }
  }
  return FALSE;
}


STATIC UINT32 ReadCcciBootStatusExpected (CONST VOID *Fdt,
                                          UINT32 *MdGeneration,
                                          UINT32 *MdSubVersion) {
  INT32 Node;
  INT32 Length;
  CONST UINT8 *Generation;
  CONST UINT8 *SubVersion;
  UINT32 GenerationValue, SubVersionValue = 0U;
  if ((Fdt == NULL) || (MdGeneration == NULL) || (MdSubVersion == NULL)) return 0U;
  *MdGeneration = 0U;
  *MdSubVersion = 0U;
  Node = FdtPathOffset (Fdt, "/soc/mddriver");
  if ((Node < 0) || !NodeEnabled (Fdt, Node)) return 0U;
  Generation = FdtGetProp (Fdt, Node, "mediatek,md-generation", &Length);
  if ((Generation == NULL) || (Length != 4)) return 0U;
  GenerationValue = (UINT32)ReadCells (Generation, 1);
  SubVersion = FdtGetProp (Fdt, Node, "mediatek,md-sub-version", &Length);
  if (SubVersion != NULL) {
    if (Length != 4) return 0U;
    SubVersionValue = (UINT32)ReadCells (SubVersion, 1);
  }
  *MdGeneration = GenerationValue;
  *MdSubVersion = SubVersionValue;
  return T70CcciExpectedMdBootStatus (GenerationValue, SubVersionValue);
}

STATIC BOOLEAN LoadExistingCcciHandoff (VOID) {
  T70_CCCI_MEMORY_HANDOFF Handoff;
  UINT64 State = MmioRead64 ((UINTN)T70_CCCI_MEMORY_MAP_STATE_ADDRESS);
  if (State == T70_CCCI_MEMORY_MAP_STATE_FAILED) {
    mCcciTagReadAllowed = FALSE;
    return FALSE;
  }
  if (State != T70_CCCI_MEMORY_MAP_STATE_VALID) {
    mCcciTagReadAllowed = TRUE;
    return FALSE;
  }
  for (UINTN Offset = 0U; Offset < sizeof (Handoff); Offset += sizeof (UINT64)) {
    UINT64 Word = MmioRead64 ((UINTN)T70_CCCI_MEMORY_HANDOFF_ADDRESS + Offset);
    CopyMem ((UINT8 *)&Handoff + Offset, &Word, sizeof (Word));
  }
  if (!T70CcciMemoryHandoffIsValid (&Handoff)) {
    mCcciTagReadAllowed = FALSE;
    return FALSE;
  }
  ZeroMem (&mCcciMemory, sizeof (mCcciMemory));
  mCcciMemory.Base = Handoff.ModemBase;
  mCcciMemory.Size = (UINT32)Handoff.ModemBytes;
  mCcciMemory.MdGeneration = Handoff.MdGeneration;
  mCcciMemory.MdSubVersion = Handoff.MdSubVersion;
  mCcciMemory.ExpectedBootStatus = T70CcciExpectedMdBootStatus (
      Handoff.MdGeneration, Handoff.MdSubVersion);
  mCcciMemory.BootStatus0 = Handoff.BootStatus0;
  mCcciMemory.BootStatus1 = Handoff.BootStatus1;
  mCcciMemory.ImageLoadReportedOk = (BOOLEAN)(
      (Handoff.Flags & T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK) != 0U);
  mCcciMemory.BootStatusMatched = (BOOLEAN)(
      (Handoff.Flags & T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED) != 0U);
  mCcciMemoryValid = TRUE;
  mCcciHandoffReused = TRUE;
  mCcciTagReadAllowed = FALSE;
  return TRUE;
}

STATIC VOID MarkCcciMemoryMapState (UINT64 State) {
  MmioWrite64 ((UINTN)T70_CCCI_MEMORY_MAP_STATE_ADDRESS, State);
  MemoryFence ();
  WriteBackDataCacheRange ((VOID *)(UINTN)T70_CCCI_MEMORY_MAP_STATE_ADDRESS,
                           sizeof (UINT64));
  MemoryFence ();
}

STATIC VOID QueryCcciBootStatus (CCCI_MD_MEMORY *Memory) {
  ARM_SMC_ARGS Args;
  if (Memory == NULL) return;
  Memory->BootStatus0 = 0U;
  Memory->BootStatus1 = 0U;
  Memory->BootStatusMatched = FALSE;
  if (!Memory->ImageLoadReportedOk ||
      (Memory->ExpectedBootStatus == 0U)) return;
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = T70_CCCI_SMC_BOOT_STATUS_FID;
  Args.Arg1 = T70_CCCI_SMC_POWER_CONFIG;
  Args.Arg2 = T70_CCCI_SMC_MD_BOOT_STATUS;
  ArmCallSmc (&Args);
  if ((Args.Arg0 != T70_CCCI_SMC_SUCCESS) ||
      (Args.Arg1 > MAX_UINT32) || (Args.Arg2 > MAX_UINT32) ||
      !T70CcciBootStatusMatches (Memory->ExpectedBootStatus,
          (UINT32)Args.Arg0, (UINT32)Args.Arg1)) return;
  Memory->BootStatus0 = (UINT32)Args.Arg1;
  Memory->BootStatus1 = (UINT32)Args.Arg2;
  Memory->BootStatusMatched = TRUE;
}

// Publish both usable RAM and fixed reservations above the bootstrap map.
// No-map and external LK data get HOBs without an MMU mapping. Other fixed
// pools remain boot-services data until the OS owns memory after EBS.

EFI_STATUS BuildRuntimeMemoryMap (CONST VOID *Fdt, UINTN Size,
                                  EFI_MEMORY_REGION_DESCRIPTOR *Regions, UINT8 *Count) {
  RAM_RANGE Ram[MAX_RANGES], Reserved[MAX_RANGES], Swap;
  UINTN RamCount = 0, ReservedCount = 0;
  INT32 AddressCells, SizeCells, Node, Root, Depth, Length, Entries;
  CONST VOID *Property;
  EFI_STATUS Status;
  UINTN Output;
  BOOLEAN HaveModemInfo = FALSE, HaveFrameBuffer = FALSE;
  RAM_RANGE Video = {0};
  MTK_DISPLAY_FRAMEBUFFER FrameBuffer;
  UINT32 MdGeneration = 0U, MdSubVersion = 0U;
  if ((Fdt == NULL) || (Regions == NULL) || (Count == NULL)) { return EFI_INVALID_PARAMETER; }
  if (*Count >= MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT) { return EFI_INVALID_PARAMETER; }
  if ((Size < sizeof (FDT_HEADER)) || (FdtCheckFull (Fdt, Size) != 0) ||
      (FdtTotalSize (Fdt) != Size)) { return EFI_COMPROMISED_DATA; }
  RuntimePhase (1);
  if (!mCcciHandoffReused) {
    mCcciMemoryValid = FALSE;
    ZeroMem (&mCcciMemory, sizeof (mCcciMemory));
  }
  mCcciExpectedBootStatus = ReadCcciBootStatusExpected (Fdt, &MdGeneration,
                                                       &MdSubVersion);
  mDisplayAdoptionStatus = EFI_NOT_STARTED;
  mOverlapValid = FALSE; ZeroMem (mOverlapDetail, sizeof (mOverlapDetail));
  Output = *Count;
  AddressCells = FdtAddressCells (Fdt, 0);
  SizeCells = FdtSizeCells (Fdt, 0);
  if ((AddressCells < 1) || (AddressCells > 2) || (SizeCells < 1) || (SizeCells > 2)) {
    return EFI_UNSUPPORTED;
  }
  Status = AddRange (Reserved, &ReservedCount, 0, DTB_END, EfiReservedMemoryType);
  if (EFI_ERROR (Status)) { return Status; }
  // Exact preloader descriptor: aee_debug_kinfo immediately follows mb_dtb.
  Status = AddRange (Reserved, &ReservedCount, DTB_END, 0x10000, EfiReservedMemoryType);
  if (EFI_ERROR (Status)) { return Status; }
  RuntimePhase (2);
  Entries = FdtGetNumberOfReserveMapEntries (Fdt);
  if (Entries < 0) { return EFI_COMPROMISED_DATA; }
  for (INT32 Index = 0; Index < Entries; Index++) {
    UINT64 Address, Bytes;
    if (FdtGetReserveMapEntry (Fdt, Index, &Address, &Bytes) != 0) { return EFI_COMPROMISED_DATA; }
    Status = AddRange (Reserved, &ReservedCount, Address, Bytes, EfiReservedMemoryType);
    if (EFI_ERROR (Status)) { return Status; }
    Reserved[ReservedCount - 1].Owner = RANGE_OWNER_MEMRESERVE - Index;
  }
  RuntimePhase (3);
  Root = FdtPathOffset (Fdt, "/reserved-memory");
  if (Root < 0) { return EFI_NOT_FOUND; }
  // A nonempty ranges property needs address translation, which is unsupported.
  Property = FdtGetProp (Fdt, Root, "ranges", &Length);
  if ((Property == NULL) || (Length != 0) || !NodeEnabled (Fdt, Root)) { return EFI_UNSUPPORTED; }
  INT32 ResAddressCells = FdtAddressCells (Fdt, Root);
  INT32 ResSizeCells = FdtSizeCells (Fdt, Root);
  if ((ResAddressCells < 1) || (ResAddressCells > 2) || (ResSizeCells < 1) || (ResSizeCells > 2)) {
    return EFI_UNSUPPORTED;
  }
  FdtForEachSubnode (Node, Fdt, Root) {
    if (!NodeEnabled (Fdt, Node)) { continue; }
    Status = ReadReserved (Fdt, Node, ResAddressCells, ResSizeCells, Reserved, &ReservedCount);
    if (EFI_ERROR (Status)) { return Status; }
  }
  /* Collect and validate RAM before following LK's external CCCI metadata
   * pointer. The tag block is in physical RAM but is not a DT cell array. */
  FdtForEachSubnode (Node, Fdt, 0) {
    Property = FdtGetProp (Fdt, Node, "device_type", &Length);
    if ((Property == NULL) || (Length != 7) ||
        (CompareMem (Property, "memory", 7) != 0) ||
        !NodeEnabled (Fdt, Node)) {
      continue;
    }
    Status = ReadReg (Fdt, Node, AddressCells, SizeCells, Ram, &RamCount,
                      EfiConventionalMemory);
    if (EFI_ERROR (Status)) { return Status; }
  }
  if (RamCount == 0U) { return EFI_NOT_FOUND; }
  for (UINTN Index = 1U; Index < RamCount; Index++) {
    for (UINTN J = Index; J > 0U && Ram[J].Start < Ram[J - 1U].Start; J--) {
      Swap = Ram[J]; Ram[J] = Ram[J - 1U]; Ram[J - 1U] = Swap;
    }
  }
  for (UINTN Index = 1U; Index < RamCount; Index++) {
    if (Ram[Index].Start < Ram[Index - 1U].End) {
      return EFI_COMPROMISED_DATA;
    }
  }
  // CCCI's property contains little-endian native LK metadata, not DT cells.
  // Protect the entire 64 KiB mapped by ccci_util_lib, not just the tag count.
  RuntimePhase (4);
  Depth = 0;
  for (Node = FdtNextNode (Fdt, -1, &Depth); Node >= 0 && Depth >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    Property = FdtGetProp (Fdt, Node, "ccci,modem_info_v2", &Length);
    BOOLEAN Version2Property = (BOOLEAN)(Property != NULL);
    if (Property == NULL) { Property = FdtGetProp (Fdt, Node, "ccci,modem_info", &Length); }
    if (Property != NULL) {
      CONST UINT8 *Bytes = Property;
      UINT64 Base;
      UINT32 InfoBytes;
      CCCI_MD_MEMORY ModemMemory;
      if (HaveModemInfo) return EFI_COMPROMISED_DATA;
      if (Length < (Version2Property ? 24 : 16)) {
        return EFI_COMPROMISED_DATA;
      }
      Base = ReadLe (Bytes, 8);
      InfoBytes = (UINT32)ReadLe (Bytes + 8, 4);
      if ((Base == 0) || (InfoBytes == 0U) || (InfoBytes > 0x10000U)) {
        return EFI_NOT_READY;
      }
      if (!RangeInRam (Ram, RamCount, Base, 0x10000U) ||
          !RangeInRamPageAligned (Ram, RamCount, Base, 0x10000U)) {
        return EFI_ACCESS_DENIED;
      }
      Status = AddRange (Reserved, &ReservedCount, Base, 0x10000U,
                         EfiReservedMemoryType);
      if (EFI_ERROR (Status)) { return Status; }
      Reserved[ReservedCount - 1U].Owner = RANGE_OWNER_CCCI_METADATA;
      HaveModemInfo = TRUE;

      if (mCcciHandoffReused) {
        ModemMemory = mCcciMemory;
        if (ModemMemory.MdGeneration != MdGeneration ||
            ModemMemory.MdSubVersion != MdSubVersion) {
          return EFI_COMPROMISED_DATA;
        }
      } else {
        if (!mCcciTagReadAllowed) return EFI_NOT_READY;
        Status = mCcciTagReader (Base, InfoBytes, mCcciTagBuffer);
        if (EFI_ERROR (Status)) { return EFI_NOT_READY; }
        Status = ParseCcciMdMemory (Bytes, (UINTN)Length, Version2Property,
                                    mCcciTagBuffer, InfoBytes, &ModemMemory);
        if (EFI_ERROR (Status)) { return EFI_NOT_READY; }
        ModemMemory.ExpectedBootStatus = mCcciExpectedBootStatus;
        ModemMemory.MdGeneration = MdGeneration;
        ModemMemory.MdSubVersion = MdSubVersion;
      }
      if (!RangeInRam (Ram, RamCount, ModemMemory.Base, ModemMemory.Size) ||
          !RangeInRamPageAligned (Ram, RamCount,
                                  ModemMemory.Base, ModemMemory.Size) ||
          OverlapsFixedReservation (Reserved, ReservedCount,
                                    ModemMemory.Base, ModemMemory.Size)) {
        return EFI_ACCESS_DENIED;
      }
      Status = AddRange (Reserved, &ReservedCount, ModemMemory.Base,
                         ModemMemory.Size, EfiReservedMemoryType);
      if (EFI_ERROR (Status)) { return Status; }
      Reserved[ReservedCount - 1U].Owner = RANGE_OWNER_MODEM_MEMORY;
      mCcciMemory = ModemMemory;
      mCcciMemoryValid = TRUE;
    }
  }
  // Without runtime modem metadata, copying DTB bytes alone cannot protect its
  // external pointers. Keep bootstrap-only memory rather than reclaiming it.
  if (!HaveModemInfo) { return EFI_NOT_READY; }
  RuntimePhase (5);
  Status = ReserveLkVideo (Fdt, Reserved, &ReservedCount, &Video);
  if (EFI_ERROR (Status)) { return Status; }
  // Reserve first, before any allocator can overwrite the bootloader's scanout.
  // Only the shared, strictly validated GOP handoff permits a CPU mapping.
  RuntimePhase (7);
  Status = MtkDisplayParseHandoff (Fdt, Size, &FrameBuffer);
  mDisplayAdoptionStatus = Status;
  if (Status == EFI_NOT_FOUND) {
    // Check the whole LK reservation before the collector can touch MMIO or
    // prepare pixel alpha. Overlapping private data is never scanout storage.
    RuntimePhase (6);
    if ((Video.End != 0) && ProtectedOverlap (Reserved, ReservedCount, Video.Start, Video.End, -1)) {
      return EFI_ACCESS_DENIED;
    }
    RuntimePhase (8);
    Status = MtkDisplayAcquireLkHandoff (Fdt, Size, ReadDisplay32, WriteDisplay32, NULL, &FrameBuffer);
    mDisplayAdoptionStatus = Status;
    // Display adoption is optional; LK VRAM remains protected on any failure.
    if (EFI_ERROR (Status)) { Status = EFI_NOT_FOUND; }
  }
  RuntimePhase (9);
  if (Status != EFI_NOT_FOUND) {
    if (EFI_ERROR (Status)) { return Status; }
    if (FrameBuffer.FrameBufferBase < DTB_END + 0x10000) { return EFI_ACCESS_DENIED; }
    if ((Video.End != 0) &&
        ((FrameBuffer.FrameBufferBase < Video.Start) ||
         (FrameBuffer.FrameBufferBase >= Video.End) ||
         (FrameBuffer.ProtectedSize > Video.End - FrameBuffer.FrameBufferBase))) {
      return EFI_COMPROMISED_DATA;
    }
    if (ProtectedOverlap (Reserved, ReservedCount, FrameBuffer.FrameBufferBase,
                          FrameBuffer.FrameBufferBase + FrameBuffer.ProtectedSize,
                          FrameBufferPool (Fdt))) {
      return EFI_ACCESS_DENIED;
    }
    Status = AddRange (Reserved, &ReservedCount, FrameBuffer.FrameBufferBase,
                       FrameBuffer.ProtectedSize, EfiReservedMemoryType);
    if (EFI_ERROR (Status)) { return Status; }
    Reserved[ReservedCount - 1].Owner = RANGE_OWNER_VIDEO;
    if (Video.End == 0) { Video = Reserved[ReservedCount - 1]; }
    HaveFrameBuffer = TRUE;
  }
  RuntimePhase (10);
  // RAM banks were collected and checked before reading external CCCI tags.
  if (Video.End != 0) {
    BOOLEAN InRam = FALSE;
    for (UINTN Bank = 0; Bank < RamCount; Bank++) {
      if ((Video.Start >= Ram[Bank].Start) && (Video.End <= Ram[Bank].End)) {
        InRam = TRUE;
        break;
      }
    }
    if (!InRam) { return EFI_ACCESS_DENIED; }
  }
  for (UINTN Bank = 0; Bank < RamCount; Bank++) {
    UINT64 Cursor = MAX (Ram[Bank].Start, DTB_END);
    while (Cursor < Ram[Bank].End) {
      UINT64 Next = Ram[Bank].End;
      EFI_MEMORY_TYPE Type = EfiConventionalMemory;
      BOOLEAN IsFrameBuffer = HaveFrameBuffer &&
        (Cursor >= FrameBuffer.FrameBufferBase) &&
        (Cursor - FrameBuffer.FrameBufferBase < FrameBuffer.ProtectedSize);
      // Split at every boundary. Reserved wins over boot-services data when
      // fixed pools, memreserve entries and external LK buffers overlap.
      for (UINTN Index = 0; Index < ReservedCount; Index++) {
        if (Reserved[Index].Start > Cursor) {
          Next = MIN (Next, Reserved[Index].Start);
        } else if (Reserved[Index].End > Cursor) {
          Next = MIN (Next, Reserved[Index].End);
          if ((Type == EfiConventionalMemory) ||
              (Reserved[Index].Type == EfiReservedMemoryType)) {
            Type = Reserved[Index].Type;
          }
        }
      }
      EFI_MEMORY_REGION_DESCRIPTOR Region = {
        "Runtime RAM", Cursor, Next - Cursor,
        Type == EfiReservedMemoryType ? HobOnlyNoCacheSetting : AddMem,
        SYS_MEM, SYS_MEM_CAP, Type, WRITE_BACK
      };
      if (Type != EfiConventionalMemory) {
        CopyMem (Region.Name, "Runtime Reserved", sizeof ("Runtime Reserved"));
      }
      if (IsFrameBuffer) {
        CopyMem (Region.Name, "Runtime FrameBuffer", sizeof ("Runtime FrameBuffer"));
        Region.HobOption = AddMem;
        Region.ArmAttributes = UNCACHED_UNBUFFERED;
      }
      // Keep the whole strict GOP range in one GCD descriptor despite nested
      // reservations. Protected overlaps were rejected before any display write.
      if ((Output > *Count) &&
          (Regions[Output-1].Address + Regions[Output-1].Length == Cursor) &&
          (Regions[Output-1].MemoryType == Region.MemoryType) &&
          (Regions[Output-1].HobOption == Region.HobOption) &&
          (Regions[Output-1].ArmAttributes == Region.ArmAttributes)) {
        Regions[Output-1].Length += Region.Length;
      } else {
        if (Output >= MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT - 1) { return EFI_OUT_OF_RESOURCES; }
        Regions[Output++] = Region;
      }
      Cursor = Next;
    }
  }
  RuntimePhase (11);
  *Count = (UINT8)Output;
  return EFI_SUCCESS;
}

STATIC VOID PublishCcciMemoryHandoff (BOOLEAN Valid) {
  const UINTN Address = (UINTN)T70_CCCI_MEMORY_HANDOFF_ADDRESS;
  UINT32 Flags = 0U;

  if (mCcciHandoffReused) return;
  MmioWrite64 (Address, 0U);
  MemoryFence ();
  WriteBackDataCacheRange ((VOID *)(UINTN)Address, sizeof (UINT64));
  if (!Valid || !mCcciMemoryValid) {
    MemoryFence ();
    return;
  }

  if (mCcciMemory.ImageLoadReportedOk) {
    Flags |= T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK;
    QueryCcciBootStatus (&mCcciMemory);
  }
  if (mCcciMemory.BootStatusMatched) {
    Flags |= T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED;
  }
  MmioWrite32 (Address + 8U, T70_CCCI_MEMORY_HANDOFF_VERSION);
  MmioWrite32 (Address + 12U, sizeof (T70_CCCI_MEMORY_HANDOFF));
  MmioWrite64 (Address + 16U, mCcciMemory.Base);
  MmioWrite64 (Address + 24U, mCcciMemory.Size);
  MmioWrite32 (Address + 32U, Flags);
  MmioWrite32 (Address + 36U, mCcciMemory.MdGeneration);
  MmioWrite32 (Address + 40U, mCcciMemory.MdSubVersion);
  MmioWrite32 (Address + 44U, mCcciMemory.BootStatus0);
  MmioWrite32 (Address + 48U, mCcciMemory.BootStatus1);
  MmioWrite32 (Address + 52U, 0U);
  MmioWrite64 (Address + 56U, 0U);
  MemoryFence ();
  WriteBackDataCacheRange ((VOID *)(UINTN)(Address + sizeof (UINT64)),
                           sizeof (T70_CCCI_MEMORY_HANDOFF) - sizeof (UINT64));
  MemoryFence ();
  MmioWrite64 (Address, T70_CCCI_MEMORY_HANDOFF_SIGNATURE);
  MemoryFence ();
  WriteBackDataCacheRange ((VOID *)(UINTN)Address, sizeof (UINT64));
  MemoryFence ();
}

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  UINT64 Address, Size;
  EFI_STATUS Status = EFI_NOT_FOUND;
  // This is the fixed platform layout, not Boot Services' dynamic memory map.
  if (mMapCached) {
    *MemoryDescriptor = mRegions;
    *MemoryDescriptorCount = mCachedCount;
    return;
  }
  mCcciHandoffReused = FALSE;
  mCcciTagReadAllowed = TRUE;
  if (!LoadExistingCcciHandoff ()) {
    if (mCcciTagReadAllowed) {
      MmioWrite64 ((UINTN)T70_CCCI_MEMORY_HANDOFF_ADDRESS, 0U);
      MemoryFence ();
      WriteBackDataCacheRange ((VOID *)(UINTN)T70_CCCI_MEMORY_HANDOFF_ADDRESS,
                               sizeof (UINT64));
      mCcciMemoryValid = FALSE;
      ZeroMem (&mCcciMemory, sizeof (mCcciMemory));
    }
  }
  mCcciExpectedBootStatus = 0U;
  mRuntimeTraceActive = FALSE;
  mRuntimePhase = 0;
  mRuntimeLastAddress = mRuntimeLastValue = mRuntimeLastOperation = 0;
  mDisplayAdoptionStatus = EFI_NOT_STARTED;
  mOverlapValid = FALSE; ZeroMem (mOverlapDetail, sizeof (mOverlapDetail));
  UINT8 Count = ARRAY_SIZE (mBootstrap);
  CopyMem (mRegions, mBootstrap, sizeof (mBootstrap));
  if (MmioRead64 (0x43F7F000) == 0x0000000148303754ULL) {
    // Other images may link this library after DXE. Their resource lookups
    // must not overwrite the live GOP or the pre-DXE SEC snapshot.
    mRuntimeTraceActive = MmioRead64 (0x43F7F038) == 3;
    RecordRuntimeProgress (EFI_NOT_READY, 0);
    Address = MmioRead64 (0x43F7F008);
    Size = MmioRead64 (0x43F7F010);
    if ((Address >= 0x47C80000) && (Address < DTB_END) && ((Address & 7) == 0) &&
        (Size >= sizeof (FDT_HEADER)) && (Size <= 0x200000) && (Size <= DTB_END - Address)) {
      Status = BuildRuntimeMemoryMap ((CONST VOID *)(UINTN)Address, (UINTN)Size, mRegions, &Count);
    } else { Status = EFI_COMPROMISED_DATA; }
    // Count is committed only on success. Partial output cannot expose RAM.
    if (EFI_ERROR (Status)) { Count = ARRAY_SIZE (mBootstrap); }
    if (!EFI_ERROR (Status)) {
      PublishCcciMemoryHandoff (TRUE);
      if (!mCcciHandoffReused) MarkCcciMemoryMapState (
          T70_CCCI_MEMORY_MAP_STATE_VALID);
      mCachedCount = Count;
      mMapCached = TRUE;
    } else {
      PublishCcciMemoryHandoff (FALSE);
      if (!mCcciHandoffReused) MarkCcciMemoryMapState (
          T70_CCCI_MEMORY_MAP_STATE_FAILED);
    }
    if (mRuntimeTraceActive) {
      // Own reserved handoff bytes only; GOP copies this fixed metadata into
      // its bounded reset record. No reserved memory contents are exported.
      MmioWrite64 (0x43F7F080, 0);
      if (mOverlapValid) {
        for (UINTN Index = 0; Index < ARRAY_SIZE (mOverlapDetail); Index++) {
          MmioWrite64 (0x43F7F088 + Index * 8, mOverlapDetail[Index]);
        }
        MmioWrite64 (0x43F7F080, 0x000000014F4D3754ULL);
      }
      MmioWrite64 (0x43F7F060, Status);
      // Same bounded SEC status slots, with tagged phase/adoption metadata.
      UINT32 DisplayCode = (UINT32)(mDisplayAdoptionStatus & 0x7F) |
                           (EFI_ERROR (mDisplayAdoptionStatus) ? 0x80U : 0);
      UINT64 Meta = 0x4D4D0000U | (mRuntimePhase << 8) | DisplayCode;
      MmioWrite64 (0x43F7F068, (Meta << 32) | (Count - ARRAY_SIZE (mBootstrap)));
      RecordRuntimeProgress (Status, Count - ARRAY_SIZE (mBootstrap));
    }
    mRuntimeTraceActive = FALSE;
  }
  *MemoryDescriptor = mRegions;
  *MemoryDescriptorCount = Count;
}
