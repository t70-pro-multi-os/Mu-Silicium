# SD operating-condition review

2026-11-18. The host tests inject ACMD41 busy replies and independently alter
CCS without altering CSD. Before the fix, both contradictory CCS/CSD combinations
reached ready state. `MsdcReadSdLastBlock` now checks that the final ready OCR's
addressing type agrees with CSD structure before publishing capacity. CMD8-capable
cards use ready CCS; the legacy CMD8-timeout path remains byte-addressed.

The [SD Association Physical Layer Simplified Specification, section 4.2.3](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver6.00.pdf)
defines ready as OCR bit 31 set and makes CCS meaningful only in that state:
CCS distinguishes standard capacity from high/extended capacity. The change
rejects inconsistent input rather than guessing which address format to use.

## Polling and remaining limits

- The first zero-voltage ACMD41 is an inquiry. Up to 100 subsequent polls each
  include CMD55/ACMD41 and a 10 ms delay. Tests cover ready immediately, after
  one busy response, at the final poll, and still busy after all polls. The last
  case does not reach CMD2 and reports timeout without publishing a child.
- This is an iteration limit, not an overall elapsed-time deadline. Each command
  receives its own default timeout; slow transport responses can extend total
  initialization beyond the approximately one-second fast-response simulation.
- The current platform contract has no supported OCR voltage mask. The loop
  echoes the card's reported voltage bits instead of intersecting them with an
  independently established host-supported range. Therefore actual voltage
  compatibility is **not verified** by this code or these tests. No host voltage
  capability was invented and no power-resource code was changed.

Focused host suite: 17,389,259 checks with ASan/UBSan. No full build, physical
card initialization or electrical-voltage test was performed.
