// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SD_FAT_EXPORT_H
#define T70_SD_FAT_EXPORT_H
#include "SdFatTarget.h"
#include "ManualExportCore.h"
/* Read-only protocol discovery; never connects controllers or reads media.
 * Only the platform MSDC1 vendor path and its proven SD/FAT children qualify.
 * A full list is required: BUFFER_TOO_SMALL returns Count=0, no partial UI. */
EFI_STATUS T70DiscoverSdFatTargets(EFI_BOOT_SERVICES *, T70_SD_FAT_IDENTITY *, UINTN Capacity, UINTN *Count);
/* Call only after explicit selection AND confirmation. RAM remains caller-owned.
 * Detail preserves the first EFI error; result separately reports close failure.
 * Success is Flush/Close success, not independent physical persistence proof. */
T70_EXPORT_RESULT T70ExportSelectedSdFat(EFI_BOOT_SERVICES *, CONST T70_SD_FAT_IDENTITY *,
 T70_EXPORT_KIND, UINT32 Number, CONST VOID *, UINTN Size, EFI_STATUS *Detail);
#endif
