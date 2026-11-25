# SD asynchronous partial-submission review

## Current implementation update

The findings below are historical reproductions, not the final implementation
state. The paired change now makes Msdc ignore Event and execute synchronously,
as required for this synchronous-only provider. T70 enables the new fixed PCD
`PcdSdDxeSynchronousPassThru`; its default is FALSE for existing platforms.
When TRUE, SdDxe read/write and erase submit NULL lower events, do not modify
the caller's Event, and signal the caller once after the complete synchronous
operation succeeds. Immediate failures return errors without a notification or
pending request. This is a platform policy for all SdDxe-bound hosts, not an
automatically detected host capability. Other users of Msdc with SdDxe must
select the same policy. T70's read-only media policy is unchanged.

The default asynchronous **read/write** path now has one reference-counted batch
per caller operation. A submission reference prevents inline callbacks from
ending the batch early. Accepted commands hold their own references; callbacks
retire each request before releasing them. After a partial submission failure,
the API returns accepted/success while preserving the error for the single
completion notification after all outstanding commands finish. A first-command
rejection instead returns an error and never signals. Completion order does not
determine notification order: even the last-submitted command completing first
cannot prematurely signal the caller. The first observed error is retained.

Limits: the generic asynchronous **erase** path is still separate and was not
redesigned. T70 takes the synchronous erase path and rejects real writes via its
read-only policy. All-inline async read completion can signal while the outer
operation pin remains held; a reentrant Stop must return NOT_READY, not free the
device. Delayed final completion removes the last queue pin before notification.

Verification uses actual SdDxe functions with separate synchronous-only and
async-capable PassThru mocks; actual Msdc is independently exercised by MsdcTest.
It covers split success, first/middle/final submission errors, batch/request/event
allocation failures, reverse completion, delayed errors, immediate callback/token
freeing, pending Stop and one-time notification. These are host tests, not one
integrated hardware run. No full firmware build or device access was performed.


2026-11-18, source baseline: main `2ce7f8e`, Mu_Basecore `6c2c497`.

## Unresolved generic SdDxe defect

A host-only injection reproduced the following sequence through the actual
`BlockIo2.ReadBlocksEx` implementation:

1. Present, sector-addressed media, `LastBlock = 65535`, MediaId 7.
2. Submit 65536 blocks (32 MiB) with a non-NULL token event.
3. Mock PassThru accepts the first CMD18 (65535 blocks), leaving completion pending.
4. Mock PassThru rejects the final CMD17 with `EFI_DEVICE_ERROR`.
5. ReadBlocksEx returns that error without signaling the caller event, but the
   first request remains in `Device->Queue`, retaining the caller token/buffer.

The regression assertion `IsListEmpty (&mDevice->Queue)` fails after confirming
that the call returned `EFI_DEVICE_ERROR`, two PassThru calls occurred and the
caller event was not signaled. This is a lifetime hazard if the caller releases
its token or buffer after the rejected request. No physical I/O was performed.

`SdReadWriteInternal` returns immediately on a chunk submission error;
`AsyncIoCallback` signals only the chunk marked `IsEnd`. Merely signaling on the
submission error or freeing queued requests would be unsafe while an earlier
transport operation still owns them. A future fix needs batch-level ownership,
pending counts, partial-acceptance semantics and tests for immediate, delayed
and reordered completion. This review does **not** implement that fix.

The [UEFI Block I/O 2 contract](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html)
specifies that immediate device/media errors do not signal the caller event;
therefore absence of a signal alone is not the defect.

## T70 reachability and priority

T70 `MsdcPassThru` rejects every non-NULL Event with `EFI_UNSUPPORTED` before
executing a command. It cannot accept the first asynchronous chunk, so the
partial-submission sequence above is not reachable through that host. This
is not a claim that T70 supports asynchronous Block I/O or that other hosts
are safe. Synchronous T70 boot-path work takes priority over a generic batch
redesign. Temporary failing test changes were removed; existing tests and
production sources were preserved. No full firmware build or device test ran.

