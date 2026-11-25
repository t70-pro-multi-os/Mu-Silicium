/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Host.h"
NTSTATUS DriverEntry(PDRIVER_OBJECT Object,PUNICODE_STRING Path) {
    return MtkMaliBoardHostDriverEntry(Object,Path);
}
