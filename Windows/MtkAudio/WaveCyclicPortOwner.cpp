// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicPortOwner.h"
static NTSTATUS Exact(NTSTATUS S){return S==STATUS_SUCCESS?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);}
NTSTATUS T70WaveCyclicPortWithdraw(T70_WAVECYCLIC_PORT_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 O->PublicationClosed=true;
 if(!O->Registered)return STATUS_SUCCESS;
 if(!O->Unregister||!O->RegisteredDevice||!O->Port)return STATUS_INVALID_DEVICE_STATE;
 NTSTATUS S=Exact(O->Unregister->UnregisterSubdevice(O->RegisteredDevice,O->Port));
 if(S!=STATUS_SUCCESS)return S; // retain registration and every owned reference
 O->Registered=false;O->RegisteredDevice=nullptr;
 return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicPortPublish(T70_WAVECYCLIC_PORT_OWNER *O,PDEVICE_OBJECT D){
 if(!O||!D||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(D!=O->OpenDevice||!O->Initialized||!O->Port||!O->Miniport||O->Registered||O->PublicationClosed||O->Unregister)
  return STATUS_INVALID_DEVICE_STATE;
 static const GUID Id={0x16738177,0xe199,0x41f9,{0x9a,0x87,0xab,0xb2,0xa5,0x43,0x2f,0x21}};
 PUNREGISTERSUBDEVICE U=nullptr;
 NTSTATUS S=Exact(O->Port->QueryInterface(Id,(void **)&U));
 if(S!=STATUS_SUCCESS)return S;
 if(!U)return STATUS_DEVICE_DATA_ERROR;
 O->Unregister=U;
 static WCHAR Name[]={'T','7','0','W','a','v','e',0};
 S=Exact(PcRegisterSubdevice(D,Name,O->Port));
 if(S!=STATUS_SUCCESS){O->Unregister=nullptr;U->Release();return S;}
 O->RegisteredDevice=D;O->Registered=true;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicPortClose(T70_WAVECYCLIC_PORT_OWNER *O,NTSTATUS (*Confirm)(void *),void *Context){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 NTSTATUS Withdraw=T70WaveCyclicPortWithdraw(O);
 if(Withdraw!=STATUS_SUCCESS){O->CleanupStatus=Withdraw;return Withdraw;}
 if(O->Miniport){
  // Init can have succeeded internally even when the enclosing Port.Init fails.
  // Confirm is also needed for partially initialized provider resources.
  if(!Confirm){O->CleanupStatus=STATUS_DEVICE_NOT_READY;return O->CleanupStatus;}
  NTSTATUS S=Exact(Confirm(Context));
  if(S!=STATUS_SUCCESS){O->CleanupStatus=S;return S;}
  // Already proved external rundown; use that same result without repeating
  // a potentially stateful provider cleanup inside miniport Close.
  S=O->Miniport->Close([](void *)->NTSTATUS{return STATUS_SUCCESS;},nullptr);
  if(S!=STATUS_SUCCESS){O->CleanupStatus=S;return S;}
 }
 auto P=O->Port;auto M=O->Miniport;
 O->Port=nullptr;O->Miniport=nullptr;O->Initialized=false;O->OpenDevice=nullptr;
 auto U=O->Unregister;O->Unregister=nullptr;
 if(U)U->Release();if(P)P->Release();if(M)M->Release();
 O->CleanupStatus=STATUS_SUCCESS;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicPortOpen(T70_WAVECYCLIC_PORT_OWNER *O,PDEVICE_OBJECT D,PIRP I,
 PUNKNOWN Parent,PUNKNOWN Adapter,PRESOURCELIST R,const T70_WAVECYCLIC_MINIPORT_PROVIDER &Provider,
 NTSTATUS (*Confirm)(void *),void *Context){
 if(!O||!D||!I||!Parent||!R||!Confirm||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Attempted||O->Miniport||O->Port)return STATUS_INVALID_DEVICE_STATE;
 O->Attempted=true;O->OpenDevice=D;
 NTSTATUS S=T70WaveCyclicMiniport::Create(Parent,Provider,&O->Miniport);
 if(S!=STATUS_SUCCESS){O->InitStatus=S;return S;}
 static const GUID Cyclic={0xb4c90a2a,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
 PPORT P=nullptr;S=Exact(PcNewPort(&P,Cyclic));
 if(S==STATUS_SUCCESS&&!P)S=STATUS_DEVICE_NOT_READY;
 if(S!=STATUS_SUCCESS){
  // PcNewPort failure does not transfer an interface reference.
  O->InitStatus=S;O->Miniport->Release();O->Miniport=nullptr;return S;
 }
 O->Port=P;
 S=Exact(P->Init(D,I,O->Miniport,Adapter,R));O->InitStatus=S;
 if(S!=STATUS_SUCCESS){T70WaveCyclicPortClose(O,Confirm,Context);return S;}
 O->Initialized=true;return STATUS_SUCCESS;
}
