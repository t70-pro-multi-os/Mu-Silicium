// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MusbHostCore.h>

#define REG_FADDR             0x000U
#define REG_POWER             0x001U
#define REG_INTRUSB           0x00AU
#define REG_INTRUSBE          0x00BU
#define REG_INDEX             0x00EU
#define REG_DEVCTL            0x060U
#define REG_TXTOG             0x084U
#define REG_RXTOG             0x080U
#define REG_EP_BASE           0x010U
#define REG_FIFO_BASE         0x020U
#define REG_TXFUNCADDR        0x480U
#define REG_TXHUBADDR         0x482U
#define REG_RXFUNCADDR        0x484U
#define REG_RXHUBADDR         0x486U

#define REG_TXMAXP            0x00U
#define REG_TXCSR             0x02U
#define REG_RXMAXP            0x04U
#define REG_RXCSR             0x06U
#define REG_RXCOUNT           0x08U
#define REG_TXTYPE            0x0AU
#define REG_TXINTERVAL        0x0BU
#define REG_RXTYPE            0x0CU
#define REG_RXINTERVAL        0x0DU
#define REG_TYPE0             REG_TXTYPE
#define REG_NAKLIMIT0         REG_TXINTERVAL
#define REG_COUNT0            REG_RXCOUNT

#define POWER_HSMODE          0x10U
#define POWER_RESET           0x08U
#define POWER_RESUME          0x04U
#define POWER_SUSPENDM        0x02U
#define POWER_SOFTCONN        0x40U
#define SUSPEND_SETTLE_US      3000U
#define RESUME_SIGNAL_US      20000U
#define RESUME_RECOVERY_US    10000U

#define DEVCTL_FSDEV          0x40U
#define DEVCTL_LSDEV          0x20U
#define DEVCTL_HM              0x04U
#define DEVCTL_SESSION         0x01U
#define DEVCTL_BDEVICE         0x80U
#define DEVCTL_VBUS_MASK       0x18U
#define DEVCTL_VBUS_VALID      0x18U
#define DEVCTL_HOST_READY_MASK \
  (DEVCTL_HM | DEVCTL_SESSION | DEVCTL_BDEVICE | DEVCTL_VBUS_MASK)
#define DEVCTL_HOST_READY      (DEVCTL_HM | DEVCTL_SESSION | DEVCTL_VBUS_VALID)

#define INTR_CONNECT          0x10U
#define INTR_DISCONNECT       0x20U

#define CSR0_FLUSHFIFO        0x0100U
#define CSR0_TXPKTRDY         0x0002U
#define CSR0_RXPKTRDY         0x0001U
#define CSR0_H_STATUSPKT      0x0040U
#define CSR0_H_REQPKT         0x0020U
#define CSR0_H_ERROR          0x0010U
#define CSR0_H_SETUPPKT       0x0008U
#define CSR0_H_RXSTALL        0x0004U
#define CSR0_H_NAKTIMEOUT     0x0080U
#define CSR0_H_DIS_PING       0x0800U

#define TXCSR_AUTOSET         0x8000U
#define TXCSR_DMAENAB         0x1000U
#define TXCSR_FRCDATATOG      0x0800U
#define TXCSR_DMAMODE         0x0400U
#define TXCSR_TXPKTRDY        0x0001U
#define TXCSR_FLUSHFIFO       0x0008U
#define TXCSR_FIFONOTEMPTY    0x0002U
#define TXCSR_H_NAKTIMEOUT    0x0080U
#define TXCSR_H_RXSTALL       0x0020U
#define TXCSR_H_ERROR         0x0004U
#define TXCSR_H_WZC_BITS      0x00A6U

#define RXCSR_AUTOCLEAR       0x8000U
#define RXCSR_DMAENAB         0x2000U
#define RXCSR_DISNYET         0x1000U
#define RXCSR_INCOMPRX        0x0100U
#define RXCSR_FLUSHFIFO       0x0010U
#define RXCSR_DATAERROR       0x0008U
#define RXCSR_RXPKTRDY        0x0001U
#define RXCSR_H_AUTOREQ       0x4000U
#define RXCSR_H_RXSTALL       0x0040U
#define RXCSR_H_REQPKT        0x0020U
#define RXCSR_H_ERROR         0x0004U
#define RXCSR_H_WZC_BITS      0x004DU

#define CSR0_H_WZC_BITS       0x00A5U
#define MUSB_DEFAULT_TIMEOUT_MS 1000U
#define MUSB_MAX_TIMEOUT_MS     60000U
#define MUSB_POLL_DELAY_US      50U
#define MUSB_MAX_POLL_COUNT     1200000U
#define MUSB_EP0_FLUSH_ATTEMPTS 5U
#define MUSB_TX_FLUSH_ATTEMPTS  100U

typedef struct {
  uint64_t StartedUs;
  uint64_t TimeoutUs;
  uint32_t PollBudget;
} MUSB_DEADLINE;

static int
RangeOk(const MUSB_HOST *Host, uint32_t Offset, uint32_t Width)
{
  return (Host != NULL) &&
         ((uint64_t)Offset + Width <= Host->Io.ControllerLength);
}

static MUSB_STATUS
CheckCancelled(MUSB_HOST *Host)
{
  if ((Host != NULL) && (Host->Io.Cancelled != NULL) &&
      Host->Io.Cancelled(Host->Io.Context)) {
    return MusbCancelled;
  }
  return MusbOk;
}

static MUSB_STATUS
Read8(MUSB_HOST *Host, uint32_t Offset, uint8_t *Value)
{
  if ((Value == NULL) || !RangeOk(Host, Offset, 1U) ||
      (Host->Io.Read8 == NULL) ||
      !Host->Io.Read8(Host->Io.Context, Offset, Value)) {
    return MusbIoError;
  }
  return MusbOk;
}

static MUSB_STATUS
Read16(MUSB_HOST *Host, uint32_t Offset, uint16_t *Value)
{
  if ((Value == NULL) || !RangeOk(Host, Offset, 2U) ||
      (Host->Io.Read16 == NULL) ||
      !Host->Io.Read16(Host->Io.Context, Offset, Value)) {
    return MusbIoError;
  }
  return MusbOk;
}

static MUSB_STATUS
Read32(MUSB_HOST *Host, uint32_t Offset, uint32_t *Value)
{
  if ((Value == NULL) || !RangeOk(Host, Offset, 4U) ||
      (Host->Io.Read32 == NULL) ||
      !Host->Io.Read32(Host->Io.Context, Offset, Value)) {
    return MusbIoError;
  }
  return MusbOk;
}

static MUSB_STATUS
Write8(MUSB_HOST *Host, uint32_t Offset, uint8_t Value)
{
  if (!RangeOk(Host, Offset, 1U) || (Host->Io.Write8 == NULL) ||
      !Host->Io.Write8(Host->Io.Context, Offset, Value)) {
    return MusbIoError;
  }
  return MusbOk;
}

static MUSB_STATUS
Write16(MUSB_HOST *Host, uint32_t Offset, uint16_t Value)
{
  if (!RangeOk(Host, Offset, 2U) || (Host->Io.Write16 == NULL) ||
      !Host->Io.Write16(Host->Io.Context, Offset, Value)) {
    return MusbIoError;
  }
  return MusbOk;
}

static MUSB_STATUS
Write32(MUSB_HOST *Host, uint32_t Offset, uint32_t Value)
{
  if (!RangeOk(Host, Offset, 4U) || (Host->Io.Write32 == NULL) ||
      !Host->Io.Write32(Host->Io.Context, Offset, Value)) {
    return MusbIoError;
  }
  return MusbOk;
}

static uint32_t
EpReg(uint8_t Endpoint, uint8_t Register)
{
  (void)Endpoint;
  return REG_EP_BASE + Register;
}

static uint32_t
FifoReg(uint8_t Endpoint)
{
  return REG_FIFO_BASE + ((uint32_t)Endpoint * 4U);
}

static MUSB_STATUS
SelectEndpoint(MUSB_HOST *Host, uint8_t Endpoint)
{
  return Write8(Host, REG_INDEX, Endpoint);
}

static MUSB_STATUS
TakeLock(MUSB_HOST *Host)
{
  if ((Host == NULL) || !Host->Initialized) {
    return MusbNotReady;
  }
  if ((Host->Io.TryLock != NULL) &&
      !Host->Io.TryLock(Host->Io.Context)) {
    return MusbBusy;
  }
  return MusbOk;
}

static void
DropLock(MUSB_HOST *Host)
{
  if ((Host != NULL) && (Host->Io.Unlock != NULL)) {
    Host->Io.Unlock(Host->Io.Context);
  }
}

static MUSB_STATUS
MakeDeadline(MUSB_HOST *Host, uint32_t TimeoutMilliseconds, MUSB_DEADLINE *Deadline)
{
  uint64_t TimeoutUs;

  if ((Host == NULL) || (Deadline == NULL) || (Host->Io.NowUs == NULL) ||
      (Host->Io.DelayUs == NULL)) {
    return MusbInvalid;
  }

  if (TimeoutMilliseconds == 0U) {
    TimeoutMilliseconds = MUSB_DEFAULT_TIMEOUT_MS;
  }
  if (TimeoutMilliseconds > MUSB_MAX_TIMEOUT_MS) {
    TimeoutMilliseconds = MUSB_MAX_TIMEOUT_MS;
  }

  TimeoutUs = (uint64_t)TimeoutMilliseconds * 1000U;
  Deadline->StartedUs = Host->Io.NowUs(Host->Io.Context);
  Deadline->TimeoutUs = TimeoutUs;
  Deadline->PollBudget = (uint32_t)(TimeoutUs / MUSB_POLL_DELAY_US) + 2U;
  if (Deadline->PollBudget > MUSB_MAX_POLL_COUNT) {
    Deadline->PollBudget = MUSB_MAX_POLL_COUNT;
  }
  return MusbOk;
}

static int
Expired(MUSB_HOST *Host, const MUSB_DEADLINE *Deadline, uint32_t *Polls)
{
  uint64_t Now;

  (*Polls)++;
  if (*Polls >= Deadline->PollBudget) {
    return 1;
  }
  Now = Host->Io.NowUs(Host->Io.Context);
  return (Now - Deadline->StartedUs) >= Deadline->TimeoutUs;
}

static void
Pause(MUSB_HOST *Host, uint32_t Microseconds)
{
  Host->Io.DelayUs(Host->Io.Context, Microseconds);
}

