// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef CONN_TEST_BCRYPT_H
#define CONN_TEST_BCRYPT_H
#include <wdm.h>
#define BCRYPT_SHA256_ALG_HANDLE ((PVOID)(uintptr_t)0x41)
NTSTATUS BCryptHash(PVOID, PUCHAR, ULONG, PUCHAR, ULONG, PUCHAR, ULONG);
#endif
