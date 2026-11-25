// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_CONN_PMIC_H
#define T70_CONN_PMIC_H

#include <ntddk.h>
#include <prminterface.h>

typedef struct {
    PRM_INTERFACE Interface;
    ULONGLONG Lease;
    BOOLEAN ModuleLocked;
    BOOLEAN Uncertain;
} T70_CONN_PMIC;

/* A passive-level board owner serializes calls and keeps this zero-initialized
 * context until Release succeeds. Stop CONN and restore its pin and rail
 * state before Release. An uncertain hardware job keeps the module pinned. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnPmicAcquire(T70_CONN_PMIC *Pmic);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnPmicRelease(T70_CONN_PMIC *Pmic);

/* MTK_CONN_RAIL_IO PMIC callbacks. Context is the active T70_CONN_PMIC. */
_IRQL_requires_(PASSIVE_LEVEL)
int T70ConnPmicRead(void *Context, unsigned Address, unsigned *Value);
_IRQL_requires_(PASSIVE_LEVEL)
int T70ConnPmicWrite(void *Context, unsigned Address, unsigned Value);

#endif
