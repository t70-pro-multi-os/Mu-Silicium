# BTIF FIFO driver

The KMDF driver owns the T70 Pro BTIF resources declared by `Btif.asl`.

| DT/ACPI index | Resource |
|---|---|
| reg 0 | 0x1100c000 / 0x1000 |
| reg 1 | 0x10217b00 / 0x80 |
| reg 2 | 0x10217b80 / 0x80 |
| IRQ 0/1/2 | GSIV 222 / 216 / 217, level, exclusive |

`HANA6795` is the board's development hardware ID. The driver uses GSIV 222
for PIO and reads the two DMA enable registers to verify the FIFO is idle
before starting. All three ranges and IRQs are checked before mapping.

The driver acquires BTIF access from WCN0 at D0 entry. WCN0 holds its power
lock around bounded register operations. BTIF's passive interrupt lock
serializes FIFO access, STP decoding, and teardown. On a fresh MCU boot, BTIF
polls the FIFO until WMT confirms the switch from mandatory framing to full
mode 4 and the first full-mode exchange is acknowledged. The driver releases
both locks between polls. It retains the link sequence numbers across a clean
D0 exit and entry with the same MCU session.

When WCN0 or BTIF is removed, the driver retires its lease before unmapping.
A failed or uncertain stop leaves clock recovery to WCN0 after CONN isolation.

Build the unsigned ARM64 SYS using the existing WDK on the builder:

```sh
python3 Windows/MtkBtif/build.py --prototype --output-dir Windows/Build/btif-fifo-dev
```

Use a new output directory. The target links the KMDF 1.33 driver and HCI
adapter, checks the PE checksum, entrypoint and loader imports, and copies the
ACPI parent INF. The driver creates a BTHX child with the Microsoft compatible
ID and handles static version/capability queries. Event/ACL reads and HCI writes use non-power-managed manual queues. The ISR
completes reads when channel-zero HCI packets arrive and writes when their full
STP frame drains into the BTIF FIFO. D0 exit and removal close the HCI epoch and
fail queued requests; a clean same-session resume opens a new epoch. Radio
firmware startup and RF power ownership are not connected, so these queues do
not establish a usable Bluetooth radio by themselves.

Run the native checks on the builder:

```sh
python3 Windows/MtkBtif/test-owner.py
python3 Windows/MtkBtif/test-build.py
```

The owner check runs the production callbacks, PDO creation, BTHX child forwarding and the actual HCI adapter through read/write queue requests, plus FIFO/STP code under ASan/UBSan. It exercises resource validation, WCN0 query and lease failures,
mode negotiation and timeout, split frames, the interrupt budget, idle
register restoration, same-session resume, surprise removal, BTHX PDO creation,
compatible ID, non-power-managed queues, IOCTL forwarding, cancellation,
power-exit queue draining and HCI epoch rebinding. The simulated registers
require an active WCN0 lease on every read and write. Current result: 40,967
assertions under ASan/UBSan.

## HCI packet adapter

`HciTransport.c` binds to `MtkWmtMode.Link` after the MCU enters STP full mode, then maps the Bluetooth H4 byte stream on channel 0 to WDK `BTHX_HCI_READ_WRITE_CONTEXT` command, event and ACL buffers. It does not send HCI using the startup-only mandatory framing. The channel and
payload framing follow `__bt_write` and `BT_read` in the board's
`bt_drv_connac1x.ko` (SHA-256
`f64d70ae7dd1351d7370683fb69f76860db0bb81abe431aca3804b744b86b73c`).
The adapter reassembles packets across STP frames, preserves queue order within
each BTHX packet type and reports byte counts with the H4 indicator removed.
An ACL packet including its HCI header is limited to 1995 bytes by the STP
transmitter. RX backpressure preserves a complete packet and reports the
consumed prefix so the caller can retain the suffix. A partial TX acceptance
requires stream recovery. The KMDF owner now serializes HCI with STP and binds the BTHX read/write
queues. After full-mode negotiation, D0 startup sends the WMT Bluetooth
function-on command and waits up to two seconds for the firmware status and
the STP/FIFO queues to drain. The function ID `0` matches `WMTDRV_TYPE_BT=0` in the T70 kernel-reference
`wmt_exp.h` at commit `a71515dbaf0d539ffb36416f14186dad17eef743` (header SHA-256
`47d802fc0fe3df51d87dd35ce67c511ca970c7e4de5a06f340ae088be1af2054`).

