# DL1 stream lifecycle integration: missing platform provider

This review examined the current `Windows/MtkAudio` interfaces and the already
recorded stock evidence. It did not revisit blocked power, USB, GPU or firmware
build work, and did not execute hardware operations.

## What can actually be called now

- `T70AfeDl1Prepare`: validates values and produces a format/geometry snapshot.
  Its DMA address is an integer supplied by the caller, not a retained mapping.
- `T70AfeDl1Configure`: now connects address writes and format field updates
  through an explicit provider callback. Completion is not readback or playback
  readiness; no concrete MMIO provider, buffer lease or IRQ owner is supplied.
- `T70AwPairProfileView`: validates both board-channel catalogs and publishes
  borrowed register-bin views. It does not apply either amplifier profile.
- `T70AfeDl1IrqCount`: validates that snapshot and returns a count. It neither
  acquires an IRQ nor proves exclusive DL1 ownership.
- `T70AfeDl1NormalizedPosition`: converts a supplied CUR address. It does not
  obtain coherent hardware samples or pin the buffer while reading.
- AW identity/status and pair-probe callbacks access amplifier register values.
  The optional `AwSpb` adapter uses an already owned amplifier I2C target. It is
  not an AFE MMIO, PCM DMA or IRQ provider and cannot substitute for one.

No production DL1 resource creator, mapping lease, IRQ owner, start/stop backend
or coherent CUR provider exists in this audio component. The current test
callbacks are mocks, not a platform implementation. An invented callback that
returns success would not turn these routines into a playback connection.

## Contracts needed before a real lifetime state machine

| Operation | Missing provider guarantee |
| --- | --- |
| Prepare | Pin/retain the actual DMA allocation and mapping; validate access/cache ownership and exclusive DL1/IRQ resources; unwind only acquired resources on failure. |
| Start | Apply a verified device configuration under the correct ownership; report whether failure caused partial hardware activity. |
| Stop | Prove DMA is quiescent and callbacks/IRQs drained before releasing memory; failed stop must not pretend the resource is free. |
| Position | Supply a coherent full CUR from the same live allocation under lifetime/serialization protection; reject stale generation or removal. |

These are missing contracts, not a newly invented API or an implemented sequence.
The earlier stock IRQ review establishes a frame counter and dynamic IRQ
assignment, not a portable resource allocator or a safe rollback/quiescence
protocol. The CUR review establishes arithmetic but explicitly leaves coherent
sampling to a future provider. Amplifier clock snapshots likewise cannot prove
AFE DMA ownership or safe teardown.

## Outcome

No synthetic Running transition, mock-backed production provider, presumed PnP
binding or unconditional-success start/stop callback was added. Prepare remains
arithmetic-only. Start/stop/position acquisition remain unavailable rather than
being advertised as functional. A wrapper that merely adds state labels and
always rejects start would not satisfy the requested real connection progress.

The next meaningful implementation must supply and verify an explicit platform
resource provider, with failure ownership and quiescence semantics, within the
permitted scope. Until that exists, failure-injection lifecycle tests would validate only
a proposed contract, not a real platform connection. This is a limitation record; no new playback code or
new test-success claim is made. Existing cores and tests are left unchanged.

## Post-configure prerequisites (2026-11-18)

This is a dependency inventory, NOT a proposed start or power sequence. Existing
stock evidence supports the following distinctions:

| Dependency | Evidence already established | Missing real connection |
| --- | --- | --- |
| DMA allocation and mapping | Geometry evidence records a 34-bit DMA mask and inclusive END; stock hw_params uses runtime allocation/address/size before set_addr at 0xd34. | An accessible, retained platform DMA allocation, cache coherency/ownership and lifetime guarantee. An integer base within 34 bits is not such a mapping. |
| IRQ ownership | IRQ-count evidence records 27 descriptors and dynamic assignment; common startup/acquire paths were identified at 0x81c..0x888 / 0x918..0x998. Trigger consumes the assigned descriptor and period frames, not a fixed DL1 IRQ. | Exclusive assignment tied to the same DL1 stream, actual interrupt registration/dispatch, acknowledgement and callback-drain ownership. The pure count does not acquire any of these. |
| Codec/amp route and state | Board evidence identifies two AW88261 targets; current identity/clock snapshots prove only the observed ID and bits 0/4 in their respective reads. | Verified PCM route and codec/amp configuration/state agreement for both targets. No MT6366 route or AW startup completion contract is implemented by these snapshots. No unverified sequence is prescribed here. |
| Position | CUR evidence establishes low-before-high order but not coherent sampling. | A same-allocation coherent reader and lifetime protection; normalized arithmetic cannot supply them. |

