// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_STP_FULL_H
#define MTK_STP_FULL_H

#include <Library/MtkStpCore.h>
#include <stdint.h>

/* BTIF switches from mandatory framing to full mode (4) during WMT startup.
 * A one-frame transmit window uses the same wire protocol as the stock window
 * of seven. Link ACKs confirm delivery, not completion of a WMT or HCI command. */
#define MTK_STP_BTIF_FULL 4U
#define MTK_STP_FULL_RX_MAX 2048U
#define MTK_STP_FULL_SYNC_BYTES 4U
#define MTK_STP_FULL_ACK_MS 180U
#define MTK_STP_FULL_RETRIES 10U

typedef struct {
  void *Context;
  MTK_STP_WRITE Write;
  uint64_t FirstSendMs, LastSendMs;
  unsigned TxNext, TxLength, RxNext, AckPending, Faulted;
  unsigned HeaderUsed, BodyUsed, Resync, SyncUsed, OfferedLength;
  unsigned char Header[4];
  unsigned char Body[MTK_STP_FULL_RX_MAX+2U];
  /* Keep space for the stock four-byte sync prefix when retrying. */
  unsigned char Tx[MTK_STP_FULL_SYNC_BYTES+MTK_STP_TX_MAX+6U];
} MTK_STP_FULL_LINK;

/* Call only at a confirmed mode switch or after resetting the peer, with
 * exclusive stream ownership. Serialize all calls, including the write
 * callback. Input/output argument storage must not overlap the link. */
MTK_STP_STATUS MtkStpFullInit(MTK_STP_FULL_LINK *Link,void *Context,
  MTK_STP_WRITE Write);

/* Transmit atomically accepts at most one outstanding frame. Call Service
 * after receive/accept and on a monotonic millisecond timer for ACKs, retries
 * and expiry. Full means zero acceptance for Transmit, or pending work for
 * Service. An ambiguous write or expired window faults until link recovery. */
MTK_STP_STATUS MtkStpFullTransmit(MTK_STP_FULL_LINK *Link,unsigned Channel,
  const unsigned char *Data,unsigned Length,uint64_t NowMs);
MTK_STP_STATUS MtkStpFullService(MTK_STP_FULL_LINK *Link,uint64_t NowMs);

/* Advance through at most one offered payload. ACK frames and discarded
 * bytes are consumed internally. Accept only after the owner has taken the
 * payload; the view remains valid until Accept or Init. Receive while a
 * payload is offered returns Full with no bytes consumed. */
MTK_STP_STATUS MtkStpFullReceive(MTK_STP_FULL_LINK *Link,
  const unsigned char *Data,unsigned Length,unsigned *Consumed,
  MTK_STP_FRAME *Frame);
MTK_STP_STATUS MtkStpFullAccept(MTK_STP_FULL_LINK *Link);

/* WMT function control over an already negotiated full-mode link. Opcode-6
 * replies have no transaction ID, so keep one outstanding command per link.
 * The owner serializes Begin, Event, Expire and all full-link operations. */
#define MTK_WMT_FULL_CONTROL_TIMEOUT_MS 2000U
typedef struct {
  MTK_STP_FULL_LINK *Link;
  uint64_t SinceMs;
  unsigned Pending,Faulted,Confirmed,Function,Enable,FirmwareStatus;
} MTK_WMT_FULL_CONTROL;
MTK_WMT_STATUS MtkWmtFullControlInit(MTK_WMT_FULL_CONTROL *Control,
  MTK_STP_FULL_LINK *Link);
MTK_WMT_STATUS MtkWmtFullControlBegin(MTK_WMT_FULL_CONTROL *Control,
  unsigned Function,unsigned Enable,uint64_t NowMs);
MTK_WMT_STATUS MtkWmtFullControlEvent(MTK_WMT_FULL_CONTROL *Control,
  const MTK_STP_FRAME *Frame);
MTK_WMT_STATUS MtkWmtFullControlExpire(MTK_WMT_FULL_CONTROL *Control,
  uint64_t NowMs);

#endif
