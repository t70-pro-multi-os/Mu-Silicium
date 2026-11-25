// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_PMIC_GAUGE_PROTOCOL_H
#define MTK_PMIC_GAUGE_PROTOCOL_H

#include <Uefi.h>

extern EFI_GUID gMediaTekPmicGaugeProtocolGuid;

typedef struct {
  // One boot-services snapshot of FG_CURRENT. Value is a raw sample.
  // A failed call leaves the output untouched.
  EFI_STATUS (EFIAPI *ReadCurrent)(OUT UINT16 *Raw);
} MTK_PMIC_GAUGE_PROTOCOL;

extern EFI_GUID gMediaTekPmicBatteryAdcProtocolGuid;

typedef enum {
  MtkPmicBatteryAdcVoltage,
  MtkPmicBatteryAdcTemperature,
  MtkPmicBatteryAdcCalibratedVoltage,
  MtkPmicBatteryAdcTemperatureDeciK
} MTK_PMIC_BATTERY_ADC_CHANNEL;

typedef struct {
  // One boot-services conversion through the PMIC AUXADC.
  // CalibratedVoltage returns mV. TemperatureDeciK returns the NTC estimate
  // in tenths of a kelvin (2732 at 0 C). Voltage and Temperature are raw codes.
  // On failure the result is left untouched.
  EFI_STATUS (EFIAPI *Read)(
    IN MTK_PMIC_BATTERY_ADC_CHANNEL Channel,
    OUT UINT16                    *Sample
    );
} MTK_PMIC_BATTERY_ADC_PROTOCOL;

#endif
