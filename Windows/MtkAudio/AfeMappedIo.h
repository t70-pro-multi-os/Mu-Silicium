// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_MAPPED_IO_H
#define T70_AFE_MAPPED_IO_H
#include <ntddk.h>
#include <stdint.h>
#include <stddef.h>
#include "AfeFormatUpdate.h"
typedef struct {
 uint64_t PhysicalBase;
 volatile void *VirtualBase;
 size_t Length;
} T70_AFE_MAPPED_IO;
/* Caller supplies an already valid resident Windows device mapping for exactly
 * AFE 0x11210000/0x2000. The numeric tuple cannot prove the mapping or ownership.
 * Context itself is ordinary resident memory, immutable and outside device
 * memory. Caller owns accessibility, mapping lifetime, whole-operation RMW
 * serialization and required hardware quiescence. Do NOT use normal RAM as a
 * real device mapping, and do not unmap concurrently. No map/unmap is performed.
 * These callbacks use WDK READ/WRITE_REGISTER_ULONG, not plain volatile access.
 * Zero means the primitive returned, not device acknowledgement or readiness.
 * Invalid alignment/range/tuple/output fails before the register primitive.
 * Read output must be ordinary writable memory, disjoint from context/mapping.
 * No exceptions/hardware faults are caught; invalid actual mappings are a caller
 * error. Bounds are not a register-policy allowlist. Never use this to bypass
 * unresolved power/reset/IRQ semantics. Host tests substitute the primitives,
 * so they do not validate WDK ABI, Windows barriers or real MMIO. */
int32_t T70AfeMappedRead32(void *Context,uint32_t Offset,uint32_t *Value);
int32_t T70AfeMappedWrite32(void *Context,uint32_t Offset,uint32_t Value);
/* Bind using { &CallerOwnedMapping, T70AfeMappedRead32, T70AfeMappedWrite32 }
 * as T70_AFE_FORMAT_IO. Binding alone performs no accesses. */
#endif
