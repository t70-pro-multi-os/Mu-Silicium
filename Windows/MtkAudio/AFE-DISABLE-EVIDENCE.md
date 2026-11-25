# DL1 disable request is not DMA drain or buffer release

Stock modules and hashes are recorded in AFE-GEOMETRY-EVIDENCE.md. Read-only
inspection gives the following concrete path:

- Common `mtk_afe_fe_trigger` calls `mtk_memif_set_disable` at 0x1e9c, saves its
  status in w20, updates assigned IRQ enable at 0x1ed4, then writes the assigned
  IRQ clear at 0x1ef4. It returns the saved disable status, not a checked result
  of all three operations. Its error logging branch also proceeds to IRQ work.
- `mtk_memif_set_disable` loads descriptor +0x54 (enable register) and +0x58
  (shift), adds 8 when AFE+0x98 is nonzero, then passes mask/value to
  regmap_update_bits_base at 0x1c30 or 0x1c94. The semaphore-controlled path is
  distinct; this component does not implement or bypass its ownership protocol.
- T70 AFE DL1 descriptor at .rodata 0x1148 has +0x54=0x1b00 and +0x58=4.
  Probe instructions 0x9f9c and 0x9fb0 set AFE+0x98 to 1. Thus the selected
  disable alias is 0x1b08 and the bit is 0x10.

The pinned [same-SoC GPL implementation](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/d26d588883468251678619757cc9f0f6a5f37cb8/sound/soc/mediatek/common/mtk-afe-fe-dai.c)
labels the bit-banding write-one action as memif disable. Its stop sequence is
disable, IRQ disable, pending clear. It does not establish a Windows callback
rundown or a DMA completion/idle contract. No source implementation was copied;
the small callback helper was independently written from the observed register
facts. This source is another board, not evidence of T70 codec/power wiring.

`T70AfeDl1RequestDisable` only reads the observed alias and requests its bit.
Stock uses a generic masked update. Because other alias readback bits have not
been characterized, this implementation refuses any value other than 0/0x10,
rather than replay unknown bits in a write. For 0x10 it returns an observation
without writing, preserving non-forced masked-update no-change behavior; only
zero readback leads to a write. It may reject a legitimate readback;
that restriction does not prove alias read semantics. It neither polls nor
interprets readback as stopped. A failing write may have applied. Every result
requires retaining the allocation and existing configuration lease.

No assigned IRQ is invented, no IRQ clear is issued, and no BASE/END zeroing is
added. The inspected stop branch does not reset those addresses. There is no
proven hardware idle acknowledgment, outstanding-bus-transaction drain, interrupt
callback rundown or safe allocation release established here. Consequently this
helper is intentionally not connected to CommonBufferEnd/Release. A future full
stop provider must settle those obligations before releasing the retained lease.
Caller must own the complete AFE access/serialization interval, including any
cross-processor arbitration; neither callback existence nor the numeric register
address provides that guarantee. No actual register/clock/power/DMA access ran.

Host tests cover every low-16-bit readback with zero/all-one upper halves,
nonzero signed/positive read/write errors, strict no-write rejection and null
callback preflight. ARM64 clang-cl object compilation also succeeded with the
official SDK/WDK include environment and no mock includes. This pure callback
file uses no WDF API, so that compile is not a new WDF lifecycle validation.

## Normal STOP contract versus Windows PAUSE (follow-up)

The earlier demand for an independently polled DMA-idle flag is not present in
stock normal STOP. The T70 trigger returns after memif disable, IRQ disable and
clear; common `mtk_afe_fe_hw_free` contains the ordinary
`snd_pcm_lib_free_pages` path (stock .text0x1afc). For the same exclusively owned,
accessible normal hardware path, the factory now executes this STOP sequence
before asking its external provider to drain CPU/queued users and releasing the
buffer. This is adoption of the stock driver's normal-stop contract, not a new
silicon-level bus-drain guarantee. Register failure or interrupted ownership
still retains the allocation; no guessed timeout/idle register is introduced.

The T70 prepare path (`mtk_afe_fe_prepare` .text0x2148, call0x21b8) delegates to
`mtk_memif_set_pbuf_size` (.text0x2228). DL1 descriptor +0xa4..0xb8 encodes
0x4c/mask3/shift12 followed by0x4c/mask15/shift20. T70
`mt6789_get_memif_pbuf_size` (.text0x41c) divides period_frames*1000 by rate;
integer quotient >10 selects3, otherwise0. This is32/256-byte prefetch selection,
not a CUR-reset operation. `AfePrefetch` independently implements these two
masked updates, preserving other bits and stopping on any transport error.
Stock ignores their return codes; our error propagation deliberately does not.
The helper is actually called after address/format configuration on ACQUIRE.
No CUR/base-zero write or invented reset sequence has been added.

However stock capabilities omit ALSA PAUSE, and the normal trigger accepts
START and STOP/SUSPEND, not RESUME or PAUSE_PUSH/RELEASE. The separate common
trigger supports RESUME, but the actual MT6789 ops select `mt6789_fe_trigger`
at .rodata0x4848+0xb0. The dedicated trigger's 0x108..0x118 dispatch accepts
commands 0/5/1; others reach -EINVAL at 0x2b4. Stock `mt6789_afe_hardware.info`
at 0x85c0 is 0x00800103, omitting both PAUSE 0x80000 and RESUME 0x40000. The
[pinned MT6789 source](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/d26d588883468251678619757cc9f0f6a5f37cb8/sound/soc/mediatek/mt6789/mt6789-afe-pcm.c)
agrees. These are read-only source/binary observations, not a device test.
Absence of a CUR write establishes neither hardware retention nor reset on
memif disable/enable. That cannot establish
position-preserving Windows pause. An attempted direct-STOP-only factory policy
was rejected in independent review and removed BEFORE commit: PortCls always
uses RUN->PAUSE->ACQUIRE->STOP, and its GetPosition freezes/unfreezes the actual
cyclic-buffer position. Keeping a software number while hardware restarts from
BASE would not preserve the data sequence. The existing explicit real pause
provider is therefore retained; no default no-op pretends to implement it.

Official Microsoft WDK8 MSVAD archive basewave.cpp SetState preserves
m_ulDmaPosition at PAUSE, reactivates at RUN, and resets it only at STOP. This
supports the Windows meaning but its virtual timer is NOT T70 DMA evidence.
The review did not locate an official AC97 source to establish another hardware
contract. No timer or speculative paused-buffer relocation was ported.

https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-setstate
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-getposition

Thus normal STOP and prepare now have concrete code, while physical PAUSE
position retention/resume remains a distinct real platform obligation. Power,
clock, route and ownership admission are unchanged. No actual device ran.
