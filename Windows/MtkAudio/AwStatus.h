// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_STATUS_H
#define T70_AW_STATUS_H
#include <stdint.h>
#define T70_AW88261_STATUS_REGISTER UINT16_C(0x01)
typedef struct {
  uint16_t Raw,UninterpretedBits;
  uint8_t PllLocked,ClockStable,ClockPairSet;
} T70_AW_CLOCK_SNAPSHOT;
typedef struct {
  void *Context;
  /* One synchronous read: zero only for complete success. */
  int32_t (*Read)(void *Context,uint16_t Register,uint16_t *Value);
} T70_AW_STATUS_IO;
typedef enum { T70AwStatusRead,T70AwStatusInvalid,T70AwStatusReadFailed } T70_AW_STATUS_CODE;
typedef struct { T70_AW_STATUS_CODE Code;int32_t TransportStatus; } T70_AW_STATUS_RESULT;
/* One read, no poll/write/reset/state transition. On any failure Snapshot is
 * unchanged. Only PLL bit0 and stable-clock bit4 are interpreted; all other
 * bits are retained without claiming fault-free, ready or safe-to-transmit.
 * The caller owns the verified target and pointer lifetimes for this call. */
T70_AW_STATUS_RESULT T70Aw88261ReadClockStatus(const T70_AW_STATUS_IO *Io,
                                             T70_AW_CLOCK_SNAPSHOT *Snapshot);
#endif
