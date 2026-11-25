// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Library/MtkDisplayCore.h>
#include "LkBootMode.h"

extern EFI_NARROW_GLYPH gUsStdNarrowGlyphData[];

#define VOLUME_UP       1U
#define VOLUME_DOWN     2U
#define KEYS_INVALID    4U
#define GLYPH_SCALE     4U
#define MENU_WIDTH      960U
#define MENU_HEIGHT     312U

static unsigned
ReadVolumes (void)
{
  const volatile UINT16 *Enable = (const volatile UINT16 *)(T70_LK_KPD_BASE + T70_LK_KPD_ENABLE);
  const volatile UINT16 *Keys = (const volatile UINT16 *)(T70_LK_KPD_BASE + T70_LK_KPD_KEYS);

  if ((*Enable & 1U) == 0) {
    return KEYS_INVALID;
  }

  UINT16 Status = *Keys;
  if ((Status | T70_LK_KPD_VOLUME_MASK) != 0xffffU) {
    return KEYS_INVALID;
  }

  return ~Status & T70_LK_KPD_VOLUME_MASK;
}

unsigned
FactoryKeyHeld (void)
{
  // LK enables and debounces the keypad before its early factory-key sample.
  // Read the same key at kernel handoff, after LK has loaded the boot image.
  return ReadVolumes () == VOLUME_UP;
}

unsigned
ChooseFactoryBoot (void)
{
  // LK confirmed Factory entry. Releasing volume up starts EDK2 without a display.
  const unsigned Keys = ReadVolumes ();
  return Keys == 0 || Keys == VOLUME_DOWN;
}

static BOOLEAN
RecoveryKeyHeld (void)
{
  // LK's recovery key is matrix index 17 on the G100 table at 0xbc530.
  const volatile UINT16 *Keys = (const volatile UINT16 *)(T70_LK_KPD_BASE + T70_LK_KPD_KEYS + 4);
  return *Keys == 0xfffdU;
}

static EFI_STATUS EFIAPI
ReadDisplay (VOID *Context, UINTN Address, UINT32 *Value)
{
  (void)Context;
  *Value = *(const volatile UINT32 *)Address;
  return EFI_SUCCESS;
}

static void
DrawText (
  volatile UINT32 *Pixels,
  UINT32           Pitch,
  const char      *Text
  )
{
  for (; *Text != '\0'; ++Text, Pixels += 8 * GLYPH_SCALE) {
    const UINT8 *Glyph = gUsStdNarrowGlyphData[(unsigned)*Text - ' '].GlyphCol1;
    for (UINT32 Row = 0; Row < EFI_GLYPH_HEIGHT; ++Row) {
      for (UINT32 Column = 0; Column < EFI_GLYPH_WIDTH; ++Column) {
        if ((Glyph[Row] & (0x80U >> Column)) == 0) {
          continue;
        }
        for (UINT32 Dy = 0; Dy < GLYPH_SCALE; ++Dy) {
          for (UINT32 Dx = 0; Dx < GLYPH_SCALE; ++Dx) {
            Pixels[(Row * GLYPH_SCALE + Dy) * Pitch + Column * GLYPH_SCALE + Dx] = 0xffffffffU;
          }
        }
      }
    }
  }
}

static BOOLEAN
DrawRecoveryMenu (const void *Fdt, unsigned Size)
{
  MTK_DISPLAY_FRAMEBUFFER Display;
  if (EFI_ERROR (MtkDisplayInspectLkHandoff (Fdt, Size, ReadDisplay, NULL, &Display)) ||
      Display.Width < MENU_WIDTH || Display.Height < MENU_HEIGHT) {
    return FALSE;
  }

  UINT32 Pitch = Display.PixelsPerScanLine;
  UINT32 X = (Display.Width - MENU_WIDTH) / 2;
  UINT32 Y = (Display.Height - MENU_HEIGHT) / 2;
  volatile UINT32 *Pixels = (volatile UINT32 *)(UINTN)Display.FrameBufferBase + Y * Pitch + X;
  for (UINT32 Row = 0; Row < MENU_HEIGHT; ++Row) {
    for (UINT32 Column = 0; Column < MENU_WIDTH; ++Column) {
      Pixels[Row * Pitch + Column] = 0xff182230U;
    }
  }
  DrawText (Pixels + 16 * Pitch + 32, Pitch, "Release volume keys");
  DrawText (Pixels + 112 * Pitch + 32, Pitch, "VOL +   Stock recovery");
  DrawText (Pixels + 208 * Pitch + 32, Pitch, "VOL -   EDK2");
  __asm__ volatile ("dsb sy" ::: "memory");
  return TRUE;
}

static __attribute__((noinline)) UINT64
MenuCounter (void)
{
  UINT64 Counter;
  __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r"(Counter));
  return Counter;
}

unsigned
ChooseRecoveryMenu (const void *Fdt, unsigned Size)
{
  unsigned InitialKeys = ReadVolumes ();
  if (InitialKeys == KEYS_INVALID || (InitialKeys == 0 && !RecoveryKeyHeld ())) {
    return 0;
  }

  UINT64 Frequency;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(Frequency));
  if (Frequency < 1000 || Frequency > 1000000000 || !DrawRecoveryMenu (Fdt, Size)) {
    return 0;
  }

  UINT64 Start = MenuCounter ();
  UINT64 NextSample = Start;
  UINT64 Interval = Frequency / 100;
  unsigned Previous = KEYS_INVALID;
  unsigned Stable = 0;
  BOOLEAN Armed = FALSE;

  for (;;) {
    UINT64 Now = MenuCounter ();
    if (Now - Start >= 6 * Frequency) {
      return 0;
    }
    if (Now - NextSample < Interval) {
      continue;
    }
    NextSample = Now;

    unsigned Keys = ReadVolumes ();
    if (Keys == 0 && RecoveryKeyHeld ()) {
      Keys = KEYS_INVALID;
    }
    if (Keys != Previous) {
      Previous = Keys;
      Stable = 0;
      continue;
    }
    if (++Stable != 3) {
      continue;
    }
    if (Keys == 0) {
      Armed = TRUE;
    } else if (Armed && (Keys == VOLUME_UP || Keys == VOLUME_DOWN)) {
      return Keys == VOLUME_DOWN;
    }
  }
}
