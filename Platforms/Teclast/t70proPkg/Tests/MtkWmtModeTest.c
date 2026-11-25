// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkWmtMode.h>
#ifdef STP_TEST_NATIVE
#include <stdio.h>
#endif

unsigned T70WmtModeAssertions;
#define CHECK(Condition) do { ++T70WmtModeAssertions; if (!(Condition)) return __LINE__; } while (0)

static const unsigned char Query[] = {1, 4, 1, 0, 4};
static const unsigned char Set[] = {1, 4, 5, 0, 3, 0xdb, 0x0e, 0x68, 1};
static const unsigned char Initial[] = {2, 4, 6, 0, 0, 4, 0x11, 0, 0, 0};
static const unsigned char Changed[] = {2, 4, 2, 0, 0, 3};
static const unsigned char Verified[] = {2, 4, 6, 0, 0, 4, 0xdb, 0x0e, 0x68, 1};
static const unsigned char Ack[] = {0x80, 0, 0, 0x80};

static MTK_WMT_MODE Mode;
static MTK_STP_FULL_LINK Peer;
static unsigned char Inbound[128], Outbound[128];
static unsigned Bytes, LastFull, Writes, Block, Short;

static MTK_STP_STATUS Capture(unsigned Full, const unsigned char *Data, unsigned Length, unsigned *Written)
{
  if (Block) {
    *Written = 0;
    return MtkStpFull;
  }
  if (Length > sizeof(Outbound)) return MtkStpInvalid;
  for (unsigned I = 0; I < Length; ++I) Outbound[I] = Data[I];
  Bytes = Length;
  LastFull = Full;
  ++Writes;
  *Written = Short ? Length - 1 : Length;
  return MtkStpOk;
}

static MTK_STP_STATUS MandatoryWrite(void *Context, const unsigned char *Data, unsigned Length, unsigned *Written)
{
  if (Context != &Mode) return MtkStpInvalid;
  return Capture(0, Data, Length, Written);
}

static MTK_STP_STATUS FullWrite(void *Context, const unsigned char *Data, unsigned Length, unsigned *Written)
{
  if (Context != &Mode) return MtkStpInvalid;
  return Capture(1, Data, Length, Written);
}

static MTK_STP_STATUS PeerWrite(void *Context, const unsigned char *Data, unsigned Length, unsigned *Written)
{
  unsigned char *Output = Context;
  if (Length > 64) return MtkStpInvalid;
  for (unsigned I = 0; I < Length; ++I) Output[I] = Data[I];
  *Written = Length;
  return MtkStpOk;
}

static int Equals(const unsigned char *A, const unsigned char *B, unsigned Length)
{
  for (unsigned I = 0; I < Length; ++I)
    if (A[I] != B[I]) return 0;
  return 1;
}

static int Mandatory(unsigned Channel, const unsigned char *Reply, unsigned Length, unsigned Split, uint64_t Now)
{
  unsigned Encoded;
  CHECK(MtkStpEncode(MTK_STP_BTIF_MANDATORY, Channel, Reply, Length, Inbound,
                     sizeof(Inbound), &Encoded) == MtkStpOk && Split <= Encoded);
  CHECK(MtkWmtModeReceive(&Mode, Inbound, Split, Now) == MtkWmtPending);
  CHECK(MtkWmtModeReceive(&Mode, Inbound + Split, Encoded - Split, Now) == MtkWmtPending);
  return 0;
}

static int Start(unsigned Split)
{
  Bytes = LastFull = Writes = Block = Short = 0;
  CHECK(MtkWmtModeInit(&Mode, &Mode, MandatoryWrite, FullWrite, 0) == MtkWmtPending);
  Block = 1;
  CHECK(MtkWmtModeService(&Mode, 0) == MtkWmtPending && Writes == 0);
  Block = 0;
  CHECK(MtkWmtModeService(&Mode, 1) == MtkWmtPending && Writes == 1 && !LastFull);
  CHECK(Bytes == sizeof(Query) + 6 && Equals(Outbound + 4, Query, sizeof(Query)));
  CHECK(MtkWmtModeService(&Mode, 2) == MtkWmtPending && Writes == 1);
  CHECK(!Mandatory(0, Initial, 3, 1, 2));
  CHECK(!Mandatory(4, Initial, 3, 2, 2));
  CHECK(Mode.Stage == MtkWmtModeQueryMandatory);
  CHECK(!Mandatory(4, Initial + 3, sizeof(Initial) - 3, Split, 3));
  CHECK(MtkWmtModeService(&Mode, 4) == MtkWmtPending);
  CHECK(Writes == 2 && !LastFull && Bytes == sizeof(Set) + 6 &&
        Equals(Outbound + 4, Set, sizeof(Set)));
  return 0;
}

