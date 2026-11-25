# T70 synchronous SD read retry state

2026-11-18. Host-only regression `MsdcTest.ReadRetryStateTests` injects CMD18
AutoCMD12 timeout/CRC error, data timeout and elapsed-budget timeout through the
actual `MsdcPassThru`/`MsdcSendCmd`/PIO path. It verifies that the original error
reaches both return status and packet status, `CommandActive` is released,
AutoCMD12 was selected, and sampled interrupt error bits are cleared.

If the simulated hardware releases SDC_STS.BUSY, a subsequent read retires stale
interrupt status, clears FIFO and completes successfully without GPIO/power
reinitialization. If BUSY remains asserted, the next request times out before
issuing another command. The software does not automatically send a recovery
CMD12 or reset/re-identify the controller/card after these transfer failures.
`MsdcResetDevice` also returns unsupported for present media.

This establishes a recovery **limitation**, not that real T70 hardware always
retains BUSY after any particular error. The mock deliberately supplies that
state; it does not prove hardware transitions. A safe recovery implementation
requires evidence for card/host state after AutoCMD12 failure and preservation
of clock/bus-width/media identity across reset. No speculative reset, power
cycle, or success masking was added. Clearing the software transaction guard
alone must not be described as guaranteed controller/card recovery.

Focused MsdcTest run: 17,378,970 checks passed with ASan/UBSan. This was a host
simulation, not a firmware build, real card retry or physical stop command.
