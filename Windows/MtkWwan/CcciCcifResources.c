/* SPDX-License-Identifier: GPL-2.0-only */
/* HANA6798 CCF0 mapping follows the stock T70 ccifdriver reg/IRQ order. */
#include "CcciCcifResources.h"
#include <string.h>

T70_CCCI_CCIF_RESOURCE_STATUS
T70CcciCcifMapResources (
  const T70_CCCI_CCIF_RESOURCE *Resources,
  size_t ResourceCount,
  T70_CCCI_CCIF_RESOURCE_MAP *Map
  )
{
  static const uint64_t MemoryBases[2] = {
    UINT64_C(0x10209000), UINT64_C(0x1020a000)
  };
  static const uint32_t Interrupts[2] = {258U,259U};
  T70_CCCI_CCIF_RESOURCE_MAP Result;
  uint8_t Seen[4] = {0};
  size_t Index;

  if (Resources == NULL || Map == NULL) {
    return T70CcciCcifResourceInvalid;
  }
  if (ResourceCount != 4U) {
    return T70CcciCcifResourceMismatch;
  }
  memset(&Result,0,sizeof(Result));
  for (Index=0U; Index<ResourceCount; ++Index) {
    const T70_CCCI_CCIF_RESOURCE *R=&Resources[Index];
    if (R->Type==T70CcciCcifResourceMemory) {
      unsigned Slot;
      for (Slot=0U; Slot<2U; ++Slot) {
        if (R->Start==MemoryBases[Slot]) break;
      }
      if (Slot==2U || R->Length!=0x1000U || R->Shared!=0U ||
          Seen[Slot]!=0U) {
        return T70CcciCcifResourceMismatch;
      }
      Seen[Slot]=1U;
      if (Slot==0U) Result.ApBase=R->Start;
      else Result.ModemBase=R->Start;
    } else if (R->Type==T70CcciCcifResourceInterrupt) {
      unsigned Slot;
      for (Slot=0U; Slot<2U; ++Slot) {
        if (R->Start==Interrupts[Slot]) break;
      }
      if (Slot==2U || R->LevelTriggered!=1U || R->ActiveHigh!=1U ||
          R->Shared!=0U || Seen[Slot+2U]!=0U) {
        return T70CcciCcifResourceMismatch;
      }
      Seen[Slot+2U]=1U;
      if (Slot==0U) Result.DataInterrupt=(uint32_t)R->Start;
      else Result.ExceptionInterrupt=(uint32_t)R->Start;
    } else {
      return T70CcciCcifResourceMismatch;
    }
  }
  for (Index=0U; Index<4U; ++Index) {
    if (Seen[Index]==0U) return T70CcciCcifResourceMismatch;
  }
  *Map=Result;
  return T70CcciCcifResourceSuccess;
}
