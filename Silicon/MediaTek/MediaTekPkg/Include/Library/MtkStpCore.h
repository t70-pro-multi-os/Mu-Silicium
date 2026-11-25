// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_STP_CORE_H
#define MTK_STP_CORE_H
/* Mandatory STP framing and channel handling from the shipped wmt_drv.ko.
 * The full-mode link with acknowledgements lives in MtkStpFull.h. */
#define MTK_STP_UART_MANDATORY 2U
#define MTK_STP_BTIF_MANDATORY 8U
#define MTK_STP_SDIO 16U
#define MTK_STP_GPS_CHANNEL 2U
#define MTK_STP_TX_MAX 1996U
#define MTK_STP_RX_MAX 1999U
#define MTK_STP_FRAME_MAX (MTK_STP_RX_MAX+6U)
typedef enum {
  MtkStpOk, MtkStpInvalid, MtkStpUnsupported, MtkStpNeedMore,
  MtkStpFrameReady, MtkStpMalformed, MtkStpFaulted, MtkStpIoError,
  MtkStpStale, MtkStpFull
} MTK_STP_STATUS;
typedef struct {
  unsigned Mode, State, Length, Used, Channel, Padding, Faulted;
  unsigned char Payload[MTK_STP_RX_MAX];
} MTK_STP_DECODER;
typedef struct {
  unsigned Channel, Length;
  const unsigned char *Data;
} MTK_STP_FRAME;
/* One synchronous, serialized write; Written reports bytes actually accepted.
 * MtkStpFull with Written == 0 guarantees no acceptance or ambiguous IO and
 * permits a later retry. Other errors, including Full with nonzero Written,
 * remain uncertain. The backend owns wake/power/transport and bounds its wait. */
typedef MTK_STP_STATUS (*MTK_STP_WRITE)(void *Context,
  const unsigned char *Data, unsigned Length, unsigned *Written);
typedef struct {
  void *Context;
  MTK_STP_WRITE Write;
  unsigned Mode, Faulted;
  unsigned char Buffer[MTK_STP_FRAME_MAX];
} MTK_STP_TRANSMITTER;
/* Reinitialize only after the owner has drained/reset the physical stream.
 * All operations on an object must be serialized by its OS owner. */
MTK_STP_STATUS MtkStpDecoderInit(MTK_STP_DECODER *Decoder, unsigned Mode);
/* Consume up to one complete frame, preserving partial header/payload/tail and
 * SDIO receive padding. Frame is unchanged unless FrameReady is returned; its
 * data remains valid until the next Receive/Init call. On malformed input the
 * decoder latches failure instead of searching payload bytes for a new sync.
 * Header byte 3 and the two mandatory-mode tail bytes must be zero. The stock
 * parser only logs a nonzero tail; this port intentionally rejects it. */
MTK_STP_STATUS MtkStpReceive(MTK_STP_DECODER *Decoder,
  const unsigned char *Data, unsigned Length, unsigned *Consumed,
  MTK_STP_FRAME *Frame);
/* Produces the exact bytes passed to the stock physical TX callback. SDIO
 * transfer alignment is the physical backend's responsibility. Input and
 * output may overlap. Length must be 1..1996, matching public stock send_data. */
MTK_STP_STATUS MtkStpEncode(unsigned Mode, unsigned Channel,
  const unsigned char *Data, unsigned Length, unsigned char *Output,
  unsigned Capacity, unsigned *Written);
MTK_STP_STATUS MtkStpTransmitterInit(MTK_STP_TRANSMITTER *Transmitter,
  unsigned Mode, void *Context, MTK_STP_WRITE Write);
/* Zero-accepted Full returns backpressure without faulting; WMT Begin maps
 * that case to Busy without a pending command. A partial/failed write latches
 * failure. Retrying the whole frame could send
 * a command twice or splice into a partial frame, so recovery is explicit. */
MTK_STP_STATUS MtkStpTransmit(MTK_STP_TRANSMITTER *Transmitter,
  unsigned Channel, const unsigned char *Data, unsigned Length);

#define MTK_STP_WMT_CHANNEL 4U
#define MTK_WMT_GPS_FUNCTION 2U
#define MTK_WMT_REGISTER_OPCODE 8U
#define MTK_WMT_SOC_GPS_SYNC_FLAG 0x80U
#define MTK_WMT_GPS_MCU_OPCODE 0x32U
#define MTK_WMT_GPS_MCU_TX_MAX (MTK_STP_TX_MAX-4U)
#define MTK_WMT_GPS_MCU_RX_MAX (MTK_STP_RX_MAX-4U)
typedef enum {
  MtkWmtOk, MtkWmtInvalid, MtkWmtBusy, MtkWmtPending, MtkWmtUnrelated,
  MtkWmtRejected, MtkWmtMalformed, MtkWmtUncertain, MtkWmtUnsupported
} MTK_WMT_STATUS;
typedef struct {
  MTK_STP_TRANSMITTER *Transmitter;
  unsigned Pending, Faulted, Confirmed, Function, Enable, FirmwareStatus;
  unsigned Opcode, ReplyCapacity, ReplyLength, GpsPowerSave;
  unsigned RegisterWrite, RegisterAddress, RegisterValue;
  unsigned char Reply[MTK_WMT_GPS_MCU_RX_MAX];
} MTK_WMT_CONTROL;
/* One shared WMT control owner: opcode 6 replies contain no function
 * ID or transaction ID. Serialize every function-on/off command through this
 * object with exactly-once, ordered responses from the transport. A duplicate
 * arriving while the next command is pending cannot be distinguished. Drain
 * old replies before Init; an uncertain outcome requires
 * external stream/firmware recovery before reinitialization. This implements
 * function-control, register and MCU transport stages, not conninfra boot, full GPS
 * pre-control, LNA or coexistence. */
