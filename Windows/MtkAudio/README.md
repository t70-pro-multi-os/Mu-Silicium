# T70 audio: AW88261 configuration component

`AwAcf.c` is an allocation-free C component intended for a future Windows audio
amplifier driver. It validates the actual T70 AW88261 ACF v1 envelope, every
section boundary and CRC8, and finds a unique device-index/profile/type section.
It is executable parsing code, not a PnP driver, INF, audio endpoint or playback
implementation. No driver currently calls it. It never performs hardware I/O.
The caller must keep the input immutable and alive while using the returned view.
CRC8 does not authenticate firmware. Trusted-file acquisition is still required.

## Local primary evidence

The extracted `vendor_a_firmware/aw882xx_acf.bin` has SHA256
`4bb530416e53d20bdb4e42fd63a77e04c7d1aa26ced0c24404752bf967385bd5`,
3856 bytes, an 80-byte header and twelve 64-byte descriptors. It identifies T70
and aw88261, with device indices 0..3, profile IDs 0/10, and section types 4/10.
The envelope parser alone does not map indices onto DT amplifier addresses,
assign human profile names or issue register writes. Register-bin decoding and
board selection are implemented below.
Other section types and runtime programming still require additional evidence.

The stock `snd-soc-aw882xx.ko` SHA256 is
`ea99858a0dffa782cf024871793c7b897839c6fdb3020a7344f47a982cd04f0f`.
Its function `aw882xx_dev_parse_check_acf`
(0x5f88) checks magic 0x0a15f908, version and descriptor/data lengths.
Its version-1 path at 0x6294 uses 64-byte entries, length +32, offset +36,
and CRC +40. `aw_dev_parse_crc8_check` at 0x781c uses reflected polynomial
0x8c with initial zero. All twelve original payload CRCs match this implementation.
This implementation deliberately rejects overlapping/non-contiguous payloads,
ambiguous keys, unsupported version/layout, unrecognized chip/type/profile, and
trailing data instead of reproducing permissive or wrapping stock arithmetic.

## Validation

Compile only this portable component and its host test:

```sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all Windows/MtkAudio/AwAcf.c \
  Windows/MtkAudio/AcfTest.c -o /tmp/t70-acf-test
/tmp/t70-acf-test /path/to/stock/aw882xx_acf.bin
```

The fixture is read externally, not copied or redistributed here. Tests cover
all original sections, every truncation length, a single-bit change at every
payload byte, invalid structural fields, duplicate selectors, unaligned input,
missing selection, output preservation, null inputs and trailing data.

## Still missing before audio can work

Windows PortCls/WaveRT endpoint and PCM DMA/interrupt handling; MT6789 AFE
routing; MT6366 codec control; runtime amplifier identity verification, register programming
semantics and calibration; I2C1 provider; lifecycle and clock/power ownership.
No ACPI device is invented or enabled. No device playback, firmware build, boot, flash or hardware write has been
performed. Selected Windows adapter translation units have now been compiled
against official WDK headers as described below; a linked audio driver is absent.

## Register-bin decoder

`AwRegisters.c` now connects ACF section selection to the inner register-bin
format. It validates the 60-byte single-bin header, version 0x01000000, data
version 1, two-byte register/value widths, exact count/length and additive
checksum, then exposes a chosen 16-bit address/value pair. All eight original
register sections contain 57 records, totaling 456 records. Both upper/lower
case AW88261 chip labels in the shipped profiles are accepted.

Stock evidence: `aw_check_sum` at 0x568c sums bytes after the checksum field;
`aw_check_register_num_v1` at 0x582c checks (data length - 4)/(register width +
value width) against the count and skips that count before exposing records.
5,192 decoder checks cover every original record, truncations, byte corruption,
structural corruption with a recalculated checksum, bad index and output
preservation. This is parsing, not an amplifier programming sequence: reserved,
read-only and sequencing-sensitive registers are not classified for writes.

Run both suites against the hash-checked external fixture:

```sh
python3 Windows/MtkAudio/test-audio-config.py /path/to/stock/aw882xx_acf.bin
```

The DT explicitly maps `sound-channel` 0 to I2C address 0x34 and 1 to 0x35.
This alone is not proof that ACF device indices 0..3 have the same meaning;
the additional binary evidence below is used for the board selection layer.


## Board-to-profile connection

`AwBoard.c` now feeds the verified T70 I2C1/base 0x11e01000, address 0x34/0x35,
and sound-channel 0/1 identity to the validated ACF/register path. Mismatched
bus/address/channel tuples, unsupported profiles and out-of-range records fail
without changing output. Device indices 2/3 remain inaccessible through this
board API. No guessed left/right speaker naming is assigned.

Additional stock evidence: relocation at 0x10b2c in `aw882xx_dev_probe` names
`.rodata.str1.1+0x5da6`, the string `sound-channel`; the read at 0x10b58 fills
stack+4 and 0x10c24/0x10c2c stores it to device+0x34 in the accepted path.
The default ACF selector at 0x681c..0x6838 compares device+0x34 to descriptor
+0x14 and requires descriptor type 2. This matches the two DT channel values.
The host board test checks all 512 channel/address combinations and all 228
records across both board channels and two profiles, plus wrong-bus and bad
selection cases. A trusted live resource provider is still absent: this API
consumes supplied identity and is not a PnP/SPB implementation or hardware probe.

## Windows SPB register transport (not yet a bound device)

`AwSpb.c` now contains actual KMDF I/O-target calls rather than a parser-only
callback: `T70AwSpbWriteRegister` sends three bytes through
WdfIoTargetSendWriteSynchronously; `T70AwSpbReadRegister` submits one atomic
IOCTL_SPB_EXECUTE_SEQUENCE (write register byte, read two value bytes).
Both require PASSIVE_LEVEL, use a bounded 50 ms request timeout, check exact
three-byte completion, propagate errors, and never retry a possibly applied
write. The 50 ms timeout is a provisional transport policy, not a recovered
stock timing requirement. A failed/short read leaves the caller's output unchanged. Register
arguments above 255 are rejected rather than silently truncated.

Primary stock evidence in the already hash-identified snd-soc-aw882xx.ko:
aw882xx_i2c_read at .text 0x1394 builds two I2C messages (1-byte write,
2-byte read), checks return count 2 at 0x145c, and uses REV16 at 0x14c8.
aw882xx_i2c_write at 0x154c stores the register byte at 0x15f4 and value at
0x15f8, then submits length 3 at 0x15fc. Thus wire values are big-endian,
not the little-endian ACF encoding. Unlike the stock write's nonnegative
success check, this adapter rejects a short successful Windows transfer.

Windows contracts: [synchronous writes](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetsendwritesynchronously),
[atomic SPB operations](https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/atomic-bus-operations).
The existing Himax consumer supplies the repository's SPB sequence pattern;
its device identity/resources are not reused for audio.

`SpbTest.c` compiles the actual adapter against focused WDF/SPB host mocks.
982,674 checks cover all 16-bit values, all 8-bit addresses, invalid 16-bit
addresses, short/overreported transfers, failures, exact sequence direction
and lengths, timeout options, IRQL rejection and output preservation. These
mocks are not a WDK ABI check. The existing test-audio-config.py runs this
suite alongside the ACF tests. No Windows kernel build or live I/O was run.

