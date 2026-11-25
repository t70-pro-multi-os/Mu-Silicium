# Manual RAM export core unit tests

ManualExportCore accepts caller-owned log or BMP bytes already in RAM. It never
captures a screen, discovers a volume, changes media policy, or frees the input.
Cancel (Manual=0) performs no callbacks. Failed exports leave the RAM input owned
by the caller, and keep a potentially partial new file rather than deleting it.

The trusted adapter must revalidate one explicitly selected, path-backed SD FAT
volume and its media identity. Unknown/mixed ancestry, UFS, USB, read-only media,
or media replacement must fail closed. The unit test uses mock callbacks.
The production adapter is in SdFatExport.c. In particular,
EFI_FILE_MODE_CREATE is not exclusive-create; it cannot directly implement the
CreateNew contract. The callback must never open/truncate an existing file.

Write length and explicit Flush result are checked. Close cannot erase an earlier
failure. Success is only callback-level write/flush/close success, not a physical
persistence guarantee. A same-volume readback may hit FAT cache and is not proof.
This test alone does not exercise the UI, GOP, real FAT access, or build
inclusion. For the current linked DeviceCheck UI, paging, RAM capture, FAT
adapter tests and writable media limits, see the
[DeviceCheck export handoff](../../Applications/DeviceCheck/ManualExportUi.md).

Focused existing-VM compilation (run under the shared workspace build lock):

```sh
clang -std=c11 -Wall -Wextra -Werror \
  Platforms/Teclast/t70proPkg/Applications/DeviceCheck/ManualExportCore.c \
  Platforms/Teclast/t70proPkg/Tests/DeviceCheck/test-manual-export.c \
  -o /tmp/t70-manual-export-test
/tmp/t70-manual-export-test
```

Sixteen mock scenarios cover both types, cancellation/rejected target, existing
file, write/short-write/flush/close errors, bad kind and payload bounds. No real
medium is involved and no firmware filesystem operations are performed.
