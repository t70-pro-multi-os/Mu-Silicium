// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_PREFLIGHT_H
#define T70_AW_PREFLIGHT_H
#include "AwRegisters.h"
typedef struct {
  uint32_t RecordIndex;
  size_t BinOffset; /* Relative to the inner register-bin, NOT the ACF file. */
  T70_AW_REGISTER Register;
} T70_AW_PREFLIGHT_ENTRY;
/* Read-only inventory of known special addresses 04/05/08/14/50. Neither an
 * apply plan nor an exhaustive safety classification: unlisted addresses remain
 * unclassified. Duplicates and original order are retained. No I/O/allocation.
 * Validate the entire bin on every call. Caller keeps input immutable and
 * output disjoint from input. Outputs unchanged on failure. No pointers retained.
 * Count can be zero on success; Entry selects a zero-based inventory ordinal. */
int T70AwPreflightCount(const void *Bin,size_t Bytes,uint32_t *Count);
int T70AwPreflightEntry(const void *Bin,size_t Bytes,uint32_t Ordinal,
                        T70_AW_PREFLIGHT_ENTRY *Entry);
#endif
