// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_WMT_MODE_H
#define MTK_WMT_MODE_H

#include <Library/MtkStpFull.h>

#define MTK_WMT_MODE_SETTLE_MS 10U
#define MTK_WMT_MODE_TIMEOUT_MS 2000U

typedef enum {
  MtkWmtModeQueryMandatory, MtkWmtModeSetFull, MtkWmtModeQueryFull,
  MtkWmtModeReady
} MTK_WMT_MODE_STAGE;

typedef struct {
  MTK_STP_DECODER Decoder;
  MTK_STP_FULL_LINK Link;
  void *Context;
  MTK_STP_WRITE WriteMandatory;
  uint64_t SinceMs;
  MTK_WMT_MODE_STAGE Stage;
  MTK_WMT_STATUS Failure;
  unsigned Sent, ReplyBytes;
} MTK_WMT_MODE;

/* Start on a newly booted MCU in mandatory mode. The owner serializes RX,
 * timer and writes and drains the FIFO after Service. A failure is terminal for
 * this MCU session, including a missing response to the set-mode command.
 * Do not run other WMT transactions in parallel. */
MTK_WMT_STATUS MtkWmtModeInit(MTK_WMT_MODE *Mode, void *Context,
  MTK_STP_WRITE WriteMandatory, MTK_STP_WRITE WriteFull, uint64_t NowMs);

/* Pending includes queue pressure. Uncertain includes timeout or partial IO.
 * Ok means both the new mode's WMT response and the command's STP ACK arrived,
 * and any response ACK was accepted by the TX backend. Subsequent traffic
 * can use Link directly with the same serial ownership. */
MTK_WMT_STATUS MtkWmtModeService(MTK_WMT_MODE *Mode, uint64_t NowMs);
/* Route received FIFO bytes in order through the current decoder. When the
 * set-mode reply ends, remaining bytes in the batch already use full mode.
 * Non-WMT payloads during bring-up are acknowledged and discarded. */
MTK_WMT_STATUS MtkWmtModeReceive(MTK_WMT_MODE *Mode,
  const unsigned char *Data, unsigned Length, uint64_t NowMs);

#endif
