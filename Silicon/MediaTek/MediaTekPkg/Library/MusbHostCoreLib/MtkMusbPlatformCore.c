// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkMusbPlatformCore.h>

#define REG_INTRTX       0x002U
#define REG_INTRRX       0x004U
#define REG_INTRTXE      0x006U
#define REG_INTRRXE      0x008U
#define REG_INTRUSB      0x00AU
#define REG_INTRUSBE     0x00BU
#define REG_INDEX        0x00EU
#define REG_POWER        0x001U
#define REG_TXCSR        0x012U
#define REG_RXCSR        0x016U
#define REG_DEVCTL       0x060U
#define REG_SWRST        0x074U
#define REG_TXFIFOSZ     0x062U
#define REG_RXFIFOSZ     0x063U
#define REG_TXFIFOADD    0x064U
#define REG_RXFIFOADD    0x066U
#define REG_HSDMA_INTR   0x200U
#define REG_HSDMA_CTRL0  0x204U
#define REG_QMU_GCSR     0x300U

/* Stock HSDMA stop clears channel control at +0x204 + 16*n. Stock
 * mtk_is_qmu_enabled reads USB+0x300; low/high endpoint lanes are the
 * Tx/Rx queue enable masks corroborated by the same-chip QMU header. */

#define DEVCTL_SESSION       0x01U
#define DEVCTL_HOSTMODE      0x04U
#define DEVCTL_BDEVICE       0x80U
#define CSR0_BUSY            0x006BU
#define CSR0_DEVICE_DATAEND  0x0008U
#define CHECK_DEVICE_CONFIG  0x01U
#define CHECK_DEVICE_DATAEND 0x02U
#define TXCSR_BUSY           0x9403U
#define RXCSR_BUSY           0xE823U
/* Peripheral request configuration is retired only with all FIFOs and DMA/
 * QMU engines idle. The strict host checks retain their original masks. */
#define TXCSR_DEVICE_ACTIVE  0x004BU
#define RXCSR_DEVICE_ACTIVE  0x40B3U
#define TXCSR_DEVICE_WZC     0x00A6U
#define RXCSR_DEVICE_WZC     0x0045U
#define HSDMA_ENABLE         0x0001U
#define QMU_QUEUE_ENABLES    0x00FF00FFU
#define PHY_DTM0_FORCE_MASK  0x00BE3CF4U
#define PHY_DTM1_POWER_MASK  ((1U << 2) | (1U << 5) | (1U << 4))
#define PHY_DTM1_HOST_MASK   ((1U << 9) | (1U << 1) | (1U << 16))
#define PHY_DTM1_EXPECTED    ((1U << 2) | (1U << 5) | (1U << 9))
#define PHY_ACR1_MASK        0x00007700U
#define PHY_ACR1_EXPECTED    0x00004400U
#define PHY_ACR6_MASK        0xD01000FFU
#define PHY_ACR6_EXPECTED    0x50100075U
#define PHY_ACR6_BC11_ENABLE (1U << 23)
/* Same-chip MUSB header defines VBUS_SHIFT=3 and full-valid field=3. */
#define DEVCTL_VBUS_MASK     0x18U
#define DEVCTL_VBUS_VALID    0x18U
#define POWER_ENSUSPEND      0x01U
#define POWER_SUSPENDM       0x02U
#define POWER_RESUME          0x04U
#define POWER_RESET           0x08U
#define POWER_HSENAB           0x20U
#define POWER_SOFTCONN         0x40U
#define POWER_HOST_SET         (POWER_ENSUSPEND | POWER_HSENAB)
#define POWER_HOST_CLEAR       (POWER_SUSPENDM | POWER_SOFTCONN)
#define POWER_OWNED_MASK       (POWER_HOST_SET | POWER_HOST_CLEAR)
#define POWER_TRANSIENT_MASK  (POWER_SUSPENDM | POWER_RESUME | POWER_RESET)
#define COLD_STATE_SIGNATURE  0x4355534DU

static int
RangeOk(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Offset, uint32_t Width)
{
  return (Io != NULL) && ((uint64_t)Offset + Width <= Io->ControllerLength);
}

