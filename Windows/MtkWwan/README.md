# T70 CCCI/CCIF ring and CCMNI channel core

`CcciRing.c` implements the shared-memory ring layout and CCIF frame checks
used by the stock CCCI control transport. It validates guard words, pointer
ranges, packet markers, minimum message size, alignment, wraparound, and free
space before exposing a message. DpmaifRing.c ports the stock BAT/PIT/DRB
producer, consumer and release index arithmetic, including the one-empty-slot
rule and stale hardware-index rejection. DpmaifBatCore tracks BAT slot and
buffer-token ownership and validates PIT buffer IDs, DMA address ranges, offsets
and lengths. DpmaifResources validates the four stock DPMAIF MMIO windows, the
level/high exclusive interrupt, and the fixed 64-byte CCCI handoff resource
from the HANA6797 ACPI _CRS. UEFI reads only LK's CCCI header-count and
header-table tags, reserves the parsed MD_SYS1 range, and publishes its
validated address and length in the handoff record. The Windows miniport maps
and validates that record; modem initialization does not consume it yet. DpmaifWdmDma.c requests a WDM DMA adapter using
DEVICE_DESCRIPTION_VERSION3 and the kernel-derived 36-bit address width. It
provides common-buffer allocation/free, alignment and address-range checks,
WDM scatter/gather map/put lifetime, and conversion of the returned list to
DPMAIF TX segments. DpmaifSgCore coalesces adjacent ranges and rejects length,
byte-count, capacity, and DMA-aperture errors before modifying the list. The
mapping reference stays held until PutScatterGatherList, and adapter release
returns busy while any mapping or common buffer remains outstanding. The raw
descriptor codec continues to represent the full 40-bit wire field.

MtkWwan is now linked as an NDIS miniport INF/SYS for ACPI HANA6797. Its
MiniportInitializeEx validates raw and translated resources, maps the four
MMIO windows and the CCCI handoff record, reads and validates its bounds,
and initializes the WDM DMA adapter. It allocates and zeroes separate
64-byte-aligned common buffers for the 32,768-entry PIT, two 16,384-entry BAT
tables and four 2,048-entry TX DRBs (851,968 bytes total), matching the stock
separate DMA allocations. When the CCCI handoff reports that LK found no MD1 image-load error, it validates and stores the
source-ordered common/interrupt/queue/init-done plan without applying it. This flag is not a runtime modem-boot or clock/power readiness proof.
It also requests separate WDM common-buffer vectors for 16,383 request
buffers (3,584 bytes each) and 16,383 fragment buffers (128 bytes each),
checks each returned address against the 128-byte alignment and 36-bit DMA
aperture, initializes the BAT ownership slots, and encodes the initial
count-minus-one BAT entries. The final table slot remains empty. The buffers
are prepared in memory only; DPMAIF registers and interrupts remain untouched.
DriverCaps and DeviceCaps return NOT_SUPPORTED until the WWAN model is
implemented end-to-end, so the MB Service will ignore the adapter. Send
requests complete as disconnected.
Portable source-locked common/IRQ/queue register plans and a bounded fake-MMIO
runner exist. The miniport stores the combined source-ordered startup plan only when LK reports no MD1 image-load error. It does not apply the plan, program DPMAIF registers, register the IRQ,
refill or reclaim BAT buffers, submit rings, boot the modem, or implement
CCCI/MBIM data control. The ARM64 link is unsigned; the package check passed,
but no Windows load or PnP execution was performed. Build record:
Windows/Build/wwan-rx-bat-vectors-20261123/mtkwwan-verification.json.

Microsoft recommends WDF/WDM DMA for ARM64 rather than NDIS scatter/gather
DMA; the implementation follows that recommendation
([IoGetDmaAdapter](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iogetdmaadapter),
[DMA_OPERATIONS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_dma_operations),
[GetScatterGatherList](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pget-scatter-gather-list)).

The wire format is based on the stock reference files
`kernel-reference/mindone/modules/ccci_ccif/ccci_ringbuf.c` and
`kernel-reference/mindone/modules/ccci_md_all/hif/ccci_ringbuf.h`. The
15-bit receive sequence check follows `kernel-reference/mindone/modules/ccci_md_all/hif/ccci_hif_cldma.c`;
the 16-byte header layout follows
`kernel-reference/mindone/modules/include/drivers/misc/mediatek/include/mt-plat/mtk_ccci_common.h`.
Their SHA-256 values at the time of this port were
`e1fd1b18fa30bc2558eaed4b8156bed6070c9d1251e07c998857b6ea1f552c7d`,
`eeb7deb3d8be64b2cc8a4925de4b922cdf92a990adea1f482a6e37fd156eebc3`,
`78acfedd59053f62a281a04d771280006bab63e614ba2bb5c66ad443a130ae00`, and
`f422efd13e6e8c7eb8725dfc1634c5b953a800b95dba517e975e09fd1919ad36`.
The ring implementation retains the source license (`GPL-2.0-only`).

Run the production C core under address and undefined-behavior sanitizers with:

```sh
python3 Windows/MtkWwan/test-ring.py
```

`python3 Windows/build.py --wwan-ring-test --output-dir Build/NAME` also
compiles the CCCI ring, CCMNI table, DPMAIF cores, WDM DMA adapter and NDIS miniport into ARM64 Windows objects and an unsigned SYS/INF package. Host checks exercise the portable cores; Windows DMA callbacks and NDIS loading are not executed.

