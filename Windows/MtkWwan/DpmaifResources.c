/* SPDX-License-Identifier: GPL-2.0-only */
/* HANA6797 mapping follows DPMAIF reg[0..3], GSI, and CCCI handoff resource. */
#include "DpmaifResources.h"
#include <string.h>

T70_DPMAIF_RESOURCE_STATUS
T70DpmaifMapResources (
  const T70_DPMAIF_RESOURCE *Resources,
  size_t ResourceCount,
  T70_DPMAIF_RESOURCE_MAP *Map
  )
{
  static const uint64_t Bases[4] = {
    UINT64_C(0x10014000),
    UINT64_C(0x1022D000),
    UINT64_C(0x1022C000),
    UINT64_C(0x1022E000)
  };
  T70_DPMAIF_RESOURCE_MAP Result;
  uint8_t Seen[6] = {0};
  size_t Index;

  if (Resources == NULL || Map == NULL) {
    return T70DpmaifResourceInvalid;
  }
  if (ResourceCount != 6U) {
    return T70DpmaifResourceMismatch;
  }
  memset(&Result, 0, sizeof(Result));

  for (Index = 0; Index < ResourceCount; ++Index) {
    const T70_DPMAIF_RESOURCE *Resource = &Resources[Index];
    if (Resource->Type == T70DpmaifResourceMemory) {
      unsigned MemoryIndex;
      for (MemoryIndex = 0; MemoryIndex < 4U; ++MemoryIndex) {
        if (Resource->Start == Bases[MemoryIndex]) {
          break;
        }
      }
      if (MemoryIndex == 4U) {
        if (Resource->Start != UINT64_C(0x43f7f180) ||
            Resource->Length != 0x40U || Resource->Shared != 0U ||
            Seen[4] != 0U) {
          return T70DpmaifResourceMismatch;
        }
        Seen[4] = 1U;
        Result.CcciHandoffBase = Resource->Start;
        Result.CcciHandoffLength = Resource->Length;
      } else {
        if (Resource->Length != 0x1000U || Resource->Shared != 0U ||
            Seen[MemoryIndex] != 0U) {
          return T70DpmaifResourceMismatch;
        }
        Seen[MemoryIndex] = 1U;
        switch (MemoryIndex) {
        case 0: Result.AoUlBase = Resource->Start; break;
        case 1: Result.PdUlBase = Resource->Start; break;
        case 2: Result.PdMdMiscBase = Resource->Start; break;
        case 3: Result.PdSramBase = Resource->Start; break;
        }
      }
    } else if (Resource->Type == T70DpmaifResourceInterrupt) {
      if (Seen[5] != 0U || Resource->Start != 273U ||
          Resource->LevelTriggered != 1U || Resource->ActiveHigh != 1U ||
          Resource->Shared != 0U) {
        return T70DpmaifResourceMismatch;
      }
      Seen[5] = 1U;
      Result.Interrupt = (uint32_t)Resource->Start;
    } else {
      return T70DpmaifResourceMismatch;
    }
  }
  for (Index = 0; Index < sizeof(Seen) / sizeof(Seen[0]); ++Index) {
    if (Seen[Index] == 0U) {
      return T70DpmaifResourceMismatch;
    }
  }
  *Map = Result;
  return T70DpmaifResourceSuccess;
}