`T70AfeDl1Configure` assumes quiescence/exclusive ownership BEFORE its first
write. It cannot establish them afterward. On an address or format failure,
completed writes remain and a failing write may also have applied. Progress
counts therefore do not authorize automatic retry, DMA start or memory release.
Even all callbacks completing successfully leaves every provider dependency
above outstanding. No combination of `T70AfeConfigCallbacksCompleted`, a valid profile view and
`ClockPairSet` is a substitute for a verified hardware-ready state.

No independent production start/ownership implementation is justified by the
currently inspected evidence. Adding a boolean ready flag, fixed IRQ selection
or a mock-backed allocator would conceal the missing provider rather than solve
it. This update records the newly connected configure/profile APIs and their
remaining boundaries only. No start/power code, live access or new test run was
added. Prior host successes test value and callback contracts, not playback.

## SPB target lifetime and concurrent teardown (2026-11-18)

A symbol search of Windows C/header sources finds `T70AwSpb*` only in
`AwSpb.c`, `AwSpb.h` and `SpbTest.c`. There is no production caller in that
scope that acquires/releases a target lease, gates new calls during removal,
or serializes teardown with these operations. The portable pair APIs accept
caller-owned contexts and likewise do not create such a provider.

The header's pinned-target and serialization requirements are preconditions,
not implemented guarantees. Non-NULL handles are not proof of validity.
[WdfObjectReference](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/wdfobjectreference)
requires a valid object handle and must be paired with dereference. Adding a
reference inside AwSpb cannot repair a handle already invalidated before entry.
Object retention also must not be confused with an open, usable target:
[WdfIoTargetClose](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetclose)
closes the target and cancels queued requests; it is distinct from object
reference counting. Removal can therefore still produce an I/O failure.

A real provider must establish admission/lifetime protection before publishing
a handle, prevent new admissions during teardown, and retain contexts through
all admitted operations. It must define drain/cancellation and lock ordering
against the actual target owner, rather than add a lock that teardown ignores.
Register read-modify-write requires exclusion across BOTH sends, including
volume and TX updates, not a separate lock per read/write callback. Pair probe
requires both endpoint contexts retained and a consistent lock order; its
sequential observations are still not an atomic hardware snapshot. These are
missing integration obligations, not a newly implemented removal protocol.

The current synchronous sends use internal requests (`Request=NULL`). They do
not expose a caller-owned cancellation handle. Their timeout and failure
returns do not prove cancellation/drain of arbitrary provider callbacks, nor
undo a potentially applied write. Microsoft also warns that
[WdfIoTargetStop](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetstop)
is not safe when called simultaneously from different threads. No synthetic
reference/lock/Stop wrapper was added without the missing owner and callers.

This is a static contract review only. Host mocks do not model KMDF object
removal, wait dependencies or target deletion. No new tests, WDK build or live
I/O were performed, and no runtime lifetime-safety claim is made.

## Connection-ID target ownership review

Microsoft documents a concrete route from an externally assigned connection ID
through RESOURCE_HUB_CREATE_PATH_FROM_ID to WdfIoTargetOpen:
https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/connection-ids-for-spb-connected-peripheral-devices
The ID is not the I2C slave address or a value derivable from the T70 DT tuple.
Its association with I2C1/channel 0 or 1 must come from the actual resource
supplier; a successful open alone cannot establish the board association.

The missing lifetime owner matters independently of path construction.
WdfIoTargetCreate requires a valid WDFDEVICE as its Device argument. A custom
IoTargetAttributes.ParentObject must lead to that device in its parent chain.
The target is deleted when the device is deleted:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetcreate
The current audio scope has no such actual device owner or its teardown
integration. A local externally serialized lease counter cannot, by itself,
prevent that independent parent cleanup from invalidating a stored target.

