# T70 Pro Windows SDPORT driver

The portable C cores issue MSDC1 commands, validate SD responses, and drive
read and write DMA through an MMIO interface. The `SDC1` / `HANA6792` ACPI node
provides card detection, pin configuration and clock gates for the Windows host.

The port driver must validate card initialization replies before
using an RCA or issuing the next application command. These three bounded
checks reuse the working EFI implementation's response-payload contract rather
than inventing Windows hardware behavior:

| API | Existing source evidence in `MsdcDxe.c` |
| --- | --- |
| `T70SdCheckCmd8` | CMD8 sent with `0x1AA`; low 12 bits must echo it; bit 22 must be clear |
| `T70SdCheckAppCommand` | `SdCardSendOpCond` checks APP_CMD bit 5 and ILLEGAL_COMMAND bit 22 after CMD55 |
| `T70SdDecodeRelativeAddress` | `SdCardSetRca` rejects R6 bits 15:13 and RCA zero before publishing the upper 16 bits |

Source: `Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c`.
The stock DTB identifies `/soc/mmc@11240000` as `mediatek,mt6789-mmc`, with
a four-bit SD bus.

## Caller contract

Only call these functions after the transport has established successful
completion, correct response type, and valid timeout/CRC status. Pass the
normalized host-endian 32-bit response payload, equivalent to EFI `Resp0`, not
raw SD wire bytes. R6 is SD CMD3, **not eMMC's CMD3 response**. CMD8 is restricted
to the existing `0x1AA` request. No fallback to a legacy card is decided here.

The CMD55 helper checks only application-command acknowledgement. It does not
validate every R1 error bit, decide data readiness, or authorize an I/O request.
Bits outside each documented mask are intentionally ignored, matching the EFI
checks. The RCA output stays unchanged on rejected input. Result values are
portable enum values, not EFI_STATUS or NTSTATUS.

## Command transport

`SdCommand.c` encodes MSDC command and data fields, acknowledges only
command status, and normalizes the response for SDPORT. It checks DAT0
after the response to commands with busy signaling. The caller serializes
the command and data paths and supplies a deadline and another service
opportunity while DAT0 stays low. A canceled, removed, or software-timed-out
command requires a controller reset before the slot can issue another command.

Run `CC=gcc python3 Windows/MtkSd/test-command.py` on the builder. It checks
all command indices and response formats, isolated command and data status,
DAT0 busy release, error precedence, and command reuse after recovery.

## Host setup

`SdHost.c` resets the slot, sets the card clock divider and data timeout,
and switches between one-bit and four-bit bus widths. The SD clock method
`SCOP` enables the AXI-side gate, isolates the source to select the 26 MHz
oscillator, and restores the stock pin masks. Run `SDTC` to set the MT6789
sampling registers during host reset. `SDCD` reports the active-high GPIO7
level or returns 2 when the pin state cannot be read. The caller must stop
the card clock and retire commands and DMA before isolating the source.
The initial 400 kHz request yields 382,352 Hz. With the oscillator source,
the 25 MHz request currently yields 13 MHz.

Run `CC=gcc python3 Windows/MtkSd/test-host.py` for controller timing and
reset checks, and `python3 Windows/MtkSd/test-acpi.py` to exercise the
compiled AML and check its write addresses and masks under ACPICA.

## DMA

`SdDma.c` uses the MediaTek descriptor layout and DMA registers in
`drivers/mmc/host/mtk-sd.c`. It uses four-byte-aligned segments in a
32-bit DMA mapping, a general descriptor and its zeroed terminator, and
up to 1,024 linked buffer descriptors with 16-bit lengths. The checksum
covers the first 16 bytes of each descriptor. Invalid maps are rejected
before the descriptor table or registers are changed.

The host owns the noncoherent cache synchronization, MMIO barrier and
interrupt serialization. Arm with a valid DMA map and clocks, then start
after the command response succeeds. Service the data interrupt or the
request's cancel, removal or timeout through `T70SdUpdateDma`. Keep
the mappings until STOP has cleared and the engine reports inactive. The
first terminal result releases that request's DMA ownership. If STOP
stalls, the host must quiesce or reset the engine before freeing buffers.
Retire the request's interrupt snapshot before rearming the next request.
An unavailable card-detect sample retains the last observed state. A confirmed
removal aborts the active request.

## Slot power