static MUSB_STATUS
ReadBusState(MUSB_HOST *Host, uint8_t *DevCtl)
{
  MUSB_STATUS Status;

  Status = Read8(Host, REG_DEVCTL, DevCtl);
  if (Status != MusbOk) {
    return Status;
  }
  if ((*DevCtl & (DEVCTL_HM | DEVCTL_SESSION)) !=
      (DEVCTL_HM | DEVCTL_SESSION)) {
    return MusbNotReady;
  }
  return MusbOk;
}

static void
SetDisconnected(MUSB_HOST *Host)
{
  if ((Host->PortStatus & MUSB_PORT_CONNECTION) != 0U) {
    Host->PortStatus &= (uint16_t)~(MUSB_PORT_CONNECTION |
                                    MUSB_PORT_ENABLE |
                                    MUSB_PORT_RESET |
                                    MUSB_PORT_LOW_SPEED |
                                    MUSB_PORT_HIGH_SPEED |
                                    MUSB_PORT_SUSPEND);
    Host->PortChangeStatus |= MUSB_PORT_CHANGE_CONNECTION;
    Host->PortChangeStatus |= MUSB_PORT_CHANGE_ENABLE;
  }
}

/* Once a configured port loses VBUS, the hub must see and acknowledge
 * its detach before a powered attachment can reuse the change bit. */
static void
SetPortUnpowered(MUSB_HOST *Host)
{
  if ((Host->PortStatus & MUSB_PORT_CONNECTION) != 0U ||
      (Host->PortChangeStatus & MUSB_PORT_CHANGE_CONNECTION) != 0U) {
    Host->PortPowerLost = 1U;
  }
  SetDisconnected(Host);
  Host->PortStatus = 0U;
}

static MUSB_STATUS
PollPortEvents(MUSB_HOST *Host, uint8_t *DevCtlOut)
{
  MUSB_STATUS Status;
  uint8_t Interrupts;
  uint8_t DevCtl;
  uint8_t Relevant;
  uint8_t Speed;
  int Powered;
  int DetachPending;

  Status = Read8(Host, REG_INTRUSB, &Interrupts);
  if (Status != MusbOk) {
    return Status;
  }
  Relevant = (uint8_t)(Interrupts & Host->Config.InterruptMask &
                       (INTR_CONNECT | INTR_DISCONNECT | MUSB_HOST_RESUME_MASK));
  if (Relevant != 0U) {
    /*
     * The stock ISR acknowledges the USB interrupt register by writing back
     * the asserted bits (W1C). Acknowledge only the events this owner supports.
     */
    Status = Write8(Host, REG_INTRUSB, Relevant);
    if (Status != MusbOk) {
      return Status;
    }
  }

  Status = Read8(Host, REG_DEVCTL, &DevCtl);
  if (Status != MusbOk) {
    return Status;
  }

  /* The board supplies VBUS. The host core only reports its live state. */
  Powered = (DevCtl & DEVCTL_HOST_READY_MASK) == DEVCTL_HOST_READY;
  if (Powered) {
    Host->PortStatus |= MUSB_PORT_POWER;
  }
  Speed = (uint8_t)(DevCtl & (DEVCTL_FSDEV | DEVCTL_LSDEV));
  DetachPending = Host->PortPowerLost &&
                  ((Host->PortStatus & MUSB_PORT_CONNECTION) == 0U) &&
                  ((Host->PortChangeStatus & MUSB_PORT_CHANGE_CONNECTION) != 0U);
  if (!Powered) {
    SetPortUnpowered(Host);
  } else if (((Relevant & INTR_DISCONNECT) != 0U) ||
             ((Speed != DEVCTL_FSDEV) && (Speed != DEVCTL_LSDEV))) {
    SetDisconnected(Host);
  } else if (!DetachPending &&
             (((Relevant & INTR_CONNECT) != 0U) ||
              ((Host->PortStatus & MUSB_PORT_CONNECTION) == 0U))) {
    if ((Host->PortStatus & MUSB_PORT_ENABLE) != 0U) {
      Host->PortChangeStatus |= MUSB_PORT_CHANGE_ENABLE;
    }
    Host->PortStatus |= MUSB_PORT_CONNECTION;
    Host->PortStatus &= (uint16_t)~(MUSB_PORT_ENABLE |
                                    MUSB_PORT_LOW_SPEED |
                                    MUSB_PORT_HIGH_SPEED |
                                    MUSB_PORT_SUSPEND);
    Host->PortChangeStatus |= MUSB_PORT_CHANGE_CONNECTION;
    if (Speed == DEVCTL_LSDEV) {
      Host->PortStatus |= MUSB_PORT_LOW_SPEED;
    }
  }

  if (DevCtlOut != NULL) {
    *DevCtlOut = DevCtl;
  }
  return MusbOk;
}

static MUSB_STATUS
RequirePort(MUSB_HOST *Host, uint16_t Required)
{
  MUSB_STATUS Status;

  Status = PollPortEvents(Host, NULL);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Host->PortStatus & MUSB_PORT_POWER) == 0U) {
    return MusbNotReady;
  }
  return ((Host->PortStatus & Required) == Required) ? MusbOk : MusbDisconnected;
}

static MUSB_STATUS
CheckConnected(MUSB_HOST *Host)
{
  MUSB_STATUS Status;

  Status = RequirePort(Host, MUSB_PORT_ENABLE);
  if ((Status == MusbOk) && ((Host->PortStatus & MUSB_PORT_SUSPEND) != 0U)) {
    return MusbNotReady;
  }
  return Status;
}

static MUSB_STATUS
WriteFifo(MUSB_HOST *Host, uint8_t Endpoint, const uint8_t *Data, uint32_t Length)
{
  MUSB_STATUS Status;
  uint32_t Offset;
  uint32_t Position;
  uint32_t Word;

  if ((Length != 0U) && (Data == NULL)) {
    return MusbInvalid;
  }
  Offset = FifoReg(Endpoint);
  if (!RangeOk(Host, Offset, 4U)) {
    return MusbIoError;
  }

  Position = 0U;
  while (Position + 4U <= Length) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) return Status;
    Word = (uint32_t)Data[Position] |
           ((uint32_t)Data[Position + 1U] << 8) |
           ((uint32_t)Data[Position + 2U] << 16) |
           ((uint32_t)Data[Position + 3U] << 24);
    Status = Write32(Host, Offset, Word);
    if (Status != MusbOk) {
      return Status;
    }
    Position += 4U;
  }
  if (Position + 2U <= Length) {
    uint16_t Half;
    Status = CheckCancelled(Host);
    if (Status != MusbOk) return Status;
    Half = (uint16_t)Data[Position] |
                    (uint16_t)((uint16_t)Data[Position + 1U] << 8);
    Status = Write16(Host, Offset, Half);
    if (Status != MusbOk) {
      return Status;
    }
    Position += 2U;
  }
  if (Position < Length) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) return Status;
    Status = Write8(Host, Offset, Data[Position]);
    if (Status != MusbOk) {
      return Status;
    }
  }
  return MusbOk;
}

static MUSB_STATUS
ReadFifo(MUSB_HOST *Host, uint8_t Endpoint, uint8_t *Data, uint32_t Length)
{
  MUSB_STATUS Status;
  uint32_t Offset;
  uint32_t Position;
  uint32_t Word;
  uint16_t Half;

  if ((Length != 0U) && (Data == NULL)) {
    return MusbInvalid;
  }
  Offset = FifoReg(Endpoint);
  if (!RangeOk(Host, Offset, 4U)) {
    return MusbIoError;
  }

  Position = 0U;
  while (Position + 4U <= Length) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) return Status;
    Status = Read32(Host, Offset, &Word);
    if (Status != MusbOk) {
      return Status;
    }
    Data[Position] = (uint8_t)Word;
    Data[Position + 1U] = (uint8_t)(Word >> 8);
    Data[Position + 2U] = (uint8_t)(Word >> 16);
    Data[Position + 3U] = (uint8_t)(Word >> 24);
    Position += 4U;
  }
  if (Position + 2U <= Length) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) return Status;
    Status = Read16(Host, Offset, &Half);
    if (Status != MusbOk) {
      return Status;
    }
    Data[Position] = (uint8_t)Half;
    Data[Position + 1U] = (uint8_t)(Half >> 8);
    Position += 2U;
  }
  if (Position < Length) {
    uint8_t Byte;
    Status = CheckCancelled(Host);
    if (Status != MusbOk) return Status;
    Status = Read8(Host, Offset, &Byte);
    if (Status != MusbOk) {
      return Status;
    }
    Data[Position] = Byte;
  }
  return MusbOk;
}

static MUSB_STATUS
FlushEp0(MUSB_HOST *Host)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Attempt;

  Status = SelectEndpoint(Host, 0U);
  if (Status != MusbOk) {
    return Status;
  }
  for (Attempt = 0U; Attempt < MUSB_EP0_FLUSH_ATTEMPTS; ++Attempt) {
    Status = Read16(Host, EpReg(0U, REG_TXCSR), &Csr);
    if (Status != MusbOk) {
      return Status;
    }
    if ((Csr & (CSR0_TXPKTRDY | CSR0_RXPKTRDY)) == 0U) {
      break;
    }
    Status = Write16(Host, EpReg(0U, REG_TXCSR), CSR0_FLUSHFIFO);
    if (Status != MusbOk) {
      return Status;
    }
    Pause(Host, 10U);
  }
  Status = Read16(Host, EpReg(0U, REG_TXCSR), &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Csr & (CSR0_TXPKTRDY | CSR0_RXPKTRDY)) != 0U) {
    return MusbTimeout;
  }
  return Write16(Host, EpReg(0U, REG_TXCSR), 0U);
}

static MUSB_STATUS
FlushTx(MUSB_HOST *Host)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Attempt;

  for (Attempt = 0U; Attempt < MUSB_TX_FLUSH_ATTEMPTS; ++Attempt) {
    Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), &Csr);
    if (Status != MusbOk) {
      return Status;
    }
    if ((Csr & (TXCSR_FIFONOTEMPTY | TXCSR_TXPKTRDY)) == 0U) {
      return MusbOk;
    }
    Csr &= (uint16_t)~TXCSR_TXPKTRDY;
    Csr |= TXCSR_FLUSHFIFO;
    Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), Csr);
    if (Status != MusbOk) {
      return Status;
    }
    Pause(Host, 10U);
  }
  return MusbTimeout;
}

