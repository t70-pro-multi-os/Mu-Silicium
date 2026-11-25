// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkThermalCore.h>

/* effective.dtb /thermal-ntc1 temperature-lookup-table, temperature column
 * -40000 + index*1000 mC. AP zone references ntc1 phandle 0x1e.
 * Stock thermal-generic-adc.ko gadc_thermal_get_temp .text+0x260..0x2d0:
 * descending processed values, endpoint clamp, integer linear interpolation.
 * Pure conversion only: neither sensor freshness nor ADC availability proven.
 */
static const int ApProcessedAdc[166] = {
  1760, 1757, 1754, 1751, 1747, 1744, 1740, 1736, 1731, 1727, 1722, 1716,
  1711, 1705, 1699, 1693, 1686, 1679, 1672, 1664, 1656, 1648, 1639, 1630,
  1620, 1610, 1599, 1588, 1577, 1565, 1553, 1540, 1527, 1514, 1500, 1485,
  1470, 1455, 1439, 1423, 1406, 1389, 1372, 1354, 1335, 1317, 1298, 1279,
  1259, 1239, 1219, 1198, 1178, 1157, 1136, 1115, 1093, 1072, 1050, 1029,
  1007, 986, 964, 943, 921, 900, 879, 858, 837, 816, 796, 775,
  755, 736, 716, 697, 678, 659, 641, 623, 605, 588, 571, 555,
  538, 523, 507, 492, 477, 463, 449, 435, 422, 409, 396, 384,
  372, 360, 349, 338, 327, 317, 307, 297, 288, 279, 270, 261,
  253, 245, 237, 230, 222, 215, 209, 202, 196, 189, 183, 178,
  172, 167, 161, 156, 151, 147, 142, 138, 133, 129, 125, 121,
  118, 114, 111, 107, 104, 101, 98, 95, 92, 89, 87, 84,
  81, 79, 77, 74, 72, 70, 68, 66, 64, 62, 61, 59,
  57, 56, 54, 52, 51, 50, 48, 47, 46, 44
};

int MtkThermalApNtcConvert(int ProcessedAdc, MTK_THERMAL_SAMPLE *Sample)
{
  unsigned int Index;
  int Temperature, Clamp = 0;
  if (Sample == 0) return 0;
  if (ProcessedAdc >= ApProcessedAdc[0]) {
    Temperature = -40000;
    Clamp = ProcessedAdc > ApProcessedAdc[0] ? 1 : 0;
  } else if (ProcessedAdc <= ApProcessedAdc[165]) {
    Temperature = 125000;
    Clamp = ProcessedAdc < ApProcessedAdc[165] ? -1 : 0;
  } else {
    for (Index = 1; ProcessedAdc < ApProcessedAdc[Index]; ++Index) {}
    /* Input is bracketed before arithmetic; product <= 22000. Signed
     * division truncates toward zero, matching the shipped interpolation. */
    Temperature = -40000 + ((int)Index - 1) * 1000 +
      (ProcessedAdc - ApProcessedAdc[Index - 1]) * 1000 /
      (ApProcessedAdc[Index] - ApProcessedAdc[Index - 1]);
  }
  Sample->MilliCelsius = Temperature;
  Sample->Clamped = Clamp;
  return 1;
}

/* Stock soc_temp_lvts.ko mt6789_efuse_to_cal_data and check_cal_data_v1.
 * The upper half of word 7 has no sensor. Words 8..11 hold 24-bit RC
 * values and contribute to the decision to use the default calibration.
 */
int MtkThermalLvtsCalibration(const unsigned int Words[MTK_LVTS_EFUSE_WORDS],
                             MTK_LVTS_CALIBRATION *Calibration)
{
  unsigned int Index, Used;
  if (!Words || !Calibration || Words[0] == 0xffffffffU) return 0;
  Used = (Words[0] >> 24) | (Words[7] & 0xffffU);
  for (Index = 1; Index < MTK_LVTS_EFUSE_WORDS; ++Index) {
    if (Words[Index] == 0xffffffffU) return 0;
    if (Index < 7) Used |= Words[Index];
    if (Index >= 8) Used |= Words[Index] & 0xffffffU;
  }
  Calibration->Golden = Used ? Words[0] >> 24 : 60;
  for (Index = 0; Index < MTK_LVTS_SENSOR_COUNT; ++Index) {
    Calibration->CountR[Index] = Used ?
      (Words[1 + Index / 2] >> (16 * (Index % 2))) & 0xffffU : 0x88b8U;
  }
  return 1;
}

/* stock MT6789 lvts_raw_to_temp_v1. The signed shift rounds down, even
 * for a negative product that is not an exact multiple of 2^14.
 */
int MtkThermalLvtsConvert(unsigned int Code, unsigned int Golden, int *MilliCelsius)
{
  if (!MilliCelsius || !Code || Code > 0xffff || Golden > 255)
    return 0;
  *MilliCelsius = (int)Golden * 500 + 250460 -
    (int)((250460ULL * Code + 16383) / 16384);
  return 1;
}

int MtkThermalToAcpi(int MilliCelsius, unsigned int *DeciKelvin)
{
  if (DeciKelvin == 0 || MilliCelsius < -40000 || MilliCelsius > 125000)
    return 0;
  *DeciKelvin = (unsigned int)(MilliCelsius + 273150 + 50) / 100;
  return 1;
}
