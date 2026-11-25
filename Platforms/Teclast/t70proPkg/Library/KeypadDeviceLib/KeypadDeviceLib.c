#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/MtkPmicWrapper.h>

#define T70_KPD_BASE               0x10010000U
#define T70_KPD_LENGTH             0x1000U
#define T70_KPD_STATUS             0x04U
// Stock kpd_pdrv writes DT key-debounce-ms (1024 on T70) to +0x18, low 14 bits.
#define T70_KPD_DEBOUNCE_MS        0x18U
#define T70_KPD_DEBOUNCE_MASK      0x3fffU
#define T70_KPD_DEBOUNCE_VALUE     1024U
#define T70_KPD_ENABLE             0x24U
#define T70_KPD_ENABLE_BIT         BIT0
#define T70_KPD_VOLUME_UP_BIT      BIT0
#define T70_KPD_VOLUME_DOWN_BIT    BIT1
#define T70_PMIC_TOPSTATUS         0x0028U
#define T70_PMIC_POWER_KEY_BIT     BIT1

#define MS2NS(Ms)  (((UINT64)(Ms)) * 1000000ULL)

STATIC MTK_PMIC_WRAPPER_PROTOCOL *mPmicWrapper;
STATIC UINTN                       mKpdBase;
STATIC BOOLEAN                     mInitialized;
STATIC BOOLEAN                     mSampleActive;
STATIC BOOLEAN                     mVolumeUpPressed;
STATIC BOOLEAN                     mVolumeDownPressed;
STATIC BOOLEAN                     mPowerPressed;
STATIC KEY_CONTEXT                 mVolumeUpContext;
STATIC KEY_CONTEXT                 mVolumeDownContext;
STATIC KEY_CONTEXT                 mPowerContext;

STATIC EFI_KEY_DATA mPowerDownKey = { .Key = { .ScanCode = SCAN_RIGHT } };
STATIC EFI_KEY_DATA mPowerUpKey   = { .Key = { .ScanCode = SCAN_LEFT } };
STATIC EFI_KEY_DATA mPowerLongKey = { .Key = { .ScanCode = SCAN_ESC } };

STATIC
VOID
KeyInitializeContext (
  KEY_CONTEXT *Context
  )
{
  SetMem (&Context->KeyData, sizeof (Context->KeyData), 0);
  Context->Time      = 0;
  Context->State     = KEYSTATE_RELEASED;
  Context->Repeat    = FALSE;
  Context->Longpress = FALSE;
}

STATIC
BOOLEAN
KeyIsPressed (
  UINT16 ScanCode,
  CHAR16 UnicodeChar
  )
{
  if (ScanCode == SCAN_UP) {
    return mVolumeUpPressed;
  }

  if (ScanCode == SCAN_DOWN) {
    return mVolumeDownPressed;
  }

  if (UnicodeChar == CHAR_CARRIAGE_RETURN) {
    return mPowerPressed;
  }

  return FALSE;
}

STATIC
VOID
KeyUpdateStatus (
  KEY_CONTEXT       *Context,
  KEYPAD_RETURN_API *KeypadReturnApi,
  BOOLEAN            IsPressed,
  UINT64             Delta
  )
{
  if (MAX_UINT64 - Context->Time < Delta) {
    Context->Time = MAX_UINT64;
  } else {
    Context->Time += Delta;
  }

  switch (Context->State) {
    case KEYSTATE_RELEASED:
      if (IsPressed) {
        Context->Time  = 0;
        Context->State = KEYSTATE_PRESSED;
      }
      break;

    case KEYSTATE_PRESSED:
      if (IsPressed) {
        if (Context->Repeat && (Context->Time >= MS2NS (100))) {
          KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
          Context->Time   = 0;
          Context->Repeat = TRUE;
        } else if (!Context->Longpress && (Context->Time >= MS2NS (500))) {
          if (Context->KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            if (KeyIsPressed (SCAN_DOWN, 0)) {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &mPowerDownKey);
            } else if (KeyIsPressed (SCAN_UP, 0)) {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &mPowerUpKey);
            } else {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &mPowerLongKey);
            }
          } else if (!KeyIsPressed (0, CHAR_CARRIAGE_RETURN)) {
            KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
            Context->Time   = 0;
            Context->Repeat = TRUE;
          }

          Context->Longpress = TRUE;
        }
      } else if (!Context->Longpress) {
        KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
        Context->State = KEYSTATE_LONGPRESS_RELEASE;
      } else if (Context->Time >= MS2NS (10)) {
        Context->Time      = 0;
        Context->Repeat    = FALSE;
        Context->Longpress = FALSE;
        Context->State     = KEYSTATE_RELEASED;
      }
      break;

    case KEYSTATE_LONGPRESS_RELEASE:
      Context->Time      = 0;
      Context->Repeat    = FALSE;
      Context->Longpress = FALSE;
      Context->State     = KEYSTATE_RELEASED;
      break;

    default:
      KeyInitializeContext (Context);
      break;
  }
}

