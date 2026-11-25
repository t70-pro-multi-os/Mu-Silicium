# MediaTek STP common transport

Portable C reconstructed from selected bodies in the shipped `wmt_drv.ko`.
This carries opaque channel bytes; it does not calculate or publish positions.

- Mandatory modes 2 (UART), 8 (BTIF), 16 (SDIO) are supported explicitly.
  These constants do not establish which mode this board has selected.
  Fullset modes 1/4 and their ARQ are rejected.
- Physical TX receives `80 channel/length length 00 payload 00 00` exactly.
  The backend owns SDIO TX alignment. The decoder preserves partial header,
  payload, tail and SDIO RX padding, and returns one frame with an exact cursor.
- TX payload is 1..1996 bytes; RX payload is 0..1999 bytes. The stock GPS
  character layer's 16KiB copy limit does not allow a 16KiB STP command.
  Do not split opaque receiver commands without their protocol definition.
- Channel 2 is the primary GPS byte stream. The 16384-byte ring reserves one
  slot, preserving bytes across frame/read boundaries. Consumed/reset bytes
  are wiped. Overflow wipes/faults instead of silently losing a position fragment.
- Reset increments the queue epoch; tag asynchronous reads before starting them
  and reject callbacks from an old epoch. Drain/reset the physical stream and
  reset the decoder under the same owner. An epoch cannot identify old wire data.
- Channel 4 carries WMT control. Function-control opcode 6 sends
  `01 06 02 00 function enable` and requires five actual ACK bytes
  `02 06 01 00 status`. Only status zero confirms this command.
  This does not implement full function-on, conninfra, GPS pre-control or LNA.
- `MtkWmtRegisterBegin` sends the stock 20-byte opcode-8 command, preserving
  `01 00 01` at bytes 5..7, address/value/mask as little-endian words and the
  original input value even for reads. Addresses belong to the connectivity
  firmware; they are not host MMIO mappings. Reads require exactly 16 response
  bytes, writes 8, with event type 2, opcode 8, matching declared length and
  status zero. Reads also require the requested echoed address before publishing
  `RegisterValue`. Response bytes 5..7 remain opaque. Write ACK is not readback.
- `MtkWmtGpsSyncBegin` implements pin 3/state 6 or 7 of the original SoC
  pin-control branch: masked write to firmware address `0x80050078`, mask
  `0x70000000`, on value `0x10000000`, off value `0x50000000`. Require bit 7 of
  initialized SoC operations byte 9. Static stock flags are zero; this board's
  runtime flag is not established. Without the flag, return Unsupported and
  send nothing. Do not treat that as completed GPS preparation. Propagate WMT
  rejection/ambiguity; stock pin-control swallows worker failures. Remaining
  host sync/LNA/coexistence/firmware-on steps are not supplied by this command.
- GPS MCU opcode `0x32` sends `01 32 lenLo lenHi body` on the same control
  owner. Requests are 1..1992 bytes; replies are 0..1995 bytes, additionally
  bounded by the caller's requested capacity. Validate response type/opcode
  and exact declared/actual size before publishing the opaque reply. Generic
  MCU `Confirmed` means a complete transport reply, not semantic success.
- `MtkWmtGpsPowerSaveBegin` implements the stock `{Suspend ? 4 : 5, Control}`
  request with a three-byte reply capacity. Require at least two actual body
  bytes and status zero. The control byte and response details remain opaque;
  status 6 remains a failure, without the original global-state reconciliation.
  This ACK does not establish whole-device suspend/resume or RF ownership.
- Serialize all objects, function control, register and MCU requests through one transport owner.
  Replies have no function/transaction ID. A duplicate arriving during a later
  pending command cannot be distinguished, so the backend must provide exactly
  once, ordered responses. Timeouts, failed/partial TX and unowned replies
  require external recovery; retrying an ambiguous command is unsafe.
- The owner supplies bounded synchronous writes, event dispatch, timers and OS
  wait/cancellation. Call `MtkWmtControlExpire` on the owner's timeout. Queue
  reads never block. Frame data is copied before the decoder is reused.

