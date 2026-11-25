# AFE buffer geometry: arithmetic only

## Identified stock inputs

- `snd-soc-mt6789-afe.ko`: SHA256
  `67f1ec09fc9d883d2d4933b8f4b062bf8c473f5bd4968761054914f703706ba8`
- `snd-soc-mtk-common.ko`: SHA256
  `34476abe92b41828f4fc1a6f44e0a5b833d94987118e776475de4e564d703644`
- Effective DTB: SHA256
  `a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391`

The AFE module's probe passes `0x3ffffffff` to both DMA mask setters at .text
0x9e38/0x9e4c. This establishes a 34-bit device-address limit, not a valid
allocation or a guarantee that an arbitrary address is accessible.

`mt6789_fe_startup` at 0xb158/0xb15c supplies parameter 18 and step 16 to
`snd_pcm_hw_constraint_step` at 0xb17c. Parameter 18 is BUFFER_BYTES in the
[ALSA UAPI](https://raw.githubusercontent.com/torvalds/linux/v6.1/include/uapi/sound/asound.h).
The constraint concerns buffer length, NOT the alignment of its starting
address. The integer-period constraint is called at 0xb194 with parameter 15.

`mt6789_afe_hardware`, .rodata 0x85c0, size 0x50, has the following fields in
the AArch64 [snd_pcm_hardware layout](https://raw.githubusercontent.com/torvalds/linux/v6.1/include/sound/pcm.h):

| Decimal byte offset | Field | Value |
| --- | --- | --- |
| 40 | buffer_bytes_max | 196608 |
| 48 | period_bytes_min | 96 |
| 56 | period_bytes_max | 196608 |
| 64 | periods_min | 2 |
| 68 | periods_max | 256 |

In the common module, `mtk_memif_set_addr` computes address + length at 0x10f0
and subtracts one at 0x1110. It splits the starting address at 0x1084 and the
inclusive ending address at 0x1154 into high 32-bit parts. The new core performs
this arithmetic only after checking the whole range against the DMA mask. It
never calls this stock routine, writes registers or emits a register write plan.

DTB node `/soc/mt6789-afe-pcm@11210000` describes register space 0x11210000
length 0x2000; the audio SRAM node describes 0x11212000 length 0xd000. Neither
range is used as an inferred DMA allocation pool by this implementation.

## Implemented and deliberately unimplemented

`T70AfeBufferGeometry` checks the recovered byte/period limits, integral periods,
16-byte buffer-length step, whole-frame divisibility and nonwrapping 34-bit
inclusive range. It computes frame counts and address halves with no I/O.
Frame storage width is supplied by the caller; this is NOT validation of any
particular PCM format, sample rate or channel arrangement. Every failure leaves
the output untouched. Address 0 or an unaligned address may pass arithmetic:
that is not authorization to use it as DMA memory.

Still unverified here: base alignment requirements, allocator/translation and
cache contracts, chosen playback memif/register routing, actual device ownership,
format negotiation, and complete playback connection. No power, reset, clock or
DMA-start sequence was implemented or exercised. Host tests use a separate
128-bit arithmetic oracle around zero, limits, 32-bit crossings, 34-bit limits,
overflow inputs and every buffer length through 196624 bytes. This code is a
bounded geometry utility, not a working PCM driver.
