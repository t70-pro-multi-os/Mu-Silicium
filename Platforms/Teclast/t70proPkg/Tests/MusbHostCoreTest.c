// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/MusbHostCore.h>

#define TEST_REG_BYTES 0x500U
#define REG_INTRUSB 0x00AU
#define REG_INDEX 0x00EU
#define REG_DEVCTL 0x060U
#define REG_POWER 0x001U
#define REG_EP_BASE 0x010U
#define REG_FIFO_BASE 0x020U
#define REG_RXCSR 0x006U
#define REG_TXCSR 0x002U
#define REG_RXCOUNT 0x008U
#define CSR0_RXPKTRDY 0x0001U
#define CSR0_TXPKTRDY 0x0002U
#define CSR0_H_REQPKT 0x0020U
#define CSR0_H_STATUSPKT 0x0040U
#define CSR0_H_SETUPPKT 0x0008U
#define CSR0_FLUSHFIFO 0x0100U
#define TXCSR_TXPKTRDY 0x0001U
#define TXCSR_FIFONOTEMPTY 0x0002U
#define TXCSR_FLUSHFIFO 0x0008U
#define TXCSR_H_ERROR 0x0004U
#define TXCSR_H_RXSTALL 0x0020U
#define TXCSR_H_NAKTIMEOUT 0x0080U
#define RXCSR_RXPKTRDY 0x0001U
#define RXCSR_DATAERROR 0x0008U
#define RXCSR_FLUSHFIFO 0x0010U
#define RXCSR_H_REQPKT 0x0020U
#define RXCSR_H_RXSTALL 0x0040U

#define CHECK(Condition) do { \
  ++Checks; \
  if (!(Condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #Condition); \
    abort(); \
  } \
} while (0)

static unsigned Checks;

typedef struct {
  uint8_t Registers[TEST_REG_BYTES];
  uint8_t Indexed[16][16];
  uint8_t Index;
  uint8_t TxFifo[4096];
  uint32_t TxFifoLength;
  uint16_t TxPacketLengths[8];
  uint8_t TxPacketCount;
  uint16_t CurrentTxPacketLength;
  uint8_t RxFifo[1024];
  uint32_t RxFifoLength;
  uint32_t RxFifoPosition;
  uint8_t ControlResponse[64];
  uint8_t ControlResponseLength;
  uint8_t RxPackets[8][512];
  uint16_t RxPacketLengths[8];
  uint8_t RxPacketToggle[8];
  uint8_t RxPacketCount;
  uint8_t RxPacketIndex;
  uint8_t RxHardwareToggle;
  uint8_t RxNakRemaining;
  uint8_t InjectRxStall;
  uint8_t TxStall;
  uint8_t TxNakOnce;
  uint8_t TxNakAlways;
  uint8_t HoldTxReady;
  uint8_t DisconnectOnTx;
  uint8_t ReconnectOnTx;
  uint8_t FailEnabled;
  uint32_t FailOffset;
  uint32_t ReadCount;
  uint32_t WriteCount;
  uint32_t FifoRead8Count;
  uint32_t FifoRead16Count;
  uint32_t FifoRead32Count;
  uint32_t FifoWrite8Count;
  uint32_t FifoWrite16Count;
  uint32_t FifoWrite32Count;
  uint32_t ToggleWriteCount;
  uint32_t DevCtlWriteCount;
  uint32_t TxFlushWriteCount;
  uint32_t RxFlushWriteCount;
  uint32_t RxFlushAtCancel;
  uint8_t CancelCaptured;
  uint32_t Ep0FlushWriteCount;
  uint32_t Ep0ZeroWriteCount;
  uint32_t Ep0StatusWriteCount;
  uint32_t Ep0StatusInWriteCount;
  uint32_t Ep0StatusOutWriteCount;
  uint16_t Ep0StatusCommands[16];
  uint32_t Ep0StatusCommandCount;
  uint32_t Ep0SetupWriteCount;
  uint32_t CleanupFailureObserved;
  uint32_t CleanupFailureCountdown;
  uint32_t LockAttemptCount;
  uint32_t DelayCount;
  uint32_t CancelChecks;
  uint32_t CancelAfterDelay;
  uint32_t CancelAfterTxFifoWords;
  uint32_t CancelAfterRxFifoWords;
  uint64_t TimeUs;
  uint8_t CancelImmediately;
  uint8_t CancelOnControlSetup;
  uint8_t CancelOnControlStatus;
  uint8_t CancelOnTxComplete;
  uint8_t CancelOnRxPacket;
  uint8_t ArmCancelOnRxPacket;
  uint8_t TxPacketCompleted;
  uint8_t FailCleanupOnCancel;
  uint8_t CleanupFailureArmed;
  uint8_t ControlSetupStarted;
  uint8_t ControlStatusStarted;
  uint8_t LockHeld;
  uint8_t ForceBusy;
  uint8_t ResetWriteFault;
  uint8_t LoseVbusDuringReset;
  uint32_t ResetPowerWrites;
  uint8_t DropPowerBits;
  uint8_t UsbEventOnDelay;
  uint8_t DevCtlOnDelay;
  uint32_t EventDelayUs;
} FAKE_MUSB;

static int
IsIndexedOffset(uint32_t Offset)
{
  return (Offset >= REG_EP_BASE) && (Offset < REG_EP_BASE + 0x10U);
}

static int
IsFifoOffset(uint32_t Offset)
{
  return (Offset == REG_FIFO_BASE) || (Offset == REG_FIFO_BASE + 4U);
}

static uint32_t
FifoEndpoint(uint32_t Offset)
{
  return (Offset - REG_FIFO_BASE) / 4U;
}

static uint8_t
FakeReadFifoByte(FAKE_MUSB *Bus)
{
  if (Bus->RxFifoPosition < Bus->RxFifoLength) {
    return Bus->RxFifo[Bus->RxFifoPosition++];
  }
  return 0U;
}

static int
FakeRead8(void *Context, uint32_t Offset, uint8_t *Value)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;

  CHECK(Bus != NULL && Value != NULL);
  ++Bus->ReadCount;
  if (Bus->FailEnabled && (Offset == Bus->FailOffset)) {
    return 0;
  }
  if (Offset >= TEST_REG_BYTES) {
    return 0;
  }
  if (Offset == REG_INDEX) {
    *Value = Bus->Index;
  } else if (Offset == REG_INTRUSB) {
    *Value = Bus->Registers[Offset];
  } else if (IsIndexedOffset(Offset)) {
    *Value = Bus->Indexed[Bus->Index][Offset - REG_EP_BASE];
  } else if (IsFifoOffset(Offset)) {
    ++Bus->FifoRead8Count;
    *Value = FakeReadFifoByte(Bus);
  } else {
    *Value = Bus->Registers[Offset];
  }
  return 1;
}

static int
FakeRead16(void *Context, uint32_t Offset, uint16_t *Value)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;

  CHECK(Bus != NULL && Value != NULL);
  ++Bus->ReadCount;
  if (Bus->FailEnabled && (Offset == Bus->FailOffset)) {
    if (Bus->CleanupFailureArmed &&
        (Bus->CleanupFailureCountdown != 0U)) {
      --Bus->CleanupFailureCountdown;
      if (Bus->CleanupFailureCountdown != 0U) {
        // Let the cancellation completion probe read status; fail cleanup.
      } else {
        ++Bus->CleanupFailureObserved;
        return 0;
      }
    } else {
      if (Bus->CleanupFailureArmed) {
        ++Bus->CleanupFailureObserved;
      }
      return 0;
    }
  }
  if (Offset + 2U > TEST_REG_BYTES) {
    return 0;
  }
  if (IsIndexedOffset(Offset)) {
    *Value = (uint16_t)Bus->Indexed[Bus->Index][Offset - REG_EP_BASE] |
             (uint16_t)((uint16_t)Bus->Indexed[Bus->Index][Offset + 1U - REG_EP_BASE] << 8);
  } else if (IsFifoOffset(Offset)) {
    uint8_t Low;
    uint8_t High;

    ++Bus->FifoRead16Count;
    Low = FakeReadFifoByte(Bus);
    High = FakeReadFifoByte(Bus);
    *Value = (uint16_t)Low | (uint16_t)((uint16_t)High << 8);
  } else {
    *Value = (uint16_t)Bus->Registers[Offset] |
             (uint16_t)((uint16_t)Bus->Registers[Offset + 1U] << 8);
  }
  return 1;
}

static int
FakeRead32(void *Context, uint32_t Offset, uint32_t *Value)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;

  CHECK(Bus != NULL && Value != NULL);
  ++Bus->ReadCount;
  if (Bus->FailEnabled && (Offset == Bus->FailOffset)) {
    return 0;
  }
  if (Offset + 4U > TEST_REG_BYTES) {
    return 0;
  }
  if (IsFifoOffset(Offset)) {
    uint8_t Byte0;
    uint8_t Byte1;
    uint8_t Byte2;
    uint8_t Byte3;

    ++Bus->FifoRead32Count;
    Byte0 = FakeReadFifoByte(Bus);
    Byte1 = FakeReadFifoByte(Bus);
    Byte2 = FakeReadFifoByte(Bus);
    Byte3 = FakeReadFifoByte(Bus);
    *Value = (uint32_t)Byte0 | ((uint32_t)Byte1 << 8) |
             ((uint32_t)Byte2 << 16) | ((uint32_t)Byte3 << 24);
  } else if (Offset == 0x080U) {
    *Value = Bus->RxHardwareToggle ? (1U << 1) : 0U;
  } else {
    *Value = (uint32_t)Bus->Registers[Offset] |
             ((uint32_t)Bus->Registers[Offset + 1U] << 8) |
             ((uint32_t)Bus->Registers[Offset + 2U] << 16) |
             ((uint32_t)Bus->Registers[Offset + 3U] << 24);
  }
  return 1;
}

static int
FakeWriteFifo(FAKE_MUSB *Bus, uint32_t Offset, const uint8_t *Bytes,
              uint32_t Length, uint32_t Width)
{
  uint32_t Index;

  CHECK(FifoEndpoint(Offset) < 16U);
  CHECK(Bus->TxFifoLength + Length <= sizeof(Bus->TxFifo));
  if (Width == 1U) {
    ++Bus->FifoWrite8Count;
  } else if (Width == 2U) {
    ++Bus->FifoWrite16Count;
  } else {
    ++Bus->FifoWrite32Count;
  }
  for (Index = 0U; Index < Length; ++Index) {
    Bus->TxFifo[Bus->TxFifoLength++] = Bytes[Index];
  }
  if (FifoEndpoint(Offset) == 1U) {
    Bus->CurrentTxPacketLength = (uint16_t)(Bus->CurrentTxPacketLength + Length);
  }
  if (FifoEndpoint(Offset) == 1U) {
    Bus->Indexed[1][REG_TXCSR] |= TXCSR_FIFONOTEMPTY;
  }
  return 1;
}