static MUSB_STATUS
FlushRx(MUSB_HOST *Host)
{
  MUSB_STATUS Status;
  uint32_t Offset;
  uint16_t Flush;

  Offset = EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR);
  Flush = RXCSR_FLUSHFIFO | RXCSR_RXPKTRDY;
  Status = Write16(Host, Offset, Flush);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Write16(Host, Offset, Flush);
  if (Status != MusbOk) {
    return Status;
  }
  {
    uint16_t Csr;
    Status = Read16(Host, Offset, &Csr);
    if (Status != MusbOk) {
      return Status;
    }
    if ((Csr & (RXCSR_RXPKTRDY | RXCSR_H_REQPKT)) != 0U) {
      return MusbTimeout;
    }
  }
  return MusbOk;
}

static MUSB_STATUS
ValidateRoute(MUSB_HOST *Host, uint8_t DeviceSpeed,
              const MUSB_HOST_ROUTE *Route)
{
  MUSB_STATUS Status;
  uint8_t Power;

  if ((DeviceSpeed > MUSB_SPEED_HIGH) ||
      ((Route != NULL) &&
       ((Route->HubAddress > 127U) || (Route->HubPort > 127U) ||
        (Route->MultiTt > 1U) || (Route->Reserved != 0U) ||
        ((Route->HubAddress == 0U) != (Route->HubPort == 0U)) ||
        ((Route->HubAddress == 0U) && (Route->MultiTt != 0U))))) {
    return MusbInvalid;
  }
  if ((Route == NULL) || (Route->HubAddress == 0U)) {
    return MusbOk;
  }
  if (DeviceSpeed == MUSB_SPEED_HIGH) {
    return MusbInvalid;
  }
  // A TT is meaningful only while the root link is high speed.  Check
  // before touching an endpoint or its FIFO, under the controller lock.
  Status = Read8(Host, REG_POWER, &Power);
  if (Status != MusbOk) {
    return Status;
  }
  return ((Power & POWER_HSMODE) != 0U) ? MusbOk : MusbNotReady;
}

static MUSB_STATUS
SetTargetAddress(MUSB_HOST *Host, uint8_t Endpoint, uint8_t DeviceAddress,
                 uint8_t DeviceSpeed, uint8_t TypeRegister, int Receive,
                 const MUSB_HOST_ROUTE *Route)
{
  MUSB_STATUS Status;
  uint8_t Power;
  uint16_t FunctionAddress;
  uint32_t FunctionOffset;
  uint32_t HubOffset;

  Status = Read8(Host, REG_POWER, &Power);
  if (Status != MusbOk) {
    return Status;
  }

  FunctionAddress = DeviceAddress;
  // The MTK full-speed extension is TX-only; RXFUNCADDR stays 7-bit.
  if (!Receive && ((Power & POWER_HSMODE) != 0U) &&
      ((TypeRegister & 0xC0U) == 0x80U) &&
      (DeviceSpeed == MUSB_SPEED_FULL)) {
    FunctionAddress |= 0x0100U;
  }

  if (Receive) {
    FunctionOffset = REG_RXFUNCADDR + (8U * Endpoint);
    HubOffset = REG_RXHUBADDR + (8U * Endpoint);
  } else {
    FunctionOffset = REG_TXFUNCADDR + (8U * Endpoint);
    HubOffset = REG_TXHUBADDR + (8U * Endpoint);
  }

  Status = Write16(Host, FunctionOffset, FunctionAddress);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Route == NULL) || (Route->HubAddress == 0U)) {
    return Write16(Host, HubOffset, 0U);
  }
  // The original MTK module uses two 16-bit RMW operations at +0x482
  // (TX) or +0x486 (RX), not byte writes to HUBADDR/HUBPORT.
  {
    uint16_t Hub;
    Status = Read16(Host, HubOffset, &Hub);
    if (Status != MusbOk) {
      return Status;
    }
    Hub = (uint16_t)((Hub & 0xFF00U) | Route->HubAddress |
                     (Route->MultiTt ? 0x80U : 0U));
    Status = Write16(Host, HubOffset, Hub);
    if (Status != MusbOk) {
      return Status;
    }
    Status = Read16(Host, HubOffset, &Hub);
    if (Status != MusbOk) {
      return Status;
    }
    Hub = (uint16_t)((Hub & 0x00FFU) | ((uint16_t)Route->HubPort << 8));
    return Write16(Host, HubOffset, Hub);
  }
}

static MUSB_STATUS
SetToggle(MUSB_HOST *Host, uint8_t Endpoint, uint8_t Toggle, int Receive)
{
  uint32_t Offset;
  uint32_t Mask;
  uint32_t Value;
  MUSB_STATUS Status;

  Offset = Receive ? REG_RXTOG : REG_TXTOG;
  Mask = 1U << Endpoint;
  Value = (1U << (Endpoint + 16U)) | (Toggle ? Mask : 0U);
  Status = Write32(Host, Offset, Value);
  if (Status != MusbOk) {
    return Status;
  }
  return Write32(Host, Offset, Toggle ? Mask : 0U);
}

static MUSB_STATUS
ReadRxToggle(MUSB_HOST *Host, uint8_t Endpoint, uint8_t *Toggle)
{
  MUSB_STATUS Status;
  uint32_t Value;

  if ((Endpoint >= MUSB_HOST_MAX_ENDPOINTS) || (Toggle == NULL)) {
    return MusbInvalid;
  }
  Status = Read32(Host, REG_RXTOG, &Value);
  if (Status != MusbOk) {
    return Status;
  }
  *Toggle = (uint8_t)((Value & (1U << Endpoint)) != 0U);
  return MusbOk;
}

static MUSB_STATUS
PreserveRxToggleIfReady(MUSB_HOST *Host, uint8_t *Toggle)
{
  MUSB_STATUS Status;
  uint16_t Csr;

  Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR), &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Csr & RXCSR_RXPKTRDY) == 0U) {
    return MusbOk;
  }
  return ReadRxToggle(Host, MUSB_HOST_PIO_ENDPOINT, Toggle);
}

static MUSB_STATUS
ReturnRxWaitError(MUSB_HOST *Host, uint8_t *Toggle, MUSB_STATUS Status)
{
  MUSB_STATUS CaptureStatus;

  CaptureStatus = PreserveRxToggleIfReady(Host, Toggle);
  return (CaptureStatus == MusbOk) ? Status : CaptureStatus;
}

static MUSB_STATUS
ClearEp0AfterError(MUSB_HOST *Host)
{
  MUSB_STATUS Status;
  uint16_t Csr;

  Status = Read16(Host, EpReg(0U, REG_TXCSR), &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Csr & CSR0_H_REQPKT) != 0U) {
    Csr &= (uint16_t)~CSR0_H_REQPKT;
    Status = Write16(Host, EpReg(0U, REG_TXCSR), Csr);
    if (Status != MusbOk) {
      return Status;
    }
    Csr &= (uint16_t)~CSR0_H_NAKTIMEOUT;
    Status = Write16(Host, EpReg(0U, REG_TXCSR), Csr);
    if (Status != MusbOk) {
      return Status;
    }
  } else {
    Status = FlushEp0(Host);
    if (Status != MusbOk) {
      return Status;
    }
  }
  Status = Write8(Host, EpReg(0U, REG_NAKLIMIT0), 0U);
  if (Status != MusbOk) {
    return Status;
  }
  return Write16(Host, EpReg(0U, REG_TXCSR), 0U);
}

static MUSB_STATUS
ClearDataEndpointAfterError(MUSB_HOST *Host, int Receive)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Offset;

  if (Receive) {
    Status = FlushRx(Host);
    if (Status != MusbOk) {
      return Status;
    }
    Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR), &Csr);
    if (Status != MusbOk) {
      return Status;
    }
    Csr &= (uint16_t)~(RXCSR_H_REQPKT | RXCSR_H_AUTOREQ |
                       RXCSR_AUTOCLEAR | RXCSR_DMAENAB |
                       RXCSR_H_RXSTALL | RXCSR_H_ERROR |
                       RXCSR_DATAERROR | RXCSR_RXPKTRDY);
    Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR), Csr);
    if (Status != MusbOk) {
      return Status;
    }
    return Write8(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXINTERVAL), 0U);
  }

  Status = FlushTx(Host);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Csr &= (uint16_t)~(TXCSR_AUTOSET | TXCSR_DMAENAB |
                     TXCSR_H_ERROR | TXCSR_H_RXSTALL |
                     TXCSR_H_NAKTIMEOUT | TXCSR_TXPKTRDY);
  Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Csr &= (uint16_t)~TXCSR_DMAMODE;
  Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Offset = EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXINTERVAL);
  return Write8(Host, Offset, 0U);
}

static MUSB_STATUS
WaitEp0Tx(MUSB_HOST *Host, const MUSB_DEADLINE *Deadline, uint16_t RetryCommand)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Polls;

  Polls = 0U;
  for (;;) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) {
      // A cancellation can race with completion. Confirm an idle EP0 CSR once
      // so the caller can account for a packet that completed before abort.
      if ((Read16(Host, EpReg(0U, REG_TXCSR), &Csr) == MusbOk) &&
          ((Csr & (CSR0_TXPKTRDY | CSR0_RXPKTRDY | CSR0_H_NAKTIMEOUT |
                   CSR0_H_ERROR | CSR0_H_RXSTALL)) == 0U)) {
        return MusbOk;
      }
      return MusbCancelled;
    }
    uint8_t DevCtl;
    Status = PollPortEvents(Host, &DevCtl);
    if (Status != MusbOk) {
      return Status;
    }
    if (((DevCtl & (DEVCTL_HM | DEVCTL_SESSION)) !=
         (DEVCTL_HM | DEVCTL_SESSION)) ||
        ((Host->PortStatus & MUSB_PORT_ENABLE) == 0U)) {
      return MusbDisconnected;
    }
    Status = Read16(Host, EpReg(0U, REG_TXCSR), &Csr);
    if (Status != MusbOk) {
      return Status;
    }
    if ((Csr & CSR0_H_RXSTALL) != 0U) {
      return MusbStall;
    }
    if ((Csr & CSR0_H_ERROR) != 0U) {
      return MusbProtocolError;
    }
    if ((Csr & CSR0_H_NAKTIMEOUT) != 0U) {
      Status = CheckCancelled(Host);
      if (Status != MusbOk) return Status;
      Status = Write16(Host, EpReg(0U, REG_TXCSR), 0U);
      if (Status != MusbOk) {
        return Status;
      }
      Status = Write16(Host, EpReg(0U, REG_TXCSR), RetryCommand);
      if (Status != MusbOk) {
        return Status;
      }
    } else if ((Csr & CSR0_TXPKTRDY) == 0U) {
      return MusbOk;
    }
    if (Expired(Host, Deadline, &Polls)) {
      return MusbTimeout;
    }
    Pause(Host, MUSB_POLL_DELAY_US);
  }
}

