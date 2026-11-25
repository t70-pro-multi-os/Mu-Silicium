// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_GAUGE_CORE_H
#define MTK_GAUGE_CORE_H
#include <stdint.h>
/* Stock MT6358 current snapshot transaction only. No SOC estimator or ACPI
 * publishing. Bind only through a board-qualified shared PMIC owner. All
 * callbacks must be bounded and retain the same lease. The caller must
 * externally serialize the ENTIRE MtkGaugeReadCurrent call, including reads
 * of Core before Acquire. Not reentrant. Keep Core, IO and Context paired and
 * alive while Owned/Quarantined; never swap callbacks to recover a lease.
 * Acquire returning false must mean no lease was acquired. */
typedef enum { MtkGaugeOk, MtkGaugeInvalid, MtkGaugeBusy, MtkGaugeIoError,
               MtkGaugeTimeout, MtkGaugeQuarantined } MTK_GAUGE_RESULT;
typedef struct {
  void *Context;
  int (*Acquire)(void *);
  int (*Release)(void *);
  int (*Read16)(void *, uint16_t, uint16_t *);
  int (*Write16)(void *, uint16_t, uint16_t);
} MTK_GAUGE_IO;
typedef struct { unsigned Owned, Quarantined; } MTK_GAUGE_CORE;
/* Zero-initialize once, never reset a live/quarantined instance to retry.
 * A failed write may have taken effect; cleanup is mandatory. Cleanup failure
 * retains the exclusive lease and denies all later samples. No force reset. */
MTK_GAUGE_RESULT MtkGaugeReadCurrent(MTK_GAUGE_CORE *, const MTK_GAUGE_IO *, uint16_t *);
/* Caller already owns the exclusive IO lease. It must release the lease after
 * every result except MtkGaugeQuarantined; that result retains it fail-closed.
 * Externally serialize this call with all uses of the same Core and lease. */
MTK_GAUGE_RESULT MtkGaugeReadCurrentInLease(MTK_GAUGE_CORE *, const MTK_GAUGE_IO *, uint16_t *);
/* Returns the stock driver's integer current unit, NOT yet an ACPI mA value.
 * Calibration inputs are runtime internal values, not unscaled DT properties.
 * Does not supply defaults or silently apply T70 initial DT values. */
MTK_GAUGE_RESULT MtkGaugeConvertCurrent(uint16_t Raw, uint32_t SenseResistance,
                                      uint32_t CarTune, int32_t *StockCurrent);

typedef struct {
  uint16_t DieTemperature;
  uint16_t TemperatureReference;
  uint16_t TemperatureSlope;
  uint16_t Fuse[4];
} MTK_GAUGE_VOLTAGE_TRIM;

/* T70 stock BATADC (15-bit, 3:1) and CHIP_TEMP (12-bit, 1:1) conversions.
 * The three temperature words and four AUXADC calibration fuse words come
 * from the board's PMIC. Returns the corrected battery voltage in mV. */
MTK_GAUGE_RESULT MtkGaugeCalibrateVoltage(uint16_t BatteryAdc, uint16_t ChipAdc,
                                         const MTK_GAUGE_VOLTAGE_TRIM *Trim, uint16_t *Millivolts);
/* T70 battery thermistor with stock PMIC 12-bit, 2:1 input scaling and
 * initial current trim from its DT. The outputs are signed tenths of a degree. */
MTK_GAUGE_RESULT MtkGaugeBatteryTemperature(uint16_t ThermistorAdc, uint16_t BifAdc,
                                           uint16_t CurrentRaw, int32_t *DeciCelsius);
#endif