Intentional differences from stock: reject reserved/nonzero mandatory header or
trailer fields; latch malformed streams rather than searching payload for sync;
require the actual fifth WMT reply byte; invalidate an overflowed GPS stream;
wipe consumed queue bytes; reject malformed/oversized MCU replies and missing
power-save status/detail; validate register reply type/opcode/declared size/status
and read address before publishing a value. Original register writes check only
actual length; reads also check address, and pin-control hides worker errors.
Original ACK code accepts a missing zero status byte
from its zero-initialized receive buffer. Original MCU code checks only opcode and may truncate or copy bytes beyond the
actual reply; its GPS power-save caller accepts missing status/detail from a
cleared buffer. No NMEA or fitted L5 support is inferred.

## BTIF FIFO and wake signal

`MtkBtifCore.c` is a working `MTK_STP_WRITE` backend and a bounded FIFO/IRQ
engine recovered from `btif_drv.ko` (SHA-256
`1570949e96c7d9495be844a05f50925088b60bfbd9054d40867f7c4885233bbc`).
The stock effective DT supplies controller reg 0 at `0x1100c000/0x1000` and
two DMA reg windows at `0x10217b00/0x80` and `0x10217b80/0x80`. The IO provider
maps those three entries in their original order; the core only reads the DMA
enable bits and never programs a DMA address or channel. DT clocks `btifc`
and `apdmac`, interrupt routing and peer/conninfra ownership need OS providers.

- `MtkBtifInit` touches software only. `Start` requires exclusive ownership,
  clocked mappings, inactive DMA, masked controller IRQs and empty RX/TX before
  the original 13 initialization writes/FIFO reset pulses. Stock trigger
  defaults are 8/1. Stable control writes are checked by reading the device;
  the aliased IIR/FCR reset pulses cannot be checked by reading them back.
- The 4096-byte TX queue accepts a whole encoded mandatory frame or none.
  `MtkBtifStpWrite` enables the physical TX interrupt after queue publication.
  `Service` writes one 8/16-byte batch, using byte accesses and DSB SY after
  each write. Successful queue acceptance is neither delivery nor a firmware
  ACK. The old WMT BTIF wrapper rejects more than 2000 bytes; this direct FIFO
  backend supports the entire 2002-byte STP frame, also accepted and drained
  by the original `hal_btif_send_data`/IRQ bodies in the virtual check.
- `Service` copies at most the caller's 1..256-byte RX buffer each pass. It
  reports remaining work without consuming beyond that buffer. Dispatch its
  successful bytes, in order, to `MtkStpReceive`; then route complete WMT
  replies to the control owner and GPS channel 2 to the receiver queue.
- `MtkBtifRaiseWake` clears `0x64` bit 0, waits 128..160 microseconds through
  the owner's bounded timer, then sets it. Preserve the first inherited value
  until Stop. This requests a wake signal; it does not prove firmware readiness
  or RF/position engine power-on. The owner supplies a timer that actually
  waits at least the requested minimum.
- FIFO byte IO failures latch ambiguity and prohibit replay. Queue pressure
  accepts no prefix. Faults retain the baseline and lease. Stop never flushes
  pending bytes, rejects active DMA or unfinished/unread FIFO data, and retries
  failed control/wake restoration. An uncertain FIFO needs external stream and
  hardware recovery. A peer must be quiesced before Start or Stop.

The OS owns one serialized dispatcher, clocks/mappings, IRQ registration and
work scheduling. This library is not a WDF or Linux device driver. It supplies
neither DMA transfers nor WMT/conninfra firmware boot, full reset recovery,
coexistence or RF power resources. It does not implement the HCI/positioning
payload carried over STP. No automatic device or service consumer is installed.

## T70 stock Bluetooth/Wi-Fi function IDs

The stock T70 ARM64 `bt_drv_connac1x.ko` (SHA-256
`f64d70ae7dd1351d7370683fb69f76860db0bb81abe431aca3804b744b86b73c`)
passes `w0=0` to `mtk_wcn_wmt_func_on` in `BT_open` (call relocation
at `0x1780`) and to `mtk_wcn_wmt_func_off` in `BT_close` (`0x1c94`).
The exact `wmt_drv.ko` (SHA-256
`d395f5249bdeb2b5f6134f70c64e156bd682ba895a6da2d4c2544836a27b91e3`)
passes function zero to the Bluetooth command encoder and sends
`01 06 02 00 00 01` for enable, `01 06 02 00 00 00` for disable.
This matches the function ID and payload used by Windows BTIF's full-mode
control encoder, not proof of controller or radio readiness.

