// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_BTIF_CORE_H
#define MTK_BTIF_CORE_H
#include <Library/MtkStpCore.h>
/* Register/FIFO operations recovered from the shipped btif_drv.ko. Regions
 * 0/1/2 are DT reg entries 0/1/2. Only region 0 is ever written. Width is bytes.
 * Read/Write return zero only on success; an error may have reached hardware.
 * Barrier must complete the stock DSB SY after each write. The OS owns clocks,
 * mappings, IRQ registration, serialization and conninfra/peer quiescence. */
typedef struct {
  void *Context;
  int (*Read)(void *Context,unsigned Region,unsigned Offset,unsigned Width,unsigned *Value);
  int (*Write)(void *Context,unsigned Region,unsigned Offset,unsigned Width,unsigned Value);
  void (*Barrier)(void *Context);
} MTK_BTIF_IO;
typedef enum {
  MtkBtifOk, MtkBtifInvalid, MtkBtifBusy, MtkBtifMore,
  MtkBtifNotOurs, MtkBtifFaulted, MtkBtifIoError
} MTK_BTIF_STATUS;
#define MTK_BTIF_TX_BYTES 4096U
#define MTK_BTIF_RX_BUDGET 256U
/* Stock defaults in mtk_btif_info, not inferred baud rates or byte counts. */
#define MTK_BTIF_TX_TRIGGER 8U
#define MTK_BTIF_RX_TRIGGER 1U
typedef struct {
  MTK_BTIF_IO Io;
  unsigned Initialized,Owned,Running,Faulted,Uncertain;
  unsigned Saved[5],SavedWake,WakeOwned,Head,Tail,Count;
  unsigned char Tx[MTK_BTIF_TX_BYTES];
} MTK_BTIF_CORE;
typedef struct {
  unsigned Received,Transmitted,More,Handled;
} MTK_BTIF_SERVICE;
/* Init is software-only, for a fresh object or after external stream/hardware
 * recovery, never to discard an active lease. Buffers/IO/results must not alias
 * Core; input/output and RX/result buffers must not overlap each other. All calls, including STP calls and interrupt work, share one owner lock. */
MTK_BTIF_STATUS MtkBtifInit(MTK_BTIF_CORE *Core,const MTK_BTIF_IO *Io);
/* Requires exclusive OS ownership and a quiesced peer. Reject active DMA, live
 * controller IRQs, unread RX or unfinished TX before any FIFO reset/write.
 * On a failure after the first write, retain the baseline and lease for Stop. */
MTK_BTIF_STATUS MtkBtifStart(MTK_BTIF_CORE *Core,unsigned TxTrigger,unsigned RxTrigger);
/* The stock wake pulse clears controller 0x64 bit 0, waits 128..160 us, then
 * sets it. Wait returns zero only after at least MinimumUs; it must be bounded
 * and must not reenter Core. This is an electrical request, not a firmware ACK
 * or RF power-on. Require empty TX/RX and retain the inherited pin level for
 * Stop. Any failure latches the owner until explicit recovery/idle restore. */
typedef int (*MTK_BTIF_WAIT_US)(void *Context,unsigned MinimumUs,unsigned MaximumUs);
MTK_BTIF_STATUS MtkBtifRaiseWake(MTK_BTIF_CORE *Core,MTK_BTIF_WAIT_US Wait);
/* Real MTK_STP_WRITE backend. Atomically accepts one complete mandatory frame
 * into the 4096-byte queue, then enables TX IRQ. Written means queued, not sent
 * or acknowledged. Queue pressure is returned without accepting a prefix.
 * Supports 2002 encoded bytes directly; the old WMT wrapper's 2000-byte limit
 * is not a hardware limit. Do not use this backend for UART/SDIO framing. */
MTK_STP_STATUS MtkBtifStpWrite(void *Context,const unsigned char *Data,
  unsigned Length,unsigned *Written);
/* Backend for MtkStpFull. The full-mode encoder supplies the payload CRC.
 * Accept a data frame, an ACK, or a sync prefix and retransmission as one
 * queue entry. The owner must already have switched the peer to full mode. */
MTK_STP_STATUS MtkBtifFullStpWrite(void *Context,const unsigned char *Data,
  unsigned Length,unsigned *Written);
/* At most Capacity (1..256) RX bytes and one 8/16-byte TX batch. Return counts
 * even on an error; the owner must invalidate its stream after any fault.
 * Pass successful RX bytes to MtkStpReceive after releasing the hardware lock;
 * serialize that dispatch before servicing later bytes. More requests another
 * bounded pass/IRQ, not a tight wait. No callbacks run inside this function.
 * A byte-write error is ambiguous: latch failure and never replay that byte. */
MTK_BTIF_STATUS MtkBtifService(MTK_BTIF_CORE *Core,unsigned char *Rx,
  unsigned Capacity,MTK_BTIF_SERVICE *Result);
/* Never flush outstanding TX/RX. Quiesce the peer first. Restore controls only
 * after empty FIFO/queue and inactive DMA; a failed restore keeps its lease for
 * retry. Uncertain FIFO IO needs external stream/hardware recovery, not Stop.
 * FIFO reset pulses cannot be undone and are only issued at idle Start. */
MTK_BTIF_STATUS MtkBtifStop(MTK_BTIF_CORE *Core);
#endif
