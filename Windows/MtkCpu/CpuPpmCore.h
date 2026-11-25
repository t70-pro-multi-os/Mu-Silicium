// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_CPU_PPM_CORE_H
#define T70_CPU_PPM_CORE_H
#include <stdint.h>

#define T70_CPU_PPM_DOMAINS 2U
#define T70_CPU_PPM_MAX_OPPS 32U

typedef struct {
  uint32_t Count;
  uint32_t FrequencyKhz[T70_CPU_PPM_MAX_OPPS];
  uint32_t Performance[T70_CPU_PPM_MAX_OPPS];
  uint32_t HighestPerformance,NominalPerformance;
  uint32_t LowestNonlinearPerformance,LowestPerformance;
} T70_CPU_PPM_DOMAIN;

/* Capacities are stock capacity-dmips-mhz values from the effective DTB. */
int T70CpuPpmBuild(const uint32_t FrequenciesKhz[T70_CPU_PPM_DOMAINS][T70_CPU_PPM_MAX_OPPS],
  const uint32_t Counts[T70_CPU_PPM_DOMAINS],
  const uint32_t CapacityDmipsMhz[T70_CPU_PPM_DOMAINS],
  T70_CPU_PPM_DOMAIN Output[T70_CPU_PPM_DOMAINS]);
/* Select the closest supported state inside the requested min/max window. */
int T70CpuPpmSelect(const T70_CPU_PPM_DOMAIN *Domain,uint32_t Minimum,
  uint32_t Desired,uint32_t Maximum,uint32_t *FrequencyKhz);
/* ACPI0007 UID and the explicit CPU0..CPU7 namespace paths map to logical IDs. */
int T70CpuPpmUidFromDeviceId(const uint16_t *DeviceId,uint32_t Characters,uint32_t *Uid);
#endif
