# DL1 frontend format encoding, not a playback driver

Uses the two stock module hashes recorded in `AFE-GEOMETRY-EVIDENCE.md`.
`mt6789_memif_dai_driver` is at AFE .data 0x190, with 20 entries of stride 0xd0.
The first entry's name relocation resolves to DL1 and its ID is zero. Playback
fields are at entry offsets 0x98 (name), 0xa0 (formats 0x444), 0xa8 (subformats
zero), 0xac (rates 0x1efe), 0xb8/0xbc (channels 1/2).
The [current snd_soc_pcm_stream declaration](https://github.com/torvalds/linux/blob/master/include/sound/soc.h)
includes the subformats field matching this layout; the older v6.1 stream
layout does not. No claim is made that the whole vendor ABI matches upstream.

The [ALSA format and rate definitions](https://github.com/torvalds/linux/blob/master/include/sound/pcm.h)
identify advertised formats S16_LE/S24_LE/S32_LE and the eleven discrete rates
below. Advertised frontend capabilities are not a codec/speaker route guarantee.

| Rate Hz | General memif code |
| --- | --- |
| 8000 | 0 |
| 11025 | 1 |
| 16000 | 4 |
| 22050 | 5 |
| 32000 | 8 |
| 44100 | 9 |
| 48000 | 10 |
| 88200 | 11 |
| 96000 | 12 |
| 176400 | 13 |
| 192000 | 14 |

These codes are recovered from `mt6789_general_rate_transform` at .text
0xf0c8..0xf2b8. `mt6789_memif_fs` calls `mt6789_rate_transform`, which selects
the general mapping for ID 0; IDs 9/11/33 have other mappings and are outside
this API. Although the general function handles other rates and defaults unknown
rates to a code, this API rejects anything outside the advertised DL1 list.

The common module's `mtk_memif_set_format` at 0x1594..0x15a4 branches S16
(format index 2) to 0x1624: unshifted HD and alignment values are both zero.
S24 (index 6) branches via 0x15a8..0x15b0 to 0x15ec..0x1600: HD is 1 and
alignment depends on descriptor offset 0x70 being positive. DL1's descriptor
at AFE .rodata 0x1148 has zero there, so alignment is zero. S24_LE stores its
24 significant bits in a 32-bit container, not the packed three-byte variant.
S32 takes an additional runtime-dependent branch at 0x15c8; its full policy is
not established here and is deliberately rejected despite being advertised.

For channels, DL1 descriptor offsets 0x38/0x3c are shift 8 and invert 0.
`mtk_memif_set_channel` at 0x12e0..0x1304 therefore produces unshifted mono=1
for one channel and 0 for two. Other channel counts are rejected. These are
field values only: the utility emits no register addresses, shifted register
images or writes. SampleBytes/FrameBytes describe PCM storage geometry and can
be supplied to the separate geometry utility; they do not configure a device.

No unsupported-format fallback, clock setting, power action, MMIO/DMA access,
stream start or end-to-end audio route was added. Failure preserves the output.
Tests cover every integer rate through 384001, both accepted formats/channels,
invalid format/channel ranges and UINT32_MAX, with no hardware operation.
