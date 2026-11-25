// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_MALI_JM_H
#define T70_MALI_JM_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MALI_JM_MAX_AS 16u
#define MALI_JM_MAX_SLOTS 3u
#define MALI_JM_MAX_EVENTS 20u
#define MALI_JM_PAGE_SIZE 4096u
#define MALI_JM_VA_LIMIT (UINT64_C(1) << 48)
#define MALI_JM_PA_LIMIT (UINT64_C(1) << 40)
#define MALI_JM_MEMATTR UINT64_C(0x00004c8d888d8f88)
typedef enum {
    MaliOk=0, MaliInvalid, MaliUnsupported, MaliBusy, MaliTimeout,
    MaliFault, MaliOffline
} MALI_RESULT;
enum {
    MaliPteValid=1u, MaliPteRead=2u, MaliPteWrite=4u, MaliPteExecute=8u,
    MaliPteCached=16u, MaliPteTable=32u
};
typedef struct { uint64_t Address; uint32_t Flags; } MALI_PTE;
/* G57 AArch64-4K MMU: root level 0 .. leaf 3, AF=1, L3 type=3.
   Cached chooses MEMATTR index 2 (0x8d); default is index 0 (0x88).
   GPU mappings are non-shareable; CPU/GPU coherence is never implied. */
MALI_RESULT MaliJmEncodePte(uint32_t Level, const MALI_PTE *Entry, uint64_t *Encoded);
/* Scratch is caller-owned serialized storage, never a 4 KiB kernel local.
   It must cover Count words and overlap neither the table nor the input.
   The full update is snapshotted before any target byte is changed. */
MALI_RESULT MaliJmWritePtes(uint64_t *Table, uint32_t Level, uint32_t Start,
                          uint32_t Count, const MALI_PTE *Entries, int Repeat,
                          uint64_t *Scratch, uint32_t ScratchWords);
typedef const uint64_t *(*MALI_RESOLVE_TABLE)(void *, uint64_t);
MALI_RESULT MaliJmTranslate(void *Context, MALI_RESOLVE_TABLE Resolve,
                           uint64_t Root, uint64_t Va, uint64_t *Pa, uint32_t *Flags);
MALI_RESULT MaliJmLockAddress(uint64_t Start, uint64_t Size, uint64_t *Encoded);
typedef struct {
    void *Context;
    uint32_t (*Read32)(void *, uint32_t);
    void (*Write32)(void *, uint32_t, uint32_t);
    void (*Barrier)(void *);
    void (*DelayUs)(void *, uint32_t);
    /* Prove exclusive ownership, powered rails and running board clocks. */
    int (*PowerReady)(void *);
} MALI_IO;
typedef struct {
    uint32_t GpuId, MmuFeatures, AddressSpaces, JobSlots;
    uint32_t JobFeatures[MALI_JM_MAX_SLOTS];
    uint64_t Shaders, Tilers, L2;
} MALI_CAPS;
typedef struct {
    uint64_t Chain, Affinity, Fence;
    uint32_t AddressSpace, Active;
} MALI_JOB;
typedef struct {
    MALI_IO Io;
    MALI_CAPS Caps;
    uint64_t Roots[MALI_JM_MAX_AS];
    MALI_JOB Jobs[MALI_JM_MAX_SLOTS];
    uint32_t BoundSpaces, Started, Poisoned, ManagedInterrupts;
} MALI_JM;
typedef enum { MaliJobDone=1, MaliJobFailed, MaliMmuFault, MaliGpuFault, MaliJobAborted } MALI_EVENT_TYPE;
typedef struct {
    MALI_EVENT_TYPE Type;
    uint32_t Slot, AddressSpace, Status;
    uint64_t Fence, Address;
} MALI_EVENT;
/* Caller serializes every entry, including IRQ/reset. All waits are bounded.
   Clean CPU writes to GPU memory to PoC before Bind/Submit/Flush; invalidate
   readback memory after actual completion. This core never guesses board power. */
MALI_RESULT MaliJmInitialize(MALI_JM *Device, const MALI_IO *Io);
MALI_RESULT MaliJmProbe(MALI_JM *Device, MALI_CAPS *Caps);
/* Select before first Start, with no live GPU work. Managed mode keeps all
   interrupt masks zero across every Start/reset; the OS IRQ owner arms them
   only after its context/handlers/workers are ready. Caller serializes. */
MALI_RESULT MaliJmSetManagedInterrupts(MALI_JM *Device,uint32_t Managed);
MALI_RESULT MaliJmStart(MALI_JM *Device);
MALI_RESULT MaliJmBind(MALI_JM *Device, uint32_t As, uint64_t Root);
MALI_RESULT MaliJmUnbind(MALI_JM *Device, uint32_t As);
MALI_RESULT MaliJmFlush(MALI_JM *Device, uint32_t As, uint64_t Start, uint64_t Size);
MALI_RESULT MaliJmSubmit(MALI_JM *Device, uint32_t Slot, uint32_t As,
                         uint64_t Chain, uint64_t Affinity, uint64_t Fence);
MALI_RESULT MaliJmPoll(MALI_JM *Device, MALI_EVENT *Events, uint32_t Capacity, uint32_t *Count);
/* Retire jobs only after observed hardware reset completion. Failed reset keeps
   every in-flight reference; the caller must not free their allocations. */
MALI_RESULT MaliJmReset(MALI_JM *Device, MALI_EVENT *Aborted, uint32_t Capacity, uint32_t *Count);
MALI_RESULT MaliJmStop(MALI_JM *Device);
#ifdef __cplusplus
}
#endif
#endif
