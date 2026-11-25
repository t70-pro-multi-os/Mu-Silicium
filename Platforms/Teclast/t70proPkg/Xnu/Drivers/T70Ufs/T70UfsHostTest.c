/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define _POSIX_C_SOURCE 200112L
#include "T70UfsHost.h"

#include <stdio.h>
#include <string.h>

#define CHECK(Expression) do { \
  if (!(Expression)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #Expression); \
    Failures++; \
  } \
} while (0)

typedef union {
  uint64_t Alignment;
  uint8_t Bytes[MTK_UFS_DMA_BYTES];
} ALIGNED_DMA;

typedef struct {
  uint32_t Registers[0x2400U / sizeof(uint32_t)];
  uint8_t *Dma;
  uint8_t Cdbs[8][16];
  unsigned Reads;
  unsigned Writes;
  unsigned Commands;
  unsigned CompletedCommands;
  unsigned StallCalls;
  uint64_t LastLogicalBlock;
  uint32_t BlockSize;
  uint8_t ScsiStatus;
  int CompleteOnDelay;
  int Rc10Needs16;
  int MalformedResponse;
  int InterruptFault;
  int RefuseStop;
} FAKE_HARDWARE;

typedef struct {
  FAKE_HARDWARE Fake;
  T70_UFS_HOST Host;
  ALIGNED_DMA Dma;
} FIXTURE;

static unsigned Failures;

static void
PutBe32(uint8_t *Data, uint32_t Value)
{
  Data[0] = (uint8_t)(Value >> 24);
  Data[1] = (uint8_t)(Value >> 16);
  Data[2] = (uint8_t)(Value >> 8);
  Data[3] = (uint8_t)Value;
}

static void
PutBe64(uint8_t *Data, uint64_t Value)
{
  PutBe32(Data, (uint32_t)(Value >> 32));
  PutBe32(Data + 4, (uint32_t)Value);
}

static uint32_t
FakeRead32(void *Context, uint32_t Offset)
{
  FAKE_HARDWARE *Fake = (FAKE_HARDWARE *)Context;
  if ((Offset & 3U) || (Offset >= sizeof(Fake->Registers))) {
    return UINT32_MAX;
  }
  Fake->Reads++;
  return Fake->Registers[Offset / sizeof(uint32_t)];
}

static void
FakeWrite32(void *Context, uint32_t Offset, uint32_t Value)
{
  FAKE_HARDWARE *Fake = (FAKE_HARDWARE *)Context;
  if ((Offset & 3U) || (Offset >= sizeof(Fake->Registers))) {
    return;
  }
  Fake->Writes++;
  if (Offset == 0x20U) {
    Fake->Registers[Offset / sizeof(uint32_t)] &= ~Value;
  } else {
    Fake->Registers[Offset / sizeof(uint32_t)] = Value;
  }

  if ((Offset == 0x34U) && (Value == 0U) && !Fake->RefuseStop) {
    Fake->Registers[0x58U / sizeof(uint32_t)] = 0U;
    Fake->Registers[0x78U / sizeof(uint32_t)] = 0U;
  }
  if ((Offset == 0x58U) && (Value == 1U)) {
    if (Fake->Commands < 8U) {
      memcpy(Fake->Cdbs[Fake->Commands],
             Fake->Dma + MTK_UFS_COMMAND_OFFSET + 16U, 16U);
    }
    Fake->Commands++;
  }
}

static void
FakeFence(void *Context)
{
  (void)Context;
}

