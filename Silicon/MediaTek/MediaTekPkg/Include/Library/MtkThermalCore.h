// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_THERMAL_CORE_H
#define MTK_THERMAL_CORE_H

/* T70 Pro stock thermal conversions.
 * Input is the stock IIO processed channel value, NOT an ADC register code.
 * Physical voltage scaling must be verified when a runtime provider is added.
 */
typedef struct {
  int MilliCelsius;
  int Clamped; /* -1: below table ADC range, +1: above, 0: in range. */
} MTK_THERMAL_SAMPLE;

/* Returns 0 for a null output; otherwise 1. Extreme inputs clamp as stock
 * does, but expose that fact so callers need not treat them as valid sensors.
 */
int MtkThermalApNtcConvert(int ProcessedAdc, MTK_THERMAL_SAMPLE *Sample);
#define MTK_LVTS_EFUSE_DATA1_WORDS 11U
#define MTK_LVTS_EFUSE_WORDS (MTK_LVTS_EFUSE_DATA1_WORDS + 1U)

#define MTK_LVTS_SENSOR_COUNT 13U

typedef struct {
  unsigned int Golden;
  unsigned int CountR[MTK_LVTS_SENSOR_COUNT];
} MTK_LVTS_CALIBRATION;

/* Decode the MT6789 golden temperature and sensor trims from nvmem data1 and
 * data2. A failed read leaves the output unchanged. */
int MtkThermalLvtsCalibration(const unsigned int Words[MTK_LVTS_EFUSE_WORDS],
                             MTK_LVTS_CALIBRATION *Calibration);
/* MT6789 LVTS measurement code and factory golden temperature in half-degree
 * units. Returns 0 for invalid inputs and leaves the result unchanged. */
int MtkThermalLvtsConvert(unsigned int Code, unsigned int Golden, int *MilliCelsius);
/* Rounded nearest tenth kelvin. Only table temperature range accepted;
 * invalid input/output returns 0 and leaves output unchanged. */
int MtkThermalToAcpi(int MilliCelsius, unsigned int *DeciKelvin);
#endif
