// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_CPU_IO_H
#define T70_CPU_IO_H

// Input is one ULONG domain index. Frequencies in the reply are in kHz.
DEFINE_GUID(GUID_DEVINTERFACE_T70_CPU, 0xa07d9c40, 0xe66a, 0x498c,
    0x9a, 0xf5, 0x48, 0x76, 0x72, 0xe5, 0x79, 0x39);

#define IOCTL_T70_CPU_QUERY_DOMAIN CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
/* Input is a domain and an exact table frequency in kHz. Success means the
 * controller accepted the request. There is no output buffer. */
#define IOCTL_T70_CPU_SET_OPP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct {
    ULONG Domain;
    ULONG FrequencyKhz;
} T70_CPU_TARGET;
#define T70_CPU_DOMAIN_COUNT 2
#define T70_CPU_MAX_OPPS 32

typedef struct {
    ULONG ControllerValue;
    ULONG RequestedKhz;
    ULONG FrequencyCount;
    ULONG FrequenciesKhz[T70_CPU_MAX_OPPS];
} T70_CPU_READING;
#endif
