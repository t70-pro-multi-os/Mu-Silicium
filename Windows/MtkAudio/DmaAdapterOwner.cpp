// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "DmaAdapterOwner.h"
#include <stdint.h>
#include <stddef.h>
static bool CanPut(PDMA_ADAPTER A){
 return A&&A->Size>=offsetof(DMA_ADAPTER,DmaOperations)+sizeof(A->DmaOperations)&&A->DmaOperations&&A->DmaOperations->Size>=
 offsetof(DMA_OPERATIONS,PutDmaAdapter)+sizeof(A->DmaOperations->PutDmaAdapter)&&
 A->DmaOperations->PutDmaAdapter;
}
NTSTATUS T70DmaAdapterOpen(T70_DMA_ADAPTER_OWNER *O,T70WaveRtMiniport *M,PDEVICE_OBJECT P){
 if(!O||!M||!P||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Miniport||O->Pdo||O->Adapter||O->MapRegisters||O->Users||O->Closing)return STATUS_INVALID_DEVICE_STATE;
 DEVICE_DESCRIPTION D={};NTSTATUS S=M->GetDeviceDescription(&D);if(S!=STATUS_SUCCESS)return S;
 M->AddRef();ObReferenceObject(P);
 ULONG Count=0;PDMA_ADAPTER A=IoGetDmaAdapter(P,&D,&Count);
 if(!A){ObDereferenceObject(P);M->Release();return STATUS_INSUFFICIENT_RESOURCES;}
 // Preserve ownership even if an invalid OS/provider table cannot be released.
 O->Miniport=M;O->Pdo=P;O->Adapter=A;O->MapRegisters=Count;
 if(!CanPut(A)){O->Closing=true;return STATUS_DEVICE_DATA_ERROR;}
 return STATUS_SUCCESS;
}
NTSTATUS T70DmaAdapterBegin(T70_DMA_ADAPTER_OWNER *O,T70_DMA_ADAPTER_USE *U,PDMA_ADAPTER *Out){
 if(!O||!U||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Adapter||!O->Miniport||!O->Pdo||O->Closing||U->Owner||O->Users==UINT32_MAX)return STATUS_INVALID_DEVICE_STATE;
 ++O->Users;U->Owner=O;*Out=O->Adapter;return STATUS_SUCCESS;
}
NTSTATUS T70DmaAdapterEnd(T70_DMA_ADAPTER_OWNER *O,T70_DMA_ADAPTER_USE *U){
 if(!O||!U||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(U->Owner!=O||!O->Adapter||!O->Users)return STATUS_INVALID_DEVICE_STATE;
 U->Owner=nullptr;--O->Users;return STATUS_SUCCESS;
}
NTSTATUS T70DmaAdapterClose(T70_DMA_ADAPTER_OWNER *O,NTSTATUS (*Confirm)(void *),void *C){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Adapter)return (O->Miniport||O->Pdo||O->Users||O->MapRegisters)?STATUS_INVALID_DEVICE_STATE:STATUS_SUCCESS;
 O->Closing=true;
 if(O->Users)return STATUS_INVALID_DEVICE_STATE;
 if(!O->Miniport||!O->Pdo||!CanPut(O->Adapter))return STATUS_DEVICE_DATA_ERROR;
 if(!Confirm)return STATUS_DEVICE_NOT_READY;
 NTSTATUS S=Confirm(C);if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 auto M=O->Miniport;auto P=O->Pdo;
 O->Adapter->DmaOperations->PutDmaAdapter(O->Adapter);
 *O={};ObDereferenceObject(P);M->Release();return STATUS_SUCCESS;
}

NTSTATUS T70WaveRtOwnedDmaRelease(T70_WAVERT_OWNED_DMA *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 NTSTATUS S=T70WaveRtDmaRelease(&O->Mapping);
 if(S!=STATUS_SUCCESS)return S; // Keep adapter alive through partial MDL cleanup.
 if(O->AdapterUse.Owner)return T70DmaAdapterEnd(O->AdapterUse.Owner,&O->AdapterUse);
 return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtOwnedDmaAcquire(T70_WAVERT_OWNED_DMA *O,T70_WAVERT_MDL_OWNER *B,T70_DMA_ADAPTER_OWNER *A){
 if(!O||!B||!A||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 auto &M=O->Mapping;
 if(O->AdapterUse.Owner||M.Backing||M.Adapter||M.Free||M.LeaseHeld||M.Mapped||M.ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 PDMA_ADAPTER Adapter=nullptr;NTSTATUS S=T70DmaAdapterBegin(A,&O->AdapterUse,&Adapter);
 if(S!=STATUS_SUCCESS)return S;
 S=T70WaveRtDmaAcquire(&O->Mapping,B,Adapter);
 if(S==STATUS_SUCCESS)return S;
 // On failure the lower layer can retain cleanup ownership. Retain the adapter
 // in either case until that ownership is fully removed, even if cleanup fails.
 NTSTATUS Cleanup=T70WaveRtOwnedDmaRelease(O);
 return Cleanup==STATUS_SUCCESS?S:Cleanup;
}
