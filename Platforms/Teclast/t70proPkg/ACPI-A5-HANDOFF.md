# T70 Pro: distinguish the observed 0xA5 from the existing QEMU comparison

The latest physical stop-screen photo shows only `ACPI_BIOS_ERROR (0xA5)`;
it does not show Parameter 1, the firmware image, or the USB media identity.
Do not identify this physical stop with an earlier virtual subtype without
matching evidence.

The older board-model run's Parameter 4 bytes begin with the ASCII name `GPO0`.
That run installed a DSDT which published `LID0._PRW`; the later run's DSDT did
not. This is a table-level correlation for that virtual `0xA5/0x06`, not proof
that the new physical photo has the same cause. Microsoft documents Parameter 1
`0x06` as a missing named object while processing a `_PRx` resource.

The current source DSDT is 10,115 bytes (ASL SHA-256
`6389d1ce0287bb9e89295842681c063ed7bbc379e4abe311a3a10904e541db4d`). Its
current AML is 10,115 bytes (SHA-256
`37a748d45faae4e76871540e00d8b45e850eb9cfbf1b5078c70de6d1b6aeedfa`). A fresh
host build evaluates this DSDT and all four SSDTs successfully; the full trial
FD package also matches these generated tables. Neither result reproduces the
physical Windows stop.

| Existing board-model QEMU run | EFI image SHA-256 | Media SHA-256 | Observed result |
| --- | --- | --- | --- |
| a5-factory-pr0-before.h1RIf0eX | 44c09fe771fdab570735f4d0eea45e289a734245bbc874bb73fd47b9bb3a36a1 | 6e9ef9d29ee907a19beb48f8683be433841eb1d440bc5a65f0303f09fd23534e | 0xA5, first parameter 0x6 |
| a5-factory-after.9tp7ONb1 | 4e5ff9e9b328ea77da5d16c2c47f902641a94f148f6bc03caa45c9f50bfae666 | same as above | Later 0x1A / 0x1233; not an ACPI success claim on hardware |
| winpe-userland.i4o2JrvJ | 4e5ff9e9b328ea77da5d16c2c47f902641a94f148f6bc03caa45c9f50bfae666 | 87ba4d4beee41df00f65a8e21adad4d484fdc60434e072941537e87ed39d2f9d | Windows Setup reached |

The first two existing execution.json records have byte-identical installed
PRMT, FACP, APIC, GTDT, PPTT and four SSDTs. The only differing installed
table is DSDT. It matches the compiled table in each build, with hashes
2f43e58ab7d7dcaf757939002481894ef2846710a6d9abdcea7ae254ab95731e
and e862b78df5d6f10d14025f28562a199c5057660bab4b44e1067d8369bf4bd889.
Independent iasl disassembly shows one substantive DSDT difference:
the older one publishes the lid's LID0._PRW and the newer one does not. In the
older bugcheck record, Parameter 4 points to bytes beginning `GPO0`. The "pr0"
in the older fixture folder name is not the AML object name. The two EFI images
as a whole are not byte-identical, so this isolates a table-level difference,
not definitive attribution. The third run has the same installed table hashes
as the second.

Next: bind the real device's trial/image and media to hashes first. If it
already uses the newer DSDT, do not apply the old table assumption again;
obtain the physical bug-check parameter or capture and compare the relevant
runtime ACPI evidence. The [Microsoft 0xA5 parameter reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0xa5--acpi-bios-error)
lists different causes under the same stop code.
