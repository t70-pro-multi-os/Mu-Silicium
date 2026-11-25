# DL1 base/end callback transport, without starting DMA

The identified stock module hashes are in `AFE-GEOMETRY-EVIDENCE.md`. DL1's
`memif_data` at AFE .rodata 0x1148 supplies descriptor fields +0x10/+0x18/
+0x1c/+0x24 as register offsets 0x54/0x64/0x50/0x60. CUR fields +0x14/+0x20
are offsets 0x5c/0x58; CUR is not a write target of this implementation.

The common `mtk_memif_set_addr` path calls the register writer in this order:

| Stock call offset | AFE-relative register | Value |
| --- | --- | --- |
| 0x1104 | 0x54 | Base low 32 bits |
| 0x1124 | 0x64 | Inclusive end low 32 bits |
| 0x1140 | 0x50 | Base high 32 bits |
| 0x1158 | 0x60 | Inclusive end high 32 bits |

Inclusive end is Base + Length - 1 (0x10f0/0x1110). The stock descriptor has
nonzero explicit end/high fields, so fallback offsets for other memifs are not
used. This evidence supports the four values and their call order only. It does
not establish that programming them while DMA runs is safe.

`T70AfeDl1SetAddress` connects a revalidated prepared snapshot to an explicit
32-bit write callback, using exactly those relative offsets in that order. No
physical address is invented or mapped by this component. The provider owns the
verified AFE mapping and required access ordering/barriers. The caller must
establish DMA quiescence and exclusive ownership before calling, keep the actual
buffer mapped, and retain all resources through any failure recovery. Those are
unverified preconditions, not guarantees produced by the arithmetic validator.
No boolean 'ready' argument is treated as proof of hardware state.

The stock four write calls do not check each returned error. This implementation
instead stops immediately on a nonzero callback result, a deliberate fail-closed
transport policy rather than identical stock failure behavior. The returned
attempted count, completed count and failed offset distinguish progress. Even
the failing callback may already have modified hardware. There is no automatic
retry, rollback, buffer release or DMA start. Four successful callbacks establish
only callback completion, not a working playback route or verified register
readback. Partial failure must not be treated as a safely released device.

No concrete MMIO provider, resource creator or stream start/stop implementation
has been added. Host tests use only mocks and verify every failure position,
positive/negative errors, 32-bit crossings and 34-bit limits, invalid snapshots,
call order, written values and absence of extra writes. No real MMIO, DMA,
power/reset operation or playback test was performed.