static MUSB_STATUS
WaitEp0Rx(MUSB_HOST *Host, const MUSB_DEADLINE *Deadline, uint16_t RetryCommand,
          uint16_t *Count, int StatusStage)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Polls;

  Polls = 0U;
  for (;;) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) return Status;
    uint8_t DevCtl;
    Status = PollPortEvents(Host, &DevCtl);
    if (Status != MusbOk) {
      return Status;
    }
    if (((DevCtl & (DEVCTL_HM | DEVCTL_SESSION)) !=
         (DEVCTL_HM | DEVCTL_SESSION)) ||
        ((Host->PortStatus & MUSB_PORT_ENABLE) == 0U)) {
      return MusbDisconnected;
    }
    Status = Read16(Host, EpReg(0U, REG_TXCSR), &Csr);
    if (Status != MusbOk) {
      return Status;
    }
    if ((Csr & CSR0_H_RXSTALL) != 0U) {
      return MusbStall;
    }
    if ((Csr & CSR0_H_ERROR) != 0U) {
      return MusbProtocolError;
    }
    if ((Csr & CSR0_H_NAKTIMEOUT) != 0U) {
      Status = CheckCancelled(Host);
      if (Status != MusbOk) return Status;
      Status = Write16(Host, EpReg(0U, REG_TXCSR), 0U);
      if (Status != MusbOk) {
        return Status;
      }
      Status = Write16(Host, EpReg(0U, REG_TXCSR), RetryCommand);
      if (Status != MusbOk) {
        return Status;
      }
    } else if (((Csr & CSR0_RXPKTRDY) != 0U) ||
               (StatusStage && ((Csr & CSR0_H_REQPKT) == 0U))) {
      if ((Csr & CSR0_RXPKTRDY) != 0U) {
        uint8_t ByteCount;
        Status = Read8(Host, EpReg(0U, REG_COUNT0), &ByteCount);
        if (Status != MusbOk) {
          return Status;
        }
        *Count = ByteCount;
      } else {
        *Count = 0U;
      }
      return MusbOk;
    }
    if (Expired(Host, Deadline, &Polls)) {
      return MusbTimeout;
    }
    Pause(Host, MUSB_POLL_DELAY_US);
  }
}

static MUSB_STATUS
WaitTx(MUSB_HOST *Host, const MUSB_DEADLINE *Deadline)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Polls;
  uint32_t Offset;

  Offset = EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR);
  Polls = 0U;
  for (;;) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) {
      // Count an in-flight packet only if the hardware already reports idle.
      if ((Read16(Host, Offset, &Csr) == MusbOk) &&
          ((Csr & (TXCSR_FIFONOTEMPTY | TXCSR_TXPKTRDY |
                   TXCSR_H_NAKTIMEOUT | TXCSR_H_RXSTALL |
                   TXCSR_H_ERROR)) == 0U)) {
        return MusbOk;
      }
      return MusbCancelled;
    }
    uint8_t DevCtl;
    Status = PollPortEvents(Host, &DevCtl);
    if (Status != MusbOk) {
      return Status;
    }
    if (((DevCtl & (DEVCTL_HM | DEVCTL_SESSION)) !=
         (DEVCTL_HM | DEVCTL_SESSION)) ||
        ((Host->PortStatus & MUSB_PORT_ENABLE) == 0U)) {
      return MusbDisconnected;
    }
    Status = Read16(Host, Offset, &Csr);
    if (Status != MusbOk) {
      return Status;
    }
    if ((Csr & TXCSR_H_RXSTALL) != 0U) {
      return MusbStall;
    }
    if ((Csr & TXCSR_H_ERROR) != 0U) {
      return MusbProtocolError;
    }
    if ((Csr & TXCSR_H_NAKTIMEOUT) != 0U) {
      Status = CheckCancelled(Host);
      if (Status != MusbOk) {
        return Status;
      }
      Status = Write16(Host, Offset,
                       (uint16_t)((TXCSR_H_WZC_BITS & ~TXCSR_H_NAKTIMEOUT) |
                                  TXCSR_TXPKTRDY));
      if (Status != MusbOk) {
        return Status;
      }
    } else if ((Csr & (TXCSR_FIFONOTEMPTY | TXCSR_TXPKTRDY)) == 0U) {
      return MusbOk;
    }
    if (Expired(Host, Deadline, &Polls)) {
      return MusbTimeout;
    }
    Pause(Host, MUSB_POLL_DELAY_US);
  }
}

static MUSB_STATUS
RequestRxPacket(MUSB_HOST *Host)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Offset;

  Offset = EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR);
  Status = Read16(Host, Offset, &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Csr |= RXCSR_H_WZC_BITS;
  Csr &= (uint16_t)~(RXCSR_RXPKTRDY | RXCSR_H_REQPKT |
                     RXCSR_DATAERROR);
  Csr |= RXCSR_H_REQPKT;
  return Write16(Host, Offset, Csr);
}

static MUSB_STATUS
WaitRxPacket(MUSB_HOST *Host, const MUSB_DEADLINE *Deadline,
             uint16_t *PacketLength, uint8_t *DataToggle, int SinglePoll)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Offset;
  uint32_t Polls;

  Offset = EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR);
  Polls = 0U;
  for (;;) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) {
      return ReturnRxWaitError(Host, DataToggle, Status);
    }
    uint8_t DevCtl;
    Status = PollPortEvents(Host, &DevCtl);
    if (Status != MusbOk) {
      return ReturnRxWaitError(Host, DataToggle, Status);
    }
    if (((DevCtl & (DEVCTL_HM | DEVCTL_SESSION)) !=
         (DEVCTL_HM | DEVCTL_SESSION)) ||
        ((Host->PortStatus & MUSB_PORT_ENABLE) == 0U)) {
      return ReturnRxWaitError(Host, DataToggle, MusbDisconnected);
    }
    Status = Read16(Host, Offset, &Csr);
    if (Status != MusbOk) {
      return ReturnRxWaitError(Host, DataToggle, Status);
    }
    if ((Csr & RXCSR_H_RXSTALL) != 0U) {
      return ReturnRxWaitError(Host, DataToggle, MusbStall);
    }
    if ((Csr & RXCSR_H_ERROR) != 0U) {
      return ReturnRxWaitError(Host, DataToggle, MusbProtocolError);
    }
    if ((Csr & RXCSR_INCOMPRX) != 0U) {
      return ReturnRxWaitError(Host, DataToggle, MusbProtocolError);
    }
    if ((Csr & RXCSR_DATAERROR) != 0U) {
      if (SinglePoll) {
        // An idle endpoint can wait for the next software-scheduled poll.
        return ReturnRxWaitError(Host, DataToggle, MusbTimeout);
      }
      Status = CheckCancelled(Host);
      if (Status != MusbOk) {
        return ReturnRxWaitError(Host, DataToggle, Status);
      }
      if ((Csr & RXCSR_RXPKTRDY) != 0U) {
        Status = ReadRxToggle(Host, MUSB_HOST_PIO_ENDPOINT, DataToggle);
        if (Status != MusbOk) {
          return Status;
        }
      }
      Csr |= RXCSR_H_WZC_BITS;
      Csr &= (uint16_t)~(RXCSR_DATAERROR | RXCSR_RXPKTRDY |
                         RXCSR_H_REQPKT);
      Status = Write16(Host, Offset, Csr);
      if (Status != MusbOk) {
        return ReturnRxWaitError(Host, DataToggle, Status);
      }
      Status = RequestRxPacket(Host);
      if (Status != MusbOk) {
        return ReturnRxWaitError(Host, DataToggle, Status);
      }
    } else if ((Csr & RXCSR_RXPKTRDY) != 0U) {
      Status = ReadRxToggle(Host, MUSB_HOST_PIO_ENDPOINT, DataToggle);
      if (Status != MusbOk) {
        return Status;
      }
      Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCOUNT),
                      PacketLength);
      return (Status == MusbOk) ? Status :
             ReturnRxWaitError(Host, DataToggle, Status);
    }
    if (Expired(Host, Deadline, &Polls)) {
      return ReturnRxWaitError(Host, DataToggle, MusbTimeout);
    }
    Pause(Host, MUSB_POLL_DELAY_US);
  }
}

static MUSB_STATUS
FinishRxPacket(MUSB_HOST *Host, int RequestAnother)
{
  MUSB_STATUS Status;
  uint16_t Csr;
  uint32_t Offset;

  Offset = EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR);
  Status = Read16(Host, Offset, &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Csr |= RXCSR_H_WZC_BITS;
  Csr &= (uint16_t)~(RXCSR_RXPKTRDY | RXCSR_H_REQPKT |
                     RXCSR_DATAERROR);
  if (RequestAnother) {
    Csr |= RXCSR_H_REQPKT;
  }
  return Write16(Host, Offset, Csr);
}

static MUSB_STATUS
PrepareTxEndpoint(MUSB_HOST *Host, uint8_t DeviceAddress, uint8_t Endpoint,
                  uint8_t Speed, uint8_t TransferType, uint16_t MaximumPacket,
                  uint8_t Interval, uint8_t Toggle,
                  const MUSB_HOST_ROUTE *Route)
{
  MUSB_STATUS Status;
  uint8_t TypeRegister;
  uint16_t Csr;

  Status = SelectEndpoint(Host, MUSB_HOST_PIO_ENDPOINT);
  if (Status != MusbOk) {
    return Status;
  }
  Status = FlushTx(Host);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Csr &= (uint16_t)~(TXCSR_H_NAKTIMEOUT | TXCSR_AUTOSET |
                     TXCSR_DMAENAB | TXCSR_FRCDATATOG |
                     TXCSR_H_RXSTALL | TXCSR_H_ERROR |
                     TXCSR_TXPKTRDY);
  Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Csr &= (uint16_t)~TXCSR_DMAMODE;
  Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR), Csr);
  if (Status != MusbOk) {
    return Status;
  }

  TypeRegister = (uint8_t)((TransferType << 4) |
                           (Endpoint & 0x0FU) |
                           ((Speed == MUSB_SPEED_HIGH) ? 0x40U :
                            (Speed == MUSB_SPEED_FULL) ? 0x80U : 0xC0U));
  Status = Write8(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXTYPE),
                  TypeRegister);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXMAXP),
                   MaximumPacket);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Write8(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXINTERVAL),
                  Interval);
  if (Status != MusbOk) {
    return Status;
  }
  Status = SetTargetAddress(Host, MUSB_HOST_PIO_ENDPOINT, DeviceAddress,
                            Speed, TypeRegister, 0, Route);
  if (Status != MusbOk) {
    return Status;
  }
  return SetToggle(Host, MUSB_HOST_PIO_ENDPOINT, Toggle, 0);
}