Still required: a verified PnP resource provider opening the correct target,
lifetime/serialization and power ownership, identity and register-write policy,
ACF runtime sequencing/calibration, and the audio endpoint/AFE path. The adapter
is callable source for that future consumer, not an installed driver. No code
automatically writes the 57 ACF records: their special register semantics have
not yet been implemented. No HID, ACPI node or INF was invented.

### SPB Resource Hub target opening

The Resource Hub ID resolves to an SPB target: `reshub.sys` reparses the open to
MtkI2c/SpbCx. The client must not send `IOCTL_RH_QUERY_CONNECTION_PROPERTIES`
to that target because SpbCx does not implement that Resource Hub IOCTL. The
MtkAudio wrapper now opens the target and relies on MtkI2c's `TargetConnect`
callback to validate the I²C descriptor. Its board tuple still limits the
amplifier base, address, and channel before any chip access. The host test checks
the reparse-open path and confirms that it sends no Resource Hub query to the
SPB endpoint.

### I2C1 resource validation now implemented

The independent resource-map gap above is now fixed: ResourcesValid also accepts
I2C1 MMIO 0x11e01000/0x1000, APDMA 0x10217100/0x80 and raw GSI 177.
Evidence is `/soc/i2c@11e01000` in the effective stock DTB, SHA256
`a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391`: the IRQ
cells are `<0 145 4 0>` (SPI145 plus 32, level-high), and the compatible is
`mediatek,mt6983-i2c`. Existing controller instances are unchanged.

`python3 Windows/MtkI2c/test-resources.py` extracts the actual ResourcesValid
function into a focused host mock and runs 135 ASan/UBSan assertions. The same
test fails against the preceding validator. It checks valid instances and
wrong/missing resources, addresses, sizes, flags, sharing and raw IRQ; translated
Windows interrupt vectors remain OS-assigned. No mapping or I/O runs in this
test. This does not enable a controller node or supply IRDY/IRCK, amplifier
connection identities, power ownership or PnP lifecycle. Those remain unresolved.

## Stock preserve-mask update connected to SPB

`AwMask.c` implements the independently recovered `aw882xx_i2c_write_bits`
operation through read/write callbacks. `T70AwSpbUpdatePreservedBits` connects
those callbacks to the real Windows transport and preserves its failure status.
The mask means **bits to preserve**, so the update is
`(old & preserve_mask) | (new_value & ~preserve_mask)`, restricted to 16 bits.
A failed read prevents writing; a failed write stops immediately. Even when
unchanged, the value is written once, matching the stock helper. No rollback,
retry, register permission or calibration policy is inferred. The owner must
serialize the entire RMW pair, not just each separate SPB operation.

Stock helper `.text 0x16f4` (size 0xcc): read at 0x1730, negative-status exit at
0x1734, BIC/AND/ORR at 0x173c/0x1748/0x174c, write at 0x1750 and error check at
0x1754. This is independent of the lower Linux transport's own retries; the
Windows adapter deliberately does not duplicate those retries.

The portable mask suite checks every 16-bit mask with four old/new patterns,
per-bit selection, no-op writes and callback failures: 5,242,896 checks.
The actual Windows adapter host suite now has 982,746 checks, including failed
or short reads preventing writes and failed or short writes propagating errors.
Both pass ASan/UBSan along with the existing ACF suites.

This does **not** establish which ACF registers need particular masks, which
values require calibration, or when the amplifier is safe to program. The
57-record profile is still not applied automatically. A verified full register
policy and live lifecycle owner remain prerequisites; power sequencing has not
been implemented or bypassed in this work.

## Read-only AW88261 identification

The platform-neutral `AwIdentity.c` takes the explicit register-read callback
and reads register 0x00 once. Only ID 0x2113 succeeds; no write callback is
required or called. Invalid arguments, failed reads and every other ID leave
the caller's output unchanged. This does not discover a board, select a bus,
reset or power a chip, or authorize profile writes. The existing optional SPB
adapter calls this core and distinguishes wrong ID from transport errors.

Stock probe evidence: register zero is passed at .text 0x5c4 to the read at
0x5cc; compare 0x2113 at 0x6a8/0x6ac reaches the accepted branch and stores
that ID at 0xbb4. The stock family driver also accepts other chips, so its whole
ID list must not be treated as AW88261-compatible. The model association is
independently corroborated by Awinic's upstream Linux
[AW88261 header](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.h)
and [identity check](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.c).

Host tests cover all 65,536 IDs on successful and failing callbacks, null input,
exact read-only behavior, short/overreported transfers and output preservation:
393,220 identity assertions and 1,572,664 adapter assertions, ASan/UBSan.
Existing mask and ACF suites also pass. No chip was accessed; matching a mock
ID is not evidence that the actual T70 amplifier is powered or reachable.
The control core does not assume automatic device enumeration or a particular OS.

## Pure volume-field arithmetic

`AwVolume.c` now implements the verified 2113/AW88261 register-0x05 arithmetic
without transport calls or state transitions. Bits 15:10 are preserved with
0xfc00; the low ten bits decode as `(field >> 6) * 48 + (field & 63)`.
Canonical attenuation values 0..720 encode as `(value / 48) * 64 + value % 48`.
The unit is 1/8 dB attenuation, not a percentage. The compose helper combines
initial and requested attenuation, saturates at 720 without integer overflow,
and merges the encoded field into the existing upper six bits. It returns a
value only; it does not apply it. Invalid standalone encode requests preserve
output. The limit 720 encodes as 0x3c0.

Stock evidence: descriptor stores at 0x1306c..0x1307c establish register 5 and
mask 0xfffffc00; shift zero is stored at 0x12fc0. The 2113 branch at
0x13330..0x13344 selects decode 0x14154 and encode 0x14164; 0x13448..0x13450
sets the 720 limit. aw882xx_dev_set_volume adds initial attenuation before its
limit/encode/merge sequence. Awinic's upstream header independently defines
the same field and conversion. This does not reproduce the distinct live
getter, which clamps the raw field before decoding. Noncanonical field values
need not round-trip to identical bits, so tests do not assume that they do.

1,943,027 ASan/UBSan volume assertions cover all raw 16-bit registers, accepted
and rejected encodes, upper-bit preservation, coarse/fine boundaries, saturation
and UINT32_MAX requests. Saturating before addition deliberately avoids the
stock 32-bit addition wrap for extreme out-of-range inputs; this is not a
bit-identical emulation of that overflow. Existing audio suites still pass. No actual volume
change, write, mute/power transition or speaker test was performed.

## Pure register-0x08 TX field composition

`AwTxField.c` implements only the verified stock 2113 descriptor transformation:
`(existing & 0xffbf) | (enabled ? 0x0040 : 0)`. Enabled accepts exactly 0 or 1;
all other inputs fail without changing output. Bit 6 is active-high and the
other fifteen bits are retained. No transport call or TX state transition occurs.

