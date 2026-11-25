# Official-header component compile and link audit

2026-11-18, source baseline `098862d`, Linux ARM64 builder, clang-cl/lld 21.1.8,
Microsoft NuGet WDK/SDK 10.0.28000.2526, headers 10.0.28000.0, KMDF 1.35.
This is **not a completed Windows audio driver** or a successful driver link.

Reproduce against the already installed, licensed official packages and their
case-insensitive VFS overlay (no download or installation by this script):

```sh
python3 Windows/MtkAudio/check-wdk-components.py \
  --cache "$HOME/.cache/t70-official-wdk/10.0.28000.2526" \
  --output /tmp/t70-audio-wdk-components-new
```

The output directory must be new and outside the source directory. The script
compiles every top-level non-`*Test.c` separately, verifies ARM64 COFF, captures
`/showIncludes`, commands, source/header/library hashes and tool versions, and
uses llvm-nm to distinguish project definitions from external references.
Compiler include injection environment variables are removed; overlay backing
files must be within the official include trees. No `Tests/include` is supplied.
The caller must retain the official package contents and valid overlay; hashes
record inputs, not proof of package provenance. Official headers produce clang
compatibility warnings; this is not a warning-free MSVC/WDK build.

## Observed result

Run `/tmp/t70-audio-wdk-components-3`: **48/48 components compiled**, zero
unresolved T70 project references. All component objects participate directly
in the diagnostic link with `/opt:noref`; no fake entry, unresolved-symbol force,
or dead-code elimination is used to conceal missing consumers.

External component references and installed ARM64 providers:

- `ntoskrnl.lib`: MmMapIoSpaceEx/MmUnmapIoSpace imports, `_vsnwprintf`,
  `memcmp`, `memcpy`, `memset`.
- `hal.lib`: KeGetCurrentIrql import.
- `volatileaccessk.lib`: RtlCopyDeviceMemory.
- KMDF 1.35 `wdfdriverentry.lib`: WdfDriverGlobals, WdfFunctions_01035,
  WdfClientVersionHigherThanFramework, WdfStructureCount, WdfStructures.
- KMDF `wdfldr.lib`: dependencies pulled in by the framework stub.
- `bufferoverflowfastfailk.lib`: compiler/frame security support including
  security cookie and guard support needed by the framework stub. Security
  instrumentation was not disabled to make the link appear successful.

The report's `library_providers` is a symbol-name candidate inventory; the actual
link diagnostics are authoritative. With these libraries, `lld-link` reports
**only undefined `DriverEntry`**, referenced by the real KMDF framework stub.
No blank DriverEntry was added: the actual driver owner, initialization/cleanup,
endpoint registration and hardware/resource lifetime are still absent.

Exit zero from the script means the **audit completed**, not that linking passed.
Inspect `diagnostic_link.exit` in `report.json` and `link.log`; this run is exit 1.
The console explicitly prints `driver_link_succeeded: false`. Compilation errors,
mock contamination and unresolved project symbols fail the audit itself.
No `.sys` was successfully produced, loaded or signed. No official Windows build
pipeline, Driver Verifier, hardware access, DMA/cache coherency or playback was
validated. This work neither builds EDK2 nor touches USB/DSDT.

## DriverEntry is an integration boundary, not a missing stub

A follow-up comparison with Microsoft's
[PortCls startup sequence](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/startup-sequence)
and [PcAddAdapterDevice contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcaddadapterdevice)
finds that DriverEntry initializes driver-wide dispatch; it does not receive our
explicit mapping, DMA adapter or two SPB targets. PortCls creates an FDO attached
to a supplied real PDO. Its StartDevice callback receives the device's assigned
resource list. A bare physical address or a fabricated DEVICE_OBJECT cannot
replace that ownership chain. This describes the Windows API requirement, not
an assumption that T70 currently has automatic enumeration or an installed PDO.

The official [SysVAD adapter.cpp](https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/adapter.cpp)
uses WdfDriverCreate with WdfDriverInitNoDispatchOverride before PortCls driver
initialization, preserves PortCls dispatch ownership, and combines PortCls
unload with WdfDriverMiniportUnload. Its virtual-device behavior supplies no T70
hardware resources and must not be imported as evidence of T70 readiness.
Only API relationships were examined; no sample implementation was copied.

[WdfDeviceMiniportCreate](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfminiport/nf-wdfminiport-wdfdeviceminiportcreate)
can bridge an explicitly supplied real WDM FDO to the WDFDEVICE needed by
AwSpbTarget. It still requires an initialized WDFDRIVER and an existing live FDO;
it does not create the missing device stack, resource assignments or lifetime
owner. The present audio C/header tree has no DriverEntry, WdfDriverCreate,
WdfDeviceMiniportCreate, PortCls initialization or WaveRT miniport implementation.

Required actual connections, in order:

1. A real explicit device-stack/resource provider and its removal contract,
   including AFE assignment, DMA adapter and verified resource-hub connection IDs.
   An I2C address is not a connection ID; the I2C controller's own device is not
   automatically the audio function owner.
2. Driver-wide PortCls/KMDF initialization with failure unwind and dispatch/
   unload ownership, then the real FDO/WDF miniport-device association.
3. Device context admission and draining before target close, mapping unmap or
   parent deletion. Current AwSpbTarget leases alone do not exclude remote
   removal. Current common-buffer leases must survive partially applied DL1
   configuration until hardware references are independently proven gone.
4. Actual WaveRT/Topology miniports and endpoint registration, stream buffer/MDL
   ownership, position and state transitions. Current PCM copying is quiescent
   RAM staging/prefill, not a WaveRT direct-mapped render stream. No start/stop
   or DMA visibility guarantees follow from it.

Consequently this review adds no DriverEntry that merely returns success, empty
StartDevice, dummy miniport, fabricated hardware ID or virtual audio endpoint.
Such additions could silence the linker while leaving the requested integration
absent. A fail-only endpoint would also not constitute a functioning initialization
layer. There is no newly justified loadable implementation to compile or execute
in this step; the preceding 48-component result remains unchanged. No installation,
new host execution test, hardware operation or driver-load claim accompanies this
static review. The next implementation needs the concrete provider in item 1,
not guessed addresses or another wrapper around an unowned pointer.

## Implemented driver-wide initialization owner

`PortClsOwner.c/h` now implements the justified driver-wide portion rather than
an empty DriverEntry: a real integrating entrypoint supplies its DRIVER_OBJECT,
registry path, AddDevice and unload callbacks. The code creates the KMDF driver
with `WdfDriverInitNoDispatchOverride`, calls the actual
`PcInitializeAdapterDriver`, saves the PortCls unload callback and installs the
integrator's unload callback. On PortCls initialization failure it deletes the
created framework driver and restores the saved driver dispatch/AddDevice/unload
pointers. It does not claim rollback of undocumented OS internals or permit a
previous framework binding.

After the real caller has drained and destroyed all device resources,
`T70PortClsFinishUnload` invokes PortCls unload before
[WdfDriverMiniportUnload](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfminiport/nf-wdfminiport-wdfdriverminiportunload).
The owner is detached before callbacks to prevent a second release of the same
ownership record. This is a driver-wide, externally serialized lifecycle, **not**
a substitute for device removal/rundown. The actual entrypoint, AddDevice,
StartDevice and WaveRT implementation remain missing; no device is enumerated or
hardware accessed by this module. The API is ready for those real consumers,
not currently invoked by an existing driver entrypoint.

Host tests cover invalid/duplicate initialization, incorrect IRQL, WDF failure,
PortCls failure after changed dispatch pointers, restoration, exact callback
ordering, successful dispatch ownership, repeated/reentrant finish and corrupt
owner rejection. The mocked DRIVER_OBJECT/WDF types are not ABI declarations.
The updated official-header audit compiled **49/49** audio components, with no
unresolved project symbols. Adding official `portcls.lib` resolves the new
PcInitializeAdapterDriver import; WDF's real stub supplies its miniport-unload
indirection. The diagnostic link still fails **only at DriverEntry**, as intended
until the actual integration is implemented. It is not a loadable-driver claim.

## WaveRT KS format to retained PCM/DMA metadata boundary

`WaveRtFormat.c/h` connects the actual KS outer format to `WavePrepare` and the
existing DL1 arithmetic preparation. [IMiniportWaveRT::NewStream](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavert-newstream)
receives a KSDATAFORMAT rather than the standalone inner WAVEFORMATEXTENSIBLE
previously accepted. NewStream also precedes buffer allocation: this API must be
called on an immutable retained format once the real owner supplies geometry;
it is not a NewStream implementation or a substitute for its COM stream object.

The supported subset is exactly 104 bytes, audio/PCM/WAVEFORMATEX GUIDs, zero
flags/reserved, fixed SampleSize equal to block alignment, and the existing strict
40-byte extensible descriptor. Attribute lists, DSOUND, ordinary WAVEFORMATEX and
other Windows-valid formats are deliberately unsupported. Existing S16 mono/
stereo and discrete rate checks are reused, not a claim that all these rates
are routed to the T70 speakers. 24-valid-in-32 returns the explicit normalization-
required result and preserves output: it MUST NOT be treated as stream acceptance.
An eventual direct-mapped WaveRT implementation must reject it until a real safe
conversion path exists. No stream/pin support, DMA mapping, coherency, ownership,
registration or playback result is invented.

[The KS contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/ns-ks-ksdataformat)
is checked using the official header GUID macros and compile-time offsets/size.
Official compilation caught two integration mistakes that mocks did not:
PortCls's prerequisite header chain was needed, and the native extensible member
is `WaveFormatExt`, not the initially assumed name. Both were fixed before commit.
Host structure declarations remain reduced mocks, not proof of the Windows ABI.

263,545 ASan/UBSan host checks cover supported formats, rejected S32, explicit
24/32 non-acceptance, every outer-header bit, flags/SampleSize exhaustive 16-bit
values, truncation, geometry failure, unaligned input, output overlap/alignment
and output preservation. Independent review reran the host checks successfully.
The final official-header audit compiled 50/50 components; internal project
references resolve and the diagnostic driver link still lacks DriverEntry.
The full host suite also passed. No successful driver link or live execution.

## Actual WaveRT stream interface, intentionally abstract

`WaveRtStreamBase.cpp/h` now derives from the real WDK
[IMiniportWaveRTStream](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iminiportwavertstream)
and implements actual C++ overrides. `SetFormat` calls the KS-to-DL1 validation
code through its C ABI and replaces only validated metadata. This class requires
an explicit valid S16 geometry plan; that plan is not evidence of an allocated
or configured buffer. The format object must be a genuine readable PortCls
object; its size field cannot make arbitrary memory safe to read.

[SetState](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-setstate)
acknowledges only already-STOP software state. ACQUIRE/PAUSE/RUN fail with
STATUS_NOT_SUPPORTED, without changing metadata or touching hardware. The
method is final so a subclass cannot start DMA while accidentally inheriting
this class's stopped-only format/lifetime assumptions. A future working stream
must revise these together. GetPosition returns STATUS_DEVICE_NOT_READY; clock
and position register access return STATUS_NOT_SUPPORTED and preserve outputs.
Invalid pointers/IRQL are rejected within the documented valid-memory contract.

COM reference lifetime, AllocateAudioBuffer, FreeAudioBuffer and GetHWLatency
remain pure virtual. In particular a fabricated zero hardware latency or an
unowned MDL cannot be supplied just to make a concrete object. The class is
statically verified abstract in the host test and has no production factory or
NewStream registration. A concrete miniport/stream provider and the existing
DriverEntry integration gap remain. The test subclass is solely an in-process
mock and never supplies a real buffer or hardware latency.

The host test invokes methods through IMiniportWaveRTStream pointers, exercises
format replacement/rejection, 65,536 cbSize values, STOP and denied transitions,
output-preserving position/register errors, invalid geometry and IRQL. It checks
196,637 assertions with ASan/UBSan. The full audio runner now compiles C dependencies
as C and the interface as C++, so cross-language symbol linkage is exercised.
The official audit likewise includes production .cpp files with exceptions/RTTI
disabled. All 51 components compile with actual WDK headers; the abstract vtable's
_purecall resolves using official libraries. Diagnostic linking still reports
only the absent DriverEntry, not a usable driver. No hardware or OS stream runs.

## WaveRT MDL ownership is separate from DMA common-buffer ownership

The actual [IPortWaveRTStream allocation contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iportwavertstream)
provides MDLs through its page-allocation methods. The previous AfeCommonBuffer
owner instead holds an adapter-produced logical address and HAL allocation.
Neither its logical address nor a WaveRT MDL's CPU physical address may be
substituted for the other. The current ARM64 Device Memory common-buffer prefill
path is not automatically a directly user-mappable WaveRT cyclic buffer.

`WaveRtMdlOwner.cpp/h` now implements the actual PortStream allocation lifetime:
retain the explicit interface, allocate dedicated contiguous pages, validate the
whole-page MDL response, map for cached CPU access, zero every allocated byte,
and only then publish ownership. Allocation/map/validation failure unwinds the
exact resources acquired. Final release unmaps, calls
[FreePagesFromMdl](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-freepagesfrommdl)
(which frees both pages and MDL), then releases the interface. It never calls
IoFreeMdl or FreeCommonBuffer on this different allocation. The caller supplies
an already live interface; no device or stream is synthesized.

Requests are a strict subset: nonzero whole pages up to the existing 196608-byte
buffer ceiling. Physical allocation bounds are 0..MAXLONGLONG, **not** the AFE's
34-bit device address bound. No physical-to-logical identity is asserted and no
logical address is returned. MmCached selects the ordinary CPU mapping; it is
not evidence of device coherency or permission to program a DMA engine. No client
mapping is created. Existing common-buffer storage is never cast into an MDL.

A returned borrowed view requires a lease. Release fails while leases remain;
callers must keep a lease through every independently created user mapping,
DMA mapping or hardware reference and prove their removal before ending it.
External serialization and non-reentrant provider calls are mandatory. The
counter cannot itself prove PortCls/user/DMA rundown, and there is deliberately
no destructor that silently frees a buffer while such references might remain.
This is an allocation owner, not an implementation that returns success from
IMiniportWaveRTStream::AllocateAudioBuffer or starts playback.

A possible future [CreateCommonBufferFromMdl](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pcreate-common-buffer-from-mdl)
bridge has separate resident/mapped/page-aligned MDL, platform/adapter support,
logical-address, cache and teardown requirements. Its FreeCommonBuffer removes
DMA bookkeeping but does not free the caller's MDL/pages. No such bridge is
assumed or executed here; the final device-visible range and lifetime must be
validated before the existing DL1 configuration can consume it.

Host fake-PortStream tests cover every request length through 196609, all 48
accepted page counts, allocation/map/malformed-MDL failures, full zeroing,
reference ownership, invalid/double release, nested leases/overflow, wrong IRQL
and output aliases. 5,014,909 ASan/UBSan assertions pass. Official WDK compilation
covers the real IPortWaveRTStream methods and MDL macros, not the mock layout.
Independent review reran the same host checks; output-storage and non-reentrancy
preconditions were clarified. Full host regression passed, and all 52 production
components compiled with official headers. The diagnostic link still lacks the
real DriverEntry; there was no OS allocation, user mapping or device execution.

## MDL-backed device-logical mapping owner

`WaveRtDmaOwner.cpp/h` now uses the official
[CreateCommonBufferFromMdl](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pcreate-common-buffer-from-mdl)
operation from an explicitly supplied, retained real device DMA adapter. The
runtime operations size and function pointer are checked; installed headers or
DMA_ADAPTER.Version alone do not prove support. Windows/ARM64 availability and
actual device association remain the resource supplier's responsibility.

The owner retains a WaveRT MDL lease, validates whole-page resident mapped backing,
and requests [logical-address limits](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-dma_common_buffer_extended_configuration)
from zero through 0x3ffffffff inclusive. A successful provider result is checked
again for range/end overflow and 16-byte alignment. Only that returned logical
address is stored; no CPU physical address is used as a substitute. Existing
backing leases prevent acquiring another mapping through this interface. This
allocation operation is not a DL1 register write or DMA start.

The backing remains cached ordinary page memory. Mapping success does not prove
that ARM64 CPU/device visibility, cache maintenance, user mapping or hardware
ownership conditions needed for playback are fulfilled. Unsupported operations
or configuration return failure, never an identity-address fallback.

Cleanup removes only DMA bookkeeping with the adapter's FreeCommonBuffer, using
the complete length, returned logical address, existing system virtual address
and TRUE for the cached backing. It then ends the MDL lease. The actual MDL owner
later unmaps/frees the pages using PortStream, not HAL allocation APIs. Caller
must establish device quiescence and remove every hardware reference BEFORE
release; this code cannot stop hardware. If a cleanup step cannot end the lease,
the owner retains cleanup state even when Acquire returns an error. Retrying
Release after the precondition is restored cannot repeat an already completed
FreeCommonBuffer. Never erase or reuse that retained ownership record.

3,888 final ASan/UBSan host checks exercise logical range boundaries for all 48
page counts, every injected create error, malformed backing, unavailable adapter
operations, no-create-on-existing-lease, normal release, and cleanup retry
without double-free. Independent review ran the initial 3,880 checks and found
no defect; its suggested malformed/lease cases were added. Full host regression
passed before these final narrow additions. Official WDK compilation of all 53
production components passed, with only DriverEntry still missing at diagnostic
link. No actual mapping, device transfer, cache flush or hardware I/O occurred.

## WaveRT stream preparation consumes the actual DMA logical mapping

`T70WaveRtStreamBase::ConfigureMapped` now connects the stream's validated
rate/format/channel/period metadata to the DMA owner's actual returned logical
address and buffer length, rebuilds a checked DL1 plan, and invokes the existing
AFE mapped configure path. The real resource owner calls this explicit method;
it is not an automatic NewStream/AllocateAudioBuffer/SetState side effect.
Quiescence, exclusive AFE access, power/access readiness and stable retained
owners are mandatory external preconditions. Nothing here proves them.

A separate DMA-use lease is taken before AFE configuration. Once any configure
callback begins, the lease is retained on success AND every failure. This makes
WaveRtDmaRelease refuse to remove the logical mapping; its underlying MDL lease
continues to protect the pages. Address/format writes may have partially applied,
even when a callback failed. No rollback, retry or device-ready status is claimed.
The stream latches the attempt, forbids further SetFormat/configure calls, and
continues to refuse RUN. Its copied plan describes the attempted metadata, not
observed hardware state. A fresh stream cannot reuse a DMA owner with active use.

Only a failure before configure started attempts to end the DMA-use lease.
Unresolved DMA End and AFE End outcomes are retained in the returned progress.
Caller must separately resolve any retained AFE lease and prove the device no
longer references the buffer before calling DmaEndUse. The stream destructor does
not release this ownership automatically; the caller must retain the owner even
if it destroys the stream. The counter is not a hardware-stop or drain primitive.

