// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SD_FAT_TARGET_H
#define T70_SD_FAT_TARGET_H
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
typedef struct {
 EFI_HANDLE Controller,Card,Partition;
 EFI_BLOCK_IO_PROTOCOL *BlockIo;
 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
 UINT32 MediaId,BlockSize; EFI_LBA LastBlock;
 HARDDRIVE_DEVICE_PATH PartitionPath;
} T70_SD_FAT_IDENTITY;
/* Controller must already be proven the T70 external MSDC controller by caller.
 * Exact vendor/SD(slot0)/HD single-instance paths and child relations are checked.
 * Expected=NULL captures identity; otherwise rejects changed identity. No OpenVolume,
 * reads/writes, connects, or media-policy changes. Output untouched on error.
 * UI/INF integration is pending; invoke again immediately before manual export. */
EFI_STATUS T70ValidateSelectedSdFatTarget(EFI_BOOT_SERVICES *,EFI_HANDLE Controller,
 EFI_HANDLE Card,EFI_HANDLE Partition,CONST T70_SD_FAT_IDENTITY *Expected,
 T70_SD_FAT_IDENTITY *Out);
#endif
