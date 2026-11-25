// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_PREPARE_H
#define T70_AFE_PREPARE_H
#include "AfeFormat.h"
#include "AfeGeometry.h"
typedef struct {
  uint32_t Rate,SampleFormat,Channels;
  uint64_t DmaBase,BufferBytes,PeriodBytes;
  T70_AFE_FORMAT Format;
  T70_AFE_GEOMETRY Geometry;
} T70_AFE_PREPARED;
/* Value snapshot only: no retained pointers or internal mutable state. Treat
 * the returned value as immutable; C cannot enforce that caller ownership rule.
 * Derives frame size from the accepted DL1 format, never from a caller override.
 * Publishes only after both checks pass; failure leaves output unchanged.
 * This prepares arithmetic data, NOT hardware: no allocation, MMIO, DMA start,
 * cache operation, power or routing. Passing does not verify memory accessibility
 * or base alignment, nor codec/speaker support. S32 remains unsupported. */
int T70AfeDl1Prepare(uint32_t Rate,uint32_t SampleFormat,uint32_t Channels,
                     uint64_t DmaBase,uint64_t BufferBytes,uint64_t PeriodBytes,
                     T70_AFE_PREPARED *Prepared);
/* Recalculate and compare every derived field, ignoring C padding. */
int T70AfeDl1PreparedValid(const T70_AFE_PREPARED *Prepared);
#endif