`T70BtBthxRequest` processes static version/capability requests before the
BTIF session exists, since the Bluetooth child receives them before D0.
The BTHX child forwards static queries and HCI data through non-power-managed
parent queues; HCI data still requires an active BTIF session. The adapter validates
the WDK version, capabilities and packet buffers. Packet reads preserve data when their buffers are too short. An
accepted write reports pending until `T70BtBthxService` moves its whole STP
frame into the BTIF FIFO. Shared WMT frames queued earlier are included in the
bytes ahead of the write. Cancellation completes the request without
replaying the packet and lets its already-enqueued frame drain in order.
The current capability reply advertises `ScoSupportNone` and zero SCO channels. Microsoft requires `ScoSupportHCIBypass` and one SCO channel; a capability reply that does not meet this contract prevents the Bluetooth stack from loading ([BTHX capability contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthxddi/ni-bthxddi-ioctl_bthx_query_capabilities)). T70 is a non-USB controller whose HFP voice path must use a sideband audio channel. Until the WCN owner exposes the T70 SCO SRAM/IRQ path and an HFP bypass audio endpoint uses it, this BTIF BTHX child is not a usable Windows radio. The WMT function-on path has host coverage only; firmware acknowledgement, signed package installation, Windows loading and RF exchange remain unverified.

To build an ARM64 static library and check the same object code on the builder:

```sh
Resources/Scripts/with-build-lock T70Pro 'Verify BTIF HCI' -- \
  ../.venv/bin/python Windows/MtkBtif/test-hci.py --output-dir Windows/Build/btif-hci-dev
```

The ARM64 test executes the production adapter over STP full mode and the WDK packet layout, STP parser/encoder and BTIF FIFO code with simulated registers. It verifies exact full-mode transmit bytes, split and
coalesced input, ACL size limits, short reads, both queue pressure paths,
partial writes, and stale epochs after teardown. It also checks BTHX buffer
sizes, delayed FIFO completion, WMT ordering, cancellation, and transmit
failure after a partial FIFO transfer.

## Shared CONN power domain

`MtkConnPower.c` follows the T70 Pro stock WMT driver’s CCIF4 clock and SCPSYS
runtime power path. It waits for both SPM status registers, releases the island
reset before opening the three bus protections, and closes the protections
before power-off. A shutdown timeout retains ownership for a retry. An
already-powered island is left to its existing owner.

The caller serializes power transitions, prepares the parent clocks and supplies,
and provides a contiguous 5 MiB firmware reservation aligned to 16 MiB below
2 GiB. The owner programs and verifies the firmware and peripheral address
windows before enabling CCIF4, and restores the previous fields after
power-off. A timeout leaves the firmware reservation pinned until Stop
succeeds. Bluetooth, Wi-Fi and other CONN users must be drained before shutdown.
The CCCI owner still supplies the modem-sharing window.
Run the SPM and INFRACFG model on the builder from the repository root:

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I Silicon/MediaTek/MediaTekPkg/Include \
  Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkConnPower.c \
  Platforms/Teclast/t70proPkg/Tests/MtkConnPowerTest.c \
  -o /tmp/t70-conn-power-test
/tmp/t70-conn-power-test
```

## CONN PMIC access from Windows

`ConnPmic.c` implements the PMIC callbacks for `MtkConnRails` through the
firmware PRM broker at `PASSIVE_LEVEL`. Windows 11 24H2 or later provides the
PRM interface. The firmware accepts the CONN rail fields used by the stock
MT6631 and MT6635 sequences and shares WACS arbitration with UFS, SD and keys.
The lease stays held until the broker has observed each changed field restored.
A lost response during a register operation keeps the module locked because
the outcome of the accepted operation can be unknown.

The board owner still needs to supply GPIO callbacks, coordinate CONN clock
and firmware memory ownership, and attach the power sequence to the KMDF D0
lifecycle. Run the ARM64 Windows adapter with the firmware broker and simulated
PMIC registers on the builder.

```sh
../.venv/bin/python Windows/MtkBtif/test-pmic.py
```
