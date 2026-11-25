// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_REGISTERS_H
#define T70_AW_REGISTERS_H
#include "AwAcf.h"
typedef struct { uint16_t Address, Value; } T70_AW_REGISTER;
/* Decode only the observed AW88261 single-register-bin format. These are
 * configuration data, NOT authorization or an ordered plan to write hardware.
 * All output parameters are unchanged on failure. Input must remain immutable. */
int T70AwRegisterBinValidate(const void *Buffer,size_t Bytes,uint32_t *Count);
int T70AwAcfRegister(const void *Buffer,size_t Bytes,uint16_t DeviceIndex,
                    uint16_t Profile,uint32_t Index,T70_AW_REGISTER *Register);
#endif
