/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Portable T70 hub/mux GPIO sequencer. The shipped extcon-mtk-usb.ko
 * mtk_usb_extcon_set_hub (SHA-256 c69b1b3932e0008b847967745000e732
 * bdf8b5dad59b484c1c0ef2a607201c99, .text+0x1208) sets mux, 5V, reset high
 * in that order. It calls __const_udelay(0x00418958) between the first two
 * writes. The same constant occurs again between 5V and reset. This is
 * 1000*0x10c7. Linux v6.12 include/asm-generic/delay.h expands constant
 * udelay(n) with that multiplier; arch/arm64/lib/delay.c confirms
 * __udelay(usecs) passes usecs*0x10c7 to __const_udelay. The generic delay
 * wrapper also maps mdelay(1) to udelay(1000):
 * https://github.com/torvalds/linux/blob/v6.12/include/asm-generic/delay.h
 * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/lib/delay.c
 *
 * Stock detect_cable (.text+0x10e8) reads logical ID at +0x30; zero falls
 * through to hub-on and HOST at +0x3c/+0x40, then calls set_hub at +0xb4.
 * The stock DT GPIO flags cell is 0. This core adds a bounded two-sample
 * consistency guard, not the stock debounce policy.
 *
 * The original function ignores GPIO return values. This port propagates
 * failures, stops immediately, and quarantines uncertain partial transitions.
 * It does not select a USB role, enable PD, infer Type-C state, or publish
 * readiness.
 */
#include <Library/MtkUsbHubCore.h>

#define HUB_SIGNATURE 0x48554231U
#define GPIO_BIT(Gpio) ((uint8_t)(1U << (uint32_t)(Gpio)))
static uint8_t
CoreValid(const MTK_USB_HUB_CORE *Core)
{
  return (uint8_t)(
    (Core != NULL) &&
    (Core->Signature == HUB_SIGNATURE) &&
    (Core->State >= MTK_USB_HUB_STATE_INITIALIZED) &&
    (Core->State <= MTK_USB_HUB_STATE_QUARANTINED));
}

static MTK_USB_HUB_STATUS
CheckHostInput(MTK_USB_HUB_CORE *Core)
{
  MTK_USB_HUB_STATUS Status;
  uint8_t First;
  uint8_t Second;

  Status = Core->Io.CheckHostRole(Core->Io.Context);
  if (Status != MtkUsbHubSuccess) return Status;

  Status = Core->Io.ReadGpio(Core->Io.Context, MtkUsbHubGpioId, &First);
  if (Status != MtkUsbHubSuccess) return Status;
  Status = Core->Io.ReadGpio(Core->Io.Context, MtkUsbHubGpioId, &Second);
  if (Status != MtkUsbHubSuccess) return Status;
  if ((First > 1U) || (Second > 1U)) return MtkUsbHubInvalid;
  if (First != Second) return MtkUsbHubIdUnstable;
  /* DT flags are 0; stock detect_cable requests hub-host when logical ID is 0. */
  if (First != 0U) return MtkUsbHubNotReady;
  return MtkUsbHubSuccess;
}

static void
SetKnownOutput(MTK_USB_HUB_CORE *Core, MTK_USB_HUB_GPIO Gpio, uint8_t Value)
{
  uint8_t Bit = GPIO_BIT(Gpio);

  Core->OwnedMask |= Bit;
  Core->KnownMask |= Bit;
  if (Value != 0U) {
    Core->ValueMask |= Bit;
  } else {
    Core->ValueMask &= (uint8_t)~Bit;
  }
}

static MTK_USB_HUB_STATUS
Quarantine(MTK_USB_HUB_CORE *Core, MTK_USB_HUB_STATUS Status)
{
  Core->State = MTK_USB_HUB_STATE_QUARANTINED;
  Core->LastStatus = Status;
  return Status;
}

static MTK_USB_HUB_STATUS
WriteOutput(
  MTK_USB_HUB_CORE *Core,
  MTK_USB_HUB_GPIO Gpio,
  uint8_t Value
  )
{
  MTK_USB_HUB_STATUS Status;
  uint8_t Bit = GPIO_BIT(Gpio);

  /* PrepareHost delegated the line; retain ownership even if the write fails. */
  Core->OwnedMask |= Bit;
  Core->KnownMask &= (uint8_t)~Bit;
  Core->ValueMask &= (uint8_t)~Bit;
  Status = Core->Io.WriteGpio(Core->Io.Context, Gpio, Value);
  if (Status != MtkUsbHubSuccess) {
    /* A failed provider call may have reached hardware; this line is unknown. */
    return Quarantine(Core, Status);
  }
  SetKnownOutput(Core, Gpio, Value);
  return MtkUsbHubSuccess;
}

static MTK_USB_HUB_STATUS
DelayAndRecheck(MTK_USB_HUB_CORE *Core)
{
  MTK_USB_HUB_STATUS Status;

  Status = Core->Io.DelayUs(Core->Io.Context, MTK_USB_HUB_SETTLE_US);
  if (Status != MtkUsbHubSuccess) return Quarantine(Core, Status);
  Status = CheckHostInput(Core);
  if (Status != MtkUsbHubSuccess) return Quarantine(Core, Status);
  return MtkUsbHubSuccess;
}

