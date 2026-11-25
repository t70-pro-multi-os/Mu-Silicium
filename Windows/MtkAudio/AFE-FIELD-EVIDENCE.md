# DL1 format-field callback connection

This is limited to the S16/S24-in-32-bit-storage DL1 subset established in
`AFE-FORMAT-EVIDENCE.md`, with the same stock modules and hashes. Every update
below targets AFE-relative offset 0x4c; no mapping or physical address is created.

| Order | Descriptor fields | Update mask | Shifted value |
| --- | --- | --- | --- |
| Mono | +0x34=0x4c, +0x38=8, +0x3c=0 | 0x100 | Mono << 8 |
| Rate | +0x28=0x4c, +0x2c=24, +0x30=0xf | 0x0f000000 | RateCode << 24 |
| HD | +0x5c=0x4c, +0x60=0, +0x64=0 | 0x3 | HdMode |
| Alignment | +0x68=0x4c, +0x6c=4, +0x70=0 | 0x10 | 0 |

These values come from DL1's descriptor at AFE .rodata 0x1148. The common
hardware-parameter function calls channel at 0xd44, rate at 0xd54 and format at
0xd68. Its format handler substitutes HD mask 3 when the descriptor mask is zero
(0x1634..0x165c), calls HD update at 0x1674 and alignment update at 0x16ac.
The rate path uses descriptor shift/mask; mono uses its non-inverted DL1 field.
This is field programming evidence, not a clock/power/start sequence.

`T70AfeDl1UpdateFormat` validates the format first, then performs four fresh
read/masked-write pairs in that order. Here set mask bits mean UPDATE bits,
unlike the AW preserve-mask API. The result is `(old & ~mask) | (value & mask)`;
all other bits, including upper bits, are retained from that step's read.
It deliberately writes even when unchanged; it does not reproduce regmap's
possible no-change write suppression. Nonzero read/write errors stop immediately,
which is an explicit policy rather than a claim to reproduce every stock error
path. Read failures cause no corresponding write; earlier completed writes
remain. A failed write may have applied, so no rollback or retry is attempted.

The caller/provider must establish quiescence, valid ordered mapping and
exclusive ownership/serialization across the entire sequence, including each
RMW pair. Callback existence does not prove these conditions. No enable/disable,
IRQ, power or DMA operation is included. Four successful pairs mean only callback
completion, not verified readback or functional playback. No real MMIO provider
or device execution was added. Host tests cover preserved bits, all accepted
rates, every read/write failure phase, and invalid requests with zero calls.