static MUSB_STATUS
PrepareRxEndpoint(MUSB_HOST *Host, uint8_t DeviceAddress, uint8_t Endpoint,
                  uint8_t Speed, uint8_t TransferType, uint16_t MaximumPacket,
                  uint8_t Interval, uint8_t Toggle,
                  const MUSB_HOST_ROUTE *Route)
{
  MUSB_STATUS Status;
  uint8_t TypeRegister;
  uint16_t Csr;

  Status = SelectEndpoint(Host, MUSB_HOST_PIO_ENDPOINT);
  if (Status != MusbOk) {
    return Status;
  }
  Status = FlushRx(Host);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR), &Csr);
  if (Status != MusbOk) {
    return Status;
  }
  Csr &= (uint16_t)~(RXCSR_H_REQPKT | RXCSR_H_AUTOREQ |
                     RXCSR_AUTOCLEAR | RXCSR_DMAENAB |
                     RXCSR_H_RXSTALL | RXCSR_H_ERROR |
                     RXCSR_DATAERROR | RXCSR_RXPKTRDY);
  if (TransferType == MUSB_TRANSFER_INTERRUPT) {
    Csr |= RXCSR_DISNYET;
  } else {
    Csr &= (uint16_t)~RXCSR_DISNYET;
  }
  Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXCSR), Csr);
  if (Status != MusbOk) {
    return Status;
  }

  TypeRegister = (uint8_t)((TransferType << 4) |
                           (Endpoint & 0x0FU) |
                           ((Speed == MUSB_SPEED_HIGH) ? 0x40U :
                            (Speed == MUSB_SPEED_FULL) ? 0x80U : 0xC0U));
  Status = Write8(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXTYPE),
                  TypeRegister);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXMAXP),
                   MaximumPacket);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Write8(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_RXINTERVAL),
                  Interval);
  if (Status != MusbOk) {
    return Status;
  }
  Status = SetTargetAddress(Host, MUSB_HOST_PIO_ENDPOINT, DeviceAddress,
                            Speed, TypeRegister, 1, Route);
  if (Status != MusbOk) {
    return Status;
  }
  return SetToggle(Host, MUSB_HOST_PIO_ENDPOINT, Toggle, 1);
}

static MUSB_STATUS
ControlTransferLocked(MUSB_HOST *Host, uint8_t DeviceAddress,
                      uint8_t DeviceSpeed, uint8_t MaximumPacketLength,
                      const uint8_t SetupPacket[8], uint8_t *Data,
                      uint32_t DataCapacity, uint32_t TimeoutMilliseconds,
                      const MUSB_HOST_ROUTE *Route, uint32_t *BytesTransferred)
{
  MUSB_STATUS Status;
  MUSB_DEADLINE Deadline;
  uint16_t RequestLength;
  uint16_t Count;
  uint16_t TypeValue;
  uint32_t Actual;
  uint32_t Remaining;
  uint32_t Packet;
  uint8_t DirectionIn;
  uint8_t StatusDirectionIn;
  uint8_t DevCtl;

  *BytesTransferred = 0U;
  Status = CheckCancelled(Host);
  if (Status != MusbOk) return Status;
  Actual = 0U;
  RequestLength = (uint16_t)SetupPacket[6] |
                  (uint16_t)((uint16_t)SetupPacket[7] << 8);
  DirectionIn = (uint8_t)((SetupPacket[0] & 0x80U) != 0U);
  StatusDirectionIn = (uint8_t)((RequestLength == 0U) || !DirectionIn);
  if ((RequestLength > DataCapacity) ||
      ((RequestLength != 0U) && (Data == NULL))) {
    return MusbInvalid;
  }
  if ((MaximumPacketLength == 0U) ||
      (MaximumPacketLength > MUSB_HOST_EP0_FIFO_BYTES) ||
      ((DeviceSpeed == MUSB_SPEED_LOW) && (MaximumPacketLength != 8U)) ||
      ((DeviceSpeed == MUSB_SPEED_HIGH) && (MaximumPacketLength != 64U)) ||
      ((DeviceSpeed == MUSB_SPEED_FULL) &&
       (MaximumPacketLength != 8U) && (MaximumPacketLength != 16U) &&
       (MaximumPacketLength != 32U) && (MaximumPacketLength != 64U))) {
    return MusbInvalid;
  }
  if ((DeviceSpeed != MUSB_SPEED_LOW) &&
      (DeviceSpeed != MUSB_SPEED_FULL) &&
      (DeviceSpeed != MUSB_SPEED_HIGH)) {
    return MusbInvalid;
  }

  Status = MakeDeadline(Host, TimeoutMilliseconds, &Deadline);
  if (Status != MusbOk) {
    return Status;
  }
  Status = CheckConnected(Host);
  if (Status != MusbOk) {
    return Status;
  }
  Status = SelectEndpoint(Host, 0U);
  if (Status != MusbOk) {
    return Status;
  }
  Status = FlushEp0(Host);
  if (Status != MusbOk) {
    return Status;
  }

  TypeValue = (DeviceSpeed == MUSB_SPEED_HIGH) ? 0x40U :
              (DeviceSpeed == MUSB_SPEED_FULL) ? 0x80U : 0xC0U;
  Status = Write8(Host, EpReg(0U, REG_TYPE0), (uint8_t)TypeValue);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Write8(Host, EpReg(0U, REG_NAKLIMIT0), 0U);
  if (Status != MusbOk) {
    return Status;
  }
  Status = SetTargetAddress(Host, 0U, DeviceAddress, DeviceSpeed,
                            (uint8_t)TypeValue, 0, Route);
  if (Status != MusbOk) {
    return Status;
  }

  Status = WriteFifo(Host, 0U, SetupPacket, 8U);
  if (Status != MusbOk) {
    goto Error;
  }
  Status = CheckCancelled(Host);
  if (Status != MusbOk) goto Error;
  Status = Write16(Host, EpReg(0U, REG_TXCSR),
                   (uint16_t)(CSR0_H_DIS_PING | CSR0_H_SETUPPKT |
                              CSR0_TXPKTRDY));
  if (Status != MusbOk) {
    goto Error;
  }
  Status = WaitEp0Tx(Host, &Deadline,
                     (uint16_t)(CSR0_H_DIS_PING | CSR0_H_SETUPPKT |
                                CSR0_TXPKTRDY));
  if (Status != MusbOk) {
    goto Error;
  }

  Actual = 0U;
  if (RequestLength != 0U) {
    if (DirectionIn) {
      while (Actual < RequestLength) {
        Status = CheckCancelled(Host);
        if (Status != MusbOk) goto Error;
        Status = Write16(Host, EpReg(0U, REG_TXCSR), CSR0_H_REQPKT);
        if (Status != MusbOk) {
          goto Error;
        }
        Status = WaitEp0Rx(Host, &Deadline, CSR0_H_REQPKT, &Count, 0);
        if (Status != MusbOk) {
          goto Error;
        }
        Remaining = RequestLength - Actual;
        Packet = Count;
        if ((Packet > Remaining) ||
            (Packet > MaximumPacketLength) ||
            (Packet > MUSB_HOST_EP0_FIFO_BYTES)) {
          Status = FlushEp0(Host);
          if (Status != MusbOk) {
            goto Error;
          }
          Status = (Packet > Remaining) ? MusbBufferOverflow :
                                          MusbProtocolError;
          goto Error;
        }
        Status = ReadFifo(Host, 0U, Data + Actual, Packet);
        if (Status != MusbOk) {
          goto Error;
        }
        Actual += Packet;
        if (Count < MaximumPacketLength) {
          break;
        }
      }
    } else {
      Remaining = RequestLength;
      while (Remaining != 0U) {
        Status = CheckCancelled(Host);
        if (Status != MusbOk) goto Error;
        Packet = Remaining > MaximumPacketLength ?
                 MaximumPacketLength : Remaining;
        Status = WriteFifo(Host, 0U, Data + Actual, Packet);
        if (Status != MusbOk) {
          goto Error;
        }
        Status = CheckCancelled(Host);
        if (Status != MusbOk) goto Error;
        Status = Write16(Host, EpReg(0U, REG_TXCSR), CSR0_TXPKTRDY);
        if (Status != MusbOk) {
          goto Error;
        }
        Status = WaitEp0Tx(Host, &Deadline, CSR0_TXPKTRDY);
        if (Status != MusbOk) {
          goto Error;
        }
        Actual += Packet;
        Remaining -= Packet;
      }
    }
  }

  if (StatusDirectionIn) {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) goto Error;
    Status = Write16(Host, EpReg(0U, REG_TXCSR),
                     (uint16_t)(CSR0_H_STATUSPKT | CSR0_H_REQPKT));
    if (Status != MusbOk) {
      goto Error;
    }
    Count = 0U;
    Status = WaitEp0Rx(Host, &Deadline,
                       (uint16_t)(CSR0_H_STATUSPKT | CSR0_H_REQPKT),
                       &Count, 1);
    if (Status != MusbOk) {
      goto Error;
    }
    if (Count != 0U) {
      Status = FlushEp0(Host);
      if (Status != MusbOk) {
        goto Error;
      }
      Status = MusbProtocolError;
      goto Error;
    }
  } else {
    Status = CheckCancelled(Host);
    if (Status != MusbOk) goto Error;
    Status = Write16(Host, EpReg(0U, REG_TXCSR),
                     (uint16_t)(CSR0_H_STATUSPKT | CSR0_TXPKTRDY));
    if (Status != MusbOk) {
      goto Error;
    }
    Status = WaitEp0Tx(Host, &Deadline,
                       (uint16_t)(CSR0_H_STATUSPKT | CSR0_TXPKTRDY));
    if (Status != MusbOk) {
      goto Error;
    }
  }
  // TXPKTRDY may clear while the completed STATUSPKT selector remains set.
  Status = Write16(Host, EpReg(0U, REG_TXCSR), 0U);
  if (Status != MusbOk) {
    goto Error;
  }

  Status = CheckCancelled(Host);
  if (Status != MusbOk) {
    goto Error;
  }
  Status = ReadBusState(Host, &DevCtl);
  if (Status != MusbOk) {
    goto Error;
  }
  *BytesTransferred = Actual;
  return MusbOk;

