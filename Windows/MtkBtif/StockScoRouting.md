# T70 Pro stock Bluetooth SCO routing evidence

The effective stock DTB (SHA-256 `a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391`)
describes `/mtk-btcvsd-snd@18050000` with `compatible = "mediatek,mtk-btcvsd-snd"`.
It is not disabled. The root interrupt parent is the GICv3 with four interrupt cells.

| Stock field | Value |
|---|---|
| CONN SCO control region | `0x18050000 / 0x1000` |
| BT audio SRAM region | `0x18080000 / 0x10000` |
| Interrupt | SPI `0x197` (407), level high; GSIV 439 |
| Infracfg reference | Stock phandle `0x2b` |
| mediatek,offset | `0xf00, 0x800, 0x140, 0x144, 0x148` |
| disable_write_silence | 1 |

The stock `vendor_dlkm_a/mtk-btcvsd.ko` (SHA-256
`95367cbe5b48ee4258159a6862a4432042d3120f2b18f3645432826cc9b960ab`)
exports the matching OF alias and contains the SCO IRQ handler, TX/RX
operations, and ASoC component registration. The Windows ISR uses the status
clear mask from the linked Android common source below; that mask has not been
binary-matched against the exact T70 module. This is an independent path:
the stock AFE's `CONNSYS_I2S` DAI is capture-only, and its bidirectional
`PCM 2` routes are `PCM 2 Playback -> AFE_TO_MD1` and
`MD1_TO_AFE -> PCM 2 Capture`.

## Interrupt polarity at the parent GIC

The exact T70 stock object's `mtk_btcvsd_snd_probe` passes zero in AArch64
register `x4` at offset `0x1c4`, immediately before the call at `0x1cc`
to `devm_request_threaded_irq`. The request has no trigger override and
the module does not import `irq_set_irq_type`. The SCO node inherits the
root's direct `arm,gic-v3` parent (phandle 1); its DT interrupt tuple is
`<0 0x197 4 0>`. The effective stock DT has no explicit MediaTek SYSIRQ
controller in that path. Thus level-high at GSIV 439 follows the T70
firmware-visible resource; the level-low flag in the published reference
driver is not a property of this T70 module. This says nothing about
the electrical signal before the GIC or undocumented boot-time setup.

The current Linux module archive contains the unmodified SCO module under
the audio plan. The node, module and archived bytes do not prove successful
binding, firmware handshakes, or simultaneous capture/playback.

The Windows `WCN0` ACPI resource `0x18000000 / 0x100000` already contains
both SCO MMIO ranges. Do not assign those same addresses to a second
ACPI resource owner. The WCN0 ACPI owner now carries both WLAN GSIV 441 and the distinct SCO
GSIV 439, and MtkConn registers a WDF ISR for 439 that acknowledges stock
status bits while the source remains masked. Streaming still needs coordinated
CONN lifetime/ownership, stock SRAM/voice-channel transfer, and a verified
Windows audio endpoint. A literal SCO capability value alone is not proof
that any of these exists. See the
[Microsoft BTHX capability contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthxddi/ns-bthxddi-_bthx_capabilities)
and the
[MediaTek Linux SCO PCM driver](https://android.googlesource.com/kernel/common/+/68fdd204428bfe38c87390956faf10b560db71a7/sound/soc/mediatek/common/mtk-btcvsd.c)
for the OS and reference-driver boundaries, respectively.
