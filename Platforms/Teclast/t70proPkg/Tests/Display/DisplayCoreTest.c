// SPDX-License-Identifier: BSD-2-Clause-Patent
#define TEST_DISPLAY_COPY_COUNTS
#include "MediaTekTest.h"
#include <Library/FdtLib.h>
#include <Library/MtkDisplayCore.h>
#include "LkDisplayFixture.h"

#define TEST_FDT_CAPACITY 16384
#define TEST_FB_BASE      0x00100000
#define TEST_FB_SIZE      0x00002000
#define TEST_WIDTH        64
#define TEST_HEIGHT       32

static UINT32
ToBigEndian32 (
  UINT32 Value
  )
{
  return ((Value & 0x000000FFU) << 24) |
         ((Value & 0x0000FF00U) << 8) |
         ((Value & 0x00FF0000U) >> 8) |
         ((Value & 0xFF000000U) >> 24);
}

static void
SetCell (
  VOID *Fdt,
  INT32 Node,
  CONST CHAR8 *Name,
  UINT32 Value
  )
{
  UINT32 Wire = ToBigEndian32 (Value);
  CHECK (FdtSetProp (Fdt, Node, Name, &Wire, sizeof (Wire)) == 0);
}

static void
SetReg (
  VOID *Fdt,
  INT32 Node,
  UINT32 Base,
  UINT32 Size
  )
{
  UINT32 Reg[4] = {
    ToBigEndian32 (0),
    ToBigEndian32 (Base),
    ToBigEndian32 (0),
    ToBigEndian32 (Size)
  };
  CHECK (FdtSetProp (Fdt, Node, "reg", Reg, sizeof (Reg)) == 0);
}

static UINTN
MakeFramebufferTree (
  VOID *Fdt,
  BOOLEAN IncludeFramebuffer,
  BOOLEAN IncludeReservation,
  BOOLEAN IncludeMemoryRegion,
  CONST CHAR8 *Format,
  UINT32 Stride,
  UINT32 ReservationSize
  )
{
  INT32 Chosen;
  INT32 Node;
  INT32 Reserved;
  INT32 ReservedNode;

  CHECK (FdtCreateEmptyTree (Fdt, TEST_FDT_CAPACITY) == 0);
  SetCell (Fdt, 0, "#address-cells", 2);
  SetCell (Fdt, 0, "#size-cells", 2);

  Chosen = FdtAddSubnode (Fdt, 0, "chosen");
  CHECK (Chosen >= 0);
  SetCell (Fdt, Chosen, "#address-cells", 2);
  SetCell (Fdt, Chosen, "#size-cells", 2);
  if (IncludeFramebuffer) {
    Node = FdtAddSubnode (Fdt, Chosen, "framebuffer@100000");
    CHECK (Node >= 0);
    CHECK (FdtSetPropString (Fdt, Node, "compatible", "simple-framebuffer") == 0);
    CHECK (FdtSetPropString (Fdt, Node, "format", Format) == 0);
    SetCell (Fdt, Node, "width", TEST_WIDTH);
    SetCell (Fdt, Node, "height", TEST_HEIGHT);
    SetCell (Fdt, Node, "stride", Stride);
    SetReg (Fdt, Node, TEST_FB_BASE, TEST_FB_SIZE);
    if (IncludeMemoryRegion) {
      SetCell (Fdt, Node, "memory-region", 7);
    }
  }

  if (IncludeReservation) {
    Reserved = FdtAddSubnode (Fdt, 0, "reserved-memory");
    CHECK (Reserved >= 0);
    SetCell (Fdt, Reserved, "#address-cells", 2);
    SetCell (Fdt, Reserved, "#size-cells", 2);
    CHECK (FdtSetPropEmpty (Fdt, Reserved, "ranges") == 0);
    ReservedNode = FdtAddSubnode (Fdt, Reserved, "framebuffer@100000");
    CHECK (ReservedNode >= 0);
    SetReg (Fdt, ReservedNode, TEST_FB_BASE, ReservationSize);
    SetCell (Fdt, ReservedNode, "phandle", 7);
    CHECK (FdtSetPropEmpty (Fdt, ReservedNode, "no-map") == 0);
  }

  CHECK (FdtPack (Fdt) == 0);
  return FdtTotalSize (Fdt);
}

typedef struct {
  EFI_MEMORY_DESCRIPTOR Descriptor;
  UINT8 Extension[24];
} TEST_MEMORY_DESCRIPTOR;