static int Switch(unsigned Split)
{
  CHECK(!Start(Split));
  unsigned Encoded;
  // A full-mode frame may follow the mandatory ACK in one FIFO service pass.
  const unsigned char Noise = 0x11;
  CHECK(MtkStpEncode(MTK_STP_BTIF_MANDATORY, 4, Changed, sizeof(Changed),
                     Inbound, sizeof(Inbound), &Encoded) == MtkStpOk);
  CHECK(MtkStpFullInit(&Peer, Inbound + Encoded, PeerWrite) == MtkStpOk);
  CHECK(MtkStpFullTransmit(&Peer, 0, &Noise, 1, 5) == MtkStpOk);
  CHECK(MtkWmtModeReceive(&Mode, Inbound, Encoded + Peer.TxLength, 5) == MtkWmtPending);
  CHECK(Mode.Stage == MtkWmtModeQueryFull && Mode.Link.RxNext == 1 && Mode.Link.AckPending);
  CHECK(MtkWmtModeService(&Mode, 5) == MtkWmtPending && Writes == 3 && LastFull);
  CHECK(Bytes == sizeof(Ack) && Equals(Outbound, Ack, sizeof(Ack)));
  CHECK(MtkStpFullReceive(&Peer, Outbound, Bytes, &Encoded, &(MTK_STP_FRAME){0}) == MtkStpNeedMore);
  CHECK(Peer.TxLength == 0);
  CHECK(MtkWmtModeService(&Mode, 14) == MtkWmtPending && Writes == 3);
  Block = 1;
  CHECK(MtkWmtModeService(&Mode, 15) == MtkWmtPending && Writes == 3);
  Block = 0;
  CHECK(MtkWmtModeService(&Mode, 16) == MtkWmtPending && Writes == 4 && LastFull);
  CHECK(Bytes == sizeof(Query) + 6 && Equals(Outbound + 4, Query, sizeof(Query)));
  CHECK(Outbound[0] == 0x80 && Outbound[1] == 0x40 && Outbound[2] == 5 && Outbound[3] == 0xc5);
  return 0;
}

static int Confirm(unsigned Split, int CommandAckFirst)
{
  CHECK(!Switch(Split));
  unsigned Used;
  MTK_STP_FRAME Request;
  CHECK(MtkStpFullReceive(&Peer, Outbound, Bytes, &Used, &Request) == MtkStpFrameReady);
  CHECK(Used == Bytes && Request.Channel == 4 && Request.Length == sizeof(Query));
  if (CommandAckFirst) {
    CHECK(MtkStpFullAccept(&Peer) == MtkStpOk);
    CHECK(MtkStpFullService(&Peer, 17) == MtkStpOk);
    CHECK(MtkWmtModeReceive(&Mode, Peer.Context, sizeof(Ack), 17) == MtkWmtPending);
    CHECK(Mode.Link.TxLength == 0 && MtkWmtModeService(&Mode, 17) == MtkWmtPending);
  }
  CHECK(MtkStpFullTransmit(&Peer, 4, Verified, sizeof(Verified), 17) == MtkStpOk);
  const unsigned char *Data = Peer.Context;
  CHECK(MtkWmtModeReceive(&Mode, Data, Split, 17) == MtkWmtPending);
  CHECK(MtkWmtModeReceive(&Mode, Data + Split, Peer.TxLength - Split, 17) == MtkWmtPending);
  CHECK(Mode.Stage == MtkWmtModeReady && Mode.Link.AckPending);
  if (!CommandAckFirst) {
    Block = 1;
    CHECK(MtkWmtModeService(&Mode, 17) == MtkWmtPending && Mode.Link.TxLength != 0);
    CHECK(MtkStpFullAccept(&Peer) == MtkStpOk);
    CHECK(MtkStpFullService(&Peer, 18) == MtkStpOk);
    CHECK(MtkWmtModeReceive(&Mode, Peer.Context, sizeof(Ack), 18) == MtkWmtPending);
  }
  Block = 1;
  CHECK(MtkWmtModeService(&Mode, 19) == MtkWmtPending && Mode.Link.AckPending);
  Block = 0;
  CHECK(MtkWmtModeService(&Mode, 20) == MtkWmtOk && Mode.Link.RxNext == 2 && Mode.Link.TxNext == 1);
  CHECK(Bytes == 4 && Outbound[0] == 0x81 && Outbound[3] == 0x81 && LastFull);
  return 0;
}