static void
FakeComplete(FAKE_HARDWARE *Fake)
{
  const uint8_t *Command = Fake->Dma + MTK_UFS_COMMAND_OFFSET;
  const uint8_t *Cdb = Command + 16U;
  uint8_t *Response = Fake->Dma + MTK_UFS_RESPONSE_OFFSET;
  uint8_t *Data = Fake->Dma + MTK_UFS_DATA_OFFSET;
  int IsNop = (Command[0] == 0U);

  memset(Response, 0, 64U);
  Response[0] = IsNop ? 0x20U : 0x21U;
  Response[2] = IsNop ? 0U : MTK_UFS_USER_LUN;
  Response[3] = Command[3];
  Response[7] = Fake->ScsiStatus;
  Fake->Dma[8] = 0U;

  if (!IsNop) {
    switch (Cdb[0]) {
      case 0x25U: /* READ CAPACITY(10) */
        PutBe32(Data, Fake->Rc10Needs16 ? UINT32_MAX :
                (uint32_t)Fake->LastLogicalBlock);
        PutBe32(Data + 4, Fake->BlockSize);
        break;
      case 0x9EU: /* READ CAPACITY(16) */
        PutBe64(Data, Fake->LastLogicalBlock);
        PutBe32(Data + 8, Fake->BlockSize);
        break;
      case 0x88U: /* READ(16) */
        for (unsigned Index = 0; Index < MTK_UFS_BLOCK_BYTES; ++Index) {
          Data[Index] = (uint8_t)(0xA5U ^ (uint8_t)Index);
        }
        break;
      default:
        break;
    }
  }

  if (Fake->MalformedResponse) {
    Response[0] = 0xFFU;
    Fake->MalformedResponse = 0;
  }
  if (Fake->InterruptFault) {
    Fake->Registers[0x20U / sizeof(uint32_t)] |= 0x2U;
    Fake->InterruptFault = 0;
  }
  Fake->Registers[0x58U / sizeof(uint32_t)] = 0U;
}

static void
FakeStallUs(void *Context, uint32_t Microseconds)
{
  FAKE_HARDWARE *Fake = (FAKE_HARDWARE *)Context;
  CHECK(Microseconds == T70_UFS_POLL_INTERVAL_US);
  Fake->StallCalls++;
  if (Fake->CompleteOnDelay &&
      (Fake->CompletedCommands < Fake->Commands)) {
    FakeComplete(Fake);
    Fake->CompletedCommands = Fake->Commands;
  }
}

static void
FixtureOpen(FIXTURE *Fixture)
{
  MTK_UFS_TRANSPORT_IO Io;

  memset(Fixture, 0, sizeof(*Fixture));
  Fixture->Fake.Dma = Fixture->Dma.Bytes;
  Fixture->Fake.CompleteOnDelay = 1;
  Fixture->Fake.LastLogicalBlock =
    (MTK_UFS_USER_BYTES / MTK_UFS_BLOCK_BYTES) - 1U;
  Fixture->Fake.BlockSize = MTK_UFS_BLOCK_BYTES;
  Fixture->Fake.Registers[0x00U / 4U] = (1U << 24);
  Fixture->Fake.Registers[0x08U / 4U] = 0x200U;
  Fixture->Fake.Registers[0x30U / 4U] = 0x0FU;
  Fixture->Fake.Registers[0x34U / 4U] = 1U;
  T70UfsHostInitialize(&Fixture->Host);

  Io.Context = &Fixture->Fake;
  Io.Read32 = FakeRead32;
  Io.Write32 = FakeWrite32;
  Io.Fence = FakeFence;
  Io.StallUs = FakeStallUs;
  (void)Io;
}

static T70_UFS_HANDOFF_SNAPSHOT
ValidSnapshot(void)
{
  T70_UFS_HANDOFF_SNAPSHOT Snapshot;
  memset(&Snapshot, 0, sizeof(Snapshot));
  Snapshot.HandoffAddress = T70_UFS_HANDOFF_BASE;
  Snapshot.Magic = T70_UFS_HANDOFF_MAGIC;
  Snapshot.FdtAddress = 0x44000000U;
  Snapshot.FdtSize = 0x1000U;
  Snapshot.FdtStatus = 0U;
  Snapshot.FdtNodeStatus = T70UfsFdtNodeEnabled;
  Snapshot.HandoffReady = 1U;
  Snapshot.HandoffMmioBase = T70_UFS_CONTROLLER_BASE;
  Snapshot.HandoffLun = MTK_UFS_USER_LUN;
  Snapshot.HandoffFormat = T70_UFS_HANDOFF_FORMAT;
  Snapshot.FdtControllerBase = T70_UFS_CONTROLLER_BASE;
  Snapshot.FdtControllerSize = T70_UFS_CONTROLLER_SIZE;
  return Snapshot;
}

