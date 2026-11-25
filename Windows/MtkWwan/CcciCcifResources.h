/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_CCCI_CCIF_RESOURCES_H
#define T70_CCCI_CCIF_RESOURCES_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  T70CcciCcifResourceMemory = 0,
  T70CcciCcifResourceInterrupt = 1
} T70_CCCI_CCIF_RESOURCE_TYPE;

typedef enum {
  T70CcciCcifResourceSuccess = 0,
  T70CcciCcifResourceInvalid,
  T70CcciCcifResourceMismatch
} T70_CCCI_CCIF_RESOURCE_STATUS;

typedef struct {
  T70_CCCI_CCIF_RESOURCE_TYPE Type;
  uint64_t Start;
  uint32_t Length;
  uint8_t LevelTriggered;
  uint8_t ActiveHigh;
  uint8_t Shared;
} T70_CCCI_CCIF_RESOURCE;

typedef struct {
  uint64_t ApBase;
  uint64_t ModemBase;
  uint32_t DataInterrupt;
  uint32_t ExceptionInterrupt;
} T70_CCCI_CCIF_RESOURCE_MAP;

/* Raw ACPI HANA6798 CCF0 _CRS, matched against the T70 CCIF DT order. */
T70_CCCI_CCIF_RESOURCE_STATUS T70CcciCcifMapResources(
  const T70_CCCI_CCIF_RESOURCE *Resources,
  size_t ResourceCount,
  T70_CCCI_CCIF_RESOURCE_MAP *Map
  );

#endif