Error:
  *BytesTransferred = Actual;
  (void)ClearEp0AfterError(Host);
  return Status;
}

static MUSB_STATUS
DataTransferLocked(MUSB_HOST *Host, uint8_t TransferType,
                   uint8_t DeviceAddress, uint8_t EndpointAddress,
                   uint8_t DeviceSpeed, uint16_t MaximumPacketLength,
                   uint8_t Interval, uint8_t *Data, uint32_t DataLength,
                   uint8_t *DataToggle, uint32_t TimeoutMilliseconds,
                   const MUSB_HOST_ROUTE *Route, uint32_t *BytesTransferred)
{
  MUSB_STATUS Status;
  MUSB_DEADLINE Deadline;
  uint32_t Actual;
  uint32_t Remaining;
  uint32_t Packet;
  uint16_t PacketLength;
  uint8_t Endpoint;
  uint8_t Receive;
  uint8_t Toggle;
  int SinglePoll;

  *BytesTransferred = 0U;
  Status = CheckCancelled(Host);
  if (Status != MusbOk) return Status;
  Endpoint = (uint8_t)(EndpointAddress & 0x0FU);
  Receive = (uint8_t)((EndpointAddress & 0x80U) != 0U);
  SinglePoll = (TransferType == MUSB_TRANSFER_INTERRUPT_POLL);
  if (SinglePoll) {
    if (!Receive) {
      return MusbInvalid;
    }
    TransferType = MUSB_TRANSFER_INTERRUPT;
  }
  if (((TransferType != MUSB_TRANSFER_BULK) &&
       (TransferType != MUSB_TRANSFER_INTERRUPT)) ||
      (DeviceAddress > 127U) || (Endpoint == 0U) ||
      ((EndpointAddress & 0x70U) != 0U) ||
      ((DataLength != 0U) && (Data == NULL)) ||
      (DataToggle == NULL) || (*DataToggle > 1U) ||
      (MaximumPacketLength == 0U) ||
      (MaximumPacketLength > MUSB_HOST_DATA_FIFO_BYTES) ||
      ((DeviceSpeed != MUSB_SPEED_LOW) &&
       (DeviceSpeed != MUSB_SPEED_FULL) &&
       (DeviceSpeed != MUSB_SPEED_HIGH))) {
    return MusbInvalid;
  }
  if ((TransferType == MUSB_TRANSFER_BULK) &&
      (DeviceSpeed == MUSB_SPEED_LOW)) {
    return MusbUnsupported;
  }
  if ((TransferType == MUSB_TRANSFER_INTERRUPT) &&
      ((DataLength == 0U) || (Interval == 0U))) {
    return MusbInvalid;
  }
  if ((DeviceSpeed == MUSB_SPEED_LOW) &&
      (MaximumPacketLength > 8U)) {
    return MusbInvalid;
  }
  if ((DeviceSpeed == MUSB_SPEED_FULL) &&
      (MaximumPacketLength > 64U)) {
    return MusbInvalid;
  }

  Status = MakeDeadline(Host, TimeoutMilliseconds, &Deadline);
  if (Status != MusbOk) {
    return Status;
  }
  Status = CheckConnected(Host);
  if (Status != MusbOk) {
    return Status;
  }
  Toggle = *DataToggle;
  Actual = 0U;

  if (!Receive) {
    Status = PrepareTxEndpoint(Host, DeviceAddress, Endpoint, DeviceSpeed,
                               TransferType, MaximumPacketLength, Interval,
                               Toggle, Route);
    if (Status != MusbOk) {
      goto Error;
    }
    do {
      Status = CheckCancelled(Host);
      if (Status != MusbOk) goto Error;
      Remaining = DataLength - Actual;
      Packet = Remaining > MaximumPacketLength ?
               MaximumPacketLength : Remaining;
      Status = WriteFifo(Host, MUSB_HOST_PIO_ENDPOINT,
                         (Packet == 0U) ? NULL : Data + Actual, Packet);
      if (Status != MusbOk) {
        goto Error;
      }
      Status = CheckCancelled(Host);
      if (Status != MusbOk) goto Error;
      Status = Read16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR),
                      &PacketLength);
      if (Status != MusbOk) {
        goto Error;
      }
      Status = Write16(Host, EpReg(MUSB_HOST_PIO_ENDPOINT, REG_TXCSR),
                       (uint16_t)(PacketLength | TXCSR_H_WZC_BITS |
                                  TXCSR_TXPKTRDY));
      if (Status != MusbOk) {
        goto Error;
      }
      Status = WaitTx(Host, &Deadline);
      if (Status != MusbOk) {
        goto Error;
      }
      Actual += Packet;
      Toggle ^= 1U;
      *DataToggle = Toggle;
      Status = CheckCancelled(Host);
      if (Status != MusbOk) {
        goto Error;
      }
    } while (Actual < DataLength);
  } else {
    Status = PrepareRxEndpoint(Host, DeviceAddress, Endpoint, DeviceSpeed,
                               TransferType, MaximumPacketLength, Interval,
                               Toggle, Route);
    if (Status != MusbOk) {
      goto Error;
    }
    if (TransferType == MUSB_TRANSFER_BULK) {
      // A zero-length bulk IN still receives and acknowledges one packet.
      do {
        Status = CheckCancelled(Host);
        if (Status != MusbOk) goto Error;
        Status = RequestRxPacket(Host);
        if (Status != MusbOk) {
          goto Error;
        }
        Status = WaitRxPacket(Host, &Deadline, &PacketLength, DataToggle, 0);
        Toggle = *DataToggle;
        if (Status != MusbOk) {
          goto Error;
        }
        Remaining = DataLength - Actual;
        Packet = PacketLength;
        if ((Packet > Remaining) || (Packet > MaximumPacketLength)) {
          Status = FlushRx(Host);
          if (Status != MusbOk) {
            goto Error;
          }
          Status = (Packet > Remaining) ? MusbBufferOverflow :
                                          MusbProtocolError;
          goto Error;
        }
        Status = ReadFifo(Host, MUSB_HOST_PIO_ENDPOINT,
                          (Packet == 0U) ? NULL : Data + Actual, Packet);
        if (Status != MusbOk) {
          goto Error;
        }
        Actual += Packet;
        Status = CheckCancelled(Host);
        if (Status != MusbOk) {
          goto Error;
        }
        if (Packet < MaximumPacketLength) {
          Status = FinishRxPacket(Host, 0);
          if (Status != MusbOk) {
            goto Error;
          }
          break;
        }
        // The next loop iteration requests the packet; do not request it twice.
        Status = FinishRxPacket(Host, 0);
        if (Status != MusbOk) {
          goto Error;
        }
      } while (Actual < DataLength);
    } else if (TransferType == MUSB_TRANSFER_INTERRUPT) {
      do {
        Status = CheckCancelled(Host);
        if (Status != MusbOk) goto Error;
        Status = RequestRxPacket(Host);
        if (Status != MusbOk) {
          goto Error;
        }
        Status = WaitRxPacket(Host, &Deadline, &PacketLength, DataToggle,
                              SinglePoll);
        Toggle = *DataToggle;
        if (Status != MusbOk) {
          goto Error;
        }
        Remaining = DataLength - Actual;
        Packet = PacketLength;
        if ((Packet > Remaining) || (Packet > MaximumPacketLength)) {
          Status = MusbBufferOverflow;
          goto Error;
        }
        Status = ReadFifo(Host, MUSB_HOST_PIO_ENDPOINT,
                          (Packet == 0U) ? NULL : Data + Actual, Packet);
        if (Status != MusbOk) {
          goto Error;
        }
        Actual += Packet;
        Status = CheckCancelled(Host);
        if (Status != MusbOk) {
          goto Error;
        }
        Status = FinishRxPacket(Host, 0);
        if (Status != MusbOk) {
          goto Error;
        }
        // A short packet ends an interrupt URB; SinglePoll retains its
        // one-packet contract even when that packet fills the request.
        if ((Packet < MaximumPacketLength) || SinglePoll) {
          break;
        }
      } while (Actual < DataLength);
    }
  }

  *BytesTransferred = Actual;
  return MusbOk;

Error:
  *BytesTransferred = Actual;
  (void)ClearDataEndpointAfterError(Host, Receive);
  return Status;
}

MUSB_STATUS
MusbHostInitialize(
  MUSB_HOST *Host,
  const MUSB_HOST_IO *Io,
  const MUSB_HOST_CONFIG *Config
  )
{
  MUSB_STATUS Status;
  uint8_t DevCtl;

  if ((Host == NULL) || (Io == NULL) || (Config == NULL) ||
      (Io->Read8 == NULL) || (Io->Read16 == NULL) ||
      (Io->Read32 == NULL) || (Io->Write8 == NULL) ||
      (Io->Write16 == NULL) || (Io->Write32 == NULL) ||
      (Io->NowUs == NULL) || (Io->DelayUs == NULL) ||
      (Io->TryLock == NULL) || (Io->Unlock == NULL)) {
    return MusbInvalid;
  }
  if ((Config->Revision != MUSB_HOST_CORE_REVISION) ||
      ((Config->Flags != MUSB_READY_REQUIRED) &&
       (Config->Flags != MUSB_READY_ID_LOW_REQUIRED) &&
       (Config->Flags != MUSB_READY_HOST_FIRST_REQUIRED)) ||
      (Config->ControllerLength < MUSB_HOST_CONTROLLER_MIN_BYTES) ||
      (Config->PhyLength < MUSB_HOST_PHY_MIN_BYTES) ||
      (Config->InterruptVector != MUSB_HOST_GSIV) ||
      (Config->EndpointCount != MUSB_HOST_MAX_ENDPOINTS) ||
      (Config->PortCount != MUSB_HOST_PORT_COUNT) ||
      (Config->FifoProfile != MUSB_HOST_T70_FIFO_PROFILE) ||
      ((Config->InterruptMask & MUSB_HOST_CONNECT_DISCONNECT_MASK) !=
       MUSB_HOST_CONNECT_DISCONNECT_MASK) ||
      (Io->ControllerLength < Config->ControllerLength)) {
    return MusbNotReady;
  }

  Host->Io = *Io;
  Host->Config = *Config;
  Host->PortStatus = MUSB_PORT_POWER;
  Host->PortChangeStatus = 0U;
  Host->PortPowerLost = 0U;
  Host->State = MUSB_HOST_STATE_HALTED;
  Host->Initialized = 0U;

  Status = Read8(Host, REG_DEVCTL, &DevCtl);
  if (Status != MusbOk) {
    return Status;
  }
  /* Require host/session, VBUS-valid and A-device. LSDEV/FSDEV bits are
   * intentionally outside this mask and remain part of the live speed state. */
  if ((DevCtl & DEVCTL_HOST_READY_MASK) != DEVCTL_HOST_READY) {
    return MusbNotReady;
  }
  Host->Initialized = 1U;
  return MusbOk;
}