static T70_UFS_HOST_STATUS
StartFixture(
  FIXTURE *Fixture,
  const T70_UFS_HANDOFF_SNAPSHOT *Snapshot,
  uint64_t MappedMmioBase
  )
{
  MTK_UFS_TRANSPORT_IO Io;
  Io.Context = &Fixture->Fake;
  Io.Read32 = FakeRead32;
  Io.Write32 = FakeWrite32;
  Io.Fence = FakeFence;
  Io.StallUs = FakeStallUs;
  return T70UfsHostStart(
    &Fixture->Host, Snapshot, MappedMmioBase, &Io,
    Fixture->Dma.Bytes, sizeof(Fixture->Dma.Bytes), 0x100000U);
}

static void
TestSuccessfulStartAndRead(void)
{
  FIXTURE Fixture;
  T70_UFS_HANDOFF_SNAPSHOT Snapshot = ValidSnapshot();
  uint8_t ReadBuffer[MTK_UFS_BLOCK_BYTES] = { 0 };
  const uint64_t ExpectedLast =
    (MTK_UFS_USER_BYTES / MTK_UFS_BLOCK_BYTES) - 1U;

  FixtureOpen(&Fixture);
  Fixture.Fake.Rc10Needs16 = 1;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostSuccess);
  CHECK(Fixture.Host.Ready == 1U);
  CHECK(Fixture.Host.LastLogicalBlock == ExpectedLast);
  CHECK(Fixture.Host.BlockSize == MTK_UFS_BLOCK_BYTES);
  CHECK(Fixture.Fake.Commands == 3U);
  CHECK(Fixture.Fake.Cdbs[1][0] == 0x25U);
  CHECK(Fixture.Fake.Cdbs[2][0] == 0x9EU);
  CHECK(Fixture.Fake.Cdbs[2][1] == 0x10U);
  CHECK(Fixture.Fake.Cdbs[2][13] == 32U);

  CHECK(T70UfsHostReadBlocks(
          &Fixture.Host, 17U, 1U, ReadBuffer, sizeof(ReadBuffer)) ==
        T70UfsReadSuccess);
  CHECK(Fixture.Fake.Commands == 4U);
  CHECK(ReadBuffer[0] == 0xA5U && ReadBuffer[1] == 0xA4U);
  CHECK(T70UfsHostStop(&Fixture.Host) == T70UfsHostSuccess);
  CHECK(Fixture.Host.StopSucceeded == 1U);
  CHECK(Fixture.Host.DmaQuarantined == 0U);
  CHECK(Fixture.Host.Ready == 0U);
}

static void
TestInvalidProofBeforeMmio(void)
{
  FIXTURE Fixture;
  T70_UFS_HANDOFF_SNAPSHOT Snapshot = ValidSnapshot();

  FixtureOpen(&Fixture);
  Snapshot.Magic ^= 1U;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostInvalidHandoff);
  Snapshot = ValidSnapshot();
  Snapshot.FdtAddress |= 1U;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostInvalidHandoff);
  Snapshot = ValidSnapshot();
  Snapshot.FdtSize = UINT64_MAX;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostInvalidHandoff);
  Snapshot = ValidSnapshot();
  Snapshot.FdtNodeStatus = T70UfsFdtNodeDisabled;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostInvalidHandoff);
  Snapshot = ValidSnapshot();
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE + 0x1000U) ==
        T70UfsHostInvalidHandoff);
  CHECK(Fixture.Fake.Reads == 0U);
  CHECK(Fixture.Fake.Writes == 0U);
  CHECK(Fixture.Fake.Commands == 0U);
  CHECK(Fixture.Host.DmaQuarantined == 0U);
}

