#ifndef _MTK_GPIO_H_
#define _MTK_GPIO_H_

//
// Declare forward Reference to the GPIO Protocol
//
typedef struct _MTK_GPIO_PROTOCOL MTK_GPIO_PROTOCOL;

/**
  This Function Gets direction of the defined pin.

  @param[in]  Pin                          - The Pin.
  @param[out] Direction                    - FALSE for input, TRUE for output.
**/
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_GET_DIR) (
  IN  UINT32   Pin,
  OUT BOOLEAN *Direction
  );

/**
  This Function Sets direction of the defined pin.

  @param[in] Pin                          - The Pin.
  @param[in] Direction                    - FALSE for input, TRUE for output.
**/
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_SET_DIR) (
  IN UINT32  Pin,
  IN BOOLEAN Direction
  );

/**
  This Function Gets state of the defined pin.

  @param[in]  Pin                          - The Pin.
  @param[out] State                        - The State of the Pin.
**/
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_GET_STATE) (
  IN  UINT32   Pin,
  OUT BOOLEAN *State
  );

/**
  This Function Sets state of the defined pin.

  @param[in] Pin                          - The Pin.
  @param[in] State                        - The State of the Pin.
**/
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_SET_STATE) (
  IN UINT32  Pin,
  IN BOOLEAN State
  );

/**
  This Function Sets mode of the defined pin.

  @param[in] Pin                          - The Pin.
  @param[in] Mode                         - The Mode of the Pin.
**/
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_SET_MODE) (
  IN UINT32 Pin,
  IN UINT32 Mode
  );

/**
  Atomically reserve a non-empty set of platform GPIO pins.

  Token is an opaque scalar capability. It is not a pointer and remains invalid
  after release; callers must keep the pin list unchanged for the duration of
  this call.
**/
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_ACQUIRE_PINS) (
  IN  CONST UINT32 *Pins,
  IN  UINTN         PinCount,
  OUT UINT64       *Token
  );

typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_RELEASE_PINS) (
  IN UINT64 Token
  );

typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_VALIDATE_LEASE) (
  IN UINT64 Token,
  IN UINT32 Pin
  );

typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_SET_DIR_WITH_LEASE) (
  IN UINT64  Token,
  IN UINT32  Pin,
  IN BOOLEAN Direction
  );

typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_SET_STATE_WITH_LEASE) (
  IN UINT64  Token,
  IN UINT32  Pin,
  IN BOOLEAN State
  );

typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_SET_MODE_WITH_LEASE) (
  IN UINT64 Token,
  IN UINT32 Pin,
  IN UINT32 Mode
  );

/**
  Apply the fixed MSDC1 pad configuration while the caller owns pins 71-76.

  The platform implementation is deliberately bounded to the board's SD
  controller pin group.  CanReleaseLease is FALSE only if a failed hardware
  update could not be restored and read back successfully; callers must keep
  the lease in that case so no other driver can repurpose uncertain pins.
**/
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_CONFIGURE_MSDC1_PADS_WITH_LEASE) (
  IN  UINT64   Token,
  OUT BOOLEAN *CanReleaseLease
  );

typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_IS_BUSY) (
  IN  UINT32   Pin,
  OUT BOOLEAN *Busy
  );

//
// Define Protocol Functions
//
struct _MTK_GPIO_PROTOCOL {
  // Keep these original entries and their order stable for existing callers.
  MTK_GPIO_GET_DIR   GetDir;
  MTK_GPIO_SET_DIR   SetDir;
  MTK_GPIO_GET_STATE GetState;
  MTK_GPIO_SET_STATE SetState;
  MTK_GPIO_SET_MODE  SetMode;

  // A lease reserves every pin in a set until ReleasePins succeeds. The legacy
  // setters deny writes to leased pins; lease setters only accept pins owned by
  // the supplied token. Reads remain available while a pin is leased.
  MTK_GPIO_ACQUIRE_PINS       AcquirePins;
  MTK_GPIO_RELEASE_PINS       ReleasePins;
  MTK_GPIO_VALIDATE_LEASE     ValidateLease;
  MTK_GPIO_SET_DIR_WITH_LEASE SetDirWithLease;
  MTK_GPIO_SET_STATE_WITH_LEASE SetStateWithLease;
  MTK_GPIO_SET_MODE_WITH_LEASE SetModeWithLease;
  MTK_GPIO_IS_BUSY            IsBusy;
  // Appended to preserve all existing protocol member offsets.
  MTK_GPIO_CONFIGURE_MSDC1_PADS_WITH_LEASE ConfigureMsdc1PadsWithLease;
};

extern EFI_GUID gMediaTekGpioProtocolGuid;

#endif /* _MTK_GPIO_H_ */