There is useful framework removal behavior, so lack of custom removal callbacks
must NOT be described as lack of all removal handling: absent those callbacks,
KMDF automatically stops/closes a removed remote target and cancels its queued
requests. This is documented at:
https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/controlling-a-general-i-o-target-s-state
It does not make a previously opened target permanently usable or provide this
component's caller/context lifetime protocol. Registering custom callbacks would
also assume responsibility for the documented close/reopen behavior, not solve
it by declaration. No automatic enumeration or board identification is assumed.

A create/open helper alone is technically expressible, but publishing it as a
safe target owner with close/drain would leave parent cleanup, admission,
callback/context retention and whole-RMW serialization unconnected. This task
therefore adds no production open/close adapter or mock-backed success claim.
Needed next input is a real owner interface that supplies the verified ID and
parent lifetime, and participates in shutting down all admitted AwSpb calls
before target destruction. No open/close calls, I2C access, tests or WDK build
were executed for this static review.

## WaveRT consumer boundary review (2026-11-18)

The audio scope contains no IMiniportWaveRT/IMiniportWaveRTStream implementation,
PortCls stream owner or AllocateAudioBuffer implementation. The only C callers
of T70WaveRingCopy are WaveRingTest.c. Thus another CPU wrapper would not connect
this code to an operating-system stream.

Microsoft's [WaveRT overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/understanding-the-wavert-port-driver)
describes client access to the cyclic buffer without continuous driver copying.
Our explicit Source/AvailableBytes/scratch call is not a standard WaveRT render
callback. In particular, the existing 24/32 conversion cannot simply be inserted
into a direct-mapped ring: the client may be writing that same memory, whereas
our API requires immutable source and an exclusively owned disjoint destination.
S16 avoids the representation conversion but does not supply buffer ownership.

The concrete allocation boundary is
[AllocateAudioBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-allocateaudiobuffer):
an actual IPortWaveRTStream supplies page/MDL allocation, and the miniport returns
MDL, actual size, first-page offset and cache type. Actual size must not be less
than requested. A fixed-size synthetic CPU array or a numeric DmaBase is not
this contract. The existing DL1 base/end model requires a verified device-visible
contiguous range within 34 bits; an MDL alone does not prove that mapping or the
platform's cache/coherency requirements. No Intel-specific cache choice is
transferred to this MediaTek target.

[SetState](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-setstate)
starts at STOP and implements the documented transport-state transitions.
Returning RUN success without a real backend would be false playback success.
[GetPosition](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-getposition)
reports byte offsets in the cyclic buffer, not the producer copy's NextWriteOffset.
The [official SysVAD stream example](https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/EndpointsCommon/minwavertstream.cpp)
uses a performance-counter-driven UpdatePosition in its GetPosition path. That
virtual-device behavior is not evidence of T70 DMA progress and must not be
copied as a hardware position provider. No sample source was copied.

Missing concrete interfaces before a real connection can be implemented:
- A real PortCls/stream owner supplying bounded format data, reference lifetime,
  allocation/free and teardown serialization, without assuming enumeration.
- MDL/device-address mapping and cache ownership for the actual buffer, including
  cleanup on every partial allocation failure and outstanding access drain.
- A defined direct-buffer format policy. A conversion path would instead need
  real staging ownership/publication, scheduling and position correspondence;
  the current caller-authoritative AvailableBytes does not create those.
- Verified backend start/stop and live position/notification providers. Existing
  arithmetic and observer helpers do not supply them.

The prior host inventory did not find a usable official WDK toolchain in the
checked Mac locations; the builder tool query timed out, so its absence there
was not established. This review does not repeat those checks or claim a native
WDK compile. No new adapter, fake allocation/RUN success, test, hardware access
or firmware build was added. This task stops at the missing-provider boundary;
the existing CPU helpers remain usable only by explicit callers meeting their
documented memory contracts.

## MDL/CPU/device-address validation boundary

A subsequent scoped search of the current audio C/headers found no MDL,
MapAllocatedPages, physical-page accessor or DMA-adapter provider. Accordingly
there is not yet an externally supplied *verified* descriptor to connect.
Accepting an opaque non-NULL MDL token plus caller-entered sizes would validate
numbers, not the MDL-to-CPU-to-device association.

