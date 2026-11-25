// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntifs.h>
#include "Firmware.h"

#define CONN_FIRMWARE_TAG 'mFnC'

static PCWSTR const RomFiles[2][MtkConnRomPartCount] = {
    {
        [MtkConnRomBluetooth] = L"soc2_2_ram_bt_1d_1_hdr.bin",
        [MtkConnRomWifi] = L"soc2_2_ram_wifi_1d_1_hdr.bin",
        [MtkConnRomMcu] = L"soc2_2_ram_mcu_1d_1_hdr.bin"
    },
    {
        [MtkConnRomBluetooth] = L"soc2_2_ram_bt_1e_1_hdr.bin",
        [MtkConnRomWifi] = L"soc2_2_ram_wifi_1e_1_hdr.bin",
        [MtkConnRomMcu] = L"soc2_2_ram_mcu_1e_1_hdr.bin"
    }
};

static NTSTATUS ReadImage(HANDLE Directory, PCWSTR Filename, ULONG Size,
                          MTK_CONN_ROM_IMAGE *Image)
{
    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, Filename);
    OBJECT_ATTRIBUTES Attributes;
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE |
                               OBJ_DONT_REPARSE, Directory, NULL);
    HANDLE File;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status = ZwOpenFile(&File, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 &Attributes, &IoStatus, FILE_SHARE_READ,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    PUCHAR Data = NULL;
    FILE_STANDARD_INFORMATION Information;
    Status = ZwQueryInformationFile(File, &IoStatus, &Information, sizeof(Information),
                                    FileStandardInformation);
    if (Status == STATUS_SUCCESS && (IoStatus.Information < sizeof(Information) ||
        Information.Directory || Information.EndOfFile.QuadPart != Size)) {
        Status = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (Status == STATUS_SUCCESS) {
        Data = ExAllocatePool2(POOL_FLAG_PAGED, Size, CONN_FIRMWARE_TAG);
        if (!Data) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (Status == STATUS_SUCCESS) {
        LARGE_INTEGER Offset = { .QuadPart = 0 };
        Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, Data, Size, &Offset, NULL);
        if (Status == STATUS_SUCCESS && IoStatus.Information != Size) {
            Status = STATUS_END_OF_FILE;
        }
    }
    ZwClose(File);
    if (Status == STATUS_SUCCESS) {
        *Image = (MTK_CONN_ROM_IMAGE){Data, Size};
    } else if (Data) {
        ExFreePoolWithTag(Data, CONN_FIRMWARE_TAG);
    }
    return Status != STATUS_SUCCESS && NT_SUCCESS(Status) ? STATUS_IO_DEVICE_ERROR : Status;
}

VOID T70ConnFreeFirmware(T70_CONN_FIRMWARE *Firmware)
{
    for (unsigned Variant = 0; Variant < RTL_NUMBER_OF(Firmware->Images); ++Variant) {
        for (unsigned Part = 0; Part < MtkConnRomPartCount; ++Part) {
            MTK_CONN_ROM_IMAGE *Image = &Firmware->Images[Variant].Image[Part];
            if (Image->Data) {
                ExFreePoolWithTag((PVOID)Image->Data, CONN_FIRMWARE_TAG);
                *Image = (MTK_CONN_ROM_IMAGE){0};
            }
        }
    }
}

NTSTATUS T70ConnLoadFirmware(PDRIVER_OBJECT Driver, T70_CONN_FIRMWARE *Firmware)
{
    if (!Driver || !Firmware || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Firmware->Images[0].Image[MtkConnRomBluetooth].Data) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    HANDLE Directory;
    NTSTATUS Status = IoGetDriverDirectory(Driver, DriverDirectoryImage, 0, &Directory);
    if (Status != STATUS_SUCCESS) {
        return NT_SUCCESS(Status) ? STATUS_IO_DEVICE_ERROR : Status;
    }

    T70_CONN_FIRMWARE Pending = {0};
    for (unsigned Variant = 0; Variant < RTL_NUMBER_OF(RomFiles); ++Variant) {
        unsigned Adie = Variant ? 0x6635U : 0x6631U;
        for (unsigned Part = 0; Part < MtkConnRomPartCount; ++Part) {
            Status = ReadImage(Directory, RomFiles[Variant][Part], MtkConnRomImageBytes(Adie, Part),
                               &Pending.Images[Variant].Image[Part]);
            if (Status != STATUS_SUCCESS) {
                T70ConnFreeFirmware(&Pending);
                ZwClose(Directory);
                return Status;
            }
        }
    }
    ZwClose(Directory);
    *Firmware = Pending;
    return STATUS_SUCCESS;
}