static void
TestRuntimeHandoffAndReservations (
  CONST VOID *StockFdt,
  UINTN StockSize
  )
{
  UINT8 Tree[TEST_FDT_CAPACITY];
  UINTN TreeSize;
  MTK_DISPLAY_FRAMEBUFFER FrameBuffer;
  TEST_MEMORY_DESCRIPTOR Map[2];
  EFI_STATUS Status;

  memset (&FrameBuffer, 0xA5, sizeof (FrameBuffer));
  Status = MtkDisplayParseHandoff (StockFdt, StockSize, &FrameBuffer);
  CHECK (Status == EFI_NOT_FOUND);

  TreeSize = MakeFramebufferTree (Tree, TRUE, TRUE, TRUE, "x8r8g8b8", 256, 0x4000);
  Status = MtkDisplayParseHandoff (Tree, TreeSize, &FrameBuffer);
  if (Status != EFI_SUCCESS) {
    fprintf (stderr, "valid synthetic handoff returned 0x%llx\\n", (unsigned long long)Status);
  }
  CHECK (Status == EFI_SUCCESS);
  CHECK (FrameBuffer.FrameBufferBase == TEST_FB_BASE);
  CHECK (FrameBuffer.FrameBufferRegionSize == TEST_FB_SIZE);
  CHECK (FrameBuffer.ProtectedSize == TEST_FB_SIZE);
  CHECK (FrameBuffer.FrameBufferSize == TEST_FB_SIZE);
  CHECK (FrameBuffer.Width == TEST_WIDTH && FrameBuffer.Height == TEST_HEIGHT);
  CHECK (FrameBuffer.StrideBytes == 256 && FrameBuffer.PixelsPerScanLine == TEST_WIDTH);
  CHECK (FrameBuffer.PixelFormat == PixelBlueGreenRedReserved8BitPerColor);

  TreeSize = MakeFramebufferTree (Tree, TRUE, FALSE, FALSE, "x8r8g8b8", 256, 0);
  CHECK (MtkDisplayParseHandoff (Tree, TreeSize, &FrameBuffer) == EFI_ACCESS_DENIED);

  TreeSize = MakeFramebufferTree (Tree, TRUE, TRUE, FALSE, "a8r8g8b8", 256, TEST_FB_SIZE);
  CHECK (MtkDisplayParseHandoff (Tree, TreeSize, &FrameBuffer) == EFI_UNSUPPORTED);

  TreeSize = MakeFramebufferTree (Tree, TRUE, TRUE, FALSE, "x8r8g8b8", 260, TEST_FB_SIZE);
  CHECK (MtkDisplayParseHandoff (Tree, TreeSize, &FrameBuffer) == EFI_COMPROMISED_DATA);

  TreeSize = MakeFramebufferTree (Tree, TRUE, TRUE, FALSE, "x8r8g8b8", 256, EFI_PAGE_SIZE);
  CHECK (MtkDisplayParseHandoff (Tree, TreeSize, &FrameBuffer) == EFI_ACCESS_DENIED);

  TreeSize = MakeFramebufferTree (Tree, TRUE, FALSE, TRUE, "x8r8g8b8", 256, 0);
  CHECK (MtkDisplayParseHandoff (Tree, TreeSize, &FrameBuffer) == EFI_COMPROMISED_DATA);

  TreeSize = MakeFramebufferTree (Tree, TRUE, TRUE, FALSE, "x8r8g8b8", 256, TEST_FB_SIZE);
  CHECK (MtkDisplayParseHandoff (Tree, TreeSize, &FrameBuffer) == EFI_SUCCESS);
  memset (Map, 0, sizeof (Map));
  Map[0].Descriptor.Type = EfiReservedMemoryType;
  Map[0].Descriptor.PhysicalStart = TEST_FB_BASE;
  Map[0].Descriptor.NumberOfPages = 2;
  Status = MtkDisplayValidateMemoryMap (
             &FrameBuffer,
             Map,
             sizeof (Map[0]),
             sizeof (Map[0])
             );
  CHECK (Status == EFI_SUCCESS);

  Map[0].Descriptor.Type = EfiBootServicesData;
  CHECK (MtkDisplayValidateMemoryMap (&FrameBuffer, Map, sizeof (Map[0]), sizeof (Map[0])) == EFI_ACCESS_DENIED);

  Map[0].Descriptor.Type = EfiReservedMemoryType;
  Map[0].Descriptor.NumberOfPages = 1;
  CHECK (MtkDisplayValidateMemoryMap (&FrameBuffer, Map, sizeof (Map[0]), sizeof (Map[0])) == EFI_ACCESS_DENIED);

  Map[0].Descriptor.NumberOfPages = 2;
  Map[1].Descriptor.Type = EfiConventionalMemory;
  Map[1].Descriptor.PhysicalStart = TEST_FB_BASE + EFI_PAGE_SIZE;
  Map[1].Descriptor.NumberOfPages = 1;
  CHECK (MtkDisplayValidateMemoryMap (&FrameBuffer, Map, sizeof (Map), sizeof (Map[0])) == EFI_ACCESS_DENIED);

  Map[1].Descriptor.Type = EfiReservedMemoryType;
  Map[1].Descriptor.PhysicalStart = TEST_FB_BASE + EFI_PAGE_SIZE;
  Map[1].Descriptor.NumberOfPages = 1;
  CHECK (MtkDisplayValidateMemoryMap (&FrameBuffer, Map, sizeof (Map), sizeof (Map[0])) == EFI_SUCCESS);

  CHECK (MtkDisplayValidateMemoryMap (&FrameBuffer, Map, sizeof (Map) - 1, sizeof (Map[0])) == EFI_INVALID_PARAMETER);
}

static void
TestDynamicReservations (
  VOID
  )
{
  UINT8 Tree[TEST_FDT_CAPACITY], Saved[TEST_FDT_CAPACITY];
  UINT32 Size[2] = {0, ToBigEndian32 (0x01000000)};
  UINT32 Reg[4] = {0, ToBigEndian32 (0x00200000), 0, ToBigEndian32 (0x1000)};
  MTK_DISPLAY_FRAMEBUFFER FrameBuffer;
  INT32 Root, Pool, Fixed;
  UINTN Bytes;

  MakeFramebufferTree (Tree, TRUE, TRUE, FALSE, "x8r8g8b8", 256, TEST_FB_SIZE);
  CHECK (FdtOpenInto (Tree, Tree, sizeof (Tree)) == 0);
  Root = FdtPathOffset (Tree, "/reserved-memory");
  CHECK (Root >= 0);
  // New nodes precede the existing fixed reservation in libfdt's wire order.
  Pool = FdtAddSubnode (Tree, Root, "os-pool");
  CHECK (Pool >= 0);
  CHECK (FdtSetProp (Tree, Pool, "size", Size, sizeof (Size)) == 0);
  CHECK (FdtSetPropEmpty (Tree, Pool, "no-map") == 0);
  CHECK (FdtPack (Tree) == 0);
  Bytes = FdtTotalSize (Tree);
  memcpy (Saved, Tree, Bytes);
  CHECK (MtkDisplayParseHandoff (Tree, Bytes, &FrameBuffer) == EFI_SUCCESS);
  CHECK (FrameBuffer.FrameBufferBase == TEST_FB_BASE);
  CHECK (memcmp (Saved, Tree, Bytes) == 0);

  // A usable address must still come from reg, never size or alloc-ranges.
  CHECK (FdtOpenInto (Tree, Tree, sizeof (Tree)) == 0);
  Pool = FdtPathOffset (Tree, "/reserved-memory/os-pool");
  Fixed = FdtPathOffset (Tree, "/reserved-memory/framebuffer@100000");
  CHECK (Pool >= 0 && Fixed >= 0);
  CHECK (FdtSetProp (Tree, Pool, "reg", Reg, sizeof (Reg)) == 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_SUCCESS);
  CHECK (FdtSetPropEmpty (Tree, Pool, "reg") == 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_COMPROMISED_DATA);
  CHECK (FdtDelProp (Tree, Pool, "reg") == 0);
  CHECK (FdtSetPropEmpty (Tree, Pool, "size") == 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_COMPROMISED_DATA);
  CHECK (FdtSetProp (Tree, Pool, "size", Size, sizeof (UINT32)) == 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_COMPROMISED_DATA);
  CHECK (FdtSetProp (Tree, Pool, "size", Size, sizeof (Size)) == 0);
  CHECK (FdtSetProp (Tree, Pool, "alloc-ranges", Reg, sizeof (Reg)) == 0);
  Fixed = FdtPathOffset (Tree, "/reserved-memory/framebuffer@100000");
  CHECK (FdtSetProp (Tree, Fixed, "size", Size, sizeof (Size)) == 0);
  CHECK (FdtDelProp (Tree, Fixed, "reg") == 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_ACCESS_DENIED);

  // The request uses the reserved-memory parent's size-cell width.
  MakeFramebufferTree (Tree, TRUE, TRUE, FALSE, "x8r8g8b8", 256, TEST_FB_SIZE);
  CHECK (FdtOpenInto (Tree, Tree, sizeof (Tree)) == 0);
  Root = FdtPathOffset (Tree, "/reserved-memory");
  SetCell (Tree, Root, "#size-cells", 1);
  Fixed = FdtPathOffset (Tree, "/reserved-memory/framebuffer@100000");
  Reg[1] = ToBigEndian32 (TEST_FB_BASE);
  Reg[2] = ToBigEndian32 (TEST_FB_SIZE);
  CHECK (FdtSetProp (Tree, Fixed, "reg", Reg, 3 * sizeof (UINT32)) == 0);
  Pool = FdtAddSubnode (Tree, Root, "os-pool");
  CHECK (Pool >= 0);
  CHECK (FdtSetProp (Tree, Pool, "size", Size + 1, sizeof (UINT32)) == 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_SUCCESS);
  SetCell (Tree, Pool, "size", 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_COMPROMISED_DATA);
}

