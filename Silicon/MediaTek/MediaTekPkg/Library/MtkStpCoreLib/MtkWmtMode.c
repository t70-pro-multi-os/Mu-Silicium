// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkWmtMode.h>

/* _wmt_soc_mode_ctrl of the shipped MT6789 wmt_drv.ko. The set-mode reply
 * uses mandatory framing. Enable the fresh full link for the second query. */
static const unsigned char Query[] = {1, 4, 1, 0, 4};
static const unsigned char SetFull[] = {1, 4, 5, 0, 3, 0xdb, 0x0e, 0x68, 1};
static const unsigned char MandatoryReply[] = {2, 4, 6, 0, 0, 4, 0x11, 0, 0, 0};
static const unsigned char SetReply[] = {2, 4, 2, 0, 0, 3};
static const unsigned char FullReply[] = {2, 4, 6, 0, 0, 4, 0xdb, 0x0e, 0x68, 1};

static MTK_WMT_STATUS Fail(MTK_WMT_MODE *Mode, MTK_WMT_STATUS Failure)
{
  Mode->Failure = Failure;
  return Failure;
}

MTK_WMT_STATUS MtkWmtModeInit(MTK_WMT_MODE *Mode, void *Context,
  MTK_STP_WRITE WriteMandatory, MTK_STP_WRITE WriteFull, uint64_t NowMs)
{
  if (!Mode || !WriteMandatory || !WriteFull) return MtkWmtInvalid;
  MtkStpDecoderInit(&Mode->Decoder, MTK_STP_BTIF_MANDATORY);
  MtkStpFullInit(&Mode->Link, Context, WriteFull);
  Mode->Context = Context;
  Mode->WriteMandatory = WriteMandatory;
  Mode->SinceMs = NowMs;
  Mode->Stage = MtkWmtModeQueryMandatory;
  Mode->Failure = MtkWmtOk;
  Mode->Sent = Mode->ReplyBytes = 0;
  return MtkWmtPending;
}

static MTK_STP_STATUS SendMandatory(MTK_WMT_MODE *Mode,
  const unsigned char *Command, unsigned Length)
{
  unsigned char Wire[sizeof(SetFull) + 6];
  unsigned Encoded, Written = 0;
  MTK_STP_STATUS Status = MtkStpEncode(MTK_STP_BTIF_MANDATORY,
    MTK_STP_WMT_CHANNEL, Command, Length, Wire, sizeof(Wire), &Encoded);
  if (Status != MtkStpOk) return Status;
  Status = Mode->WriteMandatory(Mode->Context, Wire, Encoded, &Written);
  if ((Status == MtkStpOk && Written != Encoded) ||
      (Status == MtkStpFull && Written)) return MtkStpIoError;
  return Status;
}

MTK_WMT_STATUS MtkWmtModeService(MTK_WMT_MODE *Mode, uint64_t NowMs)
{
  if (!Mode || !Mode->WriteMandatory) return MtkWmtInvalid;
  if (Mode->Failure != MtkWmtOk) return Mode->Failure;
  MTK_STP_STATUS Status;
  if (Mode->Stage >= MtkWmtModeQueryFull) {
    Status = MtkStpFullService(&Mode->Link, NowMs);
    if (Status != MtkStpOk && Status != MtkStpFull)
      return Fail(Mode, MtkWmtUncertain);
    if (Mode->Stage == MtkWmtModeReady && !Mode->Link.TxLength && !Mode->Link.AckPending)
      return MtkWmtOk;
  }
  if (NowMs - Mode->SinceMs >= MTK_WMT_MODE_TIMEOUT_MS)
    return Fail(Mode, MtkWmtUncertain);
  if (Mode->Stage == MtkWmtModeReady || Mode->Sent ||
      (Mode->Stage == MtkWmtModeQueryFull &&
                    NowMs - Mode->SinceMs < MTK_WMT_MODE_SETTLE_MS))
    return MtkWmtPending;
  switch (Mode->Stage) {
    case MtkWmtModeQueryMandatory:
      Status = SendMandatory(Mode, Query, sizeof(Query));
      break;
    case MtkWmtModeSetFull:
      Status = SendMandatory(Mode, SetFull, sizeof(SetFull));
      break;
    case MtkWmtModeQueryFull:
      Status = MtkStpFullTransmit(&Mode->Link, MTK_STP_WMT_CHANNEL,
        Query, sizeof(Query), NowMs);
      break;
    default:
      return Fail(Mode, MtkWmtMalformed);
  }
  if (Status == MtkStpOk) {
    Mode->Sent = 1;
    Mode->SinceMs = NowMs;
  } else if (Status != MtkStpFull) {
    return Fail(Mode, MtkWmtUncertain);
  }
  return MtkWmtPending;
}

static MTK_WMT_STATUS ReceiveReply(MTK_WMT_MODE *Mode,
  const MTK_STP_FRAME *Frame, uint64_t NowMs)
{
  static const struct {
    const unsigned char *Data;
    unsigned Length;
  } Expected[] = {
    {MandatoryReply, sizeof(MandatoryReply)},
    {SetReply, sizeof(SetReply)},
    {FullReply, sizeof(FullReply)}
  };
  if (!Mode->Sent || Mode->Stage >= MtkWmtModeReady ||
      NowMs - Mode->SinceMs >= MTK_WMT_MODE_TIMEOUT_MS)
    return Fail(Mode, MtkWmtUncertain);
  const unsigned char *Reply = Expected[Mode->Stage].Data;
  unsigned Length = Expected[Mode->Stage].Length;
  if (Frame->Length > Length - Mode->ReplyBytes)
    return Fail(Mode, MtkWmtMalformed);
  for (unsigned Index = 0; Index < Frame->Length; ++Index) {
    unsigned Position = Mode->ReplyBytes++;
    unsigned char Byte = Frame->Data[Index];
    if (Byte != Reply[Position])
      return Fail(Mode, Position == 4 ? MtkWmtRejected : MtkWmtMalformed);
  }
  if (Mode->ReplyBytes == Length) {
    ++Mode->Stage;
    Mode->Sent = Mode->ReplyBytes = 0;
    if (Mode->Stage != MtkWmtModeReady) Mode->SinceMs = NowMs;
  }
  return MtkWmtPending;
}

MTK_WMT_STATUS MtkWmtModeReceive(MTK_WMT_MODE *Mode,
  const unsigned char *Data, unsigned Length, uint64_t NowMs)
{
  if (!Mode || !Mode->WriteMandatory || (!Data && Length)) return MtkWmtInvalid;
  if (Mode->Failure != MtkWmtOk) return Mode->Failure;
  for (unsigned Position = 0; Position < Length;) {
    unsigned Consumed;
    MTK_STP_FRAME Frame;
    int Full = Mode->Stage >= MtkWmtModeQueryFull;
    MTK_STP_STATUS Status = Full
      ? MtkStpFullReceive(&Mode->Link, Data + Position, Length - Position, &Consumed, &Frame)
      : MtkStpReceive(&Mode->Decoder, Data + Position, Length - Position, &Consumed, &Frame);
    if (Status != MtkStpFrameReady && Status != MtkStpNeedMore)
      return Fail(Mode, MtkWmtMalformed);
    Position += Consumed;
    if (Status == MtkStpFrameReady) {
      if (Frame.Channel == MTK_STP_WMT_CHANNEL) {
        MTK_WMT_STATUS Reply = ReceiveReply(Mode, &Frame, NowMs);
        if (Reply != MtkWmtPending) return Reply;
      }
      if (Full) MtkStpFullAccept(&Mode->Link);
    }
  }
  return MtkWmtPending;
}
