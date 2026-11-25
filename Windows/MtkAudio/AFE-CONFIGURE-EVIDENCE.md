# DL1 register subset configuration

`AfeConfigure.c` connects the existing address and format callback cores with one
explicit target/context. Stock `snd-soc-mtk-common.ko` SHA256
`34476abe92b41828f4fc1a6f44e0a5b833d94987118e776475de4e564d703644`
`mtk_afe_fe_hw_params` relocations establish order directly:

- `.text 0xd34`: `mtk_memif_set_addr`
- `.text 0xd44`: `mtk_memif_set_channel`
- `.text 0xd54`: `mtk_memif_set_rate_substream`
- `.text 0xd68`: `mtk_memif_set_format`

Therefore address first, then mono/rate/HD/alignment, not format first. Individual
register offsets, masks and inner ordering are documented in
AFE-ADDRESS-EVIDENCE.md and AFE-FIELD-EVIDENCE.md. This is only that register
subset, not all of hw_params: memory clearing/allocation, platform callbacks,
clocks, routing, IRQ, DMA enabling and power are not implemented here.

The complete prepared snapshot and both callbacks are validated before the first
write. Address failure prevents every format callback. Format failure stops its
remaining fields. Results retain each started stage's exact error, attempted and
completed counts; `AddressStarted`/`FormatStarted` determine whether a sub-result
is applicable. A zero-initialized unstarted result is not a successful stage.
The wrapper does not undo earlier writes. Failed writes may also have applied.
This is an intentionally stricter fail-fast policy, not stock's error behavior.
CallbacksCompleted is not hardware readback, stream readiness or playback.

Caller must establish quiescence and exclusive ownership, provide ordered valid
mapping, and retain the DMA allocation through failure recovery. The core does
not establish those facts. No actual MMIO provider or hardware operation was
used for validation. Tests mock all 12 callback positions, signed errors, both
formats/channels, 11 rates, 32-bit address crossings and the 34-bit limit; every
derived snapshot field is separately corrupted to verify zero I/O. Failed mock
writes deliberately apply their value before returning failure.
