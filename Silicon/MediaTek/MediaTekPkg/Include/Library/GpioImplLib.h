#ifndef _GPIO_IMPL_LIB_H_
#define _GPIO_IMPL_LIB_H_

#include <Uefi.h>

typedef struct _MTK_GPIO_PLATFORM_INFO MTK_GPIO_PLATFORM_INFO;

// The implementation is called only while GpioDxe holds its GPIO lock and
// the caller owns the complete MSDC1 pin lease.  CanReleaseLease is FALSE
// only when a failed write could not be rolled back and verified.
typedef
EFI_STATUS
(EFIAPI *MTK_GPIO_CONFIGURE_MSDC1_PADS) (
  IN  UINTN                     PinctrlBase,
  IN  UINTN                     IoCfgRtBase,
  IN  CONST MTK_GPIO_PLATFORM_INFO *PlatformInfo,
  OUT BOOLEAN                  *CanReleaseLease
  );

struct _MTK_GPIO_PLATFORM_INFO {
  UINT32 SetOffset;
  UINT32 ResetOffset;
  UINT32 DirOffset;
  UINT32 DataOutOffset;
  UINT32 DataInOffset;
  UINT32 ModeOffset;
  UINT32 MaxPin;
  // Zero for legacy SET/CLR mux; MT6789 uses atomic mask-write at +0xC.
  UINT32 ModeMaskWriteOffset;
  // Optional: configures only GPIO71-76 as the MT6789 MSDC1 pin group.
  MTK_GPIO_CONFIGURE_MSDC1_PADS ConfigureMsdc1Pads;
};

extern MTK_GPIO_PLATFORM_INFO gPlatformInfo;

#endif /* _GPIO_IMPL_LIB_H_ */
