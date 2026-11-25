# DL1 CUR arithmetic and deliberate rejection policy

Module hashes are recorded in `AFE-GEOMETRY-EVIDENCE.md`. In DL1's stock
`memif_data` record (.rodata 0x1148), descriptor offsets 0x10/0x14/0x18 give
BASE/CUR/END low register offsets 0x54/0x5c/0x64. Descriptor offsets
0x1c/0x20/0x24 give corresponding high register offsets 0x50/0x58/0x60.
These are evidence only, not a newly implemented MMIO reader or write plan.

The common module's `mtk_afe_pcm_pointer` (.text 0x1dc, size 0x180) reads CUR
low via descriptor +0x14 at 0x244/0x248, CUR high via +0x20 at 0x25c/0x26c,
BASE low via +0x10 at 0x274/0x280 and BASE high via +0x1c at 0x294/0x2a4.
At 0x2c8/0x2cc it combines the halves; 0x2d0 subtracts BASE from CUR.
0x2d4 shifts the difference by three, 0x2d8 masks with 0x7ffffffff80,
and 0x2f8 divides by runtime frame bits. That mask discards low byte-offset
bits (16-byte granularity) as well as upper bits. It is not proof that an
unaligned hardware CUR is impossible or that DMA BASE must be aligned to 16.
A non-16-byte CUR offset may be a legitimate intermediate observation; this
review has no hardware guarantee excluding that case. Consequently the strict
API may reject legitimate progress and is NOT established as suitable for a live
playback pointer callback. Integration requires resolving that compatibility
question rather than treating rejection as a hardware fault.
The inclusive END arithmetic was separately established in the geometry review.

`T70AfeDl1Position` accepts a coherent full CUR supplied by the caller and
revalidates the prepared format/geometry, including every derived field. It
checks the 34-bit range and inclusive buffer bounds before subtracting, then
requires an exact 16-byte and whole-frame offset. Unlike the stock rounding and
error-to-zero path, invalid input is explicitly rejected with output untouched.
It neither masks away address overflow nor normalizes an out-of-range CUR modulo
the buffer. This stricter behavior is an API policy, not a claim of stock error
behavior equivalence. The output is a byte offset and PCM frame offset only.

CUR equal to BASE is valid offset zero. A single sample cannot identify a
circular lap, a reset, or the number of bytes consumed since another observation.
An apparent end+1 pointer is rejected rather than interpreted as a wrap. There
is no temporal state or monotonic progress claim. The caller must establish
coherent high/low sampling and association with the same DMA allocation; neither
is proved by arithmetic. Address zero can pass the pure math despite stock's
low-word-zero error behavior. No real register access, DMA, power or clock action
occurred. Host tests cover every offset through the largest accepted buffer,
32-bit crossings, the 34-bit boundary, misalignment, underflow and tampered views.

## Separate normalized API

`T70AfeDl1NormalizedPosition` retains all snapshot and address-bound checks, but
rounds the validated byte offset down to 16 before frame conversion. Within the
accepted buffer extent, this equals the stock expression
`((CUR - BASE) << 3) & 0x7ffffffff80`, divided by frame bits. The bounded extent
makes the upper-bit mask irrelevant; invalid addresses are still rejected before
subtraction instead of being silently masked. It rounds the offset, not the
absolute address, which matters for bases not divisible by 16.

The original `T70AfeDl1Position` remains strict and unchanged in behavior. Both
APIs share validation and publish only after success. Supported frame sizes
2/4/8 divide 16, so normalized offsets are whole frames; this is also explicitly
checked. Tests compare every in-range offset with the literal stock bit
expression alongside the original strict expectations, including nonaligned
bases and 32-bit crossings. No live pointer sampling, error-to-zero behavior,
lap tracking, or playback-driver integration is implemented by either API.

## Live split-register sampling remains unproved

Rechecked the stock `mtk_afe_pcm_pointer` and the public Linux common driver:
https://github.com/torvalds/linux/blob/master/sound/soc/mediatek/common/mtk-afe-platform-driver.c
(accessed 2026-11-18). Both read CUR low before CUR high, then BASE low before
BASE high. Neither inspected function contains a retry, high/low/high stability
check or an explicit snapshot-latch operation. The public implementation is
supporting evidence for call order, not an MT6789 hardware latch specification.

