// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SD_RESPONSE_H
#define T70_SD_RESPONSE_H
#include <stdint.h>

typedef enum {
  T70_SD_RESPONSE_OK = 0,
  T70_SD_RESPONSE_INVALID_ARGUMENT,
  T70_SD_RESPONSE_REJECTED
} T70_SD_RESPONSE_RESULT;

/* Pure SD initialization checks. Response is a host-endian, CRC-stripped
 * 32-bit response payload, like EFI SDC_RESP0; NOT raw wire bytes or MMIO.
 * The caller must first establish successful command completion, valid response
 * type and transport CRC/timeout status. These functions cannot establish them.
 * No kernel, WDF, SDPORT, register, DMA, power or interrupt dependencies. */

/* Only for CMD8 sent with argument 0x1AA (2.7-3.6V, check pattern 0xAA).
 * Rejects a legacy R1 illegal-command indication even if low bits match. */
T70_SD_RESPONSE_RESULT T70SdCheckCmd8(uint32_t Response);

/* Only the APP_CMD/ILLEGAL_COMMAND acknowledgement contract after CMD55.
 * This is NOT a complete R1 error decoder or permission to start data I/O. */
T70_SD_RESPONSE_RESULT T70SdCheckAppCommand(uint32_t Response);

/* SD CMD3 R6, not eMMC CMD3 R1. Rejects error bits 15:13 and reserved RCA 0.
 * RelativeAddress is unchanged on failure, including a null output pointer. */
T70_SD_RESPONSE_RESULT T70SdDecodeRelativeAddress(
    uint32_t Response, uint16_t *RelativeAddress);
#endif
