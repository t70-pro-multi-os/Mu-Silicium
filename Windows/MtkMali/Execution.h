/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_EXECUTION_H
#define T70_MALI_EXECUTION_H
#include "MaliJm.h"
#define MALI_EXEC_CONTEXTS 64u
#define MALI_EXEC_QUEUE_DEPTH 32u
#define MALI_EXEC_MAX_STEPS 16u
#define MALI_EXEC_PACKET_VERSION 1u
typedef struct { uint64_t Chain; uint32_t Slot, Reserved; } MALI_EXEC_STEP;
typedef struct {
    uint32_t Version, Bytes, StepCount, Flags;
    MALI_EXEC_STEP Steps[MALI_EXEC_MAX_STEPS];
} MALI_EXEC_PACKET;
#define MALI_EXEC_PACKET_BYTES(N) (16u + (N)*16u)
typedef struct {
    uint64_t Generation, Owner, Root;
    uint32_t Live, References, RootReady;
} MALI_EXEC_CONTEXT;
typedef struct {
    uint64_t Context, Root, DmaVa;
    uint32_t Fence, DmaBytes;
    MALI_EXEC_PACKET Packet;
    uint32_t PrivateRoot;
} MALI_EXEC_SUBMISSION;
typedef enum { MaliExecCompleted=1, MaliExecFaulted, MaliExecPreempted } MALI_EXEC_EVENT_TYPE;
typedef struct {
    MALI_EXEC_EVENT_TYPE Type;
    uint32_t Fence, LastCompletedFence, HardwareStatus;
    uint64_t Context, FaultAddress;
} MALI_EXEC_EVENT;
typedef struct {
    MALI_JM *Core;
    MALI_EXEC_CONTEXT Contexts[MALI_EXEC_CONTEXTS];
    MALI_EXEC_SUBMISSION Pending[MALI_EXEC_QUEUE_DEPTH], Active;
    MALI_EXEC_EVENT Event;
    uint64_t Token, ActiveToken, LastTime, StartTime, TimeoutTicks, ResetEpoch;
    uint32_t First, Count, ActiveValid, HardwareActive, Step, AddressSpace;
    uint32_t EventPending, EventAcknowledged, Faulted, LastCompletedFence;
    uint32_t LastSubmittedFence, PreemptionPending, PreemptionFence, Initialized;
} MALI_EXECUTION;
/* One software node executes a packet's JM stages in order. The owner must
   serialize every call and run Pump/Reset/MMU transitions at PASSIVE level.
   This layer is never an ISR and never dereferences user pointers. VidMm and
   the render KMD retain every allocation/root until the actual notification.
   Packet metadata must be prepared by that KMD, not trusted UMD private bytes.
   Job-chain validation, board power and cache maintenance remain prerequisites.
   Initialize only a new object, with exclusive ownership of an idle JM core. */
MALI_RESULT MaliExecInitialize(MALI_EXECUTION *Queue, MALI_JM *Core, uint64_t TimeoutTicks);
MALI_RESULT MaliExecCreateContext(MALI_EXECUTION *Queue, uint64_t Owner, uint64_t *Context);
MALI_RESULT MaliExecDestroyContext(MALI_EXECUTION *Queue, uint64_t Context);
MALI_RESULT MaliExecSetRoot(MALI_EXECUTION *Queue, uint64_t Context, uint64_t Root);
MALI_RESULT MaliExecSubmit(MALI_EXECUTION *Queue, uint64_t Context, uint32_t Fence,
                          uint64_t DmaVa, uint32_t DmaBytes, const MALI_EXEC_PACKET *Packet,
                          uint32_t PrivateBytes);
/* Trusted KMD-owned root for this submission. A differing root is detached
   before publishing completion, so notification may release its pages safely. */
MALI_RESULT MaliExecSubmitRoot(MALI_EXECUTION *,uint64_t Context,uint64_t Root,
    uint32_t Fence,uint64_t DmaVa,uint32_t DmaBytes,const MALI_EXEC_PACKET *,uint32_t PrivateBytes);
/* Time uses the caller's monotonically increasing ticks. No elapsed time is
   invented from loop counts. A timeout retains the active context until reset. */
MALI_RESULT MaliExecPump(MALI_EXECUTION *Queue, uint64_t Now);
MALI_RESULT MaliExecRequestPreempt(MALI_EXECUTION *Queue, uint32_t Fence);
/* Peek/Acknowledge is two-phase: acknowledge only after the real scheduler
   notification succeeds. Failed notifications cannot release context refs. */
MALI_RESULT MaliExecPeek(MALI_EXECUTION *Queue, MALI_EXEC_EVENT *Event);
MALI_RESULT MaliExecAcknowledge(MALI_EXECUTION *Queue, const MALI_EXEC_EVENT *Event);
/* ResetEpoch advances only after RESET_DONE, even if the subsequent start fails.
   LastAbortedFence excludes jobs never started; WDDM must resubmit those jobs. */
MALI_RESULT MaliExecReset(MALI_EXECUTION *Queue, uint32_t *LastAbortedFence);
MALI_RESULT MaliExecFlush(MALI_EXECUTION *Queue, uint64_t Root, uint64_t Start, uint64_t Size);
int MaliExecContextBusy(const MALI_EXECUTION *Queue, uint64_t Context);
#endif
