# SD CMD13 readiness gate

2026-11-18. Before the fix, successful CMD13 transport with response zero could
publish MediaReady even though the card did not report readiness or TRAN state.
The SD-only CardSetBusMode path now requires READY_FOR_DATA (bit 8) and
CURRENT_STATE (bits 12:9) equal to TRAN (4) before changing to the transfer clock
and allowing initialization to finish. eMMC behavior is unchanged.

Up to 100 CMD13 calls are made, separated by 10 ms when another poll remains.
Transport failures propagate immediately; card COM_CRC_ERROR, ILLEGAL_COMMAND,
CARD_ECC_FAILED, CC_ERROR and ERROR bits also reject readiness immediately.
Other R1 fields are not a complete status-conformance validation here.
Exhaustion returns EFI_TIMEOUT; the enclosing initialization path invalidates
capacity/card identity and does not publish a ready child.

This is a poll-count cap, not an aggregate wall-clock deadline: each CMD13 has
its own transport timeout. Host tests cover persistent not-ready, wrong states,
all five error bits, combined errors, ready on the first/second/100th poll,
exhaustion and transport timeout/CRC failures. 17,621,896 ASan/UBSan checks passed.

Bit meanings follow SD Association Physical Layer Simplified Specification
section 4.10.1: https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver6.00.pdf
No real card, full firmware build or electrical validation was performed.
