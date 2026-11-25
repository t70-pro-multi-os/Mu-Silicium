/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_PMIC_PRM_H
#define T70_MALI_PMIC_PRM_H
#include "Mfg.h"

typedef struct {
    uint16_t GroupStatus;
    uint16_t TopStatus;
} MTK_PMIC_KEY_SNAPSHOT;

/* Version 2 snapshot token. The two status words are exactly the values
   returned by the captured, read-only PMIC snapshot operation. */
typedef struct {
    uint64_t Generation;
    uint16_t GroupStatus;
    uint16_t TopStatus;
} MTK_PMIC_KEY_IRQ_SNAPSHOT;

MALI_RESULT MtkMaliPmicPrmRead16(uint32_t Address,uint16_t *Value);
MALI_RESULT MtkMaliPmicPrmWrite16(uint32_t Address,uint16_t Value);
/* Returns one brokered, read-only key-state snapshot. This does not W1C the
   PMIC status register or enable an OS wake source. Output commits only after
   the PRM metadata Ack and module Unlock both succeed. */
MALI_RESULT MtkMaliPmicPrmReadKeySnapshot(MTK_PMIC_KEY_SNAPSHOT *Snapshot);

/* Version 2 fixed-operation key IRQ interface. Query returns MaliUnsupported
   when firmware does not advertise both lease and captured-ack capabilities.
   Lease/snapshot outputs are committed only after wire metadata acknowledgement
   and exact PRM Unlock. Ack returns MaliBusy for a captured snapshot that has
   sibling status bits pending; the caller snapshot remains valid for retry; lease release remains blocked until the
   retained snapshot is acknowledged. These APIs never expose arbitrary PMIC addresses or writes. */
MALI_RESULT MtkMaliPmicPrmQueryKeyIrq(void);
MALI_RESULT MtkMaliPmicPrmAcquireKeyIrqLease(uint64_t *LeaseToken);
MALI_RESULT MtkMaliPmicPrmReleaseKeyIrqLease(uint64_t LeaseToken);
MALI_RESULT MtkMaliPmicPrmReadKeyIrqSnapshot(
    uint64_t LeaseToken, MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot);
MALI_RESULT MtkMaliPmicPrmAckKeyIrqSnapshot(
    const MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot);

/* Version 5 fixed-register gauge lease. It permits the stock FG_CTRL latch
   values and the three fixed AUXADC triggers/read-only outputs below. */
MALI_RESULT MtkMaliPmicPrmAcquireGaugeLease(uint64_t *LeaseToken);
MALI_RESULT MtkMaliPmicPrmReleaseGaugeLease(uint64_t LeaseToken);
MALI_RESULT MtkMaliPmicPrmGaugeRead16(uint64_t LeaseToken,uint32_t Address,uint16_t *Value);
MALI_RESULT MtkMaliPmicPrmGaugeWrite16(uint64_t LeaseToken,uint32_t Address,uint16_t Value);

typedef struct {
    uint16_t BatteryAdcRaw;
    uint16_t ChipTemperatureAdcRaw;
    uint16_t ThermistorAdcRaw;
    uint16_t BifAdcRaw;
    uint16_t CurrentRaw;
    uint16_t BatteryVoltageMv;
    int32_t BatteryCurrentStockUnits;
    int32_t BatteryTemperatureDeciC;
} MTK_PMIC_BATTERY_SNAPSHOT;

typedef enum {
    MtkPmicBatteryAdcVoltage,
    MtkPmicBatteryAdcTemperature,
    MtkPmicBatteryAdcChipTemperature,
    MtkPmicBatteryAdcBifVoltage
} MTK_PMIC_BATTERY_ADC_CHANNEL;

/* Reads one stock AUXADC raw sample under the shared gauge/WACS lease.
   BIF conversion restores its original disable bit before lease release. Output
   changes only after a READY sample and confirmed restoration/release. */
MALI_RESULT MtkMaliPmicPrmReadBatteryAdcRaw(
    MTK_PMIC_BATTERY_ADC_CHANNEL Channel,uint16_t *Sample);
/* Stock temperature result in signed tenths of a degree Celsius. All three
   PMIC samples use one exclusive gauge lease; output commits after release. */
MALI_RESULT MtkMaliPmicPrmReadBatteryTemperatureDeciC(int32_t *DeciCelsius);
/* Stock OTP/eFuse-trimmed battery voltage in mV. Fuse state is cached only
   after verified restoration and gauge-lease release. */
MALI_RESULT MtkMaliPmicPrmReadBatteryVoltageMv(uint16_t *Millivolts);
/* Collects calibrated voltage, signed deci-Celsius and stock current units
   under one gauge lease. Current units are not ACPI mA; output commits after
   all samples and lease release succeed. */
MALI_RESULT MtkMaliPmicPrmReadBatterySnapshot(MTK_PMIC_BATTERY_SNAPSHOT *Snapshot);
/* One stock FG_CURRENT latch transaction; the result remains a raw sample. */
MALI_RESULT MtkMaliPmicPrmReadBatteryCurrentRaw(uint16_t *Raw);
/* Returns the stock gauge integer unit using T70 parser-scaled current calibration; not mA. */
MALI_RESULT MtkMaliPmicPrmReadBatteryCurrentStockUnits(int32_t *StockUnits);

#endif