643 host assertions cover every one of the 12 register callback failure points,
AFE lease-end failure, early mapping failure, rejected logical geometry, wrong
IRQL, release blocking, and no retry/format change after partial configuration.
Mocks replace register callbacks; no MMIO occurred. Official compilation includes
all 54 production C/C++ components and real interface overrides. Diagnostic link
still requires an actual DriverEntry and complete miniport provider.
Independent review reran the 643 checks and found no defect. Its suggested full
fake-provider MDL Acquire -> DMA Acquire -> Configure -> deferred release chain
and retry after pre-callback failure were added; final focused count is 711.
The full existing host suite passed before these test-only additions. These are
still mocked providers and do not establish real Windows/hardware behavior.

## Actual AllocateAudioBuffer / FreeAudioBuffer transaction boundary

`WaveRtBufferedStreamBase` implements the real PortCls virtual methods over the
MDL/DMA/configuration owners. It remains ABSTRACT and unregistered: no concrete
provider for recovery lifetime, client-cache suitability, hardware-reference
removal, COM ownership or latency is invented. In particular no default-true
client mapping/coherency check exists.

The real integrator must first pin the stream, transaction, adapter, PortStream,
AFE mapping and recovery code against destruction/unload. Allocate then acquires
and zeroes dedicated pages, creates the logical DMA mapping, requires a provider
to verify THIS allocation's actual CPU/client/cache/coherency suitability, and
only then configures DL1. Outputs are published together only after the configure
callbacks and AFE lease ending complete. ActualSize equals the accepted whole-page
request; offset is zero and the CPU cache type is MmCached. Unsupported partial
page requests fail rather than silently returning a smaller buffer.

The [AllocateAudioBuffer contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-allocateaudiobuffer)
allows buffer configuration before RUN. This code never permits RUN. Failures
before configuration unwind acquired resources, unless cleanup itself cannot
finish. Partial configuration failures quarantine the transaction with all
necessary leases and lifetime pin. Both reallocation and SetFormat are blocked;
no partially programmed buffer is presented as successfully allocated.

[FreeAudioBuffer returns void](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-freeaudiobuffer).
The implementation verifies the exact published MDL/length. It requires real
hardware-reference removal before ending DMA use, removing the DMA mapping,
unmapping/freeing pages and unpinning lifetime. A failed removal is NOT successful
free: resources and LastStatus remain recorded. PortCls is not assumed to call
Free again. The provider must arrange explicit Recover and retain everything,
including the code, until it finishes. Published buffers cannot be recovered
before the matching PortCls Free request. NewStream, a concrete provider,
registration and device teardown integration remain missing, so this is not yet
a usable OS audio stream.

