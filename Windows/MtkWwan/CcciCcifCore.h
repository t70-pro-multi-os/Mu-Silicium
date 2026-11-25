/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_CCCI_CCIF_CORE_H
#define T70_CCCI_CCIF_CORE_H

#include <stddef.h>
#include <stdint.h>

#define T70_CCCI_CCIF_CHANNEL_COUNT 24U
#define T70_CCCI_CCIF_MAX_SEND_OPS 2U

typedef enum {
  T70CcciCcifSuccess = 0,
  T70CcciCcifInvalid,
  T70CcciCcifBusy,
  T70CcciCcifNoSpace,
  T70CcciCcifEmpty,
  T70CcciCcifIoError
} T70_CCCI_CCIF_STATUS;

typedef enum {
  T70CcciCcifDataInterrupt = 0,
  T70CcciCcifExceptionInterrupt = 1
} T70_CCCI_CCIF_INTERRUPT_KIND;

typedef struct {
  uint32_t Offset;
  uint32_t Value;
} T70_CCCI_CCIF_WRITE;

typedef struct {
  uint32_t ReceivedChannels;
  uint32_t AckMask;
} T70_CCCI_CCIF_RX;

typedef int (*T70_CCCI_CCIF_READ32)(void *Context,uint32_t Offset,uint32_t *Value);
typedef int (*T70_CCCI_CCIF_WRITE32)(void *Context,uint32_t Offset,uint32_t Value);
typedef struct {
  void *Context;
  T70_CCCI_CCIF_READ32 Read32;
  T70_CCCI_CCIF_WRITE32 Write32;
} T70_CCCI_CCIF_IO;

T70_CCCI_CCIF_STATUS T70CcciCcifBuildSendPlan(
  uint32_t Channel,
  uint32_t BusyRegister,
  T70_CCCI_CCIF_WRITE *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  );

T70_CCCI_CCIF_STATUS T70CcciCcifDecodeInterrupt(
  uint32_t ReceivedChannels,
  T70_CCCI_CCIF_INTERRUPT_KIND Kind,
  T70_CCCI_CCIF_RX *Result
  );

T70_CCCI_CCIF_STATUS T70CcciCcifSend(
  const T70_CCCI_CCIF_IO *Io,
  uint32_t Channel
  );

T70_CCCI_CCIF_STATUS T70CcciCcifReadAndAcknowledge(
  const T70_CCCI_CCIF_IO *Io,
  T70_CCCI_CCIF_INTERRUPT_KIND Kind,
  T70_CCCI_CCIF_RX *Result
  );

#endif