static void
TestWrongCapacityStopsSafely(void)
{
  FIXTURE Fixture;
  T70_UFS_HANDOFF_SNAPSHOT Snapshot = ValidSnapshot();

  FixtureOpen(&Fixture);
  Fixture.Fake.LastLogicalBlock = 99U;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostGeometryMismatch);
  CHECK(Fixture.Fake.Commands == 2U);
  CHECK(Fixture.Host.Ready == 0U);
  CHECK(Fixture.Host.StopSucceeded == 1U);
  CHECK(Fixture.Host.DmaQuarantined == 0U);
  CHECK(!Fixture.Host.Transport.Attached);
}

static void
TestCapacityProtocolFault(void)
{
  FIXTURE Fixture;
  T70_UFS_HANDOFF_SNAPSHOT Snapshot = ValidSnapshot();

  FixtureOpen(&Fixture);
  Fixture.Fake.MalformedResponse = 1;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostNopFailed);
  CHECK(Fixture.Fake.Commands == 1U);
  CHECK(Fixture.Host.Ready == 0U);
  CHECK(Fixture.Host.StopSucceeded == 1U);
  CHECK(Fixture.Host.DmaQuarantined == 0U);
}

static void
TestNopTimeoutThenSafeStop(void)
{
  FIXTURE Fixture;
  T70_UFS_HANDOFF_SNAPSHOT Snapshot = ValidSnapshot();

  FixtureOpen(&Fixture);
  Fixture.Fake.CompleteOnDelay = 0;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostNopFailed);
  CHECK(Fixture.Fake.Commands == 1U);
  CHECK(Fixture.Fake.StallCalls >= T70_UFS_MAX_POLLS - 1U);
  CHECK(Fixture.Host.Ready == 0U);
  CHECK(Fixture.Host.StopSucceeded == 1U);
  CHECK(Fixture.Host.DmaQuarantined == 0U);
  CHECK(!Fixture.Host.Transport.Attached);
}

static void
TestStopFailureQuarantinesDma(void)
{
  static FIXTURE Fixture;
  T70_UFS_HANDOFF_SNAPSHOT Snapshot = ValidSnapshot();
  uint8_t ReadBuffer[MTK_UFS_BLOCK_BYTES];

  FixtureOpen(&Fixture);
  Fixture.Fake.CompleteOnDelay = 0;
  Fixture.Fake.RefuseStop = 1;
  CHECK(StartFixture(&Fixture, &Snapshot, T70_UFS_CONTROLLER_BASE) ==
        T70UfsHostDmaOwnershipUncertain);
  CHECK(Fixture.Host.Ready == 0U);
  CHECK(Fixture.Host.StopSucceeded == 0U);
  CHECK(Fixture.Host.DmaQuarantined == 1U);
  CHECK(Fixture.Host.Transport.Attached == 1U);
  CHECK(T70UfsHostReadBlocks(
          &Fixture.Host, 0U, 1U, ReadBuffer, sizeof(ReadBuffer)) ==
        T70UfsReadDmaOwnershipUncertain);

  /* A later real stop proof releases the quarantine. */
  Fixture.Fake.RefuseStop = 0;
  Fixture.Fake.Registers[0x58U / 4U] = 0U;
  CHECK(T70UfsHostStop(&Fixture.Host) == T70UfsHostSuccess);
  CHECK(Fixture.Host.StopSucceeded == 1U);
  CHECK(Fixture.Host.DmaQuarantined == 0U);
  CHECK(!Fixture.Host.Transport.Attached);
}

int
main(void)
{
  TestSuccessfulStartAndRead();
  TestInvalidProofBeforeMmio();
  TestWrongCapacityStopsSafely();
  TestCapacityProtocolFault();
  TestNopTimeoutThenSafeStop();
  TestStopFailureQuarantinesDma();

  if (Failures != 0U) {
    fprintf(stderr, "%u UFS host checks failed\n", Failures);
    return 1;
  }
  printf("T70UfsHostTest: host start, read, proof, capacity, timeout and stop ownership checks passed\n");
  return 0;
}