[IPortWaveRTStream](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iportwavertstream)
offers GetPhysicalPagesCount/GetPhysicalPageAddress for an owned MDL.
[MapAllocatedPages](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-mapallocatedpages)
returns a contiguous *virtual* mapping or NULL; that does not establish physical
contiguity. Its accepted cache values are MmNonCached, MmCached and
MmWriteCombined. Membership in this enum is not proof of T70 coherency, safe
aliases or device access. The real owner must supply the chosen cache policy
and keep the mapping/MDL alive throughout validation and every later access.

The eventual bounded validation boundary must distinguish:
- Requested/actual buffer length and first-page offset, checked against the
  actual MDL extent and CPU mapping capacity; actual length >= requested.
- A physical-page observation obtained from the owning stream, not an invented
  PFN list. Adjacent pages can establish physical contiguity of that range but
  do not by themselves establish the target's device-visible mapping.
- A live device-visible mapping supplied by the actual platform owner, covering
  the complete buffer with the single base/end range required by this DL1 core.
  A CPU address, first physical page or fabricated identity-map flag is not
  accepted as a substitute.
- Numeric format/period/34-bit inclusive-end constraints, already implemented by
  T70AfeDl1Prepare. That function remains numeric validation only.

Release is also an ownership contract:
[FreeAudioBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-freeaudiobuffer)
takes the previously allocated MDL and runs at PASSIVE_LEVEL.
The published BufferSize prose appears inconsistent with its ULONG signature;
no pointer interpretation is derived from that paragraph.
[UnmapAllocatedPages](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-unmapallocatedpages)
must precede FreePagesFromMdl, which frees both pages and the MDL.
Neither checking numeric lengths nor retaining a raw pointer prevents a
concurrent free. The owner must serialize/drain all readers before release;
this review adds no allocator, release path or lifetime guarantee.

No new descriptor API was added: the meaningful unimplemented step is the real
owner's observation/mapping contract, not another arithmetic wrapper or a
mock MDL declaration. Once that interface exists, bounded metadata can be
validated before calling the existing preparation function. No new tests were
run for this documentation-only result, and no WDK/hardware validation is claimed.

## Concrete WaveRT allocation API assessment

The official API does provide physical contiguous allocation; its absence must
not be claimed. Specifically:
- [AllocatePagesForMdl](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-allocatepagesformdl)
  takes HighAddress and TotalBytes, returns PMDL or NULL, and does not promise
  adjacent physical pages.
- [AllocateContiguousPagesForMdl](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-allocatecontiguouspagesformdl)
  takes LowAddress, HighAddress and TotalBytes. It explicitly supplies contiguous
  nonpaged physical pages, rounds allocation to whole pages and returns NULL
  on failure. The documented method is intended for DMA controllers requiring
  physical contiguity. A physical upper bound of 0x3ffffffff is expressible;
  this does not alone certify this platform's device-visible range.
- The stream interface lists GetPhysicalPageAddress and GetPhysicalPagesCount.
  Its page-address helper is not named GetPhysicalAddress. The individual
  GetPhysicalPageAddress documentation endpoint was unavailable during this
  review; the interface index confirms the function's purpose, not a complete
  signature suitable for fabricating a replacement PortCls header.

These are useful real allocation interfaces, but this tree still lacks a
verified T70 Windows stream/device mapping contract. The current DL1 code
passes the prepared base directly to hardware-register value construction.
Microsoft's general [map-register contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/map-registers)
distinguishes device-accessible logical ranges from system physical pages.
The reviewed WaveRT pages do not establish T70 identity translation or the
required platform coherency. This is a missing target-specific proof, not a
claim that WaveRT cannot support DMA. Nor may
[MmGetPhysicalAddress](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmgetphysicaladdress)
be used as a shortcut: its documentation explicitly excludes DMA addressing.

Therefore no allocation adapter is added under this task's requirement to
stop if bus-address suitability cannot be established. Defining mock PortCls
types and returning the first physical page as Prepared.DmaBase would hide
both that gap and the unavailable native WDK validation. Existing test headers
contain only local ntddk/wdf/spb substitutes, not a verified PortCls interface.
No allocation, free, mapping or new test was executed. A future implementation
needs the actual stream interface/header plus the target's device-address/cache
contract; it can then select the documented contiguous allocator and retain
the real owner through matching unmap/free rather than invent an identity map.

