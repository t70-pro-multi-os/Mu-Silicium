# Stock RTC boot-time calendar provider

Historical directory name retained to avoid rebasing unrelated board DSC changes.
The selected RealTimeClockLib now supports best-effort `GetTime(Time, NULL)` before
ExitBootServices when the verified DT and existing PMIC-wrapper protocol are present.
It also supports validated boot-time SetTime through the same existing PWRAP
protocol. It does not initialize power/clocks or invent wall time. Only the seven
calendar halfwords and RTC write trigger are written; no PMIC setup is added.

## Stock evidence

- effective DT: `/soc/pwrap@10026000`, `mediatek,mt6789-pwrap`, MMIO size 0x1000;
  child `mt6366` compatible `mediatek,mt6366`; child `mt6358rtc` compatible
  `mediatek,mt6358-rtc`. This is an internal MT6366 PMIC RTC, not an I2C RTC.
- `mt6397.ko`: `mt6366_devs` relocation at read-only data offset 0x7600 references
  `mt6358_rtc_resources` at 0x24d8, whose first resource is 0x0588..0x05c3.
- `rtc-mt6397.ko`: `mtk_rtc_read_time` at text 0x7bc, bulk read at 0x820 reads
  seven 16-bit values at base+0x0a. It reads seconds again at 0x874 and retries
  rollover. Masks: seconds/minutes 0x3f, hours/day 0x1f, month 0xf, year 0x7f;
  field 4 is weekday and skipped. Year conversion at 0x8b8 adds 68 to Linux's
  year-since-1900, hence 1968. No RTC write in this read function.
- This provider requires equal seconds (stricter than stock's rollover check),
  retries at most three snapshots, and rejects invalid Gregorian dates.

SHA256 of existing inputs (not copied into source):

- DT: `a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391`
- MFD module: `becf4329aea9c0e64e8abbcd1f6022889a08d254cda5a8608648b93e850d0610`
- RTC module: `04bfab810f235e3132f47ce3a7e663665e3e96e63e7f168abe0b1ef7d23f0652`

## Limits

Boot-time access uses the existing PmicWrapperDxe protocol. For runtime, the
shared MtkUfsPrm owner publishes a separate PWRAP runtime-access protocol; it
serializes RTC access with UFS/PMIC power work and refuses access while another
owner holds the PMIC path. BeforeExitBootServices rechecks the verified DT,
runtime GCD mapping and provider, then enables only GetTime/SetTime in the
runtime-properties table. A gFdtTableGuid notification handles DT installation
after this library initializes, and BeforeExitBootServices also covers shell
paths that skip ReadyToBoot. VirtualAddressChange converts the runtime base and
callback. These paths have host coverage; actual ExitBootServices and virtual
address transition on T70 remain unverified.

Alarms remain unsupported. GetTime accepts an optional capabilities pointer
and fills resolution 1 count/second, accuracy MAX_UINT32, and SetsToZero=FALSE.
The UEFI field is a numeric accuracy measure, not an unknown-value marker; the
maximum value is returned because no calibrated oscillator measurement is
available, so it must not be presented as a measured accuracy. The wrapper's
timezone/daylight is preserved. Failed reads leave the library caller's outputs
unchanged (the parent wrapper itself sets timezone and daylight first). This
does not establish full Windows RTC compliance.

The DT must be available before the pre-exit handoff. Missing/unqualified DT,
failed event creation, absent provider, non-runtime PWRAP mapping or busy
transport leaves runtime calendar services disabled rather than fabricating a
clock. This does not claim alarm support, cross-CPU/secure-world ownership, or
physical hardware validation.

## Host regression

From the repository root:

```
CC=gcc python3 Platforms/Teclast/t70proPkg/Tests/test-boot-rtc.py --dtb ../analysis/stock-fdt/effective.dtb
```

Runs the actual provider with fake transport under ASan/UBSan, plus the existing
real runtime-wrapper failure tests. Covers all represented years 1968..2095,
month/day boundaries, rollover/retry limit, each read error, reentry, DT mismatch,
failed event output, EBS and untouched failure output. This is not a full firmware
build, FD integration check or tablet execution.

## Calendar setting contract (code and mock tests only)

Stock `mtk_rtc_set_time` at text 0x24f8 writes full 16-bit fields at 0x592..0x59e.
Its instruction at 0x2520 explicitly zeroes DOW; year is absolute year minus 1968.
No alarm-style reserved-bit RMW is used. After seven successful writes it calls
`mtk_rtc_write_trigger` (0x5a4), which writes 1 at base + data.wrtgr; the MT6358
data entry sets that offset to 0x3a (absolute 0x5c2). It polls bit 6 of base 0x588
for completion. The read of BBPU does not change its control bits. No unlock,
reset, power/clock or interrupt setup is in this calendar-setting sequence.

Cross-check: [Linux v6.6 RTC driver](https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/rtc/rtc-mt6397.c)
and [register definitions](https://raw.githubusercontent.com/torvalds/linux/v6.6/include/linux/mfd/mt6397/rtc.h).
The older upstream setter leaves DOW uninitialized; this implementation follows
the verified stock zero, not that omission. No GPL implementation text is copied.

Before any I/O the provider validates the full EFI date, representable year
1968..2095, nanoseconds, timezone and daylight bits. It refuses a preexisting
CBUSY commit without writing. It writes the calendar then issues exactly one
trigger. Completion uses a one-second performance-counter deadline and a finite
poll-count fallback (1 ms sampling rather than stock's 10 us). Per-transaction
PWRAP timeouts still apply; a final transport read can exceed the deadline.

Any error after submitting a calendar write leaves the provider quarantined for
this boot, including an uncertain trigger result or completion timeout. There
is no automatic rollback, second trigger or fault-reset API. Subsequent GetTime
and SetTime fail without transport access, rather than exposing an uncertain
shadow calendar. No cross-CPU/secure-world RTC ownership is established.
The shared parent wrapper now keeps timezone/daylight unchanged until both
LibSetTime and any required persistent variable write succeed. Calendar transport
may already have succeeded if variable storage fails; no calendar rollback is
attempted. Same-settings calls still skip variable writes. The actual wrapper
regression covers unsupported/failed providers, invalid input, variable write
failure (including unsupported storage), success and GetTime metadata readback.

`SetRtcTest` exercises the actual public SetTime wrapper with fake variable and
PWRAP callbacks, all supported years and invalid calendar fields, exact address/
value order, one commit, each error boundary, timeout, reentry, quarantine and
EBS. No host test changes the tablet's RTC or any other device state.
