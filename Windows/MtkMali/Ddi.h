/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_DDI_H
#define T70_MALI_DDI_H
#include "Wddm.h"
#include <dispmprt.h>
#include "Execution.h"
#include "Allocation.h"
#include "Vm.h"
#include "Adapter.h"
#define MALI_DDI_OBJECTS 64u
#define MALI_DDI_RESIDENTS (MALI_EXEC_QUEUE_DEPTH+1u)
#define MALI_DDI_DMA_BYTES (64u*1024u)

typedef enum {
    MaliDdiRejected=1, MaliDdiCompleted, MaliDdiPreempted, MaliDdiReset
} MALI_DDI_RELEASE_REASON;
typedef struct {
    MALI_EXEC_PACKET Packet;
    uint64_t DmaVa, ExecutionRoot;
    uint32_t DmaBytes;
    void *Cookie;
} MALI_DDI_PREPARED;
typedef struct {
    void *Context;
    /* Must validate and snapshot commands under the process GPUVA mappings,
       prohibit privileged descriptors, pin ALL resident backing memory/root,
       clean CPU writes and produce kernel-owned immutable job chains. DmaVa/
       DmaBytes describe that prepared range; ExecutionRoot is its retained physical
       root (detached before completion when distinct from the OS root), independently of the UMD range. The
       private bytes and GPUVA supplied by the UMD are UNTRUSTED. On failure
       this hook owns cleanup and returns no cookie. No permissive fallback. */
    NTSTATUS (*Prepare)(void *Context, HANDLE DxgkProcess, uint64_t ContextId, uint64_t Root,
        const DXGKARG_SUBMITCOMMANDVIRTUAL *Args, MALI_DDI_PREPARED *Prepared);
    /* PASSIVE, nonfailing: invoked only after no GPU access remains, or when
       a submission failed before reaching hardware. Never called for a fault
       until RESET_DONE. Successful Prepare must return a non-NULL cookie. */
    void (*Release)(void *Context, void *Cookie, MALI_DDI_RELEASE_REASON Reason);
    /* Nonblocking, nonpaged, callable at DISPATCH. Queue PASSIVE Pump work.
       Parent must also schedule real IRQ work, periodic timeout/retry work,
       serialize Stop/removal and drain all workers before freeing this object. */
    void (*Wake)(void *Context);
} MALI_DDI_HOOKS;
struct MALI_DDI_ADAPTER;
typedef struct {
    struct MALI_DDI_ADAPTER *Adapter;
    HANDLE DxgkProcess;
    uint64_t Owner;
    uint32_t Live, Devices;
} MALI_DDI_PROCESS;
typedef struct {
    struct MALI_DDI_ADAPTER *Adapter;
    MALI_DDI_PROCESS *Process;
    uint32_t Live, Contexts;
} MALI_DDI_DEVICE;
typedef struct {
    struct MALI_DDI_ADAPTER *Adapter;
    MALI_DDI_DEVICE *Device;
    uint64_t Id, Root;
    uint32_t Live;
    NTSTATUS RootStatus;
} MALI_DDI_CONTEXT;
typedef struct {
    void *Cookie;
    uint32_t Fence, Live;
} MALI_DDI_RESIDENT;
typedef struct MALI_DDI_ADAPTER {
    uint32_t Signature;
    KMUTEX Gate;
    /* One atomic word: ready bit 62, request bit 32 and 32-bit preempt fence.
       Stop/reset cannot race a separate ready check and request publication. */
    __declspec(align(8)) volatile LONG64 SchedulerState;
    uint64_t NextOwner;
    volatile LONG64 InterfaceReferences;
    SIZE_T ApertureCommitLimit;
    uint32_t AdapterConfigured,ProgressSeen,ProgressFence,ProgressStep,PowerPaused;
    MALI_CAPS PowerCaps; /* Stable query/restore identity across power loss. */
    uint64_t ProgressContext;
    MALI_EXECUTION Queue;
    MALI_WDDM_BRIDGE Paging;
    MALI_ALLOCATION_BOOK Allocations;
    MALI_VM *Vm;
    MALI_DDI_PROCESS Processes[MALI_DDI_OBJECTS];
    MALI_DDI_DEVICE Devices[MALI_DDI_OBJECTS];
    MALI_DDI_CONTEXT Contexts[MALI_DDI_OBJECTS];
    MALI_DDI_RESIDENT Resident[MALI_DDI_RESIDENTS];
    MALI_DDI_HOOKS Hooks;
    HANDLE DxgkHandle;
    ULONG InterruptMessage;
    DXGKCB_SYNCHRONIZE_EXECUTION Synchronize;
    DXGKCB_NOTIFY_INTERRUPT NotifyInterrupt;
    DXGKCB_QUEUE_DPC QueueDpc;
    DXGKCB_NOTIFY_DPC NotifyDpc;
    /* Immutable while connected. OS-assigned DIRQLs are validated before
       arming. Every mask/state critical section raises to the highest one and
       takes this shared nonpaged lock, including callbacks entered under a
       different Dxgkrnl interrupt lock. ISR never calls PASSIVE PowerReady. */
    KSPIN_LOCK InterruptGate;
    ULONG InterruptLevelMask;
    KIRQL InterruptSyncIrql;
    MALI_IO InterruptIo;
    uint32_t InterruptMasks[3];
    volatile LONG InterruptConnected,InterruptPending,InterruptPaused,InterruptRetry;
    NTSTATUS LastInterruptStatus;
    NTSTATUS LastRootStatus, LastNotifyStatus;
} MALI_DDI_ADAPTER;
/* Nonpaged, caller-allocated new adapter storage. A real render miniport must
   pass this object as hAdapter and keep the board exclusively owned/powered.
   This file implements scheduling and allocation-object lifecycle DDIs, not
   the board provider, general shader validation or a D3D user-mode driver.
   Miniport.c supplies DriverEntry/PnP and mapped-core/runtime lifetime;
   Prepare.h supplies the bounded write/cache command preparer. */
