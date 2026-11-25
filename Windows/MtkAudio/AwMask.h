// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_MASK_H
#define T70_AW_MASK_H
#include <stdint.h>
typedef struct {
  void *Context;
  int (*Read)(void *Context,uint16_t Register,uint16_t *Value);
  int (*Write)(void *Context,uint16_t Register,uint16_t Value);
} T70_AW_REGISTER_IO;
/* Callbacks return nonzero only for complete success. Mask bits are PRESERVED,
 * not selected for replacement. Caller serializes the entire read/write pair.
 * No register access permissions, calibration or power policy is inferred.
 * Returns zero on invalid input/read/write failure. No retries or rollback. */
int T70AwUpdatePreservedBits(const T70_AW_REGISTER_IO *Io,uint16_t Register,
                            uint16_t PreserveMask,uint16_t Value);
#endif