The stock `wmt_chrdev_wifi.ko` (SHA-256
`ae02b4c7cbf92798b38e8cf3e98ff4cbc9a39744b8fab4c356b9e1b7bfceb9d2`)
passes `w0=3` to the exported WMT Wi-Fi on/off entry points (for example
`WIFI_write` at `0xd50` and `0xb5c`). This verifies the stock function
selector only. It does not derive the WLAN data path or establish that the
Bluetooth command sequence applies to Wi-Fi.

### Windows WLAN HIF, beyond CONN bootstrap

The exact `wlan_drv_gen4m_6789.ko` (SHA-256
`50e3b842dfb944e7d34fe786afa96dc091f8ab0d719159d8d30bf17476a452c3`)
registers an AXI/platform driver, not PCI, USB or SDIO. The `mtk_axi_probe`
path resolves the `mediatek,wifi` node and calls `of_address_to_resource` /
`of_iomap` with the DT `remap-idx` or the initialized default zero. The
effective T70 DTB (SHA-256
`a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391`)
has one window, `0x18000000/0x100000`, and SPI 409, level-high; `glBusSetIrq`
maps interrupt index zero before `request_threaded_irq`. This matches the
single existing WCN0 interrupt resource, GSIV 441.

The module's `mt66xx_driver_data_soc2_1x1` relocates through
`mt66xx_chip_info_soc2_1x1` to `soc2_1x1_bus_info`. Its field at `+0x60`
is 36; `mtk_axi_probe` derives `DMA_BIT_MASK(36)` for streaming and coherent
DMA. `halWpdmaAllocRing` dispatches real TX and RX ring allocation. The static
DTB does not declare a `memory-region` for this node; that does not prove what
the runtime tree provides. Firmware staging EMI and host packet DMA are
different contracts.

Windows `MtkConn` already owns the aperture and firmware bootstrap.
`MapHardware` currently consumes only memory descriptors; there is no WLAN
interrupt owner, WPDMA ring lifecycle or NDIS network client in that package.
Continue through the existing owner, consuming the existing translated
Windows interrupt descriptor once and using Windows DMA mappings. Do not
add a second overlapping ACPI aperture or treat successful ROM handshake
as packet transfer. This is static source/binary evidence only.

## Verification

`Platforms/Teclast/t70proPkg/Tests/verify-stp-binary.py` holds the existing build
lock, runs ASan/UBSan, compares selected original ARM64 functions with virtual
OS/physical services, builds Linux AArch64 ELF and Windows ARM64 COFF libraries,
and executes the same STP and BTIF test suites in ARM64 PEs under Unicorn.
The selected BTIF initialization, queued TX, IRQ and wake bodies execute their
original MMIO instructions against virtual registers/FIFOs; kernel kfifo, locks,
logging and sleep are virtual. RX budgets, full duplex, queue wrap/pressure,
failed/ignored control writes, ambiguous TX, wake delay failure and retryable
baseline restoration are checked in the production C suite. Register/sync
commands run through the STP/BTIF queues and fragmented reply dispatch too.
The verifier executes the original register worker and pin-control branch for
2,832 register and 514 GPS sync comparisons, including intentional stricter
validation. Synthetic flags do not establish this board's runtime support. It requires the
existing Unicorn Python package and `T70_MALI_ZIG` pointing to the existing Zig
binary. It never downloads tools or accesses a device. Temporary objects are
removed after each run. Current results and source/artifact hashes are in
`analysis/gnss-stp-verification.json` at the project root.

The existing native runner also supports
`verify-mediatek.py --tests MtkStpCoreTest MtkBtifCoreTest MtkVcn28CoreTest MtkGnssCoreTest`.
Generated libraries and the ARM64 test executable are in `Windows/Build`.
The MediaTek package registers the EDK2 library class; no GNSS DXE consumer,
Linux receiver module/service or Windows GNSS/location driver is linked yet.
BTIF board resources and OS integration, DMA, firmware/power/coexistence,
position engine and whole-device suspend/resume remain implementation work. All tests are offline; no radio or GPS open,
ioctl, location collection or tablet operation executes.