## Follow-up: standards-required synchronous fallback

UEFI 2.11 section 13.16.2 states that a host without nonblocking support ignores
Event and performs blocking I/O:
https://uefi.org/specs/UEFI/2.11/13_Protocols_Media_Access.html

Thus T70's current EFI_UNSUPPORTED rejection of a non-NULL Event is an
implementation fact, **not** a standards-compliant fallback. Simply deleting
that rejection is not safe with the current SdDxe consumer.

Two new host-only reproductions made the mock PassThru finish synchronously
and ignore its event, as that direct host-only change would do:

- One-block ReadBlocksEx returns EFI_SUCCESS, the user event is not signaled,
  and its request remains in Device->Queue. The queue-empty assertion fails.
- For 65536 blocks, the first CMD18 completes synchronously; the final CMD17
  returns EFI_DEVICE_ERROR. The user event remains unsignaled and the first
  request remains queued. The queue-empty assertion fails again.

SdDxe allocates per-command events and releases successful async requests only
from AsyncIoCallback. It has no detection of a successful synchronous-only host
that ignores events. The stranded request can block Stop and retains token
references. In this synchronous mock, no hardware operation is still accessing
the buffer after return; that is distinct from the earlier genuine-async
partial-submission lifetime hazard. Neither case is fixed by signaling early,
freeing live requests, or interpreting every successful return as completion:
a genuinely asynchronous host may still own the request at that point.

A safe change needs a coordinated consumer/host completion design, with an
explicit reliable choice of synchronous submission versus supported async
completion, and batch ownership for truly asynchronous partial acceptance.
There is no capability flag in the standard SD PassThru interface to infer this
from Event alone. This task did not add platform configuration or redesign
BlockIo2/batch ownership. Production behavior remains unchanged and the issue
is unresolved. Temporary reproductions were removed from the active suite;
original test source was restored exactly. No firmware build or device access
was performed.

## Completion callback Stop reentry regression

The T70 synchronous policy ends its active-operation pin before signaling the
caller. A TPL_CALLBACK completion can therefore Stop the child successfully
and free its token before ReadBlocksEx returns. In contrast, the generic async
provider completing all requests inline signals while the outer operation pin
is still held: callback Stop returns EFI_NOT_READY, and Stop succeeds after
ReadBlocksEx returns. These are deliberately different contracts.

The actual SdDxe functions are exercised with one and three command reads for
both policies. Tests assert one notification, caller-token release in callback,
empty request queue, all requests freed, eventual child teardown, balanced TPL
and zero allocations. The focused mock defers caller notifications at
TPL_NOTIFY until RestoreTPL permits TPL_CALLBACK dispatch; it is not a complete
UEFI event dispatcher. No new production defect was reproduced. No firmware
build or device access was performed.

## Synchronous token outcomes

A focused 28-case matrix now exercises actual ReadBlocksEx with the T70 policy,
a caller event, zero or 512 bytes, and success/CRC/timeout/device-error/no-media/
not-found/media-changed injected at the readiness probe or command. Successful
requests set TransactionStatus to EFI_SUCCESS and signal exactly once. Immediate
failures return their status without signaling or overwriting the caller's token
status; callers must check the function return rather than wait for an event.
EFI_NOT_FOUND normalizes to EFI_NO_MEDIA; EFI_MEDIA_CHANGED is preserved.

A zero-byte request with a valid non-NULL buffer still probes media, issues no
read command, and completes once if ready. Command-only injected failures cannot
affect it. Removal invalidates the child once; subsequent old-ID/current-ID calls
return EFI_MEDIA_CHANGED/EFI_NO_MEDIA without notification. All cases leave no
pending packet/event, an empty queue, and no allocation or active-operation leak.
No production change was required; these are host mocks, not hardware evidence.