Stock descriptor initialization at 0x1339c..0x133a8 writes register 0x08 and
mask 0xffffffbf to +0x170/+0x174; 0x133ac..0x133b0 stores enable 0x40 and
disable 0 at +0x178/+0x17c. The consumer at 0xfd44..0xfd50 independently
confirms the mask-and-disable transformation. The public AW88261 reg-update
path's I2SCTRL3 handling agrees on clearing bit 6. Its separate helper referring
to I2SCFG1 (0x13) is not treated as interchangeable with this stock descriptor.
No claim is made about when enabling is valid or what board state permits it.

Exhaustive host tests cover every 16-bit input for both enable values, every
unaffected bit, idempotence, toggle behavior and invalid-input output preservation:
2,621,697 ASan/UBSan assertions. Existing audio suites also pass. This is a
platform-neutral value calculation, not hardware transmission or an ACF apply
sequence; power/reset and actual I2C remain untouched.

## Explicit callback volume update

`AwVolumeUpdate.c` now connects the pure volume calculation to a single explicit
read/compute/write operation on register 0x05. Its synchronous callbacks return
zero only for complete success; errors are retained verbatim in the result's
TransportStatus, with read/write failure distinguished by Code. The caller
supplies the verified target and holds serialization for the whole operation.
This API performs no discovery or implicit OS binding.

Initial attenuation must be a decoded profile value 0..783; requested
attenuation must be 0..720. Invalid requests, missing callbacks or missing result
storage cause no I/O. A read failure prevents the write. After a complete read,
the function preserves the upper six bits and applies the previously verified
clamp/encode calculation. The output register value is published only after a
complete write succeeds. It never retries or rolls back a failed write, which
may already have affected hardware. It retains no pointers after return.
The existing optional SPB adapter is connected through T70AwSpbUpdateVolume;
its short transfers and original failure statuses propagate through this path.

Host failure-injection results: 2,488,929 update assertions and 1,572,731 SPB
assertions, ASan/UBSan. Checks include unchanged output during both callbacks,
read-before-write order, no write after failed/short read, exact write error
preservation, invalid-input no-I/O, and all existing register values. Existing
audio suites remain passing. Only mocks were invoked; no hardware volume,
power/reset state or actual I2C transaction was changed.

## Explicit callback TX-field update

`AwTxUpdate.c` connects the verified 0x08 field calculation to an explicit
synchronous read/compute/write operation. It accepts only enabled=0 or 1 and
rejects invalid arguments before I/O. It preserves all bits except bit 6,
performs no write after a read failure, and returns the exact callback error
with a separate read/write failure code. AppliedRegister is unchanged until
write success; no callback pointers or buffers are retained after return.
The optional existing SPB adapter now calls this same core and propagates short
transfers as errors. The shared status adapters were renamed without changing
the volume update behavior.

The caller must serialize the entire operation and provide the target and
valid-state decision. This code does not establish when changing TX is safe,
perform automatic enumeration, or manage power/reset. Failed writes can have
partial effects; there is no retry or rollback guarantee.

Host tests: TX update 786,742 and SPB 1,572,810 assertions, ASan/UBSan, plus all
previous audio suites. Coverage includes every original 16-bit register and
both requested values, read/write ordering, unchanged output inside callbacks,
read/short-read failure stopping writes, write/short-write error propagation,
and invalid-input no-I/O. No actual I2C or transmission was performed.

## Read-only clock-status snapshot (not a health verdict)

`AwStatus.c` reads SYSST register 0x01 once through an explicit synchronous
callback. It returns Raw, PLL-lock bit 0, stable-clock bit 4, their conjunction,
and every other bit as UninterpretedBits. A successful read does not imply
clock lock, no faults, playback readiness or permission to change TX. No poll,
write, interrupt-clear, reset or state transition occurs. Read errors preserve
both their exact status and the entire caller snapshot. The optional existing
SPB adapter also checks exact transfer length before publishing a snapshot.

Stock evidence: 0x12fc8..0x12fec initializes descriptor register +0x1d8=1 and
PLL mask +0x1e8=0x11, not overwritten by the 2113 branch. The independent PLL
consumer 0x10fb8..0x10fe4 reads that register and checks `mask & ~raw == 0`.
Awinic's public header agrees on the two bit names and active-high polarity.

Full fault interpretation is deliberately deferred. The stock descriptor's
broader interest mask is 0x4b3b, whereas the public header expression produces
0x0b3b; bit 14 treatment differs. Stock expected values 0x211/0x311 also require
state-dependent selection not established here. No fault-free/ready Boolean
was invented, and those bits are retained without interpretation.

393,235 status assertions cover all 65,536 raw combinations, reconstruction of
uninterpreted bits, exact error propagation and untouched failed outputs.
The adapter suite has 1,572,904 assertions including short/overreported reads.
All audio suites pass ASan/UBSan. These are mock reads only, not actual amplifier
status or a firmware/OS build result.

### Read-only register preflight inventory

`AwPreflight.c/h` accepts an already selected inner register-bin (for example,
`T70AwAcfFind` section type 4). `T70AwPreflightCount` and zero-based
`T70AwPreflightEntry` validate the entire bin and enumerate known special
addresses 0x04/05/08/14/50 without changing data or invoking any transport.
Each entry includes the original record index, byte offset relative to the
inner bin, address and value. Duplicates and ordering are preserved. Unlisted
addresses remain unclassified; this is neither a complete policy nor a safe
application plan. SYSCTRL's stock/public difference remains documented in
`REGISTER-POLICY-REVIEW.md`; no power or calibration handling is implemented.
Outputs remain unchanged on failure. The caller supplies immutable input and
nonoverlapping output storage. The API retains no pointers and allocates nothing.
`PreflightTest` covers all eight stock register sections, truncation/corruption,
invalid headers with repaired checksums, duplicates, unknown 16-bit addresses,
empty inventories, invalid ordinals and null outputs under ASan/UBSan.

### Explicit board profile view

`T70AwProfileView` joins the existing board-tuple check, ACF selection,
register-bin validation and special-record inventory. Its result contains a
borrowed read-only section plus record and special-record counts; use
`T70AwPreflightEntry` to enumerate that section. The caller must keep the entire
ACF alive and immutable for every use of the view. No allocation, transport,
internal retained state or hardware discovery is involved, and failure preserves
the output. This is not an executable driver or an application approval.

The stock container has eight register sections (four indices, two profiles),
but only indices 0/1 map to the two established board channels. Consequently
this board API accepts four combinations and rejects indices 2/3 rather than
inventing two more amplifiers. Tests cover all eight selections, independent
backing buffers, malformed/truncated input, duplicate identities, repaired
outer CRC with invalid inner data, mismatched tuples and unchanged failure output.
Unused container metadata is not authenticated; structural checks and checksums
are not provenance verification. Only the selected register-bin is interpreted.

### Explicit, non-destructive probe

`T70Aw88261Probe` validates the caller's board tuple before any callback, reads
chip ID through the existing identity core, and only on a match reads SYSST
through the existing clock-snapshot core. It returns phase-specific errors with
the original transport status. Every failure leaves the whole output unchanged;
no later read is issued after a failed step. Its read-only callback interface
has no write/reset/power operation, retries, polling or discovery assumptions.

The caller must associate that callback with the stated bus/address and serialize
both reads. Tuple validation cannot verify callback routing. Keep the callback,
context and disjoint output alive throughout the call. The sequential snapshot
is not atomic and matching ID/clock bits do not establish playback readiness.
`ProbeTest` covers both channels, every ID/status value, invalid tuples and
phase failures. `SpbTest` additionally routes the core through the real register
read adapter against host mocks, including short transfers and transport errors
at either phase. These are host checks, not real I2C access or speaker testing.

