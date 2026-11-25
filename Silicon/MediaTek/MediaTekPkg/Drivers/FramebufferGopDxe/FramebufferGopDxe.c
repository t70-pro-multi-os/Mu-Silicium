// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#include <Library/MtkDisplayCore.h>

#define GOP_SIGNATURE SIGNATURE_32('M', 'T', 'K', 'G')
#define MEMORY_MAP_INITIAL_SIZE (64 * 1024)
#define MEMORY_MAP_MAX_SIZE     (2 * 1024 * 1024)
#define MEMORY_MAP_RETRIES      5
#define FDT_MAX_SIZE            (2 * 1024 * 1024)
#define T70_LK_LOGO_WIDTH       2240U
#define T70_LK_LOGO_HEIGHT      1400U
#define T70_LK_ANDROID_TOP      1038U

typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} MTK_GOP_DEVICE_PATH;

STATIC CONST EFI_GUID mMtkGopDevicePathGuid = {
  0x8a5a05cb, 0x7699, 0x4f3b, { 0xa1, 0x14, 0x52, 0x4d, 0x54, 0x4b, 0x46, 0x42 }
};

typedef struct {
  UINT32 Signature;
  EFI_HANDLE Handle;
  EFI_EVENT ExitBootServicesEvent;
  EFI_GRAPHICS_OUTPUT_PROTOCOL Protocol;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
  MTK_GOP_DEVICE_PATH DevicePath;
  MTK_DISPLAY_FRAMEBUFFER FrameBuffer;
  UINT64 OriginalGcdAttributes;
  UINT64 FrameBufferAttributes;
  BOOLEAN AttributesChanged;
  BOOLEAN BootServicesActive;
  UINT16 SuccessfulSetModeCalls;
  UINT16 SuccessfulBufferToVideoCalls;
} MTK_GOP_CONTEXT;

STATIC MTK_GOP_CONTEXT *mPublishedGop;
STATIC EFI_EVENT mDriverExitEvent;
STATIC EFI_EVENT mRetryEvents[3];
STATIC BOOLEAN mDriverRegistered;
STATIC BOOLEAN mStartInProgress;
STATIC BOOLEAN mExitingBootServices;
STATIC EFI_STATUS mLastStartStatus = EFI_NOT_READY;

// The T70 handoff page is already required by ReadRuntimeFramebuffer.
// Keep a bounded progress record so a RAM trial can report a failed stage.
STATIC UINT32 mTracePhase;
STATIC UINT32 mTraceAttempts;
STATIC UINT64 mTraceDetail;
STATIC UINT64 mEarlyMapStatus;
STATIC UINT64 mEarlyMapMeta;

STATIC
VOID
RecordGopProgress (EFI_STATUS Status)
{
  UINT64 Detail;
  if (MmioRead64 (0x43F7F000) != 0x0000000148303754ULL) return;
  MmioWrite64 (0x43F7F060, 0);
  MmioWrite32 (0x43F7F068, mTracePhase);
  MmioWrite32 (0x43F7F06C, mTraceAttempts);
  MmioWrite64 (0x43F7F070, Status);
  Detail = mTraceDetail;
  // Phase 8 bit 63 identifies bounded activity counters without enlarging
  // the existing own-RAM record. Earlier phases keep their original detail.
  if ((mTracePhase == 8) && !EFI_ERROR (Status) &&
      (mPublishedGop != NULL) && (mPublishedGop->Handle != NULL) &&
      (mPublishedGop->FrameBuffer.Width <= MAX_UINT16) &&
      (mPublishedGop->FrameBuffer.Height <= MAX_UINT16)) {
    Detail = (1ULL << 63) |
      ((UINT64)mPublishedGop->SuccessfulBufferToVideoCalls << 48) |
      ((UINT64)mPublishedGop->FrameBuffer.Width << 32) |
      ((UINT64)mPublishedGop->SuccessfulSetModeCalls << 16) |
      mPublishedGop->FrameBuffer.Height;
  }
  MmioWrite64 (0x43F7F078, Detail);
  MemoryFence ();
  MmioWrite64 (0x43F7F060, 0x0000000147503754ULL);
  WriteBackDataCacheRange ((VOID *)(UINTN)0x43F7F060, 32);
}

#define GOP_CONTEXT_FROM_PROTOCOL(This) \
  CR ((This), MTK_GOP_CONTEXT, Protocol, GOP_SIGNATURE)