## Stock AFE DMA translation evidence (bounded read-only check)

The existing effective.dtb SHA256 was rechecked as
a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391.
A targeted FDT property reader selected only root, /soc and
/soc/mt6789-afe-pcm@11210000 properties relevant to address cells, DMA ranges,
iommus, coherency and the AFE register tuple. It did not analyze reserved-memory,
power or USB nodes.

Root and /soc both declare two address cells and two size cells. /soc is a
simple-bus and its dma-ranges has these six big-endian 32-bit cells:
    00000000 00000000 00000000 00000000 00000010 00000000
Per the [Devicetree triplet definition](https://devicetree-specification.readthedocs.io/en/stable/devicetree-basics.html),
this describes child address 0, parent address 0, length 0x1000000000 (64 GiB).
It is positive evidence for a zero-offset DMA bus window in the stock DT
description, covering the entire numeric 34-bit range used by this AFE core.
It is not a statement that 64 GiB of installed/allocatable RAM exists.

The AFE node declares compatible mediatek,mt6789-sound and register address
0x11210000/length 0x2000. No iommus, dma-ranges or dma-coherent property was
present on that node. No iommus or dma-coherent was found on the selected
ancestors either. This narrows the earlier unknown: stock DT does not explicitly
attach this AFE node to an IOMMU, and its parent describes an identity window.
It does not prove actual firmware translation state, global absence of an IOMMU,
Windows' device-address policy or cache coherence. No live translation
registers or reserved-memory contents were inspected.

For ACPI, the stable Silicium-ACPI commit
6859d4b4e92c6c3e443c2d3094b5650f7284f667 was read via git show for
Platforms/Teclast/t70pro/AcpiTables.inf. Sources are MADT, GTDT, FADT, PPTT and
DSDT only; no IORT/SMMU table is listed. T70 Acpi.dsc selects that INF, and the
board FDF includes its ACPITABLE entry plus AcpiPlatformDxe. Existing IORT files
listed elsewhere in the repository belong to Qualcomm platforms, not this
table bundle. This is source-inclusion evidence, not a fresh firmware/table dump
or proof of Windows identity translation. DSDT content was deliberately not
opened because that area is outside this task's permitted work; therefore no
new claim about its AFE declaration or _CCA is made.

The inspected Windows audio sources expose only explicit AFE MMIO mapping of
0x11210000/0x2000 through AfeMapping/AfeMappedIo; that register mapping is not a
PCM DMA allocation or a Windows DMA resource provider. No audio/AFE INF was
found by the bounded Windows directory filename check. Those facts do not
create a device-visible buffer mapping contract.

Conclusion: the stock DT identity window is now supported by direct data, not
guessed. Transferring that description into a Windows allocation guarantee is
still unproven. No new allocator, identity-map flag, ACPI modification or
hardware access was added. This documentation-only check ran no firmware build
or runtime tests, and preserved concurrent source/index work.

## DMA common-buffer route: logical address is available, memory type matters

[AllocateCommonBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pallocate_common_buffer)
does solve a different problem from an MDL physical-page query: given the real
device's DMA_ADAPTER, it allocates/maps CPU and device access, supplies the
device's logical address, and makes the memory appear contiguous to the device.
That logical result, not MmGetPhysicalAddress or the DT identity assumption,
is the address a future DL1 owner must use. The ordinary routine's CacheEnabled
argument is documented as ignored; OS architecture/bus and ARM ACPI _CCA govern
the effective policy. Passing FALSE is not proof of an uncached allocation.

[AllocateCommonBufferEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pallocate_common_buffer_ex)
adds a maximum logical address and NUMA preference through version-3 DMA
operations. Thus a real version-3 adapter can request a bounded logical range;
returned base plus complete buffer length must still pass the existing 34-bit
inclusive-end validation. Do not silently fall back to an older allocation
function when required bounds/cache capabilities are absent.

A newly identified integration constraint is material: on ARM/ARM64 the
documented uncached common buffer is Device Memory, requiring naturally aligned
accesses. _CCA takes precedence over the requested cache setting. The existing
PcmRingCopy/WaveRing CPU helpers explicitly require ordinary memory and use
memcpy; they cannot automatically consume this mapping. Claiming ordinary RAM
merely because allocation returned a CPU pointer would violate their contract.
This review does not introduce a Device Memory copy implementation or infer
the actual target's _CCA from stock DT.

[FreeCommonBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pfree_common_buffer)
releases the complete allocation and its logical/virtual mappings with exactly
matching adapter, length, address and cache arguments; partial free is not
supported and it returns no status. A future owner must retain the returned
allocation tuple even when a later validation fails, free it exactly once,
and retain the externally owned adapter through all accesses and cleanup.
Acquisition/free require PASSIVE_LEVEL; no device use may outlive release.
A borrowed adapter must not be independently destroyed by this component.

The current audio source has no DMA_ADAPTER/DMA_OPERATIONS provider or official
DMA header validation. Its ntddk host header is explicitly a small substitute
and contains neither type. Under this task's requirement to stop when WDK types
are unverified, no native owner or fake structure layout was introduced.
The missing pieces are now specific: a verified modern adapter interface and
its lifetime owner, plus an effective memory-type/access contract compatible
with the eventual stream consumer. Lack of Windows identity-address proof is
not itself a blocker when a real common-buffer logical address is available.
No allocation, free, device access, cache operation or new host test ran here.

## Remote target removal design review after explicit owner

This supersedes the earlier inventory's claim that only AwSpb/SpbTest exist:
`AwSpbPair` now connects borrowed targets to the pair observer, and `AwSpbTarget`
creates/opens an explicitly supplied Resource Hub connection and bounds its own
Release with leases. Neither is an asynchronous remote-removal provider. A
current audio-source search still finds no WdfDeviceCreate, parent cleanup /
release-hardware implementation, registered target context, WDF wait lock, or
object-reference/rundown owner. External device creation is allowed, but its
teardown contract has not been connected here.

Confirmed official obligations:

- [QueryRemove](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nc-wdfiotarget-evt_wdf_io_target_query_remove)
  must prevent new sends and call CloseForQueryRemove before allowing removal;
  it can reject a query with STATUS_UNSUCCESSFUL.
- [RemoveComplete](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nc-wdfiotarget-evt_wdf_io_target_remove_complete)
  also occurs for surprise removal, regardless of an active lease, and must
  permanently close the target. A successful Begin cannot promise uninterrupted
  I/O. Consumers must handle canceled/failed sends and still release their lease.
- [RemoveCanceled](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nc-wdfiotarget-evt_wdf_io_target_remove_canceled)
  needs reopen after query-close before sends resume. Failed reopen must not
  publish an open/admitting state.
- [WDF synchronization](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/synchronization-techniques-for-wdf-drivers)
  does not automatically serialize I/O-target callbacks. Parent queue settings
  therefore cannot be substituted for target-state synchronization.
- [WdfObjectDelete](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfobject/nf-wdfobject-wdfobjectdelete)
  may return before destruction. A zero local callback counter is not evidence
  that callbacks waiting to enter, or their context/lock accesses, have drained.

Rejected incomplete design: registering three callbacks around the existing
counter without a retained WDF context and callback/API admission protocol.
Concrete races requiring resolution before implementing it:

1. Query marks admission closed, drops its lock, then calls query-close; Release
   can otherwise close/delete the target in that unlocked interval.
2. Reopen success can overwrite a concurrently published Removed/Releasing state.
3. A callback waiting for the lock is absent from a counter incremented only
   after lock acquisition. Counter zero cannot authorize context/lock destruction.
4. Holding the state lock through a potentially canceling close/open/delete can
   conflict with completion or reentrant callbacks. No such lock-order proof has
   been established for a real caller in this tree.

A candidate implementation needs explicit transition states (Open, QueryClosing,
QueryClosed, Reopening, Removed, Releasing), operation pins for unlocked WDF
calls, retained context lifetime before API entry, generation checks around
reopen, and a parent cleanup/rundown contract. These are design requirements,
not an implemented or validated state machine. Independent review confirmed
that the existing lease alone cannot supply them. No placeholder callbacks were
registered, no fictitious removal-safety claim added, and no live I/O performed.
The existing 291-case fake owner test and official-header object compilation
validate only the previously documented restricted owner, not these races.

## Upper driver owner and build entry inventory

The current MtkAudio C sources have no DriverEntry, WdfDriverCreate,
WdfDeviceCreate, PortCls adapter registration, IMiniportWaveRT implementation,
parent cleanup or release-hardware implementation. There is no audio INF,
vcxproj, sln or CMakeLists in the inspected audio directory. The Python host
runner compiles separate tests; the official-header commands likewise emit
individual COFF objects, not an audio driver image. Full firmware/build scripts
were not executed or changed for this investigation.

The existing MtkI2c/Driver.c does create a WDF device, register its own resource
and lifetime callbacks, and initialize an SPB controller. Its MtkI2c.inf binds
ACPI\\HANA6789. This is a controller provider, not proof that its device owns the
AFE PCM stream or the two amplifier resource-hub connections. Its callbacks,
resources and hardware lifecycle were not modified or adopted by audio.

A meaningful upper owner still needs an explicitly selected real attachment and
resource supplier: a retained framework device, the two assigned connection IDs
with board association, request admission/serialization, parent cleanup and
callback-context destruction. A PCM endpoint additionally needs actual PortCls
stream allocation, DMA mapping/cache ownership and completion/position delivery.
These dependencies must not be replaced by guessed IDs or a root-enumerated
fictional device. A no-op DriverEntry/DeviceAdd returning success would not
connect the current adapters or resolve target-removal races. Registering a
playable endpoint without those providers would misrepresent capability.

Accordingly no empty driver scaffold, fictitious INF, registry resource channel
or automatic discovery was added. This is an explicit implementation gap, not a
claim that a real upper owner is impossible. Next work needs the actual parent's
attachment/resource and teardown contract, then a target context whose callbacks
and admitted operations outlive asynchronous cleanup as described above.
Documentation now distinguishes official-header compilation already achieved
from a linked/loaded audio driver still absent. No new production code or runtime
concurrency test is claimed by this inventory.

## Concrete allocation owner after official-header verification

The earlier common-buffer implementation deferral on unverified WDK types is now
partly resolved. `AfeCommonBuffer` calls an explicitly supplied, real device DMA
adapter's AllocateCommonBufferEx and FreeCommonBuffer operations. It does not
obtain or invent that adapter. The caller must retain a version-3-capable adapter
and its operation table; the adapter's own Version field is not incorrectly
assumed to equal DEVICE_DESCRIPTION_VERSION3. Operation-table extent and required
function pointers are checked before allocation.

The owner requests a maximum logical address of 0x3ffffffff, uncached memory and
NUMA node zero. Returned device-logical address is revalidated through DL1Prepare
with the requested buffer/period/format. CPU physical identity is never assumed.
Invalid returned geometry or overlapping/wrapping virtual extent is freed using
the exact original adapter, length, logical address, virtual address and cache
argument. A failed acquisition preserves the owner. Begin/End reject over/underflow
and Release rejects active users; caller serialization and proven DMA quiescence
remain mandatory. The borrowed adapter is not put/released by this owner.

This is a real Windows allocation/free call path, but was executed only against
fake host operations. Official WDK/SDK headers compiled the file to ARM64 COFF
with no mock includes. It is not a loaded driver, DMA start, clock/power change,
actual allocation test, or end-to-end stream implementation. Crucially ARM64
uncached common buffers can be Device Memory. The returned pointer is NOT wired
to ordinary memcpy/PCM ring helpers, and no memory is touched by this component.
The effective cache/coherency and naturally aligned CPU access path must be
resolved before filling a real buffer. Parent/device lifetime and remote-target
rundown remain separate unfinished work, not solved by this allocation owner.

Sources: [AllocateCommonBufferEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pallocate_common_buffer_ex)
and [FreeCommonBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pfree_common_buffer).

## Allocation-to-configure lifetime connection

`T70AfeConfigureCommonBufferMapped` now connects the existing explicit buffer and
AFE mapping owners. Before any register callback it revalidates the prepared
snapshot and requires its base/length to equal the allocation's returned logical
address/length, with no existing buffer leases. It does not derive a CPU physical
address or touch the common buffer contents. Effective cache attributes still
matter to a future fill path, but not to forwarding the already allocated
logical address into the existing address-register callback sequence.

A critical ownership distinction: once configure starts, the wrapper RETAINS
its buffer lease on both success and failure. Prior or even failing writes may
leave a hardware reference to the allocation. Release therefore remains blocked;
the caller must first independently establish quiescence and remove/replace
references before explicitly ending that lease. There is no automatic retry,
rollback, free or start. A second configure is rejected while the lease remains.
If mapped configure never starts, the buffer lease is ended; an End failure is
reported as still held. Mapping-End errors remain in the nested result for
separate recovery. Returned LeaseRetained is not a successful-configure verdict.

Host tests execute the real composition with fake allocation and register
adapters, checking all twelve callback failure positions, mapping End failure,
logical-address halves, stale/inconsistent snapshots, blocked retry/release,
no-map cleanup and untouched buffer contents. Official WDK headers compiled the
wrapper to ARM64 COFF with no mock includes. Neither check performs real mapping,
allocation, register writes or DMA and neither establishes playback readiness.
The [common-buffer logical address contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pallocate_common_buffer_ex)
and [exact-tuple free contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pfree_common_buffer)
remain unchanged. Ordinary memcpy/PCM-ring helpers are intentionally not called.

## CPU prefill with an explicit Device Memory accessor

Microsoft's [RtlCopyDeviceMemory contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlcopydevicememory)
provides CPU Device Memory alignment-safe copying, forbids overlapping ranges,
and does not guarantee device-specific access widths. It is not a DMA cache
flush or completion API. The official WDK package contains its wdm.h declaration
and ARM64 volatileaccessk.lib. [AllocateCommonBufferEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pallocate_common_buffer_ex)
returns CPU-accessible common-buffer storage and documents ARM64 Device Memory
for uncached allocations. Those contracts support copying RAM-backed storage
through this accessor; they do not support using an arbitrary MMIO register
window as a destination or assuming coherence from a pointer.

`CommonBufferFill` connects strict WAVE parsing/conversion to a caller-owned RAM
staging ring, then copies only the changed one/two spans to the common buffer
with RtlCopyDeviceMemory. All caller ranges, including the allocation and owner,
are checked for overlap and arithmetic wrap before conversion/access. No normal
memcpy reaches Device Memory. Before copy, validation/conversion failures leave
the common buffer untouched. The result separately exposes copied progress and
lease state; an End failure after copy cannot pretend rollback. The RTL routine
has no failure status and access exceptions are not caught.

This is explicitly DMA-QUIESCENT CPU PREFILL ONLY. External ownership must
exclude all device and CPU consumers throughout the call. It is not a live ring
refill protocol; AvailableBytes is a caller bound, not observed consumption.
Effective cache attributes, any required cache maintenance, DMA visibility,
release/acquire publication and permission to start remain the real platform
provider's obligations. No barrier/cache operation or device-start action is
invented here. The prior prohibition on passing the allocation directly to the
ordinary PCM ring helpers remains: those run only on staging RAM.

Host tests use a fake Device Memory accessor and compare S16 and normalized
24-in-32 mono/stereo data at every ring offset/length, including wrap, no-op,
invalid padding/format/capacity, overlap and End failure. Official-header ARM64
object compilation succeeded without mocks. No real allocation, mapping, CPU
Device Memory access, DMA, driver link or playback test occurred. A final driver
must link volatileaccessk.lib; library existence is not a successful link test.

### Current DriverEntry boundary after software-owner integration

DriverEntry now reaches the production WaveCyclic runtime/software-domain owner,
not the historical WaveRT NoStream initializer. Real public COM proxy lifetime
tracking is wired into the factory. However, current StartDevice still rejects
at the distinct physical AccessReady stage, before mapping or IRQ connection:
CPU exclusivity cannot certify clock/power or exclude firmware/DSP activity.
No subdevice is registered. Persistent published device removal, queued service
rundown and actual physical Ready/Stopped providers remain unfinished. Do not
replace the missing admission/transition callbacks with success constants.