### Two-channel probe publication

`T70Aw88261PairProbe` takes separate explicit endpoints in channel 0/1 order,
validates both before I/O, then runs the existing probe on each. It publishes
neither snapshot until both finish successfully. Failures identify the channel
and underlying probe phase/status; subsequent reads stop immediately. Invalid
or reversed tuples, null callbacks and identical context pointers are rejected
before reading. Distinct non-null contexts are an intentional guard against
accidental target reuse, not proof of physical routing; distinct wrappers could
still refer to the same device. The caller verifies that association and holds
both target lifetimes/serialization throughout the call. There is no automatic
PnP/discovery assumption, hardware write or atomic stereo/readiness claim.
Host tests verify ordered distinct contexts, each failure phase, output
preservation, reversed/mismatched tuples and independent raw status values.

### AFE PCM buffer arithmetic

`AfeGeometry.c/h` checks recovered MT6789 stock byte/period limits and 34-bit
DMA-address arithmetic without touching hardware. See `AFE-GEOMETRY-EVIDENCE.md`
for module offsets, DTB bounds and explicit gaps. Buffer length must be a multiple
of 16; this does not establish a starting-address alignment rule. Caller-supplied
frame size is not a supported-format verdict. Passing geometry does not allocate,
map, start DMA, select a playback route, or establish that sound can play.

### Restricted DL1 format encoding

`T70AfeDl1Format` validates the recovered DL1 frontend rate/channel combinations
and encodes S16_LE or S24_LE-in-32-bit-container as unshifted rate/HD/alignment/
mono values plus storage widths. S32 and unknown combinations fail without
changing output. See `AFE-FORMAT-EVIDENCE.md` for the scope and stock offsets.
This does not select a speaker route, configure clocks, write MMIO or start DMA.

### Combined DL1 arithmetic preparation

`T70AfeDl1Prepare` derives frame storage size from the accepted DL1 format and
runs the geometry checks with that exact size. A self-contained value snapshot
is published only when both succeed; failed calls preserve prior output. There
are no retained pointers or shared internal state. Callers should treat the
snapshot as immutable; this is a usage contract, not C-enforced const ownership.
S32 remains rejected. Tests cross valid/invalid formats with period divisibility,
length limits, address crossings and overflow, including S24 stereo rejecting a
100-byte period that S16 stereo accepts. This is preparation of arithmetic data,
not hardware initialization or a claim that DMA or speaker output is operational.

### CUR position conversion

`T70AfeDl1Position` validates a prepared snapshot and converts a caller-supplied
coherent CUR to byte/frame offsets without MMIO. Out-of-range, wrapping arithmetic,
misaligned offsets and altered derived fields fail without changing output.
It deliberately rejects rather than reproducing stock rounding. A CUR at BASE
is zero; no lap count or temporal wrap can be inferred. See
`AFE-POSITION-EVIDENCE.md` for exact semantics and missing sampling guarantees.

`T70AfeDl1NormalizedPosition` is a separate option that rounds a validated
in-range CUR byte offset down to 16, matching the stock arithmetic before frame
conversion. Invalid addresses still fail; the original strict API retains its
behavior. This resolves rounding compatibility only, not coherent MMIO sampling
or live playback integration.

### Period-frame IRQ count

`T70AfeDl1IrqCount` revalidates prepared data and returns the period frame count
within the recovered 18-bit field limit. `T70AfeIrqCountField` exposes that pure
bound check for independent testing; neither selects or programs an IRQ. Count
is frames, not bytes and not frames-minus-one. See `AFE-IRQ-COUNT-EVIDENCE.md`.
Prepared-field validation is shared with CUR conversion, preserving both strict
and normalized position behavior. Hardware IRQ ownership remains unimplemented.

### Stream lifecycle connection status

`STREAM-INTEGRATION-GAPS.md` records why the existing arithmetic and amplifier
callbacks cannot yet form a real DL1 prepare/start/stop/position lifecycle.
There is no AFE DMA-resource/IRQ-owner/start-stop/coherent-CUR provider here.
No fake Running state or mock-backed production connection has been added.

### DL1 base/end callback connection

`T70AfeDl1SetAddress` now sends the verified snapshot's base/end halves to an
explicit write callback in the recovered stock order. It never writes CUR or
starts DMA. Invalid input causes no calls; a failed write stops the sequence and
returns attempted/completed counts and the failed offset. Partial writes are
not rolled back. Caller-proven quiescence, exclusive ownership and a real mapped
provider remain prerequisites, not implemented here. See
`AFE-ADDRESS-EVIDENCE.md`; tests use mocks only, so this is not live playback.

### DL1 format masked updates

`T70AfeDl1UpdateFormat` connects the validated format to four explicit read/write
pairs at offset 0x4c, preserving unrelated bits. It rejects invalid requests
before I/O and stops at the first callback error, reporting phase and progress.
The masks identify bits to update, not bits to preserve. Quiescence and exclusive
RMW serialization remain caller/provider requirements; no actual MMIO provider,
DMA start or power operation is added. See `AFE-FIELD-EVIDENCE.md`.

`AfeConfigure.c` combines validated DL1 address writes then format field updates
through one explicit callback target, following stock hw_params ordering. It
reports started stages and exact partial failures, without rollback or a playback
claim. Caller-owned quiescence/serialization and a real provider remain required.
See [configuration evidence](AFE-CONFIGURE-EVIDENCE.md).

`AwPairProfile.c` publishes channel 0/1 register-profile views together from one
immutable ACF. The observed board channels each have two type-4 profiles (0/10);
all four inner bins must validate, including the unrequested profile. Missing,
duplicate or damaged members leave the whole output unchanged. This is a strict
stock-catalog policy, not a general stereo-format requirement. Per-channel values
and record counts need not be identical. Type-10 metadata is only validated by
the existing container contract; ACF indices 2/3 are not board amplifiers. Physical
left/right placement is not inferred. Views borrow ACF storage and perform no I/O.

`AwPairInspect.c` is a read-only consumer joining the paired profile catalog with
explicitly injected channel-0/1 read targets. Invalid resources or catalog errors
perform no I/O; probe failure publishes neither profile views nor observations.
Success publishes both together, but the hardware reads are sequential, not an
atomic observation. It does not apply profiles or establish ready/playback state.
The caller must supply real verified targets and serialization; this component
still creates no board resource provider. Tests use only mock read callbacks.

`AfeIrqFields.c` connects an explicitly supplied, stock-whitelisted IRQ descriptor
and prepared DL1 snapshot to count-then-rate masked callbacks. It does not allocate
an IRQ or enable it. Shared-register serialization and actual ownership remain
provider obligations. Stock enables memif before this subset; this helper is not
a start sequence or proof of pre-start safety. See `AFE-IRQ-COUNT-EVIDENCE.md`.

Board DTB and machine-module endpoint evidence is recorded in
`BOARD-AUDIO-ROUTE-EVIDENCE.md`. I2S3/I2S0 and two AW targets are supported by
static declarations; the selected DL1 mixer/slot route is not yet established.
The MT6366 machine name must not be treated as proof of a serial codec-to-AW path.