MALI_RESULT MtkMaliDdiInitialize(MALI_DDI_ADAPTER *Adapter, MALI_JM *Core,
    uint64_t TimeoutTicks, const DXGKRNL_INTERFACE *Dxgk, ULONG InterruptMessage,
    const MALI_DDI_HOOKS *Hooks, const MALI_WDDM_CACHE_OPS *Cache);
/* New work is blocked on success. Refuses live objects or submissions. Parent
   must perform worker/ISR rundown before freeing Adapter or stopping Core. */
NTSTATUS MtkMaliDdiShutdown(MALI_DDI_ADAPTER *Adapter);
/* Same no-live-object proof, blocks new work but preserves VM attachment for
   backend shutdown. Parent stops external callbacks before either operation. */
NTSTATUS MtkMaliDdiQuiesce(MALI_DDI_ADAPTER *Adapter);
/* Non-destructive power admission: retain OS objects, roots, mappings and
   feature-interface references. Pause requires every submission/notification/
   resident/MDL hold to be idle. Partial IRQ failure retains PowerPaused for
   retry. Parent drains workers/IRQs and stops Core before removing board power.
   Resume publishes admission only after matching Core, worker and IRQ restore. */
NTSTATUS MtkMaliDdiPowerPause(MALI_DDI_ADAPTER *);
NTSTATUS MtkMaliDdiPowerResume(MALI_DDI_ADAPTER *);
/* Attach only a new/empty VM using this adapter's allocation book. */
NTSTATUS MtkMaliDdiAttachVm(MALI_DDI_ADAPTER *,MALI_VM *);
NTSTATUS MtkMaliDdiPump(MALI_DDI_ADAPTER *Adapter, uint64_t Now);
/* Lets the PASSIVE worker sleep indefinitely when idle. Submission/preemption
   wakes use a synchronization event, so a wake between this check and wait
   is retained. A notified fault parks until reset/new work, retaining refs. */
NTSTATUS MtkMaliDdiHasWork(MALI_DDI_ADAPTER *Adapter, BOOLEAN *Pending);
/* PASSIVE, serialized scheduler/core state; HasWork alone also succeeds while
   quiesced and cannot establish that a D0 request has a running backend. */
NTSTATUS MtkMaliDdiCheckRunning(MALI_DDI_ADAPTER *);

