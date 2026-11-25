# Explicit selected SD FAT export

`T70DiscoverSdFatTargets` enumerates already-published protocol handles only.
It does not connect a controller, open a filesystem, or issue block reads.
The controller vendor path must end in index 1: MsdcDxe assigns
`DevicePath->Mmc.Guid.Data4[7] = Index`. The common template ending in 0
is NOT external MSDC1. The global diagnostic HostIndex is not used.
The existing validator additionally checks SD slot 0, exact parent/child
relationships, EnhancedFatDxe ownership, HD geometry and writable media.
Discovery also requires the new atomic-create protocol on that same partition.
Full discovery failure returns Count=0, never an incomplete selectable list.

Call `T70ExportSelectedSdFat` ONLY after the user explicitly selects one
returned identity and confirms the export. It revalidates the snapshot before
OpenVolume and again afterward, then calls atomic CreateNew on that partition
with the captured MediaId and root handle. No generic File.Open fallback exists.
Name collision is an error, not a retry or overwrite. It closes the new handle
and root, reports explicit Flush and close failures, and never frees or modifies
payload RAM. An unchanged byte count on write failure is not reported as
confirmed progress. A failed newly created file is retained, not deleted. Successful
Flush/Close is NOT independent media persistence verification. RO remains RO.

Focused Linux host test (no device or filesystem is opened):
```
clang -std=c11 -fshort-wchar -DMDEPKG_NDEBUG \
 -I Mu_Basecore/MdePkg/Include -I Mu_Basecore/MdePkg/Include/AArch64 \
 -I Mu_Basecore/FatPkg/Include \
 Platforms/Teclast/t70proPkg/Tests/DeviceCheck/test-sd-fat-export.c \
 -o /tmp/t70-sd-export-test && /tmp/t70-sd-export-test
```
Run via the existing with-build-lock wrapper. The fixture includes the actual
identity validator, export adapter and manual core; only EFI callbacks are mock.
No full firmware build or physical persistence test is claimed.
