# Why decoded ACF records are not yet an executable write plan

This review is about data semantics only. It adds no hardware access, power or
reset operation, automatic enumeration, or guessed board state.

## T70 stock evidence

For the already identified snd-soc-aw882xx.ko (SHA256
`ea99858a0dffa782cf024871793c7b897839c6fdb3020a7344f47a982cd04f0f`),
aw882xx_dev_reg_update at .text 0xf8d8 (size 0xcc) calls aw_dev_reg_fw_update
at 0xf9a4 (size 0x688). At 0xfb44..0xfb50 the loop reads a 16-bit address/value
pair, advancing four bytes at 0xfb38, so source ordering matters. The block
0xfb54..0xfca8 compares device descriptors and performs special handling before
the default register write at 0xfcd4. A write failure stops the record loop but
branches to postprocessing at 0xfa6c; it does not immediately end all processing.
It is not equivalent to streaming every
decoded pair unchanged. At the initial review, specific descriptor-to-AW88261 register associations
had not yet been independently established. The later SYSCTRL review below
resolves that register only; it does not establish a complete apply policy.

## Public AW88261 comparison, not a substitute for stock policy

Awinic's upstream [aw88261_dev_reg_update](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.c)
treats SYSCTRL, SYSCTRL2, I2SCTRL3, DBGCTRL and VSNTM1 specially. It preserves
selected current control state, records volume and efuse-mode information,
changes a transmit setting, skips a calibration register's original value,
and subsequently invokes calibration logic. These observations prove why a
raw list is insufficient; they do not establish identical handling in the
T70 family driver. Its failure behavior must also be checked independently,
not inferred from the public routine's return path.

## Outcome

No safe executable subset or complete special-register policy was established.
A diagnostic inventory could list the existing 57 decoded records in order,
but labeling that inventory a safe application plan would be misleading.
Therefore no apply callback, guessed mask, special-register transformation,
power/reset/calibration sequence or default-permit path was added. Unknown
policy remains unsupported rather than reported as successful.

The existing parser, preserve-mask primitive and read-only chip-ID core remain
usable independently. Next implementation requires evidence for the stock
AW88261 descriptor associations and non-power special-register semantics, with
explicit preconditions supplied by the board owner. No firmware build or device
access occurred. This is a reviewed limitation record, not an implementation
or a new hardware-test result.

## SYSCTRL (0x04): stock and public preservation are not equivalent

A subsequent narrow descriptor review of the same stock binary resolves these
AW88261 associations (32-bit descriptor masks shown below; register values are
16-bit). This is data-policy evidence, not an executable power sequence.

| Descriptor offsets: register / mask / replacement | Values | Initialization evidence |
| --- | --- | --- |
| 0x118 / 0x11c / 0x120 | 4 / 0xfffffffe / 1 | .text 0x12f28..0x12f5c |
| 0x130 / 0x134 / 0x138 | 4 / 0xfffffffd / 2 | .text 0x12f40..0x12f50 |
| 0x148 / 0x14c / 0x150 | 4 / 0xfffffeff / 0x100 | .text 0x12f60..0x12fa8 |
| 0x160 / 0x164 / 0x168 | 4 / 0xffffbfff / 0x4000 | 0x2113 override, .text 0x13310 and 0x13360..0x133a8 |

The register-record loop applies these descriptors at .text 0xfbfc..0xfc08,
0xfba8..0xfbb4, 0xfc18..0xfc24 and 0xfc34..0xfc40 respectively. Each operation
is `record = (record & mask) | replacement`. For register 4 the resulting
16-bit expression is `(profile & 0xbefc) | 0x4103`. These blocks do not read
current SYSCTRL hardware state to preserve those four bits.

By contrast, the public driver preserves the current low two control bits and
mute bit, then sets the additional mute bit. Its constants and
[register-update calculation](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.c)
reduce to `(profile & 0xbefc) | (current & 0x0103) | 0x4000`.
The [header](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.h)
identifies bits 0/1 as power controls and bits 8/14 as mute controls. The two
policies therefore differ whenever the current 0x0103 bits are not all set.

Neither policy proves that every remaining bit in 0xbefc is non-power,
non-reserved, or safe to change independently. Mask complement is not a safety
allowlist. No non-power application plan, SYSCTRL write callback, or state
transition was added. The existing generic mask primitive is not permission to
apply a SYSCTRL profile. Further implementation needs separately verified field
semantics and board preconditions; no startup/shutdown sequence was investigated
or executed for this review. This limitation is deliberate, not successful
speaker initialization. No firmware build or device access occurred.

## Executable independent subset: selected-profile attenuation

`AwSpbProfileVolume` now connects the validated T70 channel/profile selection
and existing stock-derived volume setter to the actual WDF SPB transport. It
requires an already opened, externally pinned/serialized target and a board
provider's verified target/tuple association. It validates the whole ACF and
selected register bin and requires exactly one0x05 record before any I/O. It
reads ID0x00 and requires0x2113, then reads current0x05 and writes only the
verified attenuation calculation, preserving bits15:10. Missing/duplicate
volume records or invalid tuple/profile/attenuation cause no I/O. Read errors
prevent writes; write error/short completion reports an attempted write and
leaves the output unchanged, without retry or rollback fiction.

This is explicitly NOT an ACF activation transaction. The caller must know that
the selected baseline is appropriate for its existing amplifier configuration;
identity alone does not prove that. The function does not write SYSCTRL, reset,
TX-enable, PLL, calibration or any unknown register, or claim mute/power state.
The full register-bin and startup remain unimplemented in this path because
special power/calibration handling cannot be replaced with raw writes.

