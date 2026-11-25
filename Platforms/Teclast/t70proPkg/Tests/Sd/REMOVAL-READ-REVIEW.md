# SD removal during synchronous reads

2026-11-18. T70 configuration uses GPIO7, active high. The platform helper
samples an input and reports direction/read failures instead of treating them
as presence. Insertion requires three consecutive 20 ms timer samples; removal
or a failed detect sample immediately invalidates readiness. Removal is not
subject to the insertion debounce delay.

MsdcPassThru owns CommandActive during the synchronous command and samples
presence again before returning success. The media timer skips work while a
command is active; existing tests cover this reentrancy guard. Invalidation
requests child withdrawal, and a new card is not identified until the old
child is confirmed absent. This is event/TPL coordination, not a claim about
unimplemented multicore locking.

New actual-function mock cases remove the card before FIFO copying, after one
FIFO chunk, and at the final post-transfer check. Every case returns NO_MEDIA,
releases CommandActive, clears readiness/capacity and requests withdrawal.
A subsequent present sample alone returns NOT_READY without another command.
No further FIFO bytes are copied after the sampled removal.

**Buffer caveat:** bytes copied before removal detection remain in the caller's
buffer. In the final-check case the whole buffer has already been copied, but
the operation still returns NO_MEDIA. Consumers must discard data on failure;
this driver does not promise atomic buffer rollback. Physical removal between
samples, or removal/reinsertion entirely between samples, is not established
as detectable by these mocks.

Host results: MsdcTest 17,624,491 and MsdcPlatformTest 418 checks, ASan/UBSan.
No real GPIO sampling, physical removal or full firmware build was performed.