DXGKDDI_QUERYADAPTERINFO MtkMaliDdiQueryAdapterInfo;
DXGKDDI_GETNODEMETADATA MtkMaliDdiGetNodeMetadata;
DXGKDDI_QUERYDEPENDENTENGINEGROUP MtkMaliDdiQueryDependentEngineGroup;
DXGKDDI_QUERYENGINESTATUS MtkMaliDdiQueryEngineStatus;
DXGKDDI_QUERY_INTERFACE MtkMaliDdiQueryInterface;
DXGKDDI_CREATEALLOCATION MtkMaliDdiCreateAllocation;
DXGKDDI_DESTROYALLOCATION MtkMaliDdiDestroyAllocation;
DXGKDDI_OPENALLOCATIONINFO MtkMaliDdiOpenAllocation;
DXGKDDI_CLOSEALLOCATION MtkMaliDdiCloseAllocation;
DXGKDDI_DESCRIBEALLOCATION MtkMaliDdiDescribeAllocation;
DXGKDDI_SETALLOCATIONBACKINGSTORE MtkMaliDdiSetAllocationBackingStore;
/* Parent QueryInterface exposes this in DXGKDDI_FEATURE_INTERFACE on WDDM 3.2. */
DXGKDDI_QUERYFEATURESUPPORT MtkMaliDdiQueryFeatureSupport;
DXGKDDI_CREATEPROCESS MtkMaliDdiCreateProcess;
DXGKDDI_DESTROYPROCESS MtkMaliDdiDestroyProcess;
DXGKDDI_CREATEDEVICE MtkMaliDdiCreateDevice;
DXGKDDI_DESTROYDEVICE MtkMaliDdiDestroyDevice;
DXGKDDI_CREATECONTEXT MtkMaliDdiCreateContext;
DXGKDDI_DESTROYCONTEXT MtkMaliDdiDestroyContext;
DXGKDDI_SETROOTPAGETABLE MtkMaliDdiSetRootPageTable;
DXGKDDI_GETROOTPAGETABLESIZE MtkMaliDdiGetRootPageTableSize;
DXGKDDI_SUBMITCOMMANDVIRTUAL MtkMaliDdiSubmitCommandVirtual;
DXGKDDI_PREEMPTCOMMAND MtkMaliDdiPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE MtkMaliDdiQueryCurrentFence;
DXGKDDI_RESETENGINE MtkMaliDdiResetEngine;
DXGKDDI_BUILDPAGINGBUFFER MtkMaliDdiBuildPagingBuffer;
/* PASSIVE, before Connect. Exactly three actual translated level-triggered
   line-interrupt descriptors, copied from the parent's validated OS resources.
   Parent must associate them with the stock JOB/MMU/GPU lines and route all
   three through our ISR, using MessageNumber 0. No raw GSI is an IRQL. This
   one-time configuration validates the assigned levels/vectors/affinities and
   derives a common maximum DIRQL; it does not connect physical interrupts. */
/* Shared pure validation lets StartDevice reject bad IRQs before power/MMIO.
   PASSIVE; output values are unchanged on failure. */
NTSTATUS MtkMaliDdiValidateInterruptResources(const CM_PARTIAL_RESOURCE_DESCRIPTOR *,
    UINT Count,ULONG *LevelMask,KIRQL *SyncIrql);
NTSTATUS MtkMaliDdiConfigureInterruptDomain(MALI_DDI_ADAPTER *,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *Interrupts,UINT Count);
/* PASSIVE, serialized parent PnP. Requires managed Core interrupts, configured
   interrupt domain, exclusive nonpaged live GPU MMIO and all JOB/MMU/GPU lines
   routed through this ISR. Each OS synchronization callback is still entered
   via DxgkCbSynchronizeExecution(...,0); our common lock protects all sources
   even when their OS interrupt locks/IRQLs differ. This does not establish
   physical interrupt/resource binding. Keep power/mappings/adapter valid until
   disconnect succeeds AND parent drains OS ISR/DPC entry sources. Failed calls
   may retain InterruptConnected; never free on a failed connect/disconnect. */
NTSTATUS MtkMaliDdiConnectInterrupts(MALI_DDI_ADAPTER *);
NTSTATUS MtkMaliDdiDisconnectInterrupts(MALI_DDI_ADAPTER *);
DXGKDDI_INTERRUPT_ROUTINE MtkMaliDdiInterruptRoutine;
DXGKDDI_DPC_ROUTINE MtkMaliDdiDpcRoutine;
#endif
