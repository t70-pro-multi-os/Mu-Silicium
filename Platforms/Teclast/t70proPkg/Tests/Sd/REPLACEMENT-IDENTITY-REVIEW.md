# SD identity across replacement and initialization retry

The actual MsdcDxe identification path keeps OCR, RCA, HCS and CSD decoding
state local to each attempt. SD_INFO persists only CardType and LastBlock;
there is no persistent host sector-addressing flag to reset. MsdcIdentifyMedia
invalidates the previous identity before initialization and again on failure.
The CCS/HCS-derived addressing choice is passed to MsdcReadSdLastBlock for
CSD consistency checks, not retained as a host identity field.

CardReplacementIdentityTests reuses the same host instance for SDSC→SDHC and
SDHC→legacy-SDSC replacement. After old-child withdrawal, it injects CRC failure
at CMD8, ACMD41, CMD3, CMD9, CMD7 and CMD13, then allows the timer-driven retry.
All 12 scenarios require UnknownCard/zero capacity/no published child on failure,
new capacity on success, cleared retry state, and the new RCA in every CMD9,
CMD7 and CMD13 argument. HCS must change with the new card's CMD8 outcome.

The host suite passed 17,735,981 checks under ASan/UBSan. No new production
fault was reproduced. The child in this suite is a mock carrying published
geometry, not actual SdDxe address conversion; those downstream fields are
outside this replacement test's claim. No firmware build or device access.