static void TestPaddedScanlines(void) {
  UINT8 Tree[TEST_FDT_CAPACITY]; MTK_DISPLAY_FRAMEBUFFER F;
  UINTN Size=MakeFramebufferTree(Tree,TRUE,TRUE,FALSE,"x8r8g8b8",260,TEST_FB_SIZE);
  INT32 Node=FdtPathOffset(Tree,"/chosen/framebuffer@100000");CHECK(Node>=0);
  SetCell(Tree,Node,"height",TEST_HEIGHT-1);
  CHECK(MtkDisplayParseHandoff(Tree,Size,&F)==EFI_SUCCESS);
  CHECK(F.PixelsPerScanLine==65 && F.StrideBytes==260 && F.FrameBufferSize==260*(TEST_HEIGHT-1));
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Frame[65*TEST_HEIGHT],Color={1,2,3,4},Out[64*TEST_HEIGHT];
  memset(Frame,0xa5,sizeof(Frame)); memset(Out,0,sizeof(Out));
  CHECK(MtkDisplayBlt(&F,Frame,sizeof(Frame),&Color,EfiBltVideoFill,0,0,0,0,64,TEST_HEIGHT-1,0)==EFI_SUCCESS);
  for (UINTN y=0;y<TEST_HEIGHT-1;y++) {
    CHECK(Frame[y*65].Blue==1 && Frame[y*65+63].Red==3 && Frame[y*65+64].Blue==0xa5);
  }
  CHECK(MtkDisplayBlt(&F,Frame,sizeof(Frame),Out,EfiBltVideoToBltBuffer,0,0,0,0,64,TEST_HEIGHT-1,0)==EFI_SUCCESS);
  CHECK(Out[(TEST_HEIGHT-1)*64-1].Green==2);
  CHECK(MtkDisplayBlt(&F,Frame,sizeof(Frame),NULL,EfiBltVideoToVideo,0,0,0,1,64,TEST_HEIGHT-2,0)==EFI_SUCCESS);
  for (UINTN y=0;y<TEST_HEIGHT-1;y++) CHECK(Frame[y*65+64].Blue==0xa5);
}

static void
TestBltOperationsAndBounds (
  VOID
  )
{
  MTK_DISPLAY_FRAMEBUFFER FrameBuffer;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Frame[TEST_WIDTH * TEST_HEIGHT];
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Buffer[8];
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;
  EFI_STATUS Status;
  UINTN Index;

  memset (&FrameBuffer, 0, sizeof (FrameBuffer));
  FrameBuffer.FrameBufferBase = TEST_FB_BASE;
  FrameBuffer.FrameBufferRegionSize = TEST_FB_SIZE;
  FrameBuffer.Width = TEST_WIDTH;
  FrameBuffer.Height = TEST_HEIGHT;
  FrameBuffer.StrideBytes = TEST_WIDTH * sizeof (Frame[0]);
  FrameBuffer.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  CHECK (MtkDisplayValidateLayout (&FrameBuffer) == EFI_SUCCESS);
  memset (Frame, 0, sizeof (Frame));
  memset (Buffer, 0, sizeof (Buffer));

  Color.Blue = 0x11;
  Color.Green = 0x22;
  Color.Red = 0x33;
  Color.Reserved = 0x44;
  Status = MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), &Color,
                          EfiBltVideoFill, 0, 0, 1, 1, 2, 1, 0);
  CHECK (Status == EFI_SUCCESS);
  CHECK (Frame[TEST_WIDTH + 1].Blue == 0x11 && Frame[TEST_WIDTH + 2].Red == 0x33);
  CHECK (Frame[TEST_WIDTH].Blue == 0);

  Buffer[0] = Color;
  Buffer[1] = Color;
  Status = MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), Buffer,
                          EfiBltBufferToVideo, 0, 0, 4, 0, 2, 1, 0);
  CHECK (Status == EFI_SUCCESS);
  CHECK (Frame[4].Green == 0x22 && Frame[5].Reserved == 0x44);

  Status = MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), Buffer,
                          EfiBltVideoToBltBuffer, 4, 0, 1, 0, 2, 1, 16);
  CHECK (Status == EFI_SUCCESS);
  CHECK (Buffer[1].Blue == 0x11 && Buffer[2].Red == 0x33);

  for (Index = 0; Index < 6; ++Index) {
    Frame[TEST_WIDTH + Index].Blue = (UINT8)(Index + 1);
  }
  Status = MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), NULL,
                          EfiBltVideoToVideo, 0, 1, 1, 1, 5, 1, 0);
  CHECK (Status == EFI_SUCCESS);
  for (Index = 0; Index < 5; ++Index) {
    CHECK (Frame[TEST_WIDTH + 1 + Index].Blue == Index + 1);
  }

  for (Index = 0; Index < 4; ++Index) {
    Frame[3 * TEST_WIDTH + Index].Blue = (UINT8)(10 + Index);
    Frame[4 * TEST_WIDTH + Index].Blue = (UINT8)(20 + Index);
  }
  Status = MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), NULL,
                          EfiBltVideoToVideo, 0, 3, 0, 4, 4, 2, 0);
  CHECK (Status == EFI_SUCCESS);
  for (Index = 0; Index < 4; ++Index) {
    CHECK (Frame[4 * TEST_WIDTH + Index].Blue == 10 + Index);
    CHECK (Frame[5 * TEST_WIDTH + Index].Blue == 20 + Index);
  }

  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), &Color,
                        EfiBltVideoFill, 0, 0, TEST_WIDTH - 1, 0, 2, 1, 0) == EFI_INVALID_PARAMETER);
  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), NULL,
                        EfiBltVideoToVideo, 0, 0, 0, 0, 1, 1, 0) == EFI_SUCCESS);
  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), Buffer,
                        EfiBltBufferToVideo, 0, 0, 0, 0, 0, 1, 0) == EFI_INVALID_PARAMETER);
  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), Buffer,
                        EfiBltBufferToVideo, 2, 0, 0, 0, 1, 1, 4) == EFI_INVALID_PARAMETER);
  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), Buffer,
                        EfiBltBufferToVideo, 0, 0, 0, 0, 1, 1, 2) == EFI_INVALID_PARAMETER);
  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), Buffer,
                        EfiBltBufferToVideo, 0, 0, 0, 0, 1, 1, 0) == EFI_SUCCESS);

  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame) - 1, &Color,
                        EfiBltVideoFill, 0, 0, 0, 0, 1, 1, 0) == EFI_INVALID_PARAMETER);
  FrameBuffer.StrideBytes = FrameBuffer.Width * 4 + 4;
  CHECK (MtkDisplayBlt (&FrameBuffer, Frame, sizeof (Frame), &Color,
                        EfiBltVideoFill, 0, 0, 0, 0, 1, 1, 0) == EFI_INVALID_PARAMETER);
}