MUSB_STATUS
MusbHostAdoptPortState(MUSB_HOST *Host, uint16_t PortStatus, uint16_t ChangeStatus)
{
  MUSB_STATUS Status;
  uint8_t DevCtl;
  const uint16_t Allowed = MUSB_PORT_CONNECTION | MUSB_PORT_ENABLE |
    MUSB_PORT_POWER | MUSB_PORT_LOW_SPEED | MUSB_PORT_HIGH_SPEED;

  if ((PortStatus & (uint16_t)~Allowed) != 0U ||
      (PortStatus & MUSB_PORT_POWER) == 0U ||
      ChangeStatus > 0x1fU ||
      ((PortStatus & (MUSB_PORT_LOW_SPEED | MUSB_PORT_HIGH_SPEED)) ==
       (MUSB_PORT_LOW_SPEED | MUSB_PORT_HIGH_SPEED)) ||
      (((PortStatus & MUSB_PORT_CONNECTION) == 0U) &&
       PortStatus != MUSB_PORT_POWER)) return MusbInvalid;
  Status = TakeLock(Host);
  if (Status != MusbOk) return Status;
  if (Host->State != MUSB_HOST_STATE_HALTED) { DropLock(Host);return MusbBusy; }
  Status = ReadBusState(Host,&DevCtl);
  if (Status == MusbOk) {
    Host->PortStatus=PortStatus;
    Host->PortPowerLost=0U;
    /* A new operating system first sees this connection now. The snapshot
     * comes from the previous owner's polled port, never a guessed presence. */
    Host->PortChangeStatus=ChangeStatus;
    if ((PortStatus & MUSB_PORT_CONNECTION) != 0U)
      Host->PortChangeStatus |= MUSB_PORT_CHANGE_CONNECTION;
    /* Apply disconnect/connect events since the snapshot was published. */
    Status=PollPortEvents(Host,NULL);
  }
  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostSetState(
  MUSB_HOST *Host,
  uint8_t State
  )
{
  MUSB_STATUS Status;
  uint8_t DevCtl;

  if ((State != MUSB_HOST_STATE_HALTED) &&
      (State != MUSB_HOST_STATE_OPERATIONAL)) {
    return MusbInvalid;
  }
  Status = TakeLock(Host);
  if (Status != MusbOk) {
    return Status;
  }

  if (State == MUSB_HOST_STATE_OPERATIONAL) {
    Status = ReadBusState(Host, &DevCtl);
    if (Status == MusbOk) {
      Host->State = State;
    }
  } else {
    Host->State = State;
    Status = MusbOk;
  }
  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostMarkVbusLost(MUSB_HOST *Host)
{
  MUSB_STATUS Status = TakeLock(Host);
  if (Status != MusbOk) {
    return Status;
  }
  if (Host->State == MUSB_HOST_STATE_HALTED) {
    SetPortUnpowered(Host);
  } else {
    Status = MusbBusy;
  }
  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostGetPortStatus(
  MUSB_HOST *Host,
  uint8_t PortNumber,
  uint16_t *PortStatus,
  uint16_t *PortChangeStatus
  )
{
  MUSB_STATUS Status;

  if ((PortNumber != 0U) || (PortStatus == NULL) ||
      (PortChangeStatus == NULL)) {
    return MusbInvalid;
  }
  Status = TakeLock(Host);
  if (Status != MusbOk) {
    return Status;
  }

  Status = PollPortEvents(Host, NULL);
  if (Status == MusbOk) {
    *PortStatus = Host->PortStatus;
    *PortChangeStatus = Host->PortChangeStatus;
  }
  DropLock(Host);
  return Status;
}

static MUSB_STATUS
SuspendPort(MUSB_HOST *Host)
{
  MUSB_STATUS Status;
  uint8_t Power;

  Status = RequirePort(Host, MUSB_PORT_ENABLE);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Read8(Host, REG_POWER, &Power);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Power & (POWER_RESET | POWER_RESUME | POWER_SOFTCONN)) != 0U) {
    return MusbBusy;
  }
  if ((Host->PortStatus & MUSB_PORT_SUSPEND) != 0U &&
      (Power & POWER_SUSPENDM) != 0U) {
    return MusbOk;
  }

  /* Block transfers until a timed resume completes, including after a
   * failed write whose effect on the bus is unknown. */
  Host->PortStatus |= MUSB_PORT_SUSPEND;
  Status = Write8(Host, REG_POWER, (uint8_t)(Power | POWER_SUSPENDM));
  if (Status != MusbOk) {
    return Status;
  }
  Pause(Host, SUSPEND_SETTLE_US);
  Status = RequirePort(Host, MUSB_PORT_ENABLE);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Read8(Host, REG_POWER, &Power);
  if (Status != MusbOk) {
    return Status;
  }
  return (Power & (POWER_RESET | POWER_RESUME | POWER_SUSPENDM | POWER_SOFTCONN)) ==
           POWER_SUSPENDM ? MusbOk : MusbIoError;
}

static MUSB_STATUS
ResumePort(MUSB_HOST *Host, int HardwareOnly)
{
  MUSB_STATUS Status;
  MUSB_STATUS SignalStatus;
  uint8_t Power;
  const uint16_t Suspended = MUSB_PORT_ENABLE | MUSB_PORT_SUSPEND;

  Status = RequirePort(Host, MUSB_PORT_ENABLE);
  if ((Status != MusbOk) || ((Host->PortStatus & MUSB_PORT_SUSPEND) == 0U)) {
    return Status;
  }
  Status = Read8(Host, REG_POWER, &Power);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Power & (POWER_RESET | POWER_SOFTCONN)) != 0U) {
    return MusbBusy;
  }
  /* MUSB sets POWER.RESUME itself on device wake in host mode. A pending
   * status bit alone can outlive the attachment or an earlier suspend. */
  if (HardwareOnly && (Power & POWER_RESUME) == 0U) {
    return MusbOk;
  }
  SignalStatus = MusbOk;
  if ((Power & (POWER_RESUME | POWER_SUSPENDM)) != POWER_RESUME) {
    SignalStatus = Write8(Host, REG_POWER,
                          (uint8_t)((Power & ~POWER_SUSPENDM) | POWER_RESUME));
    if (SignalStatus == MusbOk) {
      SignalStatus = Read8(Host, REG_POWER, &Power);
      if ((SignalStatus == MusbOk) &&
          ((Power & (POWER_RESUME | POWER_SUSPENDM)) != POWER_RESUME)) {
        SignalStatus = MusbIoError;
      }
    }
  }

  /* USB 2.0 host resume drives the bus for at least 20 ms. A failed MMIO
   * response alone cannot tell whether the first write reached the device. */
  Pause(Host, RESUME_SIGNAL_US);
  Status = RequirePort(Host, 0U);
  if (Status != MusbOk) {
    return Status;
  }
  Status = Read8(Host, REG_POWER, &Power);
  if (Status != MusbOk) {
    return Status;
  }
  if ((Power & (POWER_RESET | POWER_SOFTCONN)) != 0U) {
    return MusbBusy;
  }
  if ((Power & POWER_RESUME) != 0U) {
    Status = Write8(Host, REG_POWER, (uint8_t)(Power & ~POWER_RESUME));
    if (Status != MusbOk) {
      return Status;
    }
    Status = Read8(Host, REG_POWER, &Power);
    if (Status != MusbOk) {
      return Status;
    }
  }
  if ((Power & (POWER_RESUME | POWER_SUSPENDM)) != 0U) {
    return MusbIoError;
  }
  if (SignalStatus != MusbOk) {
    return SignalStatus;
  }
  Pause(Host, RESUME_RECOVERY_US);
  Status = RequirePort(Host, Suspended);
  if (Status != MusbOk) {
    return Status;
  }
  Host->PortStatus &= (uint16_t)~MUSB_PORT_SUSPEND;
  Host->PortChangeStatus |= MUSB_PORT_CHANGE_SUSPEND;
  return MusbOk;
}

