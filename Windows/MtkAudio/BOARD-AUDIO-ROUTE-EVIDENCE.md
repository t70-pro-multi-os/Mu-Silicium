# Board audio route: established endpoints, unresolved runtime routing

Read-only review of stock files, not hardware execution. No route/register write
or startup sequence is introduced by this document.

## Identified inputs

- `mt6789-mt6366.ko` SHA256
  `72257b21c2b559dd85a58848fe64f46cdf81ab26e6e9f70519c1c33fa5ae8e92`
- `snd-soc-mt6366.ko` SHA256
  `45707a9123f13792057766577b395ada6f125b66096783e183d15383146ba2f4`
- AFE/common module and effective DTB hashes are recorded in the existing AFE
  geometry evidence. The machine module is a separate source of board links,
  not proof that every declared route is selected at runtime.

## Exact board and link facts

Effective DTB `/sound` uses `mediatek,mt6789-mt6366-sound` and contains
`mtk_spk_i2s_out = 3`, `mtk_spk_i2s_in = 0`, `mediatek,spk-i2s = <3 0>`.
Its `mediatek,speaker-codec/sound-dai = <0x1ec 0x1ed>` resolves in that same
DTB to I2C1 `aw882xx@34` and `aw882xx@35` respectively. Both are enabled;
their sound-channel values are 0 and 1. This is not a physical left/right claim.

Machine-module relocation records establish:

| Symbol | .data offset | Component strings |
| --- | --- | --- |
| i2s0_cpus | 0x808 | I2S0 |
| i2s3_cpus | 0x8a8 | I2S3 |
| i2s0_codecs | 0x828 | aw882xx_smartpa.1-0034 / aw882xx-aif-1-34; aw882xx_smartpa.1-0035 / aw882xx-aif-1-35 |
| i2s3_codecs | 0x8c8 | same two AW component/DAI names |

Thus board properties and static links support speaker I2S3 output / I2S0
input intent and two AW targets. They do not prove a runtime DL1-to-I2S3 mixer
selection, slot mapping, or that audio passes serially through MT6366 first.
Do not invent a `DL1 -> MT6366 -> AW` serial chain from the machine module name.

`mt6789_i2s_hw_params_fixup` (.text 0x430, size 0x78) clears format-mask bits
0..52 and sets bit 10 at params+0x24. This is a backend format constraint,
not evidence that the DL1 frontend's accepted storage format can be reused
unchanged on the wire. `mt6789_mt6366_i2s_hw_params` (.text 0x4ac, size 0x38)
loads params+0x128, shifts it left seven, and calls `snd_soc_dai_set_sysclk`
with id=0 and direction=1. No callback implementation is derived merely from
these ABI offsets; the complete slot/word-width/clock-source contract is needed.

## Why no routing callback was added

The machine's `mt6789_mt6366_routes` (.rodata 0x250, size 0x120) includes
`Ext_Speaker_Amp` with sources `LINEOUT L`, `Headphone L Ext Spk Amp`, and
`Headphone R Ext Spk Amp`. These declarations coexist with the digital AW links;
they are not evidence that all paths are enabled or serially connected.
No event or power handler was investigated or executed in this review.

AFE symbols identify the remaining non-power routing evidence to resolve:
`mt6789_memif_routes` (0x20e8), `mtk_dai_i2s_routes` (0xc7d0),
`mtk_i2s3_ch1_mix` (0x11e40), `mtk_i2s3_ch2_mix` (0x12250).
The follow-up below resolves the named mixer path, but runtime selection is
still unverified. A real implementation still needs DL1 channel-to-
I2S slot mapping, backend serial-format/word-width agreement, and verified
codec/amp interface expectations. Static alternatives alone cannot select those
values. Existing DL1 format validation and AW profile/probe callbacks establish
neither this route nor its running state. No guessed route, synthetic success,
new playback code or host-test claim is added.

## Follow-up: stock userspace explicitly names DL1 to I2S3

The existing vendor extraction under
`~/.tmp/teclast-t70pro/device-backup/drm-analysis/vendor_a/etc/` contains
`audio_device.xml`, not a file named mixer_paths. Its SHA256 is
`9ce57c22efe3d75e943894ddb88bc80f607728bd22617ca58726595f0f3b4ff3`.
Card name is `mt6789mt6366`. Lines 296..302 declare:

- `PLAYBACK1_TO_I2S3` turnon: `I2S3_CH1 DL1_CH1` = 1, then
  `I2S3_CH2 DL1_CH2` = 1.
