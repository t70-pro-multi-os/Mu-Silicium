/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_DPMAIF_HW_CORE_H
#define T70_DPMAIF_HW_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "DpmaifRing.h"

#define T70_DPMAIF_HW_TX_QUEUES 4U
#define T70_DPMAIF_HW_MAX_PLAN_OPS 96U
#define T70_DPMAIF_HW_POLL_LIMIT 1600000U

typedef enum {
  T70DpmaifHwRegionAoUl = 0,
  T70DpmaifHwRegionPdUl = 1,
  T70DpmaifHwRegionPdMdMisc = 2,
  T70DpmaifHwRegionPdSram = 3
} T70_DPMAIF_HW_REGION;

typedef enum {
  T70DpmaifHwWrite = 1,
  T70DpmaifHwUpdate = 2,
  T70DpmaifHwPollClear = 3,
  T70DpmaifHwPollMaskedNotEqual = 4
} T70_DPMAIF_HW_OP_KIND;

typedef struct {
  uint8_t Region;
  uint8_t Kind;
  uint16_t Reserved;
  uint32_t Offset;
  uint32_t Mask;
  uint32_t Value;
  uint32_t PollLimit;
} T70_DPMAIF_HW_OP;

typedef enum {
  T70DpmaifHwExecSuccess = 0,
  T70DpmaifHwExecInvalid,
  T70DpmaifHwExecIoError,
  T70DpmaifHwExecTimeout,
  T70DpmaifHwExecCancelled
} T70_DPMAIF_HW_EXEC_STATUS;

typedef int (*T70_DPMAIF_HW_READ32)(
  void *Context, uint8_t Region, uint32_t Offset, uint32_t *Value);
typedef int (*T70_DPMAIF_HW_WRITE32)(
  void *Context, uint8_t Region, uint32_t Offset, uint32_t Value);
typedef int (*T70_DPMAIF_HW_POLL_STEP)(void *Context);

typedef struct {
  void *Context;
  T70_DPMAIF_HW_READ32 Read32;
  T70_DPMAIF_HW_WRITE32 Write32;
  /* Called between unsuccessful reads; return zero to abort the plan. */
  T70_DPMAIF_HW_POLL_STEP PollStep;
} T70_DPMAIF_HW_IO;

typedef struct {
  uint64_t ModemMemoryBase;
  uint64_t ModemMemoryBytes;
} T70_DPMAIF_MODEM_MEMORY;

typedef struct {
  uint64_t PitAddress;
  uint32_t PitEntries;
  uint64_t RequestBatAddress;
  uint32_t RequestBatEntries;
  uint32_t RequestBatPostedEntries;
  uint64_t FragmentBatAddress;
  uint32_t FragmentBatEntries;
  uint32_t FragmentBatPostedEntries;
  uint64_t TxDrbAddress[T70_DPMAIF_HW_TX_QUEUES];
  uint32_t TxDrbEntries[T70_DPMAIF_HW_TX_QUEUES];
  uint8_t FragmentEnabled;
} T70_DPMAIF_QUEUE_CONFIG;

/* Build only the source-defined RX PIT/BAT and TX DRB queue programming.
 * Clock/power, INFRA protection, SRAM clear/remap, interrupt setup, BAT buffer
 * posting and final HW-init notification are separate prerequisite stages. */
T70_DPMAIF_RING_STATUS T70DpmaifBuildQueueInitPlan(
  const T70_DPMAIF_QUEUE_CONFIG *Config,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  );

/* Apply a validated plan through caller-owned MMIO callbacks. The caller must
 * provide a bounded PollStep appropriate for its execution level. */
T70_DPMAIF_HW_EXEC_STATUS T70DpmaifApplyQueueInitPlan(
  const T70_DPMAIF_HW_OP *Operations,
  size_t OperationCount,
  const T70_DPMAIF_HW_IO *Io
  );

/* Source-locked pre-queue stage. The eight remap registers cover up to
 * 768 MiB in 32 MiB banks; only banks inside the reserved MD_SYS1 range enable. */
T70_DPMAIF_RING_STATUS T70DpmaifBuildCommonInitPlan(
  const T70_DPMAIF_MODEM_MEMORY *Memory,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  );

T70_DPMAIF_RING_STATUS T70DpmaifBuildInterruptInitPlan(
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  );

T70_DPMAIF_RING_STATUS T70DpmaifBuildBatStartPlan(
  const T70_DPMAIF_QUEUE_CONFIG *Config,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  );

T70_DPMAIF_RING_STATUS T70DpmaifBuildInitDonePlan(
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  );

/* Concatenate common, interrupt, queue, posted-BAT credit/enable, and final
 * init-done stages in the stock order; output stays unchanged on failure. */
T70_DPMAIF_RING_STATUS T70DpmaifBuildStartupPlan(
  const T70_DPMAIF_MODEM_MEMORY *Memory,
  const T70_DPMAIF_QUEUE_CONFIG *QueueConfig,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  );

#endif
