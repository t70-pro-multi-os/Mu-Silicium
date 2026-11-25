#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/UefiLib.h>

#include <Library/GpioImplLib.h>

#include <Protocol/MtkGpio.h>

#define PIN_OFFSET(pin) (((pin) / 32) * 0x10)

#define PIN_MODE_OFFSET(pin) (((pin) / 8) * 0x10)
#define PIN_MODE_BIT(pin) (((pin) % 8) * 4)
#define MSDC1_FIRST_PIN 71U
#define MSDC1_LAST_PIN  76U
#define MSDC1_IOCFG_REQUIRED_LENGTH 0xF4U
#define MSDC1_PINCTRL_REQUIRED_LENGTH 0x3A0U

STATIC EFI_MEMORY_REGION_DESCRIPTOR mPinctrlRegion;
STATIC EFI_MEMORY_REGION_DESCRIPTOR mIoCfgRtRegion;
STATIC BOOLEAN mIoCfgRtAvailable;
STATIC EFI_LOCK mGpioLock = EFI_INITIALIZE_LOCK_VARIABLE (TPL_NOTIFY);
STATIC VOID   *mLeaseMemory;
STATIC UINT64 *mPinLeaseOwner;
STATIC UINT64 *mLeaseTokens;
STATIC UINT32 *mAcquirePinScratch;
STATIC UINT8  *mLeaseSeen;
STATIC UINTN   mLeaseSeenBytes;
STATIC UINTN   mPinCount;
STATIC UINT64  mNextLeaseToken = 1;
STATIC BOOLEAN mGpioInstalled;

STATIC
UINT32
GpioRead(
  IN  UINT32 Reg)
{
  return MmioRead32 (mPinctrlRegion.Address + Reg);
}

STATIC
VOID
GpioWrite(
  IN  UINT32 Reg,
  IN  UINT32 Value)
{
  MmioWrite32 (mPinctrlRegion.Address + Reg, Value);
}

STATIC
EFI_STATUS
GpioBeginOperation (
  VOID
  )
{
  if ((mLeaseMemory == NULL) || (mPinCount == 0)) {
    return EFI_NOT_READY;
  }

  // EFI_LOCK raises TPL_NOTIFY and rejects same-TPL reentry. Keep the lock
  // across validation and the complete write-one/mux operation.
  return (EfiAcquireLockOrFail (&mGpioLock) == EFI_SUCCESS)
           ? EFI_SUCCESS : EFI_NOT_READY;
}

STATIC
BOOLEAN
GpioPinValid (
  IN UINT32 Pin
  )
{
  return (BOOLEAN)((UINTN)Pin < mPinCount);
}

STATIC
UINTN
FindLeaseSlot (
  IN UINT64 Token
  )
{
  UINTN Index;

  if (Token == 0) {
    return mPinCount;
  }

  for (Index = 0; Index < mPinCount; ++Index) {
    if (mLeaseTokens[Index] == Token) {
      return Index;
    }
  }

  return mPinCount;
}

STATIC
EFI_STATUS
ValidateLeaseLocked (
  IN UINT64 Token,
  IN UINT32 Pin
  )
{
  if (!GpioPinValid (Pin)) {
    return EFI_INVALID_PARAMETER;
  }

  if (FindLeaseSlot (Token) == mPinCount) {
    return EFI_INVALID_PARAMETER;
  }

  return (mPinLeaseOwner[Pin] == Token) ? EFI_SUCCESS : EFI_ACCESS_DENIED;
}

