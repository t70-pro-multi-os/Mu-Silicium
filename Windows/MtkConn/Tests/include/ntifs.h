// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef CONN_TEST_NTIFS_H
#define CONN_TEST_NTIFS_H

#include <ntddk.h>
#include <wchar.h>

typedef void *HANDLE;
typedef const wchar_t *PCWSTR;
typedef struct { USHORT Length, MaximumLength; const wchar_t *Buffer; } UNICODE_STRING;
typedef struct {
    ULONG Attributes;
    UNICODE_STRING *ObjectName;
    HANDLE RootDirectory;
} OBJECT_ATTRIBUTES;
typedef struct { SIZE_T Information; } IO_STATUS_BLOCK;
typedef struct { LARGE_INTEGER EndOfFile; BOOLEAN Directory; } FILE_STANDARD_INFORMATION;

static inline VOID RtlInitUnicodeString(UNICODE_STRING *String, PCWSTR Text)
{
    USHORT Bytes = (USHORT)(wcslen(Text) * sizeof(*Text));
    *String = (UNICODE_STRING){Bytes, (USHORT)(Bytes + sizeof(*Text)), Text};
}
#define InitializeObjectAttributes(Object, Name, Flags, Directory, Security) \
    do { *(Object) = (OBJECT_ATTRIBUTES){(Flags), (Name), (Directory)}; (void)(Security); } while (0)
#define OBJ_CASE_INSENSITIVE 0x40U
#define OBJ_KERNEL_HANDLE 0x200U
#define OBJ_DONT_REPARSE 0x1000U
#define FILE_READ_DATA 1U
#define FILE_READ_ATTRIBUTES 0x80U
#define SYNCHRONIZE 0x100000U
#define FILE_SHARE_READ 1U
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20U
#define FILE_NON_DIRECTORY_FILE 0x40U
#define POOL_FLAG_PAGED 0x100U
#define DriverDirectoryImage 0U
#define FileStandardInformation 5U
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034)
#define STATUS_INVALID_IMAGE_FORMAT ((NTSTATUS)0xc000007b)
#define STATUS_END_OF_FILE ((NTSTATUS)0xc0000011)

NTSTATUS IoGetDriverDirectory(PDRIVER_OBJECT, ULONG, ULONG, HANDLE *);
NTSTATUS ZwOpenFile(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, ULONG, ULONG);
NTSTATUS ZwQueryInformationFile(HANDLE, IO_STATUS_BLOCK *, void *, ULONG, ULONG);
NTSTATUS ZwReadFile(HANDLE, HANDLE, void *, void *, IO_STATUS_BLOCK *, void *, ULONG,
                    LARGE_INTEGER *, ULONG *);
NTSTATUS ZwClose(HANDLE);
PVOID ExAllocatePool2(ULONGLONG, SIZE_T, ULONG);
VOID ExFreePoolWithTag(PVOID, ULONG);

#endif
