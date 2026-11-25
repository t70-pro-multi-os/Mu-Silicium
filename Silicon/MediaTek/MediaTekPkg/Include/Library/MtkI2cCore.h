// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_I2C_CORE_H
#define MTK_I2C_CORE_H
/* OS-neutral MT6983 register-v2/APDMA-v1 engine. Callers serialize ownership,
 * supply coherent mapped DMA buffers and a live, measured source clock.
 * Register/timing evidence: MTK i2c_mt65xx.c, reference pin documented in README.
 * This is not an interface for arbitrary MediaTek I2C variants. */
typedef unsigned char MI_U8;
typedef unsigned short MI_U16;
typedef unsigned int MI_U32;
typedef unsigned long long MI_U64;
typedef char MI_TYPES_MUST_MATCH[(sizeof(MI_U8)==1 && sizeof(MI_U16)==2 && sizeof(MI_U32)==4 && sizeof(MI_U64)==8)?1:-1];
typedef enum { MiOk, MiInvalid, MiUnsupported, MiNack, MiArbitration,
               MiTimeout, MiCancelled, MiDmaStuck, MiBusError } MI_STATUS;
typedef struct {
  MI_U16 Timing, LowTiming, Ext, Sda, ClockDiv;
  MI_U32 ActualHz;
} MI_TIMING;
typedef struct {
  void *Context;
  MI_U32 (*Read)(void *, unsigned, unsigned, unsigned);
  void (*Write)(void *, unsigned, unsigned, unsigned, MI_U32);
  MI_U64 (*NowUs)(void *);
  void (*DelayUs)(void *, unsigned);
  void (*Fence)(void *);
  int (*Cancelled)(void *);
  /* Optional bounded sleep. Wakeups are hints; raw status remains authoritative.
   * Caller clears its event once before the transfer, never between status reads. */
  void (*WaitForEvent)(void *, unsigned MaximumUs);
} MI_IO;
typedef struct {
  MI_IO Io;
  MI_TIMING Timing;
  MI_U32 SourceHz, RequestedHz;
  int Ready, DmaSafe, SequenceHeld;
} MI_CONTROLLER;
/* Region 0: controller 32-bit MMIO/8-bit FIFO (shipped MT6983 variant); region 1: APDMA 32-bit.
 * Max transfer 65535 bytes in each direction, unshifted 7-bit non-reserved address.
 * Tx+Rx is one repeated-START transaction. No STOP is inserted between them.
 * No implicit retry: replay could duplicate a charger/register write. */
MI_STATUS MiCalculateTiming(MI_U32 SourceHz, MI_U32 BusHz, MI_TIMING *Timing);
MI_STATUS MiInitialize(MI_CONTROLLER *Controller, const MI_IO *Io,
                       MI_U32 SourceHz, MI_U32 BusHz);
MI_STATUS MiSetFrequency(MI_CONTROLLER *Controller, MI_U32 BusHz, MI_U32 *ActualHz);
MI_STATUS MiTransfer(MI_CONTROLLER *Controller, unsigned Address,
                     const MI_U8 *Tx, MI_U32 TxLength, MI_U64 TxDma,
                     MI_U8 *Rx, MI_U32 RxLength, MI_U64 RxDma);
MI_STATUS MiTransferTimed(MI_CONTROLLER *Controller, unsigned Address,
                          const MI_U8 *Tx, MI_U32 TxLength, MI_U64 TxDma,
                          MI_U8 *Rx, MI_U32 RxLength, MI_U64 RxDma,
                          MI_U32 TimeoutUs);
#define MI_MAX_MESSAGES 32U
typedef struct {
  MI_U8 *Buffer;
  MI_U32 Length, DelayUs;
  MI_U64 Dma;
  unsigned Read;
} MI_MESSAGE;
/* One address, repeated START between messages and a final STOP. Validate all
 * buffers before access; no retry or reset between successful messages.
 * Completed counts whole messages only, even when a later transaction fails. */
MI_STATUS MiSequence(MI_CONTROLLER *Controller, unsigned Address,
                     const MI_MESSAGE *Messages, unsigned Count,
                     unsigned *Completed);
MI_STATUS MiStop(MI_CONTROLLER *Controller);
#endif
