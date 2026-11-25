// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioDeviceStart.h"
static NTSTATUS NoStream(void *,PPORTWAVERTSTREAM,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *){
 return STATUS_DEVICE_NOT_READY;
}
// Used only before Prepare publishes success: no stream, buffer, register
// configuration or DMA operation has been made available by this initializer.
static NTSTATUS Unpublished(void *){return STATUS_SUCCESS;}
NTSTATUS T70AudioDeviceRelease(T70_AUDIO_DEVICE_START *O,NTSTATUS (*Confirm)(void *),void *C){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 // DMA adapter retains the miniport and must outlive every DMA mapping.
 NTSTATUS S=T70DmaAdapterClose(&O->Dma,Confirm,C);if(S!=STATUS_SUCCESS)return S;
 if(O->Resources){S=O->Resources->Close(Confirm,C);if(S!=STATUS_SUCCESS)return S;}
 // Keep our references until both resource owners have completed teardown.
 if(O->Port){auto P=O->Port;O->Port=nullptr;P->Release();}
 if(O->Miniport){auto M=O->Miniport;O->Miniport=nullptr;M->Release();}
 if(O->Resources){auto R=O->Resources;O->Resources=nullptr;R->Release();}
 O->Prepared=false;return STATUS_SUCCESS;
}
NTSTATUS T70AudioDevicePrepare(T70_AUDIO_DEVICE_START *O,PDEVICE_OBJECT D,PIRP I,
 PDEVICE_OBJECT Pdo,PRESOURCELIST R,ULONG Index){
 if(!O||!D||!I||!Pdo||!R||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Resources||O->Miniport||O->Port||O->Prepared||O->Dma.Adapter||O->Dma.Miniport||
    O->Dma.Pdo||O->Dma.Users||O->Dma.MapRegisters||O->Dma.Closing)return STATUS_INVALID_DEVICE_STATE;
 // The caller supplies a real PortCls device, not an arbitrary DEVICE_OBJECT.
 // Query its existing relation; do not create/discover another device or infer
 // that two independently supplied pointers describe the same DMA endpoint.
 PDEVICE_OBJECT ActualPdo=nullptr;
 NTSTATUS S=PcGetPhysicalDeviceObject(D,&ActualPdo);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(!ActualPdo||ActualPdo!=Pdo)return STATUS_NO_SUCH_DEVICE;
 S=T70AudioResourceOwner::Create(Index,&O->Resources);
 if(S!=STATUS_SUCCESS)return S;
 T70_WAVERT_MINIPORT_PROVIDER Provider={O->Resources,T70AudioResourceOwner::Initialize,NoStream};
 S=T70WaveRtMiniport::Create(O->Resources,Provider,&O->Miniport);
 if(S==STATUS_SUCCESS)S=T70WaveRtPortBind(D,I,O->Miniport,O->Resources,R,&O->Port);
 if(S==STATUS_SUCCESS)S=T70DmaAdapterOpen(&O->Dma,O->Miniport,Pdo);
 if(S==STATUS_SUCCESS){O->Prepared=true;return S;}
 NTSTATUS Cleanup=T70AudioDeviceRelease(O,Unpublished,nullptr);
 return Cleanup==STATUS_SUCCESS?S:Cleanup;
}