STATIC
EFI_STATUS
ReadCurrentMemoryMap (
  IN CONST MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  )
{
  EFI_STATUS Status;
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  UINTN Capacity;
  UINTN MapSize;
  UINTN MapKey;
  UINTN DescriptorSize;
  UINT32 DescriptorVersion;
  UINTN Attempt;

  MemoryMap = NULL;
  Capacity = MEMORY_MAP_INITIAL_SIZE;
  Status = EFI_BUFFER_TOO_SMALL;
  DescriptorSize = sizeof (EFI_MEMORY_DESCRIPTOR);
  for (Attempt = 0; Attempt < MEMORY_MAP_RETRIES; ++Attempt) {
    if ((Capacity < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
        (Capacity > MEMORY_MAP_MAX_SIZE)) {
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    Status = gBS->AllocatePool (EfiBootServicesData, Capacity, (VOID **)&MemoryMap);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    MapSize = Capacity;
    Status = gBS->GetMemoryMap (
                    &MapSize,
                    MemoryMap,
                    &MapKey,
                    &DescriptorSize,
                    &DescriptorVersion
                    );
    if (Status == EFI_BUFFER_TOO_SMALL) {
      gBS->FreePool (MemoryMap);
      MemoryMap = NULL;
      if ((DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) ||
          (DescriptorSize > MEMORY_MAP_MAX_SIZE / 2) ||
          (MapSize > MEMORY_MAP_MAX_SIZE - 2 * DescriptorSize)) {
        return EFI_OUT_OF_RESOURCES;
      }

      Capacity = MapSize + 2 * DescriptorSize;
      continue;
    }

    if (EFI_ERROR (Status)) {
      gBS->FreePool (MemoryMap);
      return Status;
    }

    if (DescriptorVersion != EFI_MEMORY_DESCRIPTOR_VERSION) {
      Status = EFI_UNSUPPORTED;
    } else {
      Status = MtkDisplayValidateMemoryMap (
                 FrameBuffer,
                 MemoryMap,
                 MapSize,
                 DescriptorSize
                 );
    }

    gBS->FreePool (MemoryMap);
    return Status;
  }

  if (MemoryMap != NULL) {
    gBS->FreePool (MemoryMap);
  }

  return EFI_ERROR (Status) ? Status : EFI_OUT_OF_RESOURCES;
}

STATIC
EFI_STATUS
ProtectFrameBuffer (
  IN OUT MTK_GOP_CONTEXT *Context
  )
{
  EFI_STATUS Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  UINT64 Base;
  UINT64 Size;
  UINT64 RequiredCapabilities;
  UINT64 WritableFrameBufferAttributes;

  Base = Context->FrameBuffer.FrameBufferBase;
  Size = Context->FrameBuffer.ProtectedSize;
  if ((Base > MAX_UINTN) || (Size == 0) ||
      (Base > MAX_UINT64 - Size)) {
    return EFI_UNSUPPORTED;
  }

  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Descriptor.BaseAddress > Base) ||
      (Descriptor.Length == 0) ||
      (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length) ||
      ((Base - Descriptor.BaseAddress) > Descriptor.Length) ||
      (Size > Descriptor.Length - (Base - Descriptor.BaseAddress)) ||
      ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeSystemMemory) &&
       (Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo))) {
    return EFI_ACCESS_DENIED;
  }

  // A validated RAM framebuffer can use normal, non-cacheable ARM memory.
  // Prefer WC only when the memory map advertises it; retain UC otherwise.
  RequiredCapabilities = ((Descriptor.Capabilities & EFI_MEMORY_WC) != 0 ?
                          EFI_MEMORY_WC : EFI_MEMORY_UC) | EFI_MEMORY_XP;
  if ((Descriptor.Capabilities & RequiredCapabilities) != RequiredCapabilities) {
    return EFI_UNSUPPORTED;
  }

  Context->OriginalGcdAttributes = Descriptor.Attributes;
  WritableFrameBufferAttributes =
    (Descriptor.Attributes &
     ~(EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT |
       EFI_MEMORY_WB | EFI_MEMORY_UCE | EFI_MEMORY_WP |
       EFI_MEMORY_RO | EFI_MEMORY_RP | EFI_MEMORY_XP)) |
     RequiredCapabilities;

  Status = gDS->SetMemorySpaceAttributes (
                  Base,
                  Size,
                  WritableFrameBufferAttributes
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Context->AttributesChanged = TRUE;
  Context->FrameBufferAttributes = RequiredCapabilities;
  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status) ||
      ((Descriptor.Attributes & (EFI_MEMORY_UC | EFI_MEMORY_WC |
                                 EFI_MEMORY_WT | EFI_MEMORY_WB |
                                 EFI_MEMORY_UCE | EFI_MEMORY_WP |
                                 EFI_MEMORY_RO | EFI_MEMORY_RP |
                                 EFI_MEMORY_XP)) != RequiredCapabilities)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
