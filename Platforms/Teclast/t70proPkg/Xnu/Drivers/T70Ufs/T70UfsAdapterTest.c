/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define _POSIX_C_SOURCE 200112L
#include "T70UfsReadOnly.h"
#include <Library/MtkUfsCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(Expression) do { \
  if (!(Expression)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #Expression); \
    Failures++; \
  } \
} while (0)

typedef struct {
  uint32_t Registers[0x2400U / sizeof(uint32_t)];
  uint8_t *Dma;
  unsigned Reads;
  unsigned Commands;
  unsigned DelayCalls;
  int CompleteOnDelay;
  int MalformedResponse;
  int InterruptFault;
  uint8_t ScsiStatus;
} FAKE_HARDWARE;

static unsigned Failures;

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
  if (Offset == 0x20U) {
    Fake->Registers[Offset / sizeof(uint32_t)] &= ~Value;
  } else {
    Fake->Registers[Offset / sizeof(uint32_t)] = Value;
  }
  if ((Offset == 0x34U) && (Value == 0U)) {
    /* The fake HCI retires the command doorbell when host enable is cleared. */
    Fake->Registers[0x58U / sizeof(uint32_t)] = 0U;
    Fake->Registers[0x78U / sizeof(uint32_t)] = 0U;
  }
  if ((Offset == 0x58U) && (Value == 1U)) {
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
  uint8_t *Response = Fake->Dma + MTK_UFS_RESPONSE_OFFSET;
  unsigned Index;

  memset(Response, 0, 64U);
  Response[0] = MTK_UFS_UPIU_RESPONSE_TYPE;
  Response[2] = MTK_UFS_USER_LUN;
  Response[3] = Command[3];
  Response[7] = Fake->ScsiStatus;
  if (Fake->MalformedResponse) {
    Response[0] = 0xFFU;
  }
  if (Fake->InterruptFault) {
    Fake->Registers[0x20U / sizeof(uint32_t)] |= 0x2U;
  }
  Fake->Dma[8] = 0U;
  for (Index = 0; Index < MTK_UFS_BLOCK_BYTES; ++Index) {
    Fake->Dma[MTK_UFS_DATA_OFFSET + Index] =
      (uint8_t)(0xA5U ^ (uint8_t)Index);
  }
  Fake->Registers[0x58U / sizeof(uint32_t)] = 0U;
}

static void
FakeStallUs(void *Context, uint32_t Microseconds)
{
  FAKE_HARDWARE *Fake = (FAKE_HARDWARE *)Context;
  CHECK(Microseconds == T70_UFS_POLL_INTERVAL_US);
  Fake->DelayCalls++;
  if (Fake->CompleteOnDelay && Fake->DelayCalls == 1U) {
    FakeComplete(Fake);
  }
}

typedef struct {
  FAKE_HARDWARE Fake;
  MTK_UFS_TRANSPORT Transport;
} FIXTURE;

static int
FixtureOpen(FIXTURE *Fixture, int CompleteOnDelay, uint8_t ScsiStatus)
{
  MTK_UFS_TRANSPORT_IO Io;
  void *Dma = NULL;
  memset(Fixture, 0, sizeof(*Fixture));
  if (posix_memalign(&Dma, 64U, MTK_UFS_DMA_BYTES) != 0) {
    return 0;
  }
  memset(Dma, 0, MTK_UFS_DMA_BYTES);
  Fixture->Fake.Dma = (uint8_t *)Dma;
  Fixture->Fake.CompleteOnDelay = CompleteOnDelay;
  Fixture->Fake.ScsiStatus = ScsiStatus;
  Fixture->Fake.Registers[0x00U / 4U] = (1U << 24);
  Fixture->Fake.Registers[0x08U / 4U] = 0x200U;
  Fixture->Fake.Registers[0x30U / 4U] = 0x0FU;
  Fixture->Fake.Registers[0x34U / 4U] = 1U;
  Io.Context = &Fixture->Fake;
  Io.Read32 = FakeRead32;
  Io.Write32 = FakeWrite32;
  Io.Fence = FakeFence;
  Io.StallUs = FakeStallUs;
  if (MtkUfsTransportAttach(&Fixture->Transport, &Io, Dma,
        MTK_UFS_DMA_BYTES, 0x100000U) != MtkUfsIoSuccess) {
    free(Dma);
    Fixture->Fake.Dma = NULL;
    return 0;
  }
  Fixture->Fake.Reads = 0;
  return 1;
}