The T70 descriptor and setter evidence for init+requested clamp/encoding is
recorded in README.md (0x1306c..0x1307c,0x14154/0x14164,720 limit). Current
upstream aw88261_dev_reg_update independently extracts init volume from the
SYSCTRL2 record, but its live setter now takes an already selected control
volume directly. We retain the T70 stock semantics rather than claim that the
current upstream complete startup/setter policy is identical. No GPL source
was copied; the new adapter composes existing independently written components.
https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.c

Tests use actual ACF/board/volume/SPB code with fake WDF completions, covering
both enabled channels and profiles0/10, all upper-six-bit combinations, selected
attenuations, ID/read/write timeout/short/error paths, valid-checksum missing or
duplicate0x05 records, every truncation, invalid tuples, IRQL, overlapping or
misaligned output and overflow. No actual I2C or amplifier state changed.

## Serial input fields applied through SPB, not a startup sequence

`AwSpbProfileSerial` now consumes the existing validated pair-profile serial
view and writes the selected enabled board channel's observed wire settings:
register 0x06 bits11:0 (input channel, I2S mode, data width, BCK/rate codes), then
0x07 bits14:0 (slot mode and TX/RX slot selectors). The opaque upper4/1 bits are
preserved from current hardware. Field definitions are the existing reviewed
AW88261 serial decoder/data-sheet evidence and match the public register header.
The stock ACF record loop treats 06/07 as ordinary ordered writes, unlike the
special 04/05/08/14/50 policies. The API additionally rejects reordered 07/06,
missing/duplicate records and unsupported field codes before any I/O.

After ID0x2113 verification the actual transport performs read06/write06,
read07/write07. Failure stops the sequence and reports attempted/completed writes
and the failing register. It never claims rollback, coherent application across
channels, or complete profile activation. The selected values come from ACF,
not an assumption that S16 DMA samples imply16-bit wire slots.

Caller must already own an accessible chip with serial traffic/playback/TX
excluded, the verified target-to-board association and target lifetime. No
power transition is performed to create that state. No SYSCTRL, PLL, TX enable,
calibration or AFE routing write is added. Full codec/AFE wire compatibility and
playback remain separate; writing slot selectors does not enable transmission.
Public driver and register definitions were used as factual references, not
copied implementation or a replacement for T70 wiring evidence:
https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.h
https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.c

## Explicit Windows resource-to-pair serial application

`AwSpbResources` now consumes two real translated
`CM_PARTIAL_RESOURCE_DESCRIPTOR` connection resources with their owning
`WDFDEVICE`s. It checks SERIAL/I2C, reconstructs the full 64-bit Resource Hub ID,
rejects duplicate IDs and incorrect T70 tuples, then opens both existing
`AwSpbTarget` owners. Failure opening the second target closes the first.
The supplied tuple must match stock `/soc/i2c@11e01000`: sound-channel 0 at
0x34, channel 1 at 0x35. These are not asserted physical left/right labels.

The CM descriptor does **not** encode the controller MMIO/slave address, so a
real board resource supplier still must prove that association. The ID is not
an I2C address, an index, or derived from the ACF. No new ACPI declaration,
automatic enumeration, invented connection resource or DriverEntry publication
was added. Both parents and their real removal exclusion remain caller-owned.

The owner preflights both selected serial profiles (including 06-before-07
order) before any I/O, leases both targets, and invokes the actual existing SPB
serial applicator channel 0 then 1. First error stops; per-channel attempted /
completed writes and transport status are retained. This is not an atomic
hardware transaction. Once application begins, another attempt is rejected;
release closes targets but neither rolls back registers nor certifies retry
safety. Traffic/TX must already be stopped by the real owner. Power, reset,
PLL, calibration and playback-start sequences are unchanged.

Official contracts checked:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_cm_partial_resource_descriptor
- https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/hardware-resources-for-kernel-mode-spb-peripheral-drivers
- https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/simple-peripheral-bus--spb--overview

`SpbResourcesTest` exercises actual resource/target ownership code with fake
WDF and applicator boundaries: both target-create/open failures, reverse
cleanup, invalid connection/tuple/duplicate ID, leases blocking release,
first/second channel failure, exact partial progress, no automatic retry, and
both preflight failures before any application. Existing full applicator tests
exercise actual profile decoding/SPB request generation; added pure-preflight
checks use stock profiles and checksummed malformed input. No live I2C was used.
The test-only CM union extension is isolated in `Tests/spb-resources`; the
concurrently edited shared `Tests/include/ntddk.h` remains untouched.

### I2C1 resource supplier is still absent

Read-only comparison after `c85e806` found the Windows I2C validator already
accepts 0x11e01000/0x1000, APDMA 0x10217100/0x80, GSI177, but current board DSDT
only declares I2T0 (I2C0) and I2C6. Its I2C handoff supplies ICL0/ICL6 and two
ready bits, not an I2C1 clock/readiness instance. `MtkI2c::D0Entry` requires
IRDY==1 and a valid live IRCK result before initializing the controller.
`Audio.asl` currently declares only AFE MMIO/IRQ, no amplifier connections.
Thus an I2cSerialBus ResourceSource targeting an invented I2C1 node would not
create a working provider. No readiness constant, fake connection ID, or clock/
power resource implementation was added. DSDT and other workers' files were
not modified. This missing supplier blocks real `AwSpbResourcesAcquire` input;
compiling that owner does not mean Windows has supplied the two targets.