MTK_USB_HUB_STATUS
MtkUsbHubCoreInitialize(
  MTK_USB_HUB_CORE *Core,
  const MTK_USB_HUB_IO *Io
  )
{
  if ((Core == NULL) || (Io == NULL)) return MtkUsbHubInvalid;
  if (Core->Signature == HUB_SIGNATURE) return MtkUsbHubBusy;
  if (Core->Signature != 0U) return MtkUsbHubInvalid;
  if ((Io->ReadGpio == NULL) || (Io->CheckHostRole == NULL) ||
      (Io->WriteGpio == NULL) || (Io->DelayUs == NULL)) {
    return MtkUsbHubNotReady;
  }

  Core->Io = *Io;
  Core->Signature = HUB_SIGNATURE;
  Core->State = MTK_USB_HUB_STATE_INITIALIZED;
  Core->HostPrepared = 0U;
  Core->OwnedMask = 0U;
  Core->KnownMask = 0U;
  Core->ValueMask = 0U;
  Core->LastStatus = MtkUsbHubSuccess;
  return MtkUsbHubSuccess;
}

MTK_USB_HUB_STATUS
MtkUsbHubCorePrepareHost(MTK_USB_HUB_CORE *Core)
{
  MTK_USB_HUB_STATUS Status;

  if (!CoreValid(Core)) return MtkUsbHubInvalid;
  if (Core->State == MTK_USB_HUB_STATE_QUARANTINED) {
    return MtkUsbHubQuarantined;
  }
  if ((Core->State != MTK_USB_HUB_STATE_INITIALIZED) &&
      (Core->State != MTK_USB_HUB_STATE_PREPARED) &&
      (Core->State != MTK_USB_HUB_STATE_OFF) &&
      (Core->State != MTK_USB_HUB_STATE_ON)) {
    return MtkUsbHubNotReady;
  }

  Status = CheckHostInput(Core);
  if (Status != MtkUsbHubSuccess) {
    Core->LastStatus = Status;
    return Status;
  }
  Core->HostPrepared = 1U;
  if (Core->State == MTK_USB_HUB_STATE_INITIALIZED) {
    Core->State = MTK_USB_HUB_STATE_PREPARED;
  }
  Core->LastStatus = MtkUsbHubSuccess;
  return MtkUsbHubSuccess;
}

MTK_USB_HUB_STATUS
MtkUsbHubCoreSetHub(MTK_USB_HUB_CORE *Core, uint8_t Enable)
{
  MTK_USB_HUB_STATUS Status;
  uint8_t WasQuarantined;

  if (!CoreValid(Core) || (Enable > 1U)) return MtkUsbHubInvalid;
  if (Core->HostPrepared == 0U) return MtkUsbHubNotReady;

  WasQuarantined = (uint8_t)(Core->State == MTK_USB_HUB_STATE_QUARANTINED);
  if (WasQuarantined && (Enable != 0U)) return MtkUsbHubQuarantined;
  if (!WasQuarantined && (Core->State == MTK_USB_HUB_STATE_ON) &&
      (Enable != 0U)) {
    Status = CheckHostInput(Core);
    Core->LastStatus = Status;
    return Status;
  }
  if (!WasQuarantined && (Core->State == MTK_USB_HUB_STATE_OFF) &&
      (Enable == 0U)) {
    Core->LastStatus = MtkUsbHubSuccess;
    return MtkUsbHubSuccess;
  }
  if (!WasQuarantined &&
      (Core->State != MTK_USB_HUB_STATE_PREPARED) &&
      (Core->State != MTK_USB_HUB_STATE_OFF) &&
      (Core->State != MTK_USB_HUB_STATE_ON)) {
    return MtkUsbHubNotReady;
  }

  if (Enable != 0U) {
    Status = CheckHostInput(Core);
    if (Status != MtkUsbHubSuccess) {
      Core->LastStatus = Status;
      return Status;
    }

    Status = WriteOutput(Core, MtkUsbHubGpioMux, 1U);
    if (Status != MtkUsbHubSuccess) return Status;
    Status = DelayAndRecheck(Core);
    if (Status != MtkUsbHubSuccess) return Status;

    Status = WriteOutput(Core, MtkUsbHubGpioUsb5v, 1U);
    if (Status != MtkUsbHubSuccess) return Status;
    Status = DelayAndRecheck(Core);
    if (Status != MtkUsbHubSuccess) return Status;

    Status = WriteOutput(Core, MtkUsbHubGpioReset, 1U);
    if (Status != MtkUsbHubSuccess) return Status;
    Core->State = MTK_USB_HUB_STATE_ON;
    Core->LastStatus = MtkUsbHubSuccess;
    return MtkUsbHubSuccess;
  }

  /* Stock off order: hub reset low, 5V low, then mux low. */
  Status = WriteOutput(Core, MtkUsbHubGpioReset, 0U);
  if (Status != MtkUsbHubSuccess) return Status;
  Status = WriteOutput(Core, MtkUsbHubGpioUsb5v, 0U);
  if (Status != MtkUsbHubSuccess) return Status;
  Status = WriteOutput(Core, MtkUsbHubGpioMux, 0U);
  if (Status != MtkUsbHubSuccess) return Status;

  Core->State = MTK_USB_HUB_STATE_OFF;
  Core->LastStatus = MtkUsbHubSuccess;
  return MtkUsbHubSuccess;
}

uint8_t
MtkUsbHubCoreIsPrepared(const MTK_USB_HUB_CORE *Core)
{
  return (uint8_t)(CoreValid(Core) && (Core->HostPrepared != 0U));
}

uint8_t
MtkUsbHubCoreIsOn(const MTK_USB_HUB_CORE *Core)
{
  return (uint8_t)(CoreValid(Core) &&
                   (Core->State == MTK_USB_HUB_STATE_ON));
}