static void
TestFixedReservationTuples (void)
{
  UINT8 Tree[TEST_FDT_CAPACITY], Before[TEST_FDT_CAPACITY];
  MTK_DISPLAY_FRAMEBUFFER FrameBuffer;
  UINT32 Reg[8] = {0, ToBigEndian32 (0x90000000), 0, ToBigEndian32 (0x2000),
                  0, ToBigEndian32 (TEST_FB_BASE), 0, ToBigEndian32 (TEST_FB_SIZE)};
  for (BOOLEAN Phandle = FALSE; Phandle <= TRUE; ++Phandle) {
    MakeFramebufferTree (Tree, TRUE, TRUE, Phandle, "x8r8g8b8", 256, TEST_FB_SIZE);
    CHECK (FdtOpenInto (Tree, Tree, sizeof (Tree)) == 0);
    INT32 Pool = FdtPathOffset (Tree, "/reserved-memory/framebuffer@100000");
    CHECK (Pool >= 0);
    CHECK (FdtSetProp (Tree, Pool, "reg", Reg, sizeof (Reg)) == 0);
    memcpy (Before, Tree, sizeof (Tree));
    CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_SUCCESS);
    CHECK (FrameBuffer.FrameBufferBase == TEST_FB_BASE && FrameBuffer.ProtectedSize == TEST_FB_SIZE);
    CHECK (memcmp (Before, Tree, sizeof (Tree)) == 0);
    // A matched first tuple cannot hide a malformed second one.
    UINT32 Bad[8];
    memcpy (Bad, Reg + 4, 4 * sizeof (UINT32));
    memcpy (Bad + 4, Reg, 4 * sizeof (UINT32));
    Bad[7] = 0;
    CHECK (FdtSetProp (Tree, Pool, "reg", Bad, sizeof (Bad)) == 0);
    CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_COMPROMISED_DATA);
    Bad[4] = Bad[5] = ToBigEndian32 (MAX_UINT32);
    Bad[7] = ToBigEndian32 (2);
    CHECK (FdtSetProp (Tree, Pool, "reg", Bad, sizeof (Bad)) == 0);
    CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_COMPROMISED_DATA);
    CHECK (FdtSetProp (Tree, Pool, "reg", Reg, sizeof (Reg) - 1) == 0);
    CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_COMPROMISED_DATA);
  }
  MakeFramebufferTree (Tree, TRUE, TRUE, FALSE, "x8r8g8b8", 256, TEST_FB_SIZE);
  CHECK (FdtOpenInto (Tree, Tree, sizeof (Tree)) == 0);
  INT32 Root = FdtPathOffset (Tree, "/reserved-memory");
  INT32 Other = FdtAddSubnode (Tree, Root, "unrelated-two-ranges");
  CHECK (Other >= 0);
  Reg[5] = ToBigEndian32 (0x91000000);
  CHECK (FdtSetProp (Tree, Other, "reg", Reg, sizeof (Reg)) == 0);
  CHECK (MtkDisplayParseHandoff (Tree, sizeof (Tree), &FrameBuffer) == EFI_SUCCESS);
}

static void
StoreLe32 (UINT8 *Bytes, UINT32 Value)
{
  for (UINTN Index = 0; Index < 4; ++Index) {
    Bytes[Index] = (UINT8)(Value >> (Index * 8));
  }
}

static void
MakeOvlRegisters (UINT8 *Page, UINT32 Layer)
{
  memset (Page, 0, 0x1000);
  StoreLe32 (Page + 0x0C, 0x400001);
  StoreLe32 (Page + 0x20, (TEST_HEIGHT << 16) | TEST_WIDTH);
  StoreLe32 (Page + 0x24, 0x5000001);
  StoreLe32 (Page + 0x2C, 1U << Layer);
  StoreLe32 (Page + 0x30 + Layer * 0x20, 0x21FF);
  StoreLe32 (Page + 0x38 + Layer * 0x20, (TEST_HEIGHT << 16) | TEST_WIDTH);
  StoreLe32 (Page + 0x44 + Layer * 0x20, 260);
  StoreLe32 (Page + 0xC0 + Layer * 0x20, 1);
  StoreLe32 (Page + 0xF40 + Layer * 0x20, 0x90000100);
}

static void
CheckOvlFailure (UINT32 Base, CONST UINT8 *Page, UINTN Size, EFI_STATUS Expected)
{
  MTK_DISPLAY_OVL_PLANE Plane, Before;
  UINT8 Original[0x1000];
  memcpy (Original, Page, sizeof (Original));
  memset (&Plane, 0xA5, sizeof (Plane));
  Before = Plane;
  CHECK (MtkDisplayDecodeLkOvl (Base, Page, Size, &Plane) == Expected);
  CHECK (memcmp (&Plane, &Before, sizeof (Plane)) == 0);
  CHECK (memcmp (Page, Original, sizeof (Original)) == 0);
}