static void
FakeIssueRx(FAKE_MUSB *Bus)
{
  uint8_t *Csr;

  Csr = &Bus->Indexed[1][REG_RXCSR];
  if (Bus->InjectRxStall) {
    Bus->InjectRxStall = 0U;
    *Csr = RXCSR_H_RXSTALL;
    return;
  }
  if (Bus->RxNakRemaining != 0U) {
    --Bus->RxNakRemaining;
    *Csr = RXCSR_DATAERROR;
    return;
  }
  if (Bus->RxPacketIndex < Bus->RxPacketCount) {
    uint8_t Packet;

    Packet = Bus->RxPacketIndex++;
    Bus->RxPacketToggle[Packet] = Bus->RxHardwareToggle;
    Bus->RxHardwareToggle ^= 1U;
    if (Bus->ArmCancelOnRxPacket) {
      Bus->CancelOnRxPacket = 1U;
    }
    Bus->RxFifoLength = Bus->RxPacketLengths[Packet];
    Bus->RxFifoPosition = 0U;
    memcpy(Bus->RxFifo, Bus->RxPackets[Packet], Bus->RxFifoLength);
    Bus->Indexed[1][REG_RXCOUNT] = (uint8_t)Bus->RxFifoLength;
    Bus->Indexed[1][REG_RXCOUNT + 1U] =
      (uint8_t)(Bus->RxFifoLength >> 8);
    *Csr = RXCSR_RXPKTRDY;
  } else {
    *Csr = RXCSR_DATAERROR;
  }
}

static int
FakeWrite8(void *Context, uint32_t Offset, uint8_t Value)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;

  CHECK(Bus != NULL);
  ++Bus->WriteCount;
  if (Offset == REG_DEVCTL) {
    ++Bus->DevCtlWriteCount;
  }
  if (Bus->FailEnabled && (Offset == Bus->FailOffset)) {
    return 0;
  }
  if (Offset >= TEST_REG_BYTES) {
    return 0;
  }
  if (Offset == REG_POWER) {
    ++Bus->ResetPowerWrites;
    if ((Bus->ResetWriteFault == 1U && (Value & 0x08U) != 0U) ||
        (Bus->ResetWriteFault == 2U && (Value & 0x08U) == 0U)) return 1;
    if (Bus->LoseVbusDuringReset && (Value & 0x08U) == 0U)
      Bus->Registers[REG_DEVCTL] &= (uint8_t)~0x18U;
    Value &= (uint8_t)~Bus->DropPowerBits;
  }
  if (Offset == REG_INDEX) {
    Bus->Index = Value;
  } else if (IsFifoOffset(Offset)) {
    return FakeWriteFifo(Bus, Offset, &Value, 1U, 1U);
  } else if (Offset == REG_INTRUSB) {
    Bus->Registers[Offset] &= (uint8_t)~Value;
  } else if (IsIndexedOffset(Offset)) {
    Bus->Indexed[Bus->Index][Offset - REG_EP_BASE] = Value;
  } else {
    Bus->Registers[Offset] = Value;
  }
  return 1;
}

static int
FakeWrite16(void *Context, uint32_t Offset, uint16_t Value)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;
  uint8_t Bytes[2];

  CHECK(Bus != NULL);
  ++Bus->WriteCount;
  if (Bus->FailEnabled && (Offset == Bus->FailOffset)) {
    return 0;
  }
  if (Offset + 2U > TEST_REG_BYTES) {
    return 0;
  }
  Bytes[0] = (uint8_t)Value;
  Bytes[1] = (uint8_t)(Value >> 8);
  if (IsFifoOffset(Offset)) {
    return FakeWriteFifo(Bus, Offset, Bytes, sizeof(Bytes), 2U);
  }
  if (IsIndexedOffset(Offset)) {
    if ((Bus->Index == 0U) && (Offset == REG_EP_BASE + REG_TXCSR)) {
      uint8_t *Csr0;

      Csr0 = &Bus->Indexed[0][REG_TXCSR];
      if (Value == 0U) {
        ++Bus->Ep0ZeroWriteCount;
      }
      if ((Value & CSR0_H_SETUPPKT) != 0U) {
        Bus->ControlSetupStarted = 1U;
        ++Bus->Ep0SetupWriteCount;
      }
      if ((Value & CSR0_H_STATUSPKT) != 0U) {
        Bus->ControlStatusStarted = 1U;
        if (Bus->Ep0StatusCommandCount <
            (sizeof(Bus->Ep0StatusCommands) /
             sizeof(Bus->Ep0StatusCommands[0]))) {
          Bus->Ep0StatusCommands[Bus->Ep0StatusCommandCount] = Value;
        }
        ++Bus->Ep0StatusCommandCount;
        ++Bus->Ep0StatusWriteCount;
        if ((Value & CSR0_H_REQPKT) != 0U) {
          ++Bus->Ep0StatusInWriteCount;
        }
        if ((Value & CSR0_TXPKTRDY) != 0U) {
          ++Bus->Ep0StatusOutWriteCount;
        }
      }
      if ((Value & CSR0_FLUSHFIFO) != 0U) {
        ++Bus->Ep0FlushWriteCount;
        *Csr0 = 0U;
        Bus->RxFifoLength = 0U;
        Bus->RxFifoPosition = 0U;
      } else if ((Value & CSR0_H_REQPKT) != 0U) {
        if ((Value & CSR0_H_STATUSPKT) != 0U) {
          *Csr0 = 0U;
          Bus->Indexed[0][REG_RXCOUNT] = 0U;
        } else {
          *Csr0 = CSR0_RXPKTRDY;
          Bus->Indexed[0][REG_RXCOUNT] = Bus->ControlResponseLength;
          Bus->RxFifoLength = Bus->ControlResponseLength;
          Bus->RxFifoPosition = 0U;
          memcpy(Bus->RxFifo, Bus->ControlResponse,
                 Bus->ControlResponseLength);
        }
      } else if ((Value & CSR0_TXPKTRDY) != 0U) {
        // The controller completes TX while retaining the status selector.
        *Csr0 = (uint8_t)(Value & CSR0_H_STATUSPKT);
      } else {
        *Csr0 = 0U;
      }
    } else if ((Bus->Index == 1U) &&
               (Offset == REG_EP_BASE + REG_TXCSR)) {
      uint8_t *Csr;

      Csr = &Bus->Indexed[1][REG_TXCSR];
      if ((Value & TXCSR_FLUSHFIFO) != 0U) {
        ++Bus->TxFlushWriteCount;
        *Csr = 0U;
      } else if ((Value & TXCSR_TXPKTRDY) != 0U) {
        if (Bus->TxStall) {
          Bus->TxStall = 0U;
          *Csr = 0x20U;
        } else if (Bus->DisconnectOnTx) {
          Bus->DisconnectOnTx = 0U;
          Bus->Registers[REG_DEVCTL] = 0U;
          *Csr = 0U;
        } else if (Bus->ReconnectOnTx) {
          Bus->ReconnectOnTx = 0U;
          Bus->Registers[REG_INTRUSB] |= 0x10U;
          *Csr = 0U;
        } else if (Bus->TxNakAlways) {
          *Csr = TXCSR_H_NAKTIMEOUT | TXCSR_FIFONOTEMPTY;
        } else if (Bus->TxNakOnce) {
          Bus->TxNakOnce = 0U;
          *Csr = TXCSR_H_NAKTIMEOUT | TXCSR_FIFONOTEMPTY;
        } else if ((*Csr & TXCSR_H_NAKTIMEOUT) != 0U) {
          // The timeout survives a write of one. All other WZC bits must be preserved.
          CHECK((Value & (TXCSR_FIFONOTEMPTY | TXCSR_H_ERROR | TXCSR_H_RXSTALL)) ==
                (TXCSR_FIFONOTEMPTY | TXCSR_H_ERROR | TXCSR_H_RXSTALL));
          if ((Value & TXCSR_H_NAKTIMEOUT) == 0U) {
            *Csr = 0U;
            if (Bus->TxPacketCount < sizeof(Bus->TxPacketLengths) / sizeof(Bus->TxPacketLengths[0])) {
              Bus->TxPacketLengths[Bus->TxPacketCount] = Bus->CurrentTxPacketLength;
            }
            ++Bus->TxPacketCount;
            Bus->CurrentTxPacketLength = 0U;
            Bus->TxPacketCompleted = 1U;
          }
        } else if (Bus->HoldTxReady) {
          *Csr = TXCSR_TXPKTRDY;
        } else {
          *Csr = 0U;
          if (Bus->TxPacketCount < sizeof(Bus->TxPacketLengths) / sizeof(Bus->TxPacketLengths[0])) {
            Bus->TxPacketLengths[Bus->TxPacketCount] = Bus->CurrentTxPacketLength;
          }
          ++Bus->TxPacketCount;
          Bus->CurrentTxPacketLength = 0U;
          Bus->TxPacketCompleted = 1U;
        }
      } else {
        *Csr = (uint8_t)(Value & 0xFFU);
      }
    } else if ((Bus->Index == 1U) &&
               (Offset == REG_EP_BASE + REG_RXCSR)) {
      uint8_t *Csr;

      Csr = &Bus->Indexed[1][REG_RXCSR];
      if ((Value & RXCSR_FLUSHFIFO) != 0U) {
        ++Bus->RxFlushWriteCount;
        *Csr = 0U;
        Bus->RxFifoLength = 0U;
        Bus->RxFifoPosition = 0U;
      } else if ((Value & RXCSR_H_REQPKT) != 0U) {
        FakeIssueRx(Bus);
      } else {
        *Csr = (uint8_t)(Value & 0xFFU);
      }
    } else {
      uint8_t Index;

      Index = (uint8_t)(Offset - REG_EP_BASE);
      Bus->Indexed[Bus->Index][Index] = Bytes[0];
      Bus->Indexed[Bus->Index][Index + 1U] = Bytes[1];
    }
    return 1;
  }

  Bus->Registers[Offset] = Bytes[0];
  Bus->Registers[Offset + 1U] = Bytes[1];
  return 1;
}

static int
FakeWrite32(void *Context, uint32_t Offset, uint32_t Value)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;
  uint8_t Bytes[4];
  uint32_t Index;

  CHECK(Bus != NULL);
  ++Bus->WriteCount;
  if (Bus->FailEnabled && (Offset == Bus->FailOffset)) {
    return 0;
  }
  if (Offset + 4U > TEST_REG_BYTES) {
    return 0;
  }
  if ((Offset == 0x080U) || (Offset == 0x084U)) {
    ++Bus->ToggleWriteCount;
  }
  if (Offset == 0x080U) {
    Bus->RxHardwareToggle = (uint8_t)((Value & (1U << 1)) != 0U);
    return 1;
  }
  Bytes[0] = (uint8_t)Value;
  Bytes[1] = (uint8_t)(Value >> 8);
  Bytes[2] = (uint8_t)(Value >> 16);
  Bytes[3] = (uint8_t)(Value >> 24);
  if (IsFifoOffset(Offset)) {
    return FakeWriteFifo(Bus, Offset, Bytes, sizeof(Bytes), 4U);
  }
  for (Index = 0U; Index < sizeof(Bytes); ++Index) {
    Bus->Registers[Offset + Index] = Bytes[Index];
  }
  return 1;
}

