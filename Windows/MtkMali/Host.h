/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_HOST_H
#define T70_MALI_HOST_H
#include "Board.h"
#define MALI_BOARD_PARENT_INTERFACE_GUID_INIT {0x5ed36789,0x4701,0x42d1,{0xb1,0x70,0x53,0x72,0x70,0x03,0x10,0x01}}
/* Published by the real SoC resource owner (same-stack interface or shared
   HANA678F root owner in the same driver service). Query
   success owns one reference. Open returns this same PDO's fresh initialized
   unacquired controller and immutable resources, with no host lifetime hooks.
   It does not enable hardware. Cookie on any result retains ALL config objects
   until Close succeeds. Close must not drop ownership or mutate storage on
   failure. The SoC owner validates PDO identity; neither the host nor Open may
   assume address/cache/OPP/rail ownership from the ACPI GPU memory range. */
typedef struct {
    INTERFACE Header;
    NTSTATUS (*Open)(void *,PDEVICE_OBJECT,MALI_BOARD_CONFIG *,void **Cookie);
    NTSTATUS (*Close)(void *,void *Cookie);
} MALI_BOARD_PARENT_INTERFACE;
typedef struct {ULONG Size,Version;PDEVICE_OBJECT Pdo;} MALI_BOARD_PARENT_QUERY;
DRIVER_INITIALIZE MtkMaliBoardHostDriverEntry;
DRIVER_ADD_DEVICE MtkMaliBoardHostAddDevice;
DRIVER_DISPATCH MtkMaliBoardHostDispatch;
#endif
