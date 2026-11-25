// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_DISPLAY_CORE_H
#define MTK_DISPLAY_CORE_H
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

typedef struct {
  EFI_PHYSICAL_ADDRESS FrameBufferBase;
  UINT64 FrameBufferRegionSize;
  UINT64 ProtectedSize;
  UINTN FrameBufferSize;
  UINT32 Width;
  UINT32 Height;
  UINT32 StrideBytes;
  UINT32 PixelsPerScanLine;
  EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
} MTK_DISPLAY_FRAMEBUFFER;

/* A decoded OVL plane is a device bus address, not a CPU framebuffer. The
 * board must still establish clocks, routing, DMA translation and opaque pixel
 * output before it may construct a GOP handoff from these values. */
typedef struct {
  UINT32 DmaAddress;
  UINT32 Width;
  UINT32 Height;
  UINT32 StrideBytes;
  UINT32 LayerIndex;
  UINT32 AlphaControl;
  BOOLEAN Opaque;
} MTK_DISPLAY_OVL_PLANE;

/* Decode a copied MT6789 OVL0/OVL0_2L register page. No MMIO is performed.
 * Accept a single full-ROI, unrotated, uncompressed format-2 plane; retain the
 * alpha control and reject unknown pitch flags; do not assume its fourth byte
 * is ignored.
 * Success does not establish that this OVL is routed to the physical panel. */
EFI_STATUS
EFIAPI
MtkDisplayDecodeLkOvl (
  IN UINT32 ControllerBase,
  IN CONST VOID *RegisterPage,
  IN UINTN RegisterPageSize,
  OUT MTK_DISPLAY_OVL_PLANE *Plane
  );

typedef EFI_STATUS (EFIAPI *MTK_DISPLAY_READ32) (
  IN VOID *Context,
  IN UINTN Address,
  OUT UINT32 *Value
  );

typedef EFI_STATUS (EFIAPI *MTK_DISPLAY_WRITE32) (
  IN VOID *Context,
  IN UINTN Address,
  IN UINT32 Value
  );

/* Acquire the retained MT6789 LK plane. Power/bus/clock, routing, SMI bypass,
 * stable working registers and runtime VRAM/RAM bounds are mandatory. Before
 * memory publication, Write32 may prepare only the existing plane's constant
 * pixel-alpha bit, with readback; NULL requests an already-opaque read-only
 * handoff. No clocks, power, timing, DMA addresses or DT are changed here. */
EFI_STATUS
EFIAPI
MtkDisplayAcquireLkHandoff (
  IN CONST VOID *Fdt,
  IN UINTN FdtSize,
  IN MTK_DISPLAY_READ32 Read32,
  IN MTK_DISPLAY_WRITE32 Write32 OPTIONAL,
  IN VOID *Context,
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  );

/* Parse a live /chosen simple-framebuffer node and require a matching reserved
 * range in the same runtime FDT. Only the Windows-compatible BGRX8 layout is
 * accepted because the T70 runtime pixel layout has not been observed. */
// Inspect the live plane without changing blend. A pixel overlay uses alpha 0xff.
EFI_STATUS
EFIAPI
MtkDisplayInspectLkHandoff (
  IN CONST VOID *Fdt,
  IN UINTN FdtSize,
  IN MTK_DISPLAY_READ32 Read32,
  IN VOID *Context,
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  );

EFI_STATUS
EFIAPI
MtkDisplayParseHandoff (
  IN CONST VOID *Fdt,
  IN UINTN FdtSize,
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  );

EFI_STATUS
EFIAPI
MtkDisplayValidateLayout (
  IN OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  );

/* Require the full page-rounded region to be mapped only as reserved memory
 * or MMIO in the current EFI memory map. DescriptorSize is the GetMemoryMap
 * stride, which can be larger than sizeof(EFI_MEMORY_DESCRIPTOR). */
EFI_STATUS
EFIAPI
MtkDisplayValidateMemoryMap (
  IN CONST MTK_DISPLAY_FRAMEBUFFER *FrameBuffer,
  IN CONST VOID *MemoryMap,
  IN UINTN MemoryMapSize,
  IN UINTN DescriptorSize
  );

/* Implements all UEFI GOP BLT operations for the validated BGRX8 layout. */
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
  );

#endif