`SdPower.c` uses the firmware PMIC broker v3 to manage VMCH and VMC on
Windows 11 24H2. The slot initializes the PRM interface at PASSIVE_LEVEL
and can change card power from an SDPORT bus operation at APC_LEVEL.
Power-on waits for the acknowledged lease and the same 20 ms settle time
used by `SdCardIdentification`. Stop the clock and retire commands and
DMA before releasing power. The broker restores the inherited rail state.
An uncertain accepted operation retains the module lock and blocks new
slot commands until restart.

Run `python3 Windows/build.py --sd-power-test` on the builder. It compiles
the production code against the ARM64 WDK and runs the Windows caller and
firmware broker through emulated PRM and WACS registers.

## Windows SDPORT driver

Build the unsigned Windows 11 24H2 ARM64 driver on the builder with
`python3 Windows/build.py --sd`. The INF binds the driver to
`ACPI\HANA6792` and does not advertise SD boot or crash-dump support.
SDPORT handles card enumeration. The miniport issues card commands and
reads and writes through MSDC1 using a private, noncached DMA buffer. For a
multi-block read, it waits for DMA and automatic stop to finish, then delivers
each block using the SDPORT PIO request sequence. For CMD24 and CMD25, it waits
for the card to accept the command, copies the complete SDPORT buffer when
StartTransfer arrives, and starts DMA. A single-block or automatically stopped
write waits for DAT0 to return high. An explicitly stopped multi-block write
leaves the final busy wait to CMD12. Erase and card programming commands stay
blocked. The slot has no physical write-protect switch. An R1 write-protect
violation returns STATUS_MEDIA_WRITE_PROTECTED.

`GetCardDetectState` reads card presence from the board's GPIO7 through
`SDCD` at PASSIVE_LEVEL. The GPIO7 both-edge interrupt in ACPI `_CRS` is separate from the MSDC1
controller interrupt. The host keeps controller interrupt sources masked, so
its SDPORT interrupt callback reports the ACPI GPIO event as a card change.
SDPORT then queries `SDCD` at PASSIVE_LEVEL. Removal marks active requests
as removed, stops further commands, and advertises write protection until a
full host reset confirms the card returned. An ACPI-backed both-edge event in
the SDPORT callback and reinsert-after-reset flow pass the ARM64 mock. OS
interrupt routing and re-enumeration still need runtime observation.

It currently reports up to four-bit, 3.3 V operation using the 26 MHz
oscillator. SDPORT's save and restore callbacks pause the card clock and
restore the controller's timing and bus width when the board clocks,
pads and card supply are retained. The callbacks use MMIO at
DISPATCH_LEVEL. The normal bus operations own ACPI and PMIC calls.
An interrupted request completes once. A failed restore or detected
removal requires a bus reset before further I/O. SDIO function I/O,
UHS and wake from loss of board power are outside this coverage.

Run `python3 Windows/build.py --sd --test` for the ARM64 callback tests.
They cover ACPI input and reply validation, power/clock order, commands,
64 KiB reads and writes, write direction and payload, DAT0 and CMD12 ordering,
write-protect and transfer errors, card replacement before DMA, cancellation,
and SDPORT save and restore with retained and lost controller state.
With a DMA engine that cannot be stopped, the driver reports the timeout
and retains its buffer until the hardware becomes idle. On removal in
this state it retains the allocation to avoid writes into released memory.
The SD power client can be tested on its own with `--sd-power-test`.

## Host validation

Run `python3 Windows/MtkSd/test-response.py` from the repository root.
The actual C decoder runs with `-Wall -Wextra -Werror` and ASan/UBSan:

- all 65,536 R6 low-status combinations for five RCA boundaries;
- every nonzero 16-bit RCA and unchanged output on rejection;
- every 12-bit CMD8 response with and without illegal-command indication;
- walking-bit and all-ones inputs for CMD8/CMD55;
- all APP_CMD/ILLEGAL_COMMAND combinations and null output pointers.

Run `python3 Windows/MtkSd/test-dma.py` to check descriptor layout, invalid
maps, selective interrupt acknowledgement, completion ordering, CRC,
timeout, cancellation, card removal and subsequent requests. Set `CC=gcc`
on builders with GCC's sanitizer runtime. The DMA source also compiles as
an ARM64 Windows kernel object. The Windows driver and its callbacks have been
built and tested with simulated SDPORT and MSDC1 services.