- The corresponding turnoff lists the same controls in that order with value 0.

This resolves the previously missing *named control path*: DL1 channel 1/2 to
I2S3 channel 1/2 is explicitly available in stock userspace configuration.
It does not establish which runtime stream selected the path or serial wire
slots/width, nor guarantee that XML order equals raw register-write order.

`audio_policy_configuration.xml` SHA256
`c4c919fc5f11e4155aa6444510c345968b430c9e4d172c71aab571a12a41928c`
declares the logical Speaker sink with primary/deep-buffer/fast and other sources.
That policy is not a physical register/slot map. `audio_param/SmartPa_AudioParam.xml`
selects the AW882xx parameter entry through param_id 20 and records i2s_set_stage
5; no undocumented stage-number semantics are inferred here.

The AFE module corroborates the exact mixer names: the first entries of
`mtk_i2s3_ch1_mix` and `mtk_i2s3_ch2_mix` name `DL1_CH1` and `DL1_CH2`.
Their private-value relocations point to .data 0x60b8 and 0x6668 respectively.
Raw first eight 32-bit words are:

- channel 1: `0, 1, 0, 0x20, 0x20, 5, 5, 0`
- channel 2: `0, 1, 0, 0x24, 0x24, 6, 6, 0`

These are descriptor bytes, not a new authorized write plan. Most importantly,
both entries' put callback relocation (entry +0x38) is
`snd_soc_dapm_put_volsw`, not a simple direct register update callback.
No DAPM power/event handler was investigated or executed. The review does not
establish a power-independent equivalent write sequence, synchronization or
rollback contract. Replacing that control path with direct writes based only on
the descriptor would discard unverified framework behavior. Therefore no routing
callback or register writes were added. Remaining gaps are now the control-to-
register lifecycle boundary and serial slot/format contract, not absence of a
stock named DL1/I2S3 path. This is evidence-only; no new test execution is claimed.

## Backend serial-format cross-check: stock callbacks do not prove encoding

The stock AW module `aw882xx_set_fmt` (.text 0x18dc, size 0x4c) logs the supplied
format and returns zero. It contains no format validation, register write, or
stored encoding. `aw882xx_hw_params` (.text 0x1c10, size 0x160) obtains component
context, extracts/logs rate and format width, and returns zero. These observed
callback bodies therefore cannot demonstrate that a requested serial format was
actually configured or accepted by the hardware. No matching stock
`set_tdm_slot` symbol was found in this module's symbol table; this is a limited
symbol observation, not proof that slots cannot be configured elsewhere.

The machine I2S3 CPU/codec references occur at .data 0x27d8/0x27e8 and resolve
to i2s3_cpus/i2s3_codecs. The previously documented machine hw_params calls
`snd_soc_dai_set_sysclk` on the first CPU DAI with id 0, frequency equal to the
32-bit value at params+0x128 shifted left 7, direction 1. That is not an AW
sysclk call, and it does not establish the AW slot assignment. The exact
machine-link dai_fmt field and its stock struct-layout binding remain unresolved
in this review; no guessed struct offset is promoted to a serial-format enum.

For comparison, the public driver inspected on 2026-11-18 is:
https://raw.githubusercontent.com/torvalds/linux/master/sound/soc/codecs/aw88261.c
Its `aw88261_set_fmt` explicitly records polarity and format selections;
`aw88261_hw_params` separately derives rate, logical width and physical-width
values; `aw88261_set_tdm_slot` maintains slot selections. This is materially
richer than the stock callback bodies above. Public support for a format is not
proof that this board's stock profiles, machine link and two amplifiers use it.
Public slot defaults must not be substituted for the unresolved board settings.

No common, board-proven serial-format/slot encoding was established, so no new
OS-neutral encoding function is added. Existing frontend S16/S24 validation
remains separate from backend transport format. Needed evidence is the exact
machine-link format field plus the stock ACF serial fields and their per-target
slot interpretation. No clock setter, power code, actual register operation or
new host test was executed for this evidence-only update.

## Four board-channel ACF profiles: serial-register inventory

Read-only extraction from the established stock ACF (SHA256
`4bb530416e53d20bdb4e42fd63a77e04c7d1aa26ced0c24404752bf967385bd5`).
Each selected type-4 bin is 292 bytes. Offsets below are relative to the inner
bin start and address/value words are little-endian as in `AwRegisters`:

| Channel/profile | 0x06 at bin+0x4c | 0x07 at +0x50 | 0x08 at +0x54 | 0x13 at +0x80 |
| --- | --- | --- | --- | --- |
| 0 / 0 | 0x04e8 | 0x0010 | 0x0076 | 0x2102 |
| 0 / 10 | 0x04e8 | 0x0010 | 0x0076 | 0x2102 |
| 1 / 0 | 0x08e8 | 0x0010 | 0x0077 | 0x2102 |
| 1 / 10 | 0x38e8 | 0x0010 | 0x0077 | 0x2102 |

The public AW88261 register header names 0x06/07/08 as I2SCTRL1/2/3 and
0x13 as I2SCFG1:
https://github.com/torvalds/linux/blob/master/sound/soc/codecs/aw88261.h
The neighboring 0x12 record (0x002b in all four) is PWMCTRL2, not a serial
register; it is deliberately excluded from the serial inventory.

This proves raw profile differences, not their slot interpretation. In
particular channel 1 profile 10 differs from profile 0 in register 0x06, so
channel identity alone cannot be used to derive an entire serial register image.
Equal 0x07 and 0x13 values do not establish identical effective wire configuration.
The known stock 0x08 bit-6 TX field is already handled by AwTxField/AwTxUpdate;
all four raw values set that bit, but the stock special loader clears it as
previously documented. A raw ACF bit therefore is not necessarily live state.
The differing 0x08 bit 0 is NOT assigned a channel/slot meaning here.

No new serial bit-field has been corroborated against both this stock driver's
map/consumer and matching public definitions. The public driver and header views
retrieved in this review do not provide a matched, revision-pinned definition
set for the newer slot macros. No widths or slot masks are guessed from their
names. The next evidence needed is a version-matched register field definition
and its stock consumer for 0x06/07/08/13, plus the machine-link serial format.
Consequently no new calculation/validation API or hardware operation is added;
the existing proven TX calculation remains unchanged. This inventory is not a
profile-application plan or a declaration of playback compatibility.

## Policy formats are not a DL1-to-profile compatibility proof

The extracted `audio_policy_configuration.xml` SHA256 is
`c4c919fc5f11e4155aa6444510c345968b430c9e4d172c71aab571a12a41928c`.
In its primary module, `primary output`, `deep_buffer` and `fast` each advertise
PCM_32_BIT and PCM_16_BIT at 44100/48000 Hz, stereo. The `Speaker` sink has the
same declarations. Its sources include those three ports, FM Tuner In, voip_rx
and Voice Call In. Notably voip_rx advertises 16000/48000 Hz while Speaker
advertises 44100/48000: an Android policy edge cannot be treated as a promise
of identical, unconverted backend parameters. The XML does not bind these
profile declarations to an AW ACF profile ID or a specific DL1 DMA format.

The separate `audio_device.xml` PLAYBACK1_TO_I2S3 controls prove the named
DL1_CH1/CH2 route described above, but do not attach sample rates, sample word
format or an ACF selector to that path. The machine module was rechecked:
`mt6789_i2s_hw_params_fixup` at 0x430 clears mask indices 0..52 at params+0x24
and sets index 10. It does not set rate/channel fields in this function.
This backend fixup is not evidence that frontend policy PCM_32_BIT should be
accepted as DL1 S32; the existing DL1 core deliberately rejects unsupported S32.
AW set_fmt/hw_params logging-only callbacks and unresolved profile serial fields
remain the previously documented missing backend evidence.

S16 stereo at 44100/48000 is a numerical intersection of the policy declarations
and current DL1 validation, NOT a demonstrated compatibility set for either
stereo ACF profile. Likewise lack of policy S24 here does not prove the hardware
cannot use the already supported DL1 S24 format. A cross-compatibility predicate
would invent the missing HAL/PCM/backend/profile mapping and either falsely
approve playback or incorrectly reject configurations. No such API or new test
claim is added. Required evidence remains the actual frontend-to-backend
conversion/selection and version-matched serial-field interpretation for both
board profile IDs. No hardware operations or routing changes were performed.

## Read-only DL1/I2S3 selection observation

`T70AfeObserveDl1I2s3` accepts an explicit read callback and returns both raw
connection registers and their two DL1 selection bits only after both reads
succeed. It never writes routing, replaces DAPM, enables clocks or starts DMA.
Other mixer sources can remain selected; two set bits are not playback proof.
Caller must already own an accessible mapping and protect its lifetime. Read
order is a diagnostic choice, not a claimed stock startup sequence or atomicity.

