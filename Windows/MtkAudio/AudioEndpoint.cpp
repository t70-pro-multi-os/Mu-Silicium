// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioEndpoint.h"
static NTSTATUS Exact(NTSTATUS S){return S==0?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);}
NTSTATUS T70AudioEndpointWithdraw(T70_AUDIO_ENDPOINT *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 O->Closing=true;NTSTATUS S=0;
 if(O->Connected){
  S=Exact(O->Unlink->UnregisterPhysicalConnection(O->Device,O->Wave->Port,1,O->Port,0));
  if(S)goto Done;O->Connected=false;
 }
 if(O->Wave){S=T70WaveCyclicPortWithdraw(O->Wave);if(S)goto Done;}
 if(O->Registered){
  S=Exact(O->Unregister->UnregisterSubdevice(O->Device,O->Port));
  if(S)goto Done;O->Registered=false;
 }
Done:O->CleanupStatus=S;return S;
}
NTSTATUS T70AudioEndpointClose(T70_AUDIO_ENDPOINT *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 NTSTATUS S=T70AudioEndpointWithdraw(O);if(S)return S;
 if(O->Miniport){S=O->Miniport->Close();if(S){O->CleanupStatus=S;return S;}}
 auto U=O->Unlink;auto R=O->Unregister;auto P=O->Port;auto M=O->Miniport;
 O->Unlink=nullptr;O->Unregister=nullptr;O->Port=nullptr;O->Miniport=nullptr;
 O->Device=nullptr;O->Wave=nullptr;
 if(U)U->Release();if(R)R->Release();if(P)P->Release();if(M)M->Release();
 return STATUS_SUCCESS;
}
static NTSTATUS CreatePlain(PUNKNOWN P,const void *,T70TopologyMiniport **M){return T70TopologyMiniport::Create(P,M);}
NTSTATUS T70AudioEndpointOpen(T70_AUDIO_ENDPOINT *O,PDEVICE_OBJECT D,PIRP I,PUNKNOWN Parent,
 PRESOURCELIST Resources,T70_WAVECYCLIC_PORT_OWNER *Wave){
 return T70AudioEndpointOpenUsing(O,D,I,Parent,Resources,Wave,CreatePlain,nullptr);
}
NTSTATUS T70AudioEndpointOpenUsing(T70_AUDIO_ENDPOINT *O,PDEVICE_OBJECT D,PIRP I,PUNKNOWN Parent,
 PRESOURCELIST Resources,T70_WAVECYCLIC_PORT_OWNER *Wave,T70_TOPOLOGY_CREATOR Creator,const void *Context){
 if(!O||!D||!I||!Parent||!Resources||!Wave||!Creator||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Attempted||O->Closing||!Wave->Initialized||Wave->OpenDevice!=D||Wave->Registered||Wave->PublicationClosed)
  return STATUS_INVALID_DEVICE_STATE;
 O->Attempted=true;O->Device=D;O->Wave=Wave;
 NTSTATUS S=Exact(Creator(Parent,Context,&O->Miniport));if(S)return S;
 if(!O->Miniport)return STATUS_DEVICE_DATA_ERROR;
 static const GUID Topology={0xb4c90a32,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
 static const GUID UnregisterId={0x16738177,0xe199,0x41f9,{0x9a,0x87,0xab,0xb2,0xa5,0x43,0x2f,0x21}};
 static const GUID UnlinkId={0x6c38e231,0x2a0d,0x428d,{0x81,0xf8,0x07,0xcc,0x42,0x8b,0xb9,0xa4}};
 S=Exact(PcNewPort(&O->Port,Topology));if(S)return S;if(!O->Port)return STATUS_DEVICE_DATA_ERROR;
 S=Exact(O->Port->Init(D,I,O->Miniport,Parent,Resources));if(S)return S;
 S=Exact(O->Port->QueryInterface(UnregisterId,(void **)&O->Unregister));if(S)return S;
 if(!O->Unregister)return STATUS_DEVICE_DATA_ERROR;
 S=Exact(O->Port->QueryInterface(UnlinkId,(void **)&O->Unlink));if(S)return S;
 if(!O->Unlink)return STATUS_DEVICE_DATA_ERROR;
 static WCHAR Name[]={'T','7','0','T','o','p','o','l','o','g','y',0};
 S=Exact(PcRegisterSubdevice(D,Name,O->Port));if(S)return S;O->Registered=true;
 S=T70WaveCyclicPortPublish(Wave,D);if(S)return S;
 S=Exact(PcRegisterPhysicalConnection(D,Wave->Port,1,O->Port,0));if(S)return S;
 O->Connected=true;return STATUS_SUCCESS;
}
