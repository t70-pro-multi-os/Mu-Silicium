// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_INTERRUPT_OWNER_H
#define T70_AFE_INTERRUPT_OWNER_H
#include "AfeDl1Interrupt.h"
struct T70_AFE_INTERRUPT_RESOURCE {
 PDEVICE_OBJECT Pdo;
 ULONG RawGsi,Vector;
 unsigned char Irql;
 USHORT Group;
 uintptr_t Affinity;
};
struct T70_AFE_INTERRUPT_OWNER {
 PUNKNOWN Parent;
 T70_WAVECYCLIC_SERVICE Service;
 T70_AFE_DL1_INTERRUPT Handler;
 PKINTERRUPT Interrupt;
 ULONG Version;
 bool Attempted,ConnectionUnknown,SourceMasked;
 volatile LONG Removed;
};
/* Resource is from the actual exclusive level-high T70 AFE interrupt resource:
 * raw GSI266; translated vector/IRQL/group/affinity MUST be copied from OS data,
 * not synthesized from GSI. Parent retains actual PDO, nonpaged owner/context,
 * ordered DIRQL-safe I/O, and code until successful Close. No auto enumeration.
 * PASSIVE serialized lifetime calls; ISR can start before Connect returns.
 * Ready must establish the entire AFE domain exclusive and source disabled,
 * including exclusion of firmware/other memifs. No enable/power happens here.
 * OnFault is mandatory DIRQL-safe, nonblocking, preallocated recovery scheduling;
 * it must arrange source masking/recovery and PASSIVE Close, NOT call Close in
 * ISR or release context. Recovery must wait for Connect to return before
 * entering serialized Close. Even failed Connect retains Parent/Service until
 * explicit Close drains any pre-return callback work. No kernel timer or recovery provider is fabricated.
 * Close: MaskSource exact success -> IoDisconnectInterruptEx -> external queued
 * Notify/service/recovery work drain -> Group/Port release -> Parent release.
 * Mask/Drain failure retains resources for retry. Unknown successful OS output
 * is quarantined rather than guessed disconnected. Existing driver startup does
 * not call this until real ownership/mask/recovery providers exist.
 */
NTSTATUS T70AfeInterruptConnect(T70_AFE_INTERRUPT_OWNER *,PUNKNOWN,IPortWaveCyclic *,
 const T70_AFE_INTERRUPT_RESOURCE &,const T70_AFE_DL1_INTERRUPT &,
 NTSTATUS (*Ready)(void *),void *);
// Actual OS surprise-removal notification only, never a normal-stop shortcut.
// Synchronously closes ISR MMIO admission; does NOT prove DMA idle or mask source.
// Parent and IRQ remain retained; caller must subsequently quiesce/drain.
NTSTATUS T70AfeInterruptMarkRemoved(T70_AFE_INTERRUPT_OWNER *);
// Serialized PASSIVE mask/disconnect only; retains all references for drain.
NTSTATUS T70AfeInterruptQuiesce(T70_AFE_INTERRUPT_OWNER *,NTSTATUS (*)(void *),void *);
NTSTATUS T70AfeInterruptClose(T70_AFE_INTERRUPT_OWNER *,NTSTATUS (*MaskSource)(void *),
 NTSTATUS (*DrainWork)(void *),void *);
#endif