static uint64_t
FakeNowUs(void *Context)
{
  return ((FAKE_MUSB *)Context)->TimeUs;
}

static void
FakeDelayUs(void *Context, uint32_t Microseconds)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;

  Bus->TimeUs += Microseconds;
  ++Bus->DelayCount;
  if (Microseconds == Bus->EventDelayUs) {
    Bus->Registers[REG_DEVCTL] = Bus->DevCtlOnDelay;
    Bus->Registers[REG_INTRUSB] |= Bus->UsbEventOnDelay;
    Bus->EventDelayUs = 0U;
  }
}

static int
FakeCancelled(void *Context)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;
  int Cancelled;

  ++Bus->CancelChecks;
  Cancelled = Bus->CancelImmediately ||
              ((Bus->CancelAfterDelay != 0U) &&
               (Bus->DelayCount >= Bus->CancelAfterDelay)) ||
              ((Bus->CancelAfterTxFifoWords != 0U) &&
               (Bus->FifoWrite32Count >= Bus->CancelAfterTxFifoWords)) ||
              ((Bus->CancelAfterRxFifoWords != 0U) &&
               (Bus->FifoRead32Count >= Bus->CancelAfterRxFifoWords)) ||
              Bus->CancelOnRxPacket ||
              (Bus->CancelOnControlSetup && Bus->ControlSetupStarted) ||
              (Bus->CancelOnControlStatus && Bus->ControlStatusStarted) ||
              (Bus->CancelOnTxComplete && Bus->TxPacketCompleted);
  if (Cancelled && !Bus->CancelCaptured) {
    Bus->CancelCaptured = 1U;
    Bus->RxFlushAtCancel = Bus->RxFlushWriteCount;
  }
  if (Cancelled && Bus->FailCleanupOnCancel &&
      !Bus->CleanupFailureArmed) {
    Bus->CleanupFailureArmed = 1U;
    Bus->CleanupFailureCountdown = 2U;
    Bus->FailEnabled = 1U;
    Bus->FailOffset = REG_EP_BASE + REG_TXCSR;
  }
  return Cancelled;
}

static int
FakeTryLock(void *Context)
{
  FAKE_MUSB *Bus = (FAKE_MUSB *)Context;

  ++Bus->LockAttemptCount;
  if (Bus->ForceBusy || Bus->LockHeld) {
    return 0;
  }
  Bus->LockHeld = 1U;
  return 1;
}

static void
FakeUnlock(void *Context)
{
  ((FAKE_MUSB *)Context)->LockHeld = 0U;
}

static void
InitFake(FAKE_MUSB *Bus)
{
  memset(Bus, 0, sizeof(*Bus));
  Bus->Registers[REG_DEVCTL] = 0x5DU;
  Bus->Registers[REG_INTRUSB] = 0x10U;
}

static void
GetConfig(MUSB_HOST_CONFIG *Config)
{
  memset(Config, 0, sizeof(*Config));
  Config->Revision = MUSB_HOST_CORE_REVISION;
  Config->Flags = MUSB_READY_REQUIRED;
  Config->ControllerLength = 0x10000U;
  Config->PhyLength = 0x10000U;
  Config->InterruptVector = MUSB_HOST_GSIV;
  Config->EndpointCount = MUSB_HOST_MAX_ENDPOINTS;
  Config->PortCount = MUSB_HOST_PORT_COUNT;
  Config->FifoProfile = MUSB_HOST_T70_FIFO_PROFILE;
  Config->InterruptMask = MUSB_HOST_CONNECT_DISCONNECT_MASK;
}

static void
InitHost(FAKE_MUSB *Bus, MUSB_HOST *Host)
{
  MUSB_HOST_IO Io;
  MUSB_HOST_CONFIG Config;

  memset(&Io, 0, sizeof(Io));
  Io.Context = Bus;
  Io.ControllerLength = 0x10000U;
  Io.Read8 = FakeRead8;
  Io.Read16 = FakeRead16;
  Io.Read32 = FakeRead32;
  Io.Write8 = FakeWrite8;
  Io.Write16 = FakeWrite16;
  Io.Write32 = FakeWrite32;
  Io.NowUs = FakeNowUs;
  Io.DelayUs = FakeDelayUs;
  Io.TryLock = FakeTryLock;
  Io.Unlock = FakeUnlock;
  GetConfig(&Config);
  CHECK(MusbHostInitialize(Host, &Io, &Config) == MusbOk);
}

static void
InitFakeHost(FAKE_MUSB *Bus, MUSB_HOST *Host)
{
  InitFake(Bus);
  InitHost(Bus, Host);
}

static void
EnableCancellation(MUSB_HOST *Host)
{
  Host->Io.Cancelled = FakeCancelled;
}

static void
SetControlResponse(FAKE_MUSB *Bus, const uint8_t *Data, uint8_t Length)
{
  CHECK(Length <= sizeof(Bus->ControlResponse));
  memcpy(Bus->ControlResponse, Data, Length);
  Bus->ControlResponseLength = Length;
}

static void
QueueRxPacket(FAKE_MUSB *Bus, const uint8_t *Data, uint16_t Length)
{
  uint8_t Index;

  CHECK(Bus->RxPacketCount < 8U);
  CHECK(Length <= sizeof(Bus->RxPackets[0]));
  Index = Bus->RxPacketCount++;
  memcpy(Bus->RxPackets[Index], Data, Length);
  Bus->RxPacketLengths[Index] = Length;
}

static void
TestReadinessAndLock(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  MUSB_HOST_IO Io;
  MUSB_HOST_CONFIG Config;
  uint32_t Reads;

  InitFake(&Bus);
  memset(&Io, 0, sizeof(Io));
  Io.Context = &Bus;
  Io.ControllerLength = 0x10000U;
  Io.Read8 = FakeRead8;
  Io.Read16 = FakeRead16;
  Io.Read32 = FakeRead32;
  Io.Write8 = FakeWrite8;
  Io.Write16 = FakeWrite16;
  Io.Write32 = FakeWrite32;
  Io.NowUs = FakeNowUs;
  Io.DelayUs = FakeDelayUs;
  Io.TryLock = FakeTryLock;
  Io.Unlock = FakeUnlock;
  GetConfig(&Config);
  Config.Flags = 0U;
  Reads = Bus.ReadCount;
  CHECK(MusbHostInitialize(&Host, &Io, &Config) == MusbNotReady);
  CHECK(Bus.ReadCount == Reads);

  GetConfig(&Config);
  Config.Flags = MUSB_READY_ID_LOW_REQUIRED;
  Bus.Registers[REG_DEVCTL] = 0x05U;  /* VBUS below SessionEnd. */
  CHECK(MusbHostInitialize(&Host, &Io, &Config) == MusbNotReady);
  Bus.Registers[REG_DEVCTL] = 0x0DU;  /* VBUS above SessionEnd only. */
  CHECK(MusbHostInitialize(&Host, &Io, &Config) == MusbNotReady);
  Bus.Registers[REG_DEVCTL] = 0x15U;  /* VBUS above A-valid, not VBUS-valid. */
  CHECK(MusbHostInitialize(&Host, &Io, &Config) == MusbNotReady);
  Bus.Registers[REG_DEVCTL] = 0x9DU;  /* B-device, even with valid VBUS. */
  CHECK(MusbHostInitialize(&Host, &Io, &Config) == MusbNotReady);
  Bus.Registers[REG_DEVCTL] = 0x3DU;  /* Valid host with LSDEV speed bit. */
  CHECK(MusbHostInitialize(&Host, &Io, &Config) == MusbOk);
  CHECK(Host.Config.Flags == MUSB_READY_ID_LOW_REQUIRED);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_POWER) == MusbOk);
  Config.Flags = (MUSB_READY_REQUIRED | MUSB_READY_ID_LOW_HUB);
  CHECK(MusbHostInitialize(&Host, &Io, &Config) == MusbNotReady);

  Bus.Registers[REG_DEVCTL]=0x1DU;
  GetConfig(&Config);
  InitHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  Bus.ForceBusy = 1U;
  Reads = Bus.ReadCount;
  {
    uint8_t Toggle = 0U;
    uint8_t Data[1] = {0U};
    uint32_t Bytes = 99U;

    CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 0x01U,
                               MUSB_SPEED_HIGH, 512U, 0U, Data,
                               sizeof(Data), &Toggle, 1U, &Bytes) == MusbBusy);
    CHECK(Bytes == 0U);
    CHECK(Bus.ReadCount == Reads);
  }
}

static void
TestEp0ZeroLengthStatusDirection(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t SetupOut[8] = {0U, 0x05U, 7U, 0U, 0U, 0U, 0U, 0U};
  const uint8_t SetupIn[8] = {0x80U, 0x06U, 0U, 1U, 0U, 0U, 0U, 0U};
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                SetupOut, NULL, 0U, 10U,
                                &Bytes) == MusbOk);
  CHECK(Bytes == 0U && Bus.Ep0StatusInWriteCount == 1U);
  CHECK(Bus.Ep0StatusOutWriteCount == 0U);
  CHECK(Bus.Ep0StatusCommandCount == 1U);
  CHECK(Bus.Ep0StatusCommands[0] == 0x0060U);
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                SetupIn, NULL, 0U, 10U,
                                &Bytes) == MusbOk);
  CHECK(Bytes == 0U);
  CHECK(Bus.Ep0StatusCommandCount == 2U);
  CHECK(Bus.Ep0StatusCommands[1] == 0x0060U);
  CHECK(Bus.Ep0StatusInWriteCount == 2U);
  CHECK(Bus.Ep0StatusOutWriteCount == 0U);
}