RETURN_STATUS
EFIAPI
KeypadDeviceConstructor (
  VOID
  )
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR Region;
  MTK_PMIC_WRAPPER_PROTOCOL   *PmicWrapper;

  mInitialized  = FALSE;
  mSampleActive = FALSE;
  mPmicWrapper  = NULL;
  mKpdBase      = 0;
  mVolumeUpPressed   = FALSE;
  mVolumeDownPressed = FALSE;
  mPowerPressed      = FALSE;
  KeyInitializeContext (&mVolumeUpContext);
  KeyInitializeContext (&mVolumeDownContext);
  KeyInitializeContext (&mPowerContext);

  if ((gBS == NULL) || (gBS->LocateProtocol == NULL)) {
    return RETURN_UNSUPPORTED;
  }

  PmicWrapper = NULL;
  Status = gBS->LocateProtocol (&gMediaTekPmicWrapperProtocolGuid, NULL, (VOID **)&PmicWrapper);
  if (EFI_ERROR (Status) || (PmicWrapper == NULL) || (PmicWrapper->Read == NULL)) {
    return RETURN_NOT_FOUND;
  }

  SetMem (&Region, sizeof (Region), 0);
  Status = LocateMemoryRegionByName ((CHAR8 *)"Keypad Controller", &Region);
  if (EFI_ERROR (Status)) {
    return RETURN_NOT_FOUND;
  }

  if ((Region.Address != T70_KPD_BASE) ||
      (Region.Length != T70_KPD_LENGTH) ||
      (Region.HobOption != AddDev) ||
      (Region.ResourceType != MMAP_IO) ||
      (Region.ResourceAttribute != UNCACHEABLE) ||
      (Region.MemoryType != MmIO) ||
      (Region.ArmAttributes != NS_DEVICE)) {
    return RETURN_UNSUPPORTED;
  }

  mPmicWrapper = PmicWrapper;
  mKpdBase     = (UINTN)Region.Address;
  mVolumeUpContext.KeyData.Key.ScanCode   = SCAN_UP;
  mVolumeDownContext.KeyData.Key.ScanCode = SCAN_DOWN;
  mPowerContext.KeyData.Key.UnicodeChar   = CHAR_CARRIAGE_RETURN;
  mInitialized = TRUE;
  return RETURN_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceReset (
  KEYPAD_DEVICE_PROTOCOL *This
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mInitialized) {
    return EFI_NOT_READY;
  }

  if (mSampleActive) {
    return EFI_NOT_READY;
  }

  KeyInitializeContext (&mVolumeUpContext);
  mVolumeUpContext.KeyData.Key.ScanCode = SCAN_UP;
  KeyInitializeContext (&mVolumeDownContext);
  mVolumeDownContext.KeyData.Key.ScanCode = SCAN_DOWN;
  KeyInitializeContext (&mPowerContext);
  mPowerContext.KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
  mVolumeUpPressed   = FALSE;
  mVolumeDownPressed = FALSE;
  mPowerPressed      = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceGetKeys (
  KEYPAD_DEVICE_PROTOCOL *This,
  KEYPAD_RETURN_API      *KeypadReturnApi,
  UINT64                  Delta
  )
{
  EFI_STATUS Status;
  UINT16     Enable;
  UINT16     Debounce;
  UINT16     KeyStatus;
  UINT16     TopStatus;

  if ((This == NULL) || (KeypadReturnApi == NULL) || (KeypadReturnApi->PushEfikeyBufTail == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mInitialized || (mPmicWrapper == NULL) || (mKpdBase == 0)) {
    return EFI_NOT_READY;
  }

  if (mSampleActive) {
    return EFI_NOT_READY;
  }

  mSampleActive = TRUE;

  Enable = MmioRead16 (mKpdBase + T70_KPD_ENABLE);
  if ((Enable & T70_KPD_ENABLE_BIT) == 0) {
    Status = EFI_NOT_READY;
    goto Exit;
  }

  Debounce = MmioRead16 (mKpdBase + T70_KPD_DEBOUNCE_MS);
  if ((Debounce & T70_KPD_DEBOUNCE_MASK) != T70_KPD_DEBOUNCE_VALUE) {
    Status = EFI_NOT_READY;
    goto Exit;
  }

  KeyStatus = MmioRead16 (mKpdBase + T70_KPD_STATUS);
  TopStatus = 0;
  Status = mPmicWrapper->Read (T70_PMIC_TOPSTATUS, &TopStatus);
  mVolumeUpPressed   = (KeyStatus & T70_KPD_VOLUME_UP_BIT) == 0;
  mVolumeDownPressed = (KeyStatus & T70_KPD_VOLUME_DOWN_BIT) == 0;
  mPowerPressed      = !EFI_ERROR (Status) && (TopStatus & T70_PMIC_POWER_KEY_BIT) == 0;

  KeyUpdateStatus (&mVolumeUpContext, KeypadReturnApi, mVolumeUpPressed, Delta);
  KeyUpdateStatus (&mVolumeDownContext, KeypadReturnApi, mVolumeDownPressed, Delta);
  // Only a verified power-key sample advances the power and chord timing.
  if (!EFI_ERROR (Status)) {
    KeyUpdateStatus (&mPowerContext, KeypadReturnApi, mPowerPressed, Delta);
  }

Exit:
  mSampleActive = FALSE;
  return Status;
}
