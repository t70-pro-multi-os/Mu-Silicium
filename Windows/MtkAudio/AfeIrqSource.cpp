// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeIrqSource.h"
static NTSTATUS Exact(NTSTATUS S){return S==0?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);}
static NTSTATUS Mask(T70_AFE_IRQ_SOURCE *S){
 auto &H=S->Owner->Handler;ULONG On=0,Enable=0,Status=0;S->Completed=0;
 // Checked under the interrupt lock (also in OnFault). A recovery worker may
 // have been waiting for that lock when surprise removal was published.
 if(InterlockedCompareExchange(&S->Owner->Removed,0,0))return STATUS_DEVICE_NOT_READY;
 NTSTATUS R=Exact(H.Read32(H.IoContext,0x3a0,&On));if(R)return R;
 // Stock fixed IRQ0 disable: preserve every other ON field. Do not disable
 // unknown clients as an error-recovery shortcut.
 R=Exact(H.Write32(H.IoContext,0x3a0,On&~1u));if(R)return R;S->Completed=1;
 R=Exact(H.Read32(H.IoContext,0x3a0,&On));if(R)return R;
 if(On&0x87ffffffu)return STATUS_DEVICE_NOT_READY;
 R=Exact(H.Write32(H.IoContext,0x3a8,1));if(R)return R;S->Completed=2;
 // Stronger than stock's unchecked STOP: don't disconnect with an observed
 // selected MCU interrupt still pending (including IRQ31). No polling/retry.
 R=Exact(H.Read32(H.IoContext,0x3b4,&Enable));if(R)return R;
 R=Exact(H.Read32(H.IoContext,0x3a4,&Status));if(R)return R;
 if(Enable&Status&0x87ffffffu)return STATUS_DEVICE_NOT_READY;
 S->Completed=3;return STATUS_SUCCESS;
}
static bool Valid(T70_AFE_IRQ_SOURCE *S){
 return S&&S->Owner&&S->Owner->Handler.DomainExclusive&&
  S->Owner->Handler.Read32&&S->Owner->Handler.Write32&&S->ScheduleRecovery;
}
static BOOLEAN Synchronized(void *Context){
 auto S=(T70_AFE_IRQ_SOURCE *)Context;S->MaskStatus=Mask(S);return S->MaskStatus==0;
}
NTSTATUS T70AfeIrqSourceMask(void *Context){
 auto S=(T70_AFE_IRQ_SOURCE *)Context;
 if(!Valid(S)||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!S->Owner->Interrupt||S->Owner->ConnectionUnknown)return STATUS_DEVICE_NOT_READY;
 // Close and Connect are externally serialized. OS lock excludes the ISR and
 // keeps the interrupt object alive until this returns, before disconnect.
 S->MaskStatus=STATUS_DEVICE_NOT_READY;
 if(!KeSynchronizeExecution(S->Owner->Interrupt,Synchronized,S)&&S->MaskStatus==0)
  S->MaskStatus=STATUS_DEVICE_NOT_READY;
 return S->MaskStatus;
}
void T70AfeIrqSourceFault(void *Context,NTSTATUS Error){
 auto S=(T70_AFE_IRQ_SOURCE *)Context;if(!Valid(S))return;
 // Invoked by ISR under its interrupt lock, possibly before Connect returns:
 // never synchronize recursively or require an Interrupt output pointer here.
 S->MaskStatus=Mask(S);
 S->ScheduleRecovery(S->RecoveryContext,Error);
}
