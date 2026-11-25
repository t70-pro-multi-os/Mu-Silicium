# T70 SD bus width review

2026-11-18. The inspected T70 path does not perform an ACMD6 bus-width
transition. `MsdcInit` sets the host to one bit; SD identification issues CMD0,
CMD8, CMD55/ACMD41, CMD2, CMD3, CMD9 and CMD7, then CMD13 readiness polling.
The upper SdDxe does not issue ACMD6 or change the host width either.

T70 DSC selects host 1, read-only operation and 25 MHz. The `SdCardSwitch`
helper is ordinary CMD6 (64-byte function-switch response), not CMD55+ACMD6;
its SD high-speed branch requires a clock setting above 25 MHz and is not
entered by this configuration. The host width helper supports 4/8-bit encodings,
but the only initialization caller requests one bit.

The host regression now explicitly checks that successful T70 initialization
sends no command index 6 and leaves the host width field at the one-bit encoding.
There is no ACMD6 failure/rollback path to inject in this configuration and no
reproduced width-switch mismatch to fix. This is not a claim that four-bit mode
is implemented or tested. Card-side electrical state was not measured; no
hardware access or full firmware build occurred.
