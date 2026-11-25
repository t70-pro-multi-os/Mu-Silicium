#ifndef T70_CAPTURE_BMP_H
#define T70_CAPTURE_BMP_H
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

/* Captures the screen visible at call time. On success caller FreePool(*Buffer).
   No storage, input handling, console drawing or screenshot replacement here.
   Pass empty output slots; on failure they are NULL/0. */
EFI_STATUS CaptureBmp(EFI_BOOT_SERVICES *Bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                      VOID **Buffer, UINTN *Size);
#endif
