// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_THERMAL_IO_H
#define T70_THERMAL_IO_H

DEFINE_GUID(GUID_DEVINTERFACE_T70_NTC,
    0xc5e8b4ee, 0xea13, 0x456d, 0xb2, 0xd7, 0x7e, 0x31, 0x4f, 0x0b, 0x3e, 0x56);

#define IOCTL_T70_READ_NTC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_T70_READ_SOC_TEMP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define T70_SOC_SENSOR_COUNT 13

/* Both requests return this sample. READ_NTC takes 0 (board) or 1 (LTE).
 * READ_SOC_TEMP takes the stock thermal sensor ID 1..13, or 0 for the maximum.
 */
typedef struct {
    ULONG RawCode;
    LONG MilliCelsius;
    ULONG DeciKelvin;
} T70_NTC_READING;

#endif