static int Failures(void)
{
  unsigned Encoded;
  const unsigned char Denied[] = {2, 4, 2, 0, 1, 3};
  const unsigned char Wrong[] = {2, 4, 2, 0, 0, 4};
  CHECK(!Start(4));
  CHECK(MtkStpEncode(MTK_STP_BTIF_MANDATORY, 4, Denied, sizeof(Denied),
                     Inbound, sizeof(Inbound), &Encoded) == MtkStpOk);
  CHECK(MtkWmtModeReceive(&Mode, Inbound, Encoded, 6) == MtkWmtRejected);
  CHECK(MtkWmtModeService(&Mode, 7) == MtkWmtRejected && Writes == 2);
  CHECK(!Start(5));
  CHECK(MtkStpEncode(MTK_STP_BTIF_MANDATORY, 4, Wrong, sizeof(Wrong),
                     Inbound, sizeof(Inbound), &Encoded) == MtkStpOk);
  CHECK(MtkWmtModeReceive(&Mode, Inbound, Encoded, 6) == MtkWmtMalformed);
  CHECK(MtkWmtModeService(&Mode, 7) == MtkWmtMalformed && Writes == 2);
  CHECK(!Start(6));
  CHECK(MtkWmtModeService(&Mode, 4 + MTK_WMT_MODE_TIMEOUT_MS - 1) == MtkWmtPending);
  CHECK(MtkWmtModeService(&Mode, 4 + MTK_WMT_MODE_TIMEOUT_MS) == MtkWmtUncertain);
  CHECK(MtkWmtModeReceive(&Mode, Inbound, Encoded, 5 + MTK_WMT_MODE_TIMEOUT_MS) == MtkWmtUncertain);
  CHECK(Writes == 2);
  CHECK(!Switch(7));
  CHECK(MtkWmtModeService(&Mode, 16 + MTK_STP_FULL_ACK_MS) == MtkWmtPending);
  CHECK(Writes == 5 && Bytes == sizeof(Query) + 10 &&
        Equals(Outbound, (const unsigned char[]){0x7f, 0x7f, 0x7f, 0x7f}, 4));
  CHECK(MtkWmtModeService(&Mode, 16 + MTK_WMT_MODE_TIMEOUT_MS) == MtkWmtUncertain);
  CHECK(Writes == 5);
  CHECK(MtkWmtModeInit(&Mode, &Mode, MandatoryWrite, FullWrite, 0) == MtkWmtPending);
  Short = 1;
  CHECK(MtkWmtModeService(&Mode, 1) == MtkWmtUncertain);
  Short = 0;
  CHECK(MtkWmtModeService(&Mode, 2) == MtkWmtUncertain && Writes == 6);
  return 0;
}

int TestMain(void)
{
  for (unsigned Split = 0; Split <= 13; ++Split) {
    int Line = Confirm(Split, 0);
    if (Line) return Line;
    Line = Confirm(Split, 1);
    if (Line) return Line;
  }
  return Failures();
}

#ifdef STP_TEST_NATIVE
int main(void)
{
  int Line = TestMain();
  if (Line) fprintf(stderr, "MtkWmtModeTest.c:%d\n", Line);
  else printf("WMT mode switch passed %u assertions\n", T70WmtModeAssertions);
  return !!Line;
}
#endif