static void
TestControlInAndOutStatus(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t Response[] = {0x12U, 0x01U, 0x00U, 0x02U, 0x09U};
  uint8_t SetupIn[8] = {0x80U, 0x06U, 0U, 1U, 0U, 0U, 8U, 0U};
  uint8_t SetupOut[8] = {0U, 0x05U, 7U, 0U, 0U, 0U, 0U, 0U};
  uint8_t SetupOutData[8] = {0U, 0x09U, 1U, 0U, 0U, 0U, 1U, 0U};
  uint8_t SetupNoDataIn[8] = {0x80U, 0x06U, 0U, 1U, 0U, 0U, 0U, 0U};
  uint8_t Data[8];
  uint8_t OutData = 0x5AU;
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  SetControlResponse(&Bus, Response, sizeof(Response));
  memset(Data, 0xCC, sizeof(Data));
  Bytes = 0U;
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                SetupIn, Data, sizeof(Data), 10U,
                                &Bytes) == MusbOk);
  CHECK(Bytes == sizeof(Response));
  CHECK(memcmp(Data, Response, sizeof(Response)) == 0);
  CHECK(Bus.Ep0StatusOutWriteCount == 1U);
  CHECK(Bus.Ep0StatusInWriteCount == 0U);
  CHECK(Bus.Ep0StatusCommandCount == 1U);
  CHECK(Bus.Ep0StatusCommands[0] == 0x0042U);
  CHECK(Bus.Indexed[0][REG_TXCSR] == 0U);
  CHECK(Bus.DevCtlWriteCount == 0U);

  Bytes = 55U;
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                SetupOut, NULL, 0U, 10U,
                                &Bytes) == MusbOk);
  CHECK(Bytes == 0U);
  CHECK(Bus.TxFifoLength == 16U);
  CHECK(Bus.Ep0StatusInWriteCount == 1U);
  CHECK(Bus.Ep0StatusOutWriteCount == 1U);
  CHECK(Bus.Ep0StatusCommandCount == 2U);
  CHECK(Bus.Ep0StatusCommands[1] == 0x0060U);
  CHECK(Bus.Indexed[0][REG_TXCSR] == 0U);
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                SetupOutData, &OutData, sizeof(OutData), 10U,
                                &Bytes) == MusbOk);
  CHECK(Bytes == sizeof(OutData));
  CHECK(Bus.Ep0StatusInWriteCount == 2U);
  CHECK(Bus.Ep0StatusOutWriteCount == 1U);
  CHECK(Bus.Ep0StatusCommandCount == 3U);
  CHECK(Bus.Ep0StatusCommands[2] == 0x0060U);
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                SetupNoDataIn, NULL, 0U, 10U,
                                &Bytes) == MusbOk);
  CHECK(Bytes == 0U);
  CHECK(Bus.Ep0StatusInWriteCount == 3U);
  CHECK(Bus.Ep0StatusOutWriteCount == 1U);
  CHECK(Bus.Ep0StatusCommandCount == 4U);
  CHECK(Bus.Ep0StatusCommands[3] == 0x0060U);
  CHECK(Bus.DevCtlWriteCount == 0U);
}

static void
TestBulkOutWidthsToggleAndStall(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Data[519];
  uint8_t Toggle;
  uint32_t Bytes;
  uint32_t Index;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  for (Index = 0U; Index < sizeof(Data); ++Index) {
    Data[Index] = (uint8_t)Index;
  }
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 2U, 0x02U,
                             MUSB_SPEED_HIGH, 512U, 0U, Data,
                             sizeof(Data), &Toggle, 10U, &Bytes) == MusbOk);
  CHECK(Bytes == sizeof(Data));
  CHECK(Toggle == 0U);
  CHECK(Bus.TxFifoLength == sizeof(Data));
  CHECK(memcmp(Bus.TxFifo, Data, sizeof(Data)) == 0);
  CHECK(Bus.FifoWrite32Count == 129U);
  CHECK(Bus.FifoWrite16Count == 1U);
  CHECK(Bus.FifoWrite8Count == 1U);
  CHECK(Bus.ToggleWriteCount == 2U);
  CHECK(Bus.DevCtlWriteCount == 0U);

  Bus.TxFifoLength = 0U;
  Bus.TxNakOnce = 1U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 2U, 0x02U,
                             MUSB_SPEED_HIGH, 512U, 0U, Data,
                             64U, &Toggle, 10U, &Bytes) == MusbOk);
  CHECK(Bytes == 64U && Toggle == 1U && Bus.TxFifoLength == 64U);
  CHECK(!Bus.TxNakOnce && (Bus.Indexed[1][REG_TXCSR] & TXCSR_H_NAKTIMEOUT) == 0U);

  Bus.TxFifoLength = 0U;
  Bus.TxStall = 1U;
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 2U, 0x02U,
                             MUSB_SPEED_HIGH, 512U, 0U, Data,
                             64U, &Toggle, 10U, &Bytes) == MusbStall);
  CHECK(Bytes == 0U);
  CHECK(Bus.DevCtlWriteCount == 0U);
}

static void
TestBulkInNakShortAndOverflow(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Packet[16];
  uint8_t Data[12];
  uint8_t Toggle;
  uint32_t Bytes;
  unsigned Index;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  for (Index = 0U; Index < sizeof(Packet); ++Index) {
    Packet[Index] = (uint8_t)(0xA0U + Index);
  }
  QueueRxPacket(&Bus, Packet, 7U);
  Bus.RxNakRemaining = 1U;
  memset(Data, 0xCC, sizeof(Data));
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x83U,
                             MUSB_SPEED_FULL, 64U, 0U, Data,
                             sizeof(Data), &Toggle, 10U, &Bytes) == MusbOk);
  CHECK(Bytes == 7U);
  CHECK(Toggle == 1U);
  CHECK(memcmp(Data, Packet, 7U) == 0);
  CHECK(Data[7] == 0xCCU);

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  {
    uint8_t Guarded[10];
    uint8_t *Payload = &Guarded[1];

    memset(Guarded, 0xA5, sizeof(Guarded));
    Toggle = 0U;
    Bytes = 0U;
    CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x83U,
                               MUSB_SPEED_FULL, 16U, 0U, Payload,
                               8U, &Toggle, 10U, &Bytes) == MusbBufferOverflow);
    CHECK(Bytes == 0U);
    CHECK(Toggle == 1U);
    CHECK(Guarded[0] == 0xA5U && Guarded[9] == 0xA5U);
    CHECK(Bus.RxPacketToggle[0] == 0U);

    QueueRxPacket(&Bus, Packet, 8U);
    memset(Data, 0xCC, sizeof(Data));
    Bytes = 0U;
    CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x83U,
                               MUSB_SPEED_FULL, 16U, 0U, Data,
                               sizeof(Data), &Toggle, 10U, &Bytes) == MusbOk);
    CHECK(Bytes == 8U && Toggle == 0U);
    CHECK(Bus.RxPacketToggle[1] == 1U);
    CHECK(memcmp(Data, Packet, 8U) == 0);
  }
}

static void
TestInterruptMultiPacket(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t OutData[18];
  uint8_t InData[24];
  uint8_t Packet[8];
  uint8_t Toggle;
  uint32_t Bytes;

  for (unsigned I = 0U; I < sizeof(OutData); ++I) OutData[I] = (uint8_t)(0x30U + I);
  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_INTERRUPT, 3U, 0x01U,
          MUSB_SPEED_FULL, 8U, 1U, OutData, sizeof(OutData), &Toggle,
          10U, &Bytes) == MusbOk);
  CHECK(Bytes == sizeof(OutData) && Toggle == 1U);
  CHECK(Bus.TxFifoLength == sizeof(OutData) && !memcmp(Bus.TxFifo, OutData, sizeof(OutData)));
  CHECK(Bus.TxPacketCount == 3U && Bus.TxPacketLengths[0] == 8U &&
        Bus.TxPacketLengths[1] == 8U && Bus.TxPacketLengths[2] == 2U);
  CHECK(!Bus.LockHeld);

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  for (unsigned I = 0U; I < sizeof(Packet); ++I) Packet[I] = (uint8_t)(0x80U + I);
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  QueueRxPacket(&Bus, Packet, 4U);
  memset(InData, 0xCC, sizeof(InData));
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_INTERRUPT, 3U, 0x81U,
          MUSB_SPEED_FULL, 8U, 1U, InData, 20U, &Toggle, 10U, &Bytes) == MusbOk);
  CHECK(Bytes == 20U && Toggle == 1U && Bus.RxPacketIndex == 3U);
  CHECK(Bus.RxPacketToggle[0] == 0U && Bus.RxPacketToggle[1] == 1U &&
        Bus.RxPacketToggle[2] == 0U);
  CHECK(!memcmp(InData, Packet, 8U) && !memcmp(InData + 8U, Packet, 8U) &&
        !memcmp(InData + 16U, Packet, 4U) && InData[20] == 0xCCU);

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  QueueRxPacket(&Bus, Packet, 3U);
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_INTERRUPT, 3U, 0x81U,
          MUSB_SPEED_FULL, 8U, 1U, InData, sizeof(InData), &Toggle,
          10U, &Bytes) == MusbOk);
  CHECK(Bytes == 11U && Toggle == 0U && Bus.RxPacketIndex == 2U);
  CHECK(Bus.RxPacketToggle[0] == 0U && Bus.RxPacketToggle[1] == 1U);

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_INTERRUPT_POLL, 3U, 0x81U,
          MUSB_SPEED_FULL, 8U, 1U, InData, 16U, &Toggle, 10U, &Bytes) == MusbOk);
  CHECK(Bytes == 8U && Toggle == 1U && Bus.RxPacketIndex == 1U);
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_INTERRUPT_POLL, 3U, 0x81U,
          MUSB_SPEED_FULL, 8U, 1U, InData + 8U, 8U, &Toggle, 10U, &Bytes) == MusbOk);
  CHECK(Bytes == 8U && Toggle == 0U && Bus.RxPacketIndex == 2U);
  CHECK(!Bus.LockHeld);
}

static void
TestBulkOutZeroLengthAndInterruptTimeouts(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Data[8] = {0};
  uint8_t Toggle;
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  Toggle=0U;Bytes=99U;
  CHECK(MusbHostDataTransfer(&Host,MUSB_TRANSFER_BULK,2U,0x02U,
      MUSB_SPEED_HIGH,512U,0U,NULL,0U,&Toggle,10U,&Bytes)==MusbOk);
  CHECK(Bytes==0U && Toggle==1U && Bus.TxPacketCount==1U &&
        Bus.TxPacketLengths[0]==0U && !Bus.LockHeld);

  InitFakeHost(&Bus,&Host);
  CHECK(MusbHostSetState(&Host,MUSB_HOST_STATE_OPERATIONAL)==MusbOk);
  CHECK(MusbHostSetPortFeature(&Host,0U,MUSB_PORT_FEATURE_RESET)==MusbOk);
  Bus.TxNakAlways=1U;Toggle=0U;Bytes=99U;
  CHECK(MusbHostDataTransfer(&Host,MUSB_TRANSFER_INTERRUPT,3U,0x01U,
      MUSB_SPEED_FULL,8U,1U,Data,sizeof(Data),&Toggle,1U,&Bytes)==MusbTimeout);
  CHECK(Bytes==0U && Toggle==0U && Bus.TxPacketCount==0U &&
        !Bus.LockHeld);

  InitFakeHost(&Bus,&Host);
  CHECK(MusbHostSetState(&Host,MUSB_HOST_STATE_OPERATIONAL)==MusbOk);
  CHECK(MusbHostSetPortFeature(&Host,0U,MUSB_PORT_FEATURE_RESET)==MusbOk);
  Bus.RxNakRemaining=1U;Toggle=0U;Bytes=99U;
  CHECK(MusbHostDataTransfer(&Host,MUSB_TRANSFER_INTERRUPT_POLL,3U,0x81U,
      MUSB_SPEED_FULL,8U,1U,Data,sizeof(Data),&Toggle,10U,&Bytes)==MusbTimeout);
  CHECK(Bytes==0U && Toggle==0U && Bus.RxPacketIndex==0U &&
        Bus.RxNakRemaining==0U && !Bus.LockHeld);
}

