# Stock power status core

This library ports selected read-only behavior from the exact SC8989x charger and HUSB311 TCPC modules extracted from the T70 Pro stock `vendor_boot_a:0` image. The C routines are usable from EDK2 callers, but they do not configure a controller or perform I2C themselves.

The caller provides `SP_READ_REGS`. Its implementation is expected to send a one-byte register pointer and then read the requested bytes with a repeated START. The library exposes no write callback. Callers should verify the chip identity before asking for its status, use the tablet-specific I2C path, and treat callback failures as transport errors.

## Implemented observations

- SC8989x identity reads register `0x14`, extracts bits 3–5, and reports whether the stock driver accepts that code. Status reads stop immediately for a code the stock driver does not recognize; otherwise they read `0x0b` then `0x0c`. The charge state and VBUS state are returned as raw field values, the power-good bit is decoded, and the stock status mapping is reproduced: VBUS code 7 yields Discharging; otherwise charge codes 0–3 map to NotCharging, Charging, Charging, and Full.
- SC8989x ADC scaling is a pure conversion helper: channel 2 is multiplied by 1000 and reported in microvolts, channel 4 is multiplied by 1000 and reported in microamps, and channel 3 is passed through in microamps. This does not read or enable the ADC.
- HUSB311 identity reads little-endian 16-bit values at registers `0x00` and `0x02`; the stock driver reads vendor ID `0x2e99` at register `0x00` and device ID `0x0311` at register `0x02`.
- HUSB311 power status reads `0x1e` and `0x97`, preserving both raw bytes and the observed bits 2 and 1.
- HUSB311 CC status reads `0x1d`; when bit 5 is clear it also reads `0x1a`. It exposes the two 2-bit CC fields and raw role-control byte. For nonzero CC fields, the driver adds an upper code bit derived from CCStatus bit 4 when RoleControl bit 6 is set, otherwise from `(RoleControl & 3) != 1`. When bit 5 is set, the stock routine reports both CC output values as `0x0f` and skips the role-control read. Role and orientation meanings are not inferred.
- HUSB311 alerts read `0x10` (two bytes), `0x98`, `0x12` (two bytes), then `0x99`; each pair is little-endian and the vendor byte occupies bits 16–23. Fault status at `0x1f` is returned raw because this module's getter does not decode its bits.

The C routines use a temporary result and leave the caller's output unchanged if a register read fails. This describes only the software output buffer; hardware read side effects (including read-to-clear or latched status behavior) were not established. No status path writes registers.

## Deliberate boundary

The stock charger's ADC helper performs operations around the ADC read whose read/write side effects are not established here. Other status and fault reads may also have device-specific side effects that static call-site evidence does not exclude. Therefore this library does not start ADC conversion or read ADC registers. The HUSB311 probe also writes register `0xa0` after checking its IDs; that initialization write, alert clearing, fault clearing, watchdog changes, PD message exchange, CC/role changes, and charger limits are excluded.

The shared EDK2 DEC/DSC wiring is maintained by the package integration owner. This library is not yet a tablet runtime driver, and no on-device I2C behavior has been checked.

## MT6358 gauge and AP NTC provider cores

`MtkGaugeCore` implements the shipped gauge's current snapshot transaction:
PMIC `0x0d0a` latch command, bounded bit-15 polling, current `0x0d8a`, and
masked latch cleanup. All accesses are supplied by a qualified exclusive PMIC
owner. A failed write may have occurred; cleanup failure or lease-release
failure quarantines the instance and retains ownership. The whole API call
must be externally serialized, not just its individual callbacks. No automatic
reset or raw PRM register access is added. Samples publish only after cleanup
and lease release succeed. The stricter idle-control check intentionally refuses
an inherited nonzero control register rather than overwriting unknown state.

Its conversion reproduces the stock one's-complement current representation
and integer truncation, taking explicit internal sense-resistance/car-tune
values. It returns **stock integer units**, not a claimed ACPI mA value. It
rejects invalid calibration and overflow; DT values must not be passed without
the stock parser's scale. Exhaustive host arithmetic tests are software tests,
not independent execution of the original ARM64 binary or calibration proof.

`MtkThermalCore` contains T70's AP NTC lookup data and stock interpolation,
with an explicit out-of-range clamp flag. Its input is the stock IIO processed
sample, not a raw ADC register or assumed millivolts. Conversion to ACPI tenth
kelvin uses nearest/half-up rounding. The test reads the actual stock DTB and
checks all lookup knots and integer interpolation cases.

Both files are included in the library INF and common host verifier. They are
**not yet live ADC/gauge drivers**: no PMIC broker binding, ADC scaling/efuse
provider, freshness/notify contract, battery SOC estimator, or AML `_BST`/`_TMP`
publisher is connected. Do not treat boot-time samples as runtime telemetry.