static MTK_MUSB_PLATFORM_STATUS
ValidateIo(const MTK_MUSB_PLATFORM_IO *Io, int NeedWrite)
{
  if ((Io == NULL) || (Io->Read8 == NULL) || (Io->Read16 == NULL) ||
      (Io->Read32 == NULL) || (Io->TryLock == NULL) ||
      (Io->Unlock == NULL) ||
      (Io->ControllerLength < REG_QMU_GCSR + sizeof(uint32_t))) {
    return MtkMusbPlatformInvalid;
  }
  if (NeedWrite && ((Io->Write8 == NULL) || (Io->Write16 == NULL))) {
    return MtkMusbPlatformUnsupported;
  }
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
Read8(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Offset, uint8_t *Value)
{
  if ((Value == NULL) || !RangeOk(Io, Offset, 1U) ||
      !Io->Read8(Io->Context, Offset, Value)) {
    return MtkMusbPlatformIoError;
  }
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
Read16(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Offset, uint16_t *Value)
{
  if ((Value == NULL) || !RangeOk(Io, Offset, 2U) ||
      !Io->Read16(Io->Context, Offset, Value)) {
    return MtkMusbPlatformIoError;
  }
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
Read32(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Offset, uint32_t *Value)
{
  if ((Value == NULL) || !RangeOk(Io, Offset, 4U) ||
      !Io->Read32(Io->Context, Offset, Value)) {
    return MtkMusbPlatformIoError;
  }
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
Write8(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Offset, uint8_t Value)
{
  if (!RangeOk(Io, Offset, 1U) || !Io->Write8(Io->Context, Offset, Value)) {
    return MtkMusbPlatformIoError;
  }
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
Write16(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Offset, uint16_t Value)
{
  if (!RangeOk(Io, Offset, 2U) || !Io->Write16(Io->Context, Offset, Value)) {
    return MtkMusbPlatformIoError;
  }
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
Enter(const MTK_MUSB_PLATFORM_IO *Io, int NeedWrite)
{
  MTK_MUSB_PLATFORM_STATUS Status;

  Status = ValidateIo(Io, NeedWrite);
  if (Status != MtkMusbPlatformSuccess) return Status;
  if (!Io->TryLock(Io->Context)) return MtkMusbPlatformBusy;
  return MtkMusbPlatformSuccess;
}

static void
Leave(const MTK_MUSB_PLATFORM_IO *Io)
{
  Io->Unlock(Io->Context);
}

static MTK_MUSB_PLATFORM_STATUS
RestoreIndex(
  const MTK_MUSB_PLATFORM_IO *Io,
  uint8_t SavedIndex,
  MTK_MUSB_PLATFORM_STATUS Status
  )
{
  if (Write8(Io, REG_INDEX, SavedIndex) != MtkMusbPlatformSuccess) {
    return MtkMusbPlatformIoError;
  }
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
IdleBlocked(MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *D, uint32_t Offset,
            uint32_t Value, uint32_t Mask, uint32_t Endpoint)
{
  if (D != NULL) {
    D->Offset = Offset; D->Value = Value; D->Mask = Mask; D->Endpoint = Endpoint;
  }
  return MtkMusbPlatformQuiescenceFailed;
}

static MTK_MUSB_PLATFORM_STATUS
CheckQuiescentLocked(
  const MTK_MUSB_PLATFORM_IO *Io,
  uint32_t RequireSessionClear,
  uint32_t AllowPendingCompletion,
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic,
  uint32_t DeviceChecks
  )
{
  const uint32_t AllowDeviceConfiguration = DeviceChecks & CHECK_DEVICE_CONFIG;
  const uint16_t Ep0Busy = (uint16_t)(CSR0_BUSY &
    ((DeviceChecks & CHECK_DEVICE_DATAEND) ? ~CSR0_DEVICE_DATAEND : 0xffffU));
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t SavedIndex;
  uint8_t InterruptEnable;
  uint8_t DeviceControl;
  uint16_t InterruptMask;
  uint16_t InterruptStatus;
  uint8_t DmaInterrupt;
  uint16_t Value16;
  uint32_t DmaControl;
  uint32_t QmuControl;
  uint32_t Endpoint;

  Status = Read8(Io, REG_INDEX, &SavedIndex);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = Read8(Io, REG_INTRUSBE, &InterruptEnable);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (!AllowDeviceConfiguration && InterruptEnable != 0U) {
    Status = IdleBlocked(Diagnostic, REG_INTRUSBE, InterruptEnable, 0xffU, 0xffU);
    goto Done;
  }
  Status = Read16(Io, REG_INTRTXE, &InterruptMask);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((!AllowDeviceConfiguration && InterruptMask != 0U) ||
      (AllowDeviceConfiguration && (InterruptMask & ~0x01ffU) != 0U)) {
    Status = IdleBlocked(Diagnostic, REG_INTRTXE, InterruptMask, 0xffffU, 0xffU);
    goto Done;
  }
  Status = Read16(Io, REG_INTRRXE, &InterruptMask);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((!AllowDeviceConfiguration && InterruptMask != 0U) ||
      (AllowDeviceConfiguration && (InterruptMask & ~0x01feU) != 0U)) {
    Status = IdleBlocked(Diagnostic, REG_INTRRXE, InterruptMask, 0xffffU, 0xffU);
    goto Done;
  }
  Status = Read16(Io, REG_INTRTX, &InterruptStatus);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (!AllowPendingCompletion && (InterruptStatus != 0U)) {
    Status = IdleBlocked(Diagnostic, REG_INTRTX, InterruptStatus, 0xffffU, 0xffU);
    goto Done;
  }
  Status = Read16(Io, REG_INTRRX, &InterruptStatus);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (!AllowPendingCompletion && (InterruptStatus != 0U)) {
    Status = IdleBlocked(Diagnostic, REG_INTRRX, InterruptStatus, 0xffffU, 0xffU);
    goto Done;
  }
  Status = Read8(Io, REG_DEVCTL, &DeviceControl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (RequireSessionClear && ((DeviceControl & DEVCTL_SESSION) != 0U)) {
    Status = IdleBlocked(Diagnostic, REG_DEVCTL, DeviceControl, DEVCTL_SESSION, 0xffU);
    goto Done;
  }

  for (Endpoint = 0U; Endpoint < MTK_MUSB_PLATFORM_ENDPOINTS; ++Endpoint) {
    Status = Write8(Io, REG_INDEX, (uint8_t)Endpoint);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = Read16(Io, REG_TXCSR, &Value16);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (Endpoint == 0U) {
      if ((Value16 & Ep0Busy) != 0U) {
        Status = IdleBlocked(Diagnostic, REG_TXCSR, Value16, Ep0Busy, Endpoint);
        goto Done;
      }
      continue;
    }
    if ((Value16 & (AllowDeviceConfiguration ? TXCSR_DEVICE_ACTIVE : TXCSR_BUSY)) != 0U) {
      Status = IdleBlocked(Diagnostic, REG_TXCSR, Value16,
                          AllowDeviceConfiguration ? TXCSR_DEVICE_ACTIVE : TXCSR_BUSY, Endpoint);
      goto Done;
    }
    Status = Read16(Io, REG_RXCSR, &Value16);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if ((Value16 & (AllowDeviceConfiguration ? RXCSR_DEVICE_ACTIVE : RXCSR_BUSY)) != 0U) {
      Status = IdleBlocked(Diagnostic, REG_RXCSR, Value16,
                          AllowDeviceConfiguration ? RXCSR_DEVICE_ACTIVE : RXCSR_BUSY, Endpoint);
      goto Done;
    }
  }

  Status = Read8(Io, REG_HSDMA_INTR, &DmaInterrupt);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (!AllowPendingCompletion && (DmaInterrupt != 0U)) {
    Status = IdleBlocked(Diagnostic, REG_HSDMA_INTR, DmaInterrupt, 0xffU, 0xffU);
    goto Done;
  }
  for (Endpoint = 0U; Endpoint < 8U; ++Endpoint) {
    Status = Read16(Io, REG_HSDMA_CTRL0 + (Endpoint * 0x10U), &Value16);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    DmaControl = Value16;
    if ((DmaControl & HSDMA_ENABLE) != 0U) {
      Status = IdleBlocked(Diagnostic, REG_HSDMA_CTRL0 + (Endpoint * 0x10U), DmaControl, HSDMA_ENABLE, Endpoint);
      goto Done;
    }
  }
  Status = Read32(Io, REG_QMU_GCSR, &QmuControl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((QmuControl & QMU_QUEUE_ENABLES) != 0U) {
    Status = IdleBlocked(Diagnostic, REG_QMU_GCSR, QmuControl, QMU_QUEUE_ENABLES, 0xffU);
  }

Done:
  return RestoreIndex(Io, SavedIndex, Status);
}

static MTK_MUSB_PLATFORM_STATUS
VerifyFifoLocked(const MTK_MUSB_PLATFORM_IO *Io)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t SavedIndex;
  uint8_t Size;
  uint16_t Address;
  uint32_t Endpoint;
  uint32_t Cursor;
  uint32_t RxBytes;

  Status = Read8(Io, REG_INDEX, &SavedIndex);
  if (Status != MtkMusbPlatformSuccess) return Status;
  /* EP0 is fixed at 64 bytes/address 0; dynamic FIFO registers apply to EP1+. */
  Cursor = 64U;
  for (Endpoint = 1U; Endpoint <= 8U; ++Endpoint) {
    Status = Write8(Io, REG_INDEX, (uint8_t)Endpoint);
    if (Status != MtkMusbPlatformSuccess) goto Done;

    RxBytes = (Endpoint == 8U) ? 64U : 512U;

    Status = Read8(Io, REG_TXFIFOSZ, &Size);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (Size != 6U) { Status = MtkMusbPlatformProfileMismatch; goto Done; }
    Status = Read16(Io, REG_TXFIFOADD, &Address);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (Address != (uint16_t)(Cursor / 8U)) {
      Status = MtkMusbPlatformProfileMismatch;
      goto Done;
    }
    Cursor += 512U;

    Status = Read8(Io, REG_RXFIFOSZ, &Size);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (Size != ((RxBytes == 64U) ? 3U : 6U)) {
      Status = MtkMusbPlatformProfileMismatch;
      goto Done;
    }
    Status = Read16(Io, REG_RXFIFOADD, &Address);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (Address != (uint16_t)(Cursor / 8U)) {
      Status = MtkMusbPlatformProfileMismatch;
      goto Done;
    }
    Cursor += RxBytes;
  }
  if (Cursor != MTK_MUSB_PLATFORM_FIFO_TOTAL_BYTES) {
    Status = MtkMusbPlatformProfileMismatch;
  }

Done:
  return RestoreIndex(Io, SavedIndex, Status);
}

static MTK_MUSB_PLATFORM_STATUS
ProgramFifoLocked(const MTK_MUSB_PLATFORM_IO *Io)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t SavedIndex;
  uint32_t Endpoint;
  uint32_t Cursor;
  uint32_t RxBytes;

  Status = Read8(Io, REG_INDEX, &SavedIndex);
  if (Status != MtkMusbPlatformSuccess) return Status;
  /* EP0 is fixed at 64 bytes/address 0; dynamic FIFO registers apply to EP1+. */
  Cursor = 64U;

  for (Endpoint = 1U; Endpoint <= 8U; ++Endpoint) {
    Status = Write8(Io, REG_INDEX, (uint8_t)Endpoint);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = Write8(Io, REG_TXFIFOSZ, 6U);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = Write16(Io, REG_TXFIFOADD, (uint16_t)(Cursor / 8U));
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Cursor += 512U;

    RxBytes = (Endpoint == 8U) ? 64U : 512U;
    Status = Write8(Io, REG_RXFIFOSZ, (uint8_t)((RxBytes == 64U) ? 3U : 6U));
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = Write16(Io, REG_RXFIFOADD, (uint16_t)(Cursor / 8U));
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Cursor += RxBytes;
  }
  if (Cursor != MTK_MUSB_PLATFORM_FIFO_TOTAL_BYTES) {
    Status = MtkMusbPlatformProfileMismatch;
    goto Done;
  }
  Status = VerifyFifoLocked(Io);

Done:
  return RestoreIndex(Io, SavedIndex, Status);
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformCheckQuiescent(
  const MTK_MUSB_PLATFORM_IO *Io,
  uint32_t RequireSessionClear
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;

  if (RequireSessionClear > 1U) return MtkMusbPlatformInvalid;
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, RequireSessionClear, 0U, NULL, 0U);
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
CheckIdleDeviceLocked(const MTK_MUSB_PLATFORM_IO *Io,
                      MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic,
                      uint32_t AllowPendingDataEnd)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl, Power, Saved;
  uint16_t Csr;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) return Status;
  // Once a peripheral session has stopped, BDEVICE may already be clear.
  if ((Devctl & DEVCTL_HOSTMODE) != 0U ||
      ((Devctl & DEVCTL_BDEVICE) == 0U &&
       ((Devctl & DEVCTL_SESSION) != 0U || (Power & POWER_SOFTCONN) != 0U)))
    return IdleBlocked(Diagnostic, REG_DEVCTL, Devctl, 0x84U, 0xffU);
  if (Power & POWER_TRANSIENT_MASK)
    return IdleBlocked(Diagnostic, REG_POWER, Power, POWER_TRANSIENT_MASK, 0xffU);
  Status = Read8(Io, REG_INDEX, &Saved);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = Write8(Io, REG_INDEX, 0U);
  if (Status == MtkMusbPlatformSuccess) Status = Read16(Io, REG_TXCSR, &Csr);
  if (Status == MtkMusbPlatformSuccess && (Csr & 0x10U))
    Status = IdleBlocked(Diagnostic, REG_TXCSR, Csr, 0x10U, 0U);
  Status = RestoreIndex(Io, Saved, Status);
  if (Status != MtkMusbPlatformSuccess) return Status;
  return CheckQuiescentLocked(Io, 0U, 1U, Diagnostic, CHECK_DEVICE_CONFIG |
    (AllowPendingDataEnd ? CHECK_DEVICE_DATAEND : 0U));
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformCheckIdleDevice(const MTK_MUSB_PLATFORM_IO *Io,
                              MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  if (Diagnostic != NULL)
    *Diagnostic = (MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){0xffffffffU, 0U, 0U, 0xffU};
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckIdleDeviceLocked(Io, Diagnostic, 0U);
  Leave(Io);
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformCheckDeviceForDisconnect(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  if (Diagnostic != NULL)
    *Diagnostic = (MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){0xffffffffU, 0U, 0U, 0xffU};
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckIdleDeviceLocked(Io, Diagnostic, 1U);
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
RetireIdleDeviceCsrLocked(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Endpoint,
                         uint32_t Offset, uint16_t ClearMask,
                         MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic,
                         uint32_t AllowPendingDataEnd)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Saved;
  uint16_t Before, After, Wzc;
  Status = Read8(Io, REG_INDEX, &Saved);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = Write8(Io, REG_INDEX, (uint8_t)Endpoint);
  if (Status == MtkMusbPlatformSuccess) Status = Read16(Io, Offset, &Before);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((Before & ClearMask) == 0U) goto Done;
  Status = CheckIdleDeviceLocked(Io, Diagnostic, AllowPendingDataEnd);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read16(Io, Offset, &Before);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Wzc = Offset == REG_TXCSR ? TXCSR_DEVICE_WZC : RXCSR_DEVICE_WZC;
  Status = Write16(Io, Offset, (uint16_t)((Before & ~ClearMask) | Wzc));
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read16(Io, Offset, &After);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (After != (uint16_t)(Before & ~ClearMask))
    Status = IdleBlocked(Diagnostic, Offset, After, ClearMask, Endpoint);
Done:
  return RestoreIndex(Io, Saved, Status);
}

static MTK_MUSB_PLATFORM_STATUS
RetireDevice(const MTK_MUSB_PLATFORM_IO *Io,
             MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic,
             uint32_t AllowPendingDataEnd)
{
  static const uint32_t Offsets[] = {REG_INTRUSBE, REG_INTRTXE, REG_INTRRXE};
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Byte = 0U;
  uint16_t Word = 0U;
  uint32_t Value;
  unsigned I;
  if (Diagnostic != NULL)
    *Diagnostic = (MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){0xffffffffU, 0U, 0U, 0xffU};
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  for (I = 0U; I < sizeof(Offsets)/sizeof(Offsets[0]); ++I) {
    Status = CheckIdleDeviceLocked(Io, Diagnostic, AllowPendingDataEnd);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (I == 0U) { Status = Read8(Io, Offsets[I], &Byte); Value = Byte; }
    else { Status = Read16(Io, Offsets[I], &Word); Value = Word; }
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (Value == 0U) continue;
    Status = I == 0U ? Write8(Io, Offsets[I], 0U) : Write16(Io, Offsets[I], 0U);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (I == 0U) { Status = Read8(Io, Offsets[I], &Byte); Value = Byte; }
    else { Status = Read16(Io, Offsets[I], &Word); Value = Word; }
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if (Value != 0U) {
      Status = IdleBlocked(Diagnostic, Offsets[I], Value, I == 0U ? 0xffU : 0xffffU, 0xffU);
      goto Done;
    }
  }
  for (I = 1U; I < MTK_MUSB_PLATFORM_ENDPOINTS; ++I) {
    // Shipped dma_channel_abort: TX &6fff, then &6bff; RX &57ff.
    // Preserve peripheral WZC flags and never execute its DMA-channel resets.
    Status = RetireIdleDeviceCsrLocked(Io, I, REG_TXCSR, 0x9000U, Diagnostic, AllowPendingDataEnd);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = RetireIdleDeviceCsrLocked(Io, I, REG_TXCSR, 0x0400U, Diagnostic, AllowPendingDataEnd);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = RetireIdleDeviceCsrLocked(Io, I, REG_RXCSR, 0xa800U, Diagnostic, AllowPendingDataEnd);
    if (Status != MtkMusbPlatformSuccess) goto Done;
  }
  Status = CheckQuiescentLocked(Io, 0U, 1U, Diagnostic,
    AllowPendingDataEnd ? CHECK_DEVICE_DATAEND : 0U);
Done:
  Leave(Io);
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRetireIdleDevice(const MTK_MUSB_PLATFORM_IO *Io,
                              MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic)
{
  return RetireDevice(Io, Diagnostic, 0U);
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRetireDeviceForDisconnect(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic)
{
  return RetireDevice(Io, Diagnostic, 1U);
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRequestDisconnectedDeviceReset(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl, Power;
  uint16_t Reset;
  if (Diagnostic != NULL)
    *Diagnostic = (MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){0xffffffffU, 0U, 0U, 0xffU};
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckIdleDeviceLocked(Io, Diagnostic, 1U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = CheckQuiescentLocked(Io, 1U, 1U, Diagnostic, CHECK_DEVICE_DATAEND);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((Devctl & 0x85U) != DEVCTL_BDEVICE) {
    Status = IdleBlocked(Diagnostic, REG_DEVCTL, Devctl, 0x85U, 0xffU);
    goto Done;
  }
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (Power & (POWER_TRANSIENT_MASK | 0x40U)) {
    Status = IdleBlocked(Diagnostic, REG_POWER, Power,
                         POWER_TRANSIENT_MASK | 0x40U, 0xffU);
    goto Done;
  }
  Status = Read16(Io, REG_SWRST, &Reset);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (Reset & 1U) {
    Status = IdleBlocked(Diagnostic, REG_SWRST, Reset, 1U, 0xffU);
    goto Done;
  }
  // Stock musb_platform_reset uses a 16-bit RMW at +74, ORing both reset bits.
  Status = Write16(Io, REG_SWRST, (uint16_t)(Reset | 3U));
Done:
  Leave(Io);
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformAcknowledgeIdleInterrupts(const MTK_MUSB_PLATFORM_IO *Io)
{
  return MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(Io, NULL);
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint16_t Tx;
  uint16_t Rx;
  uint8_t Dma;

  if (Diagnostic != NULL) {
    *Diagnostic = (MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){0xffffffffU, 0U, 0U, 0xffU};
  }
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, 0U, 1U, Diagnostic, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read16(Io, REG_INTRRX, &Rx);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read16(Io, REG_INTRTX, &Tx);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_HSDMA_INTR, &Dma);
  if (Status != MtkMusbPlatformSuccess) goto Done;

  // Exact stock musb_hdrc.ko: mt_usb_interrupt +0x130/+0x140 write
  // observed RX/TX words to +4/+2; dma_controller_irq +0x64 writes the
  // observed byte to +0x200. No DMA channel or USB common status is changed.
  if (Rx != 0U) {
    Status = Write16(Io, REG_INTRRX, Rx);
    if (Status != MtkMusbPlatformSuccess) goto Done;
  }
  if (Tx != 0U) {
    Status = Write16(Io, REG_INTRTX, Tx);
    if (Status != MtkMusbPlatformSuccess) goto Done;
  }
  if (Dma != 0U) {
    Status = Write8(Io, REG_HSDMA_INTR, Dma);
    if (Status != MtkMusbPlatformSuccess) goto Done;
  }
  Status = CheckQuiescentLocked(Io, 0U, 0U, Diagnostic, 0U);
Done:
  Leave(Io);
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformConfigureHostFifo(const MTK_MUSB_PLATFORM_IO *Io)
{
  MTK_MUSB_PLATFORM_STATUS Status;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, 1U, 0U, NULL, 0U);
  if (Status == MtkMusbPlatformSuccess) Status = ProgramFifoLocked(Io);
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
CheckRetainedHostLocked(const MTK_MUSB_PLATFORM_IO *Io)
{
  const uint8_t Ready = DEVCTL_HOSTMODE | DEVCTL_SESSION | DEVCTL_VBUS_VALID;
  uint8_t Devctl;
  uint8_t Power;
  MTK_MUSB_PLATFORM_STATUS Status = Read8(Io, REG_DEVCTL, &Devctl);

  if (Status != MtkMusbPlatformSuccess) return Status;
  if ((Devctl & (Ready | DEVCTL_BDEVICE)) != Ready) return MtkMusbPlatformNotReady;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) return Status;
  if ((Power & (POWER_TRANSIENT_MASK | POWER_SOFTCONN)) != 0U) return MtkMusbPlatformNotReady;
  return MtkMusbPlatformSuccess;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRestoreRetainedHostFifo(const MTK_MUSB_PLATFORM_IO *Io)
{
  MTK_MUSB_PLATFORM_STATUS Status;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
  if (Status == MtkMusbPlatformSuccess) Status = CheckRetainedHostLocked(Io);
  if (Status == MtkMusbPlatformSuccess) Status = ProgramFifoLocked(Io);
  if (Status == MtkMusbPlatformSuccess) Status = CheckRetainedHostLocked(Io);
  Leave(Io);
  return Status;
}

/* The previous owner has left. Stop whatever it still had queued, as stock
 * musb_h_disable/musb_h_flush do, and install the host FIFO layout. Session,
 * VBUS, PHY and role are kept, so a powered bus stays powered. */
static MTK_MUSB_PLATFORM_STATUS
TakeOverEndpointLocked(const MTK_MUSB_PLATFORM_IO *Io, uint32_t Endpoint)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint16_t Csr;
  uint32_t Try;

  Status = Write8(Io, REG_INDEX, (uint8_t)Endpoint);
  if (Status != MtkMusbPlatformSuccess) return Status;
  if (Endpoint == 0U) {
    for (Try = 0U; Try < 10U; ++Try) {
      Status = Read16(Io, REG_TXCSR, &Csr);
      if (Status != MtkMusbPlatformSuccess) return Status;
      if ((Csr & 0x0003U) == 0U) break;
      Status = Write16(Io, REG_TXCSR, 0x0100U);
      if (Status != MtkMusbPlatformSuccess) return Status;
    }
    return Write16(Io, REG_TXCSR, 0U);
  }

  Status = Read16(Io, REG_TXCSR, &Csr);
  for (Try = 0U; (Status == MtkMusbPlatformSuccess) && (Try < 100U) &&
       ((Csr & 0x0003U) != 0U); ++Try) {
    Status = Write16(Io, REG_TXCSR, (uint16_t)((Csr & 0x2400U) | 0x0009U));
    if (Status == MtkMusbPlatformSuccess) Status = Read16(Io, REG_TXCSR, &Csr);
  }
  /* DMA mode may only change after DMA enable has been cleared. */
  if (Status == MtkMusbPlatformSuccess)
    Status = Write16(Io, REG_TXCSR, (uint16_t)(Csr & 0x2400U));
  if (Status == MtkMusbPlatformSuccess)
    Status = Write16(Io, REG_TXCSR, (uint16_t)(Csr & 0x2000U));

  if (Status == MtkMusbPlatformSuccess) Status = Read16(Io, REG_RXCSR, &Csr);
  /* Withdraw the IN request before flushing its FIFO. */
  if (Status == MtkMusbPlatformSuccess)
    Status = Write16(Io, REG_RXCSR, (uint16_t)(Csr & 0x084DU));
  for (Try = 0U; (Status == MtkMusbPlatformSuccess) && (Try < 4U); ++Try) {
    Status = Read16(Io, REG_RXCSR, &Csr);
    if ((Status != MtkMusbPlatformSuccess) || ((Csr & 0x0001U) == 0U)) break;
    Status = Write16(Io, REG_RXCSR, 0x0011U);
  }
  if (Status == MtkMusbPlatformSuccess) Status = Write16(Io, REG_RXCSR, 0U);
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformTakeOverHost(const MTK_MUSB_PLATFORM_IO *Io)
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t SavedIndex, Value8;
  uint16_t Value16;
  uint32_t Index;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = Read8(Io, REG_INDEX, &SavedIndex);
  if (Status == MtkMusbPlatformSuccess) Status = Write8(Io, REG_INTRUSBE, 0U);
  if (Status == MtkMusbPlatformSuccess) Status = Write16(Io, REG_INTRTXE, 0U);
  if (Status == MtkMusbPlatformSuccess) Status = Write16(Io, REG_INTRRXE, 0U);
  for (Index = 0U; (Status == MtkMusbPlatformSuccess) && (Index < 8U); ++Index) {
    Status = Read16(Io, REG_HSDMA_CTRL0 + Index * 0x10U, &Value16);
    if ((Status == MtkMusbPlatformSuccess) && ((Value16 & HSDMA_ENABLE) != 0U))
      Status = Write16(Io, REG_HSDMA_CTRL0 + Index * 0x10U, 0U);
  }
  for (Index = 0U; (Status == MtkMusbPlatformSuccess) && (Index <= 8U); ++Index)
    Status = TakeOverEndpointLocked(Io, Index);
  if (Status == MtkMusbPlatformSuccess) Status = Read16(Io, REG_INTRRX, &Value16);
  if ((Status == MtkMusbPlatformSuccess) && (Value16 != 0U))
    Status = Write16(Io, REG_INTRRX, Value16);
  if (Status == MtkMusbPlatformSuccess) Status = Read16(Io, REG_INTRTX, &Value16);
  if ((Status == MtkMusbPlatformSuccess) && (Value16 != 0U))
    Status = Write16(Io, REG_INTRTX, Value16);
  if (Status == MtkMusbPlatformSuccess) Status = Read8(Io, REG_HSDMA_INTR, &Value8);
  if ((Status == MtkMusbPlatformSuccess) && (Value8 != 0U))
    Status = Write8(Io, REG_HSDMA_INTR, Value8);
  if (Status == MtkMusbPlatformSuccess) Status = Read8(Io, REG_POWER, &Value8);
  if (Status == MtkMusbPlatformSuccess)
    Status = Write8(Io, REG_POWER, (uint8_t)((Value8 &
      ~(POWER_TRANSIENT_MASK | POWER_SOFTCONN)) | POWER_HOST_SET));
  if (Status == MtkMusbPlatformSuccess) {
    Status = ProgramFifoLocked(Io);
    (void)RestoreIndex(Io, SavedIndex, Status);
  }
  Leave(Io);
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformVerifyHostFifo(const MTK_MUSB_PLATFORM_IO *Io)
{
  MTK_MUSB_PLATFORM_STATUS Status;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = VerifyFifoLocked(Io);
  Leave(Io);
  return Status;
}

static int
CheckGpioEvidence(const MTK_MUSB_PLATFORM_EVIDENCE *Evidence)
{
  static const uint32_t Pins[MTK_MUSB_PLATFORM_GPIO_COUNT] = {
    MTK_MUSB_PLATFORM_GPIO_ID_PIN,
    MTK_MUSB_PLATFORM_GPIO_MUX_PIN,
    MTK_MUSB_PLATFORM_GPIO_5V_PIN,
    MTK_MUSB_PLATFORM_GPIO_RESET_PIN,
    MTK_MUSB_PLATFORM_GPIO_OTG_PIN
  };
  uint32_t Index;
  const MTK_MUSB_PLATFORM_GPIO_SAMPLE *Sample;

  for (Index = 0U; Index < MTK_MUSB_PLATFORM_GPIO_COUNT; ++Index) {
    Sample = &Evidence->Gpio[Index];
    if ((Sample->Number != Pins[Index]) || (Sample->Mode != 0U)) return 0;
    if (Index == 0U) {
      if ((Sample->Direction != MTK_MUSB_PLATFORM_GPIO_INPUT) ||
          (Sample->PhysicalInput > 1U)) return 0;
    } else if ((Sample->Direction != MTK_MUSB_PLATFORM_GPIO_OUTPUT) ||
               (Sample->PhysicalInput != 1U)) {
      return 0;
    }
  }
  return 1;
}

static int
CheckIdLowGpioEvidence(const MTK_MUSB_PLATFORM_EVIDENCE *Evidence)
{
  static const uint32_t Pins[MTK_MUSB_PLATFORM_GPIO_COUNT] = {
    MTK_MUSB_PLATFORM_GPIO_ID_PIN,
    MTK_MUSB_PLATFORM_GPIO_MUX_PIN,
    MTK_MUSB_PLATFORM_GPIO_5V_PIN,
    MTK_MUSB_PLATFORM_GPIO_RESET_PIN,
    MTK_MUSB_PLATFORM_GPIO_OTG_PIN
  };
  uint32_t Index;
  const MTK_MUSB_PLATFORM_GPIO_SAMPLE *Sample;

  for (Index = 0U; Index < MTK_MUSB_PLATFORM_GPIO_COUNT; ++Index) {
    Sample = &Evidence->Gpio[Index];
    if ((Sample->Number != Pins[Index]) || (Sample->Mode != 0U)) return 0;
    if (Index == 0U) {
      if ((Sample->Direction != MTK_MUSB_PLATFORM_GPIO_INPUT) ||
          (Sample->PhysicalInput != 0U)) return 0;
    } else if ((Sample->Direction != MTK_MUSB_PLATFORM_GPIO_OUTPUT) ||
               (Sample->PhysicalInput != ((Index == 4U) ? 0U : 1U))) {
      return 0;
    }
  }
  return 1;
}

static int
CheckTypecOnlyGpioEvidence(const MTK_MUSB_PLATFORM_EVIDENCE *Evidence)
{
  static const uint32_t Pins[MTK_MUSB_PLATFORM_GPIO_COUNT] = {
    MTK_MUSB_PLATFORM_GPIO_ID_PIN,
    MTK_MUSB_PLATFORM_GPIO_MUX_PIN,
    MTK_MUSB_PLATFORM_GPIO_5V_PIN,
    MTK_MUSB_PLATFORM_GPIO_RESET_PIN,
    MTK_MUSB_PLATFORM_GPIO_OTG_PIN
  };
  uint32_t Index;
  const MTK_MUSB_PLATFORM_GPIO_SAMPLE *Sample;

  for (Index = 0U; Index < MTK_MUSB_PLATFORM_GPIO_COUNT; ++Index) {
    Sample = &Evidence->Gpio[Index];
    if ((Sample->Number != Pins[Index]) || (Sample->Mode != 0U) ||
        (Sample->PhysicalInput > 1U)) return 0;
    if (Index == 0U) {
      // Mux/5V and the live TCPC source identify the connector.
      if (Sample->Direction != MTK_MUSB_PLATFORM_GPIO_INPUT) return 0;
    } else if ((Sample->Direction != MTK_MUSB_PLATFORM_GPIO_OUTPUT) ||
               ((Index == 1U || Index == 2U) &&
                (Sample->PhysicalInput != 0U)) ||
               ((Index == 4U) && (Sample->PhysicalInput != 1U))) {
      return 0;
    }
  }
  return 1;
}

int
MtkMusbPlatformHostPhyLinkReady(
  uint32_t Acr6,
  uint32_t Dtm0,
  uint32_t Dtm1
  )
{
  return ((Dtm0 & PHY_DTM0_FORCE_MASK) == 0U) &&
         ((Dtm1 & (PHY_DTM1_POWER_MASK | PHY_DTM1_HOST_MASK)) == PHY_DTM1_EXPECTED) &&
         ((Acr6 & (1U << 20)) != 0U) &&
         ((Acr6 & PHY_ACR6_BC11_ENABLE) == 0U);
}

int
MtkMusbPlatformHostPhyConfigured(
  uint32_t Acr1,
  uint32_t Acr6,
  uint32_t Dtm0,
  uint32_t Dtm1
  )
{
  return MtkMusbPlatformHostPhyLinkReady(Acr6, Dtm0, Dtm1) &&
         ((Acr1 & PHY_ACR1_MASK) == PHY_ACR1_EXPECTED) &&
         ((Acr6 & PHY_ACR6_MASK) == PHY_ACR6_EXPECTED);
}

static int
CheckPhyEvidence(const MTK_MUSB_PLATFORM_EVIDENCE *Evidence)
{
  if (((Evidence->PhyState != MTK_MUSB_PLATFORM_PHY_STATE_UNOBSERVED) &&
       (Evidence->PhyState != MTK_MUSB_PLATFORM_PHY_POWERED)) ||
      (Evidence->PhyPowerCleanupPending != 0U) ||
      (Evidence->PhyReadMask != 0x0FU)) return 0;
  return MtkMusbPlatformHostPhyConfigured(Evidence->PhyAcr1, Evidence->PhyAcr6,
    Evidence->PhyDtm0, Evidence->PhyDtm1);
}

int
MtkMusbPlatformHusb311SourceAttached(
  uint8_t CcStatus,
  uint8_t RoleControl
  )
{
  uint8_t Cc1State;
  uint8_t Cc2State;
  uint8_t Cc1Role;
  uint8_t Cc2Role;
  uint8_t RpValue;
  uint8_t RdCount;

  if (((CcStatus & MTK_MUSB_PLATFORM_HUSB311_CC_RESERVED_MASK) != 0U) ||
      ((RoleControl & MTK_MUSB_PLATFORM_HUSB311_ROLE_RESERVED_MASK) != 0U)) {
    return 0;
  }

  /* CC_STATUS fields are not valid while HUSB311 is looking for a connection. */
  if ((CcStatus & MTK_MUSB_PLATFORM_HUSB311_CC_LOOKING_FOR_CONNECTION) != 0U) {
    return 0;
  }

  Cc1Role = (uint8_t)(RoleControl & MTK_MUSB_PLATFORM_HUSB311_ROLE_CC1_MASK);
  Cc2Role = (uint8_t)((RoleControl & MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_MASK) >>
                      MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT);
  RpValue = (uint8_t)((RoleControl & MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_MASK) >>
                      MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_SHIFT);
  if (RpValue == MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_RESERVED) return 0;

  /* HUSB311 allows Rp/Rp or Rd/Rd as a DRP's configured starting pair. Its
   * ConnectResult bit reports the role currently presented after attachment. */
  if ((RoleControl & MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP) != 0U) {
    if (!(((Cc1Role == MTK_MUSB_PLATFORM_HUSB311_ROLE_RP) &&
           (Cc2Role == MTK_MUSB_PLATFORM_HUSB311_ROLE_RP)) ||
          ((Cc1Role == MTK_MUSB_PLATFORM_HUSB311_ROLE_RD) &&
           (Cc2Role == MTK_MUSB_PLATFORM_HUSB311_ROLE_RD)))) {
      return 0;
    }
    if ((CcStatus & MTK_MUSB_PLATFORM_HUSB311_CC_CONNECT_RESULT) != 0U) {
      return 0;
    }
  } else {
    /* A fixed source must present Rp on either receptacle orientation. The
     * stock getter's CC1-only role decode is not enough to validate this. */
    if ((Cc1Role != MTK_MUSB_PLATFORM_HUSB311_ROLE_RP) ||
        (Cc2Role != MTK_MUSB_PLATFORM_HUSB311_ROLE_RP) ||
        ((CcStatus & MTK_MUSB_PLATFORM_HUSB311_CC_CONNECT_RESULT) != 0U)) {
      return 0;
    }
  }

  /* In source presentation the fields use SRC.Open/Ra/Rd = 0/1/2. One and
   * only one Rd is a normal attached sink; reserved or dual-Rd states are
   * ambiguous for a USB data host and are rejected. */
  Cc1State = (uint8_t)((CcStatus >> MTK_MUSB_PLATFORM_HUSB311_CC1_SHIFT) &
                       MTK_MUSB_PLATFORM_HUSB311_CC_STATE_MASK);
  Cc2State = (uint8_t)((CcStatus >> MTK_MUSB_PLATFORM_HUSB311_CC2_SHIFT) &
                       MTK_MUSB_PLATFORM_HUSB311_CC_STATE_MASK);
  if ((Cc1State > MTK_MUSB_PLATFORM_HUSB311_SRC_RD) ||
      (Cc2State > MTK_MUSB_PLATFORM_HUSB311_SRC_RD)) {
    return 0;
  }
  RdCount = (uint8_t)((Cc1State == MTK_MUSB_PLATFORM_HUSB311_SRC_RD ? 1U : 0U) +
                      (Cc2State == MTK_MUSB_PLATFORM_HUSB311_SRC_RD ? 1U : 0U));
  return (RdCount == 1U) ? 1 : 0;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformEvaluateReady(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_EVIDENCE *Evidence,
  MTK_MUSB_PLATFORM_READY_RESULT *Result
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  MTK_MUSB_PLATFORM_READY_RESULT Local;
  uint32_t Index;
  uint8_t InterruptStatus;
  uint16_t InterruptEnable;
  MTK_MUSB_PLATFORM_STATUS IdleStatus;
  int GpioReady;
  int HostReady;
  uint32_t RequiredFlags;
  uint32_t HostPowerPath;

  if ((Evidence == NULL) || (Result == NULL)) return MtkMusbPlatformInvalid;
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Local = (MTK_MUSB_PLATFORM_READY_RESULT){0};
  Local.Flags = 0U;
  HostPowerPath = Evidence->HostPowerPath;
  if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC) {
    HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  }
  if ((HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE) ||
      (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY)) {
    RequiredFlags = MTK_MUSB_PLATFORM_READY_REQUIRED;
  } else if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB) {
    RequiredFlags = MTK_MUSB_PLATFORM_READY_ID_LOW_REQUIRED;
  } else if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST) {
    RequiredFlags = MTK_MUSB_PLATFORM_READY_HOST_FIRST_REQUIRED;
  } else {
    Local.MissingFlags = MTK_MUSB_PLATFORM_READY_REQUIRED;
    Local.HostPowerPath = 0U;
    Status = MtkMusbPlatformNotReady;
    goto Exit;
  }
  Local.MissingFlags = RequiredFlags;
  Local.HostPowerPath = HostPowerPath;
  /* EBS snapshot may arrive after initial live-readiness publication. */
  Local.HostPortSnapshotValid =
      (uint8_t)((Evidence->HostPortSnapshotValid == 1U) ? 1U : 0U);
  Local.HostPortStatus = Local.HostPortSnapshotValid ? Evidence->HostPortStatus : 0U;
  Local.HostPortChangeStatus =
      Local.HostPortSnapshotValid ? Evidence->HostPortChangeStatus : 0U;
  Local.DeviceControl = 0U;
  Local.UsbInterruptStatus = 0U;
  Local.Reserved = 0U;

  /* Only a snapshot produced by the stopped host core may reach ACPI/UCX.
   * Reject contradictory speed, unpowered and disconnected enabled states. */
  if (Local.HostPortSnapshotValid &&
      (((Local.HostPortStatus & ~0x0703U) != 0U) ||
       ((Local.HostPortStatus & 0x0100U) == 0U) ||
       ((Local.HostPortStatus & 0x0600U) == 0x0600U) ||
       (((Local.HostPortStatus & 1U) == 0U) &&
        (Local.HostPortStatus != 0x0100U)) ||
       ((Local.HostPortChangeStatus & ~0x001FU) != 0U))) {
    Status = MtkMusbPlatformNotReady;
    goto Exit;
  }

  if ((Evidence->ControllerMapped != 1U) ||
      (Evidence->ControllerBase != MTK_MUSB_PLATFORM_CONTROLLER_BASE) ||
      (Evidence->ControllerLength < MTK_MUSB_PLATFORM_CONTROLLER_BYTES) ||
      (Io->ControllerLength < MTK_MUSB_PLATFORM_CONTROLLER_BYTES)) {
    Status = MtkMusbPlatformNotReady;
    goto Exit;
  }

  if ((Evidence->ControllerMapped == 1U) &&
      (Evidence->PhyMapped == 1U) &&
      (Evidence->ControllerBase == MTK_MUSB_PLATFORM_CONTROLLER_BASE) &&
      (Evidence->ControllerLength >= MTK_MUSB_PLATFORM_CONTROLLER_BYTES) &&
      (Evidence->PhyBase == MTK_MUSB_PLATFORM_PHY_BASE) &&
      (Evidence->PhyLength >= MTK_MUSB_PLATFORM_PHY_BYTES) &&
      (Io->ControllerLength >= MTK_MUSB_PLATFORM_CONTROLLER_BYTES)) {
    Local.Flags |= MTK_MUSB_PLATFORM_READY_MMIO;
    Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_MMIO;
  }

  if ((Evidence->ClockReadMask == MTK_MUSB_PLATFORM_CLOCK_MASK) &&
      ((Evidence->ClockEnabledMask & MTK_MUSB_PLATFORM_CLOCK_MASK) ==
       MTK_MUSB_PLATFORM_CLOCK_MASK) &&
      (Evidence->RefClockEnabled == 1U) &&
      (Evidence->RefClockHz == 26000000ULL) &&
      (Evidence->TopUsbParent == Evidence->ExpectedTopUsbParent) &&
      (Evidence->TopUsbFrequencyHz != 0U) &&
      (Evidence->TopUsbFrequencyHz == Evidence->ExpectedTopUsbFrequencyHz)) {
    for (Index = 0U; Index < 3U; ++Index) {
      if ((Evidence->ClockHz[Index] == 0U) ||
          (Evidence->ExpectedClockHz[Index] == 0U) ||
          (Evidence->ClockHz[Index] != Evidence->ExpectedClockHz[Index])) break;
    }
    if (Index == 3U) {
      Local.Flags |= MTK_MUSB_PLATFORM_READY_CLOCKS;
      Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_CLOCKS;
    }
  }

  if ((Evidence->PhyMapped == 1U) &&
      (Evidence->PhyBase == MTK_MUSB_PLATFORM_PHY_BASE) &&
      (Evidence->PhyLength >= MTK_MUSB_PLATFORM_PHY_BYTES) &&
      CheckPhyEvidence(Evidence)) {
    Local.Flags |= MTK_MUSB_PLATFORM_READY_PHY;
    Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_PHY;
  }

  if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB) {
    GpioReady = CheckIdLowGpioEvidence(Evidence);
  } else if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST) {
    GpioReady = 1;
  } else if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY) {
    GpioReady = CheckTypecOnlyGpioEvidence(Evidence);
  } else {
    GpioReady = CheckGpioEvidence(Evidence);
  }
  Status = Read8(Io, REG_DEVCTL, &Local.DeviceControl);
  if (Status != MtkMusbPlatformSuccess) goto Exit;
  IdleStatus = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
  if (IdleStatus == MtkMusbPlatformIoError) {
    Status = IdleStatus;
    goto Exit;
  }
  HostReady = GpioReady &&
      ((Local.DeviceControl & (DEVCTL_HOSTMODE | DEVCTL_SESSION | DEVCTL_BDEVICE)) ==
       (DEVCTL_HOSTMODE | DEVCTL_SESSION)) &&
      (IdleStatus == MtkMusbPlatformSuccess);
  if (HostReady) {
    Local.Flags |= MTK_MUSB_PLATFORM_READY_HOST;
    Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_HOST;
  }

  /* Source proof requires a live Type-C source attachment, the stock charger
   * boost-field readback, TCPC VBUS-present, host ID/boost GPIO levels, and
   * MUSB VBUS-valid status together. */
  /* HUSB311 VBUS-present is usable only when its detector is enabled and its
   * initialization flag is clear. */
  if (((HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE) ||
       (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY)) &&
      HostReady &&
      (Evidence->Sc8989xBoostReadValid == 1U) &&
      (Evidence->Sc8989xBoostEnabled == MTK_MUSB_PLATFORM_SC8989X_BOOST_ENABLED) &&
      (Evidence->Husb311PowerStatusReadValid == 1U) &&
      (Evidence->Husb311CcStatusReadValid == 1U) &&
      (Evidence->Husb311CcStatus <= 0xFFU) &&
      (Evidence->Husb311RoleControl <= 0xFFU) &&
      MtkMusbPlatformHusb311SourceAttached(
        (uint8_t)Evidence->Husb311CcStatus,
        (uint8_t)Evidence->Husb311RoleControl) &&
      ((Evidence->Husb311PowerStatus &
        (MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING |
         MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
         MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)) ==
        (MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
         MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)) &&
      ((Local.DeviceControl & DEVCTL_VBUS_MASK) == DEVCTL_VBUS_VALID)) {
    Local.Flags |= MTK_MUSB_PLATFORM_READY_VBUS;
    Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_VBUS;
  } else if ((HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB) &&
             HostReady && (Evidence->IdLowRoleToken != 0U) &&
             (Evidence->Sc8989xBoostReadValid == 1U) &&
             (Evidence->Sc8989xBoostEnabled == 0U) &&
             ((Local.DeviceControl & DEVCTL_VBUS_MASK) == DEVCTL_VBUS_VALID)) {
    Local.Flags |= MTK_MUSB_PLATFORM_READY_ID_LOW_HUB;
    Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_ID_LOW_HUB;
  }

  Status = VerifyFifoLocked(Io);
  if (Status == MtkMusbPlatformSuccess) {
    Local.Flags |= MTK_MUSB_PLATFORM_READY_FIFO;
    Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_FIFO;
  } else if (Status == MtkMusbPlatformIoError) {
    goto Exit;
  }

  Status = Read8(Io, REG_INTRUSBE, &InterruptStatus);
  if (Status != MtkMusbPlatformSuccess) goto Exit;
  if ((InterruptStatus == 0U) && HostReady &&
      (Evidence->InterruptVector == MTK_MUSB_PLATFORM_GSIV) &&
      (Evidence->SupportedEventMask == MTK_MUSB_PLATFORM_EVENTS)) {
    Status = Read16(Io, REG_INTRTXE, &InterruptEnable);
    if (Status != MtkMusbPlatformSuccess) goto Exit;
    if (InterruptEnable == 0U) {
      Status = Read16(Io, REG_INTRRXE, &InterruptEnable);
      if (Status != MtkMusbPlatformSuccess) goto Exit;
      if (InterruptEnable == 0U) {
        Status = Read8(Io, REG_INTRUSB, &Local.UsbInterruptStatus);
        if (Status != MtkMusbPlatformSuccess) goto Exit;
        Local.Flags |= MTK_MUSB_PLATFORM_READY_EVENTS;
        Local.MissingFlags &= ~MTK_MUSB_PLATFORM_READY_EVENTS;
      }
    }
  }
  Status = MtkMusbPlatformSuccess;
  if (Local.MissingFlags != 0U) Status = MtkMusbPlatformNotReady;

Exit:
  Leave(Io);
  *Result = Local;
  return Status;
}

/* This separate entry preserves an already active host session. It is usable
 * only before publishing the controller, with fresh board power/PHY evidence.
 * No role, DEVCTL, reset, interrupt-enable or DMA register is written here. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformConfigureInheritedHostFifo(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_EVIDENCE *Evidence
  )
{
  MTK_MUSB_PLATFORM_READY_RESULT Result = {0};
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl;
  Status = MtkMusbPlatformEvaluateReady(Io, Evidence, &Result);
  if (Status == MtkMusbPlatformSuccess) return Status;
  if (Status != MtkMusbPlatformNotReady) return Status;
  if (Result.MissingFlags != MTK_MUSB_PLATFORM_READY_FIFO ||
      Result.Flags != ((Result.HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB)
        ? (MTK_MUSB_PLATFORM_READY_ID_LOW_REQUIRED & ~MTK_MUSB_PLATFORM_READY_FIFO)
        : (Result.HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST)
        ? (MTK_MUSB_PLATFORM_READY_HOST_FIRST_REQUIRED & ~MTK_MUSB_PLATFORM_READY_FIFO)
        : (MTK_MUSB_PLATFORM_READY_REQUIRED & ~MTK_MUSB_PLATFORM_READY_FIFO)))
    return MtkMusbPlatformNotReady;
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status == MtkMusbPlatformSuccess &&
      (Devctl & (DEVCTL_HOSTMODE | DEVCTL_SESSION | DEVCTL_VBUS_MASK)) !=
        (DEVCTL_HOSTMODE | DEVCTL_SESSION | DEVCTL_VBUS_VALID))
    Status = MtkMusbPlatformNotReady;
  if (Status == MtkMusbPlatformSuccess) Status = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
  if (Status == MtkMusbPlatformSuccess) Status = ProgramFifoLocked(Io);
  if (Status == MtkMusbPlatformSuccess) Status = VerifyFifoLocked(Io);
  Leave(Io);
  return Status;
}

static void
ColdClearState(MTK_MUSB_PLATFORM_COLD_HOST_STATE *State)
{
  *State = (MTK_MUSB_PLATFORM_COLD_HOST_STATE){0};
}

static MTK_MUSB_PLATFORM_STATUS
ColdValidateCallbacks(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;

  Status = ValidateIo(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  if ((Callbacks == NULL) || (Callbacks->FreshGuard == NULL) ||
      (Callbacks->SetPhyMode == NULL) || (Callbacks->GetTimeUs == NULL)) {
    return MtkMusbPlatformNotReady;
  }
  return MtkMusbPlatformSuccess;
}

static int
ColdPhyEvidenceValid(
  const MTK_MUSB_PLATFORM_EVIDENCE *Evidence,
  uint32_t PhyUsbMode
  )
{
  uint32_t ExpectedHostBits;

  if ((Evidence->PhyState != MTK_MUSB_PLATFORM_PHY_POWERED) ||
      (Evidence->PhyPowerCleanupPending != 0U) ||
      (Evidence->PhyReadMask != 0x0FU) ||
      ((Evidence->PhyDtm0 & PHY_DTM0_FORCE_MASK) != 0U) ||
      ((Evidence->PhyAcr1 & PHY_ACR1_MASK) != PHY_ACR1_EXPECTED) ||
      ((Evidence->PhyAcr6 & PHY_ACR6_MASK) != PHY_ACR6_EXPECTED) ||
      ((Evidence->PhyAcr6 & PHY_ACR6_BC11_ENABLE) != 0U)) {
    return 0;
  }

  if (PhyUsbMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID) {
    return ((Evidence->PhyDtm1 & 0x00003f3fU) == 0x00003f11U) &&
           ((Evidence->PhyDtm1 & (1U << 16)) == 0U);
  }
  if ((Evidence->PhyDtm1 & PHY_DTM1_POWER_MASK) !=
      ((1U << 2) | (1U << 5))) return 0;
  if (PhyUsbMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST) {
    if ((Evidence->PhyDtm1 & 0x00003f3fU) != 0x00003f2dU) return 0;
    ExpectedHostBits = (1U << 9);
  } else if (PhyUsbMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG) {
    ExpectedHostBits = 0U;
  } else {
    return 0;
  }
  return (Evidence->PhyDtm1 & PHY_DTM1_HOST_MASK) == ExpectedHostBits;
}

static int
ColdClockEvidenceValid(
  const MTK_MUSB_PLATFORM_EVIDENCE *Evidence,
  uint32_t PhyUsbMode,
  int AllowFaultedCleanup
  )
{
  uint32_t Index;
  uint32_t AcpiFlags;

  AcpiFlags = Evidence->AcpiColdFlags;
  if ((AcpiFlags & ~MTK_MUSB_PLATFORM_ACPI_FLAGS_MASK) != 0U ||
      ((AcpiFlags & MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE) != 0U &&
       (AcpiFlags & MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID) == 0U) ||
      ((AcpiFlags & MTK_MUSB_PLATFORM_ACPI_ID_LOW_ROUTE) != 0U &&
       (AcpiFlags & MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID) == 0U)) {
    return 0;
  }

  if ((Evidence->ControllerMapped != 1U) ||
      (Evidence->ControllerBase != MTK_MUSB_PLATFORM_CONTROLLER_BASE) ||
      (Evidence->ControllerLength < MTK_MUSB_PLATFORM_CONTROLLER_BYTES) ||
      (Evidence->PhyMapped != 1U) ||
      (Evidence->PhyBase != MTK_MUSB_PLATFORM_PHY_BASE) ||
      (Evidence->PhyLength < MTK_MUSB_PLATFORM_PHY_REG_BYTES)) {
    return 0;
  }

  if ((AcpiFlags & MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID) != 0U) {
    if (Evidence->AcpiClockIdentity == 0U) return 0;
  } else {
    if ((Evidence->ClockReadMask != MTK_MUSB_PLATFORM_CLOCK_MASK) ||
        ((Evidence->ClockEnabledMask & MTK_MUSB_PLATFORM_CLOCK_MASK) !=
         MTK_MUSB_PLATFORM_CLOCK_MASK) ||
        (Evidence->RefClockEnabled != 1U) ||
        (Evidence->RefClockHz != 26000000ULL) ||
        (Evidence->TopUsbParent != Evidence->ExpectedTopUsbParent) ||
        (Evidence->TopUsbFrequencyHz == 0U) ||
        (Evidence->TopUsbFrequencyHz != Evidence->ExpectedTopUsbFrequencyHz)) {
      return 0;
    }
    for (Index = 0U; Index < 3U; ++Index) {
      if ((Evidence->ClockHz[Index] == 0U) ||
          (Evidence->ExpectedClockHz[Index] == 0U) ||
          (Evidence->ClockHz[Index] != Evidence->ExpectedClockHz[Index])) {
        return 0;
      }
    }
  }

  if (Evidence->PhyState == MTK_MUSB_PLATFORM_PHY_STATE_FAULTED) {
    return AllowFaultedCleanup &&
        (Evidence->PhyPowerCleanupPending ==
         MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING) &&
        (Evidence->PhyReadMask == 0x0FU) &&
        (PhyUsbMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN);
  }
  return (Evidence->PhyPowerCleanupPending == 0U) &&
      ColdPhyEvidenceValid(Evidence, PhyUsbMode);
}

static int
ColdSourceEvidenceValid(
  const MTK_MUSB_PLATFORM_EVIDENCE *Evidence,
  uint32_t HostPowerPath
  )
{
  uint32_t AcpiFlags = Evidence->AcpiColdFlags;
  int ContractRoute =
      (AcpiFlags & (MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
                    MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE)) ==
      (MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
       MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE);
  int GpioValid = ContractRoute ||
      ((HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY)
         ? CheckTypecOnlyGpioEvidence(Evidence)
         : ((HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE) &&
            CheckGpioEvidence(Evidence)));
  int SourcePowerValid =
      ((AcpiFlags & MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE) != 0U) ||
      ((Evidence->Sc8989xBoostReadValid == 1U) &&
       (Evidence->Sc8989xBoostEnabled == MTK_MUSB_PLATFORM_SC8989X_BOOST_ENABLED));

  return GpioValid && SourcePowerValid &&
      (Evidence->Husb311PowerStatusReadValid == 1U) &&
      (Evidence->Husb311CcStatusReadValid == 1U) &&
      (Evidence->Husb311CcStatus <= 0xFFU) &&
      (Evidence->Husb311RoleControl <= 0xFFU) &&
      MtkMusbPlatformHusb311SourceAttached(
        (uint8_t)Evidence->Husb311CcStatus,
        (uint8_t)Evidence->Husb311RoleControl) &&
      ((Evidence->Husb311PowerStatus &
        (MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING |
         MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
         MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)) ==
        (MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
         MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT));
}

static int
ColdIdLowEvidenceValid(const MTK_MUSB_PLATFORM_EVIDENCE *Evidence)
{
  static const uint32_t Pins[MTK_MUSB_PLATFORM_GPIO_COUNT] = {
    MTK_MUSB_PLATFORM_GPIO_ID_PIN,
    MTK_MUSB_PLATFORM_GPIO_MUX_PIN,
    MTK_MUSB_PLATFORM_GPIO_5V_PIN,
    MTK_MUSB_PLATFORM_GPIO_RESET_PIN,
    MTK_MUSB_PLATFORM_GPIO_OTG_PIN
  };
  uint32_t Index;

  if ((Evidence->HostPowerPath != MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB) ||
      (Evidence->IdLowRoleToken == 0U)) {
    return 0;
  }
  if ((Evidence->AcpiColdFlags &
       (MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
        MTK_MUSB_PLATFORM_ACPI_ID_LOW_ROUTE)) ==
      (MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID |
       MTK_MUSB_PLATFORM_ACPI_ID_LOW_ROUTE)) {
    return 1;
  }
  if ((Evidence->Sc8989xBoostReadValid != 1U) ||
      (Evidence->Sc8989xBoostEnabled != 0U)) {
    return 0;
  }
  for (Index = 0U; Index < MTK_MUSB_PLATFORM_GPIO_COUNT; ++Index) {
    const MTK_MUSB_PLATFORM_GPIO_SAMPLE *Sample = &Evidence->Gpio[Index];
    if ((Sample->Number != Pins[Index]) || (Sample->Mode != 0U)) return 0;
    if (Index == 0U) {
      if ((Sample->Direction != MTK_MUSB_PLATFORM_GPIO_INPUT) ||
          (Sample->PhysicalInput != 0U)) return 0;
    } else if ((Sample->Direction != MTK_MUSB_PLATFORM_GPIO_OUTPUT) ||
               (Sample->PhysicalInput != ((Index == 4U) ? 0U : 1U))) {
      return 0;
    }
  }
  return 1;
}

static MTK_MUSB_PLATFORM_STATUS
ColdGetGuard(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State,
  MTK_MUSB_PLATFORM_COLD_PHASE Phase,
  uint32_t RequireSource,
  uint32_t ExpectedPhyMode,
  MTK_MUSB_PLATFORM_COLD_EVIDENCE *Evidence
  )
{
  int StopPhase;
  int FaultedCleanup;
  uint32_t HostPowerPath;

  if (Evidence == NULL) return MtkMusbPlatformInvalid;
  State->GuardPhase = (uint32_t)Phase;
  State->GuardReason = MtkMusbPlatformColdGuardFresh;
  *Evidence = (MTK_MUSB_PLATFORM_COLD_EVIDENCE){0};
  if ((Callbacks->FreshGuard(Callbacks->Context, (uint32_t)Phase, Evidence) == 0) ||
      (Evidence->ExclusiveOwnerToken == 0U) ||
      (Evidence->SampleSequence == 0U)) {
    return MtkMusbPlatformNotReady;
  }
  State->GuardReason = MtkMusbPlatformColdGuardSequence;
  if (Evidence->SampleSequence <= State->LastGuardSequence) {
    return MtkMusbPlatformConflict;
  }
  State->GuardReason = MtkMusbPlatformColdGuardOwner;
  if (State->ExclusiveOwnerToken == 0U) {
    State->ExclusiveOwnerToken = Evidence->ExclusiveOwnerToken;
  } else if (State->ExclusiveOwnerToken != Evidence->ExclusiveOwnerToken) {
    return MtkMusbPlatformConflict;
  }
  State->LastGuardSequence = Evidence->SampleSequence;
  if ((Evidence->Platform.AcpiColdFlags &
       MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID) != 0U) {
    if (State->ClockIdentity == 0U) {
      State->ClockIdentity = Evidence->Platform.AcpiClockIdentity;
    } else if (State->ClockIdentity != Evidence->Platform.AcpiClockIdentity) {
      return MtkMusbPlatformConflict;
    }
  }

  StopPhase = (Phase == MtkMusbPlatformColdBeforeStop) ||
              (Phase == MtkMusbPlatformColdAfterStop);
  State->GuardReason = MtkMusbPlatformColdGuardPowerPath;
  HostPowerPath = Evidence->Platform.HostPowerPath;
  if (!StopPhase) {
    if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC) {
      HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
    }
    if ((HostPowerPath != MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE) &&
        (HostPowerPath != MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB) &&
        (HostPowerPath != MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST) &&
        (HostPowerPath != MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY)) {
      return MtkMusbPlatformNotReady;
    }
    if (State->HostPowerPath == 0U) {
      State->HostPowerPath = HostPowerPath;
    } else if (State->HostPowerPath != HostPowerPath) {
      return MtkMusbPlatformConflict;
    }
  } else if ((HostPowerPath != 0U) &&
             (HostPowerPath != State->HostPowerPath) &&
             !((HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC) &&
               (State->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE))) {
    return MtkMusbPlatformConflict;
  }

  FaultedCleanup =
      (Evidence->Platform.PhyState == MTK_MUSB_PLATFORM_PHY_STATE_FAULTED) &&
      (Evidence->Platform.PhyPowerCleanupPending ==
       MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING) &&
      (Evidence->PhyUsbMode == MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN);
  State->GuardReason = MtkMusbPlatformColdGuardClockPhy;
  if ((Io->ControllerLength < MTK_MUSB_PLATFORM_CONTROLLER_BYTES) ||
      !ColdClockEvidenceValid(&Evidence->Platform, Evidence->PhyUsbMode,
                              StopPhase)) {
    return MtkMusbPlatformNotReady;
  }
  State->GuardReason = MtkMusbPlatformColdGuardFaultCleanup;
  if (FaultedCleanup) {
    if (!StopPhase || ((State->PhyHostAttempted == 0U) &&
                       (State->PhyInvalidAttempted == 0U))) {
      return MtkMusbPlatformNotReady;
    }
    if (Phase == MtkMusbPlatformColdBeforeStop) {
      State->PhyFaultedCleanup = 1U;
    } else if (State->PhyFaultedCleanup == 0U) {
      return MtkMusbPlatformConflict;
    }
  } else {
    if (State->PhyFaultedCleanup != 0U) return MtkMusbPlatformConflict;
    State->GuardReason = MtkMusbPlatformColdGuardPhyMode;
    if (ExpectedPhyMode != 0U) {
      if (Evidence->PhyUsbMode != ExpectedPhyMode) return MtkMusbPlatformNotReady;
    } else if ((Evidence->PhyUsbMode != MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST) &&
               (Evidence->PhyUsbMode != MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG) &&
               (Evidence->PhyUsbMode != MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID)) {
      return MtkMusbPlatformNotReady;
    }
  }
  State->GuardReason = MtkMusbPlatformColdGuardSource;
  if ((RequireSource != 0U) &&
      (HostPowerPath != MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST)) {
    if (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB) {
      if ((Evidence->UsbDataRole != MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW) ||
          !ColdIdLowEvidenceValid(&Evidence->Platform)) {
        return MtkMusbPlatformNotReady;
      }
      State->GuardReason = MtkMusbPlatformColdGuardRoleToken;
      if (State->IdLowRoleToken == 0U) {
        State->IdLowRoleToken = Evidence->Platform.IdLowRoleToken;
      } else if (State->IdLowRoleToken != Evidence->Platform.IdLowRoleToken) {
        return MtkMusbPlatformConflict;
      }
    } else {
      if ((Evidence->UsbDataRole != MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP) ||
          (Evidence->DataRoleToken == 0U) ||
          !ColdSourceEvidenceValid(&Evidence->Platform, HostPowerPath)) {
        return MtkMusbPlatformNotReady;
      }
      State->GuardReason = MtkMusbPlatformColdGuardRoleToken;
      if (State->DataRoleToken == 0U) {
        State->DataRoleToken = Evidence->DataRoleToken;
      } else if (State->DataRoleToken != Evidence->DataRoleToken) {
        return MtkMusbPlatformConflict;
      }
    }
  } else if (!StopPhase &&
             (HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB)) {
    State->GuardReason = MtkMusbPlatformColdGuardRoleToken;
    if ((State->IdLowRoleToken == 0U) ||
        (Evidence->Platform.IdLowRoleToken != State->IdLowRoleToken)) {
      return MtkMusbPlatformConflict;
    }
  }
  State->GuardReason = MtkMusbPlatformColdGuardPassed;
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
ColdReadRegisters(
  const MTK_MUSB_PLATFORM_IO *Io,
  uint8_t *Devctl,
  uint8_t *Power
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = Read8(Io, REG_DEVCTL, Devctl);
  if (Status == MtkMusbPlatformSuccess) Status = Read8(Io, REG_POWER, Power);
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
ColdStartFailure(
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State,
  MTK_MUSB_PLATFORM_STATUS Status
  )
{
  uint32_t StartStep = State->StartStep;
  uint32_t GuardPhase = State->GuardPhase;
  uint32_t GuardReason = State->GuardReason;

  if (State->OwnedMask != 0U) {
    State->Kind = MtkMusbPlatformColdStateNeedsStop;
  } else {
    ColdClearState(State);
    State->StartStep = StartStep;
    State->GuardPhase = GuardPhase;
    State->GuardReason = GuardReason;
  }
  State->LastStatus = (uint32_t)Status;
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
ColdSetSession(
  const MTK_MUSB_PLATFORM_IO *Io,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State,
  int Enable
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl;
  uint8_t Power;
  uint8_t Written;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, 1U, 0U, NULL, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (((Devctl & DEVCTL_SESSION) != State->OriginalSession) ||
      ((Power & (Enable ? (POWER_RESUME | POWER_RESET) : POWER_TRANSIENT_MASK)) != 0U) ||
      ((Power & (Enable ? POWER_HOST_SET : POWER_OWNED_MASK)) != State->OriginalPower)) {
    Status = MtkMusbPlatformConflict;
    goto Done;
  }

  Written = (uint8_t)((Devctl & (uint8_t)~DEVCTL_SESSION) |
                      (Enable ? DEVCTL_SESSION : 0U));
  State->OwnedMask |= MTK_MUSB_PLATFORM_COLD_OWNS_SESSION;
  State->SessionAttempted = 1U;
  Status = Write8(Io, REG_DEVCTL, Written);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((Devctl & DEVCTL_SESSION) != (Enable ? DEVCTL_SESSION : 0U)) {
    Status = MtkMusbPlatformIoError;
    goto Done;
  }
  State->SessionVerified = Enable ? 1U : 0U;

Done:
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
ColdPollForHostVbus(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_COLD_EVIDENCE Evidence = {0};
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl, Power;

  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdAfterPower, 1U,
                        MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST, &Evidence);
  if (Status == MtkMusbPlatformSuccess)
    Status = ColdReadRegisters(Io, &Devctl, &Power);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  if ((Devctl & DEVCTL_SESSION) == 0U)
    return ColdStartFailure(State, MtkMusbPlatformConflict);
  if ((Power & POWER_TRANSIENT_MASK) != 0U)
    return ColdStartFailure(State, MtkMusbPlatformQuiescenceFailed);
  if ((State->PowerVerified == 0U) ||
      ((Power & POWER_OWNED_MASK) != State->ExpectedPower))
    return ColdStartFailure(State, MtkMusbPlatformConflict);
  if (((Devctl & (DEVCTL_BDEVICE | DEVCTL_HOSTMODE)) != DEVCTL_HOSTMODE) ||
      ((Devctl & DEVCTL_VBUS_MASK) != DEVCTL_VBUS_VALID)) {
    return MtkMusbPlatformPending;
  }

  State->StartStep = MtkMusbPlatformColdStartReady;
  State->Kind = MtkMusbPlatformColdStateStarted;
  State->LastStatus = MtkMusbPlatformSuccess;
  return MtkMusbPlatformSuccess;
}

static MTK_MUSB_PLATFORM_STATUS
ColdSetPower(
  const MTK_MUSB_PLATFORM_IO *Io,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl;
  uint8_t Power;
  uint8_t Written;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  /* A fresh HOST PHY selection can still report inherited peripheral
   * SUSPENDM/SOFTCONN. Stock musb_start initializes POWER before requiring
   * HOSTMODE; only this owned, idle transition may clear those two bits.
   * RESET/RESUME, lost SESSION or drift of the original enable bits block. */
  if ((State->SessionVerified == 0U) || (State->PhyHostVerified == 0U) ||
      ((Devctl & DEVCTL_SESSION) == 0U) ||
      ((Power & (POWER_RESUME | POWER_RESET)) != 0U) ||
      ((Power & POWER_HOST_SET) != State->OriginalPower)) {
    Status = MtkMusbPlatformConflict;
    goto Done;
  }

  State->ExpectedPower = POWER_HOST_SET;
  State->PowerOwnedBits = (uint8_t)((Power ^ State->ExpectedPower) & POWER_OWNED_MASK);
  /* musb_start always issues this initialization, even when the enable
   * bits already match. Equal readback does not replace the start write. */
  Written = (uint8_t)((Power & (uint8_t)~POWER_OWNED_MASK) |
                      State->ExpectedPower);
  State->OwnedMask |= MTK_MUSB_PLATFORM_COLD_OWNS_POWER;
  State->PowerAttempted = 1U;
  Status = Write8(Io, REG_POWER, Written);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((Power & POWER_OWNED_MASK) != State->ExpectedPower) {
    Status = MtkMusbPlatformConflict;
    goto Done;
  }
  State->PowerVerified = 1U;

Done:
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
ColdCheckPowerTransition(
  const MTK_MUSB_PLATFORM_COLD_HOST_STATE *State,
  uint8_t Power
  )
{
  uint8_t CurrentBits = (uint8_t)(Power & POWER_OWNED_MASK);
  uint8_t Bit;

  for (Bit = 1U; Bit != 0U; Bit = (uint8_t)(Bit << 1)) {
    if ((POWER_OWNED_MASK & Bit) != 0U) {
      uint8_t Current = (uint8_t)(CurrentBits & Bit);
      uint8_t Original = (uint8_t)(State->OriginalPower & Bit);
      uint8_t Expected = (uint8_t)(State->ExpectedPower & Bit);
      if ((Current != Original) && (Current != Expected)) {
        return MtkMusbPlatformConflict;
      }
    }
    if (Bit == 0x80U) break;
  }
  if ((State->PowerVerified != 0U) &&
      (CurrentBits != State->OriginalPower) &&
      (CurrentBits != State->ExpectedPower)) {
    return MtkMusbPlatformConflict;
  }
  return MtkMusbPlatformSuccess;
}

static void
ColdResetDiagnostic(MTK_MUSB_PLATFORM_COLD_HOST_STATE *State,
                    uint32_t Check, uint32_t Offset, uint32_t Value)
{
  if (State != NULL) {
    State->ResetCheck = Check;
    State->ResetOffset = Offset;
    State->ResetValue = Value;
  }
}

/* With IRQs masked and DMA/QMU and PIO idle, repeat stock
 * musb_platform_reset, including its halfword SWRST|3 write. Only cold
 * startup inherits the additional musb_start babble setup. */
static MTK_MUSB_PLATFORM_STATUS
ResetControllerLocked(
  const MTK_MUSB_PLATFORM_IO *Io,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Reset8;
  uint16_t Reset16;
  uint32_t Value, Index;
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC Diagnostic = {0};

  for (Index = 0U; Index < 8U; ++Index) {
    ColdResetDiagnostic(State, MtkMusbPlatformResetDmaClear, REG_HSDMA_CTRL0 + 16U * Index, 0U);
    Status = Write16(Io, REG_HSDMA_CTRL0 + 16U * Index, 0U);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = Read16(Io, REG_HSDMA_CTRL0 + 16U * Index, &Reset16);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    ColdResetDiagnostic(State, MtkMusbPlatformResetDmaClear, REG_HSDMA_CTRL0 + 16U * Index, Reset16);
    /* Direction/endpoint configuration may remain after stop; only ENABLE starts DMA. */
    if ((Reset16 & HSDMA_ENABLE) != 0U) { Status = MtkMusbPlatformConflict; goto Done; }
  }
  ColdResetDiagnostic(State, MtkMusbPlatformResetDmaConfig, REG_HSDMA_CTRL0, 0U);
  Status = Read32(Io, REG_HSDMA_CTRL0, &Value);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (!Io->Write32(Io->Context, REG_HSDMA_CTRL0, Value | 0x600U)) {
    Status = MtkMusbPlatformIoError; goto Done;
  }
  Status = Read32(Io, REG_HSDMA_CTRL0, &Value);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (!Io->Write32(Io->Context, REG_HSDMA_CTRL0, Value | 0x4000U)) {
    Status = MtkMusbPlatformIoError; goto Done;
  }
  Status = Read32(Io, REG_HSDMA_CTRL0, &Value);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  ColdResetDiagnostic(State, MtkMusbPlatformResetDmaConfig, REG_HSDMA_CTRL0, Value);
  if ((Value & 0x4601U) != 0x4600U) {
    Status = MtkMusbPlatformConflict; goto Done;
  }
  ColdResetDiagnostic(State, MtkMusbPlatformResetController, REG_SWRST, 0U);
  Status = Read16(Io, REG_SWRST, &Reset16);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  ColdResetDiagnostic(State, MtkMusbPlatformResetController, REG_SWRST, Reset16);
  Status = Write16(Io, REG_SWRST, (uint16_t)(Reset16 | 3U));
  if (Status != MtkMusbPlatformSuccess) goto Done;
  /* Match stock musb_start(): reset, then mask IRQs again. Reset may
   * restore enables or leave completion flags. Only the existing safe W1C
   * helper, called after this lock is released, may acknowledge those flags
   * after proving all endpoints, DMA channels and QMU queues are stopped. */
  ColdResetDiagnostic(State, MtkMusbPlatformResetFinalIdle, REG_INTRUSBE, 0U);
  Status = Write8(Io, REG_INTRUSBE, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  ColdResetDiagnostic(State, MtkMusbPlatformResetFinalIdle, REG_INTRTXE, 0U);
  Status = Write16(Io, REG_INTRTXE, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  ColdResetDiagnostic(State, MtkMusbPlatformResetFinalIdle, REG_INTRRXE, 0U);
  Status = Write16(Io, REG_INTRRXE, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (State != NULL) {
    Status = Read8(Io, REG_SWRST, &Reset8);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    ColdResetDiagnostic(State, MtkMusbPlatformResetBabble, REG_SWRST, Reset8);
    /* Stock keeps SOF on babble and clears BABBLE_CLR_EN (bit 6). */
    Reset8 = (uint8_t)((Reset8 | MTK_MUSB_PLATFORM_SWRST_BABBLE_SET) &
                       ~MTK_MUSB_PLATFORM_SWRST_BABBLE_CLEAR);
    Status = Write8(Io, REG_SWRST, Reset8);
    if (Status != MtkMusbPlatformSuccess) goto Done;
  }
  ColdResetDiagnostic(State, MtkMusbPlatformResetFinalIdle, 0U, 0U);
  Status = CheckQuiescentLocked(Io, 0U, 1U, &Diagnostic, 0U);
  if (Status == MtkMusbPlatformQuiescenceFailed)
    ColdResetDiagnostic(State, MtkMusbPlatformResetFinalIdle,
                        Diagnostic.Offset, Diagnostic.Value);
  if (Status == MtkMusbPlatformSuccess)
    ColdResetDiagnostic(State, MtkMusbPlatformResetComplete, 0U, 0U);
Done:
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformResetRetainedHost(const MTK_MUSB_PLATFORM_IO *Io)
{
  MTK_MUSB_PLATFORM_STATUS Status;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  if (Io->Write32 == NULL) { Status = MtkMusbPlatformUnsupported; goto Done; }
  Status = CheckRetainedHostLocked(Io);
  if (Status == MtkMusbPlatformSuccess)
    Status = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
  if (Status == MtkMusbPlatformSuccess)
    Status = CheckRetainedHostLocked(Io);
  if (Status == MtkMusbPlatformSuccess)
    Status = ResetControllerLocked(Io, NULL);
Done:
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
ColdResetController(
  const MTK_MUSB_PLATFORM_IO *Io,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl, Power;

  ColdResetDiagnostic(State, MtkMusbPlatformResetIdle, 0U, 0U);
  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  ColdResetDiagnostic(State, MtkMusbPlatformResetStartPower, REG_POWER, Power);
  if (!State->SessionVerified || !State->PhyHostVerified ||
      !State->SessionAttempted ||
      !(State->OwnedMask & MTK_MUSB_PLATFORM_COLD_OWNS_SESSION) ||
      (Power & (POWER_RESUME | POWER_RESET)) ||
      ((Power & POWER_HOST_SET) != State->OriginalPower)) {
    Status = MtkMusbPlatformConflict;
    goto Done;
  }
  /* The external board/role guard runs after selecting HOST PHY. SESSION
   * can clear during that interval. It is our already-owned writable bit,
   * not proof that another owner started a transfer. Re-arm it once under
   * the controller lock before reset, preserving every hardware status bit.
   * Do not retry a failed write/readback or synthesize HOSTMODE/BDEVICE/VBUS. */
  if (!(Devctl & DEVCTL_SESSION)) {
    ColdResetDiagnostic(State, MtkMusbPlatformResetSession, REG_DEVCTL, Devctl);
    Status = Write8(Io, REG_DEVCTL, (uint8_t)(Devctl | DEVCTL_SESSION));
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = Read8(Io, REG_DEVCTL, &Devctl);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    ColdResetDiagnostic(State, MtkMusbPlatformResetSession, REG_DEVCTL, Devctl);
    if (!(Devctl & DEVCTL_SESSION)) {
      Status = MtkMusbPlatformIoError;
      goto Done;
    }
    ColdResetDiagnostic(State, MtkMusbPlatformResetRearmIdle, 0U, 0U);
    Status = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    Status = Read8(Io, REG_POWER, &Power);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    ColdResetDiagnostic(State, MtkMusbPlatformResetRearmPower, REG_POWER, Power);
    if ((Power & (POWER_RESUME | POWER_RESET)) ||
        ((Power & POWER_HOST_SET) != State->OriginalPower)) {
      Status = MtkMusbPlatformConflict;
      goto Done;
    }
  }
  Status = ResetControllerLocked(Io, State);
Done:
  Leave(Io);
  return Status;
}

static MTK_MUSB_PLATFORM_STATUS
ColdStartWait(
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State,
  MTK_MUSB_PLATFORM_COLD_START_STEP Step
  )
{
  State->StartStep = Step;
  if (!Callbacks->GetTimeUs(Callbacks->Context, &State->WaitSinceUs))
    return ColdStartFailure(State, MtkMusbPlatformIoError);
  return MtkMusbPlatformPending;
}

static MTK_MUSB_PLATFORM_STATUS
ColdBeginHost(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_COLD_EVIDENCE Evidence = {0};
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl;
  uint8_t Power;

  ColdClearState(State);
  State->Signature = COLD_STATE_SIGNATURE;
  State->Kind = MtkMusbPlatformColdStateStarting;
  State->StartStep = MtkMusbPlatformColdStartGuard;
  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdBeforeSession, 1U,
                        MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG, &Evidence);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);

  State->StartStep = MtkMusbPlatformColdStartRegisters;
  Status = ColdReadRegisters(Io, &Devctl, &Power);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  if (((Devctl & DEVCTL_SESSION) != 0U) ||
      ((Power & (POWER_TRANSIENT_MASK | POWER_SOFTCONN)) != 0U)) {
    return ColdStartFailure(State, MtkMusbPlatformNotReady);
  }
  State->OriginalSession = (uint8_t)(Devctl & DEVCTL_SESSION);
  State->OriginalPower = (uint8_t)(Power & POWER_OWNED_MASK);
  State->ExpectedPower = POWER_HOST_SET;
  State->PowerOwnedBits = (uint8_t)((State->OriginalPower ^ POWER_HOST_SET) & POWER_OWNED_MASK);

  /* Clear only proven-idle W1C completion bits. This helper refuses busy
   * endpoint, DMA or QMU state and leaves USB common events untouched. */
  State->StartStep = MtkMusbPlatformColdStartAck;
  Status = MtkMusbPlatformAcknowledgeIdleInterrupts(Io);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  State->StartStep = MtkMusbPlatformColdStartQuiescent;
  Status = MtkMusbPlatformCheckQuiescent(Io, 1U);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  return ColdStartWait(Callbacks, State, MtkMusbPlatformColdStartSettle);
}

static MTK_MUSB_PLATFORM_STATUS
ColdPrepareHostRole(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_COLD_EVIDENCE Evidence = {0};
  MTK_MUSB_PLATFORM_STATUS Status;

  State->StartStep = MtkMusbPlatformColdStartRecheck;
  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdBeforeSession, 1U,
                        MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG, &Evidence);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);

  State->StartStep = MtkMusbPlatformColdStartSessionClear;
  Status = ColdSetSession(Io, State, 0);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  /* Stock MT6789 keeps the powered PHY in OTG mode while clearing SESSION,
   * then selects HOST.  An intermediate INVALID PHY state is not part of the
   * vendor role-switch path and prevents cold Factory entry on this board. */
  return ColdStartWait(Callbacks, State, MtkMusbPlatformColdStartPhySettle);
}

static MTK_MUSB_PLATFORM_STATUS
ColdStartPower(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_COLD_EVIDENCE Evidence = {0};
  MTK_MUSB_PLATFORM_STATUS Status;

  State->StartStep = MtkMusbPlatformColdStartRecheck;
  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdBeforeSession, 1U,
                        MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG, &Evidence);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  State->StartStep = MtkMusbPlatformColdStartSession;
  Status = ColdSetSession(Io, State, 1);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  State->StartStep = MtkMusbPlatformColdStartAfterSession;
  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdAfterSession, 1U,
                        MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG, &Evidence);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  /* Stock do_host_work selects HOST PHY then calls musb_start, whose
   * POWER initialization precedes host readiness. BDEVICE/HOSTMODE/VBUS
   * are observations: validate them after starting POWER, never write them. */
  State->StartStep = MtkMusbPlatformColdStartPhyHost;
  State->OwnedMask |= MTK_MUSB_PLATFORM_COLD_OWNS_PHY;
  State->PhyHostAttempted = 1U;
  if (Callbacks->SetPhyMode(Callbacks->Context,
                            MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST) == 0) {
    return ColdStartFailure(State, MtkMusbPlatformIoError);
  }
  State->PhyHostVerified = 1U;
  State->StartStep = MtkMusbPlatformColdStartBeforePower;
  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdBeforePower, 1U,
                        MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST, &Evidence);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  State->StartStep = MtkMusbPlatformColdStartReset;
  Status = ColdResetController(Io, State);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  State->StartStep = MtkMusbPlatformColdStartAckPower;
  Status = MtkMusbPlatformAcknowledgeIdleInterrupts(Io);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  State->StartStep = MtkMusbPlatformColdStartPower;
  Status = ColdSetPower(Io, State);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);
  State->StartStep = MtkMusbPlatformColdStartAfterPower;
  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdAfterPower, 1U,
                        MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST, &Evidence);
  if (Status != MtkMusbPlatformSuccess) return ColdStartFailure(State, Status);

  Status = ColdStartWait(Callbacks, State, MtkMusbPlatformColdStartWaitHost);
  if (Status != MtkMusbPlatformPending) return Status;
  return ColdPollForHostVbus(Io, Callbacks, State);
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformAdvanceColdHost(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint64_t NowUs, ElapsedUs;

  if (State == NULL) return MtkMusbPlatformInvalid;
  Status = ColdValidateCallbacks(Io, Callbacks);
  if (Status != MtkMusbPlatformSuccess) return Status;
  if (Io->Write32 == NULL) return MtkMusbPlatformUnsupported;
  if (State->Signature == 0U) return ColdBeginHost(Io, Callbacks, State);
  if (State->Signature != COLD_STATE_SIGNATURE) return MtkMusbPlatformInvalid;
  if (State->Kind != MtkMusbPlatformColdStateStarting) return MtkMusbPlatformBusy;
  if (!Callbacks->GetTimeUs(Callbacks->Context, &NowUs))
    return ColdStartFailure(State, MtkMusbPlatformIoError);
  ElapsedUs = NowUs - State->WaitSinceUs;

  switch (State->StartStep) {
    case MtkMusbPlatformColdStartSettle:
      if (ElapsedUs < 5000U) return MtkMusbPlatformPending;
      return ColdPrepareHostRole(Io, Callbacks, State);
    case MtkMusbPlatformColdStartPhySettle:
      if (ElapsedUs < 5000U) return MtkMusbPlatformPending;
      return ColdStartPower(Io, Callbacks, State);
    case MtkMusbPlatformColdStartWaitHost:
      /* A delayed timer notification may arrive after hardware is ready. */
      Status = ColdPollForHostVbus(Io, Callbacks, State);
      if (Status == MtkMusbPlatformPending &&
          ElapsedUs >= MTK_MUSB_PLATFORM_COLD_TIMEOUT_MS * 1000U)
        return ColdStartFailure(State, MtkMusbPlatformTimeout);
      return Status;
    default:
      return MtkMusbPlatformBusy;
  }
}

static MTK_MUSB_PLATFORM_STATUS
ColdRestoreRegisters(
  const MTK_MUSB_PLATFORM_IO *Io,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl;
  uint8_t Power;
  uint8_t Written;
  uint8_t CurrentBits;

  Status = Enter(Io, 1);
  if (Status != MtkMusbPlatformSuccess) return Status;
  Status = CheckQuiescentLocked(Io, 0U, 0U, NULL, 0U);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if ((Power & POWER_TRANSIENT_MASK) != 0U) {
    Status = MtkMusbPlatformQuiescenceFailed;
    goto Done;
  }

  if (State->SessionAttempted != 0U) {
    CurrentBits = (uint8_t)(Devctl & DEVCTL_SESSION);
    if ((CurrentBits != State->OriginalSession) &&
        (CurrentBits != DEVCTL_SESSION)) {
      Status = MtkMusbPlatformConflict;
      goto Done;
    }
  }
  if ((State->PowerAttempted != 0U) &&
      (ColdCheckPowerTransition(State, Power) != MtkMusbPlatformSuccess)) {
    Status = MtkMusbPlatformConflict;
    goto Done;
  }

  if (State->SessionAttempted != 0U) {
    CurrentBits = (uint8_t)(Devctl & DEVCTL_SESSION);
    if (CurrentBits != State->OriginalSession) {
      Written = (uint8_t)(Devctl & (uint8_t)~DEVCTL_SESSION);
      Status = Write8(Io, REG_DEVCTL, Written);
      if (Status != MtkMusbPlatformSuccess) goto Done;
      Status = Read8(Io, REG_DEVCTL, &Devctl);
      if (Status != MtkMusbPlatformSuccess) goto Done;
      if ((Devctl & DEVCTL_SESSION) != State->OriginalSession) {
        Status = MtkMusbPlatformConflict;
        goto Done;
      }
    }
  }

  if (State->PowerAttempted != 0U) {
    Status = Read8(Io, REG_POWER, &Power);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    if ((Power & POWER_TRANSIENT_MASK) != 0U) {
      Status = MtkMusbPlatformQuiescenceFailed;
      goto Done;
    }
    Status = ColdCheckPowerTransition(State, Power);
    if (Status != MtkMusbPlatformSuccess) goto Done;
    CurrentBits = (uint8_t)(Power & POWER_OWNED_MASK);
    if (CurrentBits != State->OriginalPower) {
      Written = (uint8_t)((Power & (uint8_t)~State->PowerOwnedBits) |
                          (State->OriginalPower & State->PowerOwnedBits));
      Status = Write8(Io, REG_POWER, Written);
      if (Status != MtkMusbPlatformSuccess) goto Done;
      Status = Read8(Io, REG_POWER, &Power);
      if (Status != MtkMusbPlatformSuccess) goto Done;
      if ((Power & POWER_OWNED_MASK) != State->OriginalPower) {
        Status = MtkMusbPlatformConflict;
        goto Done;
      }
    }
  }

  Status = Read8(Io, REG_DEVCTL, &Devctl);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  Status = Read8(Io, REG_POWER, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Done;
  if (((Devctl & DEVCTL_SESSION) != State->OriginalSession) ||
      ((Power & POWER_OWNED_MASK) != State->OriginalPower)) {
    Status = MtkMusbPlatformConflict;
  }

Done:
  Leave(Io);
  return Status;
}

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformStopColdHost(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  )
{
  MTK_MUSB_PLATFORM_COLD_EVIDENCE Evidence = {0};
  MTK_MUSB_PLATFORM_STATUS Status;
  uint8_t Devctl;
  uint8_t Power;

  if (State == NULL) return MtkMusbPlatformInvalid;
  if (State->Signature == 0U) return MtkMusbPlatformSuccess;
  if (State->Signature != COLD_STATE_SIGNATURE) return MtkMusbPlatformInvalid;
  if (State->Kind == MtkMusbPlatformColdStateStopping) return MtkMusbPlatformBusy;
  if (State->Kind == MtkMusbPlatformColdStateStarting && State->OwnedMask == 0U) {
    ColdClearState(State);
    return MtkMusbPlatformSuccess;
  }
  if ((State->Kind != MtkMusbPlatformColdStateStarted) &&
      (State->Kind != MtkMusbPlatformColdStateStarting) &&
      (State->Kind != MtkMusbPlatformColdStateNeedsStop)) {
    return MtkMusbPlatformInvalid;
  }
  Status = ColdValidateCallbacks(Io, Callbacks);
  if (Status != MtkMusbPlatformSuccess) return Status;
  State->Kind = MtkMusbPlatformColdStateStopping;

  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdBeforeStop, 0U, 0U, &Evidence);
  if (Status != MtkMusbPlatformSuccess) goto Failed;
  Status = ColdReadRegisters(Io, &Devctl, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Failed;
  if ((Power & POWER_TRANSIENT_MASK) != 0U) {
    Status = MtkMusbPlatformQuiescenceFailed;
    goto Failed;
  }

  /* Clear only observed, idle W1C completions. Busy DMA/QMU/endpoints block. */
  Status = MtkMusbPlatformAcknowledgeIdleInterrupts(Io);
  if (Status != MtkMusbPlatformSuccess) goto Failed;
  Status = ColdRestoreRegisters(Io, State);
  if (Status != MtkMusbPlatformSuccess) goto Failed;

  if (((State->PhyHostAttempted != 0U) || (State->PhyInvalidAttempted != 0U)) &&
      (State->PhyOtgVerified == 0U) &&
      (State->PhyFaultedCleanup == 0U)) {
    State->PhyOtgAttempted = 1U;
    if (Callbacks->SetPhyMode(Callbacks->Context,
                              MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG) == 0) {
      Status = MtkMusbPlatformIoError;
      goto Failed;
    }
    State->PhyOtgVerified = 1U;
  }
  Status = ColdGetGuard(Io, Callbacks, State,
                        MtkMusbPlatformColdAfterStop, 0U,
                        State->PhyFaultedCleanup != 0U
                          ? MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN
                          : MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG,
                        &Evidence);
  if (Status != MtkMusbPlatformSuccess) goto Failed;
  Status = MtkMusbPlatformCheckQuiescent(Io, 1U);
  if (Status != MtkMusbPlatformSuccess) goto Failed;
  Status = ColdReadRegisters(Io, &Devctl, &Power);
  if (Status != MtkMusbPlatformSuccess) goto Failed;
  if (((Devctl & DEVCTL_SESSION) != State->OriginalSession) ||
      ((Power & POWER_OWNED_MASK) != State->OriginalPower)) {
    Status = MtkMusbPlatformConflict;
    goto Failed;
  }
  ColdClearState(State);
  return MtkMusbPlatformSuccess;

Failed:
  State->Kind = MtkMusbPlatformColdStateNeedsStop;
  State->LastStatus = (uint32_t)Status;
  return Status;
}