static void
TestBulkInZeroLength(void)
{
  static const struct {
    MUSB_STATUS Status;
    uint8_t Received;
  } Cases[] = {
    {MusbOk, 1U}, {MusbBufferOverflow, 1U}, {MusbStall, 0U},
    {MusbTimeout, 0U}, {MusbCancelled, 1U}, {MusbDisconnected, 0U}
  };
  const uint8_t Packet = 0xA5U;
  unsigned Index;

  for (Index = 0U; Index < sizeof(Cases) / sizeof(Cases[0]); ++Index) {
    FAKE_MUSB Bus;
    MUSB_HOST Host;
    uint32_t Bytes = 99U;
    uint8_t Toggle = 0U;
    uint8_t Data = 0U;

    InitFakeHost(&Bus, &Host);
    CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    switch (Cases[Index].Status) {
      case MusbOk:
        Bus.RxNakRemaining = 1U;
        QueueRxPacket(&Bus, &Packet, 0U);
        QueueRxPacket(&Bus, &Packet, 1U);
        break;
      case MusbBufferOverflow:
        QueueRxPacket(&Bus, &Packet, 1U);
        break;
      case MusbStall:
        Bus.InjectRxStall = 1U;
        break;
      case MusbCancelled:
        EnableCancellation(&Host);
        QueueRxPacket(&Bus, &Packet, 0U);
        Bus.ArmCancelOnRxPacket = 1U;
        break;
      case MusbDisconnected:
        Bus.EventDelayUs = 50U;
        Bus.DevCtlOnDelay = 0x1dU;
        Bus.UsbEventOnDelay = 0x20U;
        break;
      default:
        break;
    }

    CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x83U,
                               MUSB_SPEED_FULL, 64U, 0U, NULL, 0U, &Toggle,
                               1U, &Bytes) == Cases[Index].Status);
    CHECK(Bytes == 0U && Toggle == Cases[Index].Received);
    CHECK(Bus.RxPacketIndex == Cases[Index].Received && Bus.RxFifoPosition == 0U);
    CHECK((Bus.Indexed[1][REG_RXCSR] & (RXCSR_RXPKTRDY | RXCSR_H_REQPKT)) == 0U);
    CHECK(!Bus.LockHeld);
    if (Cases[Index].Status == MusbOk) {
      CHECK(Bus.RxNakRemaining == 0U);
      CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x83U,
                                 MUSB_SPEED_FULL, 64U, 0U, &Data, sizeof(Data),
                                 &Toggle, 1U, &Bytes) == MusbOk);
      CHECK(Bytes == 1U && Data == Packet && Toggle == 0U);
      CHECK(Bus.RxPacketIndex == 2U && Bus.RxPacketToggle[1] == 1U);
    }
  }
}

static void
TestTimeoutDisconnectAndReset(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Data[32];
  uint8_t Toggle;
  uint32_t Bytes;
  uint16_t Port;
  uint16_t Change;
  uint8_t PowerBefore;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  Bus.HoldTxReady = 1U;
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 0x01U,
                             MUSB_SPEED_HIGH, 32U, 0U, Data,
                             sizeof(Data), &Toggle, 1U, &Bytes) == MusbTimeout);
  CHECK(Bytes == 0U);

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  Bus.DisconnectOnTx = 1U;
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 0x01U,
                             MUSB_SPEED_HIGH, 32U, 0U, Data,
                             sizeof(Data), &Toggle, 10U, &Bytes) == MusbDisconnected);
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK((Port & MUSB_PORT_CONNECTION) == 0U);
  CHECK((Change & MUSB_PORT_CHANGE_CONNECTION) != 0U);

  InitFakeHost(&Bus, &Host);
  Bus.Registers[REG_POWER] = 0x13U;
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  CHECK((Bus.Registers[REG_POWER] & 0x10U) != 0U);
  CHECK((Bus.Registers[REG_POWER] & 0x03U) == 0x01U);
  CHECK((Bus.Registers[REG_POWER] & 0x08U) == 0U);
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK((Port & (MUSB_PORT_CONNECTION | MUSB_PORT_ENABLE |
                 MUSB_PORT_HIGH_SPEED | MUSB_PORT_POWER)) ==
        (MUSB_PORT_CONNECTION | MUSB_PORT_ENABLE |
         MUSB_PORT_HIGH_SPEED | MUSB_PORT_POWER));
  CHECK((Change & MUSB_PORT_CHANGE_RESET) != 0U);
  CHECK(Bus.DevCtlWriteCount == 0U);

  PowerBefore = Bus.Registers[REG_POWER];
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_POWER) == MusbOk);
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_POWER) ==
        MusbUnsupported);
  CHECK(Bus.Registers[REG_POWER] == PowerBefore);
}


static void
TestResetRequiresHardwareCompletion(void)
{
  for (uint8_t Fault = 1U; Fault <= 4U; ++Fault) {
    FAKE_MUSB Bus;
    MUSB_HOST Host;
    uint16_t Port, Change;
    MUSB_STATUS Status;

    InitFakeHost(&Bus, &Host);
    Bus.Registers[REG_POWER] = 0x31U;
    CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    /* Repeat reset on an enabled port: a failed reset must revoke enable. */
    Host.PortStatus |= MUSB_PORT_ENABLE | MUSB_PORT_HIGH_SPEED;
    Host.PortChangeStatus = 0U;
    if (Fault <= 2U) Bus.ResetWriteFault = Fault;
    if (Fault == 3U) Bus.LoseVbusDuringReset = 1U;
    if (Fault == 4U) Bus.Registers[REG_DEVCTL] &= (uint8_t)~0x18U;
    Status = MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET);
    fprintf(stderr, "root reset fault=%u status=%u port=%04x change=%04x power=%02x\n",
            Fault, (unsigned)Status, Host.PortStatus, Host.PortChangeStatus,
            Bus.Registers[REG_POWER]);
    CHECK(Status == ((Fault <= 2U) ? MusbIoError : MusbNotReady));
    CHECK((Host.PortChangeStatus & MUSB_PORT_CHANGE_RESET) == 0U);
    CHECK((Host.PortStatus & MUSB_PORT_ENABLE) == 0U);
    if (Fault >= 3U) {
      CHECK(Host.PortStatus == 0U);
      CHECK((Host.PortChangeStatus & MUSB_PORT_CHANGE_CONNECTION) != 0U);
    }
    CHECK((Bus.Registers[REG_POWER] & 0x31U) == 0x31U);
    CHECK(!Bus.LockHeld);
    CHECK(Bus.DevCtlWriteCount == 0U);
    if (Fault == 4U) CHECK(Bus.ResetPowerWrites == 0U);
    else CHECK(Bus.TimeUs >= 50000U);
    /* Recovery can succeed after read-only status and the fault recover. */
    Bus.ResetWriteFault = Bus.LoseVbusDuringReset = 0U;
    if (Fault >= 3U) {
      CHECK(MusbHostClearPortFeature(&Host, 0U,
                                    MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
    }
    Bus.Registers[REG_DEVCTL] = 0x5dU;
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    CHECK((Host.PortStatus & MUSB_PORT_ENABLE) != 0U);
    CHECK((Host.PortChangeStatus & MUSB_PORT_CHANGE_RESET) != 0U);
    CHECK((Bus.Registers[REG_POWER] & 0x08U) == 0U);
    CHECK(!Bus.LockHeld);
  }
}

static uint16_t
TargetRegister(const FAKE_MUSB *Bus, uint32_t Offset)
{
  return (uint16_t)Bus->Registers[Offset] |
         (uint16_t)((uint16_t)Bus->Registers[Offset + 1U] << 8);
}

static void
TestHubRouting(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  MUSB_HOST_ROUTE Route;
  MUSB_HOST_ROUTE Direct = {0};
  const uint8_t Values[] = {1U, 127U};
  const uint8_t Setup[8] = {0U, 5U, 9U, 0U, 0U, 0U, 0U, 0U};
  uint8_t Data[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  uint8_t ReadData[8];
  uint8_t Toggle, Speed, Multi, H, P;
  uint16_t Hub, Function;
  uint32_t Bytes;

  for (Speed = MUSB_SPEED_FULL; Speed <= MUSB_SPEED_LOW; ++Speed) {
    for (Multi = 0U; Multi <= 1U; ++Multi) {
      for (H = 0U; H < sizeof(Values); ++H) {
        for (P = 0U; P < sizeof(Values); ++P) {
          InitFakeHost(&Bus, &Host);
          Bus.Registers[REG_POWER] = 0x10U;
          CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
          CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
          Route = (MUSB_HOST_ROUTE){Values[H], Values[P], Multi, 0U};
          Hub = (uint16_t)(Values[H] | (Multi ? 0x80U : 0U) |
                          ((uint16_t)Values[P] << 8));
          Function = (uint16_t)(9U | ((Speed == MUSB_SPEED_FULL) ? 0x100U : 0U));
          CHECK(MusbHostControlTransferRouted(&Host, 9U, Speed, 8U, Setup,
                  NULL, 0U, 10U, &Route, &Bytes) == MusbOk);
          CHECK(Bytes == 0U);
          CHECK(TargetRegister(&Bus, 0x480U) == Function);
          CHECK(TargetRegister(&Bus, 0x482U) == Hub);

          Toggle = 0U;
          CHECK(MusbHostDataTransferRouted(&Host,
                  Speed == MUSB_SPEED_LOW ? MUSB_TRANSFER_INTERRUPT : MUSB_TRANSFER_BULK,
                  9U, 0x01U, Speed, 8U, 1U, Data, sizeof(Data), &Toggle,
                  10U, &Route, &Bytes) == MusbOk);
          CHECK(Bytes == sizeof(Data) && Toggle == 1U);
          CHECK(TargetRegister(&Bus, 0x488U) == Function);
          CHECK(TargetRegister(&Bus, 0x48AU) == Hub);

          QueueRxPacket(&Bus, Data, sizeof(Data));
          Toggle = 0U;
          CHECK(MusbHostDataTransferRouted(&Host, MUSB_TRANSFER_INTERRUPT,
                  9U, 0x81U, Speed, 8U, 1U, ReadData, sizeof(ReadData), &Toggle,
                  10U, &Route, &Bytes) == MusbOk);
          CHECK(Bytes == sizeof(Data) && Toggle == 1U);
          CHECK(memcmp(ReadData, Data, sizeof(Data)) == 0);
          CHECK(TargetRegister(&Bus, 0x48CU) == 9U);
          CHECK(TargetRegister(&Bus, 0x48EU) == Hub);

          // A direct transfer must clear a previous hub route in each direction.
          CHECK(MusbHostControlTransferRouted(&Host, 9U, Speed, 8U, Setup,
                  NULL, 0U, 10U, &Direct, &Bytes) == MusbOk);
          CHECK(TargetRegister(&Bus, 0x482U) == 0U);
          Toggle = 0U;
          CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_INTERRUPT, 9U, 0x01U,
                  Speed, 8U, 1U, Data, sizeof(Data), &Toggle, 10U, &Bytes) == MusbOk);
          CHECK(TargetRegister(&Bus, 0x48AU) == 0U);
          QueueRxPacket(&Bus, Data, sizeof(Data));
          CHECK(MusbHostDataTransferRouted(&Host, MUSB_TRANSFER_INTERRUPT,
                  9U, 0x81U, Speed, 8U, 1U, ReadData, sizeof(ReadData), &Toggle,
                  10U, &Direct, &Bytes) == MusbOk);
          CHECK(TargetRegister(&Bus, 0x48EU) == 0U);
          CHECK(!Bus.LockHeld);
        }
      }
    }
  }
}