RestoreFrameBufferAttributes (
  IN OUT MTK_GOP_CONTEXT *Context
  )
{
  EFI_STATUS Status;

  if (!Context->AttributesChanged) {
    return;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  Context->FrameBuffer.FrameBufferBase,
                  Context->FrameBuffer.ProtectedSize,
                  Context->OriginalGcdAttributes
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FramebufferGopDxe: could not restore GCD attributes: %r\n", Status));
  }

  Context->AttributesChanged = FALSE;
}

STATIC
EFI_STATUS
EFIAPI
GopQueryMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32 ModeNumber,
  OUT UINTN *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
  )
{
  MTK_GOP_CONTEXT *Context;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Copy;
  EFI_STATUS Status;

  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Context = GOP_CONTEXT_FROM_PROTOCOL (This);
  if ((Context->Signature != GOP_SIGNATURE) || !Context->BootServicesActive) {
    return EFI_DEVICE_ERROR;
  }

  if (ModeNumber >= Context->Mode.MaxMode) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (*Copy), (VOID **)&Copy);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (Copy, &Context->Info, sizeof (*Copy));
  *SizeOfInfo = sizeof (*Copy);
  *Info = Copy;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32 ModeNumber
  )
{
  MTK_GOP_CONTEXT *Context;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black;
  EFI_STATUS Status;
  UINTN ClearY;
  UINTN ClearHeight;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Context = GOP_CONTEXT_FROM_PROTOCOL (This);
  if ((Context->Signature != GOP_SIGNATURE) || !Context->BootServicesActive) {
    return EFI_DEVICE_ERROR;
  }

  if (ModeNumber >= Context->Mode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  Black.Blue = 0;
  Black.Green = 0;
  Black.Red = 0;
  Black.Reserved = 0;
  ClearY = 0;
  ClearHeight = Context->FrameBuffer.Height;
  // The first GOP mode set normally erases LK's splash. Preserve only the
  // exact stock TECLAST area and clear the lower Android branding. Unknown
  // framebuffer contents retain the normal full-screen black fallback.
  if ((Context->SuccessfulSetModeCalls == 0) &&
      (Context->FrameBuffer.Width == T70_LK_LOGO_WIDTH) &&
      (Context->FrameBuffer.Height == T70_LK_LOGO_HEIGHT) &&
      (Context->FrameBuffer.PixelsPerScanLine >= T70_LK_LOGO_WIDTH) &&
      (Context->FrameBuffer.PixelFormat == PixelBlueGreenRedReserved8BitPerColor)) {
    volatile UINT32 *Pixels;
    UINTN Pitch;

    Pixels = (volatile UINT32 *)(UINTN)Context->FrameBuffer.FrameBufferBase;
    Pitch = Context->FrameBuffer.PixelsPerScanLine;
    if (((Pixels[0] & 0x00FFFFFFU) == 0) &&
        ((Pixels[650U * Pitch + 750U] & 0x00FFFFFFU) == 0x00F76707U) &&
        ((Pixels[650U * Pitch + 1120U] & 0x00FFFFFFU) == 0x00FC6505U)) {
      ClearY = T70_LK_ANDROID_TOP;
      ClearHeight -= ClearY;
    }
  }
  Status = MtkDisplayBlt (
           &Context->FrameBuffer,
           (VOID *)(UINTN)Context->FrameBuffer.FrameBufferBase,
           Context->FrameBuffer.FrameBufferSize,
           &Black,
           EfiBltVideoFill,
           0,
           0,
           0,
           ClearY,
           Context->FrameBuffer.Width,
           ClearHeight,
           0
           );
  if (!EFI_ERROR (Status)) {
    MemoryFence ();
    Context->Mode.Mode = ModeNumber;
    if (Context->SuccessfulSetModeCalls < MAX_UINT16)
      Context->SuccessfulSetModeCalls++;
    // Installation can synchronously call GOP methods. Count those calls,
    // but publish success only after the protocol installation returned.
    if ((Context == mPublishedGop) && (Context->Handle != NULL) && (mTracePhase == 8))
      RecordGopProgress (EFI_SUCCESS);
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation,
  IN UINTN SourceX,
  IN UINTN SourceY,
  IN UINTN DestinationX,
  IN UINTN DestinationY,
  IN UINTN Width,
  IN UINTN Height,
  IN UINTN Delta OPTIONAL
  )
{
  MTK_GOP_CONTEXT *Context;
  EFI_STATUS Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Context = GOP_CONTEXT_FROM_PROTOCOL (This);
  if ((Context->Signature != GOP_SIGNATURE) || !Context->BootServicesActive) {
    return EFI_UNSUPPORTED;
  }

  Status = MtkDisplayBlt (
           &Context->FrameBuffer,
           (VOID *)(UINTN)Context->FrameBuffer.FrameBufferBase,
           Context->FrameBuffer.FrameBufferSize,
           BltBuffer,
           Operation,
           SourceX,
           SourceY,
           DestinationX,
           DestinationY,
           Width,
           Height,
           Delta
           );
  if (!EFI_ERROR (Status) && (Operation != EfiBltVideoToBltBuffer)) {
    MemoryFence ();
  }
  if (!EFI_ERROR (Status) && (Operation == EfiBltBufferToVideo)) {
    if (Context->SuccessfulBufferToVideoCalls < 0x7FFFU)
      Context->SuccessfulBufferToVideoCalls++;
    if ((Context == mPublishedGop) && (Context->Handle != NULL) && (mTracePhase == 8))
      RecordGopProgress (EFI_SUCCESS);
  }
  return Status;
}

STATIC EFI_STATUS ReadRuntimeFramebuffer (OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer);

STATIC
VOID
EFIAPI
GopExitBootServices (
  IN EFI_EVENT Event,
  IN VOID *Context
  )
{
  MTK_GOP_CONTEXT *GopContext;

  GopContext = (MTK_GOP_CONTEXT *)Context;
  if ((GopContext != NULL) && (GopContext->Signature == GOP_SIGNATURE)) {
    MTK_DISPLAY_FRAMEBUFFER Current;
    BOOLEAN Ready;
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
    // BootShim reserves this RAM and clears it on every boot. Publish ready last.
    MmioWrite32 (0x43F7F0C0, 0);
    MemoryFence ();
    Ready = GopContext->BootServicesActive && GopContext->Handle != NULL &&
      GopContext->AttributesChanged && !EFI_ERROR (ReadRuntimeFramebuffer (&Current)) &&
      CompareMem (&Current, &GopContext->FrameBuffer, sizeof (Current)) == 0 &&
      GopContext->Mode.Mode == 0 && GopContext->Mode.MaxMode == 1 &&
      GopContext->Mode.FrameBufferBase == Current.FrameBufferBase &&
      GopContext->Mode.FrameBufferSize == Current.FrameBufferSize &&
      GopContext->Info.HorizontalResolution == Current.Width &&
      GopContext->Info.VerticalResolution == Current.Height &&
      GopContext->Info.PixelsPerScanLine == Current.PixelsPerScanLine &&
      GopContext->Info.PixelFormat == PixelBlueGreenRedReserved8BitPerColor &&
      Current.Width <= 4096 && Current.Height <= 4096 && Current.StrideBytes <= 65536 &&
      Current.FrameBufferRegionSize <= 64U*1024U*1024U;
    if (Ready) {
      Ready = !EFI_ERROR (gDS->GetMemorySpaceDescriptor (Current.FrameBufferBase, &Descriptor)) &&
        Descriptor.BaseAddress <= Current.FrameBufferBase &&
        Descriptor.Length >= Current.ProtectedSize &&
        Current.FrameBufferBase - Descriptor.BaseAddress <= Descriptor.Length - Current.ProtectedSize &&
        (Descriptor.GcdMemoryType == EfiGcdMemoryTypeSystemMemory ||
         Descriptor.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) &&
        (Descriptor.Attributes & (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT |
          EFI_MEMORY_WB | EFI_MEMORY_UCE | EFI_MEMORY_WP | EFI_MEMORY_RO | EFI_MEMORY_RP | EFI_MEMORY_XP)) ==
          GopContext->FrameBufferAttributes;
    }
    GopContext->BootServicesActive = FALSE;
    MmioWrite32 (0x43F7F0C4, 1);
    MmioWrite64 (0x43F7F0C8, GopContext->FrameBuffer.FrameBufferBase);
    MmioWrite64 (0x43F7F0D0, GopContext->FrameBuffer.FrameBufferRegionSize);
    MmioWrite32 (0x43F7F0D8, GopContext->FrameBuffer.Width);
    MmioWrite32 (0x43F7F0DC, GopContext->FrameBuffer.Height);
    MmioWrite32 (0x43F7F0E0, GopContext->FrameBuffer.StrideBytes);
    MmioWrite32 (0x43F7F0E4, 1); // x8r8g8b8: little-endian B,G,R,X
    MmioWrite64 (0x43F7F0E8, 0);
    MemoryFence ();
    if (Ready) MmioWrite32 (0x43F7F0C0, 1);
    WriteBackDataCacheRange ((VOID *)(UINTN)0x43F7F0C0, 48);
    MemoryFence ();
  }
}

// Own reserved handoff page; these are already-read display registers only.
#define LK_SNAPSHOT_BASE  0x43F7F800U
#define LK_SNAPSHOT_MAGIC 0x000000014C443754ULL
#define LK_DSI_SNAPSHOT_MAGIC 0x000000024C443754ULL
#define LK_OVERLAP_SNAPSHOT_MAGIC 0x000000034C443754ULL
#define LK_SNAPSHOT_BYTES 72U
typedef struct {
  UINT32 Base, Layer, Seen;
  UINT32 Reg[14];
  UINT32 DsiSeen;
  UINT32 Dsi[12];
} MTK_GOP_LK_TRACE;

STATIC VOID ClearLkSnapshot (VOID) {
  if (MmioRead64 (0x43F7F000) != 0x0000000148303754ULL) return;
  MmioWrite64 (LK_SNAPSHOT_BASE, 0);
  WriteBackDataCacheRange ((VOID *)(UINTN)LK_SNAPSHOT_BASE, 8);
}

STATIC VOID CollectLkSnapshot (MTK_GOP_LK_TRACE *Trace, UINTN Address, UINT32 Value) {
  STATIC CONST UINT32 Header[] = {0x0C,0x20,0x24,0x2C,0x2D0,0x324};
  STATIC CONST UINT32 Layer[] = {0x30,0x38,0x3C,0x40,0x44,0x4C,0xC0,0xF40};
  if (Trace == NULL) return;
  if ((Address == 0x1400500C) || (Address == 0x1400600C)) {
    ZeroMem (Trace, sizeof (*Trace));
    Trace->Base = (UINT32)Address - 0x0C;
    Trace->Layer = MAX_UINT32;
  }
  if (Trace->Base == 0) return;
  // Capture only controls already read by MtkDisplayAcquireLkHandoff.
  STATIC CONST UINT32 Dsi[] = {0x14,0x18,0x1C,0x20,0x24,0x28,0x2C,0x38,0x50,0x54,0x58,0x5C};
  for (UINTN Index=0; Index<ARRAY_SIZE (Dsi); Index++) {
    if (Address != 0x14013000U + Dsi[Index]) continue;
    Trace->Dsi[Index] = Value; Trace->DsiSeen |= 1U << Index;
  }
  for (UINTN Index=0; Index<ARRAY_SIZE (Header); Index++) {
    if (Address != Trace->Base + Header[Index]) continue;
    Trace->Reg[Index] = Value; Trace->Seen |= 1U << Index;
    if ((Index == 3) && (Value != 0) && ((Value & (Value-1U)) == 0) &&
        (Value < (Trace->Base == 0x14005000 ? 16U : 4U))) {
      for (Trace->Layer=0; (Value & (1U << Trace->Layer)) == 0; Trace->Layer++) {}
    }
  }
  if (Trace->Layer == MAX_UINT32) return;
  for (UINTN Index=0; Index<ARRAY_SIZE (Layer); Index++) {
    if (Address != Trace->Base + Layer[Index] + Trace->Layer*0x20) continue;
    Trace->Reg[6+Index] = Value; Trace->Seen |= 1U << (6+Index);
  }
}

STATIC VOID PublishLkSnapshot (CONST MTK_GOP_LK_TRACE *Trace, EFI_STATUS Status) {
  BOOLEAN DsiFailure;
  if (MmioRead64 (0x43F7F000) != 0x0000000148303754ULL) return;
  if ((mEarlyMapStatus == EFI_ACCESS_DENIED) &&
      (((mEarlyMapMeta >> 40) & 0xFF) == 6 || ((mEarlyMapMeta >> 40) & 0xFF) == 9) &&
      MmioRead64 (0x43F7F080) == 0x000000014F4D3754ULL) {
    UINT64 Detail[5];
    for (UINTN Index=0; Index<ARRAY_SIZE (Detail); Index++)
      Detail[Index] = MmioRead64 (0x43F7F088 + Index*8);
    if (Detail[0] < Detail[1] && Detail[2] < Detail[3]) {
      MmioWrite64 (LK_SNAPSHOT_BASE, 0);
      MmioWrite32 (LK_SNAPSHOT_BASE+8, 0x43F7F080U);
      MmioWrite32 (LK_SNAPSHOT_BASE+12, 0);
      for (UINTN Index=0; Index<ARRAY_SIZE (Detail); Index++)
        MmioWrite64 (LK_SNAPSHOT_BASE+16+Index*8, Detail[Index]);
      for (UINTN Index=10; Index<14; Index++)
        MmioWrite32 (LK_SNAPSHOT_BASE+16+Index*4, 0);
      MemoryFence ();
      MmioWrite64 (LK_SNAPSHOT_BASE, LK_OVERLAP_SNAPSHOT_MAGIC);
      WriteBackDataCacheRange ((VOID *)(UINTN)LK_SNAPSHOT_BASE, LK_SNAPSHOT_BYTES);
      return;
    }
  }
  if (Trace->Seen != 0x3FFF) return;
  // A typed 72-byte record keeps the reset reader bounded. On a failure
  // after all DSI controls were read, export those instead of the OVL plane.
  DsiFailure = EFI_ERROR (Status) && Trace->DsiSeen == 0xFFF;
  MmioWrite64 (LK_SNAPSHOT_BASE, 0);
  MmioWrite32 (LK_SNAPSHOT_BASE+8, DsiFailure ? 0x14013000U : Trace->Base);
  MmioWrite32 (LK_SNAPSHOT_BASE+12, DsiFailure ? 0 : Trace->Layer);
  for (UINTN Index=0; Index<ARRAY_SIZE (Trace->Reg); Index++) {
    UINT32 Value = DsiFailure ? (Index < ARRAY_SIZE (Trace->Dsi) ? Trace->Dsi[Index] : 0) : Trace->Reg[Index];
    MmioWrite32 (LK_SNAPSHOT_BASE+16+Index*4, Value);
  }
  MemoryFence ();
  MmioWrite64 (LK_SNAPSHOT_BASE, DsiFailure ? LK_DSI_SNAPSHOT_MAGIC : LK_SNAPSHOT_MAGIC);
  WriteBackDataCacheRange ((VOID *)(UINTN)LK_SNAPSHOT_BASE, LK_SNAPSHOT_BYTES);
}

STATIC
EFI_STATUS
EFIAPI
ReadDisplay32 (VOID *Context, UINTN Address, UINT32 *Value)
{
  *Value = MmioRead32 (Address);
  CollectLkSnapshot (Context, Address, *Value);
  mTraceDetail = ((UINT64)(UINT32)Address << 32) | *Value;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReadRuntimeFramebuffer (
  OUT MTK_DISPLAY_FRAMEBUFFER *FrameBuffer
  )
{
  EFI_STATUS Status;
  VOID *Fdt;
  UINTN FdtSize;
  MTK_GOP_LK_TRACE Trace;

  ClearLkSnapshot ();
  ZeroMem (&Trace, sizeof (Trace));
  // Phase 2 detail is a bounded step code, then the last display MMIO pair.
  mTraceDetail = 1ULL << 32;
  Fdt = NULL;
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);
  if (EFI_ERROR (Status) || (Fdt == NULL)) {
    return EFI_NOT_FOUND;
  }

  mTraceDetail = 2ULL << 32;
  // BootStatus publishes the bounded runtime copy before the configuration
  // table signal; refuse any inconsistent or failed publication.
  if ((MmioRead64 (0x43F7F000) != 0x0000000148303754ULL) ||
      (MmioRead64 (0x43F7F048) != (UINT64)(UINTN)Fdt) ||
      (MmioRead64 (0x43F7F058) != EFI_SUCCESS)) {
    return EFI_NOT_READY;
  }
  mTraceDetail = 3ULL << 32;
  FdtSize = (UINTN)MmioRead64 (0x43F7F050);
  if ((FdtSize < sizeof (FDT_HEADER)) || (FdtSize > FDT_MAX_SIZE) ||
      (FdtCheckFull (Fdt, FdtSize) != 0) ||
      (FdtTotalSize (Fdt) != FdtSize)) {
    return EFI_COMPROMISED_DATA;
  }

  mTraceDetail = 4ULL << 32;
  Status = MtkDisplayParseHandoff ((CONST VOID *)Fdt, FdtSize, FrameBuffer);
  if (Status == EFI_NOT_FOUND) {
    // SEC prepared/protected LK scanout before HOBs. DXE only revalidates it.
    mTraceDetail = 5ULL << 32;
    Status = MtkDisplayAcquireLkHandoff (Fdt, FdtSize, ReadDisplay32, NULL, &Trace, FrameBuffer);
    PublishLkSnapshot (&Trace, Status);
  }
  if (!EFI_ERROR (Status) && FrameBuffer->FrameBufferBase < 0x43F80000ULL &&
      FrameBuffer->FrameBufferBase + FrameBuffer->ProtectedSize > 0x43F7F000ULL) {
    return EFI_ACCESS_DENIED;
  }
  return Status;
}

STATIC
EFI_STATUS
TryStartGop (
  VOID
  )
{
  EFI_STATUS Status;
  MTK_GOP_CONTEXT *Context;
  MTK_DISPLAY_FRAMEBUFFER FrameBuffer;
  EFI_HANDLE NewHandle;
  BOOLEAN RetainContext;

  if (mPublishedGop != NULL) return EFI_ALREADY_STARTED;
  RetainContext = FALSE;
  mTracePhase = 2;
  RecordGopProgress (EFI_NOT_READY);
  mTraceDetail = 0;
  Status = ReadRuntimeFramebuffer (&FrameBuffer);
  if (EFI_ERROR (Status)) {
    if ((mEarlyMapMeta >> 48) == 0x4D4D) {
      UINT32 Tag = 0x4D000000U | ((UINT32)(mEarlyMapMeta >> 32) & 0xFFFFU) << 8 |
                   (UINT32)(mEarlyMapMeta & 0xFF);
      UINT32 Code = (UINT32)(mEarlyMapStatus & 0x7FFFFFFF) |
                    (EFI_ERROR (mEarlyMapStatus) ? 0x80000000U : 0);
      mTraceDetail = ((UINT64)Tag << 32) | Code;
    }
    return Status;
  }

  mTraceDetail = ((UINT64)FrameBuffer.Width << 32) | FrameBuffer.Height;
  mTracePhase = 3;
  RecordGopProgress (EFI_NOT_READY);
  Context = NULL;
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (*Context), (VOID **)&Context);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (Context, sizeof (*Context));
  Context->Signature = GOP_SIGNATURE;
  Context->BootServicesActive = TRUE;
  Context->FrameBuffer = FrameBuffer;

  mTracePhase = 4;
  RecordGopProgress (EFI_NOT_READY);
  Status = ReadCurrentMemoryMap (&Context->FrameBuffer);
  if (EFI_ERROR (Status)) {
    goto Failure;
  }

  mTracePhase = 5;
  RecordGopProgress (EFI_NOT_READY);
  Status = ProtectFrameBuffer (Context);
  if (EFI_ERROR (Status)) {
    RestoreFrameBufferAttributes (Context);
    goto Failure;
  }

  Context->DevicePath.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  Context->DevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  Context->DevicePath.Vendor.Header.Length[0] = sizeof (VENDOR_DEVICE_PATH) & 0xFF;
  Context->DevicePath.Vendor.Header.Length[1] = sizeof (VENDOR_DEVICE_PATH) >> 8;
  Context->DevicePath.Vendor.Guid = mMtkGopDevicePathGuid;
  Context->DevicePath.End.Type = END_DEVICE_PATH_TYPE;
  Context->DevicePath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  Context->DevicePath.End.Length[0] = sizeof (EFI_DEVICE_PATH_PROTOCOL);
  Context->DevicePath.End.Length[1] = 0;

  Context->Info.Version = 0;
  Context->Info.HorizontalResolution = Context->FrameBuffer.Width;
  Context->Info.VerticalResolution = Context->FrameBuffer.Height;
  Context->Info.PixelFormat = Context->FrameBuffer.PixelFormat;
  Context->Info.PixelsPerScanLine = Context->FrameBuffer.PixelsPerScanLine;
  ZeroMem (&Context->Info.PixelInformation, sizeof (Context->Info.PixelInformation));

  Context->Mode.MaxMode = 1;
  Context->Mode.Mode = 0;
  Context->Mode.Info = &Context->Info;
  Context->Mode.SizeOfInfo = sizeof (Context->Info);
  Context->Mode.FrameBufferBase = Context->FrameBuffer.FrameBufferBase;
  Context->Mode.FrameBufferSize = Context->FrameBuffer.FrameBufferSize;
  Context->Protocol.QueryMode = GopQueryMode;
  Context->Protocol.SetMode = GopSetMode;
  Context->Protocol.Blt = GopBlt;
  Context->Protocol.Mode = &Context->Mode;

  mTracePhase = 6;
  RecordGopProgress (EFI_NOT_READY);
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  GopExitBootServices,
                  Context,
                  &gEfiEventExitBootServicesGuid,
                  &Context->ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    RestoreFrameBufferAttributes (Context);
    goto Failure;
  }

  mTracePhase = 7;
  RecordGopProgress (EFI_NOT_READY);
  NewHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &NewHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Context->Protocol,
                  &gEfiDevicePathProtocolGuid,
                  &Context->DevicePath.Vendor,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    EFI_STATUS CloseStatus;

    Context->BootServicesActive = FALSE;
    CloseStatus = gBS->CloseEvent (Context->ExitBootServicesEvent);
    if (EFI_ERROR (CloseStatus)) {
      // The registered callback still owns Context. Keep it allocated until
      // firmware reaches ExitBootServices and invokes the callback.
      RetainContext = TRUE;
      DEBUG ((DEBUG_ERROR, "FramebufferGopDxe: event close failed; retaining callback context: %r\n", CloseStatus));
    } else {
      Context->ExitBootServicesEvent = NULL;
    }

    RestoreFrameBufferAttributes (Context);
    goto Failure;
  }

  Context->Handle = NewHandle;
  mPublishedGop = Context;
  mTracePhase = 8;
  DEBUG ((DEBUG_INFO, "FramebufferGopDxe: installed validated runtime framebuffer GOP\n"));
  return EFI_SUCCESS;