MUSB_STATUS
MusbHostServiceRemoteWake(MUSB_HOST *Host)
{
  MUSB_STATUS Status = TakeLock(Host);

  if (Status != MusbOk) {
    return Status;
  }
  if ((Host->Config.InterruptMask & MUSB_HOST_RESUME_MASK) == 0U) {
    Status = MusbUnsupported;
  } else if (Host->State != MUSB_HOST_STATE_OPERATIONAL) {
    Status = MusbNotReady;
  } else if ((Host->PortStatus & MUSB_PORT_SUSPEND) != 0U) {
    Status = ResumePort(Host, 1);
  }
  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostSetPortFeature(
  MUSB_HOST *Host,
  uint8_t PortNumber,
  uint8_t Feature
  )
{
  MUSB_STATUS Status;
  uint8_t DevCtl;
  uint8_t Power;
  uint8_t PowerAfter;

  if (PortNumber != 0U) {
    return MusbInvalid;
  }
  Status = TakeLock(Host);
  if (Status != MusbOk) {
    return Status;
  }

  if (Feature == MUSB_PORT_FEATURE_POWER) {
    /* The platform drives VBUS. Acknowledge power only if it is present. */
    Status = PollPortEvents(Host, NULL);
    if ((Status == MusbOk) && ((Host->PortStatus & MUSB_PORT_POWER) == 0U)) {
      Status = MusbNotReady;
    }
    goto Done;
  }
  if ((Feature != MUSB_PORT_FEATURE_RESET) &&
      (Feature != MUSB_PORT_FEATURE_SUSPEND)) {
    Status = MusbUnsupported;
    goto Done;
  }
  if (Host->State != MUSB_HOST_STATE_OPERATIONAL) {
    Status = MusbNotReady;
    goto Done;
  }
  if (Feature == MUSB_PORT_FEATURE_SUSPEND) {
    Status = SuspendPort(Host);
    goto Done;
  }
  Status = RequirePort(Host, MUSB_PORT_CONNECTION);
  if (Status != MusbOk) {
    goto Done;
  }
  if ((Host->PortStatus & (MUSB_PORT_ENABLE | MUSB_PORT_SUSPEND)) ==
      (MUSB_PORT_ENABLE | MUSB_PORT_SUSPEND)) {
    Status = ResumePort(Host, 0);
    if (Status != MusbOk) {
      goto Done;
    }
  }
  Status = Read8(Host, REG_POWER, &Power);
  if (Status != MusbOk) {
    goto Done;
  }
  if ((Host->PortStatus & MUSB_PORT_ENABLE) != 0U) {
    Host->PortChangeStatus |= MUSB_PORT_CHANGE_ENABLE;
  }
  Host->PortStatus &= (uint16_t)~(MUSB_PORT_ENABLE |
                                  MUSB_PORT_LOW_SPEED |
                                  MUSB_PORT_HIGH_SPEED |
                                  MUSB_PORT_SUSPEND);
  /* A replacement attachment can inherit stale suspend/resume bits. */
  Power &= (uint8_t)~(POWER_RESUME | POWER_SUSPENDM);
  Status = Write8(Host, REG_POWER, (uint8_t)(Power | POWER_RESET));
  if (Status != MusbOk) {
    goto Done;
  }
  Pause(Host, 50000U);

  Status = Read8(Host, REG_POWER, &PowerAfter);
  if (Status != MusbOk) {
    (void)Write8(Host, REG_POWER, (uint8_t)(Power & ~POWER_RESET));
    goto Done;
  }
  /* MMIO completion alone does not prove that the bus reset occurred. */
  if ((PowerAfter & POWER_RESET) == 0U) {
    Status = MusbIoError;
    goto Done;
  }
  Status = Write8(Host, REG_POWER,
                  (uint8_t)(PowerAfter & (uint8_t)~POWER_RESET));
  if (Status != MusbOk) {
    goto Done;
  }
  Status = Read8(Host, REG_POWER, &PowerAfter);
  if (Status != MusbOk) {
    goto Done;
  }

  if ((PowerAfter & POWER_RESET) != 0U) {
    Status = MusbIoError;
    goto Done;
  }

  Status = PollPortEvents(Host, &DevCtl);
  if (Status != MusbOk) {
    goto Done;
  }
  if ((DevCtl & 0x9DU) != 0x1DU) {
    Status = MusbNotReady;
    goto Done;
  }
  if ((Host->PortStatus & MUSB_PORT_CONNECTION) == 0U) {
    Status = MusbDisconnected;
    goto Done;
  }

  Host->PortStatus &= (uint16_t)~(MUSB_PORT_RESET |
                                  MUSB_PORT_ENABLE |
                                  MUSB_PORT_LOW_SPEED |
                                  MUSB_PORT_HIGH_SPEED);
  if ((PowerAfter & POWER_HSMODE) != 0U) {
    Host->PortStatus |= MUSB_PORT_HIGH_SPEED;
  } else if ((DevCtl & DEVCTL_LSDEV) != 0U) {
    Host->PortStatus |= MUSB_PORT_LOW_SPEED;
  }
  Host->PortPowerLost = 0U;
  Host->PortStatus |= MUSB_PORT_ENABLE;
  Host->PortChangeStatus |= MUSB_PORT_CHANGE_RESET |
                            MUSB_PORT_CHANGE_ENABLE;
  Status = MusbOk;

Done:
  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostClearPortFeature(
  MUSB_HOST *Host,
  uint8_t PortNumber,
  uint8_t Feature
  )
{
  MUSB_STATUS Status;
  uint8_t Power;

  if (PortNumber != 0U) {
    return MusbInvalid;
  }
  Status = TakeLock(Host);
  if (Status != MusbOk) {
    return Status;
  }

  switch (Feature) {
    case MUSB_PORT_FEATURE_ENABLE:
      if ((Host->PortStatus & MUSB_PORT_ENABLE) != 0U) {
        Host->PortStatus &= (uint16_t)~MUSB_PORT_ENABLE;
        Host->PortChangeStatus |= MUSB_PORT_CHANGE_ENABLE;
      }
      Status = MusbOk;
      break;
    case MUSB_PORT_FEATURE_POWER:
      /* Clearing POWER would turn off externally managed VBUS. */
      Status = MusbUnsupported;
      break;
    case MUSB_PORT_FEATURE_SUSPEND:
      Status = (Host->State == MUSB_HOST_STATE_OPERATIONAL) ?
                 ResumePort(Host, 0) : MusbNotReady;
      break;
    case MUSB_PORT_FEATURE_RESET:
      /* SetPortFeature completes reset synchronously; UsbBus still clears it. */
      Status = Read8(Host, REG_POWER, &Power);
      if ((Status == MusbOk) && ((Power & POWER_RESET) != 0U)) {
        Status = MusbIoError;
      }
      break;
    case MUSB_PORT_FEATURE_CHANGE_CONNECTION:
      Host->PortChangeStatus &= (uint16_t)~MUSB_PORT_CHANGE_CONNECTION;
      Status = MusbOk;
      break;
    case MUSB_PORT_FEATURE_CHANGE_ENABLE:
      Host->PortChangeStatus &= (uint16_t)~MUSB_PORT_CHANGE_ENABLE;
      Status = MusbOk;
      break;
    case MUSB_PORT_FEATURE_CHANGE_SUSPEND:
      Host->PortChangeStatus &= (uint16_t)~MUSB_PORT_CHANGE_SUSPEND;
      Status = MusbOk;
      break;
    case MUSB_PORT_FEATURE_CHANGE_OVERCURRENT:
      Host->PortChangeStatus &= (uint16_t)~MUSB_PORT_CHANGE_OVERCURRENT;
      Status = MusbOk;
      break;
    case MUSB_PORT_FEATURE_CHANGE_RESET:
      Host->PortChangeStatus &= (uint16_t)~MUSB_PORT_CHANGE_RESET;
      Status = MusbOk;
      break;
    default:
      Status = MusbInvalid;
      break;
  }

  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostControlTransferRouted(
  MUSB_HOST *Host,
  uint8_t DeviceAddress,
  uint8_t DeviceSpeed,
  uint8_t MaximumPacketLength,
  const uint8_t SetupPacket[8],
  void *Data,
  uint32_t DataCapacity,
  uint32_t TimeoutMilliseconds,
  const MUSB_HOST_ROUTE *Route,
  uint32_t *BytesTransferred
  )
{
  MUSB_STATUS Status;
  uint8_t DevCtl;

  if ((BytesTransferred == NULL) || (SetupPacket == NULL) ||
      (DeviceAddress > 127U)) {
    return MusbInvalid;
  }
  *BytesTransferred = 0U;
  if ((Host == NULL) || !Host->Initialized) {
    return MusbNotReady;
  }

  Status = CheckCancelled(Host);
  if (Status != MusbOk) return Status;
  Status = TakeLock(Host);
  if (Status != MusbOk) {
    return Status;
  }
  Status = CheckCancelled(Host);
  if (Status != MusbOk) {
    DropLock(Host);
    return Status;
  }
  if (Host->State != MUSB_HOST_STATE_OPERATIONAL) {
    DropLock(Host);
    return MusbNotReady;
  }
  Status = ValidateRoute(Host, DeviceSpeed, Route);
  if (Status == MusbOk) {
    Status = CheckCancelled(Host);
  }
  if (Status == MusbOk) {
    Status = ReadBusState(Host, &DevCtl);
  }
  if (Status == MusbOk) {
    Status = ControlTransferLocked(Host, DeviceAddress, DeviceSpeed,
                                   MaximumPacketLength, SetupPacket,
                                   (uint8_t *)Data, DataCapacity,
                                   TimeoutMilliseconds, Route, BytesTransferred);
  }
  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostDataTransferRouted(
  MUSB_HOST *Host,
  uint8_t TransferType,
  uint8_t DeviceAddress,
  uint8_t EndpointAddress,
  uint8_t DeviceSpeed,
  uint16_t MaximumPacketLength,
  uint8_t Interval,
  void *Data,
  uint32_t DataLength,
  uint8_t *DataToggle,
  uint32_t TimeoutMilliseconds,
  const MUSB_HOST_ROUTE *Route,
  uint32_t *BytesTransferred
  )
{
  MUSB_STATUS Status;
  uint8_t DevCtl;

  if (BytesTransferred == NULL) {
    return MusbInvalid;
  }
  *BytesTransferred = 0U;
  if ((Host == NULL) || !Host->Initialized) {
    return MusbNotReady;
  }

  Status = CheckCancelled(Host);
  if (Status != MusbOk) return Status;
  Status = TakeLock(Host);
  if (Status != MusbOk) {
    return Status;
  }
  Status = CheckCancelled(Host);
  if (Status != MusbOk) {
    DropLock(Host);
    return Status;
  }
  if (Host->State != MUSB_HOST_STATE_OPERATIONAL) {
    DropLock(Host);
    return MusbNotReady;
  }
  Status = ValidateRoute(Host, DeviceSpeed, Route);
  if (Status == MusbOk) {
    Status = CheckCancelled(Host);
  }
  if (Status == MusbOk) {
    Status = ReadBusState(Host, &DevCtl);
  }
  if (Status == MusbOk) {
    Status = DataTransferLocked(Host, TransferType, DeviceAddress,
                                EndpointAddress, DeviceSpeed,
                                MaximumPacketLength, Interval,
                                (uint8_t *)Data, DataLength, DataToggle,
                                TimeoutMilliseconds, Route, BytesTransferred);
  }
  DropLock(Host);
  return Status;
}

MUSB_STATUS
MusbHostControlTransfer(
  MUSB_HOST *Host, uint8_t DeviceAddress, uint8_t DeviceSpeed,
  uint8_t MaximumPacketLength, const uint8_t SetupPacket[8], void *Data,
  uint32_t DataCapacity, uint32_t TimeoutMilliseconds,
  uint32_t *BytesTransferred)
{
  return MusbHostControlTransferRouted(
           Host, DeviceAddress, DeviceSpeed, MaximumPacketLength, SetupPacket,
           Data, DataCapacity, TimeoutMilliseconds, NULL, BytesTransferred);
}

MUSB_STATUS
MusbHostDataTransfer(
  MUSB_HOST *Host, uint8_t TransferType, uint8_t DeviceAddress,
  uint8_t EndpointAddress, uint8_t DeviceSpeed, uint16_t MaximumPacketLength,
  uint8_t Interval, void *Data, uint32_t DataLength, uint8_t *DataToggle,
  uint32_t TimeoutMilliseconds, uint32_t *BytesTransferred)
{
  return MusbHostDataTransferRouted(
           Host, TransferType, DeviceAddress, EndpointAddress, DeviceSpeed,
           MaximumPacketLength, Interval, Data, DataLength, DataToggle,
           TimeoutMilliseconds, NULL, BytesTransferred);
}