The stock instruction sequence is fixed (0x248, 0x26c, 0x280, 0x2a4), but that
alone does not establish coherent halves while DMA advances. For example,
without a hardware latch, low=0xfffffff0 sampled before crossing 4 GiB and
high=1 sampled afterward combine into 0x1fffffff0 instead of either real sample.
This is a hypothetical split-read counterexample, not a measured chip defect.
The prepared buffer-range check can reject that example; it cannot establish general sampling
coherence or hardware latch semantics. Adding a generic high/low/high retry
would also assume undocumented register side effects and progress behavior.

Consequently no live read-callback adapter was added. Needed evidence is an
MT6789 CUR sampling/latch specification or an equivalent verified provider
contract, including rollover, reset and allocation lifetime synchronization.
The existing normalized API still requires an already coherent 34-bit sample.
No actual register reads or new hardware/host execution were performed for this
document-only review; it does not claim rollover validation of a live provider.

## WaveCyclic 32-bit allocation specialization (2026-11-18)

The general 34-bit split-register problem above remains unresolved, but the
actual PortCls master channel is deliberately restricted to32-bit allocation.
ConfigureDl1 rejects any buffer ending above0xffffffff. Under the retained,
exclusive, no-reset/reconfiguration allocation contract, CUR high cannot change
when this ring wraps: both ends are below4GiB. `AfeCur32` therefore follows stock
low0x5c then high0x58 order and requires high==0. The low word is a sample from
that ring even if wrap occurs between reads. Buffers crossing4GiB or residing
above it fail before I/O; unexpected high/out-of-range values fail without
modulo salvage. This is a bounded-domain solution, NOT a generic coherent34-bit
reader, hardware latch assertion or reset-safe sample.

WaveCyclicStream ObserveDmaPosition connects the actual configured channel plan
and retained I/O to this reader. GetPosition now returns initial/pre-start STOP
zero as specified, and after a completed explicit start request uses a separately
bound DISPATCH-safe observation executor to return the stock-normalized cyclic
CUR offset as an **uncompensated play-position estimate**. Unknown FIFO/AFE/codec
buffering may make this lead the actual DAC; no zero delay or calibrated accuracy
is asserted. A hardware read or executor error preserves the caller's output.
A failed/partial start is not treated as a running position. Ordinary SetState
RUN/PAUSE remains unfinished, so this is still not a publishable playback stack.

Microsoft's GetPosition contract asks for the best estimate, not an exact
physical DAC measurement, and explicitly allows inference from a DMA pointer.
The same-SoC/stock pointer function uses CUR minus BASE rounded to16bytes, with
no identified FIFO correction in that path. We use that available estimator
rather than invent a delay or block all position reporting until perfect
measurement. This is an engineering inference from those sources, not Microsoft
certification of this T70 estimate. Searches of official samples found virtual
MSVAD/SysVAD timing models and older AC97 references, not a physical T70 FIFO
calibration; no simulated timer or unrelated sample delay was copied.
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-getposition

The prior PASSIVE mutex executor cannot satisfy GetPosition's <=DISPATCH IRQL.
T70AfeIrqRecoveryObserve instead takes a nonblocking atomic observation lease,
then KeSynchronizeExecution. Close/worker close lease admission before attempting
disconnection. Active observations force NOT_READY with IRQ/references retained;
after they finish, explicit Close retry can disconnect. No blocking wait or
kernel mutex is acquired in the observation path. Fault is checked inside the
ISR lock. Context/code lifetime and stream-method serialization still require
the real owner. No firmware/reset contender is assumed absent merely because
this CPU-side lock is held.

Tests cover every offset in the192-byte fixture, normalized frame/byte values,
end→base wrap, read errors, unexpected high and out-of-range CUR, a buffer ending
at0xffffffff, rejection of a4GiB-crossing buffer, PASSIVE/DISPATCH GetPosition,
output preservation, observation-gate overflow and active observation vs Close
retention/retry. These mock tests do not measure real FIFO lead, validate kernel
scheduler behavior, or establish audible playback.