The review also distinguishes the hardware/page lifetimes emphasized in
[Microsoft's buffer-management discussion](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/managing-memory-buffers-rebalance).
That example concerns HD Audio and is not evidence of T70 power or enumeration
behavior. No STOP software state is treated as proof that T70 DMA references
are gone.
Final focused tests pass 556 ASan/UBSan assertions across allocation/map/DMA/
policy failures, all 12 configure failure positions, AFE End failure, output
preservation, exact Free matching, deferred cleanup, pin retention and terminal
state. Independent review reran 551 checks; its LastStatus consistency finding
was fixed and null-output tests added. The earlier full host suite passed.
Official WDK compilation of all 55 production components passed, with the
expected actual DriverEntry gap still present at diagnostic link. No live
allocation, mapping, PortCls client access, DMA operation or device run occurred.

## WaveRT cache-provider boundary and requested-size rounding

The concrete cache-suitability provider remains unimplemented. Microsoft's
[CreateCommonBufferFromMdl contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pcreate-common-buffer-from-mdl)
provides device-access testing and a logical address, but does not explicitly
certify the cache/coherency properties of these already-cached WaveRT pages.
[AllocateCommonBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pallocate_common_buffer)
separately documents device-specific ARM64 coherence selection using ACPI _CCA.
Neither a mock success nor an MmCached enum establishes that T70 device contract.
[MapAllocatedPages](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-mapallocatedpages)
requires client-compatible access alignment. No default-success cache provider,
new ACPI declaration, noncached alias, or hardware experiment was added.

An independent actual-interface gap was corrected instead:
[AllocateAudioBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-allocateaudiobuffer)
accepts a byte request and permits hardware-constrained size rounding, while
requiring the actual size not be smaller than the request. Previously this
implementation rejected every non-page-multiple request even when a suitable
allocation existed. It now selects the smallest page-backed size at or above
the request that also satisfies the retained DL1 period and geometry, up to
196608 bytes. The bounded page search performs no allocation or I/O. No valid
size, zero, or oversized requests fail before pinning. The selected size flows
through the MDL, logical DMA mapping, DL1 configuration and published output.

Host ASan/UBSan/-Werror full regression passed, including 148095 Allocate/Free
checks: every request 1..4096, a 1536-byte period requiring 12288 bytes for a
4097-byte request, the maximum size, oversized/UINT32_MAX, and a valid period
with no page-backed solution. Existing partial-config/recovery tests remain.
Official ARM64 WDK audit `/tmp/t70-audio-wdk-rounding-1` compiled 55/55 production
units with no unresolved project references. Diagnostic linking still fails
on missing real DriverEntry. This does not establish loadability, a concrete
cache/recovery provider, DMA stopping, or playback.

Independent read-only review found no defects and separately reproduced all
148095 focused checks with sanitizers and -Werror.

## WaveRT position-query execution level; no fabricated progress

Rechecked the retained stock pointer/IRQ evidence against the actual stream
interface. `AFE-POSITION-EVIDENCE.md` records stock low-before-high CUR reads
and no demonstrated latch/retry contract. The arithmetic helper accepts an
already coherent address; it does not establish hardware consumption or a
client-safe write cursor. `STREAM-INTEGRATION-GAPS.md` identifies dynamically
assigned IRQ ownership; the earlier status/clear review still lacks proof of
shared-pending preservation for an ACK. No IRQ registration, ACK, period
notification, live CUR reader, or DMA execution was added.

The official [GetPosition contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-getposition)
permits PASSIVE_LEVEL or DISPATCH_LEVEL. The real interface implementation
incorrectly classified a dispatch-level query as invalid input. It now returns
DEVICE_NOT_READY at either documented level, preserving both output fields;
NULL or other execution levels remain invalid. No passive-only mapping/lease
helper is called in this dispatch-capable path.

[KSAUDIO_POSITION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-ksaudio_position)
uses distinct play and write positions and identifies a client-inaccessible
region between them. Consequently substituting one normalized CUR value for
both fields would assert an unproved ownership boundary. A future live provider
must establish coherent sampling, allocation/lifetime association, both cursor
semantics, and dispatch-safe synchronization. RUN remains unsupported.

Full host regression passed; the actual-interface mock now passes 197405
ASan/UBSan/-Werror checks, including every KIRQL byte and NULL/output preservation.
Independent review reproduced that count and identified a stale all-PASSIVE
header comment, corrected to document the GetPosition exception. Official WDK
55/55 component compilation passed (`/tmp/t70-audio-wdk-position-irql-1`), with
no unresolved project symbols. Diagnostic driver link remains blocked only by
the missing real DriverEntry, not treated as a loadable driver result.

## Explicit WaveRT port-to-miniport binding

`WaveRtPortBind.cpp` now executes the real PortCls connection calls rather than
adding another abstract hardware callback: `PcNewPort` for WaveRT, then
`IPort::Init` with the caller's actual device, startup IRP, uninitialized
miniport, optional adapter and resource list. It transfers the created port
reference only after successful initialization; Init failure releases that
reference and preserves the caller's output. The miniport must implement its
own initialization-failure cleanup; releasing the port is not a claimed rollback
of hardware effects. Retry is not promised.

Sources: [PcNewPort](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcnewport),
[IPort::Init](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iport-init),
and [PcRegisterSubdevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcregistersubdevice).
The WaveRT class identifier matches installed official portcls.h. Explicit
startup objects and their COM references must already be valid and retained by
the caller; this function does not discover or manufacture a device stack.
PASSIVE_LEVEL, disjoint output storage and external serialization are required.

No subdevice is registered or exposed to clients. In particular, the repository
still lacks a concrete miniport/NewStream factory, device/resource owner,
cache/recovery providers and working RUN backend. This real binding function
requires such a caller-supplied miniport; it does not make the current abstract
stream usable. PortCls initialization may invoke miniport code and must never be
called on fabricated startup arguments in a real kernel. Host tests use fakes
only and verify call order, unchanged explicit arguments, optional adapter,
create/Init failure, malformed success-with-null, successful reference transfer,
invalid arguments/IRQL, and no caller-reference consumption.

Full host regression and independent read-only review passed; the focused fake
port test has 71 sanitizer/-Werror checks. Its Init mock does not emulate
PortCls-internal references or a real miniport initializer. Official WDK audit
`/tmp/t70-audio-wdk-portbind-1` compiled 56/56 production units, resolving the new
PcNewPort import with official portcls.lib. There are no unresolved project
symbols; diagnostic linking still fails for the absent real DriverEntry.
No loaded endpoint, playback, or actual device access was tested.

## Actual IMiniportWaveRT Init/NewStream admission boundary

`T70WaveRtMiniportBoundary` implements the real WDK Init and NewStream signatures
as final methods. It remains abstract for COM ownership, filter/range metadata
and the actual device description. No endpoint or creatable miniport factory is
invented. A retained, non-reentrant, externally serialized provider supplies
resource initialization and construction of an owned concrete stream. Absent
either callback, initialization fails. A real initialization attempt is terminal
for this object, including failure; the provider must unwind its own failure.
Copy construction/assignment are deleted to prevent cloning initialized state.

The explicit render pin must match the integrating filter descriptor. Capture
and other pins fail. The factory receives the actual PortStream and format and
must validate/retain what it uses, provide resource/cache/recovery lifetime,
and return an unstarted, safely releasable owned stream. Failed owned results
are released. Success-with-NULL is rejected. Before output publication the
boundary checks STOP and both initial position fields, using sentinel values
to detect a provider that reports success without writing its result. Any error
releases the unpublished stream. These checks do not certify hardware readiness.
The existing T70 abstract stream's DEVICE_NOT_READY position response prevents
it from being passed off as a completed NewStream result.

Official contracts: [IMiniportWaveRT::Init](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavert-init)
and [NewStream](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavert-newstream).
NewStream requires a stopped, initially zero-position stream and uses COM
references. The implementation is a callable admission boundary, not a supplied
hardware provider or a loaded miniport. Its real subclass/caller and resource
provider remain missing. No actual resource initialization or streaming ran.

Full host regression and independent final 196-check ASan/UBSan/-Werror test
passed, including actual PortBind -> fake system port -> actual miniport Init
and NewStream. The system port and stream factory are fakes, not a real COM
QueryInterface/resource implementation. Official headers caught a missing
stdint include (masked by the test header); this was corrected. Final official
WDK audit `/tmp/t70-audio-wdk-miniport-2` compiled 57/57 production units, no
unresolved project symbols. The diagnostic link still lacks real DriverEntry;
there is no loadable-driver or playback claim.

## Concrete miniport COM object and bounded filter metadata

`T70WaveRtMiniport` is now a concrete nonaggregated IMiniportWaveRT object, created
from nonpaged pool with one caller reference. It retains an explicitly supplied
IUnknown lifetime owner for its provider context/code/resources. Atomic reference
counts and QueryInterface for IUnknown/IMiniport/IMiniportWaveRT use one identity;
unsupported queries clear the output and fail. Final Release destroys/frees the
miniport and releases the owner. The owner's rundown must already be real,
DISPATCH-safe when required, and cycle-free; streams retain their own owner.
No concrete T70 resource owner is invented by this factory.

GetDescription supplies object-lifetime filter metadata only after the explicit
initialization callback succeeds. Pin 0 is a single software render admission
pin, with eleven discrete rates already accepted by AfeFormat, one/two-channel
S16 only, and no physical pins, codec nodes or connection claims. These ranges
are arithmetic/format metadata, not evidence that a T70 playback route works.
The provider must also enforce single-stream admission. DataRangeIntersection
remains unsupported and GetDeviceDescription fails without a proven device DMA
contract. No PcRegisterSubdevice call or working RUN path was added. Actual
PortCls initialization ordering is not established by host mocks.

Sources: Microsoft's [audio COM reference conventions](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/reference-counting-conventions-for-com-objects),
[reference-count implementation](https://learn.microsoft.com/en-us/windows/win32/com/implementing-reference-counting),
[PCFILTER_DESCRIPTOR](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/ns-portcls-pcfilter_descriptor),
[GetDescription](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiport-getdescription),
and [ExAllocatePool2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exallocatepool2).
[SysVAD minwavert.cpp](https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/EndpointsCommon/minwavert.cpp)
was checked for interface identity/reference and descriptor-return patterns.
No sample implementation or its virtual PCI/DMA device description was copied;
that description would not establish a T70 device contract.

Independent ASan/UBSan/-Werror host checks passed: 39 checks plus 80000 concurrent
AddRef/Release pairs while retaining the creator reference. Allocation failure,
missing provider, interface identity/rejection, descriptor rate bounds, rejected
stream publication and final owner/free balance are covered. Final full host
regression passed. WDK initially rejected the CRT new header's throwing paths;
class-local nonallocating placement construction avoids those paths without
enabling kernel exceptions. Final official audit
`/tmp/t70-audio-wdk-miniport-com-2` compiled 58/58 production units, resolving
pool imports in official kernel libraries. No internal unresolved symbols;
DriverEntry still absent at diagnostic link, no loadable or playable endpoint.

Independent final review also accepted the class-local placement construction
and clarified provider admission/output-storage preconditions.

## S16 DataRangeIntersection connected to the published metadata

`WaveRtIntersection.cpp` replaces the unsupported method with explicit PortCls
negotiation. MatchingDataRange must equal one of the object's eleven discrete
S16 ranges by defined fields. Client ranges must explicitly specify audio/PCM/
WAVEFORMATEX, contain 16-bit samples and the selected rate, and permit at least
one channel. Stereo is selected when allowed, otherwise mono. Wildcards,
attributes, alternate encodings, unsupported matching ranges and malformed
bounds are rejected rather than delegated to a more permissive default handler.

The result is the same 104-byte KS/WAVEFORMATEXTENSIBLE representation accepted
by WaveRtFormat: S16, matching frame/sample size, exact byte rate, valid-bits16,
PCM GUID and mono-center or stereo-left/right channel mask. Size queries return
BUFFER_OVERFLOW with length104; nonzero short buffers return BUFFER_TOO_SMALL
without modifying either output. Other failures preserve outputs; success writes
only104 bytes. Null, wrong pin/IRQL, overlapping outputs/inputs and address-range
arithmetic are checked within the caller's valid-readable-memory contract.
GetDeviceDescription and actual hardware/readiness remain unresolved; format
negotiation does not make playback possible.

Reference: [IMiniport::DataRangeIntersection](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiport-datarangeintersection).
Each successful output in the host matrix is fed into the existing production
WaveRtFormat/DL1 preparation parser. The matrix covers all eleven rates, mono/
stereo/higher channel ceilings, every short capacity1..103, invalid encodings
and rates, size query, overlap, output sentinels and unchanged buffer tails.

Independent review found and corrected one intersection-selection case: fixed
SampleSize2 constrains S16 to mono even when MaximumChannels permits stereo.
Nonzero SampleSize2/4 now participates in channel selection before validation;
other sizes or insufficient channel ceilings fail. The expanded channel/sample
matrix and all returned formats pass the production parser. Final full host
regression and independent 374098 checks plus 80000 reference pairs passed with
ASan/UBSan/-Werror. Official WDK audit `/tmp/t70-audio-wdk-intersection-3` compiled
59/59 units, including native header layout assertions for the serialized format.
No internal unresolved symbols; diagnostic link still lacks real DriverEntry.
No hardware or endpoint execution was performed.

## Startup IResourceList -> owned AFE mapping initialization

`AfeResourceMapping` connects the actual miniport Initialize callback signature
to the existing AFE mapping owner. The caller supplies an already-assigned
IResourceList and explicit memory-resource index. It retains the list before
looking up entries, independently checks raw and translated memory descriptors
against the previously established AFE tuple 0x11210000/0x2000, and accepts only
normal read/write memory flags. It does not infer translated addresses from raw
ones, discover devices, or manufacture resource entries. Other tuples/flags fail
closed; this is deliberately not a general bus-translation implementation.
The translated descriptor feeds the existing MmMapIoSpaceEx READWRITE|NOCACHE
path. Failure releases the acquired list reference; successful mapping retains
it until the mapping can be released. Active mapping leases block release.
Unmap completes before the list reference is dropped, and repeated release is
idempotent. Dirty partial owner fields are rejected rather than erased.

Sources: [IResourceList resource objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/resource-list-objects)
and [FindUntranslatedEntry](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iresourcelist-finduntranslatedentry).
The board tuple is the same established contract used in AfeMapping/README;
no new board address or interrupt value was inferred. This is only the memory
resource layer, not the entire device owner. The external lifetime owner must
pin context/code, serialize use and exclude removal/resource reassignment and
incompatible cache aliases. Holding IResourceList alone proves none of these.
IRQ assignment/ACK, DMA adapter identity, DMA logical/physical correspondence,
cache coherency, clock/power and stream readiness remain separate unresolved
contracts. No actual map/MMIO/device operation was executed.

Host tests route actual MiniportBoundary.Init into this callback with fake
resources and fake map/unmap. They cover raw/translated address, length, type
and flags mismatches, missing/out-of-range entries, map failure, retry on the
same clean owner, dirty owner preservation, duplicate Init, high-IRQL release,
active lease refusal and unmap-before-list-release. NoStream still fails and
preserves its output, so successful resource mapping does not expose playback.

Final full host regression and independent 92-check ASan/UBSan/-Werror run
passed. Official WDK audit `/tmp/t70-audio-wdk-resource-1` compiled 60/60
production units with no unresolved project symbols. Diagnostic link still
fails only for missing real DriverEntry; resource ownership tests do not
establish an actual device stack or loaded driver.

## COM resource owner, client leases and fail-closed teardown admission

`T70AudioResourceOwner` is an actual IUnknown owner usable as the concrete
miniport's lifetime owner and provider context. Its Initialize callback connects
the existing resource-list/AFE mapping acquisition. Successful mapping holds a
self-reference independently of the miniport's reference. BeginUse pins an
explicit client lease and supplies the owned AFE mapping; a stream/DMA consumer
must bracket its complete use and independently retain its DMA/MDL resources.
The miniport can release its last reference without freeing the mapped resource
while clients or the open self-reference remain.

Close permanently rejects new clients, waits by returning failure while client
or raw mapping leases remain, then requires an explicit quiescence proof
callback. Missing, failed or pending/informational proof retains the mapping
and self-reference. Only exact STATUS_SUCCESS means completed proof. Successful
unmap/list release precedes dropping the self-reference. No destructor silently
unmaps live resources. A failed close therefore requires explicit integrator
recovery, and can intentionally retain memory indefinitely instead of unsafe
free. Close-before-initialize is terminal; no resource reopening occurs.

Microsoft's [Using Remove Locks](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-remove-locks)
separates admission closure and outstanding-I/O rundown before device deletion.
This code does NOT implement OS removal dispatch, an IO_REMOVE_LOCK, interrupt
cancellation or kernel-image unload exclusion. Those still require the actual
adapter/device owner. All non-reference operations here are externally
serialized PASSIVE_LEVEL and non-reentrant; atomic COM counts do not make the
other methods concurrent-safe. A self-reference pins object memory, not the
kernel image. The integrating driver must keep code loaded and a recovery
reference live. No automatic discovery or removal handler is assumed.

ConfirmQuiesced must establish that DMA/IRQ/MMIO clients, including separate
common-buffer/MDL owners, cannot access resources again. No such hardware proof
provider is supplied; a stop request or zero software count is not substituted.
The code does not free DMA buffers or claim that the existing stream's DMA
release contract is solved. Tests use an explicit fake proof only.

Final full host regression and independent 63-check sanitizer/-Werror test
passed. Tests cover actual miniport-owner retention, lease admission closure,
failed/pending proof, close retry, map failure self-pin unwind and final balance.
A deliberately outstanding raw mapping lease also prevents close even after a
client lease is dropped. Independent review's object-vs-code pin distinction
is stated explicitly above. Official WDK audit `/tmp/t70-audio-wdk-owner-1`
compiled 61/61 production units with no unresolved project symbols. Diagnostic
link still lacks real DriverEntry; neither actual removal nor DMA stop ran.

## Stream factory feasibility and failure-only ownership integration

No production success-returning stream factory was added in this step. Source
tracing found concrete blockers: WaveRtStreamBase::SetState is final and rejects
RUN; its GetPosition reports DEVICE_NOT_READY; WaveRtBufferedStreamBase still
requires real latency/COM ownership, cache suitability and hardware-reference
recovery implementations. The miniport's actual DMA device description remains
unavailable. Wrapping these missing providers in a new factory would not make a
usable stream and would violate the existing admission contract.

Instead the executable integration regression now invokes the actual concrete
Miniport.NewStream boundary with a deliberately failure-only test provider. It
borrows the real AudioResourceOwner lease, calls real WaveRtMdlAcquire and
WaveRtDmaAcquire with fake PortStream/DMA operations, then rejects publication
because cache proof is unavailable. The test separately injects MDL allocation,
CPU mapping and DMA-logical mapping failures. Every case preserves the output
stream pointer, unwinds DMA bookkeeping before the MDL mapping/pages, releases
the resource client lease and balances PortStream/resource/COM references.
The retained AFE owner still refuses Close without the global proof callback.

This fixture is NOT a production NewStream implementation or a recommendation
to preallocate a client audio buffer in NewStream. No DL1 configuration, device
buffer reference or DMA start occurs, making immediate temporary-buffer cleanup
valid only in this pre-configuration fixture. Partial hardware-configuration
cleanup remains subject to the earlier retained-lease/quiescence requirements.
No new abstract wrapper or fabricated resource/capability was introduced.

Full host regression and independent 172-check ASan/UBSan/-Werror integration
run passed. No production ownership defect was reproduced in these cases.
Official WDK audit `/tmp/t70-audio-wdk-admission-integration-1` reconfirmed 61/61
production units and no unresolved project symbols; diagnostic linking still
fails for the absent real DriverEntry. These results do not unblock RUN or
establish a real factory/cache/latency/teardown provider.


## DL1 DMA description; hardware latency remains unknown

The concrete miniport now fills its actual `GetDeviceDescription` output after
successful resource initialization. It advertises VERSION3, bus mastering,
no scatter/gather list, a 196608-byte implementation maximum and 34 address bits.
The existing stock geometry evidence establishes the mask and size bound;
`mtk_memif_set_addr` uses one BASE/inclusive END range, rather than a scatter list.
`InterfaceTypeUndefined` delegates bus identity to the actual PDO. It does not
invent a PCI bus, create a PDO/adapter, or establish a translation/cache contract.
The remaining fields are zero. The 34-bit mask must not become a 64-bit claim.

Official contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_description
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavert-getdevicedescription

This removes one metadata blocker, not the actual adapter/resource provider or
production stream admission blockers. VERSION3 support must be exercised on the
actual Windows platform; compilation alone does not prove PortCls/HAL behavior.

`GetHWLatency` cannot honestly be implemented with available numeric evidence.
Its return type is VOID, not an NTSTATUS that can express unknown. The output
requires FIFO bytes and chipset/codec delay in 100-nanosecond units. The pinned
public MT6789 `mt6789-afe-pcm.c` has `fifo_size=0`, but that ALSA declaration is
not a measurement of the complete T70 path or proof of zero codec delay.
Its `mt6789_get_memif_pbuf_size` selects 32/256-byte prefetch configurations.
Stock AFE module SHA256 67f1ec09fc9d883d2d4933b8f4b062bf8c473f5bd4968761054914f703706ba8,
.text 0x41c..0x440, independently computes period_frames*1000/rate and returns
code 3 when the integer result exceeds 10, else code 0. A selected prefetch size
is neither evidence of its actual programming nor total DAC/amp latency.
No timing values, zero-latency stub, or latency conversion helper were invented.
A real stream must obtain validated path-specific latency before publication.

Sources (facts only, no GPL code copied):
- https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/d26d588883468251678619757cc9f0f6a5f37cb8/sound/soc/mediatek/mt6789/mt6789-afe-pcm.c
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-gethwlatency
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-ksrtaudio_hwlatency

Validation: full audio host regression passed with ASan/UBSan and warnings as
errors, including pre-Init output preservation, invalid IRQL/null, and exact
post-Init DMA description fields. Independent read review found no blocking
issue; its pre-Init test recommendation was applied. Official WDK audit
`/tmp/t70-audio-wdk-description-1` compiled all 61 production translation units
with zero unresolved project symbols. Diagnostic driver link still fails on
missing real DriverEntry; no driver load or hardware execution was performed.

## Explicit OS DMA adapter owner

`DmaAdapterOwner` now consumes the concrete miniport's initialized DMA description
and a caller-supplied live PDO, calls `IoGetDmaAdapter`, and records the adapter
and returned map-register count. It retains a miniport COM reference and PDO
object reference until `PutDmaAdapter` has completed. No automatic discovery or
PDO fabrication is present. A platform must establish that this PDO and the
miniport describe the same device and provide removal/unload exclusion.

Begin/End leases bracket the entire downstream DMA buffer/mapping/transfer
lifetime, not merely the allocation call. A closing owner refuses new leases.
Close needs zero leases and an exact-success external quiescence proof before
PutDmaAdapter, PDO dereference, then miniport Release. Buffers/map registers and
hardware references must already be gone. This code does not supply that proof,
cache coherence, a stream factory, a DMA mapping, or a running device. Owner
storage is externally retained nonpaged storage, not a self-freeing COM object.

NULL acquisition unwinds both references. A malformed returned operations table
without a callable PutDmaAdapter is quarantined with its ownership still visible,
not silently discarded. Such an OS/provider-contract violation may be unrecoverable;
do not free the owner or unload its code while quarantined. Operation-table
extent is checked, not DMA_ADAPTER.Version (IoGetDmaAdapter reports Version 1
regardless of the requested interface generation). The recorded map-register
count is not treated as transfer capacity or proof common-buffer allocation will
succeed. Existing consumers must retain their per-operation checks.

Sources:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iogetdmaadapter
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pput_dma_adapter
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-obreferenceobject
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-obdereferenceobject

Validation: the full host regression passed with ASan/UBSan and warnings as
errors; focused adapter checks cover real-miniport description use, acquisition
failure, reference retention past caller miniport Release, nested leases,
overflow, IRQL/duplicate operations, failed/pending quiescence, and malformed
adapter/table sizes. Independent review passed the initial 41 checks and its
boundary recommendations were added (final 79 checks). Mock table repair tests
are cleanup fixtures, not an operating-system recovery mechanism.
Official WDK audit `/tmp/t70-audio-wdk-dma-owner-2` compiled all 62 production
units with no unresolved project symbols. Diagnostic linking still fails due
to the absent real DriverEntry. No OS adapter acquisition ran on real hardware.

## MDL mapping retains the OS adapter owner

`T70WaveRtOwnedDmaAcquire` now connects the actual adapter-owner lease to the
existing `T70WaveRtDmaAcquire` MDL mapping. The composite retains that adapter
lease until the logical mapping and backing MDL lease have both been removed.
Its Mapping member can be supplied to the existing DL1 configure consumer;
that consumer's ActiveUses pin still blocks release after hardware configuration
or partial application. This is not a new stream factory or a second mapping
implementation. It preserves the existing lower-level mapping checks.

Failed acquisition unwinds in reverse order when possible. If mapping cleanup
is unavailable (including injected IRQL changes), both the mapping cleanup
ownership and adapter lease remain visible for retry. Release never ends the
adapter lease before the lower-level release succeeds. Adapter Close therefore
cannot call PutDmaAdapter while a mapping or its cleanup is outstanding. A
FreeCommonBuffer that completed before MDL-end failure is not repeated on retry.
The explicit miniport reference remains held by the adapter owner throughout.

Tests exercise the actual miniport and owner functions with fake OS services:
map failure, partial cleanup failure, closed admission, double acquisition,
backing page release denial, active DL1-use denial, and retry without duplicate
unmap. The test's EndUse is host-only recovery with no configured hardware;
production callers still owe real hardware-reference removal, cache proof,
external serialization, stable owner storage and OS removal/unload exclusion.
No actual mapping, cache validation, DMA start or quiescence was performed.

Validation: focused 119 checks passed independently under ASan/UBSan and
warnings-as-errors; full host regression passed. The backing MDL owner is a
synthetic valid fixture, not an end-to-end real MDL allocation/configure test.
Official WDK `/tmp/t70-audio-wdk-dma-bound-1` compiled 62 production units with
zero unresolved project symbols. Diagnostic link remains incomplete without
real DriverEntry. Independent review found no defect in the lease/retry path.

## Reject incomplete stream safety proofs

A host regression reproduced a real completion-status bug in the buffered
WaveRT stream: NT_SUCCESS accepted pending/informational callback results as
completed lifetime/cache/hardware-reference proofs. In particular, incomplete
RemoveHardwareReferences could permit DMA/MDL release before actual recovery.

The three synchronous safety callbacks now require exact STATUS_SUCCESS.
Other nonnegative results become STATUS_DEVICE_NOT_READY; negative errors remain
unchanged. Incomplete pin does not enter allocation, incomplete cache verification
does not configure hardware, and incomplete recovery retains all lifetime/DMA
references and published storage for explicit retry. Providers must not queue
asynchronous work through these synchronous callbacks; failed pin owns no pin.
This is a safety gate correction, not a new cache or quiescence provider.

Regression injects STATUS_PENDING, a positive success-class value and an
informational-class value at all three boundaries. The original implementation
aborted on the new assertion; corrected code passes all nine cases, including
unchanged outputs before configuration and no frees until exact-success recovery.
NT_SUCCESS is intentionally broader than completed STATUS_SUCCESS:
https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-ntstatus-values

Full host regression passed; independent buffered-stream run passed 148248
checks with ASan/UBSan and warnings-as-errors. Official WDK audit
`/tmp/t70-audio-wdk-pending-1` compiled all 62 production units, no unresolved
project symbols. Diagnostic driver link still fails at missing DriverEntry.
No production callback provider, actual cache proof or hardware test was added.

## NTSTATUS completion audit and miniport admission fix

All production C/C++ NT_SUCCESS sites (19 files) and the declared NTSTATUS
callback boundaries were traced. The second reproducible defect was the
miniport admission boundary: pending Initialize marked resources initialized;
noncompleted Create/STOP/position could publish an owned stream as successful.
Exact STATUS_SUCCESS is now required at these synchronous admission stages.
Other nonnegative values become NOT_READY, negative errors are preserved,
outputs stay untouched and unpublished owned references are released once.
Provider contracts prohibit queued work, including initial STOP/position; the
rejection is not cancellation of a genuinely asynchronous operation.

Audit classification, rather than a blanket NT_SUCCESS replacement:
- WaveRtMiniportBoundary: fixed four provider/virtual admission boundaries.
- WaveRtBufferedStreamBase: previous exact-success safety gates retained.
- AudioResourceOwner and DmaAdapterOwner: exact-success quiescence gates already
  present. Remaining NT_SUCCESS checks consume concrete internal synchronous
  routines returning zero or negative errors, not external proof callbacks.
- AfeMapping, AfeCommonBuffer, AfeResourceMapping, ConfigureMapped, RouteMapped,
  FormatMappedObserve, AddressMappedObserve, CommonBufferConfigure,
  CommonBufferFill, WaveRtStreamConfigure: internal acquire/begin/end/release
  status propagation. No asynchronous success state is produced by those callees.
- WaveRtDmaOwner: OS CreateCommonBufferFromMdl is synchronous, with documented
  successful completion plus a returned logical mapping; no completion callback
  is passed. Other checks use concrete MDL owner routines. No claim that a fake
  pending return would be a supported OS mapping result; no speculative cleanup
  was invented for an out-of-contract asynchronous OS implementation.
- AwSpb: WDF synchronous sends return completed request status; valid
  informational completion is not the same as an outstanding async request.
  Exact transferred-byte checks remain. Its local adapters propagate these
  normalized complete results. NT_SUCCESS was retained here.
- AwSpbTarget, PortClsOwner, WaveRtPortBind: synchronous WDF/PortCls resource
  creation/open/initialization calls, not asynchronous user safety proofs.
  Their NT_SUCCESS uses remain under those OS contracts.

Primary contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-setstate
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-getposition
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pcreate-common-buffer-from-mdl
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetsendioctlsynchronously
- https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/sending-i-o-requests-synchronously

The new 3-positive-status by 4-stage regression failed on the original Init
path, then passed after the fix. Independent admission test: 292 checks under
ASan/UBSan and warnings-as-errors. No real asynchronous operation was executed.
Full host regression passed. Official WDK audit
`/tmp/t70-audio-wdk-admission-status-1` compiled 62 production units with zero
unresolved project symbols; diagnostic link still lacks real DriverEntry.

## Concurrent COM references versus serialized close

The concrete resource owner and miniport use Interlocked reference counts.
Their non-reference methods (Initialize/Begin/End/Close and miniport admission)
require external serialization and a caller-held reference. Parallel Close and
Begin without that serialization would violate the API contract, not expose a
new supported interleaving. No production caller currently supplies an OS removal
or rundown implementation; COM object refs alone cannot solve driver-image
unload or hardware removal, and no such implementation was claimed here.

The added test exercises concurrency that IS permitted: 32 rounds, eight workers
holding valid references to both actual objects before launch, each performing
1000 reference pairs while the main thread closes resources and relinquishes
its caller references. The mapping is unmapped once, neither object is freed
before worker references are released, and both are freed exactly once after
joining. Host counters involved in final destruction are atomic to avoid mock
races. Workers never acquire a reference from an unowned raw pointer, and no
non-reference method is intentionally raced outside its contract.

No production defect was reproduced in this supported lifetime interleaving.
No new lock or speculative owner wrapper was added. These finite stress cases
are not proof of arbitrary race freedom or a substitute for actual platform
remove/unload exclusion.

Validation: full host suite passed. Independent focused test passed 684 checks
with ASan/UBSan/-Werror; a separate focused ThreadSanitizer run also passed with
no reported race. Go/Drop barriers preserve lifetime but do not force every
reference-loop instruction to overlap Close; the stress test samples schedules.
Official WDK `/tmp/t70-audio-wdk-com-race-1`: 62 units compiled, zero unresolved
project symbols, final diagnostic link still missing real DriverEntry.

## Actual device-start resource composition

`T70AudioDevicePrepare` now connects explicit real startup objects through the
existing concrete implementations: AFE resource owner, concrete WaveRT miniport,
PcNewPort/IPort::Init binding, then OS DMA adapter acquisition. No device or PDO
is discovered/fabricated, and no DriverEntry stub or endpoint registration is
added. The actual installed stream factory returns NOT_READY, so this resource
connection cannot falsely create an incomplete production stream.

Prepare success denotes ownership of resources/port/adapter only, not successful
hardware start. It has no cache, codec, routing, IRQ, latency or DMA-start proof.
The integrating actual device still supplies its start IRP, translated resources,
matching PDO and OS removal/unload exclusion. Existing miniport/adapter references
are retained instead of replacing the lower ownership implementations.

Failure before publication uses a narrowly justified local cleanup proof: this
initializer has not exposed a stream, allocated any DMA buffer or written any
hardware register. It may only have acquired an AFE virtual mapping, PortCls
objects and an unused adapter. Partial cleanup remains visible for recovery.
After Prepare succeeds, Release instead requires the external real quiescence
callback. Adapter close precedes AFE owner close, then port, miniport and resource
owner references are released. Outstanding DMA/resource leases prevent their
respective destruction; a failure after adapter release retains the remaining
objects and retries without double PutDmaAdapter. No destructor guesses safety.

Official entry contracts used by this composition:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcnewport
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iport-init
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavert-init
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iogetdmaadapter

Tests call the real composition/miniport/resource/adapter implementations with
fake OS primitives, failing resource allocation, miniport allocation, port
creation, AFE mapping and DMA acquisition individually. They also verify failed
NewStream, pending quiescence, active lease rejection and partial teardown retry.
`Prepared` records the completed initialization milestone, not ongoing health.
Once Release is attempted, remaining fields are recovery-only: never resume use,
even if Prepared remains true during a partially completed teardown.

Validation: full host regression and independent focused 60 checks passed under
ASan/UBSan/-Werror. Official WDK `/tmp/t70-audio-wdk-device-prepare-1` compiled
63 production units with zero unresolved project symbols. DriverEntry remains
absent and diagnostic link incomplete. No real OS/device startup was executed.

## Existing PortCls device identity, not invented driver entry

The audio source has no actual DriverEntry, AddDevice/PcAddAdapterDevice caller,
registered endpoint or supplied platform device object. Existing PortClsOwner
requires real caller-provided callbacks; AudioDevicePrepare requires actual
startup objects. Official PortCls startup uses the device-stack/PDO/start-IRP
contract. That does not establish an equivalent non-PnP provider for this board.
No fake PDO, fabricated resource list, automatic enumeration assumption or
success-only entry-point stub was introduced.

A concrete missing check in the existing composition is now implemented:
AudioDevicePrepare uses PcGetPhysicalDeviceObject on the already supplied valid
PortCls device and verifies its existing PDO equals the explicit DMA PDO. Failed,
pending, NULL or mismatched results fail before allocation, mapping or DMA
acquisition. This is association verification, not discovery. It prevents two
independently supplied device pointers from silently being used as one device.
The API cannot make an arbitrary invalid DEVICE_OBJECT safe; the integrating
platform must first provide a real PortCls device and exclude removal. The
association query is used as a borrowed result, consistent with Microsoft's
SysVAD usage; DmaAdapterOwner subsequently takes its own explicit PDO reference.

Sources:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/startup-sequence
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcaddadapterdevice
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcgetphysicaldeviceobject
- https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/common.cpp

Still needed: an actual platform-provided Windows device/start-resource lifecycle
compatible with the selected audio class interface. The current non-PnP setup
has not supplied that contract. A static board MMIO tuple is not a PDO, start IRP
or permission to claim/assign resources. This check does not close that gap.

Validation: full host regression passed. Independent 68-check review passed;
its additional input-device assertion was applied (final focused count 79).
Official WDK `/tmp/t70-audio-wdk-pdo-identity-1` compiled 63 production units
with zero unresolved project symbols; PcGetPhysicalDeviceObject resolved through
the official PortCls library in the diagnostic link. Real DriverEntry is still
missing; no actual device creation, start or endpoint publication occurred.

## Architecture reassessment: non-enumerated hardware is not a PortCls provider

The largest remaining integration blocker is the actual OS device lifecycle,
not another format/lease helper. A source inventory still finds no audio
DriverEntry, actual device creation/reporting callback, installation definition
or endpoint registration. The implemented AudioDevicePrepare is only callable
with a real existing PortCls device and startup objects; it cannot manufacture
those prerequisites by composing more current helpers.

Microsoft documents a real legacy-device reporting route, IoReportDetectedDevice.
It is NOT equivalent to keeping a purely non-PnP service: it establishes a
persistent root-enumerated OS device, requires a matching INF for later boots,
and needs device-state IRP handling. Critically, on the first reporting call
the manager considers the device started and does not deliver AddDevice or a
start IRP. Later boots follow a different lifecycle. Therefore simply passing
its returned PDO into the existing PortCls startup routine would leave the first
boot's actual PortCls device/start-IRP/resource construction unresolved. The PDO
is OS-owned; inventing one or treating IoDeleteDevice as generic rollback would
be wrong. No such registration, fabricated IRP or inferred resource assignment
was implemented or executed.

A non-PnP KMDF control-device/service can be loaded by a service manager, but
that alone does not expose a Windows audio endpoint or provide PortCls hardware
resources. Adding a diagnostic-only DriverEntry would hide rather than solve
the user's playback goal. No such substitute was added.

Concrete paths that require an explicit platform decision/implementation:
1. A real root/platform-enumerated audio device with assigned resources and a
   matching installation definition, then actual PortCls start/stop/rundown.
   This changes the OS device representation; it must not be assumed acceptable
   merely because the physical device itself cannot enumerate.
2. Retain a strictly non-PnP service architecture and supply a separately proven
   Windows audio-class integration/lifecycle. The present source and reviewed
   official material do not establish such a provider for this board.

Neither path removes the independent hardware/cache/IRQ/routing/quiescence gaps.
Until the lifecycle is chosen, further pass-through wrappers or a success-only
entry point are not an executable solution. This is an evidence-only boundary
record, not a new test/build/start success or a claim that Windows can never
support this non-enumerated hardware.

Sources:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-ioreportdetecteddevice
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-ioreportresourcefordetection
- https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/installing-a-non-pnp-driver
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/startup-sequence

## Existing ACPI device inventory before adding an audio binding

Read-only inventory at ACPI submodule commit
`6859d4b4e92c6c3e443c2d3094b5650f7284f667` confirms that the target
AcpiTables.inf includes MADT, GTDT, FADT, PPTT and DSDT only. No separate
audio SSDT is included. The target DSDT has no AFE device/resource declaration
for the proven `0x11210000/0x2000` range, no AW88261 device, and no MT6366 audio
device. This is a source inventory, not an observation of Windows enumeration.

Existing I2T0 and I2C6 use `HANA6789`, which Windows/MtkI2c/MtkI2c.inf matches
as an I2C controller. Those resources and identities are not an audio PDO and
must not be reused to bind the AFE miniport. Device status remains conditional;
source presence alone does not prove the OS has a usable controller instance.

The audio directory still has no installation INF or real DriverEntry/AddDevice
implementation. AudioDevicePrepare requires an already valid PortCls device,
its real start IRP and assigned resource list; it verifies the associated PDO.
Its stream provider remains NOT_READY. A static AFE MMIO tuple supplies none of
those lifecycle objects. A newly declared audio node would need a coordinated
project device identity, matching install package and actual start/stop/removal
implementation, not a borrowed controller ID or an INF naming an unlinked image.
No new identity, namespace node, fake startup object or ready endpoint was added.

Alternative class frameworks do not establish a missing provider by themselves:
KsCreateDevice requires a real physical device object, while ACX ties its audio
circuit lifecycle to OS device enumeration. The legacy-reporting first-start
limitation recorded above therefore remains relevant; none of these findings
assert that a Windows driver for this hardware is impossible.

This pass changed only this record. No ACPI/power or concurrent USB source was
modified, and no firmware build, installation, device creation or hardware I/O
was attempted. The remaining gap is technical, not a newly encountered tool
permission denial. No new runtime or compile success is claimed for this entry.

Sources:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/hardware-ids
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcaddadapterdevice
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/nf-ks-kscreatedevice
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/acx-device-enumeration

## Development AFE device binding and real DriverEntry (supersedes absent-entry gap)

The project explicitly assigned the new development identity `HANA6793` after
the inventory above. It is not represented as an official MediaTek ID. A separate
Audio.asl SSDT now declares AFE0 and is included in AcpiTables.inf without editing
DSDT/USB. Its single MMIO resource is 0x11210000/0x2000; the stock AFE node's
GIC SPI 234, flags 4 becomes level/high GSI 266. No clocks, power resources,
register operations, codec/amp declarations or DMA coherency assertion were added.
The separate overlapping 0x1000 syscon description is NOT the PCM resource used.

MtkAudio.inf matches exactly that ID, with a development-only description.
AudioDriver.cpp provides actual DriverEntry and AddDevice callbacks, using
T70PortClsInitialize and PcAddAdapterDevice. StartDevice receives the OS's real
FDO/start IRP/resource list, then runs the existing resource owner, WaveRT
miniport and Port.Init path. It requires exactly one memory resource; the
existing owner validates both raw and translated MMIO address/length. This maps
CPU virtual address space only; it never reads/writes the AFE or acquires DMA.

All objects remain private to the callback. The driver registers no subdevice,
exposes no resource/stream lease, and issues no hardware operation. It closes
its mapping and releases Port/miniport/owner before returning. Successful object
initialization deliberately becomes STATUS_DEVICE_NOT_READY: Windows device
start is expected to fail (typically Code 10), NOT to expose working audio.
PortCls retains responsibility for its FDO and default failure/removal handling.
There is no persistent device bundle, custom PnP handler or duplicate FDO deletion.
The local cleanup proof is about this driver's unpublished references only;
it does not assert that firmware/another owner has stopped the physical DMA.

This is a real, fail-closed development binding rather than a successful fake
endpoint. The firmware/device identity bridge is now explicit; real stream,
IRQ, codec/clock/routing, DMA/cache and hardware release proofs remain absent.
Do not install or load it expecting sound. INF validation, catalogue generation,
signing, Windows installation and actual loading have NOT been performed.
No complete firmware image containing this new SSDT has been built or flashed.

Validation:
- Full host suite passed with ASan/UBSan/-Werror. The new focused test invokes
  actual DriverEntry/AddDevice/StartDevice code with fake PortCls/OS services,
  including allocation, port and mapping failures and invalid AFE resources.
- Official WDK compile covers 64 production translation units, no mock headers.
  C linkage was added to PortClsOwner.h for its new real C++ consumer.
  All project symbols resolve and the ARM64 native driver link now succeeds.
  This is not signature validation or kernel-load validation.
- test-audio-binding.py compares project ID/INF/ACPI inclusion and stock DTB
  resources, compiles only Audio.asl and checks its AML resource bytes/checksum.
  The 104-byte SSDT compiles with zero errors, warnings or remarks.

Reproduce with the existing official-header check-wdk-components.py command;
its successfully linked image is now named MtkAudio.sys to match the INF, and
its report records the image hash. No software or driver installation is run.

Final official build report: `/tmp/t70-audio-wdk-entry-final/report.json` on
builder; image `/tmp/t70-audio-wdk-entry-final/MtkAudio.sys`, SHA-256
`67a8f84b37120bb4f2b8010033bc32305884dce350064a7a468fd6e7d27425ad`.
Independent review reran the focused test and found no blocking lifetime defect
in this deliberately unpublished path. Its recommendation to directly assert
zero IoGetDmaAdapter calls was applied, together with zero/multiple-memory-resource
rejection checks. Host mocks do not establish actual PortCls internal references.

## Buffered WaveRT stream COM ownership implementation

WaveRtComStreamBase implements the actual IMiniportWaveRTStream ownership
boundary on top of the existing buffered stream, rather than another read/write
callback wrapper. It recognizes only IUnknown and IMiniportWaveRTStream using
the official WDK IID, returns the same interface identity, adds one reference
on successful QueryInterface, and clears unsupported-interface outputs.
Interlocked references protect concurrent valid holders. Final Release invokes
the concrete provider's virtual destructor and frees its nonpaged allocation
before dropping the retained parent owner. Allocation failure does not construct
the object or retain its parent.

This closes the previously absent COM/destruction implementation, not the entire
concrete hardware provider. The base is deliberately still abstract: real
GetHWLatency, recovery lifetime pin/unpin, mapping suitability and hardware
reference removal remain mandatory. No unknown latency is replaced with zero,
no stream is registered, and the development driver still fails start. Parent
reference retention is not an OS remove lock or driver-unload barrier; the real
provider must meet those documented obligations and avoid parent/stream cycles.
Factories must validate the plan and non-null parent before construction.

Host tests instantiate a concrete mock derivative, exercise QI identities and
allocation failure, verify derived destruction/free/parent-release ordering,
and perform 80,000 concurrent reference pairs with references acquired before
worker launch. The production GetHWLatency remains absent rather than inventing
a value; the test's fake latency callback is never called. This is not proof of
hardware access, latency, endpoint creation or a runnable stream.

Official WDK report: `/tmp/t70-audio-wdk-comstream-1/report.json` compiled 65
production units and linked successfully with no unresolved project symbols.
This working-tree build also includes the three preserved, uncommitted IRQ
validation files from the stopped task; those are NOT part of this COM commit.
Accordingly that artifact is not claimed to represent the exact committed tree.

Source: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iminiportwavertstream

Full host regression passed. Independent focused ASan/UBSan/-Werror review
passed 18 checks plus 80,000 reference pairs after fixing a mock name-shadowing
compile error. The test retains the initial reference until worker joins; it
therefore verifies concurrent reference operations, not every possible final
release interleaving or an OS removal/unload race.

## Concrete buffered stream factory and NewStream boundary

WaveRtStream is now a concrete IMiniportWaveRTStream implementation, reusing the
existing COM and MDL/DMA buffer transaction code. Its Create signature directly
matches the miniport provider's Create callback. The host integration exercises
the real MiniportBoundary::NewStream calling this factory, initial STOP and zero
software position, then publishing the owned stream pointer only on success.
This is not wired to AudioDriver's missing board provider or to a live endpoint.

Creation requires an explicit retained owner, real-port interface, stable DMA
adapter/AFE owner storage and every hardware-lifecycle provider. Missing providers
return NOT_READY; unsupported format/geometry is rejected before Describe. The
synchronous Describe query must supply actual format-specific latency and confirm
admission prerequisites, acquiring no ownership. Only exact STATUS_SUCCESS is
accepted; positive incomplete statuses are not readiness. The constructor owns
its transaction, copies the callbacks/latency and retains the parent and Port.
Allocation failure preserves the output without retaining either reference.

Describe does NOT prove an allocation's cache suitability: that is still checked
by the mandatory Verify provider for the actual MDL/DMA allocation. Pin must
exclude device teardown and unload; the class additionally holds its own COM
reference across recovery. Unpin runs before dropping that reference. The real
provider remains responsible for eventual recovery after partial hardware
application and cannot treat a stop request as complete buffer-reference removal.

GetHWLatency copies the provider's immutable FIFO-byte and 100-ns delay values;
no board latency was invented. SetFormat accepts only the already admitted rate,
format and channel count, avoiding a stale latency snapshot after renegotiation.
GetPosition returns initial software zero only before a buffer transaction is
admitted, then NOT_READY rather than an invented hardware position. RUN remains
NOT_SUPPORTED. Thus a concrete object now exists, but production playback,
position/IRQ completion, cache proof and hardware recovery are still missing.

Host regression tests use fake providers and invoke actual NewStream/factory,
missing-provider and describe-error paths, strict format/geometry, allocation
failure, retained Port/parent references, pin failure, MDL allocation failure and
balanced recovery. Official WDK report
`/tmp/t70-audio-wdk-stream-factory-1/report.json` compiled 66 production units and
linked with no unresolved project symbols. As with the previous report, the
working-tree build includes the preserved uncommitted IRQ files and is not an
exact committed-tree driver image. No driver load or actual resource I/O ran.

Final validation: full host regression passed. Independent 48-check execution
with ASan/UBSan/-Werror also passed, including concrete Verify failure cleanup,
partial first configuration-write failure, failed Remove/recovery retention,
subsequent successful recovery and duplicate-Recover no-double-free behavior.
These tests retain the external stream reference throughout recovery; they do
not directly exercise survival on only the internal recovery reference.
Final official 66-unit compile/link report is
`/tmp/t70-audio-wdk-stream-factory-final/report.json` (same dirty-tree caveat).

## ARM AFE cache-coherency declaration and remaining WaveRT restriction

The new AFE0 bus-master description was missing _CCA, which ACPI requires on
ARM. Audio.asl now declares _CCA=0 as the conservative noncoherent policy
supported by the stock declarations/default configuration. This is a real OS
resource-description correction, not a new power resource or a claim that cached
WaveRT playback is ready.

Read-only evidence: effective.dtb's AFE node, /soc and / have no dma-coherent
property. /soc dma-ranges maps an identity address interval but says nothing
about cache visibility. The supplied stock-kernel.config identifies 6.12.38,
has ARCH_HAS_SYNC_DMA_FOR_CPU/DEVICE enabled and no enabled
ARCH_DMA_DEFAULT_COHERENT or OF_DMA_DEFAULT_COHERENT. Its SHA-256 is
84a38e4bf471ae21d43bf573ba93b698005ce76f3c0603c463d83dbf9622108b.
Linux 6.12's OF path walks DMA parents then uses the configured default. This
supports the conservative policy; it does NOT prove that no vendor override or
runtime coherency change exists, and it is not a measurement of physical snoops.
No Linux implementation was copied into this BSD-licensed code.

Microsoft documents that ARM common-buffer caching depends on _CCA. That does
not automatically establish the suitability of separately allocated cached
WaveRT MDLs. MapAllocatedPages requires client memory without extra platform
alignment restrictions; substituting uncached Device Memory blindly would break
that contract. The Intel HDA recommendation for MmWriteCombined is not proof of
T70 ARM64 memory attributes. FlushDmaBuffer documents an MDL passed to MapTransfer;
our CreateCommonBufferFromMdl path has no such transfer. No speculative flush,
noncached alias or default-success Verify callback was added. The real board
Verify provider is still absent, so cached WaveRT creation remains unapproved.

Validation: test-audio-binding.py now requires --kernel-config in addition to
--dtb, checks the source policy and emitted _CCA AML value, and compares stock
resources. The 111-byte audio SSDT compiles with no errors/warnings/remarks.
Independent review agrees with the conservative declaration and explicitly notes
that vendor/runtime overrides and live memory attributes remain unverified.

Sources:
- https://uefi.org/specs/ACPI/6.5/06_Device_Configuration.html (6.2.17)
- https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/of/address.c
- https://code.googlesource.com/linux/torvalds/linux/+/3a2976df778a9af95e91f7ff88008b4517ddc658/kernel/dma/mapping.c
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pallocate_common_buffer
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-mapallocatedpages
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-pflush_dma_buffer

Two temporary contradictory-coherency configurations were rejected before AML
compilation. Official WDK `/tmp/t70-audio-wdk-cca-1/report.json` again compiled
66 production units and linked; preserved uncommitted IRQ files remain part of
that working-tree build but are excluded from this commit. No C/C++ production
source was changed in this cache-declaration step.

## WaveCyclic OS-channel buffer path (2026-11-18)

`WaveCyclicBuffer` now calls the real `IPortWaveCyclic::NewMasterDmaChannel`,
`IDmaChannel::AllocateBuffer`, `CopyTo`, and `FreeBuffer` interfaces. It retains
both the initialized port and returned channel, rejects failed/nonzero status,
validates returned CPU storage/size, and prevents release during explicit leases.
It requests 32-bit addressing (a conservative subset of AFE's 34-bit capability),
not a guessed 34-bit flag or CPU-physical-to-device-address conversion.
The optional AllocateBuffer constraint is NULL: that parameter is a boundary
mask, not a maximum device address. No DMA address is published by this step.

This is an unpublished S16 mono/stereo buffer owner and RAM-to-OS-channel copy
path, not a WaveCyclic miniport, a NewStream provider, or an active refill/start
implementation. Source and all metadata must be resident, externally serialized;
no other port/device consumer can use this buffer yet. CopyTo receives a checked
contiguous frame-aligned subrange; wrap must be split by a future stream owner.
It delegates copy behavior to the OS's channel instead of ordinary memcpy into
possibly Device Memory or pretending a cached WaveRT mapping is coherent.
The CopyTo void contract cannot report copy failures. There is no timer, IRQ,
DMA start, hardware access, cache flush invention, or playback-success claim.
WaveRT code remains unchanged. Physical-address meaning/cache policy for the
actual initialized port's DMA channel must be established before AFE publication.

Primary contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavecyclic-newmasterdmachannel
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-idmachannel-allocatebuffer
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-idmachannel-copyto
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/wave-filters

Official ARM64 WDK audit `/tmp/t70-wdk-cyclic-1/report.json`: 67 production
translation units, no mock headers, linked `MtkAudio.sys`, no unresolved project
symbols. Existing three stopped IRQ edits remain uncommitted and are included
in the working-tree audit, so this is not a pristine committed-tree image.
Builder host-ASan launch failed because its clang runtime libraries are absent;
no installation was attempted. The Mac host regression uses its existing ASan
and UBSan instead. These are host/compile checks, not installation or playback.
Mac complete host suite passed (`/tmp/t70-cyclic-suite-mac.log`). Independent
review reran the focused ASan/UBSan test and found no blocker under the stated
unpublished-buffer contract. Its additional NULL-success, short allocation,
lease overflow/underflow, failed-copy no-write and retry cases were added and
passed in a subsequent focused run. No production change followed WDK validation.

## WaveCyclic miniport and concrete stream interfaces (2026-11-18)

`WaveCyclicMiniport` implements actual `IMiniportWaveCyclic` COM, Init and
NewStream signatures, with an explicit synchronous initializer/stream factory.
No real factory is fabricated: success requires three independently retained,
compatible stream/channel/service-group objects and actual hardware/notification
admission. Partial factory outputs are released; positive nonzero statuses are
not completion. Metadata is S16 mono/stereo at the existing 11 rates; intersection
negotiation remains unsupported, and there is no registered speaker endpoint.

`WaveCyclicStream` is a concrete `IMiniportWaveCyclicStream`, not a mock or an
abstract vtable. It leases the existing WaveCyclic OS-channel buffer, retains an
explicit owner, and self-pins until explicit PASSIVE Close releases that lease.
A final external Release cannot prematurely free the buffer. Signed-S16 silence
uses RtlCopyDeviceMemory in bounded zero chunks (ordinary memcpy is not used on
the destination). Same-format admission, byte-to-100ns conversion without
multiplication overflow, canonical QI and COM lifetime are implemented.

This stream is still deliberately UNPUBLISHED: it has no hardware state/IRQ
provider, so RUN/ACQUIRE/PAUSE and position queries return errors, notification
returns zero budget, and STOP acknowledges software state only. It must NOT be
passed as a successful miniport NewStream result. The actual board factory,
service-group/IRQ notification and safe device transition/rundown remain absent.
No timer or invented playback position is supplied. This step connects the
concrete software stream to the OS buffer lease, not to running hardware.
No existing WaveRT implementation or stopped IRQ edits were changed.

Official contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iminiportwavecyclicstream
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclic-newstream
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-normalizephysicalposition
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-silence
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-setnotificationfreq

Miniport Close closes admission permanently and requires exact successful
external rundown confirmation before dropping its Port reference; this breaks a
possible Port/miniport reference cycle instead of assuming destructor order.
Failed/pending confirmation retains the reference and permits retry. No hardware
stop is inferred from this confirmation callback. Independent narrow miniport
review/tests cover 193 checks; independent stream tests also pass ASan/UBSan.
Final full Mac ASan/UBSan regression passed (`/tmp/t70-cyclic-stream-final.log`).
Official WDK ARM64 audit `/tmp/t70-wdk-cyclic-stream-final/report.json` compiled
69 production units and linked the development MtkAudio.sys with no unresolved
project symbols. Existing stopped IRQ edits remain part of that dirty-tree
compile, not this commit. No install/load/INF validation or playback was done.

## WaveCyclic actual PortCls startup binding owner (2026-11-18)

`WaveCyclicPortOwner` connects the concrete cyclic miniport to real
`PcNewPort(CLSID_PortWaveCyclic)` and `IPort::Init`, passing the actual StartDevice
DeviceObject/IRP/resource list and explicit adapter. No objects or resources are
invented and no subdevice is registered. The synchronous resource initializer
is supplied explicitly and owns any references it retains after Init.

Ownership is visible even on failure: if provider cleanup cannot be proven,
the owner keeps both interfaces and separate InitStatus/CleanupStatus so the
adapter can retry Close instead of losing its recovery references. Exact status
zero is required at every completion boundary. Close requires external provider
rundown confirmation (including partial failed initialization), breaks the
miniport-to-port cycle, then releases local port and miniport references.
The already-established proof is passed to miniport Close without executing a
stateful cleanup callback twice. This is not an implicit hardware stop.

Mock Port.Init actually invokes the production miniport.Init and retains its COM
reference. Tests cover allocation/create/initialization failure, successful
miniport.Init followed by failed Port.Init, PENDING, retained failed cleanup,
retry/idempotence, normal-close failure and preservation of original InitStatus.
Independent ASan/UBSan review found no blocker within the unpublished/serialized
contract. The board's DriverEntry still uses its existing development path:
these objects are not installed, published, streaming or running an IRQ.

Contracts: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcnewport
and https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iport-init
Complete Mac host regression passed (`/tmp/t70-cyclic-port-suite.log`); additional
review-requested cleanup assertions passed in the focused test. Official WDK
ARM64 `/tmp/t70-wdk-cyclic-port-1/report.json`: 70 production units and linked
development image, no unresolved project symbols. Existing three stopped IRQ
edits remain uncommitted and included in this dirty-tree compilation only.

## WaveCyclic service group/notification ownership (2026-11-18)

`WaveCyclicService` creates the actual PcNewServiceGroup and retains its port.
An interlocked admission/count gate lets a real, independently verified ISR
submit IPortWaveCyclic::Notify while preventing release during that call.
Close first seals admission, requires no active calls and external exact-success
rundown proof, then releases group/port. Failed proof retains ownership.
The proof must cover disconnected ISR, pending downstream service work and all
exported group references; the gate cannot establish those hardware/OS facts.
This is not a timer, ISR, ACK implementation or a completed playback endpoint.
Primary API: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcnewservicegroup
and https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavecyclic-notify

Independent review and controlled concurrent Notify/Close host test passed
ASan/UBSan. Additional count-limit, NULL confirmation and IRQL failures passed.
Official WDK ARM64 `/tmp/t70-wdk-cyclic-service-1/report.json`: 71 production
units, linked development image and no unresolved project symbols. Existing
three stopped IRQ edits are uncommitted dirty-tree inputs, not changes here.
Complete Mac ASan/UBSan regression passed (`/tmp/t70-cyclic-service-suite.log`),
followed by the focused review-requested additional boundary checks.

## Source-derived AFE DL1 ISR entry (2026-11-18)

`AfeDl1Interrupt` now connects the stock fixed IRQ0/status-mask algorithm to the
real WaveCyclic service Notify function and explicit register I/O callbacks.
Its full-domain ownership, stock bit31 treatment, error quarantine and missing
OS IRQ connection are detailed in AFE-IRQ-COUNT-EVIDENCE.md. It must not be
connected merely because a caller sets DomainExclusive=true. No actual MMIO,
interrupt enable, timer or playback was run.
Independent review found and corrected the ISR's recognized-source return value:
after selecting a nonzero mask, notification/clear failure now returns TRUE
while latching the error. Read failures before source identification return
FALSE. The review rerun passed. Official final WDK ARM64 audit
`/tmp/t70-wdk-isr-final/report.json` compiled 72 production units and linked the
development image with no unresolved project symbols. This includes the three
preserved dirty IRQ validation files, not a pristine committed-tree image.
Final full Mac ASan/UBSan regression passed (`/tmp/t70-isr-final-suite.log`).
No device, live interrupt, hardware MMIO, power operation or firmware build ran.

## Actual Windows interrupt connection owner (2026-11-18)

`AfeInterruptOwner` now calls IoConnectInterruptEx with
CONNECT_FULLY_SPECIFIED_GROUP and the real AfeDl1Interrupt routine. It requires
an explicitly supplied actual PDO plus translated vector/IRQL/group/affinity;
raw GSI266 is checked but is never substituted for the translated vector.
This branch only accepts the board's exclusive, level-sensitive interrupt
contract. The real adapter must obtain/validate that resource tuple; this code
neither discovers a PDO nor invents resource ownership. A supplied Ready routine
must establish the whole AFE domain exclusive and source disabled before connect.

Parent and service group are retained and handler context populated BEFORE the
OS call, since an ISR can run before connection returns. OnFault now has a
mandatory nonblocking DIRQL-safe scheduling provider in this owner. It must
queue real recovery, wait until Connect ends, then enter serialized PASSIVE
Close; no work-item/timer or masking implementation is fabricated. A fault
before return leaves the valid connection retained and reports NOT_READY.
Failed OS connect also retains Parent/Service until explicit Close drains any
pre-return callback work. This addresses an independent review's lifetime gap.

Close requires actual source masking, calls IoDisconnectInterruptEx with the
returned version/object, then drains queued service/recovery work before
releasing Group/Port/Parent. Mask/Drain errors retain the appropriate ownership
for retry. Successful OS output with no object or an unexpected version is
quarantined without guessing disconnect parameters. Tests exercise those
malformed responses; fixture-only counter reset is NOT production recovery.

There is still no board Ready/Mask/OnFault recovery supplier, so DriverEntry does
not invoke this owner. It is compilable OS connection code, not a connected
interrupt, proved quiescent device, or playback result. Existing stopped three
IRQ validation edits and USB/other work are untouched.

Official contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_io_connect_interrupt_parameters
- https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-the-connect-fully-specified-version-of-ioconnectinterruptex
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iodisconnectinterruptex
Independent review rerun and complete Mac ASan/UBSan regression passed
(`/tmp/t70-irqowner-final-suite.log`). Official final ARM64 WDK audit
`/tmp/t70-wdk-irqowner-final/report.json`: 73 production units, linked image,
no unresolved project symbols. The preserved three dirty IRQ validation files
remain included in this compile, not committed here. No OS IRQ was connected,
no device registers were executed, and no driver was installed or loaded.

## IRQ0 source-mask provider (2026-11-18)

`AfeIrqSource` adds stock-derived IRQ0 ON-bit disable/clear and bounded readback,
actual KeSynchronizeExecution Close-side exclusion, and immediate ISR-fault
masking before calling the parent's required recovery scheduler. This is NOT a
new scheduler, complete hardware recovery, DMA stop, restart, or playback start.
No DriverEntry hookup without the remaining actual providers is claimed.

Independent review and full Mac ASan/UBSan host suite passed. New tests cover
all I/O failure stages, other-source preservation/rejection, lock contention,
pre-Connect-return fault context, and source callback through real Close with
Drain failure/retry. Integrated test calls OnFault directly; real ISR failure
identification is covered separately, not claimed as live interrupt delivery.
An initial host C-header mock declaration failure was fixed with a C++ guard.

Official WDK/SDK 10.0.28000.2526 ARM64 compile/link audit passed for **74**
production translation units; actual KeSynchronizeExecution resolves through
kernel imports. No mocks in that compile. Report `/tmp/t70-wdk-irqsource/report.json`
on builder, image SHA256:
`b0f81803f8647bad358c64895740b9011ab70323462b9ba8d42fcb5b0604b348`.
The tree still includes the three preserved uncommitted AudioDriver/test/mock
IRQ-validation edits; this is NOT an exact committed-tree build. No firmware
build, signing, install, driver load, device access, or sound execution occurred.

## Deferred IRQ recovery integration (2026-11-18)

Actual KDPC → IoQueueWorkItem → PASSIVE source-mask/disconnect is implemented
with connect publication, kernel mutex serialization, and Close-side DPC/work
completion drain before external service drain/ref release. No auto restart,
fault erasure, DMA-idle claim, power work, or hardware execution. Initial work
allocation is tied to a real caller-provided Device whose lifetime Parent owns.

Final independent review and full Mac ASan/UBSan suite passed. Actual ISR failure
before successful/failed Connect return, pre-Publish DPC, worker/Close failures,
first error preservation and retained-owner explicit retry are covered by fake
OS scheduling. This is not live Windows kernel concurrency verification.

Official ARM64 WDK/SDK compile/link: **75 production translation units** passed,
no project symbols unresolved, linked image SHA256
`889d21fb24be72b30495680134b1bb7223805bf9755b77f60a2b36a05a1cfb60`.
Report: builder `/tmp/t70-wdk-recovery-final/report.json`; final host log on Mac
`/tmp/t70-recovery-final-suite.log`. Preserved three uncommitted IRQ validation
files remain included in compilation, so this is not an exact committed-tree
build. No signing/install/load, firmware build, or actual I/O/IRQ execution.

## WaveCyclic board startup/IRQ ownership integration (2026-11-18)

`WaveCyclicBoard` composes actual PortCls initialization with mapped AFE access,
interrupt connection, source masking and deferred recovery. It consumes real
StartDevice arguments, explicit actual PDO and a retained resource list. Raw AND
translated AFE memory must be 0x11210000/0x2000; the exclusive level IRQ raw vector
must be266. The OS-translated vector/IRQL/group/affinity are copied, not inferred
from the GSI. Group0xffff is rejected when NT_PROCESSOR_GROUPS is present.

Order: real exclusive-domain claim → mapping/whole-lifetime lease → existing
WaveCyclic PortOpen → QueryInterface(IPortWaveCyclic) → recovery allocation →
read-only ON/selected-status disabled check → OS IRQ connect. No IRQ enable or
DMA start happens. ClaimDomain remains an essential actual platform provider:
it must exclude all firmware/DSP/domain clients, assure accessible MMIO and
compatible mappings, and retain that ownership. Numeric resources/readback are
NOT evidence of that handoff or power state. Without this provider this owner
must not be started. No DriverEntry publication or working playback is claimed.

Close reverses the dependency chain: mask/disconnect/deferred work/service drain
→ Port/stream detach → mapping lease end/unmap → domain release → list/parent
references. Every intermediate failure retains remaining resources. PortOpen's
own failed cleanup is not automatically retried in the same Open call. Started
is cleared immediately on Close even when later cleanup fails. Domain release
must work after unmap. DetachStreams still requires real DMA/hardware/client
quiescence; IRQ disconnect does not establish that independently.

Host integration executes actual mapping, IRQ and recovery owners with fake OS
and register primitives. The already separately tested PortOwner edge is stubbed
in this integration test. It covers invalid memory/IRQ, claim/map/port/QI/ready/
connect failure, partial PortOpen cleanup failure, service/stream/domain cleanup
failure and retained explicit retry. Nonzero/invalid processor-group extraction
is NOT executed by the reduced host descriptor; official WDK compile covers its
real type only. The three preserved dirty files, including that mock descriptor,
were not modified for this task.

Official WDK found a genuine ULONG* (unsigned long*) versus uint32_t* (unsigned
int*) difference hidden by the host ABI. The mapped-read bridge now receives a
uint32_t temporary and copies to ULONG only on success. Final compile/link audit
passed **76 production units**, no unresolved project symbols. Image SHA256:
`88c1d43a50284f4b782b710ea253758759563c71ab0e5bfe41d632920fd9d51a`.
Report: builder `/tmp/t70-wdk-board-final/report.json`. Independent ASan/UBSan
review passed; full host log `/tmp/t70-board-final-suite.log`. This still includes
the three preserved uncommitted edits, not an exact committed-tree build.
No firmware build, signing/install/load, actual mapping/IRQ/MMIO, or sound test.

Official resource and interface contracts:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_cm_partial_resource_descriptor
https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-the-connect-fully-specified-version-of-ioconnectinterruptex
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iportwavecyclic

## Concrete stream DMA configuration and trigger request (2026-11-18)

WaveCyclicStream now has explicit unpublished ConfigureDl1/RequestStartDl1
integration. Configure reads the actual PortCls master channel PhysicalAddress,
revalidates its current capacity/active size and the full 32-bit allocated range,
then prepares and invokes existing DL1 address→format configuration. This is the
address supplied by this device's DMA channel, NOT an arbitrary CPU-physical
address identity assumption. A separate real callback must establish stopped,
exclusive access before configuration. Source allocation/context lifetimes are
retained through every partial failure.

The start request ports the stock *normal period-wakeup DL1 render* trigger
subset: memif SET alias0x1b04 bit4 → observation0x1b00 → IRQ0 count0x300 → rate0x2e4
→ ON0x3a0 bit0. mtk_memif_set_enable uses descriptor+0x54/+0x58 and +4 when
bit-banding is active (stock common 0x2014..2040 and0x2078..20a4). T70 trigger
0x130 enables memif,0x1a8 count,0x1d0/0x1e8 rate,0x200 IRQ enable. Firmware-managed
or no-period-wakeup branches are NOT silently treated as this mode. Unknown
SET-alias readback bits are rejected; unchanged values skip writes. No power,
clock, codec/amp, reset or DMA-drain sequence is invented.

Whole transactions run through an explicit executor; the real available
T70AfeIrqRecoveryExecute holds its lifetime mutex and KeSynchronizeExecution ISR
lock while running the bounded nonpaged callback. It rejects closed/closing,
disconnected or faulted owners. Independent review found a real race in an early
version: an ISR could fault after the mutex-side check but before the ISR lock.
The final implementation rechecks Faulted INSIDE the synchronized callback;
a late-fault injection verifies zero transaction execution. This prevents a start
request from re-enabling an ISR-masked source. No callback sleeps/reenters.

A separate Ready callback must prove the actual configured clocks/route/amp,
DMA visibility, masked IRQ0/service, exclusive resources and recovery ability.
There is no default success provider. Start callback completion is NOT converted
to KSSTATE_RUN: ordinary SetState RUN/position/notification remain incomplete and
the stream is still unpublished. The direct methods are actual register-call
connections, not a claim that normal PortCls playback now works. Once config is
attempted, even STOP no longer claims software-only success; Close requires the
real RemoveReferences proof before dropping the buffer lease. Start requests are
one-shot; partial failure never triggers rollback or automatic retry.

Real capacity defect fixed: AllocateBuffer may return storage larger than the
channel's supported MaximumBufferSize. Acquire now checks both maximum and
allocated capacity, then verifies BufferSize after SetBufferSize. A mismatch
frees only the still-unpublished allocation and fails.

Host tests: all12 configuration and9 trigger I/O failure positions, exact start
order, no-write alias/equal values, unknown alias rejection, negative/over32-bit/
end-overflow DMA addresses, Access/Ready/Execute failure and positive statuses,
executor not running its callback, retained buffer/Close proof, one-shot start,
and synchronized late ISR fault/closing rejection. Tests use mock hardware and
bounded scheduling, not physical DMA/IRQ validation.

Sources: stock module hashes in AFE-IRQ-COUNT-EVIDENCE.md; pinned same-SoC source
facts independently encoded, not copied GPL implementation.
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/dma-channel-objects
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-setstate

Final full host ASan/UBSan suite and independent Stream/Buffer/Recovery reruns
passed. Official WDK/SDK ARM64 **77 production units** compiled/linked with no
unresolved project symbols; image SHA256
`ff29d8ee668a13520c57e87b2b7b6573a2fc84c8113b7f92c38389ceeadfc293`.
Builder report `/tmp/t70-wdk-streamstart-reviewed/report.json`, Mac full host log
`/tmp/t70-streamstart-final3-suite.log`. Preserved three dirty files remain in the
build, so not an exact committed-tree artifact. No signing/install/load, device
I/O, DMA, interrupt, firmware build or power execution was performed.

## CUR-based WaveCyclic position and nonblocking IRQ lease (2026-11-18)

GetPosition is now connected to an uncompensated CUR-based best estimate after
successful explicit start and binding the actual nonblocking observation owner.
Initial/pre-start STOP returns zero. Failure/invalid source preserves output.
The whole DMA allocation must remain below4GiB; high==0 is verified and buffers
crossing that boundary are rejected. This avoids that rollover ambiguity without
claiming a general coherent34-bit latch. DAC FIFO delay is not known to be zero.
Normal SetState RUN/PAUSE and fully published playback remain separate unfinished
work; no position success proves sound output or DMA quiescence.

The new CAS observation gate pins the IRQ at <=DISPATCH without waiting on a
mutex. Close/worker refuse disconnection while observations remain, retaining
references for retry. Independent Stream/CUR and Recovery reviews plus full Mac
ASan/UBSan suite passed. Official WDK/SDK ARM64 **78 production units** compiled
and linked; no unresolved project symbols. Image SHA256:
`8213ca7b88d39e895e68b6f4dc65274d16ea8172190bc707df910daff01c0664`.
Builder report `/tmp/t70-wdk-position/report.json`, host `/tmp/t70-position-suite.log`.
The three preserved dirty files remain part of the compile, not an exact clean
commit build. No firmware build, signing/install/load, device I/O, IRQ/DMA or
power execution. See AFE-POSITION-EVIDENCE.md for estimator limitations/sources.

## Normal WaveCyclic SetState transitions (2026-11-18)

The concrete stream now binds an explicit real state-control provider and
implements STOP→ACQUIRE→PAUSE→RUN and the reverse adjacent sequence. Identical
states are idempotent; unsupported jumps fail without register work. Unbound
streams still reject operational states, not fabricate preparation success.

STOP→ACQUIRE requires actual PrepareFromStop proof (stopped/start-of-buffer,
exclusive retained allocation), then reuses real address/format configuration.
ACQUIRE↔PAUSE does not start DMA. PAUSE→RUN checks the real readiness provider and
executes the already implemented synchronized memif/IRQ trigger. RUN→PAUSE issues
stock normal-render memif disable0x1b08 → observation0x1b00 → IRQ0 OFF0x3a0 → clear
0x3a8, then demands actual ConfirmStopped idle/drain proof, then freezes the CUR
estimate. A clear or disable write is NOT interpreted as that proof. PAUSE→RUN
resumes without reset/reconfiguration; STOP reports zero and the next ACQUIRE
must establish actual rewind/preparation again. No undocumented hardware reset
or power sequence was invented to meet that requirement.

PrepareFromStop, Ready and ConfirmStopped have NO default implementation. These
are still concrete missing platform facts when no real provider is supplied.
The existing buffer-allocation/hardware-reference ownership is retained across
STOP; Close needs the independent RemoveReferences proof before release. Close
from a healthy RUN first requests the actual pause/stop sequence. Any partial
transition failure quarantines state, preserves the buffer and makes further
SetState/GetPosition fail until the real recovery/Close provider removes refs.
Faulted cleanup does not repeatedly issue speculative register writes. Direct
RequestStartDl1 cannot bypass the state machine after binding.

AfeStopRequest is an independently encoded narrow normal period-wakeup stock
sequence, not copied Linux source or a no-period-wakeup/firmware/power path.
Unlike stock code that may continue after a failed memif disable, this helper
stops on first failure and retains references. That error policy does not claim
to quiet an unreadable source; real recovery remains responsible.

Tests include full cycles, direct PAUSE→RUN CUR preservation without rewind,
paused/ACQUIRE position freeze, STOP zero, all12 acquisition configuration and
9 bound start failure positions, all stop/readback positions, preparation/ready/
idle positive and negative errors, unknown alias, invalid transitions, and
retained buffer cleanup. Actual hardware providers are mocked, not demonstrated.
Notification cadence and real NewStream/service publication remain separate work.
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-setstate

Final independent ASan/UBSan review and full host suite passed. Official WDK/SDK
ARM64 **79 production units** compiled/linked with no unresolved project symbols.
Image SHA256 `2d8b828caa87be493a6f1984a133c28dbae10842e34d72a2c04092e07df4de0b`.
Builder `/tmp/t70-wdk-state-final/report.json`, Mac `/tmp/t70-state-reviewed-suite.log`.
The preserved three uncommitted files remain in this compile; no exact clean
commit build claim. No firmware build, install/load, device MMIO/DMA/IRQ, power
execution or audible playback verification occurred.

## WaveCyclic board stream factory and real IRQ cadence

`WaveCyclicFactory` is an actual `WaveCyclicMiniport` provider, initialized by
`WaveCyclicBoardOpen`. It allocates the PortCls master DMA channel, creates the
concrete stream, configures actual channel logical addresses through the board's
mapping and IRQ executor, binds position/state providers, and publishes three
independent COM references only on complete success. The service group is the
same `Board.Irq.Owner.Service.Group` used by the real ISR's Port.Notify path.
A second stream is rejected. S24 and malformed descriptors are rejected before
allocation; S16 mono/stereo uses the existing supported11 rates.

The factory uses two rounded10ms periods; `SetNotificationFreq` at STOP selects
a legal integral ring divisor closest to the requested milliseconds and updates
the prepared plan. Subsequent stock-derived IRQ0 start writes `PeriodFrames`,
not bytes. No synthetic timer is used. FrameSize conservatively includes both
requested-interval bytes and actual rounded-period bytes; actual returned
milliseconds round up. Unsupported requests return0/FrameSize0 without changing
the plan; period changes during active states are refused. The supplied OS
channel retains the real allocation size; no device memory memcpy is added.

On failure no outputs escape. If hardware references cannot be removed, the
unpublished stream remains reachable in the factory, blocking further creation.
A board pre-IRQ-close hook closes/stops the stream and drains exported channel/
group users before IRQ disconnection or mapping release. Failed proofs retain
ownership for explicit retry. Successful hook is cleared to prevent later
BoardClose calls dereferencing already retired factory storage.

This is not a fabricated platform provider: Access/Remove/Prepare/Ready/Stopped
and DrainExports must be real implementations. Parent must retain factory and
board storage through all exported references. Power/clock/route readiness,
DMA idle/rewind and exported PortCls work rundown are not inferred from register
writes. Existing DriverEntry is not switched to this provider without those
implementations, and no endpoint installation or playback is claimed.

Official contracts reviewed:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclic-newstream
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-setnotificationfreq
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/reference-counting-conventions-for-com-objects

Host tests run actual Miniport->Factory->Stream->Buffer code, replacing only
hardware/IRQ executor edges. They cover all12 configure I/O failures with failed
cleanup retention; missing board/service, S24, OS channel/allocation failures;
export drain failure and retry; duplicate admission; service-group identity;
STOP initial position/state, real IRQ-count programming, and all11 rates ×2
channel counts including fractional10ms rounding. Actual BoardClose hook failure
also proves no disconnect/unmap. Separate IRQ concurrency tests remain in the
full suite. These are mocks, not real interrupt or DMA execution.

Full host ASan/UBSan/-Werror suite passed (`/tmp/t70-factory-reviewed-suite.log`).
Official installed SDK/WDK ARM64 **80 production units** compiled and linked,
no unresolved project symbols (`/tmp/t70-wdk-cyclic-factory-final/report.json`).
Image SHA256 `73bd3521886cf5f1a56c7c1eaf28451d11788113e4842b479fe8381126f7626a`.
The preserved three uncommitted files were included in compilation; this is not
an exact clean commit artifact. No hardware mapping/IRQ/DMA, firmware build,
power execution, signing, installation or audible output verification occurred.

## Stock normal STOP and prefetch prepare integration

The factory's Remove callback now runs the actual serialized normal STOP helper
before external CPU/queued-user rundown. It does not demand an invented idle
flag and does not turn callback failure into safe release. Real exported-user
drain still precedes buffer free. Partial/failed stop retains all ownership.

The stream's ACQUIRE configure path now calls `AfePrefetch`: independently
encoded stock0x4c prefetch/minlen fields and integer10ms threshold, with every
unrelated bit preserved and first error propagated. Host tests cover threshold
10ms/10.916ms/11ms,256 original-bit patterns, all4 prefetch errors both directly
and through ACQUIRE, all6 close-stop errors, and normal PortCls adjacent cycles.

Review caught and removed a direct-STOP-only policy: it would break PortCls's
required RUN->PAUSE->ACQUIRE->STOP sequence. Real pause-preservation callbacks
remain mandatory. Microsoft MSVAD is a virtual reference for position semantics,
not proof T70 retains CUR when disabled. See AFE-DISABLE-EVIDENCE.md for exact
stock symbols and the unresolved physical pause distinction. No new rewind,
power, live device operation or fabricated successful playback was introduced.

Full Mac ASan/UBSan/-Werror suite passed (`/tmp/t70-prefetch-reviewed-final.log`).
Independent code review accepted the corrected PortCls-boundary version.
Official installed SDK/WDK ARM64 **81 production units** compiled and linked,
no unresolved project symbols (`/tmp/t70-wdk-prefetch-portcls-final/report.json`).
Image SHA256 `8f6a2897a5b94cc89dbfaea3f6b2b7420cf6b537f3b691c74a5d9d8b8af2fb56`.
Preserved three dirty files remain included in this compile; not a clean-commit
artifact. No device/MMIO/DMA/IRQ execution, firmware build, signing or load test.

## Profile-relative AW attenuation through actual SPB transport

Added AwSpbProfileVolume to combine selected ACF/board validation, unique0x05
extraction, chip ID verification and the existing volume RMW through actual
AwSpb WDF transport. This is a limited independently verified control operation,
not profile activation, amplifier initialization, calibration or playback.
Caller retains/serializes the real target and owns existing state/profile policy.
No power/reset/TX or unrelated register operation was added.

Independent ASan/UBSan review passed23,983 checks; final additional alias,
alignment and overflow regression is included in the full host suite log
`/tmp/t70-spb-profile-volume-reviewed.log`. Official installed SDK/WDK ARM64
**82 production units** compiled and linked with no unresolved project symbols
(`/tmp/t70-wdk-profile-volume/report.json`). Image SHA256
`e67b8c45b20efc88709a5a99a9b8ccf2e802b6449644c4f643fc51e236ff232c`.
The preserved three dirty files are included in compilation, not a clean-commit
artifact. No actual I2C/MMIO, power, DMA, driver load or firmware build occurred.

## Actual SPB serial-field configuration from validated profiles

AwSpbProfileSerial connects the existing pair serial decoder to real WDF SPB
read-modify-write operations for 06[11:0] and07[14:0], with ID validation,
opaque-bit preservation, stock ordering and explicit partial-write results.
No power, PLL, TX enable, calibration, AFE route or fabricated startup was added.
The actual owner must already exclude serial/playback/TX traffic and retain the
verified target. This does not infer wire format from the DL1 memory format.

Independent ASan/UBSan/-Werror review passed29,488 combined volume/serial checks.
Final NULL/tuple/IRQL/overflow no-I/O cases are in the full host suite log
`/tmp/t70-spb-serial-reviewed.log`. Official SDK/WDK ARM64 **83 production units**
compiled and linked with no unresolved project symbols
(`/tmp/t70-wdk-profile-serial/report.json`). Image SHA256
`d463ae790942aeade5dbce928617e63702ed605a26ec89ad37fbddaa360bb9fa`.
Preserved three dirty files are included in the build, not a clean-commit image.
No I2C/MMIO/DMA/IRQ device execution, power, firmware build, signing or load.

### Explicit AW translated-resource pair owner

`AwSpbResources` links real I2C connection resource IDs to target ownership and
existing serial-profile SPB application, without inventing the board supplier.
Full ASan/UBSan host regression passed (`/tmp/t70-spb-resources-reviewed.log`);
independent review reran 286 ownership-boundary checks and 29,528 real
volume/serial transport-generation checks. Both-channel preflight precedes all
leases and I/O. External parent/removal/traffic exclusion remains required.

Official ARM64 WDK component audit compiled 84 production units and linked
`MtkAudio.sys` with no unresolved project symbols; real headers, no mock include
paths. Report: `/tmp/t70-wdk-spb-resources-reviewed/report.json` on builder.
SHA256: `1e543fb78afea540079f7e264723fb45ecc7aa1b15d987c2dd21f53d539a84dd`.
This is a dirty-tree compile/link check, including the three preserved existing
AudioDriver/AudioDriverTest/shared-ntddk changes, not an exact clean commit
artifact. No INF verification/signing/installation, live resource binding,
I2C access or playback test was performed. No power or USB changes.

### WaveCyclic PortCls format intersection

The WaveCyclic miniport advertised eleven S16 sample-rate ranges but its actual
`DataRangeIntersection` always returned `STATUS_NOT_SUPPORTED`. That is not the
PortCls default-handler delegation code (`STATUS_NOT_IMPLEMENTED`). The actual
method now uses the existing strict PCM intersection implementation shared with
WaveRT in `PcmIntersection.h`, producing the 104-byte extensible format accepted
by the existing stream factory. No additional hardware format is advertised.

Contract: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiport-datarangeintersection

The actual Cyclic interface is tested for all eleven published rates and mono /
stereo ceilings, zero-length size query, each short output size 1..103, exact
wire fields, sample-size constraints, unsupported formats, output preservation,
alias rejection and closing state. This fixes OS format negotiation, not board
readiness, endpoint publication, I2C supplier absence or audible playback.

Full host regression passed (`/tmp/t70-cyclic-intersection.log`); Cyclic
miniport suite: 374,180 checks. Existing WaveRT checks remain enabled for the
shared implementation. Official WDK compiled 84 production units and linked
MtkAudio.sys, no unresolved project symbols, using real headers without mocks.
Builder report: `/tmp/t70-wdk-cyclic-intersection/report.json`.
Image SHA256: `4cc20471d62a5c6b38dc825854e55045e956b2970fa2ba7babb3b5617dc02e43`.
The three preserved dirty files remain included in this dirty-tree compilation;
this is not a clean-commit artifact, signed/installed driver or hardware test.
Independent review reran Cyclic 374,180 and WaveRT 374,102 checks plus 80,000
parallel reference pairs under ASan/UBSan/-Werror. The new Cyclic test against
the previous implementation fails (assertion exit 134); updated code passes.

### NewStream failure is not device teardown

Tracing the actual miniport → factory → stream flow exposed a recoverability
bug: stream-allocation failure called `T70WaveCyclicFactoryClose`, permanently
setting Closing and dropping the initialized port even after complete cleanup.
Subsequent NewStream calls therefore failed until device teardown/re-init.
The factory now separates unpublished-stream cleanup from whole-device Close.
Successful cleanup preserves Port/Initialized/pre-IRQ-close hook for a later
explicit request; failed cleanup still retains ownership and closes admission.
No hardware operation is automatically retried and the original creation error
is returned. Already published single-stream ownership is unchanged.

The host factory test now goes through actual GetDescription →
DataRangeIntersection → NewStream → configured stream state/position methods,
using all eleven rates and two channel counts. OS allocation and MMIO/IRQ
primitives remain mocks. Added regressions cover allocation failure followed by
successful explicit NewStream, and partial configure failure followed by
successful stock STOP cleanup and a later request. Existing cleanup-failure,
reference retention, state-transition and IRQ-period checks remain enabled.

Official contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclic-newstream
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/reference-counting-conventions-for-com-objects

Independent ASan/UBSan/-Werror review reproduced the failure against the previous
Factory (assertion exit134) and passed the corrected real-code mock flow.
Official WDK audit compiled 84 production units and linked MtkAudio.sys with no
unresolved project symbols; report `/tmp/t70-wdk-cyclic-retry/report.json`.
The three preserved dirty files remain included in the compile/link snapshot.
No actual PortCls load, firmware build, I2C/MMIO or playback was performed.
Full host regression passed: `/tmp/t70-cyclic-retry-final.log`.
Image SHA256: `c9de5ff7b703aa65e1a1c45f202ae8edb0bb2a91b8359018bca6c08afc2f82dc`.

### DriverEntry reaches the WaveCyclic software owners

`AudioDriver::StartDevice` now calls `T70AudioCyclicStart`, not the old temporary
WaveRT/NoStream path. The production runtime owns the Board, Factory, software
AFE domain and public COM export lifetime. The previously paused IRQ validation
hunks and their tests are retained/included as authorized, not discarded.

`AudioDomainOwner` verifies the actual PortCls device→PDO relation, retains both
OS objects, and uses a global interlocked claim to exclude a second instance of
this driver. This is **software** ownership, not proof that firmware/DSP masters
or clocks/power permit MMIO. A new Board `AccessReady` stage separates those
requirements before any map/read. Current physical access admission is absent
and returns NOT_READY there. No hardware preparation, endpoint publication or
successful StartDevice is claimed. Persistent successful-start/removal ownership
and real physical transition/service-drain providers must be implemented before
replacing that admission check; simply changing it to SUCCESS is incorrect.

`AudioExportOwner` supplies real stream/channel/service-group COM proxies. Each
retains its underlying interface and runtime parent. Final proxy Release drops
the exact live-proxy count; DrainExports closes admission and succeeds only when
all such references (including QI/AddRef copies) have ended. It does not guess
underlying reference counts. The factory's production Publish callback uses
these proxies; methods and service-group membership forward to the original
IRQ-notified group. This proves exported COM lifetime only, NOT DMA quiescence,
queued service notifications or service-member cleanup. Those remain separate
contracts. The actual factory mock flow exercises publication through proxies,
stream state/position forwarding and retained cleanup before final Release.

Verification: full ASan/UBSan/-Werror host regression passed in
`/tmp/t70-cyclic-domain-export.log`, including 1,000 competing software claims,
80,000 concurrent COM reference pairs, 200 drain/final-Release races, all proxy
methods, partial allocations and 75 DriverEntry/start checks. The real Board
test proves missing AccessReady releases the software claim without any mapping.
Official WDK compiled 87 production units and linked MtkAudio.sys with no
unresolved project symbols; no mock includes. Builder report:
`/tmp/t70-wdk-cyclic-domain-export/report.json`.
Image SHA256: `0949cc89e12183481536d757a946482ec9cbfbe89c8366b14700b6ce4ad2806f`.
No signed/loaded driver, endpoint, power/clock/USB change or live hardware test.

Contracts consulted:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/reference-counting-conventions-for-com-objects
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclic-newstream
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iunregistersubdevice
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/startup-sequence
Independent review reran the domain, export, DriverStart and actual factory/proxy
flows successfully. It explicitly noted that future hardware admission needs a
persistent FDO/remove owner holding a retry handle for failed cleanup; the current
sealed early-rejection path cannot reach that failure. Proxy lifetime tracking
does not prevent misuse of forwarded FreeBuffer/SetBufferSize calls or replace
their original PortCls contracts.

### Persistent failed-start runtime and real PortCls remove dispatch

The adapter now reserves `PORT_CLASS_DEVICE_EXTENSION_SIZE + slot size` in
PcAddAdapterDevice. Its initial Runtime reference is transferred to that real
FDO slot before fallible board initialization. Cleanup failure retains both the
reference and retry handle; a later Start or QUERY_REMOVE retries the same owner
without allocating a replacement. Successful cleanup clears the slot before
releasing its initial reference.

DriverEntry installs an IRP_MJ_PNP handler after PortCls initialization. It
processes only the current failed/unpublished lifetime, then passes remaining
work to PcDispatchIrp. A failed QUERY_REMOVE is completed synchronously using
IoStatus + IoCompleteRequest, not PcCompleteIrp (the latter is for previously
pending requests). CANCEL_REMOVE does not resurrect disposed failed-start state;
there was no active published device. SURPRISE/REMOVE closes Start admission.
No extension/device access occurs after forwarding REMOVE to PortCls.

Final removal is not vetoed. In this sealed pre-MMIO implementation, if generic
cleanup reports failure, RemoveUnpublished verifies no mapping, IRQ owner, port,
stream or live export exists and releases only the actual CPU-domain and COM/OS
references. It never forcibly frees active hardware resources. This is NOT an
active-DMA/successfully published-device surprise-removal implementation. Physical
AccessReady remains NOT_READY; it must not be enabled without the missing active
hardware/notification teardown policy.

Tests inject retained cleanup failure, repeated Start, QUERY_REMOVE veto and
retry, CANCEL_REMOVE, direct final REMOVE after failed START, persistent generic
cleanup failure resolved as verified software-only state, never-started surprise
removal, and Start rejection after removal. The forwarding mock clears the FDO's
extension pointer to detect use of that pointer after forwarding; it is not an
actual OS memory-free experiment.

Official references:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcaddadapterdevice
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcdispatchirp
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pccompleteirp
- https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-query-remove-device-request
- https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-remove-device
Full ASan/UBSan/-Werror host regression passed in
`/tmp/t70-cyclic-remove-reviewed.log`; DriverEntry/lifecycle test: 116 checks.
Official WDK compiled 87 production units and linked without unresolved project
symbols. Report: `/tmp/t70-wdk-cyclic-remove-reviewed/report.json` on builder.
Image SHA256: `be1f3752800f1970f19f3278c0921ea0064dd2baba20a27b18678eb16ba05bd7`.
No load, live PnP removal, firmware build or hardware I/O was performed.

Independent final review reran 116 checks. Nonzero success-class cleanup results
(including STATUS_PENDING) are normalized to DEVICE_NOT_READY, retaining the
Runtime handle rather than completing a synchronous query with pending status.

## WaveCyclic KS publication lifetime (2026-11-18)

The actual DriverEntry runtime now calls the explicit port publication operation
only after BoardOpen completes. The real physical-admission provider remains
`STATUS_DEVICE_NOT_READY`, so current production startup still performs **no
registration, MMIO or IRQ connection**. A successful future admission retains the
runtime slot instead of immediately closing it. This is not permission to enable
that provider before active/surprise removal and platform readiness are complete.

`WaveCyclicPortOwner` obtains the real `IUnregisterSubdevice` interface before
`PcRegisterSubdevice`, with a driver-static `T70Wave` name. Exact success owns the
registration; QI/registration failure releases only references actually acquired.
BoardClose rejects new streams (`Started=false`), withdraws discoverability, then
runs the existing stream/export, IRQ/worker/service, port, mapping and domain
rundown. Unregister failure retains the registered flag and all owned resources.
Unregister success does not establish that old streams or IRQ callbacks stopped:
failed later rundown keeps the port and unregister-interface references for retry.
Withdrawal is terminal; this owner cannot republish after close begins.

This is one KS wave subdevice, **not a complete Windows render endpoint**:
physical/topology connections and endpoint registration metadata remain absent.
The existing REMOVE dispatch is still limited to the sealed, unpublished
pre-MMIO production path. It must not be used to drop an active owner after an
unregister, stream or IRQ teardown failure, or to perform MMIO after surprise
removal. The unpublished fallback explicitly excludes registration/interface
state. QUERY_REMOVE cancellation for a live endpoint is not implemented.

Host tests exercise unavailable/null QI, register failure/pending, duplicate
publication, unregister failure/pending and retry, retained QI through later
rundown failure, and no stream/IRQ cleanup before a failed withdrawal. DriverEntry
checks continue to prove that its unavailable physical provider never publishes.
Mocks validate control flow and ownership, not a loaded PortCls endpoint.

Contracts: [PcRegisterSubdevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcregistersubdevice),
[IUnregisterSubdevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iunregistersubdevice),
[UnregisterSubdevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iunregistersubdevice-unregistersubdevice),
and [dynamic interface removal](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/managing-dynamic-topologies).

Validation for this change: full `test-audio-config.py` ASan/UBSan regression
passed (`/tmp/t70-publish-reviewed-host.log`); independent reviewer reran the
PortOwner suite and DriverEntry's 116 checks. The official SDK/WDK ARM64 audit
compiled 87 production units and linked the diagnostic driver without unresolved
project symbols (`/tmp/t70-wdk-publish-reviewed/report.json` on builder). No
INF verification, installation, device execution or endpoint functionality was
tested. Shared USB/DSDT and other working-tree changes were not modified.

## Concrete WaveCyclic topology and physical link (2026-11-18)

`T70TopologyMiniport` implements the actual IMiniportTopology COM interface with
an analog input pin 0 directly connected to speaker-category output pin 1. No
volume, mute, jack-presence, geometry, power or measured readiness is invented.
The WaveCyclic miniport now describes stream pin 0 -> logical DAC node -> analog
bridge pin 1. This describes conversion in the intended digital-to-speaker path,
not a new MMIO block or proof of T70 routing/clock/amp initialization. Stream
creation/PCM intersection still only admit streaming pin 0.

`AudioEndpoint` creates/initializes a real Topology port and acquires unregister
interfaces before publication. After real board admission it registers
`T70Topology`, registers `T70Wave`, then calls PcRegisterPhysicalConnection for
wave pin 1 -> topology pin 0. Startup failure keeps every acquired object in the
persistent Runtime; withdrawal removes the physical connection first, then wave
and topology registration. Errors retain remaining state and references for
retry. Runtime stops new stream admission before withdrawal, performs existing
board stream/IRQ rundown, and only then releases the topology owner. External
Port references keep the topology miniport and its Parent alive after local
Close; unregister is not a claim that those references disappeared.

DriverEntry reserves two PortCls subdevices. INF interface reference strings
match the static registration names and describe audio/render/topology plus the
speaker endpoint association. No pull/event-driven claim, APO, fabricated jack
state, or volume default was added. These are uninstalled package declarations,
not evidence that Windows has constructed an endpoint. Physical `AccessReady`
still fails before MMIO/registration. Active/surprise REMOVE, actual clock/power,
and working playback remain unresolved; this change does not bypass them.

Implementation independently follows the official interface/descriptor contracts
and facts, without copying sample implementation bodies:
[Microsoft SysVAD speaker topology](https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/EndpointsCommon/speakertoptable.h),
[Wave filter descriptors](https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/EndpointsCommon/speakerwavtable.h),
[Topology filters](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/topology-filters),
[physical registration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcregisterphysicalconnection),
[physical unregistration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nn-portcls-iunregisterphysicalconnection),
and [Microsoft INF interface reference conventions](https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/TabletAudioSample/ComponentizedAudioSample.inx).

Validation: full ASan/UBSan regression passed in
`/tmp/t70-topology-reviewed-host.log`, including topology 209 checks + 80,000 COM
reference pairs and endpoint 474 failure/lifetime checks. Two independent reviews
covered the endpoint integration and topology/lifetime behavior. Official SDK/WDK
ARM64 compilation passed for 89 production units; diagnostic driver linking
succeeded without unresolved project symbols (`/tmp/t70-wdk-topology/report.json`
on builder). Static INF reference-name/two-subdevice checks run in the host suite;
these do not replace INFVerif, installation, OS enumeration or playback testing.

## Surprise-removal IRQ isolation and cancel-safe queries (2026-11-18)

The existing normal STOP sequence remains the stock DL1 disable/readback/IRQ-off
and clear sequence; this change does not replace it with an invented idle bit.
It also does not claim that full active PnP STOP/REMOVE has been completed.

`T70AfeIrqRecoverySurpriseRemove` is a separate OS-surprise-notification path.
It atomically publishes removal and the ISR pre-I/O gate before waiting for
the recovery mutex; the mask callback rechecks removal under the interrupt lock.
KeSynchronizeExecution then joins previously admitted transactions and closes
observation/transaction admission. Already executing transactions may finish
before that synchronization completes; this is not instantaneous cancellation. It then
disconnects the real OS interrupt without calling source-mask/register callbacks,
flushes queued DPCs, waits for any queued worker, and requires the actual service
user drain before releasing references. A queued worker and later normal Close
honor the removal latch and cannot fall back to MMIO. Synchronization failure,
active observations, unknown IRQ connection or failed service drain retain the
owner for retry. SourceMasked and OriginalError are not fabricated. This API is
not an alternative to normal hardware shutdown and does not prove DMA idle.

Actual DriverEntry now uses the dedicated surprise path. QUERY_STOP and
QUERY_REMOVE veto an active runtime **without** unregistering it, closing stream
admission or destroying state, so a subsequent CANCEL leaves it unchanged. A
failed/unpublished CPU-only start can still be cleaned up. Unexpected mapped/DMA
state is excluded before normal Close in surprise/final-unpublished removal.

IMPORTANT REMAINING LIMIT: no-MMIO IRQ disconnect is not bus DMA quiescence.
The runtime's service/physical transition provider remains unavailable. Holding
buffers after failed rundown is containment, not a conforming final REMOVE:
REMOVE cannot be vetoed and a retained FDO pointer alone does not ensure lifetime
past PortCls deletion/unload. Therefore AccessReady remains sealed; no active
production endpoint can be started. Full active STOP/restart, final REMOVE and
surprise DMA/PortCls teardown still require real bus and service-user ownership
completion. This change must not be used as evidence to enable that path.

References: [KeSynchronizeExecution](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kesynchronizeexecution),
[ISR removal](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/removing-an-isr),
[QUERY_STOP](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-query-stop-device-request--windows-2000-and-later-),
[STOP](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-stop-device-request--windows-2000-and-later-),
[SURPRISE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-surprise-removal-request),
and [REMOVE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-remove-device-request).
The surprise documentation permits stopping hardware if still safely accessible;
T70 lacks that access-presence provider, so the separate no-MMIO path is the
conservative implementation here, not a claim that all drivers must avoid MMIO.

Independent review found and corrected a worker-first race: removal was formerly
published only after waiting for the recovery mutex. It is now published on
entry and rechecked at the source-mask interrupt-lock boundary. A host worker is
held at that boundary while owning the mutex; surprise removal overtakes it and
the resumed mask performs zero reads/writes. Existing admitted observations are
joined rather than falsely declared cancelled. The focused independent
ASan/UBSan test passed, including original-fault preservation and drain retries.

Final validation: full ASan/UBSan host regression passed
(`/tmp/t70-surprise-final-host.log`), including 154 actual DriverEntry/runtime
checks with fake board/OS boundaries. Official SDK/WDK ARM64 compilation passed
for 89 production units and diagnostic linking had no unresolved project
symbols (`/tmp/t70-wdk-surprise-reviewed/report.json`, builder). No device
removal, hardware access, installed-driver or active PnP completion test occurred.

## Normal Close uses stock STOP, not PAUSE position recovery (2026-11-18)

A concrete shutdown bug was fixed: `WaveCyclicStream::Close` previously entered
PAUSE from RUN before invoking the factory's real RemoveReferences path. PAUSE
requires preserving a resumable CUR position and the separate Stopped provider;
final destruction requires neither. Close now directly invokes that existing
factory path: actual stock DL1 disable/readback + IRQ-off/clear, then the caller's
CPU-user rundown. Only exact success clears HardwareTouched and ends the buffer
lease. The subsequent export drain still precedes buffer freeing. No undocumented
idle flag, arbitrary delay, DMA reset, clock or power operation was added.

The new Error48 regression runs the real factory and stream with fake registers:
Stopped returns NOT_READY, but destruction still performs the stock readback and
IRQ clear without CUR reads. Export-drain failure keeps the channel allocated.
An independent test using the old HEAD Close with this new regression fails at
the lease/Stopped assertion; the corrected implementation passes.

Actual Runtime Release now calls `T70AudioExportQuiesce` before endpoint and
board shutdown. Its atomically closed admission gate counts in-flight forwarded
stream/channel/group work. Failure leaves hardware and ownership untouched for
retry. Closed NTSTATUS methods return NOT_READY with outputs preserved; pointer
and scalar getters return null/zero; void data methods do nothing. Cleanup-only
RemoveMember and CancelDelayedService still reach the original group. All COM
references remain valid until their actual final Release. This gate does NOT
revoke existing raw pointers or stop callbacks to original sinks already owned
by the underlying PortCls group, and therefore is NOT full service rundown.

`AudioServiceSinkOwner` provides the next real registration boundary: it calls
actual Group.AddMember with a counted IServiceSink proxy, retains the original
sink/Parent, and calls actual RemoveMember once while separately counting active
RequestService callbacks. Late calls through retained proxy references are
no-ops. Close is nonblocking and returns NOT_READY until admitted calls finish;
recursive/concurrent Close cannot double-remove. Failed AddMember cleanup and
ambiguous informational status ownership are retained rather than guessed.

This owner covers one explicit registration, **not the entire service group**.
It is not yet substituted for the exported Group proxy's whole member collection:
identity/deduplication, all registration/removal paths and group-wide collection
rundown must be connected before Runtime.DrainService can succeed. That provider
still returns NOT_READY, and physical AccessReady remains unavailable. Full
normal PnP STOP/REMOVE and real playback are not completed by these changes.

Contracts: [PortCls service groups](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/service-sink-and-service-group-objects),
[AddMember](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iservicegroup-addmember),
[RemoveMember](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iservicegroup-removemember),
[COM ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/reference-counting-conventions-for-com-objects),
and [CancelDelayedService](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iservicegroup-canceldelayedservice).
RemoveMember releases the membership reference; neither it nor cancellation is
assumed to wait for every in-flight callback.

Validation: full ASan/UBSan regression passed
(`/tmp/t70-normal-stop-reviewed-host.log`). Independent reviews reran the actual
Stream/Factory test, demonstrated old Close failing Error48, and exercised
export gate and single-sink callback/Close races. Additional tests cover
informational/pending AddMember quarantine and failure while an early callback
continues; fixture-only destruction of a known fake is not presented as real
unknown-ownership recovery. Official WDK ARM64 compilation passed for 90
production units and diagnostic linking reported no unresolved project symbols
(`/tmp/t70-wdk-normal-stop/report.json`, builder). No installation, physical
STOP/REMOVE, audio output, USB, clock or power operation was performed.

## Counted service membership in the actual WaveCyclic export path

Every exported `IServiceGroup::AddMember` now registers a counted sink on the
original IRQ-notified group. `RemoveMember` records removal even during a
reentrant/in-flight AddMember. The append-only collection prevents traversal
use-after-free; nodes are reclaimed only after exported proxies are gone.
Successful callback rundown immediately releases original sink/group references,
not at final proxy destruction, avoiding a sink-to-export reference cycle.
Unknown positive registration statuses retain ownership rather than guessing.

`T70AudioExportDrainServices` closes work admission, removes tracked members,
and requires all admitted original callbacks to return. It is wired to the
factory's post-stock-STOP `Remove` contract and the board's post-IRQ/worker
`DrainService` contract. Final COM reference drainage remains a separate step.
Normal final REMOVE now attempts that actual normal release path. A recorded
surprise removal never takes normal MMIO STOP. These are not clock/power changes.

The official IServiceGroup AddMember/RemoveMember contracts provide membership
reference ownership, not callback completion; counting supplies the latter:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iservicegroup-addmember
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iservicegroup-removemember
The collection does not claim to drain PortCls's internal delayed work merely
because member callbacks have stopped. Underlying group COM ownership remains
with its actual PortCls implementation and the IRQ/service owner until release.

Remaining: AccessReady is still deliberately unavailable. Final REMOVE cannot
be vetoed; asynchronous ownership of a failed stock STOP or outstanding exports
past final dispatch/unload is not implemented. Such state is retained, not freed
or described as completed removal. Raw borrowed pointers require their documented
client lifetime. No driver load, device access, DMA or playback was tested.

Validation: full host ASan/UBSan regression exited 0; export tests include 80,000
concurrent reference pairs, 200 final-reference/drain races, multi-member and
reentrant registration/removal, live callback drainage, late callbacks, ambiguous
registration ownership, and the sink/export reference cycle. Actual DriverEntry
runtime with a fake board edge passed 179 assertions. Official WDK 10.0.28000.2526
headers compiled all 90 production translation units and linked the ARM64 kernel
image with `/opt:noref`, no unresolved project symbols (`t70-wdk-service-collection-reviewed`).
This does not substitute for PortCls on-device execution or driver signing.

## Hardware quiesce is not final PortCls reference destruction

A concrete ordering defect is corrected: the pre-IRQ factory hook previously
called final FactoryClose, so a remaining exported COM reference prevented IRQ
recovery/disconnect even after stock STOP and service callback rundown succeeded.
The hook now calls FactoryQuiesce, which closes the actual stream/hardware access
but preserves the stream, DMA channel/buffer, factory port and publication state.
After IRQ teardown the real Runtime Detach callback calls final FactoryClose,
then the export drain. A successful STOP is not repeated merely because final
software references have not yet gone away. STOP failure still retains the
hardware owner and blocks unsafe teardown. Failed unpublished NewStream retains
its previous complete-cleanup path.

This is a prerequisite correction, NOT a complete asynchronous final-REMOVE
implementation. In particular the current dispatcher may forward REMOVE while
a failed active runtime remains in its extension. Once PortCls deletes the
extension, that retained initial reference has no retry path. Admission must
remain sealed until this is fixed. Do not replace it with an unbounded pending
REMOVE, drop the DMA buffer, or queue a post-REMOVE retry that blindly re-enters
PortCls unregister/close routines.

Official review found these constraints:
- PnpStop provides a post-stream-STOP cleanup point and forbids waiting forever
  for open client objects. Its WaveRT/Topology applicability must not be silently
  assumed for this WaveCyclic adapter.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iadapterpnpmanagement-pnpstop
- Actual references to this driver's device objects prevent Unload invocation;
  a private Runtime COM count alone does not. The documented unload-pending
  restriction on acquiring new references is distinct from actual Unload.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/unload-routine-environment
- Queued IoWorkItem protects associated device/code lifetime, but allocation of
  an unqueued work item is not that guarantee. Post-REMOVE queue legality and
  the actual REMOVE completion fence still need a supported design.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/releasing-driver-allocated-resources
- Neither IRQ disconnection nor elapsed time proves failed/surprise DMA stopped.
  No hardware isolation provider was invented, and no power/clock work was done.

Validation of this limited correction: full host ASan/UBSan regression passed;
actual Runtime/Driver with fake board edge passed 185 checks. The factory tests
hold export references across successful quiesce, model IRQ removal, verify final
Close does not repeat MMIO, then release references and finish cleanup. All 90
production units compiled with official WDK headers and the ARM64 kernel image
linked without unresolved project symbols (`t70-wdk-hardware-ref-split`).
Independent source review found no new blocking defect in this split.

## Retained Runtime ownership and code lifetime after final REMOVE

The actual DriverEntry/REMOVE path now transfers retained Runtime ownership out
of the FDO extension into a heap token and a pre-created dedicated system thread.
`IoCreateSystemThread(Device, ...)` is called before board admission; creation
failure prevents BoardOpen. The I/O manager holds the associated device/code
reference until the start routine returns, so this does not queue new work or
acquire new device references after REMOVE/unload-pending. A successful kernel
thread handle is closed with ZwClose. The routine returns normally, rather than
calling PsTerminateSystemThread.
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iocreatesystemthread

After PcDispatchIrp returns, the dispatch wrapper touches only the retained heap
token. STATUS_PENDING is propagated unchanged and is NOT interpreted as IRP
completion. The thread does not access the IRP, device extension, registration
DDIs or device registers. Eligible retirement requires completed stock STOP and
stream admission closure, IRQ/worker/service rundown, and endpoint withdrawal.
Mapping leases/unmap and domain release are finished before forwarding REMOVE.
The remaining worker cleanup releases only validated software/COM/buffer state.

Late exported objects and retained Parent references wake the thread on release.
Refcount decrement/event signaling and the worker's final ownership test share
a spinlock, preventing event UAF and lost wakeups. The thread retains the Runtime
until its own reference is the only one left. This is the actual cleanup consumer,
not an unconnected scheduling helper. Normal failed START also signals the
pre-created thread to exit after its remaining references are released.

A failed STOP, unresolved surprise DMA, failed pre-forward resource teardown or
unexpected lifecycle state instead transfers to explicit quarantine. The thread
keeps the Runtime and OS code/device pin, performs no speculative hardware retry
and frees no uncertain DMA buffer. This prevents the previous lost retry owner,
but is NOT successful hardware removal: quarantine may permanently block unload
until shutdown. AccessReady remains unavailable. No complete active-device
removal, driver loading or playback result is claimed. Correct PortCls/WDM caller
lifetime and lifecycle serialization remain required; arbitrary raw COM calls
outside the owning OS/device lifetime are unsupported.

The two actual WaveCyclic/topology miniports now use nonblocking exclusive
admission for their mutable non-COM methods. Close permanently denies new calls
and returns NOT_READY until an admitted method/Close owner has left. Admitted
methods pin Parent and release that pin after clearing admission, waking the
retired Runtime. The worker retains RetireCpu on this transient NOT_READY and
waits for a real reference notification; it does not poll or self-signal a retry
loop. COM QI/AddRef/Release and retained immutable descriptors keep their separate
lifetime contract. This supplies the private Close/method rundown rather than
assuming that a pending PortCls dispatch return proves callbacks have finished.

RuntimeRelease now closes the wave miniport's actual method admission before
export quiesce, endpoint withdrawal or Factory STOP. This covers NewStream while
its provider is still creating a stream and has not published an export proxy.
An active provider call returns NOT_READY without starting resource teardown;
retirement also checks that gate and never classifies such state as CPU-only.

Validation: actual Runtime/Driver with fake OS/board boundaries passed 735
ASan/UBSan checks, including thread-creation failure before BoardOpen, 25 early
thread-execution cases, PENDING forwarding with an invalidated extension, late
DISPATCH-level COM release, notification-driven NOT_READY retry and isolated
quarantine. Actual wave miniport passed 377,828 checks; topology passed 222 plus
80,000 reference pairs. The retirement path was independently reviewed alongside
the miniport admission changes. Official WDK compilation/link covers all 90
production units (`t70-wdk-retired-runtime-complete`); host mocks do not validate
real PortCls scheduling, IRP completion, kernel loading or hardware behavior.

## Accepted PCM descriptors match the advertised WaveCyclic format

Microsoft requires NewStream to accept equivalent supported PCM represented by
WAVEFORMATEX or WAVEFORMATEXTENSIBLE. Its ordinary PCM example also sets the KS
SampleSize field to zero. The previous factory/SetFormat 104-byte-only checks and
parser's SampleSize==frame requirement rejected those valid representations of
the already-advertised S16 mono/stereo format before stream creation.
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/extensible-wave-format-descriptors
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/pcm-stream-data-format

The existing KS parser now accepts the official packed 82-byte KS/WAVEFORMATEX
PCM S16 descriptor as well as the existing 104-byte extensible descriptor.
SampleSize may be zero or the validated frame size. Plain PCM cbSize is ignored
as required by the WAVEFORMATEX contract, never used to read extra bytes:
https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatex

Both actual WaveCyclic factory creation and stream SetFormat pass the validated
82/104-byte size into that same parser. Plain PCM is converted on the stack into
the existing canonical metadata validation path. No new hardware, format rate,
channel count, clock or routing support is claimed. Raw 24-bit/32-bit/float PCM,
attributes, inconsistent byte rate/alignment, malformed GUIDs and unsupported
sizes remain rejected; failure preserves output. Official WDK static assertions
verify WAVEFORMATEX size 18, packed wrapper size 82 and format offset 64.

Host regression covers all 11 supported rates and mono/stereo through actual
miniport NewStream, factory configure, SetFormat and mock RUN/STOP, using both
representations and SampleSize zero/aligned forms. The parser also exhausts the
PCM cbSize values and malformed length/output-alias boundaries. These are host
and official-header checks, not Windows PortCls execution or audible playback.

Validation: parser 329,453 ASan/UBSan checks and the full host regression passed.
Independent reproduction using only the pre-change factory with the new parser
and integration test aborts at the first valid plain-PCM creation assertion;
the corrected factory passes. Official WDK compiled all 90 production units and
linked the ARM64 kernel image with no unresolved project symbols
(`t70-wdk-pcm-format-compat`). Independent review found no blocking defect.

## WaveCyclic byte-position normalization

The actual stream's NormalizePhysicalPosition previously rejected byte counts
not divisible by the PCM frame size. Microsoft's contract describes an actual
transferred byte count converted to 100-nanosecond units, without a frame-aligned
input prerequisite:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-normalizephysicalposition

The existing quotient/remainder conversion now accepts those byte counts too,
while retaining negative/null/closed-stream checks, signed-result overflow
rejection and unchanged output on errors. This is an API compatibility fix, not
evidence that Windows has issued a non-frame-aligned call on T70 or that the
unimplemented platform readiness prerequisites have been satisfied. No new
wrapper, hardware access, state transition or notification policy is introduced.

Validation: 1,298 byte/overflow oracle cases exercise the concrete stream across
all 11 supported rates and mono/stereo, including any-IRQL arithmetic and exact
signed-result overflow boundaries. Full ASan/UBSan host regression passed.
Independent review reproduced the old implementation rejecting the three-byte
case, while the corrected implementation passes. Official WDK compiled all 90
production units and linked the ARM64 image with no unresolved project symbols
(`t70-wdk-normalize-byte`). No live Windows/device playback was tested.

## Idempotent WaveCyclic notification requests

SetNotificationFreq has a PASSIVE_LEVEL contract, but no STOP-only prerequisite:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-setnotificationfreq

The actual stream previously returned a zero FrameSize for every request in
ACQUIRE, PAUSE or RUN, including an unchanged notification period. The existing
period selection is now reused to recognize an identical period and return its
normal byte budget without changing the prepared plan or accessing hardware.
STOP still selects a new period as before. A different period outside STOP
remains unsupported; returning the old interval alone would not resolve that
contract because current PortCls ignores the return value. This change does not
claim live period reprogramming, an observed Windows call sequence, or actual
playback readiness. FrameSize still covers both the requested byte interval and
the selected rounded period, so it is not reduced below either budget.

Validation covers all 11 rates and mono/stereo through the real mock-backed
Factory/Stream path, repeated requests in ACQUIRE/PAUSE/RUN and reverse
transitions, and a separate 5ms selection. Both accepted and rejected requests
preserve the complete stream representation, fake registers and I/O call count
outside STOP. The old stream implementation fails the new repeat-request
assertion. Full ASan/UBSan regression, independent review, official WDK 90-unit
compilation and ARM64 link passed (`t70-wdk-repeat-cadence`). These are host and
compiler checks, not a running Windows endpoint or audible playback test.

## Current WaveCyclic DMA size versus allocation ownership

Microsoft's DMA channel contract distinguishes allocated capacity from the
current size selected by SetBufferSize. The miniport must use BufferSize for
DMA transfer size when starting:
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/dma-channel-objects
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-idmachannel-setbuffersize

An actual mock-backed Miniport/Factory/exported-channel reproduction changed
3840 to 1920 bytes, but previously RUN succeeded with a 3840-byte DL1 END range.
The stream now revalidates current size, retained allocation, CPU/device address,
frame and hardware alignment before preparing or starting. Allocation ownership
is not shrunk or released by a metadata change. Preparation selects a legal ring
divisor no larger than the explicit notification byte budget, or fails if
no supported geometry exists. This never silently slows the prior IRQ cadence.

SetBufferSize itself remains an Any-IRQL metadata operation, not synchronous
hardware reconfiguration. A mismatch after preparation blocks a new start and
position publication. If detected in an active stream's next SetState call, the
existing stock STOP and stopped-provider path runs before leaving the stream
faulted; failures retain ownership. This does NOT implement atomic live resize,
immediate setter-triggered STOP, or a new concurrent producer protocol. The
existing external serialization/lifetime contract remains required. No buffer
is freed on a size mismatch, and no power/clock readiness is invented.

Validation: 15 actual proxy/factory/stream resize cases cover shrinking and
re-expanding within ownership, minimum geometry, invalid/oversized/misaligned
rings, changed device address, pre-RUN mismatch, active mismatch and STOP or
stopped-provider failures. HEAD's stream fails the corrected END assertion;
the new stream passes. Full ASan/UBSan regression and independent review passed.
Official WDK compiled 90 production units and linked the ARM64 image with no
unresolved project symbols (`t70-wdk-current-ring`). Device execution, atomic
live resize and complete platform playback readiness remain unverified.

## Preserve notification budget across ring resizing

The temporary IRQ divisor selected for a small current ring must not replace
the caller's notification budget. Previously, a 3840-byte allocation resized to
192 bytes selected a 96-byte period, then rejected a later valid 1936-byte ring
because it had no legal divisor at most 96. The same 1936-byte ring succeeded
when reached directly. The stream now retains the explicit notification byte
budget separately and adapts current geometry within that unchanged ceiling.

Similarly, the request byte-budget limit uses retained allocation capacity,
not half the current ring. A 10ms request with a 1920-byte current ring can
return a 1920-byte budget and use a faster 960-byte IRQ period. The current ring
still determines actual END/divisibility; requests do not allocate memory or
claim a live resize. Only a successful STOP notification request updates the
budget; idempotent requests outside STOP do not mutate the stream and never
report a smaller budget than the retained one. This prevents a short request
on a tiny ring from underreporting the period allowed after a later regrowth. The same
PortCls SetNotificationFreq and DMA-channel references above apply.

The actual proxy/factory regression now covers 3840→192→1936 with a legal
968-byte IRQ period, shrink-then-10ms requests, and a live 1ms idempotent request
that must not underreport the retained 1920-byte budget. Stream state and I/O
remain unchanged by that live request. Full ASan/UBSan regression and official
WDK 90-unit compilation/ARM64 link passed (`t70-wdk-notify-budget-final`).
Independent final review passed, including the conservative live-query budget
case. Substituting HEAD's stream into the new regression reproduces failure at
the small-ring 10ms budget assertion. No Windows runtime or audible playback
claim is made; atomic live resizing and platform readiness remain out of scope.

## PortCls CopyTo path audit

The public render path is PortCls -> exported IDmaChannel::CopyTo -> the original
PortCls DMA channel, not the private WaveCyclicBufferCopy helper. Microsoft
specifies Destination as a kernel virtual address inside the common buffer,
Source as the caller's source address, and ByteCount as a contiguous byte range;
CopyTo is Any IRQL:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-idmachannel-copyto

The existing wrapper correctly forwards all three arguments unchanged. Its
SystemAddress and PhysicalAddress calls use the same original channel retained
by the factory and configured into DL1. No extra offset, physical/virtual cast,
frame-alignment restriction, new lock or alternative copy implementation is
needed. The existing interlocked export work gate handles admission/rundown,
not synchronization of arbitrary overlapping copies or live metadata changes.
The private helper's PASSIVE restriction does not constrain this public path.
This audit found no new production defect in those contracts. Tests use fake
PortCls memory and do not establish actual DMA visibility/cache behavior.

The integration regression follows the real factory and exported channel into
its fake common buffer, checking offsets zero/nonzero/end, non-frame-aligned
byte counts, two caller-split wrap spans, source preservation and allocation-wide
sentinels at PASSIVE/DISPATCH/higher simulated IRQL. PAUSE/RUN and STOP resize
followed by a newly configured RUN use the same retained CPU allocation, with
its distinct device address in the mock DL1 registers. Independent ASan/UBSan
review passed. The split-wrap case is a caller simulation, not observation of
PortCls internals. Official WDK 90-unit compilation and ARM64 link passed
(`t70-wdk-copy-path`); production sources were intentionally unchanged.

Full ASan/UBSan host regression also passed. No endpoint or device was run.

## INF core-audio registration audit

The model's actual [Install.NT] omitted the inbox KS and WDM audio registration
sections required by the documented PortCls installation path. It now includes
ks.inf/wdmaudio.inf and needs KS.Registration/WDMAUDIO.Registration; system INF
files themselves are not modified:
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/installing-core-system-components-for-an-audio-adapter
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/installing-a-port-class-audio-adapter

The host test checks these directives inside the actual DDInstall section,
not merely anywhere in the file. Existing checks and independent review cover
T70Wave/T70Topology reference-name agreement with PcRegisterSubdevice; the
wave output 1 to topology input 0 physical connection; speaker output category
and endpoint association; service binary naming; and the KMDF NoDispatchOverride
miniport initialization. No extra PortCls service, fake endpoint property or
hardware-ready declaration is added.

Official InfVerif is present in the existing WDK cache as a Windows ARM64 PE
executable, but the Linux builder has neither Wine nor dotnet on PATH, and no
Windows execution host was used. InfVerif, catalog generation/signing, Windows
installation and device startup were NOT run. The static regression and ARM64
WDK link do not substitute for those checks. AccessReady remains unavailable.

A separate unresolved installation-compatibility issue remains: the model
permits build 26100 while the current verified binary and INF require KMDF
1.35. Microsoft's version history lists KMDF 1.33 for Windows 11 24H2:
https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/kmdf-version-history
An actual matched 1.33 header/library retarget was attempted using the existing
official cache, with a fresh version-matched overlay (no 1.35 header entries).
Clang-cl C++ failed inside official 1.33 headers: wdfdevice.h forward-declares
_WDF_REQUEST_TYPE with fixed underlying int, while wdfrequest.h defines it
without a fixed underlying type. No vendor header was patched and no diagnostic
was suppressed. The unvalidated retarget was not retained; the verified 1.35
build remains unchanged. Windows 24H2 installation compatibility is therefore
NOT established. Resolve with a supported compiler/SDK combination and matching
framework target before claiming this package can load on that OS.

Full host regression and official 90-unit KMDF1.35 ARM64 compilation/link passed
for the retained core-registration change (`t70-wdk-inf-registration`). The
static regression reproduces the old INF's missing Include directive. Neither
these checks nor the repaired Include/Needs resolve the framework-version issue.

## KMDF target 1.35 with minimum runtime 1.33

The previous exact-1.33 C++ header failure does not require patching the vendor
headers or lowering only the INF number. Microsoft's supported version-range
build keeps a consistent newer target and declares an older minimum runtime:
https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/building-a-wdf-driver-for-multiple-versions-of-windows

The checker retains official 1.35 headers, stubs and libraries, and now passes
KMDF_MINIMUM_VERSION_REQUIRED=33 alongside target major1/minor35. This matches
the installed official WindowsDriver.Common.targets preprocessor setting.
The official wdffuncenum.h emits the WdfMinimumVersionRequired binding value;
no hand-written replacement symbol is introduced. INF KmdfLibraryVersion stays
1.35 because the official StampInf target version defaults to major/minor, not
the minimum required value. The minimum OS remains build26100.

Downlevel support requires that unavailable API/structure/field use be guarded.
The current direct WDF APIs and structures are compared with the official1.33
headers, not assumed compatible merely because linking succeeds. This addresses
the earlier missing minimum-runtime declaration; it does not prove actual1.33
runtime loading, InfVerif acceptance, installation or hardware readiness.

The reproducible checker now verifies both layers: current WDF call/structure
usage against official1.33/1.35 definitions, and each compiled WDF COFF's actual
WdfMinimumVersionRequired initializer plus the selected value in the linked PE
(using the link map and PE section layout). It fails if metadata does not equal
33. An independent check found eight WDF COFF definitions and linked value33;
applying the same expected33 check to the previous image's objects fails at35.
All official files remain unmodified, no compiler diagnostics are suppressed,
and the exact1.33 C++ enum issue is not bypassed or misreported as resolved.

Final integrated validation passed: eight used APIs at matching always-available
indices, four identical structure definitions, six identical initialization/time
helpers, 90 production translation units, and the ARM64 link. The final checker
reports the actual compiled and linked minimum value33. Full ASan/UBSan host
regression passed. These results support the official downlevel build contract,
not a claim that the driver was installed or loaded on Windows 24H2.
Independent review and negative canary checks passed. The compatibility audit is
bounded to direct calls and the listed current structures/helpers; it is not a
general C/C++ analyzer for arbitrary new function-pointer or structure usage.
Review and extend that inventory when introducing new WDF features. The minimum
binary audit explicitly rejects Python optimization that would remove asserts.