Pinned same-SoC public evidence (different board, GPL-2.0):
- [I2S mixer definitions](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/d26d588883468251678619757cc9f0f6a5f37cb8/sound/soc/mediatek/mt6789/mt6789-dai-i2s.c): I2S3 channel 1/2 DL1 controls use CONN0/CONN1.
- [Interconnection definitions](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/d26d588883468251678619757cc9f0f6a5f37cb8/sound/soc/mediatek/mt6789/mt6789-interconnection.h): DL1 channel bits 5/6.
- [Register definitions](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/d26d588883468251678619757cc9f0f6a5f37cb8/sound/soc/mediatek/mt6789/mt6789-reg.h): CONN0/CONN1 AFE-relative offsets 0x20/0x24.

These values match the T70 stock descriptor bytes at .data 0x60b8/0x6668
re-read above; the T70 XML, not the Xiaomi board, supplies the named board path.
This is an independently written callback observer using corroborated hardware
facts. No GPL implementation, tables, headers or DAPM code were copied into the
BSD-licensed component; the linked source retains its GPL-2.0 license. This
observer does not resolve the still-unknown serial/profile compatibility.

The host test verifies all low-16-bit values and complements, high walking-bit
patterns with all four selected-bit combinations, exact read order, both read
failures with positive/negative statuses, NULL inputs and whole-output
preservation. 1,312,052 checks passed under ASan/UBSan and warning-as-error.
Only mock reads were executed; this is not WDK compilation or hardware testing.

## Narrow serial-field decoding from vendor documentation