`AudioInspect.c` provides one all-or-nothing consumer result containing a
revalidated DL1 value snapshot and the read-only AW pair inspection. Invalid DL1
input prevents amp I/O. This is deliberately NOT a call to `AfeConfigure`, which
writes registers: the DL1 part is a validated input copy, not a hardware
observation. Only amp reads are injected. Publication means facts collected, not
route compatibility, applied configuration, playback readiness or permission to
start. Real targets/lifetime/serialization remain caller responsibilities.

## Official-header verification status (2026-11-18)

The earlier macOS tool check did not find WDK headers. Subsequently, with user
approval, complete official Microsoft NuGet SDK.CPP, SDK.CPP.arm64 and WDK.ARM64
10.0.28000.2526 packages were acquired into the Linux builder's private cache.
Their package-linked licenses were checked; package notices remain intact.
Linux aarch64 clang-cl 21.1.8, KMDF 1.35 headers and a case-insensitive VFS overlay
compiled AwSpb.c, AwSpbPair.c, AwSpbTarget.c and AfeCommonBuffer.c as ARM64
COFF objects. Include
traces excluded Tests/include; standard integer definitions came from LLVM.
AwSpbTarget required the official RESHUB_USE_HELPER_ROUTINES declaration opt-in.
Official headers produced compiler-compatibility warnings; these checks were
not warning-free MSVC builds. No project-source warning was reported.

These are single-file compilation results, not linked .sys files, INF/package
validation, signing, loading, WDF verifier tests or hardware execution. The host
SpbTest suite still uses test substitutes and remains distinct from these checks.
No audio DriverEntry, device/stream owner, INF, .vcxproj or .sln is supplied.
See STREAM-INTEGRATION-GAPS.md for the unconnected upper-owner requirements.

## Combined read-only route and amplifier inspection

`T70AudioInspectWithRoute` is a separate opt-in consumer of `T70AudioInspect`
and `T70AfeObserveDl1I2s3`; the original API is unchanged. It first rejects a
missing route reader or reused/nonexistent endpoint contexts, then collects
the validated DL1 input copy, ACF views and four amplifier reads, then the two
route reads. Only full success publishes the combined result. A failed stage
stops later reads and leaves caller output unchanged; earlier reads cannot be
undone. The result reports which nested operations started and their errors.

Publication is all-or-nothing at the API boundary, NOT an atomic memory store
for concurrent readers or simultaneous hardware sampling. Route selection bits
may be zero and still be valid observations. Neither set bits nor Collected
means playback-ready, compatible, exclusively routed or configured. The DL1
snapshot remains supplied input, not observed hardware. Context inequality is
only a misuse check; the caller still owns routing, mapping/accessibility,
lifetime and serialization. No provider or removal guarantee is fabricated.
The synchronous function retains no contexts; borrowed ACF data must outlive
result use. Tests exercise separate mock contexts, every read failure, wrong ID,
invalid inputs/ACF, output preservation and all four route-bit combinations.
No MMIO/I2C operations, power actions or native WDK compilation were performed.

## CPU-memory PCM ring copy

`T70PcmRingCopy` connects a revalidated DL1 prepare snapshot to caller-owned
ordinary memory. It copies frame-aligned bytes with at most one wrap and returns
new write offset, copied bytes and frames. The caller supplies available space;
this code does not infer consumer progress or choose an underrun/overwrite
policy. Capacity, offsets, lengths, integer ranges and overlapping source/ring
or metadata are checked before any copy. Failure preserves ring and progress.
Zero-length copies permit NULL source and leave the write position unchanged.

This is independent byte-copy code, not borrowed Linux DMA/PCM implementation.
A valid numeric DMA base is not proof that Ring maps that address. Caller owns
valid memory/lifetimes, stable free-space accounting and exclusive synchronization
through copy/publication. No allocations, MMIO, DMA ownership transition, cache
maintenance, hardware start or sample conversion occur. Success cannot authorize
playback. Host tests use only local arrays, cover S16/S24 mono/stereo and every
frame-aligned offset/length in a 512-byte ring, including full-ring writes,
wraps, invalid availability/capacity, overlap and integer overflow rejection.

## Caller-supplied Windows AFE mapping adapter

`AfeMappedIo` supplies callbacks compatible with `T70AfeDl1Configure` for an
already owned resident mapping. It requires the stock DT tuple
0x11210000/0x2000, a non-NULL 4-byte-aligned virtual base and aligned in-range
32-bit offsets. It rejects virtual-range overflow and read outputs overlapping
the mapping or context. It does not create a mapping, validate actual page
attributes, own a target, establish accessibility or synchronize unmapping.
The tuple is a consistency check, not proof of a valid hardware mapping.

The production adapter calls Windows kernel
[READ_REGISTER_ULONG](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-read_register_ulong)
and [WRITE_REGISTER_ULONG](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-write_register_ulong).
Microsoft documents memory barriers and requires resident mapped device memory;
plain volatile accesses are not substituted. A returned write primitive has no
status to prove peripheral acceptance. This wrapper cannot detect bus faults,
replace device-specific sequencing or grant permission for arbitrary registers.
Caller must retain the mapping, serialize entire operations and establish the
existing configure quiescence/ownership requirements. No power or DMA start
sequence is added.

The host test substitutes both primitives with counted RAM accesses, checks every
aligned offset and unaligned offset in the 8 KiB range, edge/overflow/tuple errors,
output preservation, and the actual configure-to-adapter callback connection.
These tests do NOT exercise Windows barriers, genuine MMIO or WDK ABI/linking.
Native WDK compilation remains unavailable as recorded above. No real mapping,
hardware access or whole firmware build was performed.

## Explicit mapping ownership (Windows adapter, host-tested only)

`AfeMapping` accepts only the explicit 0x11210000/0x2000 translated memory
resource supplied by its actual owner. It does not discover or assign a device;
a DT address alone is not resource ownership. A zero-initialized, noncopyable
caller-owned object receives the mapping only after success. Duplicate acquire
is rejected; failed mapping preserves the object. Release unmaps the original
base/length once and clears the object; repeated release of an empty object is
harmless. Malformed returned pointers are rejected with matching cleanup.