static void
FixtureClose(FIXTURE *Fixture)
{
  if (Fixture->Transport.Attached) {
    MTK_UFS_IO_STATUS StopStatus = MtkUfsTransportStop(&Fixture->Transport);
    CHECK(StopStatus == MtkUfsIoSuccess);
    CHECK(!Fixture->Transport.Attached && !Fixture->Transport.Active);
    if ((StopStatus != MtkUfsIoSuccess) || Fixture->Transport.Attached) {
      return;
    }
  }
  free(Fixture->Fake.Dma);
  Fixture->Fake.Dma = NULL;
}

int
main(void)
{
  const uint64_t Last = (MTK_UFS_USER_BYTES / MTK_UFS_BLOCK_BYTES) - 1U;
  uint8_t ReadBuffer[MTK_UFS_BLOCK_BYTES] = { 0 };
  uint32_t TransferBytes = 1U;
  FIXTURE Fixture;
  T70_UFS_READ_STATUS Status;
  unsigned ReadsBefore;
  unsigned CommandsBefore;
  unsigned Index;
  uint64_t Lba = 0x01020304U;

  CHECK(T70UfsValidateRead(Last, MTK_UFS_BLOCK_BYTES, Lba, 1U,
        ReadBuffer, sizeof(ReadBuffer), &TransferBytes) == T70UfsReadSuccess);
  CHECK(TransferBytes == MTK_UFS_BLOCK_BYTES);

  CHECK(T70UfsValidateRead(Last, MTK_UFS_BLOCK_BYTES, Lba, 0U,
        ReadBuffer, sizeof(ReadBuffer), &TransferBytes) == T70UfsReadInvalidArgument);
  CHECK(TransferBytes == 0U);
  CHECK(T70UfsValidateRead(Last, MTK_UFS_BLOCK_BYTES, Last, 2U,
        ReadBuffer, sizeof(ReadBuffer), &TransferBytes) == T70UfsReadOutOfRange);
  CHECK(T70UfsValidateRead(Last, 2048U, Lba, 1U, ReadBuffer,
        sizeof(ReadBuffer), &TransferBytes) == T70UfsReadGeometryMismatch);
  CHECK(T70UfsValidateRead(UINT64_MAX, MTK_UFS_BLOCK_BYTES, 0U, 1U,
        ReadBuffer, sizeof(ReadBuffer), &TransferBytes) == T70UfsReadGeometryMismatch);
  CHECK(T70UfsValidateRead(Last, MTK_UFS_BLOCK_BYTES, 0U, 1U,
        ReadBuffer, MTK_UFS_BLOCK_BYTES - 1U, &TransferBytes) == T70UfsReadBufferTooSmall);
  CHECK(T70UfsValidateRead(Last, MTK_UFS_BLOCK_BYTES, 0U,
        (MTK_UFS_DATA_BYTES / MTK_UFS_BLOCK_BYTES) + 1U,
        ReadBuffer, sizeof(ReadBuffer), &TransferBytes) == T70UfsReadTooLarge);
  CHECK(!MtkUfsTransportReadCdb((const uint8_t[]){0x2AU}, 1U, 0U));

  CHECK(FixtureOpen(&Fixture, 1, 0U));
  memset(ReadBuffer, 0, sizeof(ReadBuffer));
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, Lba, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadSuccess);
  CHECK(Fixture.Fake.Commands == 1U);
  CHECK(Fixture.Fake.DelayCalls == 1U);
  CHECK(ReadBuffer[0] == 0xA5U && ReadBuffer[1] == 0xA4U);
  {
    const uint8_t *Cdb = Fixture.Fake.Dma + MTK_UFS_COMMAND_OFFSET + 16U;
    CHECK(Cdb[0] == 0x88U);
    CHECK(Cdb[2] == 0x00U && Cdb[3] == 0x00U &&
          Cdb[4] == 0x00U && Cdb[5] == 0x00U);
    CHECK(Cdb[6] == 0x01U && Cdb[7] == 0x02U &&
          Cdb[8] == 0x03U && Cdb[9] == 0x04U);
    CHECK(Cdb[10] == 0x00U && Cdb[11] == 0x00U &&
          Cdb[12] == 0x00U && Cdb[13] == 0x01U);
  }
  FixtureClose(&Fixture);

  CHECK(FixtureOpen(&Fixture, 1, 2U));
  memset(ReadBuffer, 0x5AU, sizeof(ReadBuffer));
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadDeviceError);
  CHECK(ReadBuffer[0] == 0x5AU);
  FixtureClose(&Fixture);

  CHECK(FixtureOpen(&Fixture, 1, 0U));
  Fixture.Fake.MalformedResponse = 1;
  memset(ReadBuffer, 0x6BU, sizeof(ReadBuffer));
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadDmaOwnershipUncertain);
  CHECK(Fixture.Transport.Active && Fixture.Transport.Faulted);
  CHECK(Fixture.Fake.Commands == 1U);
  for (Index = 0; Index < sizeof(ReadBuffer); ++Index) {
    CHECK(ReadBuffer[Index] == 0x6BU);
  }
  ReadsBefore = Fixture.Fake.Reads;
  CommandsBefore = Fixture.Fake.Commands;
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadNotReady);
  CHECK(Fixture.Fake.Reads == ReadsBefore);
  CHECK(Fixture.Fake.Commands == CommandsBefore);
  for (Index = 0; Index < sizeof(ReadBuffer); ++Index) {
    CHECK(ReadBuffer[Index] == 0x6BU);
  }
  FixtureClose(&Fixture);

  CHECK(FixtureOpen(&Fixture, 1, 0U));
  Fixture.Fake.InterruptFault = 1;
  memset(ReadBuffer, 0x3CU, sizeof(ReadBuffer));
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadDmaOwnershipUncertain);
  CHECK(Fixture.Transport.Active && Fixture.Transport.Faulted);
  CHECK(Fixture.Fake.Commands == 1U);
  for (Index = 0; Index < sizeof(ReadBuffer); ++Index) {
    CHECK(ReadBuffer[Index] == 0x3CU);
  }
  ReadsBefore = Fixture.Fake.Reads;
  CommandsBefore = Fixture.Fake.Commands;
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadNotReady);
  CHECK(Fixture.Fake.Reads == ReadsBefore);
  CHECK(Fixture.Fake.Commands == CommandsBefore);
  for (Index = 0; Index < sizeof(ReadBuffer); ++Index) {
    CHECK(ReadBuffer[Index] == 0x3CU);
  }
  FixtureClose(&Fixture);

  CHECK(FixtureOpen(&Fixture, 0, 0U));
  ReadsBefore = Fixture.Fake.Reads;
  CommandsBefore = Fixture.Fake.Commands;
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, Last, 2U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadOutOfRange);
  CHECK(Fixture.Fake.Reads == ReadsBefore);
  CHECK(Fixture.Fake.Commands == CommandsBefore);
  FixtureClose(&Fixture);

  CHECK(FixtureOpen(&Fixture, 0, 0U));
  ReadsBefore = Fixture.Fake.Reads;
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U,
      Fixture.Fake.Dma + MTK_UFS_DATA_OFFSET, MTK_UFS_BLOCK_BYTES);
  CHECK(Status == T70UfsReadInvalidArgument);
  CHECK(Fixture.Fake.Reads == ReadsBefore);
  CHECK(Fixture.Fake.Commands == 0U);
  FixtureClose(&Fixture);

  CHECK(FixtureOpen(&Fixture, 0, 0U));
  memset(ReadBuffer, 0x5AU, sizeof(ReadBuffer));
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadDmaOwnershipUncertain);
  CHECK(Fixture.Fake.DelayCalls == T70_UFS_MAX_POLLS - 1U);
  CHECK(Fixture.Fake.Commands == 1U);
  CHECK(Fixture.Transport.Faulted && Fixture.Transport.Active);
  CHECK(ReadBuffer[0] == 0x5AU);
  ReadsBefore = Fixture.Fake.Reads;
  Status = T70UfsReadBlocks(&Fixture.Transport, Last,
      MTK_UFS_BLOCK_BYTES, 0U, 1U, ReadBuffer, sizeof(ReadBuffer));
  CHECK(Status == T70UfsReadNotReady);
  CHECK(Fixture.Fake.Reads == ReadsBefore);
  FixtureClose(&Fixture);

  if (Failures != 0U) {
    fprintf(stderr, "%u check(s) failed\n", Failures);
    return 1;
  }
  printf("PASS: T70 UFS bounds, READ(16), malformed response and interrupt fault DMA quarantine\n");
  return 0;
}