The vendor-hosted [AW88261 V1.5 PDF](https://doc.awinic.com/doc/20230609wm/c815e155-60b7-40fb-889b-676b02e67d0a.pdf),
pages 35–37, resolves selected 0x06/07/08 fields. Applied to the four previously
inventoried board profiles, these describe standard I2S, 32-bit words, 64fs BCK,
48 kHz code, legacy mode, RX slots left=0/right=1 and TX slot=0. Board channel
0 selects digital left input/TX; channel 1 selects right. These are digital
selections, not proof of physical speaker orientation or applied hardware state.
The 0x38e8 variant changes other bits, not these decoded input fields.
Register 0x13 remains opaque; the PDF does not supply its required field map.
No complete serial compatibility or startup claim follows.

The PDF carries a confidentiality watermark and copyright notice. It and its
tables are not redistributed. The independently written decoder uses limited
hardware facts, not copied prose, tables or driver code. Vendor revision and
stock configuration are recorded rather than claiming a verified silicon stepping.
The current vendor product page lists a newer revision; this decoder's evidence
is specifically V1.5, not an assertion that all revisions are identical.

The pinned [upstream register header](https://code.googlesource.com/linux/torvalds/linux/+/f7fe9f7073602958d6b63cc58a144094533377fa/sound/soc/codecs/aw88261.h)
independently names 0x06/07/08 as I2SCTRL1/2/3 and 0x13 as I2SCFG1.
Its GPL implementation is not copied. AwSerialDecode exposes raw words and
selected fields only; unexamined bits remain unvalidated. Reserved or undocumented
selected encodings reject without modifying output. Rate/BCK/slot values remain
codes in the API, avoiding unsupported general timing inference. The existing
ACF decoder supplies four real board-profile fixtures; tests exhaust each
16-bit input word, error output preservation and the observed profile mapping.
No hardware callback, write, power, I2C or MMIO operation is involved.

### Paired serial configuration observation

`T70AwPairSerialObserve` connects the strict pair-profile view to serial field
 decoding. It requires exactly one record each for 0x06/07/08/13 in each selected
channel, rejects missing/duplicate records and unsupported decoded fields, and
publishes both channels only after all checks pass. It copies values rather than
retaining ACF views. Input must remain immutable during the call; output storage
must be disjoint. Profile 0 and 10 on both stock channels decode as standard I2S,
32-bit words; digital input/TX channel codes differ by channel. These are stored
configuration observations, not physical speaker orientation, live register
state, AFE compatibility, or playback readiness. Reg13 remains opaque.

`PairSerialTest` covers all four channel/profile combinations, every truncated
ACF length, checksum-repaired missing/duplicate/unsupported selected records,
corrupt inner bins, invalid tuples, overlapping output, and copied-value lifetime.
No hardware access or WDK execution is involved.

### Explicit Windows SPB pair observation

`T70AwSpbProbePair` connects two caller-owned, already opened WDF I/O targets to
`T70Aw88261PairProbe` via the actual `T70AwSpbReadRegister` adapter. It rejects
NULL/reused handles and non-PASSIVE calls before reads; board tuples are checked
by the shared pair core. It stops at the first failed ID/status read and retains
the failing channel, stage, and transport status, publishing neither channel on
failure. Target-to-connection association, both target lifetimes, removal
exclusion, and whole-call serialization remain the actual owner's responsibility.
No automatic discovery, target creation, or hardware availability is asserted.

The focused host test replaces the SPB read function and checks ordered target/
register dispatch, all four failure locations, wrong IDs, output preservation,
and preflight rejection. It does not test WDF removal races or actual bus I/O.
Separately, AwSpb.c and AwSpbPair.c compiled to ARM64 COFF with Linux clang-cl
21.1.8, official Microsoft SDK/WDK NuGet 10.0.28000.2526, KMDF 1.35 and a
case-insensitive VFS overlay. No Tests/include mocks were in those include traces.
The official headers emitted 51 clang compatibility warnings for AwSpb.c;
there were no project-source warnings. This is single-translation-unit type and
object compilation, not MSBuild/WDK linking, signing, driver loading or playback.
Packages and their notices remain complete in the private builder cache; no
Microsoft header implementation is copied into this repository.

### Explicit SPB target ownership

`AwSpbTarget` now accepts an existing WDF device and externally assigned 64-bit
connection ID. It validates the declared board tuple but cannot verify that the
opaque ID belongs to that tuple. It creates the official Resource Hub path,
creates a remote target, opens it exclusively for read/write, and publishes the
handle only after success. Open failure deletes the created object. The caller
must supply actual assigned resources: this code does not invent a connection ID
or automatically discover any device.

Begin/End leases reject overflow/underflow; Release refuses active leases and
otherwise closes/deletes once. The owner, parent-device lifetime, all methods,
and target operations require external serialization/removal exclusion. Parent
teardown must stop admission, finish synchronous users and Release before parent
deletion. This is not an asynchronous cancellation, internal lock or automatic
removal-management implementation. No requests may remain outstanding at release.
Leases block only this owner's Release, not WDF's default remote-target removal
handling. Query-remove/remove-complete/reopen callbacks are not implemented.
The external provider must separately exclude remote removal throughout use;
Begin is not an open-state check. Without that guarantee this owner cannot
satisfy the paired adapter's pinning precondition. An opened target is not proof
of power, identity, or playback readiness.

The focused test uses fake WDF/resource-hub methods for path/create/open errors,
cleanup and lease boundaries. The actual file also compiled with official WDK /
SDK headers to ARM64 COFF, with no mock include paths. That compile caught the
required `RESHUB_USE_HELPER_ROUTINES` declaration opt-in, which is now included.
No real connection was opened; no driver link or device test was run.

Sources: [SPB connection IDs](https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/connection-ids-for-spb-connected-peripheral-devices),
[WdfIoTargetCreate](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetcreate),
[WdfIoTargetOpen](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetopen),
and [WdfIoTargetClose](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetclose).

## I2S3 initialization boundary recheck

The pinned MT6789 I2S source still exposes DL1 mixer selection through
SOC_DAPM_SINGLE_AUTODISABLE, not a standalone initialization transaction.
Its I2S3 route declarations connect the output to enable, HD and MCLK/APLL
supply widgets. This matches the previously recovered T70 mixer descriptor's
`snd_soc_dapm_put_volsw` callback and the stock XML's named controls; it does
not prove a raw MMIO replacement order or ownership/rollback behavior.

Consequently, this pass adds no I2S3 clock or routing writes. Treating XML
turnon order as direct register-write order would bypass an unverified lifecycle
and overlap the expressly excluded power-resource work. No power event handler
or clock implementation was further investigated, executed or reconstructed.
Existing read-only route observation remains available; set selection bits do
not certify clock readiness, exclusive routing, serial compatibility or audio.

Needed before a production initialization path can be authored: an authorized,
verified lifecycle provider for the actual board supplies, explicit exclusivity
against other I2S users, and a proven control-to-register sequencing/error contract.
These are not satisfied by the existing AFE mapping or DMA adapter owners.
No production code changed, so no new compile/regression success is claimed for
this evidence-only recheck.