static void
TestLkOvlDecode (void)
{
  UINT8 Storage[0x1001];
  UINT8 *Page = Storage + 1;  // A copied snapshot need not be word-aligned.
  MTK_DISPLAY_OVL_PLANE Plane;
  static const struct { UINT32 Offset, Value; EFI_STATUS Status; } Bad[] = {
    {0x0C, 0, EFI_NOT_READY},
    {0x2C, 0, EFI_NOT_READY},
    {0x2C, 3, EFI_UNSUPPORTED},
    {0x2C, 0x10, EFI_UNSUPPORTED},
    {0x2C, 0x10001, EFI_UNSUPPORTED},
    {0x24, 0x5000011, EFI_UNSUPPORTED},
    {0x24, 0x5000081, EFI_UNSUPPORTED},
    {0x324, 1, EFI_UNSUPPORTED},
    {0x30, 0x11FF, EFI_UNSUPPORTED},
    {0x30, 0x31FF, EFI_UNSUPPORTED},
    {0x30, 0x10021FF, EFI_UNSUPPORTED},
    {0x30, 0x8021FF, EFI_UNSUPPORTED},
    {0x30, 0x100021FF, EFI_UNSUPPORTED},
    {0x30, 0x400021FF, EFI_UNSUPPORTED},
    {0x30, 0x27FF, EFI_UNSUPPORTED},
    {0x3C, 1, EFI_UNSUPPORTED},
    {0x4C, 1, EFI_UNSUPPORTED},
    {0x40, 1, EFI_UNSUPPORTED},
    {0x2D0, 1, EFI_UNSUPPORTED},
    {0x44, 0x20000104, EFI_UNSUPPORTED},
    {0x44, 0x10000, EFI_UNSUPPORTED},
    {0x44, 255, EFI_COMPROMISED_DATA},
    {0x44, 258, EFI_COMPROMISED_DATA},
    {0x44, 0, EFI_COMPROMISED_DATA},
    {0xC0, 0, EFI_NOT_READY},
    {0xC0, 2, EFI_NOT_READY},
    {0x20, (TEST_HEIGHT << 16) | (TEST_WIDTH + 1), EFI_COMPROMISED_DATA},
    {0x38, (TEST_HEIGHT << 16) | (TEST_WIDTH - 1), EFI_COMPROMISED_DATA},
    {0xF40, 0, EFI_COMPROMISED_DATA},
    {0xF40, 0x90000102, EFI_COMPROMISED_DATA},
    {0xF40, 0xFFFFF000, EFI_COMPROMISED_DATA}
  };
  for (UINT32 Controller = 0; Controller < 2; ++Controller) {
    UINT32 Base = Controller ? 0x14006000 : 0x14005000;
    UINT32 Count = Controller ? 2 : 4;
    for (UINT32 Layer = 0; Layer < Count; ++Layer) {
      MakeOvlRegisters (Page, Layer);
      CHECK (MtkDisplayDecodeLkOvl (Base, Page, 0x1000, &Plane) == EFI_SUCCESS);
      CHECK (Plane.DmaAddress == 0x90000100 && Plane.StrideBytes == 260);
      CHECK (Plane.Width == TEST_WIDTH && Plane.Height == TEST_HEIGHT);
      CHECK (Plane.LayerIndex == Layer && Plane.AlphaControl == 0x1FF);
      for (UINT32 Alpha = 0; Alpha <= 0x1FF; Alpha += 0xFF) {
        StoreLe32 (Page + 0x30 + Layer * 0x20, 0x2000 | Alpha);
        CHECK (MtkDisplayDecodeLkOvl (Base, Page, 0x1000, &Plane) == EFI_SUCCESS);
        CHECK (Plane.AlphaControl == Alpha);
      }
    }
  }
  MakeOvlRegisters (Page, 0);
  CHECK (MtkDisplayDecodeLkOvl (0x14005000, NULL, 0x1000, &Plane) == EFI_INVALID_PARAMETER);
  CHECK (MtkDisplayDecodeLkOvl (0x14005000, Page, 0x1000, NULL) == EFI_INVALID_PARAMETER);
  CheckOvlFailure (0x14007000, Page, 0x1000, EFI_UNSUPPORTED);
  CheckOvlFailure (0x14005000, Page, 0, EFI_BAD_BUFFER_SIZE);
  CheckOvlFailure (0x14005000, Page, 0xFFF, EFI_BAD_BUFFER_SIZE);
  CheckOvlFailure (0x14005000, Page, 0x1001, EFI_BAD_BUFFER_SIZE);
  MakeOvlRegisters (Page, 2);
  CheckOvlFailure (0x14006000, Page, 0x1000, EFI_UNSUPPORTED);
  for (UINTN Index = 0; Index < ARRAY_SIZE (Bad); ++Index) {
    MakeOvlRegisters (Page, 0);
    StoreLe32 (Page + Bad[Index].Offset, Bad[Index].Value);
    CheckOvlFailure (0x14005000, Page, 0x1000, Bad[Index].Status);
  }
  for (UINT32 Roi = 0; Roi < 4; ++Roi) {
    UINT32 Invalid[] = {0, TEST_WIDTH, TEST_HEIGHT << 16, 0x10001000};
    MakeOvlRegisters (Page, 0);
    StoreLe32 (Page + 0x20, Invalid[Roi]);
    StoreLe32 (Page + 0x38, Invalid[Roi]);
    CheckOvlFailure (0x14005000, Page, 0x1000, EFI_COMPROMISED_DATA);
  }
}