static void
TestBadHubRoutesDoNotSendPackets(void)
{
  const MUSB_HOST_ROUTE Invalid[] = {
    {0U, 1U, 0U, 0U}, {1U, 0U, 0U, 0U}, {128U, 1U, 0U, 0U},
    {1U, 128U, 0U, 0U}, {1U, 1U, 2U, 0U}, {1U, 1U, 0U, 1U},
    {0U, 0U, 1U, 0U}
  };
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const MUSB_HOST_ROUTE Valid = {1U, 1U, 0U, 0U};
  const uint8_t Setup[8] = {0U, 5U, 1U, 0U, 0U, 0U, 0U, 0U};
  uint8_t Data = 1U, Toggle;
  uint32_t Bytes, Writes, Reads;
  size_t I;

  InitFakeHost(&Bus, &Host);
  Bus.Registers[REG_POWER] = 0x10U;
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  for (I = 0U; I < sizeof(Invalid) / sizeof(Invalid[0]); ++I) {
    Writes = Bus.WriteCount; Reads = Bus.ReadCount; Toggle = 0U; Bytes = 99U;
    CHECK(MusbHostControlTransferRouted(&Host, 1U, MUSB_SPEED_FULL, 8U,
            Setup, NULL, 0U, 10U, &Invalid[I], &Bytes) == MusbInvalid);
    CHECK(Bytes == 0U);
    CHECK(MusbHostDataTransferRouted(&Host, MUSB_TRANSFER_BULK, 1U, 1U,
            MUSB_SPEED_FULL, 8U, 0U, &Data, 1U, &Toggle, 10U, &Invalid[I],
            &Bytes) == MusbInvalid);
    CHECK(Bytes == 0U && Toggle == 0U);
    CHECK(Bus.WriteCount == Writes && Bus.ReadCount == Reads);
    CHECK(!Bus.LockHeld);
  }
  Writes = Bus.WriteCount;
  CHECK(MusbHostControlTransferRouted(&Host, 1U, MUSB_SPEED_HIGH, 64U,
          Setup, NULL, 0U, 10U, &Valid, &Bytes) == MusbInvalid);
  CHECK(Bus.WriteCount == Writes && Bytes == 0U);
  Bus.Registers[REG_POWER] = 0U;
  CHECK(MusbHostControlTransferRouted(&Host, 1U, MUSB_SPEED_FULL, 8U,
          Setup, NULL, 0U, 10U, &Valid, &Bytes) == MusbNotReady);
  CHECK(Bus.WriteCount == Writes && Bytes == 0U);

  Bus.Registers[REG_POWER] = 0x10U;
  Bus.FailEnabled = 1U;
  Bus.FailOffset = 0x482U;
  CHECK(MusbHostControlTransferRouted(&Host, 1U, MUSB_SPEED_FULL, 8U,
          Setup, NULL, 0U, 10U, &Valid, &Bytes) == MusbIoError);
  CHECK(Bus.TxFifoLength == 0U && Bytes == 0U && !Bus.LockHeld);
  Bus.FailOffset = 0x48EU;
  Toggle = 0U;
  CHECK(MusbHostDataTransferRouted(&Host, MUSB_TRANSFER_INTERRUPT, 1U, 0x81U,
          MUSB_SPEED_FULL, 8U, 1U, &Data, 1U, &Toggle, 10U, &Valid,
          &Bytes) == MusbIoError);
  CHECK(Bytes == 0U && Toggle == 0U && !Bus.LockHeld);
}

static void
TestCancellationBeforeHardwareAccess(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t Setup[8] = {0U, 5U, 1U, 0U, 0U, 0U, 0U, 0U};
  uint8_t Data[8] = {0U};
  uint8_t Toggle;
  uint32_t Bytes;
  uint32_t Reads;
  uint32_t Writes;
  uint32_t LockAttempts;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  EnableCancellation(&Host);
  Bus.CancelImmediately = 1U;
  Reads = Bus.ReadCount;
  Writes = Bus.WriteCount;
  LockAttempts = Bus.LockAttemptCount;
  Toggle = 0U;
  Bytes = 99U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 0x01U,
                             MUSB_SPEED_HIGH, 64U, 0U, Data, sizeof(Data),
                             &Toggle, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U && Toggle == 0U);
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                Setup, NULL, 0U, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U);
  CHECK(Bus.ReadCount == Reads && Bus.WriteCount == Writes);
  CHECK(Bus.LockAttemptCount == LockAttempts && !Bus.LockHeld);
}

static void
TestCancellationDuringOutputFifoKeepsCompletedPacket(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Data[64];
  uint8_t Toggle;
  uint32_t Bytes;
  uint32_t Index;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  for (Index = 0U; Index < sizeof(Data); ++Index) {
    Data[Index] = (uint8_t)(0x40U + Index);
  }
  EnableCancellation(&Host);
  Bus.CancelAfterTxFifoWords = 9U;
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 2U, 0x01U,
                             MUSB_SPEED_HIGH, 32U, 0U, Data, sizeof(Data),
                             &Toggle, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 32U && Toggle == 1U);
  CHECK(Bus.FifoWrite32Count == 9U);
  CHECK(Bus.TxFifoLength == 36U);
  CHECK(Bus.TxFlushWriteCount >= 1U);
  CHECK(!Bus.LockHeld);
}

static void
TestCancellationAfterCompletedOutputPacketAccountsToggle(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Data[32];
  uint8_t Toggle;
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  memset(Data, 0x5A, sizeof(Data));
  EnableCancellation(&Host);
  Bus.CancelOnTxComplete = 1U;
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 2U, 0x01U,
                             MUSB_SPEED_HIGH, 32U, 0U, Data, sizeof(Data),
                             &Toggle, 10U, &Bytes) == MusbCancelled);
  CHECK(Bus.TxPacketCompleted);
  CHECK(Bytes == sizeof(Data) && Toggle == 1U);
  CHECK(!Bus.LockHeld);
}

static void
TestCancellationDuringInputFifoDoesNotCountPartialPacket(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t Packet[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  uint8_t Data[8];
  uint8_t Toggle;
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  EnableCancellation(&Host);
  Bus.CancelAfterRxFifoWords = 1U;
  memset(Data, 0xCC, sizeof(Data));
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x81U,
                             MUSB_SPEED_FULL, 8U, 0U, Data, sizeof(Data),
                             &Toggle, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U && Toggle == 1U);
  CHECK(Bus.FifoRead32Count == 1U);
  CHECK(memcmp(Data, Packet, 4U) == 0 && Data[4] == 0xCCU);
  CHECK(Bus.CancelCaptured && Bus.RxFlushWriteCount - Bus.RxFlushAtCancel == 2U);
  CHECK(!Bus.LockHeld);
}

static void
TestCancellationDuringControlSetupAndStatus(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t Setup[8] = {0U, 5U, 7U, 0U, 0U, 0U, 0U, 0U};
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  EnableCancellation(&Host);
  Bus.CancelOnControlSetup = 1U;
  Bytes = 99U;
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                Setup, NULL, 0U, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U && Bus.Ep0SetupWriteCount == 1U);
  CHECK(Bus.Ep0ZeroWriteCount != 0U && !Bus.LockHeld);

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  EnableCancellation(&Host);
  Bus.CancelOnControlStatus = 1U;
  Bytes = 99U;
  CHECK(MusbHostControlTransfer(&Host, 1U, MUSB_SPEED_FULL, 8U,
                                Setup, NULL, 0U, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U && Bus.Ep0SetupWriteCount == 1U);
  CHECK(Bus.Ep0StatusWriteCount == 1U);
  CHECK(Bus.Ep0ZeroWriteCount != 0U && !Bus.LockHeld);
}

static void
TestCancellationAfterReceiveBeforePacketRead(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t Packet[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  uint8_t Data[8];
  uint8_t Toggle;
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  QueueRxPacket(&Bus, Packet, sizeof(Packet));
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  EnableCancellation(&Host);
  Bus.ArmCancelOnRxPacket = 1U;
  memset(Data, 0xCC, sizeof(Data));
  Toggle = 0U;
  Bytes = 0U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x81U,
                             MUSB_SPEED_FULL, 8U, 0U, Data, sizeof(Data),
                             &Toggle, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U && Toggle == 1U);
  CHECK(Bus.RxPacketToggle[0] == 0U && Bus.RxHardwareToggle == 1U);
  CHECK(Bus.FifoRead32Count == 0U);
  CHECK(Bus.RxFlushWriteCount >= 2U && !Bus.LockHeld);
}

static void
TestCancellationDuringNakRetry(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Data[8];
  uint8_t Toggle;
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  EnableCancellation(&Host);
  Bus.RxNakRemaining = 3U;
  Bus.DelayCount = 0U;
  Bus.CancelAfterDelay = 1U;
  Toggle = 0U;
  Bytes = 99U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 3U, 0x81U,
                             MUSB_SPEED_FULL, 8U, 0U, Data, sizeof(Data),
                             &Toggle, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U && Toggle == 0U);
  CHECK(Bus.DelayCount >= 1U && Bus.RxNakRemaining == 1U);
  CHECK(Bus.CancelCaptured && Bus.RxFlushWriteCount - Bus.RxFlushAtCancel == 2U && !Bus.LockHeld);
}

static void
TestCancellationSurvivesOutputCleanupFailure(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint8_t Data[64];
  uint8_t Toggle;
  uint32_t Bytes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  memset(Data, 0xA5, sizeof(Data));
  EnableCancellation(&Host);
  Bus.HoldTxReady = 1U;
  Bus.DelayCount = 0U;
  Bus.CancelAfterDelay = 1U;
  Bus.FailCleanupOnCancel = 1U;
  Toggle = 0U;
  Bytes = 99U;
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 0x01U,
                             MUSB_SPEED_HIGH, 64U, 0U, Data, sizeof(Data),
                             &Toggle, 10U, &Bytes) == MusbCancelled);
  CHECK(Bytes == 0U && Toggle == 0U);
  CHECK(Bus.CleanupFailureArmed && Bus.CleanupFailureObserved == 1U);
  CHECK(Bus.CleanupFailureCountdown == 0U);
  CHECK(Bus.FailOffset == REG_EP_BASE + REG_TXCSR);
  CHECK(!Bus.LockHeld);
}


