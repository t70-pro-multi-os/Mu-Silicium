// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_IRQ_RECOVERY_H
#define T70_AFE_IRQ_RECOVERY_H
#include "AfeIrqSource.h"
struct T70_AFE_IRQ_RECOVERY {
 T70_AFE_INTERRUPT_OWNER Owner;
 T70_AFE_IRQ_SOURCE Source;
 PUNKNOWN Parent;
 PIO_WORKITEM Work;
 KDPC Dpc;
 KEVENT Done;
 KMUTEX Mutex;
 volatile LONG Fault,Published,Queued,ObservationGate,RemovalRequested;
 NTSTATUS OriginalError,RecoveryStatus;
 bool Initialized,Closed,Closing;
};
/* Zeroed stable NONPAGED owner retained by Parent. PASSIVE Init/Connect/Close
 * externally serialized; use these wrappers exclusively, not direct owner calls.
 * Init allocates only OS work item, not resources/device/IRQ. Actual Device and
 * Parent supplied by real driver. Device belongs to this driver; Parent holds
 * Device and module/context lifetime from Init through successful Close, also
 * before any work is queued. OS queued-work device reference alone is not enough. Connect copies validated IRQ/I/O into owner.
 * ISR masks then schedules once. DPC queues PASSIVE work, which masks/disconnects
 * under internal mutex, retaining all references. No automatic restart/reset.
 * Close disconnects under mutex, drops mutex, drains DPC/work, then drains actual
 * PortCls service users through caller callback before releasing. Never called
 * from this object's worker/ISR/drain callback. No indefinite hardware polling.
 * Queued work can delay Close; callers must not hold locks needed by callbacks.
 * DrainService must cover all exported group users/PortCls work, not this worker.
 * Mask failure keeps everything for explicit retry. Original fault is preserved.
 * After successful Close caller must still exclude all API callers before free.
 */
NTSTATUS T70AfeIrqRecoveryInit(T70_AFE_IRQ_RECOVERY *,PUNKNOWN,PDEVICE_OBJECT);
NTSTATUS T70AfeIrqRecoveryConnect(T70_AFE_IRQ_RECOVERY *,IPortWaveCyclic *,
 const T70_AFE_INTERRUPT_RESOURCE &,const T70_AFE_DL1_INTERRUPT &,
 NTSTATUS (*Ready)(void *),void *);
// Only after actual OS surprise-removal notification. No source-mask/MMIO.
// Waits for in-flight interrupt critical section, then denies further I/O;
// active observation reservations return NOT_READY and retain references until
// retry. Successful result proves IRQ/work/service rundown only, NOT DMA idle,
// normal STOP, safe DMA-buffer release, or device access availability.
NTSTATUS T70AfeIrqRecoverySurpriseRemove(T70_AFE_IRQ_RECOVERY *,NTSTATUS (*DrainService)(void *),void *);
NTSTATUS T70AfeIrqRecoveryClose(T70_AFE_IRQ_RECOVERY *,NTSTATUS (*DrainService)(void *),void *);
// Bounded nonpaged register transaction at connected SynchronizeIrql. Caller
// owns surrounding stream/resource lifetime; callback cannot sleep/reenter.
NTSTATUS T70AfeIrqRecoveryExecute(void *,NTSTATUS (*)(void *),void *);
// Nonblocking <=DISPATCH observation. Gate pins IRQ until callback returns;
// Close/worker close admission and retain IRQ on active observations (retry).
NTSTATUS T70AfeIrqRecoveryObserve(void *,NTSTATUS (*)(void *),void *);
#endif
