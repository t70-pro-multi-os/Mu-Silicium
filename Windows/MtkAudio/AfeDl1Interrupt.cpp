// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeDl1Interrupt.h"
static bool Fault(T70_AFE_DL1_INTERRUPT *C,NTSTATUS S,bool Recognized=false){
 C->LastError=S==STATUS_SUCCESS?STATUS_DEVICE_NOT_READY:S;
 if(InterlockedCompareExchange(&C->Faulted,1,0)==0&&C->OnFault)
  C->OnFault(C->FaultContext,C->LastError);
 return Recognized;
}
BOOLEAN T70AfeDl1Interrupt(PKINTERRUPT Interrupt,void *Context){
 (void)Interrupt;
 auto C=static_cast<T70_AFE_DL1_INTERRUPT *>(Context);
 if(!C||!C->DomainExclusive||!C->Read32||!C->Write32||!C->Service||
    InterlockedCompareExchange(&C->Faulted,0,0))return FALSE;
 ULONG Enabled=0,Status=0;
 NTSTATUS S=C->Read32(C->IoContext,0x3b4,&Enabled);
 if(S!=STATUS_SUCCESS)return Fault(C,S);
 S=C->Read32(C->IoContext,0x3a4,&Status);
 if(S!=STATUS_SUCCESS)return Fault(C,S);
 ULONG Selected=Enabled&Status&0x87ffffffu;C->LastMask=Selected;
 if(!Selected)return FALSE;
 // Stock probe fixes DL1 to IRQ0. Other selected bits are clear-only in this
 // single-stream, exclusive-domain port, as stock does for absent substreams.
 if((Selected&1u)&&!T70WaveCyclicServiceNotifyConfirmed(C->Service))
  return Fault(C,STATUS_DEVICE_NOT_READY,true);
 S=C->Write32(C->IoContext,0x3a8,Selected);
 if(S!=STATUS_SUCCESS)return Fault(C,S,true);
 C->LastError=STATUS_SUCCESS;return TRUE;
}