static void
TestVbusLossPreservesDisconnect(void)
{
  for (unsigned Halted = 0U; Halted <= 1U; ++Halted) {
    FAKE_MUSB Bus;
    MUSB_HOST Host;
    uint16_t Port, Change;
    uint32_t Writes;
    const uint8_t Setup[8] = {0U, 5U, 1U, 0U, 0U, 0U, 0U, 0U};
    uint32_t Bytes = 1U;

    InitFakeHost(&Bus, &Host);
    CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_ENABLE) == MusbOk);
    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_RESET) == MusbOk);
    CHECK(MusbHostMarkVbusLost(&Host) == MusbBusy);
    CHECK((Host.PortStatus & MUSB_PORT_ENABLE) != 0U);

    Writes = Bus.WriteCount;
    Bus.Registers[REG_DEVCTL] = 0x45U;
    if (Halted) {
      CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_HALTED) == MusbOk);
      CHECK(MusbHostMarkVbusLost(&Host) == MusbOk);
      CHECK(MusbHostMarkVbusLost(&Host) == MusbOk);
    } else {
      CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    }
    CHECK(Host.PortStatus == 0U && Host.PortPowerLost == 1U);
    CHECK(Host.PortChangeStatus ==
          (MUSB_PORT_CHANGE_CONNECTION | MUSB_PORT_CHANGE_ENABLE));
    CHECK(Bus.WriteCount == Writes && !Bus.LockHeld);

    Bus.Registers[REG_DEVCTL] = 0x5DU;
    if (Halted) CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    CHECK(Port == MUSB_PORT_POWER &&
          Change == (MUSB_PORT_CHANGE_CONNECTION | MUSB_PORT_CHANGE_ENABLE));
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbDisconnected);
    CHECK(MusbHostControlTransfer(&Host, 0U, MUSB_SPEED_FULL, 8U, Setup, NULL,
                                  0U, 10U, &Bytes) == MusbDisconnected);
    CHECK(Bytes == 0U && Bus.WriteCount == Writes);

    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    CHECK(Port == (MUSB_PORT_POWER | MUSB_PORT_CONNECTION));
    CHECK((Change & MUSB_PORT_CHANGE_CONNECTION) != 0U);
    CHECK(MusbHostControlTransfer(&Host, 0U, MUSB_SPEED_FULL, 8U, Setup, NULL,
                                  0U, 10U, &Bytes) == MusbDisconnected);
    CHECK(Bytes == 0U && Bus.WriteCount == Writes);
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    CHECK((Host.PortStatus & MUSB_PORT_ENABLE) != 0U && !Host.PortPowerLost);
    CHECK(!Bus.LockHeld && Bus.DevCtlWriteCount == 0U);
  }
}

static void TestAdoptPortState(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint16_t Status,Change;
  InitFakeHost(&Bus,&Host);
  Bus.Registers[REG_INTRUSB]=0;
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER|MUSB_PORT_CONNECTION|
                              MUSB_PORT_ENABLE|MUSB_PORT_HIGH_SPEED,0)==MusbOk);
  CHECK(MusbHostGetPortStatus(&Host,0,&Status,&Change)==MusbOk);
  CHECK(Status==(MUSB_PORT_POWER|MUSB_PORT_CONNECTION|MUSB_PORT_ENABLE|MUSB_PORT_HIGH_SPEED));
  CHECK(Change==MUSB_PORT_CHANGE_CONNECTION);
  CHECK(!Bus.LockHeld);
  CHECK(MusbHostSetState(&Host,MUSB_HOST_STATE_OPERATIONAL)==MusbOk);
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER,0)==MusbBusy);
  CHECK(MusbHostSetState(&Host,MUSB_HOST_STATE_HALTED)==MusbOk);
  CHECK(MusbHostAdoptPortState(&Host,0,0)==MusbInvalid);
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER|MUSB_PORT_ENABLE,0)==MusbInvalid);
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER|MUSB_PORT_CONNECTION|
      MUSB_PORT_LOW_SPEED|MUSB_PORT_HIGH_SPEED,0)==MusbInvalid);
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER|MUSB_PORT_RESET,0)==MusbInvalid);
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER,32)==MusbInvalid);
  Bus.Registers[REG_INTRUSB]=0x20;
  Bus.Registers[REG_DEVCTL]=0x1d;
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER|MUSB_PORT_CONNECTION|MUSB_PORT_ENABLE,0)==MusbOk);
  CHECK(MusbHostGetPortStatus(&Host,0,&Status,&Change)==MusbOk);
  CHECK(Status==MUSB_PORT_POWER && (Change&MUSB_PORT_CHANGE_CONNECTION));
  Bus.Registers[REG_DEVCTL]=0;
  CHECK(MusbHostAdoptPortState(&Host,MUSB_PORT_POWER,0)==MusbNotReady);
  CHECK(!Bus.LockHeld);
}
static void
TestCompletedResetCanBeCleared(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint32_t Writes;
  uint16_t Port, Change;
  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  Port = Host.PortStatus; Change = Host.PortChangeStatus;
  Writes = Bus.ResetPowerWrites;
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  CHECK(Host.PortStatus == Port && Host.PortChangeStatus == Change);
  CHECK(Bus.ResetPowerWrites == Writes);
  Bus.FailEnabled = 1U; Bus.FailOffset = REG_POWER;
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbIoError);
  Bus.FailEnabled = 0U;
  Bus.ResetWriteFault = 2U;
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbIoError);
  Writes = Bus.ResetPowerWrites;
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbIoError);
  CHECK((Host.PortStatus & MUSB_PORT_ENABLE) == 0U && Bus.ResetPowerWrites == Writes);
  Bus.ResetWriteFault = 0U; Bus.Registers[REG_POWER] = 0U;
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  CHECK((Host.PortStatus & MUSB_PORT_ENABLE) == 0U && !Bus.LockHeld);
}

static void
TestMissedConnectAndHotplug(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint16_t Port, Change, Expected;
  const uint8_t Speeds[] = {0x40U, 0x20U};
  const uint8_t Invalid[] = {0x1dU, 0x7dU, 0x59U, 0x5cU, 0xddU,
                             0x45U, 0x4dU, 0x55U};

  for (unsigned Index = 0U; Index < sizeof(Speeds); ++Index) {
    InitFake(&Bus);
    Bus.Registers[REG_DEVCTL] = (uint8_t)(0x1dU | Speeds[Index]);
    Bus.Registers[REG_INTRUSB] = 0x04U; /* Unrelated interrupt must survive. */
    InitHost(&Bus, &Host);
    CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
    Expected = MUSB_PORT_POWER | MUSB_PORT_CONNECTION;
    if (Speeds[Index] == 0x20U) Expected |= MUSB_PORT_LOW_SPEED;
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    CHECK(Port == Expected && Change == MUSB_PORT_CHANGE_CONNECTION);
    CHECK(Bus.Registers[REG_INTRUSB] == 0x04U);
    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_ENABLE) == MusbOk);
    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_RESET) == MusbOk);
    for (unsigned Poll = 0U; Poll < 4U; ++Poll) {
      CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
      CHECK(Port == (Expected | MUSB_PORT_ENABLE) && Change == 0U);
    }
    /* Remove and reattach repeatedly, with neither interrupt delivered. */
    for (unsigned Cycle = 0U; Cycle < 3U; ++Cycle) {
      Bus.Registers[REG_DEVCTL] = 0x1dU;
      CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
      CHECK(Port == MUSB_PORT_POWER);
      CHECK((Change & MUSB_PORT_CHANGE_CONNECTION) != 0U);
      CHECK(MusbHostClearPortFeature(&Host, 0U,
                                    MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
      CHECK(MusbHostClearPortFeature(&Host, 0U,
                                    MUSB_PORT_FEATURE_CHANGE_ENABLE) == MusbOk);
      CHECK(MusbHostClearPortFeature(&Host, 0U,
                                    MUSB_PORT_FEATURE_CHANGE_RESET) == MusbOk);
      CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
      CHECK(Port == MUSB_PORT_POWER && Change == 0U);
      Bus.Registers[REG_DEVCTL] = (uint8_t)(0x1dU | Speeds[Index]);
      CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
      CHECK(Port == Expected && Change == MUSB_PORT_CHANGE_CONNECTION);
      CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    }
    /* An explicit disconnect wins over stale speed and simultaneous connect. */
    Bus.Registers[REG_INTRUSB] = 0x34U;
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    CHECK(Port == MUSB_PORT_POWER && Bus.Registers[REG_INTRUSB] == 0x04U);
    Bus.Registers[REG_DEVCTL] = 0x1dU;
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    CHECK(Port == MUSB_PORT_POWER);
    CHECK(Bus.DevCtlWriteCount == 0U && !Bus.LockHeld);
  }
  for (unsigned Index = 0U; Index < sizeof(Invalid); ++Index) {
    InitFakeHost(&Bus, &Host);
    Bus.Registers[REG_INTRUSB] = 0U;
    Bus.Registers[REG_DEVCTL] = Invalid[Index];
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    CHECK(Port == (Index < 2U ? MUSB_PORT_POWER : 0U) && Change == 0U);
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_POWER) ==
          (Index < 2U ? MusbOk : MusbNotReady));
    CHECK(Bus.DevCtlWriteCount == 0U && !Bus.LockHeld);
  }
  InitFakeHost(&Bus, &Host);
  Bus.Registers[REG_INTRUSB] = 0U;
  Bus.Registers[REG_DEVCTL] = 0x5dU;
  Bus.FailEnabled = 1U;
  Bus.FailOffset = REG_DEVCTL;
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbIoError);
  CHECK(Host.PortStatus == MUSB_PORT_POWER && !Bus.LockHeld);
}

static void
TestPortPowerLoss(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  uint32_t Writes;
  uint16_t Port, Change;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  CHECK(MusbHostClearPortFeature(&Host, 0U,
                                MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
  CHECK(MusbHostClearPortFeature(&Host, 0U,
                                MUSB_PORT_FEATURE_CHANGE_ENABLE) == MusbOk);
  CHECK(MusbHostClearPortFeature(&Host, 0U,
                                MUSB_PORT_FEATURE_CHANGE_RESET) == MusbOk);
  Writes = Bus.WriteCount;
  Bus.Registers[REG_DEVCTL] = 0x45U; /* Stale FSDEV with no valid VBUS. */
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_POWER) == MusbNotReady);
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK(Port == 0U && Change == (MUSB_PORT_CHANGE_CONNECTION |
                                 MUSB_PORT_CHANGE_ENABLE));
  CHECK(MusbHostClearPortFeature(&Host, 0U,
                                MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
  CHECK(MusbHostClearPortFeature(&Host, 0U,
                                MUSB_PORT_FEATURE_CHANGE_ENABLE) == MusbOk);
  Bus.Registers[REG_DEVCTL] = 0x1dU;
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_POWER) == MusbOk);
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK(Port == MUSB_PORT_POWER && Change == 0U);
  Bus.Registers[REG_DEVCTL] = 0x5dU;
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK(Port == (MUSB_PORT_POWER | MUSB_PORT_CONNECTION) &&
        Change == MUSB_PORT_CHANGE_CONNECTION);
  CHECK(Bus.WriteCount == Writes && !Bus.LockHeld);
}