static void
TestLkAcquisition (CONST VOID *StockFdt, UINTN StockSize)
{
  UINT8 Tree[16384], BeforeTree[16384];
  LK_DISPLAY_FIXTURE *Io=malloc(sizeof(*Io)); CHECK(Io);
  MTK_DISPLAY_FRAMEBUFFER Fb, Before;
  static const struct { UINTN Address; UINT32 Value; EFI_STATUS Status; } Bad[]={
    {0x1000616C,0,EFI_NOT_READY}, {0x10006170,0,EFI_NOT_READY},
    {0x10006354,0xF,EFI_NOT_READY}, {0x10006354,0x1D,EFI_NOT_READY},
    {0x10006354,0x10D,EFI_NOT_READY}, {0x10006354,0x100D,EFI_NOT_READY},
    {0x100012D0,0x400,EFI_NOT_READY}, {0x100012EC,0x1000,EFI_NOT_READY},
    {0x10001220,0x40,EFI_NOT_READY}, {0x10001228,0x800000,EFI_NOT_READY},
    {0x14000100,8,EFI_NOT_READY}, {0x14000100,4,EFI_NOT_READY},
    {0x14000100,0x400000,EFI_NOT_READY}, {0x14000100,0x1000000,EFI_NOT_READY},
    {0x140001A0,1,EFI_NOT_READY}, {0x14007010,0,EFI_NOT_READY},
    {0x14007010,3,EFI_NOT_READY}, {0x14013000,0,EFI_NOT_READY},
    {0x14013018,0x1C,EFI_UNSUPPORTED},
    {0x1401301C,0x200C0,EFI_UNSUPPORTED},
    {0x1401301C,0x300C1,EFI_COMPROMISED_DATA},
    {0x1401302C,31,EFI_COMPROMISED_DATA},
    {0x14013038,(31U<<16)|64U,EFI_COMPROMISED_DATA},
    {0x14013038,(32U<<16)|65U,EFI_COMPROMISED_DATA},
    {0x14013014,0,EFI_UNSUPPORTED}, {0x14000F18,0,EFI_UNSUPPORTED},
    {0x14000F18,3,EFI_UNSUPPORTED}, {0x14000F28,1,EFI_UNSUPPORTED},
    {0x14000F0C,1,EFI_UNSUPPORTED}, {0x14000F30,2,EFI_UNSUPPORTED},
    {0x14003384,1,EFI_UNSUPPORTED}, {0x14003388,1,EFI_UNSUPPORTED},
    {0x1400500C,1,EFI_UNSUPPORTED}, {0x14005030,0x21FE,EFI_UNSUPPORTED},
    {0x14005040,1,EFI_UNSUPPORTED}, {0x140052D0,1,EFI_UNSUPPORTED},
    {0x1400504C,1,EFI_UNSUPPORTED}, {0x1400502C,3,EFI_UNSUPPORTED},
    {0x14007014,65,EFI_COMPROMISED_DATA}, {0x14007018,31,EFI_COMPROMISED_DATA},
    {0x14005F40,0x90020000,EFI_ACCESS_DENIED},
    {0x14005F40,0x90000004,EFI_COMPROMISED_DATA}
  };
  for (UINT32 C=0;C<2;C++) for (UINT32 L=0;L<(C?2U:4U);L++) for (UINT32 Color=0;Color<2;Color++) {
    UINT32 Base=C?0x14006000:0x14005000;
    LkFixtureTree(Tree,sizeof(Tree)); memcpy(BeforeTree,Tree,sizeof(Tree));
    LkFixtureInitialize(Io,Base,L,(BOOLEAN)Color,FALSE);
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,NULL,Io,&Fb)==EFI_UNSUPPORTED);
    CHECK(Io->Writes==0);
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_SUCCESS);
    CHECK(Io->Writes==1 && *LkFixtureRegister(Io,Base+0x44+L*0x20)==0x10000104);
    CHECK(Fb.FrameBufferBase==0x90000000 && Fb.Width==64 && Fb.Height==32);
    CHECK(Fb.StrideBytes==260 && Fb.FrameBufferSize==8320 && Fb.ProtectedSize==0x3000);
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,NULL,Io,&Fb)==EFI_SUCCESS);
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_SUCCESS);
    CHECK(Io->Writes==1 && memcmp(Tree,BeforeTree,sizeof(Tree))==0);
  }
  for (UINTN I=0;I<ARRAY_SIZE(Bad);I++) {
    LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
    *LkFixtureRegister(Io,Bad[I].Address)=Bad[I].Value;
    memset(&Fb,0xA5,sizeof(Fb)); Before=Fb;
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==Bad[I].Status);
    CHECK(Io->Writes==0 && memcmp(&Fb,&Before,sizeof(Fb))==0);
    if (Bad[I].Address<0x14000000) CHECK(Io->DisplayReads==0);
  }
  // Every captured DSI control must survive both snapshots and the final
  // alpha preparation; an error or a change must preserve the caller output.
  static const UINT32 DsiOffsets[]={0x18,0x1C,0x20,0x24,0x28,0x2C,0x38,0x50,0x54,0x58,0x5C};
  for (UINTN I=0;I<ARRAY_SIZE(DsiOffsets);I++) {
    for (UINTN Visit=1;Visit<=3;Visit++) {
      LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
      Io->FailAddress=0x14013000+DsiOffsets[I]; Io->FailVisit=Visit;
      memset(&Fb,0xA5,sizeof(Fb)); Before=Fb;
      CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_DEVICE_ERROR);
      CHECK(Io->Writes==(Visit==3?2U:0U) && memcmp(&Fb,&Before,sizeof(Fb))==0);
      CHECK(*LkFixtureRegister(Io,0x14005044)==260);
    }
    for (UINTN Visit=2;Visit<=3;Visit++) {
      LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
      Io->ChangeAddress=0x14013000+DsiOffsets[I]; Io->ChangeVisit=Visit;
      Io->ChangeValue=*LkFixtureRegister(Io,Io->ChangeAddress)^1U;
      memset(&Fb,0xA5,sizeof(Fb)); Before=Fb;
      CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_NOT_READY);
      CHECK(Io->Writes==(Visit==3?2U:0U) && memcmp(&Fb,&Before,sizeof(Fb))==0);
      CHECK(*LkFixtureRegister(Io,0x14005044)==260);
    }
  }
  LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
  Io->ChangeAddress=0x14013014; Io->ChangeVisit=4; Io->ChangeValue=2;
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_NOT_READY);
  CHECK(Io->Writes==0);
  for (UINTN I=1;I<=3;I++) {
    LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE); Io->WriteMode=I;
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_DEVICE_ERROR);
    CHECK(Io->Writes==(I==3?2U:1U) && *LkFixtureRegister(Io,0x14005044)==260);
  }
  // A post-write pitch change on the same controller must not be overwritten
  // by restoring the stale pre-write register value.
  LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
  Io->ChangeAddress=0x14005044; Io->ChangeVisit=3; Io->ChangeValue=0x10000108;
  memset(&Fb,0xA5,sizeof(Fb)); Before=Fb;
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_DEVICE_ERROR);
  CHECK(Io->Writes==1 && *LkFixtureRegister(Io,0x14005044)==0x10000108);
  CHECK(memcmp(&Fb,&Before,sizeof(Fb))==0);

  // If another owner has already restored our flag, do not write again.
  LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
  Io->ChangeAddress=0x14005044; Io->ChangeVisit=4; Io->ChangeValue=260;
  memset(&Fb,0xA5,sizeof(Fb)); Before=Fb;
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_NOT_READY);
  CHECK(Io->Writes==1 && *LkFixtureRegister(Io,0x14005044)==260);
  CHECK(memcmp(&Fb,&Before,sizeof(Fb))==0);

  // If the register cannot be read before rollback, retain our changed value
  // and fail closed instead of issuing an unverified stale write.
  LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
  Io->ChangeAddress=0x14005030; Io->ChangeVisit=3; Io->ChangeValue=0x21FE;
  Io->FailAddress=0x14005044; Io->FailVisit=5;
  memset(&Fb,0xA5,sizeof(Fb)); Before=Fb;
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_DEVICE_ERROR);
  CHECK(Io->Writes==1 && *LkFixtureRegister(Io,0x14005044)==0x10000104);
  CHECK(memcmp(&Fb,&Before,sizeof(Fb))==0);

  // A changing plane or a failing register read must not reach the alpha write.
  LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
  Io->ChangeAddress=0x14005F40; Io->ChangeVisit=2; Io->ChangeValue=0x90001000;
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_NOT_READY);
  CHECK(Io->Writes==0);
  LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE); Io->FailAddress=0x14005030; Io->FailVisit=1;
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_DEVICE_ERROR);
  CHECK(Io->Writes==0);
  for (UINTN I=0;I<5;I++) {
    LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
    INT32 Chosen=FdtPathOffset(Tree,"/chosen"); CHECK(Chosen>=0);
    if (I==0) LkFixtureCell(Tree,Chosen,"atag,videolfb-islcm_inited",2);
    if (I==1) LkFixtureCell(Tree,Chosen,"atag,videolfb-vramSize",0);
    if (I==2) LkFixtureCell(Tree,Chosen,"atag,videolfb-fb_base_l",0xB0000000);
    if (I==3) CHECK(FdtDelProp(Tree,Chosen,"atag,videolfb-fb_base_h")==0);
    if (I==4) {
      INT32 N=FdtPathOffset(Tree,"/ovl0@14005000"); CHECK(N>=0);
      CHECK(FdtSetPropString(Tree,N,"status","disabled")==0);
    }
    CHECK(EFI_ERROR(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)));
    CHECK(Io->Reads==0 && Io->Writes==0);
  }
  // A nested identity bus is valid; duplicates, translation, disabled paths
  // and an extra compatible on the same node still fail before any MMIO.
  for (UINTN Mode=0;Mode<9;Mode++) {
    LkFixtureTree(Tree,sizeof(Tree));LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
    INT32 Old=FdtPathOffset(Tree,"/ovl0@14005000");CHECK(Old>=0);
    if (Mode!=1) CHECK(FdtDelNode(Tree,Old)==0);
    INT32 Bus=FdtAddSubnode(Tree,0,"graphics-bus");CHECK(Bus>=0);
    LkFixtureCell(Tree,Bus,"#address-cells",2);LkFixtureCell(Tree,Bus,"#size-cells",2);
    CHECK(FdtSetPropEmpty(Tree,Bus,"ranges")==0);
    if (Mode==2) CHECK(FdtSetPropString(Tree,Bus,"status","disabled")==0);
    if (Mode==3) LkFixtureCell(Tree,Bus,"ranges",0);
    if (Mode==5) CHECK(FdtDelProp(Tree,Bus,"ranges")==0);
    if (Mode==8) for (UINTN Level=0;Level<32;Level++) {
      Bus=FdtAddSubnode(Tree,Bus,"bus");CHECK(Bus>=0);
      LkFixtureCell(Tree,Bus,"#address-cells",2);LkFixtureCell(Tree,Bus,"#size-cells",2);
      CHECK(FdtSetPropEmpty(Tree,Bus,"ranges")==0);
    }
    if (Mode!=7) {
      INT32 Child=FdtAddSubnode(Tree,Bus,"ovl@14005000");CHECK(Child>=0);
      CHECK(FdtSetPropString(Tree,Child,"compatible","mediatek,disp_ovl0")==0);
      LkFixtureReg(Tree,Child,Mode==6?0x14005004:0x14005000,0x1000);
      if (Mode==4) {
        STATIC CONST CHAR8 Two[]="mediatek,disp_ovl0\0mediatek,disp_rdma0";
        CHECK(FdtSetProp(Tree,Child,"compatible",Two,sizeof(Two))==0);
      }
    }
    EFI_STATUS Status=MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb);
    if (Mode==0) CHECK(Status==EFI_SUCCESS && Io->Writes==1 && Io->Reads>0);
    else CHECK(EFI_ERROR(Status) && Io->Writes==0 && Io->Reads==0);
  }
  // Offline metadata cannot bypass the live power guard or supply geometry.
  LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE); Io->Spm[0x16C/4]=0;
  CHECK(EFI_ERROR(MtkDisplayAcquireLkHandoff(StockFdt,StockSize,LkFixtureRead,LkFixtureWrite,Io,&Fb)));
  CHECK(Io->Reads<=1 && Io->Writes==0);
  // Exercise the shipping hardware-node layout, with only modeled LK RAM/video fixups.
  UINTN LiveSize=StockSize+32768;
  VOID *Live=malloc(LiveSize); CHECK(Live);
  CHECK(FdtOpenInto(StockFdt,Live,(INT32)LiveSize)==0);
  INT32 Chosen=FdtPathOffset(Live,"/chosen"); CHECK(Chosen>=0);
  LkFixtureCell(Live,Chosen,"atag,videolfb-fb_base_h",0);
  LkFixtureCell(Live,Chosen,"atag,videolfb-fb_base_l",0x90000000);
  LkFixtureCell(Live,Chosen,"atag,videolfb-vramSize",0x20000);
  LkFixtureCell(Live,Chosen,"atag,videolfb-islcmfound",1);
  // Keep the real stock islcm_inited=0; LK never updates this property.
  INT32 Ram=FdtAddSubnode(Live,0,"memory@80000000"); CHECK(Ram>=0);
  CHECK(FdtSetPropString(Live,Ram,"device_type","memory")==0);
  LkFixtureReg(Live,Ram,0x80000000,0x20000000);
  LkFixtureInitialize(Io,0x14005000,0,FALSE,FALSE);
  CHECK(MtkDisplayAcquireLkHandoff(Live,LiveSize,LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_SUCCESS);
  CHECK(Io->Writes==1 && Fb.FrameBufferBase==0x90000000);
  LkFixtureCell(Live,FdtPathOffset(Live,"/chosen"),"atag,videolfb-islcm_inited",1);
  CHECK(MtkDisplayAcquireLkHandoff(Live,LiveSize,LkFixtureRead,NULL,Io,&Fb)==EFI_SUCCESS);
  free(Live);
  CHECK(MtkDisplayAcquireLkHandoff(NULL,0,LkFixtureRead,NULL,Io,&Fb)==EFI_INVALID_PARAMETER);
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),NULL,NULL,Io,&Fb)==EFI_INVALID_PARAMETER);
  // Replay actual tablet OVL values: the old literal-zero/one guards failed.
  LkFixturePhysical18Tree(Tree,sizeof(Tree)); LkFixturePhysical18(Io);
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_SUCCESS);
  CHECK(Fb.Width==2240 && Fb.Height==1400 && Fb.StrideBytes==8960);
  CHECK(Fb.FrameBufferBase==0xFE648000 && Io->Writes==1);
  CHECK(*LkFixtureRegister(Io,0x14005064)==0x10002300);
  CHECK(*LkFixtureRegister(Io,0x14005060)==0x100000);
  CHECK(*LkFixtureRegister(Io,0x140050E0)==0x5A0001);
  // FIFO consumption changes during a live scanout; configuration must not.
  LkFixturePhysical18(Io);Io->ChangeAddress=0x140050E0;Io->ChangeVisit=2;Io->ChangeValue=0x600001;
  CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_SUCCESS);
  CHECK(Io->Writes==1 && *LkFixtureRegister(Io,0x140050E0)==0x600001);
  for(UINTN I=0;I<2;I++) {
    LkFixturePhysical18(Io);Io->ChangeAddress=0x140050E0;Io->ChangeVisit=2;
    Io->ChangeValue=I==0?0x600000:0x600003;Before=Fb;
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==EFI_NOT_READY);
    CHECK(Io->Writes==0 && memcmp(&Fb,&Before,sizeof(Fb))==0);
  }
  for(UINTN I=0;I<4;I++) {
    LkFixturePhysical18(Io); Before=Fb;
    *LkFixtureRegister(Io,(UINTN[]){0x14005060,0x14005060,0x140050E0,0x14005024}[I])=
      (UINT32[]){0x100001,0x110000,0x5A0000,0x7000025}[I];
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,LkFixtureWrite,Io,&Fb)==
          (I==2?EFI_NOT_READY:EFI_UNSUPPORTED));
    CHECK(Io->Writes==0 && memcmp(&Fb,&Before,sizeof(Fb))==0);
  }
  free(Io);
}