STATIC
EFI_STATUS
CheckWriteOwnershipLocked (
  IN UINT64  Token,
  IN UINT32  Pin,
  IN BOOLEAN LeaseWrite
  )
{
  if (!GpioPinValid (Pin)) {
    return EFI_INVALID_PARAMETER;
  }

  if (LeaseWrite) {
    return ValidateLeaseLocked (Token, Pin);
  }

  if (mPinLeaseOwner[Pin] != 0) {
    return EFI_ACCESS_DENIED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
GpioGetDir(
  IN  UINT32   Pin,
  OUT BOOLEAN *Direction)
{
  EFI_STATUS Status;
  UINT32 Value;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!GpioPinValid (Pin) || (Direction == NULL)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  // Hardware DIR: 0 is input, 1 is output.
  Value = GpioRead (gPlatformInfo.DirOffset + PIN_OFFSET(Pin));
  *Direction = !!(Value & (1U << (Pin % 32)));
  Status = EFI_SUCCESS;

Done:
  EfiReleaseLock (&mGpioLock);
  return Status;
}

STATIC
EFI_STATUS
GpioSetDirInternal (
  IN UINT64  Token,
  IN UINT32  Pin,
  IN BOOLEAN Direction,
  IN BOOLEAN LeaseWrite
  )
{
  EFI_STATUS Status;
  UINT32 Value;
  UINT32 Offset;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CheckWriteOwnershipLocked (Token, Pin, LeaseWrite);
  if (!EFI_ERROR (Status)) {
    // Set Pin Direction
    Offset = gPlatformInfo.DirOffset + PIN_OFFSET (Pin) +
             (Direction ? gPlatformInfo.SetOffset : gPlatformInfo.ResetOffset);
    // SET/CLR are write-one aliases; reading them can modify unrelated pins.
    Value = 1U << (Pin % 32);
    GpioWrite (Offset, Value);
  }

  EfiReleaseLock (&mGpioLock);
  return Status;
}

EFI_STATUS
GpioSetDir(
  IN UINT32  Pin,
  IN BOOLEAN Direction)
{
  return GpioSetDirInternal (0, Pin, Direction, FALSE);
}

EFI_STATUS
GpioGetState(
  IN  UINT32   Pin,
  OUT BOOLEAN *State)
{
  EFI_STATUS Status;
  UINT32 Value;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!GpioPinValid (Pin) || (State == NULL)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  // DI reflects the physical pin for both directions, not the output latch.
  Value = GpioRead (gPlatformInfo.DataInOffset + PIN_OFFSET(Pin));
  *State = !!(Value & (1U << (Pin % 32)));
  Status = EFI_SUCCESS;

Done:
  EfiReleaseLock (&mGpioLock);
  return Status;
}

STATIC
EFI_STATUS
GpioSetStateInternal (
  IN UINT64  Token,
  IN UINT32  Pin,
  IN BOOLEAN State,
  IN BOOLEAN LeaseWrite
  )
{
  EFI_STATUS Status;
  UINT32 Value, Offset;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CheckWriteOwnershipLocked (Token, Pin, LeaseWrite);
  if (!EFI_ERROR (Status)) {
    // Set Pin State
    Offset = gPlatformInfo.DataOutOffset +
             (State ? gPlatformInfo.SetOffset : gPlatformInfo.ResetOffset) +
             PIN_OFFSET(Pin);
    Value = 1U << (Pin % 32);
    GpioWrite (Offset, Value);
  }

  EfiReleaseLock (&mGpioLock);
  return Status;
}

EFI_STATUS
GpioSetState(
  IN UINT32  Pin,
  IN BOOLEAN State)
{
  return GpioSetStateInternal (0, Pin, State, FALSE);
}

STATIC
EFI_STATUS
GpioSetModeInternal (
  IN UINT64 Token,
  IN UINT32 Pin,
  IN UINT32 Mode,
  IN BOOLEAN LeaseWrite
  )
{
  EFI_STATUS Status;
  UINT32 Offset, Shift;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CheckWriteOwnershipLocked (Token, Pin, LeaseWrite);
  if (!EFI_ERROR (Status) && (Mode > 7)) {
    Status = EFI_INVALID_PARAMETER;
  }

  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mGpioLock);
    return Status;
  }

  Offset = gPlatformInfo.ModeOffset + PIN_MODE_OFFSET (Pin);
  Shift = PIN_MODE_BIT (Pin);
  if (gPlatformInfo.ModeMaskWriteOffset != 0) {
    // MT6789: each nibble's bit 3 enables an atomic mode update at +0xC.
    GpioWrite (Offset + gPlatformInfo.ModeMaskWriteOffset, (8U | Mode) << Shift);
  } else {
    // Legacy controllers use write-one clear/set aliases, without alias reads.
    GpioWrite (Offset + gPlatformInfo.ResetOffset, ((~Mode) & 7U) << Shift);
    GpioWrite (Offset + gPlatformInfo.SetOffset, Mode << Shift);
  }

  EfiReleaseLock (&mGpioLock);
  return EFI_SUCCESS;
}