Failure:
  if ((Context != NULL) && !RetainContext) {
    Context->Signature = 0;
    gBS->FreePool (Context);
  }

  // A live event references both Context and this image's callback code.
  // Returning an error would let DXE unload the image despite the retained
  // allocation. Keep it resident even though no GOP was published.
  if (RetainContext) mPublishedGop = Context;
  return RetainContext ? EFI_SUCCESS : Status;
}


STATIC
VOID
EFIAPI
GopRetry (
  IN EFI_EVENT Event,
  IN VOID *Context
  )
{
  (VOID)Event;
  (VOID)Context;
  if (mExitingBootServices || mStartInProgress || mPublishedGop != NULL) return;
  mStartInProgress = TRUE;
  mTracePhase = 1;
  if (mTraceAttempts != MAX_UINT32) mTraceAttempts++;
  RecordGopProgress (EFI_NOT_READY);
  mLastStartStatus = TryStartGop ();
  RecordGopProgress (mLastStartStatus);
  mStartInProgress = FALSE;
}

STATIC
VOID
EFIAPI
GopDriverExit (
  IN EFI_EVENT Event,
  IN VOID *Context
  )
{
  (VOID)Event;
  (VOID)Context;
  mExitingBootServices = TRUE;
  // The installed GOP owns this record even when its notification runs first.
  if ((mPublishedGop != NULL) && (mPublishedGop->Handle != NULL)) return;
  // No GOP must leave a ready record, even if a previous EBS attempt ran.
  MmioWrite32 (0x43F7F0C0, 0);
  MemoryFence ();
  WriteBackDataCacheRange ((VOID *)(UINTN)0x43F7F0C0, 48);
}

