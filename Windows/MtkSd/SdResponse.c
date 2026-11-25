// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdResponse.h"
#include <stddef.h>

T70_SD_RESPONSE_RESULT T70SdCheckCmd8(uint32_t Response)
{
    return (Response & (UINT32_C(0xfff) | (UINT32_C(1) << 22))) == UINT32_C(0x1aa)
        ? T70_SD_RESPONSE_OK : T70_SD_RESPONSE_REJECTED;
}

T70_SD_RESPONSE_RESULT T70SdCheckAppCommand(uint32_t Response)
{
    const uint32_t App = UINT32_C(1) << 5;
    const uint32_t Illegal = UINT32_C(1) << 22;
    return (Response & (App | Illegal)) == App
        ? T70_SD_RESPONSE_OK : T70_SD_RESPONSE_REJECTED;
}

T70_SD_RESPONSE_RESULT T70SdDecodeRelativeAddress(
    uint32_t Response, uint16_t *RelativeAddress)
{
    uint16_t Address;
    if (RelativeAddress == NULL) {
        return T70_SD_RESPONSE_INVALID_ARGUMENT;
    }
    Address = (uint16_t)(Response >> 16);
    if ((Response & UINT32_C(0xe000)) != 0 || Address == 0) {
        return T70_SD_RESPONSE_REJECTED;
    }
    *RelativeAddress = Address;
    return T70_SD_RESPONSE_OK;
}