EFI_STATUS
GpioSetMode(
  IN UINT32 Pin,
  IN UINT32 Mode)
{
  return GpioSetModeInternal (0, Pin, Mode, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
GpioAcquirePins (
  IN  CONST UINT32 *Pins,
  IN  UINTN         PinCount,
  OUT UINT64       *Token
  )
{
  EFI_STATUS Status;
  UINTN Index;
  UINTN Slot;
  UINT64 NewToken;

  if (Token == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Token = 0;
  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Pins == NULL) || (PinCount == 0) || (PinCount > mPinCount)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  ZeroMem (mLeaseSeen, mLeaseSeenBytes);
  for (Index = 0; Index < PinCount; ++Index) {
    UINT32 Pin;
    UINT8 Mask;

    Pin = Pins[Index];
    if (!GpioPinValid (Pin)) {
      Status = EFI_INVALID_PARAMETER;
      goto ClearSeen;
    }

    Mask = (UINT8)(1U << (Pin & 7U));
    if ((mLeaseSeen[Pin >> 3] & Mask) != 0) {
      Status = EFI_INVALID_PARAMETER;
      goto ClearSeen;
    }

    if (mPinLeaseOwner[Pin] != 0) {
      Status = EFI_ACCESS_DENIED;
      goto ClearSeen;
    }

    mLeaseSeen[Pin >> 3] |= Mask;
    mAcquirePinScratch[Index] = Pin;
  }

  for (Slot = 0; Slot < mPinCount; ++Slot) {
    if (mLeaseTokens[Slot] == 0) {
      break;
    }
  }

  if ((Slot == mPinCount) || (mNextLeaseToken == 0)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ClearSeen;
  }

  NewToken = mNextLeaseToken;
  if (mNextLeaseToken == MAX_UINT64) {
    // Never recycle a token: stale scalar tokens stay invalid permanently.
    mNextLeaseToken = 0;
  } else {
    ++mNextLeaseToken;
  }

  for (Index = 0; Index < PinCount; ++Index) {
    mPinLeaseOwner[mAcquirePinScratch[Index]] = NewToken;
  }
  mLeaseTokens[Slot] = NewToken;
  *Token = NewToken;
  Status = EFI_SUCCESS;

ClearSeen:
  ZeroMem (mLeaseSeen, mLeaseSeenBytes);
Done:
  EfiReleaseLock (&mGpioLock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GpioReleasePins (
  IN UINT64 Token
  )
{
  EFI_STATUS Status;
  UINTN Slot;
  UINTN Index;
  UINTN OwnedCount;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Slot = FindLeaseSlot (Token);
  if (Slot == mPinCount) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  OwnedCount = 0;
  for (Index = 0; Index < mPinCount; ++Index) {
    if (mPinLeaseOwner[Index] == Token) {
      ++OwnedCount;
    }
  }

  if (OwnedCount == 0) {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  for (Index = 0; Index < mPinCount; ++Index) {
    if (mPinLeaseOwner[Index] == Token) {
      mPinLeaseOwner[Index] = 0;
    }
  }
  mLeaseTokens[Slot] = 0;
  Status = EFI_SUCCESS;

Done:
  EfiReleaseLock (&mGpioLock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GpioValidateLease (
  IN UINT64 Token,
  IN UINT32 Pin
  )
{
  EFI_STATUS Status;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ValidateLeaseLocked (Token, Pin);
  EfiReleaseLock (&mGpioLock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GpioSetDirWithLease (
  IN UINT64  Token,
  IN UINT32  Pin,
  IN BOOLEAN Direction
  )
{
  return GpioSetDirInternal (Token, Pin, Direction, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
GpioSetStateWithLease (
  IN UINT64  Token,
  IN UINT32  Pin,
  IN BOOLEAN State
  )
{
  return GpioSetStateInternal (Token, Pin, State, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
GpioSetModeWithLease (
  IN UINT64 Token,
  IN UINT32 Pin,
  IN UINT32 Mode
  )
{
  return GpioSetModeInternal (Token, Pin, Mode, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
GpioConfigureMsdc1PadsWithLease (
  IN  UINT64   Token,
  OUT BOOLEAN *CanReleaseLease
  )
{
  EFI_STATUS Status;
  UINT32 Pin;

  if (CanReleaseLease == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *CanReleaseLease = TRUE;

  // The optional callback is populated only by a platform that owns the exact
  // MT6789 MSDC1 pad map. Do not expose a generic IOCFG write surface.
  if (gPlatformInfo.ConfigureMsdc1Pads == NULL) {
    return EFI_UNSUPPORTED;
  }
  if (!mIoCfgRtAvailable) {
    return EFI_NOT_READY;
  }
  if (mPinCount <= MSDC1_LAST_PIN) {
    return EFI_UNSUPPORTED;
  }

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Pin = MSDC1_FIRST_PIN; Pin <= MSDC1_LAST_PIN; ++Pin) {
    Status = ValidateLeaseLocked (Token, Pin);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  Status = gPlatformInfo.ConfigureMsdc1Pads (
             mPinctrlRegion.Address,
             mIoCfgRtRegion.Address,
             &gPlatformInfo,
             CanReleaseLease
             );

Done:
  EfiReleaseLock (&mGpioLock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GpioIsBusy (
  IN  UINT32   Pin,
  OUT BOOLEAN *Busy
  )
{
  EFI_STATUS Status;

  Status = GpioBeginOperation ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!GpioPinValid (Pin) || (Busy == NULL)) {
    Status = EFI_INVALID_PARAMETER;
  } else {
    *Busy = (BOOLEAN)(mPinLeaseOwner[Pin] != 0);
    Status = EFI_SUCCESS;
  }

  EfiReleaseLock (&mGpioLock);
  return Status;
}

STATIC MTK_GPIO_PROTOCOL mGpio = {
  GpioGetDir,
  GpioSetDir,
  GpioGetState,
  GpioSetState,
  GpioSetMode,
  GpioAcquirePins,
  GpioReleasePins,
  GpioValidateLease,
  GpioSetDirWithLease,
  GpioSetStateWithLease,
  GpioSetModeWithLease,
  GpioIsBusy,
  GpioConfigureMsdc1PadsWithLease
};

EFI_STATUS
EFIAPI
InitGpioDriver (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  UINT64 PinCount64;
  UINTN PinCount;
  UINTN OwnerBytes;
  UINTN ScratchBytes;
  UINTN SeenBytes;
  UINTN TotalBytes;
  UINTN TokenBytes;

  if (mGpioInstalled) {
    return EFI_ALREADY_STARTED;
  }

  // Locate Pinctrl Memory Region
  Status = LocateMemoryRegionByName ("Pinctrl", &mPinctrlRegion);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate Pinctrl Memory Region! Status = %r\n", Status));
    return Status;
  }

  mIoCfgRtAvailable = FALSE;
  if (gPlatformInfo.ConfigureMsdc1Pads != NULL) {
    if (mPinctrlRegion.Length >= MSDC1_PINCTRL_REQUIRED_LENGTH) {
      Status = LocateMemoryRegionByName ("IOCFG RT", &mIoCfgRtRegion);
      if (!EFI_ERROR (Status) &&
          (mIoCfgRtRegion.Length >= MSDC1_IOCFG_REQUIRED_LENGTH)) {
        mIoCfgRtAvailable = TRUE;
      } else {
        DEBUG ((DEBUG_WARN, "MSDC1 IOCFG RT region unavailable; GPIO protocol remains active, pad setup will report not-ready. Status = %r\n", Status));
      }
    } else {
      DEBUG ((DEBUG_WARN, "Pinctrl region is too short for the MSDC1 pad group; GPIO protocol remains active.\n"));
    }
  }

  // Use the active implementation's exact MaxPin. The generic DXE provider
  // also serves platforms whose pin count differs from MT6789's 188 pins.
  PinCount64 = (UINT64)gPlatformInfo.MaxPin + 1ULL;
  if ((PinCount64 == 0) || (PinCount64 > (UINT64)MAX_UINTN)) {
    return EFI_UNSUPPORTED;
  }

  PinCount = (UINTN)PinCount64;
  if ((PinCount > (MAX_UINTN / sizeof (UINT64))) ||
      (PinCount > (MAX_UINTN / sizeof (UINT32))) ||
      (PinCount > (MAX_UINTN - 7U))) {
    return EFI_UNSUPPORTED;
  }

  OwnerBytes = PinCount * sizeof (UINT64);
  TokenBytes = OwnerBytes;
  ScratchBytes = PinCount * sizeof (UINT32);
  SeenBytes = (PinCount + 7U) / 8U;
  if ((OwnerBytes > (MAX_UINTN / 2U)) ||
      (ScratchBytes > (MAX_UINTN - (OwnerBytes * 2U))) ||
      (SeenBytes > (MAX_UINTN - (OwnerBytes * 2U) - ScratchBytes))) {
    return EFI_UNSUPPORTED;
  }

  TotalBytes = (OwnerBytes * 2U) + ScratchBytes + SeenBytes;
  mLeaseMemory = AllocateZeroPool (TotalBytes);
  if (mLeaseMemory == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mPinCount = PinCount;
  mPinLeaseOwner = (UINT64 *)mLeaseMemory;
  mLeaseTokens = (UINT64 *)((UINT8 *)mLeaseMemory + OwnerBytes);
  mAcquirePinScratch = (UINT32 *)((UINT8 *)mLeaseMemory + OwnerBytes + TokenBytes);
  mLeaseSeen = (UINT8 *)mLeaseMemory + OwnerBytes + TokenBytes + ScratchBytes;
  mLeaseSeenBytes = SeenBytes;

  // Register GPIO Protocol
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gMediaTekGpioProtocolGuid, &mGpio, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register Gpio Protocol! Status = %r\n", Status));
    FreePool (mLeaseMemory);
    mLeaseMemory = NULL;
    mPinLeaseOwner = NULL;
    mLeaseTokens = NULL;
    mAcquirePinScratch = NULL;
    mLeaseSeen = NULL;
    mLeaseSeenBytes = 0;
    mPinCount = 0;
    return Status;
  }

  mGpioInstalled = TRUE;
  return Status;
}