[Microsoft's MmMapIoSpaceEx contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmmapiospaceex)
supports nonpaged mapping with PAGE_READWRITE|PAGE_NOCACHE, used here without
execute/write-combine flags. It returns NULL on insufficient mapping space.
[MmUnmapIoSpace](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmunmapiospace)
requires the mapped base and byte count. Both support IRQL <= DISPATCH_LEVEL;
this adapter checks IRQL and assumes resident caller storage/code. No mapping
can establish clock/power accessibility or resolve incompatible cache aliases.

Begin/End leases bracket an entire configure/RMW operation. Release fails while
leases exist. The caller MUST externally serialize admission, use and release,
and end a lease only after callbacks finish. Counters are not atomics, locks,
automatic removal callbacks or an asynchronous drain implementation. Copies of
mapping/IO descriptors must not survive their lease. This explicit lifetime
protocol is useful to an actual resource owner, not a substitute for one.

Fake map/unmap and RAM register primitives test allocation failure, invalid
resources/returned pointers, duplicate acquire/release, active and overflowed
leases, IRQL failures and adapter use during a lease. No actual MmMapIoSpaceEx,
MmUnmapIoSpace, device access or kernel build was executed. WDK compilation and
Windows runtime ownership integration remain unverified.

## Configure under an explicit mapping lease

`T70AfeConfigureMapped` validates the prepared snapshot, begins a lease on an
existing owner, calls the real DL1 configure with AfeMappedIo callbacks and
attempts End exactly once after every normal configure return. Begin failure
causes no register callbacks; configure failure still ends the lease. The
result retains nested progress/error details and distinguishes End failure.
If End fails, it does not claim lease cleanup, retry or rollback. Hardware
exceptions are not caught. A software lease ending says nothing about DMA
quiescence or undoing already-applied writes.

The caller must externally serialize the entire wrapper, retain immutable
snapshot/mapping and meet existing hardware ownership/access preconditions.
No automatic acquire/map, unmap, start, power transition or retry is added.
The failure-injection test substitutes adapter callbacks to exercise every
configure call position, positive/negative statuses and an intentionally raised
IRQL that prevents End. This does not mean native register primitives report or
recover from bus faults. The mapping test separately links the real wrapper,
configure and adapter over fake map/register primitives. Neither is a Windows
kernel build or real device test.

## Route observation under a mapping lease

`T70AfeObserveMappedRoute` connects the existing two-register observer to
AfeMappedIo through an explicit owner Begin/End lease. It rejects NULL,
misaligned, overflowing or owner/mapping-overlapping output before admission.
Both reads and End must succeed before publishing output. If a read fails,
End is still attempted once; if End fails, output stays unchanged and both read
result and unresolved lease are reported. No retry, unmap or register write is
performed. The caller must serialize the entire call and retain valid objects
and accessible mapping. This is neither an atomic hardware snapshot nor a
readiness/compatibility verdict, even when both selection bits are set.

Host failure tests cover both read positions, signed status values, existing
leases, invalid outputs and failed End from deliberately invalid provider IRQL.
The integrated mapping test links the actual observer, wrapper and mapped
adapter over fake kernel primitives. No real mapping/MMIO or WDK build occurred.

## Strict Windows extensible PCM descriptor parser

`T70WaveExtensiblePrepare` parses an explicitly bounded little-endian 40-byte
WAVEFORMATEXTENSIBLE descriptor without native structure casts or WDK headers.
It requires cbSize=22, the integer PCM GUID, mono/front-center or stereo/FL+FR,
exact block alignment and a checked 64-bit byte-rate product that fits uint32.
Unknown extensions, direct-out/other masks, float, packed 24-bit and S32 are
outside this deliberately strict subset, not necessarily malformed Windows
formats. Supported rates and numeric geometry use the existing DL1 prepare API.
S16/16 succeeds; every other result preserves output.

Crucially, a valid 24-in-32 descriptor returns the distinct non-success
`T70WaveNeeds24BitNormalization` after structural/geometry checks. Microsoft
requires significant bits to be LEFT-aligned when valid bits are fewer than the
container size. It is not safe to alias that data directly to the existing
ALSA-derived S24_LE path merely because both use four bytes. The
[Linux format definition](https://github.com/torvalds/linux/blob/master/include/uapi/sound/asound.h)
identifies S24_LE as using the low three bytes. No sample
normalization is implemented here, and no 24-bit prepared output is published.
This prevents the existing byte-copy ring helper from silently accepting an
unconverted layout. Caller must not treat the enum as a boolean success; only
`T70WavePrepared` publishes a usable numeric snapshot.

Primary format references:
- [WAVEFORMATEXTENSIBLE fields and valid-bit alignment](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible)
- [cbSize and extended container contract](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible)
- [PCM subtype GUID](https://learn.microsoft.com/en-us/windows/win32/directshow/audio-subtypes)
- [Block alignment and byte rate](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatex)

Tests cover all eleven DL1 rates, both channel layouts, S16 acceptance and S24
normalization-required output preservation, unaligned input storage, every
16-bit cbSize/channel/mask value, GUID bit corruption, every short descriptor,
trailing bytes, rate-product overflow and invalid geometry. These are pure host
byte-parser tests, not WDK ABI compilation or Windows endpoint integration.
No hardware, resource creation, sample routing or playback guarantee is added.

## Explicit Windows 24-bit sample normalization

`T70Wave24ToS24Le` is a separate, pure CPU conversion, not a change to the
descriptor parser. Microsoft defines 24 valid bits in a 32-bit PCM container
as left-aligned with zero low padding; Linux's S24_LE definition uses the low
three bytes in a four-byte container (primary links above). The converter moves
source bytes 1/2/3 to destination 0/1/2 and writes a canonical sign-extension
byte at destination 3. It preserves the signed 24-bit number exactly, including
-8388608, -1, zero and 8388607; it is not scaling, resampling or packed S24_3LE.

The complete input is checked before writing, including zero padding in every
sample. One/two-channel complete frames, capacity and pointer-range overflow
are checked, and all source/output overlap is rejected. Unaligned buffers are
supported through byte accesses. Caller must provide valid ordinary memory,
immutable source and exclusive output throughout both passes. Allocation
validity and concurrent mutation cannot be established by this helper.
Tests exhaust all 16,777,216 signed sample encodings with stereo complements,
unaligned memory, malformed final padding, frame/length/overlap/overflow failures
and output preservation. These are CPU host tests, not WDK or hardware tests.

`WavePrepare` still returns normalization-required for 24/32 descriptors and
does not publish a prepared snapshot. Consumers must explicitly convert each
buffer before using an independently validated S24_LE preparation; this helper
does not certify that all future buffers were converted, create mappings,
flush caches, feed hardware or establish playback compatibility.

## Explicit 24-bit conversion-to-ring memory path

`T70Wave24RingCopy` connects the strict descriptor validator, existing immutable
DL1 numeric preparation, caller-provided scratch, CPU normalization and ring
copy. It accepts only matching S24_LE32 rate/channels and a Windows descriptor
classified as 24-bit normalization-required. S16 and S32 cannot silently fall
back. Ring size/free-space/frame/offset and all six storage ranges are validated
before conversion. Source padding is checked before scratch writes. Failure
preserves ring and progress metadata; scratch is disposable workspace. Success
copies converted frames with at most one wrap and then publishes progress.

Caller must hold exclusive serialization across validation, conversion, copying
and metadata consumption, supply actual valid ordinary memory and an authoritative
free-space value. Descriptor, prepared snapshot, source, scratch allocation, ring
allocation and output metadata must be mutually disjoint. No pointer is retained;
scratch can be reused after return. This is not a concurrent producer/consumer
protocol, DMA/cache ownership, hardware mapping, routing or playback preparation.
The parser's public normalization-required behavior remains unchanged.

## Strict PCM buffer dispatch

`T70WaveRingCopy` is an optional CPU frontend consumer of the existing APIs,
not an installed OS endpoint. A matching strict descriptor selects direct S16
ring copying or the explicit Windows 24/32 conversion path; neither malformed
formats nor S32 fall back to S16. The independently validated prepared snapshot
must match rate, channels and sample format. Six storage ranges are disjoint,
including unused-but-supplied S16 scratch; S16 may instead pass NULL/0 scratch.
All failure returns preserve ring and progress. Scratch is disposable and can
be reused after return under the same caller-owned lifetime/serialization
contract as the lower-level APIs. Cross-check tests compare every mono/stereo
ring offset and frame-aligned length with each direct API, including wrap,
invalid formats, insufficient capacities, padding errors and alias/overflow.
This reduces frontend branch mistakes without claiming a real OS consumer,
DMA mapping, cache synchronization, start sequence or playback readiness.

## Read-only mapped DL1 format comparison

`T70AfeObserveMappedFormat` validates a numeric prepared snapshot, acquires an
existing mapping-owner lease, reads configuration register 0x4c once and releases
the lease. Masks/shifts are the same stock-backed fields in AFE-FIELD-EVIDENCE.md
and AfeFormatUpdate: mono bit 8, rate bits 27:24, HD bits 1:0, alignment bit 4.
It exposes raw/decoded values plus independent match bits 1/2/4/8. Reserved
observed encodings remain visible as mismatches, not newly supported formats.
This is configuration readback, not a status/ready register or proof of playback.

No output is published unless read and End both succeed. Read/Begin/End failures
are distinguished; an End failure retains the unresolved lease and read status.
No register write, automatic retry, map/unmap or hardware execution is performed.
Owner, expected snapshot and output must be valid disjoint ordinary memory
outside the device mapping. Caller owns accessibility, lifetimes and external
serialization across the entire call. Host tests exhaust field encodings with
unrelated bits clear/set for all supported formats, and inject read/lease errors;
they use a fake reader and do not validate real MMIO or WDK ABI.

## Read-only mapped DL1 base/end comparison

`T70AfeObserveMappedAddress` uses the stock descriptor offsets documented in
AFE-ADDRESS-EVIDENCE.md: base-low 0x54, end-low 0x64, base-high 0x50, end-high 0x60.
Within one existing owner lease it reads them in that order, then compares each
raw word to the corresponding validated prepared value. MatchMask bits 1/2/4/8
represent these four independent comparisons. Unexpected high bits are retained
and fail equality; they are not silently masked. No combined live address is
published, because these sequential observations are not a coherent hardware
snapshot or proven latch protocol. A matching set does not prove active DMA,
buffer mapping, playback or readiness.

First read error stops subsequent reads. End is attempted once after every
normal read return, and output is published only when all reads and End succeed.
Error results retain read counts/status/failed offset even when End also fails;
the unresolved lease remains the caller's responsibility. Tests cover all 16
partial-match combinations, every bit position, 32-bit crossing and 34-bit edge,
all read failure positions and End failure, with existing lease counts preserved.
Host fake readers only; no actual MMIO, writes, power/start or WDK ABI validation.

## Driver package completion (not yet installable)

The official-header link produces an unsigned `MtkAudio.sys`, not a complete
installable package. `CatalogFile=MtkAudio.cat` in the INF is a declaration,
not an existing catalog. Keep one explicitly selected package output directory;
do not mix old catalogs or binaries with a newly linked image. Staging must not
modify this source INF or overwrite an existing package.

Stage one package from a successful, current official-header build report:

```sh
python3 Windows/MtkAudio/stage-audio-package.py \
  --report /path/to/audio-wdk-output/report.json \
  --output /path/to/new-audio-package
```

The destination must not already exist. Staging checks the current production
source/header set and hashes against the report, plus the linked image hash and
bounded ARM64 PE/INF structure. Its standalone synthetic regression is
`python3 Windows/MtkAudio/stage-audio-package-test.py`. This is not an official
INF validator. Staging records hashes and explicit unvalidated/unsigned status; it does not generate a CAT or execute Windows tools.

On a Windows WDK host, after staging the exact INF/SYS pair, run the following
with the installed official tools (replace the package path):

```bat
InfVerif.exe /w /v C:\T70AudioPackage\MtkAudio.inf
Inf2Cat.exe /driver:C:\T70AudioPackage /os:10_GE_ARM64 /verbose
```

`10_GE_ARM64` is the documented Windows 11 24H2 Arm64 target matching the
INF's minimum build 26100. Stop on either tool's errors; do not suppress them.
The checked-in DriverVer date must not be in the future on the validation
host. If a release updates the INF date/version or any payload, generate a new
catalog for that exact package. CAT generation is not signing or installation.
The resulting catalog must undergo the applicable Microsoft driver-signing
process before a normal production installation; possessing a generic signing
certificate alone does not establish modern Windows kernel acceptance. No test
mode, certificate trust or Secure Boot settings are changed by this project.

Current builder result (2026-11-22): the documented official WDK 10.0.28000.2526/KMDF 1.35 compiled all 93 production translation units and linked `MtkAudio.sys`; no unresolved project symbols remained. `stage-audio-package.py` and its 5 package checks staged the unsigned, unvalidated pair at `/tmp/t70-audio-package-20261122-current/`. SHA-256: SYS `0a698fd06199e32e4548ac57592ac3b9405c5942939be2c8bd437d78253c02b9`; INF `3dbd76d2ac8f4c4363045b026dafbc693aa61070132fef0aaae3bae143564373`. The physical `AccessReady` gate still returns `STATUS_DEVICE_NOT_READY`; staging does not enable audio. The exact report is `Windows/Build/audio-wdk-components-current-20261122/report.json`.

Current tool availability: the official cached WDK contains `bin/.../x86/Inf2Cat.exe` (a managed PE, despite the directory name)
and ARM64 `infverif.exe`; SDK contains Windows `signtool.exe`. The available
macOS/Linux environments do not execute these Windows tools natively. No
InfVerif/Inf2Cat run, catalog, signature, package installation or device load has
been verified. A future Windows validation/signing step is still required.
Even a signed package does not remove the existing `AccessReady` hardware
admission failure or establish functioning playback.

Official contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/inf2cat
- https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/running-infverif-from-the-command-line
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-inf2cat-to-create-a-catalog-file
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-

## KS volume units and explicit amplifier ownership

The topology remains control-free: no volume node is advertised without actual
amplifier resources and an established active profile/channel mapping. A KS
volume value uses signed 16.16 dB, not the AW setter's unsigned attenuation.
The AW setter adds attenuation to the selected profile's initial value and
saturates at 720 eighth-dB units. Thus a profile-relative control has maximum
0 dB and minimum `-(720 - min(initial,720))/8` dB; two profiles may have
different ranges. One hardware step equals 8192 KS units. Requests outside the
range must be clamped, and GET must describe the observed setting rather than
merely returning the last request. Maximum finite attenuation is not a proven
mute. No profile activation, reset or power operation is implied.

The explicit resource owner must already hold both verified targets, pin their
parents and exclude remote removal and concurrent register/profile changes for
the whole operation. Software leases alone do not prove remote-target validity.
Board sound-channel indices 0/1 do not establish physical left/right or Windows
speaker-slot assignment. The optional topology publisher supplies property automation and BasicSupport
only when its caller provides that mapping; it never infers resource order.
An all-channel SET is sequential and can partially affect hardware before an
error. Neither automatic retry nor rollback is promised.

Official property and channel contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/ksproperty-audio-volumelevel
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-ksnodeproperty_audio_channel

`T70AwKsVolumeSet/Get` now call the existing resource-owner volume operations,
which bracket the actual SPB profile-volume/read paths with target Begin/End.
SET reports attempted/completed channel masks and the underlying write/End
results separately; its output is unchanged unless the selected operation fully
succeeds. GET reads register 0x05 rather than a software request cache and rejects
noncanonical or out-of-profile values. It is a per-channel helper, not a claim
that aggregate KS GET or the PortCls property handler is implemented.

The host integration test exercises the actual KS, resource, target-lease,
profile-application and volume-update functions together, with synthetic profile
views and fake register I/O. It covers signed extremes, all decoded baselines,
all raw register words, partial pair failure and lease failure. It cannot verify
real target ownership, speaker-slot mapping or physical volume. The real
production sources also compile and link against the official WDK headers;
no hardware was accessed.

## Optional topology volume publication

The optional topology path must be given real, already-owned AW targets, an
immutable active-profile ACF and an explicit Windows-channel-to-board-channel
mapping. Neither channel order nor target identities are discovered here. The
parent owner must retain those inputs and exclude remote removal/profile changes
throughout property calls and teardown. This does not establish amplifier power
or playback readiness. The ordinary production start path still has no such
supplier and remains control-free.

KS handlers receive `Instance` after PortCls removes the node-property header:
Channel plus Reserved, not a second complete KSNODEPROPERTY. BasicSupport must
report the two mapped channel ranges without asserting uniform control. The
finite maximum attenuation is not an actual mute; all-channel SET may partially
apply before an I/O failure. No arbitrary aggregate is fabricated for a GET of
two unequal independent channels.

Official interface contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-property-handlers
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/basic-support-queries-for-audio-properties
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/exposing-multichannel-nodes

`T70AudioEndpointOpenWithVolume` selects the bound topology constructor; the
ordinary `T70AudioEndpointOpen` still selects the control-free constructor. The
optional node handles channel GET/SET and multichannel BasicSupport, including
the KS TOPOLOGY request modifier. GET of channel -1 remains unsupported rather
than inventing an aggregate of independently controlled amplifiers. The object
retains its parent, serializes its own property calls, and denies new property
I/O after Close. This is not a substitute for the binding owner's external
resource/profile lifetime and remote-removal exclusion.

## Temporary notification pause versus permanent removal

The notification pause gate is separate from permanent service Close. It stops
new `IPortWaveCyclic::Notify` submissions while retaining the same service group
and port. A notification already admitted before Pause must return before Pause
reports success. That is not proof that a PortCls DPC or downstream service sink
already queued by Notify has finished. Existing buffers, group members and
references remain owned; the permanent removal drain is not reused for PAUSE.

An owned progress IRQ while paused is deliberately suppressed, not treated as a
broken service owner; the existing stock ACK path still handles the interrupt.
Resume cannot reopen a permanently closed owner. No synthetic elapsed period or
notification is generated to compensate for the pause.

Factory state callbacks pause notifications before preparation and after the
stream's stock STOP; resume is conditional on the external Ready callback. A
successful gate operation does not establish the Windows pause/resume position
contract or clock/route readiness. The production Runtime's unresolved physical
callbacks still fail rather than claiming preserved CUR or successful playback.

Official contracts:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/service-sink-and-service-group-objects
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclicstream-getposition

If the RUN request fails after admission was reopened, the Factory-provided
abort hook pauses submissions again. The original start error and retained DMA
ownership are preserved; abort is not hardware STOP, rollback or downstream
callback completion. A busy in-flight Notify may make Pause return NOT_READY
while the admission bit is already sealed. The caller's fault/cleanup path must
retain storage until actual rundown; this software gate never authorizes free.

## Temporary service-sink callback rundown

Pausing Notify submissions alone does not drain callbacks already queued by
PortCls. The export owner therefore supplies one shared admission/count gate to
all of its registered sink proxies. Pausing that gate suppresses late callback
forwarding and reports NOT_READY while any original `RequestService` call is
still executing. It does not remove members, release their references, spin,
wait for an arbitrary timeout, or permanently close the collection. A member
added while paused uses the same gate and cannot bypass the pause.

This boundary covers calls into the tracked original sinks, not an assertion
that the opaque PortCls DPC queue is empty or that work a sink independently
queued has finished. A queued notification may arrive after Resume and request
service of the then-current stream state. There is no historical period payload
or synthetic catch-up notification. Storage remains owned, and the actual
platform/position policy must approve resumption before either callback or
Notify admission reopens. Permanent quiesce seals the shared gate against
reopening; final removal still performs the separate member removal and COM
rundown. This does not fix the unresolved clock/power or CUR preservation
requirements.

Validation: shared two-sink in-flight rundown, late callbacks, registration
while paused, and permanent seal races pass the ASan/UBSan host suite. Actual
Factory/export integration is exercised; all 93 production translation units
compile against official WDK headers and link as ARM64 MtkAudio.sys. This is
not a Windows load test or a hardware playback/PAUSE-position validation.

## Stock trigger correction: pause-position preservation remains unproved

The actual T70 trigger accepts START and STOP/SUSPEND, not RESUME or
PAUSE_PUSH/RELEASE. The common helper's RESUME support is not the selected
MT6789 ops entry. Stock capability flags also omit PAUSE and RESUME. See
[the corrected source/binary evidence](AFE-DISABLE-EVIDENCE.md#normal-stop-contract-versus-windows-pause-follow-up).
No observed absence of a CUR write proves retention or reset across disable/
enable. Windows PAUSE-to-RUN continuity therefore remains a real provider
obligation, not a fabricated CUR write or buffer relocation.

The stream now distinguishes a completed stock STOP from a pending PAUSE
completion. Exact NOT_READY from callback rundown or CUR observation permits an
explicit repeated PAUSE without issuing STOP again. Once the provider succeeds,
CUR retries do not call it again. Pending RUN/position requests remain rejected;
hard errors or changed ring geometry latch a fault and keep resources owned.
This repairs a reproduced permanent-fault-on-busy-callback bug; it supplies no
missing hardware resume guarantee. The new regression fails against the prior
stream and passes with two actual callback-drain/restart cases plus failure
boundaries. Full ASan/UBSan regression and official-WDK compilation of 93 audio
units/ARM64 linking pass; no device execution was performed.

## Runtime framework parent ownership (2026-11-19)

The actual cyclic StartDevice path creates a KMDF miniport device wrapper
using the verified PortCls FDO/PDO. Its preflight opens the two ACPI Resource
Hub targets, identifies both amplifiers through MtkI2c, then releases those
short-lived targets before the physical readiness check. Failed start, normal
release, unpublished remove and final retirement delete the wrapper before
PortCls can delete its FDO. The optional next-lower device is null because this
wrapper does not send requests down the audio device stack. See Microsoft's
WdfDeviceMiniportCreate contract. Existing physical admission and
profile-selection restrictions remain.

The Mac host AudioDriverTest passes 843 checks with framework creation failure
injection, repeated starts, query/remove and retirement paths, and balanced
framework create/delete counts. This uses host mocks, not the official WDK,
Windows loading, SPB target acquisition or playback validation. The Linux command
route was unavailable; no new official WDK build is claimed.