MTK_WMT_STATUS MtkWmtControlInit(MTK_WMT_CONTROL *Control,
  MTK_STP_TRANSMITTER *Transmitter);
MTK_WMT_STATUS MtkWmtControlBegin(MTK_WMT_CONTROL *Control,
  unsigned Function, unsigned Enable);
/* Dispatch only validated STP frames. Other channels/opcodes remain available
 * to their owners. Confirmed means this command's success reply was received,
 * not that the positioning engine or antenna has started. */
MTK_WMT_STATUS MtkWmtControlEvent(MTK_WMT_CONTROL *Control,
  const MTK_STP_FRAME *Frame);
MTK_WMT_STATUS MtkWmtControlExpire(MTK_WMT_CONTROL *Control);

/* Opcode 8 addresses the connectivity firmware's register space, not host
 * MMIO. Write is 0 (read) or 1 (masked write). Preserve Value and Mask on the
 * wire, including the input Value for a read. Reads require a matching echoed
 * address before publishing RegisterValue; it is valid only with Confirmed,
 * Opcode==8 and RegisterWrite==0. Writes confirm the command ACK, not readback.
 * Reply bytes 5..7 are opaque. This shares the MCU/function timeout and owner. */
MTK_WMT_STATUS MtkWmtRegisterBegin(MTK_WMT_CONTROL *Control,
  unsigned Write,unsigned Address,unsigned Value,unsigned Mask);
/* The GPS sync branch of stock mtk_wcn_soc_pin_ctrl (pin 3, state 6/7).
 * SocPinFlags is byte 9 of the initialized SoC operations, not an assumed board
 * constant. Without bit 7, return Unsupported without sending or confirming a
 * command. Static stock flags are zero; firmware/chip initialization must first
 * establish the runtime flags. This is one pre-control step, not GPS-on. */
MTK_WMT_STATUS MtkWmtGpsSyncBegin(MTK_WMT_CONTROL *Control,
  unsigned SocPinFlags,unsigned Enable);

/* The same owner serializes opcode 0x32 with function-control commands. Reply
 * bytes are opaque and remain valid until Init or the next Begin call. Generic
 * MCU Confirmed means a complete transport reply, not semantic command success.
 * Input must not alias Control; it may alias the transmitter's staging buffer.
 * Neither this API nor the backend may reenter this control object from Write. */
MTK_WMT_STATUS MtkWmtGpsMcuBegin(MTK_WMT_CONTROL *Control,
  const unsigned char *Data,unsigned Length,unsigned ReplyCapacity);
/* Stock GPS_hw_suspend_ctrl sends {Suspend?4:5, Control} via opcode 0x32,
 * requests three reply bytes and uses reply[0] as status. Require at least two
 * actual body bytes; stock can accept a missing status from its cleared buffer.
 * Control is the original opaque byte, not an inferred band selector. Status 6
 * remains a rejection; higher-level resource reconciliation is not implemented.
 * A successful MCU ACK alone does not establish whole-device suspend/resume. */
MTK_WMT_STATUS MtkWmtGpsPowerSaveBegin(MTK_WMT_CONTROL *Control,
  unsigned Suspend,unsigned FirmwareControl);

#define MTK_STP_QUEUE_BYTES 16384U
#define MTK_STP_QUEUE_CAPACITY (MTK_STP_QUEUE_BYTES-1U)
typedef struct {
  unsigned Channel, Epoch, Head, Tail, Count, Faulted;
  unsigned char Data[MTK_STP_QUEUE_BYTES];
} MTK_STP_QUEUE;
/* GPS uses channel 2. Preserve opaque bytes across STP frame boundaries in
 * the stock-size ring. Init is for a new, quiesced object only; Reset advances
 * Epoch so callbacks captured before a receiver restart cannot append stale
 * data. The owner must reset the decoder and drain the physical stream too.
 * All queue/decoder/control calls share the owner's serialization. */
MTK_STP_STATUS MtkStpQueueInit(MTK_STP_QUEUE *Queue,unsigned Channel);
MTK_STP_STATUS MtkStpQueueReset(MTK_STP_QUEUE *Queue,unsigned *Epoch);
/* Source data must not alias Queue. Overflow invalidates and wipes buffered
 * bytes: a truncated opaque GPS stream must not become an apparently valid
 * new position. The owner must recover before any more data is consumed. */
MTK_STP_STATUS MtkStpQueuePush(MTK_STP_QUEUE *Queue,unsigned Epoch,
  const MTK_STP_FRAME *Frame);
/* A nonzero-capacity empty read returns NeedMore; this function never blocks.
 * Data and Read must not alias Queue. OS cancellation/wait and position
 * processing belong to the receiver owner. */
MTK_STP_STATUS MtkStpQueueRead(MTK_STP_QUEUE *Queue,unsigned Epoch,
  unsigned char *Data,unsigned Capacity,unsigned *Read);
#endif
