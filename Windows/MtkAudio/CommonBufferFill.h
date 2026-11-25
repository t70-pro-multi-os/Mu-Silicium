// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_COMMON_BUFFER_FILL_H
#define T70_COMMON_BUFFER_FILL_H
#include "AfeCommonBuffer.h"
#include "WaveRing.h"
typedef enum {T70BufferFillInvalid,T70BufferFillBeginFailed,
 T70BufferFillConversionFailed,T70BufferFillEndFailed,T70BufferFillCopied} T70_BUFFER_FILL_CODE;
typedef struct {
 T70_BUFFER_FILL_CODE Code;
 NTSTATUS BeginStatus,EndStatus;
 uint32_t BeginAttempted,EndAttempted,LeaseHeld,CopyCalls,ProgressValid;
 T70_PCM_RING_PROGRESS Progress;
} T70_BUFFER_FILL_RESULT;
/* CPU fill only: ordinary RAM Staging of at least Owner.Length plus conversion
 * Scratch when needed. All supplied ranges are disjoint, accessible/resident;
 * source/descriptor immutable. Owner was acquired by AfeCommonBuffer and is
 * externally serialized, including existing leases and parent teardown.
 * CPU prefill only while caller has independently proved DMA quiescent and
 * excluded every device/CPU consumer for the WHOLE call. Live ring refill is
 * NOT supported. AvailableBytes is a caller bound, not proof from CUR. PASSIVE_LEVEL only.
 * Convert/validate in RAM first, then RtlCopyDeviceMemory for one/two changed
 * ring spans. No ordinary memcpy touches Device Memory. No cache attributes
 * inferred, no cache flush/barrier/doorbell/start or DMA completion claimed.
 * Device-specific CPU access-width constraints beyond ARM alignment require a
 * different provider; this is RAM-backed common-buffer storage, not MMIO regs.
 * Validation/conversion failure leaves common buffer untouched; staging/scratch
 * disposable. Copy routine has no error status; access faults are NOT caught.
 * End failure after copying retains lease and ProgressValid, reporting changed
 * bytes rather than pretending rollback. Caller must recover that lease.
 * Success is CPU copy only, NOT DMA visibility/publication or playback readiness.
 * Link requires WDK volatileaccessk.lib; object compilation alone is not linking. */
T70_BUFFER_FILL_RESULT T70AfeCommonBufferFill(T70_AFE_COMMON_BUFFER *Owner,
 const void *Descriptor,size_t DescriptorBytes,const void *Source,size_t Bytes,
 void *Scratch,size_t ScratchCapacity,void *Staging,size_t StagingCapacity,
 size_t WriteOffset,size_t AvailableBytes);
#endif