EFI_STATUS
EFIAPI
FramebufferGopEntry (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  CONST EFI_GUID *Groups[3] = {
    &gFdtTableGuid, &gEfiEndOfDxeEventGroupGuid, &gEfiEventReadyToBootGuid
  };
  (VOID)ImageHandle;
  (VOID)SystemTable;
  if (mDriverRegistered) return EFI_ALREADY_STARTED;
  // Preserve SEC's result before GOP reuses the status slots. A tagged value
  // distinguishes the SEC result from a GOP journal or unknown RAM.
  mEarlyMapMeta = MmioRead64 (0x43F7F068);
  if ((mEarlyMapMeta >> 48) == 0x4D4D &&
      ((mEarlyMapMeta >> 40) & 0xFF) <= 11 && (mEarlyMapMeta & 0xFFFFFFFFU) < 128) {
    mEarlyMapStatus = MmioRead64 (0x43F7F060);
  } else {
    mEarlyMapMeta = 0;
    mEarlyMapStatus = 0;
  }
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL, TPL_NOTIFY, GopDriverExit, NULL,
                  &gEfiEventExitBootServicesGuid, &mDriverExitEvent
                  );
  if (EFI_ERROR (Status)) return Status;
  mDriverRegistered = TRUE;
  // The EBS event owns this image from here onward. A missing optional retry
  // must not unload callback code; remaining events and the initial try work.
  for (UINTN Index = 0; Index < ARRAY_SIZE (Groups); ++Index) {
    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL, TPL_CALLBACK, GopRetry, NULL,
                    Groups[Index], &mRetryEvents[Index]
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "FramebufferGopDxe: retry event unavailable: %r\n", Status));
    }
  }
  GopRetry (NULL, NULL);
  return EFI_SUCCESS;
}
