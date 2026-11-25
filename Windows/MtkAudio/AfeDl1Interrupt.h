// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_DL1_INTERRUPT_H
#define T70_AFE_DL1_INTERRUPT_H
#include "WaveCyclicService.h"
/* Whole AFE MCU interrupt-domain ownership is REQUIRED, not merely ownership of
 * DL1/bit0. All other memifs and firmware/DSP users must be independently ruled
 * out before connecting this ISR. Stock clears all selected MCU bits, including
 * named IRQ31; this port intentionally reproduces that algorithm, not an
 * unproved selective W1C optimization. No other client's notification is routed.
 * Nonpaged stable context/providers, ordered synchronous 32-bit register I/O at
 * device DIRQL. OS ISR serialization plus external removal/rundown required.
 * Read/write callbacks report exact complete success (zero), never pending.
 * This is an actual ISR entry but does not call IoConnectInterruptEx or enable
 * hardware. Notify precedes clear as in stock. A later write failure can follow
 * notification; Faulted latches to prevent duplicate notification on retry.
 * Integration must disconnect/recover a faulted source; this code cannot safely
 * invent an IRQ disable or claim that the line is quiet. No automatic reset.
 * IRQ31 physical meaning/concurrent-clear behavior remain source-derived, not
 * separately hardware-tested. Do not attach to a shared/partially owned domain.
 */
struct T70_AFE_DL1_INTERRUPT {
 void *IoContext;
 NTSTATUS (*Read32)(void *,ULONG,ULONG *);
 NTSTATUS (*Write32)(void *,ULONG,ULONG);
 T70_WAVECYCLIC_SERVICE *Service;
 bool DomainExclusive; // established by real adapter, not user-controlled proof
 volatile LONG Faulted;
 ULONG LastMask;
 NTSTATUS LastError;
 void (*OnFault)(void *,NTSTATUS)=nullptr;
 void *FaultContext=nullptr;
};
BOOLEAN T70AfeDl1Interrupt(PKINTERRUPT Interrupt,void *Context);
#endif
