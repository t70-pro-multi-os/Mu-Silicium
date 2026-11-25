# Partial FIFO data on a failed SD read

2026-11-18. Host injections through actual MsdcPassThru/CMD18/PIO copy one FIFO
chunk, then assert DATCRCERR or DATTMO. The operation returns EFI_CRC_ERROR or
EFI_TIMEOUT in both its direct and packet status, does not copy the remaining
bytes, releases CommandActive and clears the sampled interrupt error. Presence
and identity are not invalidated solely by these transfer failures.

A separate actual SdDxe ReadBlocks test models a failed PassThru after copying
a 16-byte prefix. SdDxe propagates the error, retires the request/queue pin and
preserves MediaId, MediaPresent and geometry. A later successful request using
the same MediaId is accepted. Existing synchronous split tests also verify
that a failed first/middle command prevents later commands in that read.

Thus partial bytes remain in a failed caller buffer; they are not successful
read results and must be discarded. MediaId names media identity, not the count
of successful transfers. A CRC or timeout without observed removal does not by
itself establish a new card. Actual removal has a separate invalidation path.

This review confirms the implementation's specific CRC/timeout propagation;
it does not claim a complete audit of every Block I/O status-code requirement.
No automatic recovery or buffer rollback was added. Host suites passed:
MsdcTest 17,626,465 checks and SdDxeMediaTest 28,575 checks with ASan/UBSan.
The layer tests use separate mocks, not one integrated DXE/hardware execution.
No real storage access, user-data recovery or full firmware build occurred.
