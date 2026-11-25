# Explicit RAM capture / manual SD FAT save UI

Current working-tree DeviceCheck hooks:
- P: capture the visible diagnostic page to RAM. Failure preserves the old BMP.
- L: snapshot the current log as UTF-16LE with BOM for a .txt export.
- B: snapshot the existing RAM BMP; if none, ask for P first.
- Show validated writable SD FAT candidates. A number selects one volume.
- Show that volume's partition/media/start/size plus payload kind/size.
- Only a subsequent Y approves creating a new file. Any other key cancels.
- Background topology rescans pause while selection/confirmation is pending.
  The adapter still revalidates the exact identity immediately before export.

No auto-selection, fallback volume, overwrite, retry, RO-policy change or
automatic save. The original screenshot/log remain in RAM after export.
Temporary export snapshot is released after completion/cancel; screenshot RAM
is released when leaving DeviceCheck. A successfully flushed/closed file is
reported as such, not independently verified persistent storage. Failure shows
operation stage and partial-file/close state; a newly created partial file may
remain. Log snapshots do not include the later save-status line.

ManualExportChoice is a pure selection/confirmation gate, tested with native
ASan/UBSan. SdFatExport/ManualExportCore provide the actual protocol adapter and
transaction handling. The restricted atomic FAT CreateNew extension is required.
With the current read-only board policies, no writable target is offered.

The pre-existing DeviceCheck C/INF dirty changes are preserved. Their new hooks
are intentionally not committed as whole files while original ownership remains
unresolved. The dated handoff below distinguishes linked/emulated evidence from
the unverified physical save path. No tablet save, flash or RO flip was performed.

## Current standalone build (2026-11-22)

`~/.tmp/teclast-t70pro/devicecheck-gop-contract-5W0Vtj/T70DeviceCheck-GopFix.efi`

- ARM64 EFI application. SHA-256
  `4c88e52a8a1937658cf65a6c24e2d6c97f05926f837df33ca1f87bad752493af`.
  This is not a full FD or deployable board image.
- Includes `49a4304`: when ConOut publishes only text, P can use a unique ready
  GOP or two GOP handles proven to refer to the same framebuffer and layout.
  Separate ambiguous displays are refused instead of picking the first handle.
- Exact app under generic AAVMF: P captured a 1,440,054-byte BMP into RAM.
  B refused before any capture and after capture with no writable SD. No file
  write was attempted. See `gop-fix-verification.json` beside the app.
- Exact app under an 80×25 AAVMF console: Down and Up moved the footer from
  rows 1–21/32 to 22–32/32 and back. The selection instructions remained
  visible. See `~/.tmp/teclast-t70pro/devicecheck-paging-esglG5/verification.json`.
- Distinct evidence: the positive FAT adapter tests below use a simulated
  controller or registry; the exact current UI build verifies RAM capture
  and refusal only. Current board SD policies still provide no writable
  target. Do not interpret this standalone app as enabling board SD writes.

## Previous standalone build (2026-11-20)

`~/.tmp/teclast-t70pro/devicecheck-373bed9-ew7vYE/T70DeviceCheck-8aedb4bf.efi`

- ARM64 EFI application, 47,104 bytes. SHA256:
  `8aedb4bf67b2a530140a6a89997239e838120eaf5c9996df0956a700d38c4a21`.
- Includes `373bed9`: if a FAT error leaves the requested byte count unchanged,
  it is no longer shown as confirmed write progress. The independent adapter
  checks cover media loss at create, write, and flush.
- The build log and source identity are in the same result directory. This
  is a module link only. It does not change SD write policy. The QEMU
  screenshots below are evidence for earlier hashes, not this exact file.

## Prior standalone handoff snapshot (2026-11-19)

Previous diagnostic artifact (not a board image):
`~/.tmp/teclast-t70pro/T70DeviceCheck-diskio-page-20261119-6e520189.efi`

- ARM64 EFI application, 47,104 bytes.
- SHA256: `6e5201893b82b1138b90b71d0124e5cd56dca2c2784b0bd97b86ab656a583a8d`.
- Normal `build.sh devicecheck` link from parent `0172c0b`, plus preserved
  working-tree Page integration. No full FD/image deployment is implied.
- Use P / L / B, destination number, then separate Y as documented above.
  Capture alone never saves; cancellation never selects a fallback volume.

### Current production eligibility

`t70pro.dsc` still sets both `PcdMsdcReadOnly` and `PcdSdDxeReadOnly` TRUE.
The validator rejects read-only media; no writable SD target is offered under
these policies. This artifact does not change them. Only a direct GPT or
primary-MBR partition on verified external MSDC1 is supported. Whole-card FAT
and nested/extended-MBR paths are excluded. The matching atomic FAT CreateNew
protocol is also required; generic FAT alone is insufficient.
`0172c0b` matches the PartitionDxe DiskIo child-controller relation. This fixes
target identification, not hardware operation.

### Evidence and boundaries

- Prior pager artifact `T70DeviceCheck-page-20261119-3219fa51.efi` ran unchanged
  in AAVMF with read-only virtual media and an emulated keyboard. At 80x25:
  initial rows 1-21/37, Down 22-37/37, repeated Down unchanged, Up restored the
  initial screenshot byte-for-byte. An 80-character line retained its final
  character on the next row; footer remained visible. Evidence:
  `~/.tmp/teclast-t70pro/aavmf-page-80x25-20261119/`.
  This predates the DiskIo fix and is not a run of the later November 19 hash.
- The November 19 `6e520189` app itself was also run under standard AAVMF
  with a read-only virtual FAT disk and 640x480 / 80-column console.
  Down reached the last page, another Down left it unchanged, and Up restored
  the initial screen byte-for-byte. P captured 921,654 bytes into RAM. L and B
  each reported no writable target; the read-only fixture gained no files.
  Evidence: `~/.tmp/teclast-t70pro/aavmf-diskio-exact-9l914vap/execution.json`.
  This verifies the shipped app bytes and refusal, not the positive save path.
- `c6b6203`: deterministic 1280x800 BMP24 (3,072,054 bytes) through actual
  FAT/SdDxe/MSDC code with a synthetic controller/FIFO and private FAT image.
  Independent mcopy readback verified the BMP/tail and existing log.
  Not the GOP-captured screenshot or a physical SD card.
- `f028618`: DeviceCheck discovery against actual FAT protocol publications
  within a synthetic host registry fixture.
- `ec76a04`: approved DeviceCheck export adapter through actual FAT writes
  within that host fixture. Synthetic selection/identity is not tablet input.

These separate checks are not one validated UI-to-physical-SD save path.
Actual tablet SD saving remains unverified. Physical log save followed by independent reopen
and byte readback remains an outstanding gate. This handoff does not recommend
flashing before that validation is established.