CcciNet.c ports the stock CCMNI index-to-channel table plus reverse lookup by
RX or TX channel. It preserves index 8 as the unused LAN-replica placeholder,
per-interface DL-ACK choices, no-ACK sentinels, and the MD_SYS1 multi-queue flag. The T70 stock DT sets
mediatek,md_generation to 0x1897 (6295), which selects the non-6293 port table.
The mapping source hashes are port_net.c
56ce39691d52d6cc34766a7659ae74b0ff76f17d6aa3b31c8bc6ffbfabbc687c,
port_cfg.c dcc5521fb81e00a866b9b94cdf59c43757459f3a3a917cc45af25a78571700f6,
ccci_core.h b2a861230ae814a20f5c14e4390a69d5818168b0e9d5f0ffe3c7895a65c0fefa,
and mt6789.dtsi c4439005ebcb7a8e5e64fde976214209eaffcfc9b157b5d7b89741c8dd2a9f09.
The channel table is now used by the NDIS miniport package described below; the channel map itself still does not provide a working network path.

The 16-byte CCCI message header is decoded without treating `data[1]` as an
RX length. The stock 15-bit per-channel receive-sequence check is also
implemented; it reports a gap without selecting modem recovery policy. The
ASan/UBSan host checks cover all 22 CCMNI indices, ACK sentinels, the MD_SYS1
multi-queue flag, ring initialization, exact frame layout, TX backpressure,
RX/TX wraparound, malformed markers and lengths, incomplete messages, and
read-buffer size errors. The combined CCCI/CCMNI test passes 236 checks. The
ARM64 Windows kernel objects built by the command above are CCCI ring 6,653
bytes, CCMNI map 2,339 bytes, DPMAIF descriptor 6,236 bytes, ring lifecycle
3,648 bytes, BAT ownership 4,866 bytes, TX packet plan 1,679 bytes, ACPI resource mapper 3,412 bytes, SG normalizer 1,055 bytes, and WDM DMA adapter 8,033 bytes. The PnP miniport invokes WDM common-buffer allocation during initialization
when LK reports no MD1 image-load error. This flag does not prove live modem power or boot. The production WDM common-buffer-vector callbacks are exercised by the host
harness: allocation/free lifetime, indexed-element address/alignment checks,
out-of-range addresses, null allocation, unsupported callbacks and failure
cleanup pass 86 checks. Scatter/gather mapping is not yet used by the miniport. DPMAIF register programming, interrupt handling, BAT refill/reclaim, packet completion, modem boot and Windows NDIS/MBIM behavior remain unimplemented.

## DPMAIF v3 descriptor decoder

DpmaifDescriptor.c decodes normal and message PIT entries, BAT entries, and uplink data/message descriptors as explicit little-endian words. It encodes BAT addresses plus the stock uplink message/data descriptors emitted by set_drb_msg/set_drb_payload. Field layouts come from T70 kernel-reference commit a71515dbaf0d539ffb36416f14186dad17eef743, header mindone/modules/include/drivers/misc/mediatek/eccci/hif/ccci_hif_dpmaif_v3.h (SHA-256 b3d8edac78acc50b77b3c7d5a212e05d378c5f76dab649317a90b99868f889b4). The decoder preserves reserved fields and combines buffer-ID extension bits. T70DpmaifRxWalkStep carries message-PIT channel/checksum metadata across data PIT entries and marks c_bit=0 as packet completion. DpmaifRing ports stock RX PIT observation/consumption/credit release and TX DRB reserve/commit/completion/release index arithmetic, including the one-empty-slot producer rule. The WDM adapter wrapper requests the source-derived 36-bit DMA width, allocates and frees common buffers, maps MDLs to WDM scatter/gather lists, and retains its lifetime reference until PutScatterGatherList. DpmaifWdmNormalizeScatterGatherList converts and validates mapped ranges before the existing TX packet planner. CcciCcifCore ports the stock 24-channel AP CCIF send busy check/write order and normal/exception receive acknowledgement masks. Its 17 host checks cover busy/no-space/invalid channels, write order, and both IRQ masks; the core is not yet called by the NDIS miniport. The HANA6798 resource mapper validates the two stock CCIF MMIO windows and both level/high IRQs, but no FDO consumes it. DpmaifHwCore now plans the stock common SRAM clear and MD_SYS1 remap, interrupt masking, RX PIT/request+fragment BAT, four TX DRB queues, and final init-done writes. Its bounded callback executor is tested with fake MMIO for register updates, masked polls, timeout, cancellation, malformed plans, and I/O errors. The miniport now initializes the WDM DMA adapter, allocates/zeros the source-sized separate ring-table buffers, and stores the combined source-ordered startup plan when LK reports no MD1 image-load error, but does not apply it to MMIO, register interrupts, refill or reclaim BAT buffers, complete packets, or implement MBIM/WWAN OIDs. Host ASan/UBSan tests cover 73 descriptor/PIT/TX/BAT encode checks, 84 ring lifecycle checks, 71 BAT slot/ownership checks, 66 TX packet-plan checks and 15 WDM SG-list normalization checks, all including 36-bit DMA boundary cases where applicable, plus 22 HANA6797 ACPI resource-map checks, 13 CCCI memory-handoff checks, 32 separate-table layout checks, and 139 DPMAIF startup-plan/fake-MMIO executor checks, 17 CCCI CCIF send/ack checks, and 16 HANA6798 CCIF resource-map checks.

python3 Windows/MtkWwan/test-ring.py runs the portable CCCI/CCIF send/ack, CCMNI, DPMAIF descriptor, ring, BAT, TX, DMA, HANA6797/HANA6798 resource-map, startup-plan, and fake-MMIO suites under ASan/UBSan. Windows/build.py --wwan-ring-test compiles those portable cores and the queue-layout helper as ARM64 objects and does not link NDIS. Windows/build.py --wwan-driver builds and links the unsigned NDIS miniport and runs source/INF package checks. Neither target loads the driver or applies the startup plans to mapped hardware.
