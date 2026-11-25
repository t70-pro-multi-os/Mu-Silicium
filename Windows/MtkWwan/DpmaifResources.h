/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_DPMAIF_RESOURCES_H
#define T70_DPMAIF_RESOURCES_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  T70DpmaifResourceMemory = 0,
  T70DpmaifResourceInterrupt = 1
} T70_DPMAIF_RESOURCE_TYPE;

typedef enum {
  T70DpmaifResourceSuccess = 0,
  T70DpmaifResourceInvalid,
  T70DpmaifResourceMismatch
} T70_DPMAIF_RESOURCE_STATUS;

typedef struct {
  T70_DPMAIF_RESOURCE_TYPE Type;
  uint64_t Start;
  uint32_t Length;
  uint8_t LevelTriggered;
  uint8_t ActiveHigh;
  uint8_t Shared;
} T70_DPMAIF_RESOURCE;

typedef struct {
  uint64_t AoUlBase;
  uint64_t PdUlBase;
  uint64_t PdMdMiscBase;
  uint64_t PdSramBase;
  uint64_t CcciHandoffBase;
  uint32_t CcciHandoffLength;
  uint32_t Interrupt;
} T70_DPMAIF_RESOURCE_MAP;

/* Normalized HANA6797 resource list from the T70 ACPI _CRS. */
T70_DPMAIF_RESOURCE_STATUS T70DpmaifMapResources(
  const T70_DPMAIF_RESOURCE *Resources,
  size_t ResourceCount,
  T70_DPMAIF_RESOURCE_MAP *Map
  );

#endif