static void
TestOriginalLkDsiSnapshots (FILE *File)
{
  UINT8 Magic[8], Page[0x1000], Tree[16384];
  UINT32 Count, Header[3];
  LK_DISPLAY_FIXTURE *Io=malloc(sizeof(*Io));
  MTK_DISPLAY_FRAMEBUFFER Fb;
  CHECK(Io!=NULL);
  CHECK(fread(Magic,1,sizeof(Magic),File)==sizeof(Magic));
  CHECK(memcmp(Magic,"LKDSI001",8)==0);
  CHECK(fread(&Count,sizeof(Count),1,File)==1 && Count==2);
  for (UINT32 I=0;I<Count;I++) {
    CHECK(fread(Header,1,sizeof(Header),File)==sizeof(Header));
    CHECK(Header[1]==2240 && Header[2]==1400);
    CHECK(fread(Page,1,sizeof(Page),File)==sizeof(Page));
    LkFixtureTree(Tree,sizeof(Tree));
    LkFixtureCell(Tree,FdtPathOffset(Tree,"/chosen"),"atag,videolfb-vramSize",0x1000000);
    LkFixtureInitialize(Io,0x14005000,0,FALSE,TRUE);
    memcpy(LkFixtureRegister(Io,0x14013000),Page,sizeof(Page));
    // DSI_config does not start the link. These two inherited controls are
    // modeled separately; geometry/packing/lane/timing words remain original.
    *LkFixtureRegister(Io,0x14013000)=1;
    *LkFixtureRegister(Io,0x14013014)=1;
    *LkFixtureRegister(Io,0x14005020)=(1400U<<16)|2240U;
    *LkFixtureRegister(Io,0x14005038)=(1400U<<16)|2240U;
    *LkFixtureRegister(Io,0x14005044)=0x10000000U|8960U;
    *LkFixtureRegister(Io,0x14007014)=2240;
    *LkFixtureRegister(Io,0x14007018)=1400;
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,NULL,Io,&Fb)==EFI_SUCCESS);
    CHECK(Fb.Width==2240 && Fb.Height==1400 && Fb.StrideBytes==8960 && Io->Writes==0);
    *LkFixtureRegister(Io,0x1401301C)^=0x10000;
    CHECK(MtkDisplayAcquireLkHandoff(Tree,sizeof(Tree),LkFixtureRead,NULL,Io,&Fb)==EFI_UNSUPPORTED);
    CHECK(Io->Writes==0);
  }
  free(Io);
  printf("Original LK DSI configurations: %u accepted, altered packing rejected by production handoff\\n",Count);
}

