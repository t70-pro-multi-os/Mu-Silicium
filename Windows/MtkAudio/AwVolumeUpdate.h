// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_VOLUME_UPDATE_H
#define T70_AW_VOLUME_UPDATE_H
#include "AwVolume.h"
typedef struct {
  void *Context;
  /* Zero means complete success; every other status is returned unchanged in
   * TransportStatus. Callbacks must finish synchronously before returning. */
  int32_t (*Read)(void *Context,uint16_t Register,uint16_t *Value);
  int32_t (*Write)(void *Context,uint16_t Register,uint16_t Value);
} T70_AW_VOLUME_IO;
typedef enum {
  T70AwVolumeUpdated,
  T70AwVolumeInvalid,
  T70AwVolumeReadFailed,
  T70AwVolumeWriteFailed
} T70_AW_VOLUME_UPDATE_CODE;
typedef struct {
  T70_AW_VOLUME_UPDATE_CODE Code;
  int32_t TransportStatus;
} T70_AW_VOLUME_UPDATE_RESULT;
/* Explicit callback operation, not a board/OS discovery mechanism. Caller owns
 * the verified target and serializes the complete read/compute/write sequence.
 * Initial is a decoded profile value (0..783); Requested is attenuation 0..720
 * in 1/8 dB units. Reject invalid inputs before I/O. AppliedRegister changes
 * only after complete write success. A failed write may still affect hardware;
 * no retry, rollback or power/reset operation is performed. All pointers must
 * remain valid throughout this synchronous call, including callback execution. */
T70_AW_VOLUME_UPDATE_RESULT T70Aw88261UpdateVolume(const T70_AW_VOLUME_IO *Io,
  uint16_t Initial,uint32_t Requested,uint16_t *AppliedRegister);
#endif