static void
TestTransferRequiresEnabledPort(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t Setup[8] = {0U, 5U, 1U, 0U, 0U, 0U, 0U, 0U};
  uint8_t Data[8] = {0U}, Toggle = 0U;
  uint16_t Port, Change;
  uint32_t Bytes, Writes;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  for (unsigned Attach = 0U; Attach < 2U; ++Attach) {
    CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
    CHECK(Port == (MUSB_PORT_POWER | MUSB_PORT_CONNECTION));
    CHECK((Change & MUSB_PORT_CHANGE_CONNECTION) != 0U);
    if (Attach != 0U) CHECK((Change & MUSB_PORT_CHANGE_ENABLE) != 0U);
    Writes = Bus.WriteCount;
    CHECK(MusbHostControlTransfer(&Host, 0U, MUSB_SPEED_FULL, 8U, Setup,
                                  NULL, 0U, 10U, &Bytes) == MusbDisconnected);
    CHECK(Bytes == 0U);
    CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 1U, MUSB_SPEED_FULL,
                               8U, 0U, Data, sizeof(Data), &Toggle, 10U,
                               &Bytes) == MusbDisconnected);
    CHECK(Bytes == 0U && Toggle == 0U && Bus.WriteCount == Writes);
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    CHECK(MusbHostControlTransfer(&Host, 0U, MUSB_SPEED_FULL, 8U, Setup,
                                  NULL, 0U, 10U, &Bytes) == MusbOk);
    if (Attach == 0U) {
      CHECK(MusbHostClearPortFeature(&Host, 0U,
                                    MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
      CHECK(MusbHostClearPortFeature(&Host, 0U,
                                    MUSB_PORT_FEATURE_CHANGE_ENABLE) == MusbOk);
      Bus.ReconnectOnTx = 1U;
      CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 1U, MUSB_SPEED_FULL,
                                 8U, 0U, Data, sizeof(Data), &Toggle, 10U,
                                 &Bytes) == MusbDisconnected);
      CHECK(Bytes == 0U && Toggle == 0U);
    }
  }
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 1U, MUSB_SPEED_FULL,
                             8U, 0U, Data, sizeof(Data), &Toggle, 10U, &Bytes) == MusbOk);
  CHECK(Bytes == sizeof(Data));
}

static void
TestPortSuspendAndResume(void)
{
  FAKE_MUSB Bus;
  MUSB_HOST Host;
  const uint8_t Setup[8] = {0U, 5U, 1U, 0U, 0U, 0U, 0U, 0U};
  uint8_t Data[8] = {0U}, Toggle = 1U;
  uint16_t Port, Change;
  uint32_t Bytes, Writes;
  uint64_t Started;

  InitFakeHost(&Bus, &Host);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbNotReady);
  CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbDisconnected);
  Bus.Registers[REG_POWER] = 0x31U;
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  Writes = Bus.ResetPowerWrites;
  Started = Bus.TimeUs;
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbOk);
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbOk);
  CHECK(Bus.TimeUs - Started == 3000U && Bus.Registers[REG_POWER] == 0x33U);
  CHECK(Bus.ResetPowerWrites == Writes + 1U);
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK(Port == (MUSB_PORT_POWER | MUSB_PORT_CONNECTION | MUSB_PORT_ENABLE |
                 MUSB_PORT_HIGH_SPEED | MUSB_PORT_SUSPEND));
  CHECK((Change & MUSB_PORT_CHANGE_SUSPEND) == 0U);

  Writes = Bus.WriteCount;
  CHECK(MusbHostControlTransfer(&Host, 0U, MUSB_SPEED_HIGH, 64U, Setup, NULL,
                                 0U, 10U, &Bytes) == MusbNotReady);
  CHECK(Bytes == 0U);
  CHECK(MusbHostDataTransfer(&Host, MUSB_TRANSFER_BULK, 1U, 1U, MUSB_SPEED_HIGH,
                             512U, 0U, Data, sizeof(Data), &Toggle, 10U,
                             &Bytes) == MusbNotReady);
  CHECK(Bytes == 0U && Toggle == 1U && Bus.WriteCount == Writes);

  Writes = Bus.ResetPowerWrites;
  Started = Bus.TimeUs;
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbOk);
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbOk);
  CHECK(Bus.TimeUs - Started == 30000U && Bus.Registers[REG_POWER] == 0x31U);
  CHECK(Bus.ResetPowerWrites == Writes + 2U);
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK(Port == (MUSB_PORT_POWER | MUSB_PORT_CONNECTION | MUSB_PORT_ENABLE |
                 MUSB_PORT_HIGH_SPEED));
  CHECK((Change & MUSB_PORT_CHANGE_SUSPEND) != 0U);
  CHECK(MusbHostClearPortFeature(&Host, 0U,
                                 MUSB_PORT_FEATURE_CHANGE_SUSPEND) == MusbOk);
  CHECK(MusbHostGetPortStatus(&Host, 0U, &Port, &Change) == MusbOk);
  CHECK((Change & MUSB_PORT_CHANGE_SUSPEND) == 0U);
  CHECK(MusbHostControlTransfer(&Host, 0U, MUSB_SPEED_HIGH, 64U, Setup, NULL,
                                 0U, 10U, &Bytes) == MusbOk);

  Bus.DropPowerBits = 0x02U;
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbIoError);
  CHECK((Host.PortStatus & MUSB_PORT_SUSPEND) != 0U);
  Bus.DropPowerBits = 0x04U;
  CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbIoError);
  CHECK((Host.PortStatus & MUSB_PORT_SUSPEND) != 0U);
  CHECK((Host.PortChangeStatus & MUSB_PORT_CHANGE_SUSPEND) == 0U);
  Bus.DropPowerBits = 0U;
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbOk);
  Started = Bus.TimeUs;
  CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
  CHECK(Bus.TimeUs - Started == 80000U && Bus.Registers[REG_POWER] == 0x31U);
  CHECK((Host.PortStatus & MUSB_PORT_SUSPEND) == 0U);
}

static void
TestPortChangesDuringSuspendAndResume(void)
{
  static const struct {
    uint32_t Delay;
    uint8_t DevCtl;
    uint8_t Interrupt;
    MUSB_STATUS Result;
    uint8_t PowerAfter;
  } Cases[] = {
    {3000U,  0x1dU, 0x20U, MusbDisconnected, 0x23U},
    {20000U, 0x5dU, 0x10U, MusbDisconnected, 0x21U},
    {10000U, 0x1dU, 0x20U, MusbDisconnected, 0x21U},
    {20000U, 0x45U, 0x00U, MusbNotReady,     0x25U},
    {20000U, 0x99U, 0x00U, MusbNotReady,     0x25U},
  };
  FAKE_MUSB Bus;
  MUSB_HOST Host;

  for (unsigned I = 0U; I < sizeof(Cases) / sizeof(Cases[0]); ++I) {
    InitFakeHost(&Bus, &Host);
    Bus.Registers[REG_POWER] = 0x21U;
    CHECK(MusbHostSetState(&Host, MUSB_HOST_STATE_OPERATIONAL) == MusbOk);
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    if (Cases[I].Delay != 3000U) {
      CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) == MusbOk);
    }
    Bus.EventDelayUs = Cases[I].Delay;
    Bus.DevCtlOnDelay = Cases[I].DevCtl;
    Bus.UsbEventOnDelay = Cases[I].Interrupt;
    if (Cases[I].Delay == 3000U) {
      CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) ==
            Cases[I].Result);
    } else {
      CHECK(MusbHostClearPortFeature(&Host, 0U, MUSB_PORT_FEATURE_SUSPEND) ==
            Cases[I].Result);
    }
    CHECK(Bus.Registers[REG_POWER] == Cases[I].PowerAfter);
    CHECK((Host.PortStatus & (MUSB_PORT_ENABLE | MUSB_PORT_SUSPEND)) == 0U);
    CHECK((Host.PortChangeStatus & MUSB_PORT_CHANGE_SUSPEND) == 0U);
    CHECK(MusbHostClearPortFeature(&Host, 0U,
                                  MUSB_PORT_FEATURE_CHANGE_CONNECTION) == MusbOk);
    Bus.Registers[REG_DEVCTL] = 0x5dU;
    Bus.Registers[REG_INTRUSB] = 0x10U;
    CHECK(MusbHostSetPortFeature(&Host, 0U, MUSB_PORT_FEATURE_RESET) == MusbOk);
    CHECK(Bus.Registers[REG_POWER] == 0x21U && (Host.PortStatus & MUSB_PORT_ENABLE));
  }
}

int
main(int Argc, char **Argv)
{
  if ((Argc == 2) && (strcmp(Argv[1], "rx-toggle") == 0)) {
    TestBulkInNakShortAndOverflow();
    TestBulkInZeroLength();
    fprintf(stderr, "MUSB RX toggle checks: %u\n", Checks);
    return 0;
  }
  if ((Argc == 2) && (strcmp(Argv[1], "rx-cancel") == 0)) {
    TestCancellationAfterReceiveBeforePacketRead();
    TestCancellationDuringInputFifoDoesNotCountPartialPacket();
    fprintf(stderr, "MUSB RX cancellation checks: %u\n", Checks);
    return 0;
  }
  if ((Argc == 2) && (strcmp(Argv[1], "ep0-no-data") == 0)) {
    TestEp0ZeroLengthStatusDirection();
    fprintf(stderr, "MUSB EP0 status checks: %u\n", Checks);
    return 0;
  }
  if (Argc != 1) {
    fprintf(stderr, "usage: %s [rx-toggle|rx-cancel|ep0-no-data]\n", Argv[0]);
    return 2;
  }
  TestCompletedResetCanBeCleared();
  TestMissedConnectAndHotplug();
  TestPortPowerLoss();
  TestTransferRequiresEnabledPort();
  TestPortSuspendAndResume();
  TestPortChangesDuringSuspendAndResume();
  TestResetRequiresHardwareCompletion();
  TestAdoptPortState();
  TestVbusLossPreservesDisconnect();
  TestReadinessAndLock();
  TestControlInAndOutStatus();
  TestBulkOutWidthsToggleAndStall();
  TestInterruptMultiPacket();
  TestBulkOutZeroLengthAndInterruptTimeouts();
  TestBulkInNakShortAndOverflow();
  TestBulkInZeroLength();
  TestTimeoutDisconnectAndReset();
  TestHubRouting();
  TestBadHubRoutesDoNotSendPackets();
  TestCancellationBeforeHardwareAccess();
  TestCancellationDuringOutputFifoKeepsCompletedPacket();
  TestCancellationAfterCompletedOutputPacketAccountsToggle();
  TestCancellationAfterReceiveBeforePacketRead();
  TestCancellationDuringInputFifoDoesNotCountPartialPacket();
  TestCancellationDuringControlSetupAndStatus();
  TestCancellationDuringNakRetry();
  TestCancellationSurvivesOutputCleanupFailure();
  fprintf(stderr, "MUSB core checks: %u\n", Checks);
  return 0;
}