static void
TestOriginalLkOvlSnapshots (CONST CHAR8 *Path)
{
  FILE *File = fopen (Path, "rb");
  UINT8 Magic[8], Page[0x1000], Before[0x1000];
  UINT32 Count, Header[8];
  MTK_DISPLAY_OVL_PLANE Plane, Unchanged;
  CHECK (File != NULL);
  CHECK (fread (Magic, 1, sizeof (Magic), File) == sizeof (Magic));
  CHECK (memcmp (Magic, "LKOVL002", 8) == 0);
  CHECK (fread (&Count, sizeof (Count), 1, File) == 1 && Count > 0 && Count <= 4096);
  for (UINT32 Index = 0; Index < Count; ++Index) {
    CHECK (fread (Header, 1, sizeof (Header), File) == sizeof (Header));
    CHECK (fread (Page, 1, sizeof (Page), File) == sizeof (Page));
    CHECK (Header[1] <= 1);
    memcpy (Before, Page, sizeof (Before));
    memset (&Plane, 0xA5, sizeof (Plane));
    Unchanged = Plane;
    CHECK (MtkDisplayDecodeLkOvl (Header[0], Page, sizeof (Page), &Plane) ==
           (Header[1] == 0 ? EFI_SUCCESS : EFI_UNSUPPORTED));
    if (Header[1] == 0) {
      CHECK (Plane.DmaAddress==Header[2] && Plane.Width==Header[3] && Plane.Height==Header[4]);
      CHECK (Plane.StrideBytes==Header[5] && Plane.LayerIndex==Header[6] && Plane.AlphaControl==Header[7]);
      CHECK (!Plane.Opaque);
    } else {
      CHECK (memcmp (&Plane, &Unchanged, sizeof (Plane)) == 0);
    }
    CHECK (memcmp (Page, Before, sizeof (Page)) == 0);
  }
  TestOriginalLkDsiSnapshots (File);
  CHECK (fgetc (File) == EOF);
  CHECK (fclose (File) == 0);
  printf ("Original LK OVL snapshots: %u decoded/rejected using production C\n", Count);
}

static void TestFullResolutionFillCost(void) {
  MTK_DISPLAY_FRAMEBUFFER F={0};
  const UINTN Width=2240U, Height=1400U, Pitch=(Width+1U)*4U, Bytes=Pitch*Height;
  UINT8 *Raw=malloc(Bytes+16U);CHECK(Raw);
  UINT8 *Frame=Raw+8U;EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color={0x11,0x22,0x33,0x44};
  UINT32 Expected;memcpy(&Expected,&Color,sizeof(Expected));
  F.FrameBufferBase=TEST_FB_BASE;F.FrameBufferRegionSize=Bytes;
  F.Width=Width;F.Height=Height;F.StrideBytes=Pitch;F.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;
  CHECK(MtkDisplayValidateLayout(&F)==EFI_SUCCESS);memset(Raw,0xa5,Bytes+16U);
  TestCopyMemCalls=TestSetMem32Calls=0;
  CHECK(MtkDisplayBlt(&F,Frame,Bytes,&Color,EfiBltVideoFill,0,0,0,0,Width,Height,0)==EFI_SUCCESS);
  printf("Full-resolution fill: CopyMem=%lu SetMem32=%lu pixels=%lu rows=%lu\n",
    (unsigned long)TestCopyMemCalls,(unsigned long)TestSetMem32Calls,(unsigned long)(Width*Height),(unsigned long)Height);fflush(stdout);
  CHECK(TestCopyMemCalls<=Height+1U && TestSetMem32Calls<=Height);
  for(UINTN y=0;y<Height;++y) {
    for(UINTN x=0;x<Width;++x)CHECK(((UINT32 *)(Frame+y*Pitch))[x]==Expected);
    CHECK(((UINT32 *)(Frame+y*Pitch))[Width]==0xa5a5a5a5U);
  }
  for(UINTN i=0;i<8U;++i)CHECK(Raw[i]==0xa5 && Raw[Bytes+8U+i]==0xa5);
  memset(Raw,0xa5,Bytes+16U);TestCopyMemCalls=TestSetMem32Calls=0;
  CHECK(MtkDisplayBlt(&F,Raw+9U,Bytes,&Color,EfiBltVideoFill,0,0,1,1,2,2,0)==EFI_SUCCESS);
  CHECK(TestSetMem32Calls==0U && TestCopyMemCalls==5U);
  for(UINTN y=1;y<3U;++y)for(UINTN x=1;x<3U;++x)
    CHECK(memcmp(Raw+9U+y*Pitch+x*4U,&Color,4U)==0);
  CHECK(Raw[9U+Pitch+3U]==0xa5 && Raw[9U+Pitch+12U]==0xa5);
  free(Raw);
}

int
main (
  int Argc,
  char **Argv
  )
{
  FILE *File;
  long Size;
  VOID *StockFdt;

  CHECK (Argc == 2 || Argc == 3);
  File = fopen (Argv[1], "rb");
  CHECK (File != NULL);
  CHECK (fseek (File, 0, SEEK_END) == 0);
  Size = ftell (File);
  CHECK (Size > 0);
  rewind (File);
  StockFdt = malloc ((UINTN)Size);
  CHECK (StockFdt != NULL);
  CHECK (fread (StockFdt, 1, (UINTN)Size, File) == (UINTN)Size);
  fclose (File);
  CHECK (FdtTotalSize (StockFdt) == (UINTN)Size);
  CHECK (FdtCheckFull (StockFdt, (UINTN)Size) == 0);

  TestRuntimeHandoffAndReservations (StockFdt, (UINTN)Size);
  TestFullResolutionFillCost();
  TestBltOperationsAndBounds ();
  TestPaddedScanlines();
  TestDynamicReservations ();
  TestFixedReservationTuples ();
  TestLkOvlDecode ();
  TestLkAcquisition (StockFdt, (UINTN)Size);
  if (Argc == 3) { TestOriginalLkOvlSnapshots (Argv[2]); }
  free (StockFdt);
  printf ("Display core: %u checks passed (runtime handoff, reservation, memory-map overlap, GOP BLT bounds/overlap and LK OVL decoding)\n", Checks);
  return 0;
}
