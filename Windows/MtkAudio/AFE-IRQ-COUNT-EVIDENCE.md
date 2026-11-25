# DL1 period-frame IRQ count, without IRQ programming

The module hashes are the ones recorded in `AFE-GEOMETRY-EVIDENCE.md`.
The common module's `mtk_afe_fe_trigger` at .text 0x1d38 loads runtime +0x78
into w24 at 0x1d70. At 0x1dd4/0x1ddc it loads IRQ descriptor count shift/mask,
shifts the unchanged w24 at 0x1de4 and passes it to the register helper at 0x1df8.
The [upstream common driver](https://github.com/torvalds/linux/blob/master/sound/soc/mediatek/common/mtk-afe-fe-dai.c)
identifies this counter input as runtime period_size, measured in PCM frames.
There is no decrement or channel multiplier in this count path. This review
examines data flow only; no trigger or MMIO operation was executed or added.

AFE `irq_data` at .rodata 0x928, length 0x6c0, contains 27 records of 0x40
bytes with IDs 0..26. All records have count shift 0 at +8 and count mask
0x3ffff at +0xc: an 18-bit unshifted field. Count register offsets at +4 vary
by IRQ. The runtime uses the memif's assigned IRQ descriptor. Correction below: T70's
probe initializes a fixed table with DL1 assigned IRQ0 and const_irq=1; the
startup dynamic-allocation fallback does not make this board's DL1 dynamic.
The descriptor alone still does not establish a Windows driver's ownership.
No register addresses or IRQ allocation decisions are emitted by the new API.

`T70AfeIrqCountField` accepts positive frame counts through 0x3ffff and returns
the count unchanged. Zero is rejected as an invalid period policy, not assigned
a guessed hardware meaning. `T70AfeDl1IrqCount` first revalidates every field of
the prepared snapshot, then applies that bound to its PeriodFrames. Under the
currently accepted geometry, the largest period is 98304 bytes and the smallest
frame is 2 bytes, so 49152 frames is the reachable maximum. The field helper's
larger boundary is separately tested; that does not broaden prepared PCM limits.

The shared prepared validator compares named fields, not padding, and is also
used by CUR conversion. Failed validation leaves output untouched. Host tests
cover the full 18-bit range plus adjacent failures, UINT64_MAX, valid period
geometries, tampered snapshots and unsupported formats. No IRQ registration,
acknowledgement, routing, enable, DMA, power or actual playback is implemented.

## Explicit descriptor count/rate callback subset

`AfeIrqFields` accepts only exact descriptor tuples from stock `irq_data` at
.rodata 0x928 (27 records, stride 0x40). It does not choose or acquire an IRQ.
Descriptor +4/+8/+0xc are count offset/shift/mask (shift 0, mask 0x3ffff).
+0x10/+0x14/+0x18 are frequency offset/shift/mask: register groups
0x2e4,0x2e8,0x2f0,0x2f4, four-bit lanes `(Id % 8)*4`, unshifted mask 0xf.
The exact 27 count offsets are whitelisted in code; caller-supplied alternatives
are rejected before I/O. Matching a tuple proves table identity, not ownership.

The common trigger uses memif's assigned IRQ index at 0x1d88..0x1d94, then
updates count at 0x1df8 and frequency at 0x1e58. It calls AFE+0xd0 for the rate
encoding at 0x1e20. AFE initialization relocations 0xa1d0/0xa1d4 reference
`mt6789_irq_fs` (0xa680), stored to +0xd0 by 0xa1e8. That function calls
`mt6789_general_rate_transform` at 0xa6b4, matching the supported rate subset
already encoded by `AfeFormat`. Unsupported formats/rates remain rejected.

Important boundary: stock calls `mtk_memif_set_enable` at 0x1dc4 BEFORE these
updates and enables IRQ afterward at 0x1e88. This implementation is only the
count-then-frequency field subset, NOT a reproduction of the complete trigger
or proof of a safe pre-start hardware sequence. It performs no enable, stop,
acknowledgement, clock or power operations. The caller must supply a verified
quiescent context and actual IRQ ownership, and serialize the shared frequency
register across all IRQ owners. No concrete provider is supplied here.

Each field uses a fresh read/masked-write. Failures stop immediately with exact
status/phase/progress; failed writes may have applied, earlier writes are not
undone. This stricter error policy differs from stock's unchecked field updates.
Mocks cover all 27 descriptors, 11 supported rates, preserved-bit patterns,
every read/write failure phase and descriptor/snapshot corruption. Tests do not
validate live IRQ or DMA behavior.

## Dispatcher observations and why no selective-ack API is claimed

Read-only stock `mt6789_afe_irq_handler` (.text 0xa430, size 0x1f0) establishes
this actual order, without executing any interrupt or register operation:

1. Read register 0x3b4 at 0xa474, then 0x3a4 at 0xa484.
2. Compute `(second_value & first_value) & 0x87ffffff` at 0xa494..0xa498.
3. For memifs with a non-NULL substream and nonnegative assigned IRQ index, use descriptor +0x20
   as the bit shift (0xa518..0xa534). Call `snd_pcm_period_elapsed` for each
   selected substream at 0xa538.
4. After notifications, write the computed mask directly to 0x3a8 at 0xa59c.
   This is not a read/modify/write of the clear register.

The 27 stock IRQ descriptors corroborate register/bit selection: +0x1c is
0x3a0, +0x20 is Id, +0x24 is 0x3a8, +0x28 is Id, for IDs 0..26. This shows
the same per-IRQ bit identity in the dispatcher and the clear descriptor. It
does not explain bit 31, which the dispatcher mask also includes. Bits 27..30
are excluded by that mask. No meaning or ownership is assigned to bit 31 here.

Error behavior is not safe to copy blindly: the first read's return code is
ignored, the second read failure/no selected status goes through a log path
and still reaches the direct clear write, and the clear-write return code is
ignored before returning handled (1). Both read destinations initially contain
zero, but a failed callback's output cannot generically be treated as valid.
Notifications have already occurred before a subsequent clear-write failure;
a retry could therefore deliver another notification without a lifetime or
sequence policy. The new portable code does not reproduce these behaviors.

The observed direct-mask write is evidence of stock intent to acknowledge that
mask. It is NOT a verified register specification for W1C/read side effects,
concurrent arrivals, or preservation of other clients' pending bits. A selective
ack helper would need that hardware contract and the real shared dispatcher/
stream-ownership synchronization; declaring them proved by a mock that clears
only selected bits would be circular. An ack-only helper also cannot silently
move acknowledgment before the observed period notification.

No status/ack helper is added in this review. Missing evidence is the exact
0x3b4/0x3a4/0x3a8 hardware semantics, bit-31 ownership, and live stream lifetime/
notification-failure contract. No power, DMA start, IRQ enable, actual MMIO,
new test run or full build occurred. Existing count/rate callbacks are unchanged.

## Public related-SoC cross-check (2026-11-18)

Primary source inspected:
https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/soc/mediatek/mt8186/mt8186-afe-pcm.c

MT8186's IRQ handler reads MCU enable then status, intersects them with its
status mask, notifies selected substreams, then writes the mask to the clear
register. Its stop path also writes one selected clear bit. This supports the
interpretation of a write-one clear interface on that related SoC, but does not
prove MT6789's shared-pending preservation or concurrent-arrival semantics.
Unlike the stock MT6789 binary inspected above, this public handler checks the
first enable-read result and returns on failure. The implementations cannot be
treated as identical just because the overall structure matches.

The related binding
https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/sound/mt8186-afe-pcm.yaml
specifies `mediatek,mt8186-sound` and one external interrupt. It is a device
resource description, not an MT6789 register-clear or per-bit ownership contract.
It does not establish bit 31's source or who may acknowledge it on T70.

Searches for MT6789-specific IRQ31/W1C documentation did not yield a primary
hardware specification establishing those properties. This is a search result
limitation, not a claim that no such specification exists. The stock binary still
only demonstrates the previously recorded masked direct write, with bit 31
included but no corresponding entry among the 27 inspected IRQ descriptors.

Outcome remains no ACK write helper. Related-SoC source strengthens the likely
interpretation but is insufficient for the required guarantee that clearing an
owned bit preserves every other pending source on this chip. No tests based on
an assumed W1C model, power operations, hardware accesses or new builds were run.

## Same-SoC original source and WaveCyclic notification (2026-11-18)

The pinned MiCode MT6789 source now provides a stronger comparison than the
related MT8186 source above:
https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/d26d588883468251678619757cc9f0f6a5f37cb8/sound/soc/mediatek/mt6789/mt6789-afe-pcm.c
(handler at lines 2709–2768), with mask definition in mt6789-reg.h line 2987.
It matches stock's enable/status intersection, assigned live-substream period
notification, and later direct mask clear. It also has the same unchecked first
read/clear error limitations. The stock binary was re-disassembled read-only:
SHA256 remains 67f1ec09fc9d883d2d4933b8f4b062bf8c473f5bd4968761054914f703706ba8;
call sites a474/a484/a538/a59c and mask a498 agree. GPL source was examined for
hardware facts; no Linux implementation was copied into the Windows code.
This does not establish bit31 ownership or all concurrent-clear semantics.
No ACK/status MMIO implementation was added or executed.

Windows' Notify contract normally follows handling the interrupt source:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavecyclic-notify
The Linux notification-before-clear order cannot simply be transplanted and
called a complete Windows shared ISR. The implemented `WaveCyclicService` is
only the OS-facing PcNewServiceGroup/Notify ownership portion. The caller must
already have handled an actual owned DMA-progress interrupt; the helper does
not infer one from a timer or a caller-supplied fabricated status mask.
Its interlocked close gate protects active Notify calls, not the device IRQ
or downstream service work. The actual owner must disconnect/drain those and
retain context storage before Close can release references. No IRQ connection,
ACK, period count, service completion, playback position or RUN is claimed.


## Correction: stock DL1 is statically assigned IRQ0 (2026-11-18)

Earlier reasoning inspected startup's negative-irq fallback but missed the
probe initializer. The stock AFE module (hash above) has `memif_irq_usage` in
.rodata at 0x8d8, size 0x50; its first u32 is zero. `memif_data` at 0x1148 starts
with memif ID 0 and DL1 BASE/CUR/END offsets 0x54/0x5c/0x64. In probe, a008
loads the assignment table, a018 stores irq_usage at memif+0x18, and a028 stores
constant 1 into const_irq at +0x1c. Startup b19c/b1a0 reaches dynamic acquisition
only when the stored assignment is negative. Those instructions and table bytes
were independently extracted and reviewed, not inferred from function names.
The pinned public source independently assigns DL1 to IRQ0 and initializes
const_irq=1. Thus IRQ0/status bit0, not an arbitrary allocator-selected bit,
is the stock DL1 assignment. This corrects the previous unknown-fixed-IRQ claim.
It does not confer Windows ownership or show that all other AFE clients stopped.

The complete pinned `mt6789-reg.h` also names IRQ31 ON/CLR/MCU_EN fields and its
counter monitor (0x6a0). A web-text extraction omitted those definitions; the
full raw file was separately read. Bit31 is a named hardware IRQ, not proved to
be a DL1 bit or a reserved bit. The memif assignment table has no IRQ31 entry,
and the inspected 27 normal descriptors cover only 0..26. Its functional owner
and safe shared handling remain unproved. IRQ0 clear field is bit0 at 0x3a8;
stock STOP writes its assigned single clear bit, while the shared handler writes
the whole selected mask. No exact-chip specification was found for zero-bit
preservation/concurrent arrival behavior. Related MT7981 documentation describes
write-one clearing but is not MT6789 evidence and is not substituted here.

No ISR/ACK write or fake handled result was added. Register ownership/clear
semantics and actual OS interrupt connection/rundown still block safe integration.
This correction changes documentation only; no production source changed, so
no new WDK image or host execution is claimed for this evidence-only commit.

## Source-derived exclusive-domain ISR implementation (2026-11-18)

After the fixed IRQ0 finding, the implementation scope was explicitly changed
to port the matching stock algorithm while documenting unresolved hardware
assumptions rather than requiring a separate register manual. `AfeDl1Interrupt`
is a Windows ISR-signature function with explicit synchronous DIRQL-safe I/O
providers. It reads MCU enable/status, intersects with 0x87ffffff, notifies the
DL1 service for bit0, then writes the entire selected mask to 0x3a8 in stock
order. This does NOT claim a proven selective W1C optimization.

Its crucial precondition is exclusive ownership of the WHOLE AFE MCU interrupt
domain: no other active memif, DSP/firmware client or driver's notification may
be lost by clearing its bit. Bit31 is handled exactly as stock's selected mask,
without inventing its functional owner. A boolean documents this real-adapter
precondition; setting it does not establish ownership. This ISR must not be
connected until that handoff is independently established. Other selected bits
are clear-only just as the stock handler treats sources without live substreams.

Deliberate error-policy differences: both reads must succeed; zero selected
status returns unhandled without a write. A failed notification or clear latches
Faulted; subsequent calls do not repeat notification. A clear failure can happen
AFTER notification and may leave the physical IRQ asserted. Actual integration
must disconnect/recover that source; the latch is not IRQ masking, hardware
quiescence, or permission to free resources. No retry/rollback is claimed.

The tests check all 32 status positions, enable gating, mixed 0/1/31 mask,
notification-before-clear order, read/write failure quarantine, and missing
ownership/closed service. They are algorithm mocks, NOT validation of physical
concurrent-clear semantics. No IoConnectInterruptEx, enable, real MMIO, power
operation, automatic timer, or playback execution was performed.

ISR return-value correction from independent review: after Selected is nonzero,
notification/clear errors return TRUE (this device was recognized), while still
latching the failure. FALSE is not used as a generic error code. Read failures
before identification remain unhandled. A subsequent quarantined entry makes
no new status claim and returns FALSE; the integrating owner MUST arrange actual
fault-source disconnection/recovery rather than leave this ISR installed in a
level-triggered interrupt storm. This remains a blocker to OS IRQ connection.
The service API contract now separates proven progress from device-specific ack
ordering, allowing the source-derived notify-before-clear sequence explicitly.

## IRQ0 source disable and fault masking (2026-11-18)

Stock common module (SHA256 above) `mtk_irq_set_enable` at 0x25f8 and
`mtk_irq_set_disable` at 0x26d0 load descriptor register/shift at +0x1c/+0x20
and update that single bit. IRQ0's descriptor selects 0x3a0 bit0. In stock
`mt6789_fe_trigger` STOP, 0x2d4 calls disable and 0x2e8 writes the descriptor's
single clear bit (0x3a8, value1). START enables it only after memif enable and
IRQ count/rate setup. The common helpers also have a firmware semaphore branch;
this port does NOT replace that arbitration. Whole-domain handoff/no firmware
or DSP users remains mandatory. These are independently encoded facts from the
pinned GPL source and matching stock binary, not copied GPL implementation.

`AfeIrqSource` implements only disable/clear, never enable or restart. It preserves
all other ON fields. It adds bounded readback/error checks: all known ON fields
must be zero before clear, and no enabled/selected MCU status may remain after
clear. A different active source is NOT forcibly disabled. Every error retains
the OS IRQ/owner through existing Close semantics. These observations do not
prove DMA idle, stop all device references, or permit freeing a PCM buffer.
Register I/O may partially take effect even when its callback reports failure.

The PASSIVE Close mask callback invokes official `KeSynchronizeExecution` on the
still-connected interrupt object. Its critical section runs at SynchronizeIrql
under the ISR lock. Lifetime Connect/Close must remain externally serialized.
The fault callback, already inside that same ISR lock (possibly before Connect
returns), masks directly without recursive synchronization and invokes a supplied
preallocated DIRQL-safe recovery scheduler even if masking failed. That real
scheduler/worker/drain is still a required parent provider, not fabricated here.
No worker, DriverEntry enable, automatic restart, power or DMA control was added.
If masking fails, a physical IRQ storm can still occur; this is NOT claimed as
complete hardware recovery. Retaining references prevents premature teardown,
not a guarantee of liveness on failed MMIO.

Official contract used:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kesynchronizeexecution
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-ksynchronize_routine

Host checks inject all six read/write failure positions; other IRQ0/31 ON bits,
readback mismatch, residual shared status, reserved-bit preservation, ISR fault
before Connect returns, and ISR-vs-Close locking. They model register semantics,
not physical concurrent-clear behavior or real interrupt delivery.

Implementation detail: the callback adapter issues the preserved ON value even
when bit0 was already zero; Linux's non-forced regmap update can skip that write.
This is the same register-value operation, not a claim of identical I/O counts.
The source provider is also tested through actual interrupt-owner Close with a
failed Drain and retry: disconnect occurs once and parent/group survive failure.

## Actual Windows deferred recovery owner (2026-11-18)

`AfeIrqRecovery` now supplies the previously missing scheduler. Init preallocates
an IoAllocateWorkItem and initializes a KDPC, kernel mutex and completion event.
The actual driver Device and Parent are explicitly provided, not enumerated or
fabricated. Parent retains Device/module/context even before work is queued.
ISR fault masking calls the scheduler once, preserving the first NTSTATUS. It
queues a DPC at DIRQL; that DPC queues IoQueueWorkItem at DISPATCH_LEVEL. Calling
IoQueueWorkItem directly at device DIRQL would violate its official contract.

The PASSIVE worker only performs the existing source-mask/disconnect operation
under a kernel mutex. It never releases service/parent references, clears the
fault latch, enables a source, restarts playback, or claims that DMA stopped.
The mutex does not raise IRQL, unlike a fast mutex; both existing connect/mask
contracts therefore remain PASSIVE. Connect and worker are mutually excluded.
A Publish gate handles an ISR/DPC occurring before IoConnectInterruptEx returns;
the end of Connect requeues the DPC if needed. A separate CAS queues work once.

Close masks/disconnects under the mutex, releases the mutex, flushes previously
queued DPCs, and waits for the worker's completion event before actual external
PortCls/service-user drain and reference release. It cannot wait under the mutex
or from its own worker. Successful disconnect plus exclusive wrapper lifetime
means no ISR producer can enqueue after the flush starts. Unknown connection or
mask failure retains everything. A failed service drain is explicitly retryable;
work-item storage is freed once only after successful completion/drain. The
worker's final owner access is completion signaling. Kernel work-item device
lifetime covers the remainder of the callback; this is not a substitute for the
parent's prequeue lifetime obligation. KeFlushQueuedDpcs is a potentially slow
PASSIVE teardown operation, never an ISR/audio-period fast path.

RecoveryStatus reports only mask/disconnect outcome. Even STATUS_SUCCESS does
not erase OriginalError or certify device health. A stuck/unreadable source can
still assert its line; failed recovery preserves references and does not invent
a reset, power operation, or successful restart. DriverEntry still does not
activate playback or attach this without real exclusive ownership/resources.

Official contracts checked:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keinsertqueuedpc
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioqueueworkitem
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iofreeworkitem
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keflushqueueddpcs

Host cases run the actual ISR/source/owner/recovery code against fake OS calls:
allocation failure, first-fault preservation, duplicate scheduling, pre-Publish
ISR/DPC, Connect failure after ISR, worker-vs-Close mutex competition, stuck ON
worker/Close failure with explicit retry, service-drain failure and exactly-once
release. The bounded DPC scheduling model is NOT real kernel concurrency proof.
