// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_XNU_PLATFORM_H
#define T70_XNU_PLATFORM_H
#include <Uefi.h>
#include <Library/MemoryMapLib.h>
#include <Protocol/GraphicsOutput.h>
EFI_STATUS T70XnuValidatePlatform(CONST VOID *Tree,UINTN Bytes,
  CONST EFI_MEMORY_REGION_DESCRIPTOR *Regions,UINT8 Count,
  CONST EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,UINT64 Mpidr,UINT32 TimerFrequency);
#endif