The existing GPIO core (`Windows/MtkGpio/Hardware.c`) now provides
`MgGpsLnaAcquire`/`MgGpsLnaSet` for the exact DT GPIO25/26 states: mode 0
output-low, mode 2 DSP control, mode 0 output-high. It captures/restores inherited
pin state, retains ownership after failed MMIO and rejects generic GPIO access
or suspend while borrowed. The RF owner must serialize the core and quiesce GPS
before releasing it. The Windows GpioClx driver publishes the typed LNA query interface described
in `Windows/README.md`; a GNSS consumer and Linux receiver service still need
to acquire/use it. The common STP library does not call it or enable RF.

## GPS/FM VCN28 power

`MtkVcn28Core.c` implements the MT6358 PMIC sequence behind stock PALDO selectors
2 (FM) and 3 (GPS), including the exact MT6789 operations-table callback at
`+0x88`. The stock runtime ADIE probe bypasses VCN28 for ID `0x6635`; the core
requires the owner's verified probe result and does no PMIC IO for that ID.
The virtual test ID `0x6631` does not identify this tablet's fitted ADIE.

The shipped regulator descriptor confirms CON0 `0x1d88` enable bit 0 and CON1
`0x1d96` readiness bit 15. Original WMT writes OP_EN `0x1d8a=3` followed by
OP_CFG `0x1d90=0`, then requests the fixed 2.8V regulator. The register names
also match the [MediaTek MT6358 patch](https://lists.openwall.net/linux-kernel/2019/02/07/201).
This board's exact DT marks `ldo-vcn28` always-on with a 270us enable delay.
PWRAP/regmap access and exclusive ownership come from the existing OS PMIC
provider; the library does not create a second PWRAP transport.

One serialized RF arbiter acquires independent GPS/FM bits before pre-control
and function-on. It releases a bit only after confirmed function-off and
quiesced firmware/data/LNA work. Other owners keep the shared configuration.
The first acquire snapshots the inherited enable/OP_EN/OP_CFG. The last release
restores inherited OP_EN/OP_CFG and preserves all enable bits plus the DT's
always-on enable bit, even if acquisition found the supply off. Stable writes
are checked by reading them back; readiness waits are bounded. Failed/ignored
IO retains the snapshot and leases, blocks new acquisition and permits an
idempotent release retry by the same authorized owner after PMIC recovery.
Unknown firmware auto-GPS/FM ownership must be reconciled before authorization.

The common Windows ARM64 COFF and Linux AArch64 ELF archives include this core;
EDK2 lists it in the existing library instance. `MtkVcn28CoreTest` runs native
ASan/UBSan and as a real ARM64 PE (20,157 assertions each). `vcn28-binary.py` executes the original
PALDO dispatch, wrapper and MT6789 callback for 49 comparisons with virtual
regmap/regulator/probe services, authenticates the regulator descriptor and
exact DT, and checks shared leases and retained errors. This is physical power
sequence code, not an installed PMIC/RF provider or whole GPS on/off owner.
WMT pre-control/firmware boot, confirmed-function integration, OS resources and
location processing remain. No real PMIC or tablet operation executes.

## Inner GNSS receiver framing

`MtkGnssCore.h/.c` reconstructs the primary input path in this board's exact
`libmnl.so` (SHA-256 `ae71dc763ef0f69ee2be01fde6dd7c48e4c4ddd2aab4cd3eeaa868d0abb3c319`).
The start/end markers are `aa f0` / `aa 0f`; inside them `de df` means `aa`
and `de e0` means `de`. The little-endian length's low 12 bits are payload
length plus four. Preserve its high four bits as opaque flags. Type is an
opaque little-endian 16-bit value; checksum is the low 16 bits of the sum of
both length bytes, both type bytes and payload. No positioning/NMEA meaning
is assigned to the body.

The whole decoded frame fits the stock 512-byte primary ring: 502 payload
bytes, at most 1020 escaped wire bytes. Receive returns one complete verified
frame with the exact consumed cursor, preserving partial escapes and markers.
Returned pointers expire on the next Receive/Init. Malformed/overflow data is
wiped and latched until explicit owner recovery; no heuristic payload resync.
Encoding writes no prefix on insufficient capacity and never sends a command.
The same owner must drain the STP physical stream, invalidate queued callbacks
and reset both decoders after receiver restart. Leading noise is ignored only
before the first start marker. The test suite runs STP decoding/channel-2
queueing/GNSS decoding together, including stale callback rejection.

`Tests/gnss-binary.py` executes only the public stock data_input entry and
selected byte parser/read/descriptor/serializer/escape/checksum bodies under
an instruction-range guard. GOT operands point only at virtual cells. Queue,
logging, memset and string append are virtual services. No general ELF linker,
Bionic runtime, constructor, engine init, daemon or device operation runs.
It checks 720 fragmented receive cases, 60 exact encodes, 120 checksum cases
and the intentional early checksum rejection difference. Native ASan/UBSan
and actual ARM64 PE use the same production C tests. Current counts/hashes
are in `analysis/gnss-stp-verification.json`. Neither a position engine nor an
OS GNSS/location consumer is supplied by this framing layer.

## MCU/offload output assembly

`MtkGnssOffloadInit/Receive/Reset` implements the separate stock offload port
format: LE16 fragment length including its four-byte header, U8 total, U8
1-based index, then payload. Each established endpoint has its own serialized
state and caller-owned buffer. The bounded core accepts at most 1532 payload
bytes per fragment (the stock multi-fragment allocation stride) and 65535
assembled bytes (the stock output count width). It does not require fixed-size
nonfinal fragments. Endpoint read boundaries and runtime offload selection must
be established by the transport owner; do not feed arbitrary STP bytes here.

Actual versus declared length, ordered exactly-once indices, consistent total
and cumulative capacity are checked before append. Malformed/capacity input
wipes and latches; it cannot silently replace an incomplete message. Reset after
owner cancel/drain, timeout, suspend or port close wipes bytes and changes epoch.
Old callbacks cannot read input or mutate the new message. Epoch exhaustion is
terminal. Completed pointers expire on the next current-epoch receive/reset.
There is no allocation, automatic I/O or callback in the portable core.

The guarded comparison executes the original pack_data, byte readers,
nmea_recv and output dispatcher through two registered virtual callbacks.
It verifies 1970 fragment comparisons across 44 completed synthetic messages,
four intentional length/order rejection differences, a disabled virtual output
gate and an allocation failure (1976 comparisons). All 50 successful virtual
allocations are freed by the original receiver. The actual output route is
established; bytes remain opaque, with no claim that a synthetic payload is a
position. Gate/debug/config services are virtual, so this does not establish
board offload policy, endpoint ownership or RF startup. The combined GNSS suite
passes 2,559,428 assertions each under native ASan/UBSan and actual ARM64 PE
emulation. Linux ELF/Windows COFF archives export the same 38 public functions.
The RMC/GGA parser and bounded stream state are described below. Physical port
ownership, the position engine and OS consumers remain implementation work.
Current evidence is `analysis/gnss-stp-verification.json`,
`original_arm64.gnss_offload`.


## Location, satellites and NMEA output

`MtkGnssDecodeLocation` decodes the exact 104-byte `GpsLocation_ext` consumed by
this board's Android GNSS implementation. `MtkGnssDecodeSvStatus` bounds the
signed count at offset 8 by actual bytes after the 16-byte header, and decodes
48-byte entries. Size/padding words remain opaque because the verified consumer
does not interpret them. The caller must establish this ABI; these are not STP,
inner framing or MCU position bytes. Unknown flags, present nonfinite/range
errors, negative/truncated counts and inadequate capacity never publish partial
records. Absent values are zero. Float fields travel as IEEE integer bits;
rounding, unsigned timing conversion and carrier-Hz truncation use integer math.
ARM64 ELF/COFF GNSS objects compile with `-mgeneral-regs-only` and no libc copy.

| Boundary | Verified fields |
| --- | --- |
| Location | flags +8, lat/lon +16/+24, ellipsoid altitude +32, speed/bearing +40/+44, signed UTC ms +56, four accuracy floats +64..76 |
| Elapsed time | flags +80, timestamp ns +88, unsigned uncertainty ns +96 converted to binary64 |
| SV entry | signed SVID +8, constellation +10, C/N0 +12, elevation/azimuth +16/+20, flags +24, carrier float +32, baseband C/N0 double +40 |
| MCU control | LE16 kind 2, length 3168, opaque LE16, then exactly 3168 bytes; no record-field interpretation |

The exact original location converter and full SV callback, including vector
growth, copying, freeing and callback dispatch, pass 934 comparisons. All 102
virtual allocations are freed by original instructions. Allocator, mutex,
AStatus, logging and final callback are bounded virtual services; Android/Binder
is never started. MCU kind-2 dispatch/ready/copy and repeated getters pass 194
comparisons on 96 synthetic records. Its ready bit is not freshness evidence.
`MtkGnssMcuPosition` publishes a bounded view with the input message's lifetime,
not a stored or newly calculated fix.

`MtkGnssNmeaParse` accepts one complete RMC/GGA output sentence, mandatory XOR
checksum and CRLF, at most 256 bytes and nine decimal places. It does not guess
an endpoint or consume primary binary receiver data. RMC uses an explicit owner
century for its two-digit year. GGA has only time of day and never borrows an old
date. A 23:59:60 value preserves a leap-second flag alongside POSIX-next-midnight
normalization. Void RMC/mode N and quality-zero GGA carry no position flags.
MSL altitude plus geoid separation produces ellipsoid altitude only when both
metre values exist. HDOP remains dimensionless; accuracy is not fabricated.
All decimal, coordinate and unit conversions use overflow-checked integer
arithmetic and correctly rounded rational-to-binary64 conversion.

The NMEA numeric reference checks 1024 synthetic cases independently using
Python Fraction/datetime. They are not original ARM64 comparisons. Six complete
messages additionally traverse actual original offload assembly/output callbacks
and the portable decoder. Detailed satellite consumer decoding is implemented;
NMEA GSV/GSA/GST are separate from this RMC/GGA boundary. OS GNSS adapters, RF startup,
position-engine execution and physical location remain outside this build.

## NMEA output stream

`MtkGnssNmeaStreamInit/Receive/Reset` adds one serialized, explicitly established
NMEA output stream. It assembles chunked CRLF lines, skips empty separators and
returns exact consumption for joined lines, bounded to 256 input bytes and one
sentence per call. Report is 208 bytes; private state is 760 bytes, with no heap
or physical I/O. Other checksum-valid sentences leave Report unchanged.

A merge (Kind 3) requires identical nanosecond time of day, overlapping signed
coordinate half-last-digit intervals and the owner's 1..60000 ms arrival budget.
RMC supplies date, coordinates, speed and bearing; GGA supplies altitude, quality,
SV count and HDOP. Precision metadata is not positioning accuracy. A merged pair
is wiped after use. Invalid fixes, expiry and Reset clear pending records. Reset
changes epoch; stale callbacks cannot read input or alter new state or Report.
Aliased arguments fail before writes; epoch exhaustion is terminal. A no-input
call ticks expiry. Malformed/overlong input, partial-line timeout, monotonic clock
regression and decreasing RMC calendar chronology wipe and latch until Reset.
Leap-second POSIX folding is compared by calendar day/time, avoiding a false
regression across midnight. Arrival time does not prove absolute fix freshness;
the OS adapter must supply that policy and cancel/drain physical I/O before Reset.

The software reference verifies 520 scenarios, including 512 randomized joined
and chunked RMC/GGA pairs in both orders. Six additional merged outputs feed from
separate actual-original-callback and portable-output stream states. These stream
checks are not original position-engine comparisons. The native and ARM64 suite
also checks every first-sentence split, input/output alias rejection, nanosecond
mismatch, coordinate precision, expiry, reset, chronology and line boundaries.

Semantic references: [AOSP GnssLocation](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/gnss/aidl/android/hardware/gnss/GnssLocation.aidl),
[AOSP satellite callback](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/gnss/aidl/android/hardware/gnss/IGnssCallback.aidl),
[Trimble RMC](https://receiverhelp.trimble.com/oem-gnss/nmea0183-messages-rmc.html),
[Trimble GGA](https://help.fieldsystems.trimble.com/r10/nmea0183-messages-gga.htm).
Exact offsets and routing come from authenticated local stock binaries, not a
header copied from another MediaTek generation.

## MT6789 CONN ROM firmware

`MtkConnRomLoad` stages the T70 Pro's three stock `soc2_2` images in the
EMI window owned by `MtkConnPower`. The detected MT6631 uses the `1d_1`
set and MT6635 uses `1e_1`, as in the stock Linux catalog used by
`WmtControl.c`. All three SHA-256 digests must match before any copy.
The stock `mtk_wcn_soc_rom_patch_dwn` drops 48 header bytes. The WMT
catalog clears byte 24 before making the low 24-bit EMI offset, which
places Bluetooth at 0x170000, Wi-Fi at 0x2a0000 and MCU at 0x0.

The loader checks the live remap, powered island, asserted MCU reset and
CONN IP/firmware revision. It verifies each copy through the same EMI
mapping. `MtkConnBootStart` holds the MCU in reset, powers CONN, loads the
ROM and then programs the MCU's firmware entries, XO setup, ADIE selection
and bus timing. The stock `WMT_SOC.cfg` uses co-clock mode and the MT6789
driver reports RC mode disabled. GPIO185 changes to the Wi-Fi function
after the MT6635 MCU reaches idle.

`MtkConnBootProbeAdie` powers the island with MCU reset held and reads the
analog chip ID through RF SPI before the full firmware boot. It uses the
stock MT6631 and MT6635 candidate order and RF SPI addresses. The platform
provides VCN18 and VCN13 leases. The probe keeps the same resources across
an incomplete shutdown, and publishes the ID only after shutdown and
rail release. A timed-out transaction supplies no chip ID.

`MtkConnRailsSetProbeRail` follows stock `consys_adie_chipid_checking_flow`.
It selects 1.8 V on VCN18, enables it and checks the ready bit after the
DT's 270 us ramp delay. For MT6635 it holds GPIO68 low until the VS2 vote
and selectors from `consys_hw_vcn13_ctrl` have been set and verified.

`MtkConnBootStartPowered` acquires the runtime supplies before setting
the CONN pinmux and powering the island. The stock `consys_hw_vcn18_ctrl`
and `consys_hw_vcn33_ctrl` sequences select VCN18 with 3.3 V VCN33 for
MT6631, or VCN18 with the GPIO68/VS2 path for MT6635. The driver verifies
the live rails before releasing MCU reset and while checking startup.
VCN33 shares a voltage selector with the Wi-Fi vote. The code preserves
that vote and returns busy if it would need to retune a live output.
The LDO voltage and status fields follow the
[MT6366 regulator definitions](https://github.com/torvalds/linux/blob/master/drivers/regulator/mt6358-regulator.c).

After island shutdown the driver restores the secondary supply first.
It waits the stock 300 us before releasing VCN18 and restores its saved
enable, operation and voltage fields. The owner keeps the PMIC/GPIO
reservation until restoration succeeds, including after an I/O failure.

`MtkConnRailsTest.c` exercises both runtime paths and the probe callback
against modeled PMIC and GPIO registers. It checks write order, ramp
and discharge delays, inherited votes, timeout and recovery after failures
during acquisition and release.

Start requires the idle word at MCU_CFG + 0x600 to become 0x1d1e after
MCU reset release. Stop asserts MCU reset before isolating an owned
island, then restores the shared GPIO fields, reset bit and EMI mapping.
A failed Stop keeps the lease for retry. The OS owner supplies the
detected analog chip, MMIO and pinned EMI, serializes shared register
access and connects PCCIF, WMT and the radio clients. Callers of
`MtkConnBootStart` continue to supply their own stable rails.

`Platforms/Teclast/t70proPkg/Tests/MtkConnRomTest.c` links
`MtkConnPower.c`, `MtkConnRom.c`, `MtkConnBoot.c` and `MtkConnRails.c`
with `-lcrypto` on ARM64 Linux. Pass the existing stock firmware directory
to the test. It checks the whole EMI window, MCU reset and power, and
runs both firmware sets through chip detection, rail acquisition and
restoration with modeled PMIC, GPIO and MMIO. The driver core has no
crypto library dependency.
