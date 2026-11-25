// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtMiniportBoundary.h"
#include <stdint.h>
T70WaveRtMiniportBoundary::T70WaveRtMiniportBoundary(ULONG P,const T70_WAVERT_MINIPORT_PROVIDER &B)
 :Pin(P),Provider(B),Attempted(false),Initialized(false){}
NTSTATUS T70WaveRtMiniportBoundary::Init(PUNKNOWN A,PRESOURCELIST R,PPORTWAVERT P){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!R||!P)return STATUS_INVALID_PARAMETER;
 if(Attempted)return STATUS_INVALID_DEVICE_STATE;
 if(!Provider.Initialize||!Provider.Create)return STATUS_DEVICE_NOT_READY;
 Attempted=true;
 NTSTATUS S=Provider.Initialize(Provider.Context,A,R,P);
 if(S==STATUS_SUCCESS)Initialized=true;
 return S==STATUS_SUCCESS?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);
}
NTSTATUS T70WaveRtMiniportBoundary::NewStream(PMINIPORTWAVERTSTREAM *Out,PPORTWAVERTSTREAM Port,
 ULONG RequestedPin,BOOLEAN Capture,PKSDATAFORMAT Format){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Out||!Port||!Format)return STATUS_INVALID_PARAMETER;
 if(!Initialized)return STATUS_DEVICE_NOT_READY;
 if(RequestedPin!=Pin||Capture)return STATUS_NOT_SUPPORTED;
 PMINIPORTWAVERTSTREAM Stream=nullptr;
 NTSTATUS S=Provider.Create(Provider.Context,Port,Pin,Format,&Stream);
 if(S!=STATUS_SUCCESS){if(Stream)Stream->Release();return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 if(!Stream)return STATUS_DEVICE_NOT_READY;
 // NewStream's documented initial state and position, not a RUN probe.
 S=Stream->SetState(KSSTATE_STOP);
 KSAUDIO_POSITION Position={UINT64_MAX,UINT64_MAX};
 if(S==STATUS_SUCCESS)S=Stream->GetPosition(&Position);
 if(S==STATUS_SUCCESS&&(Position.PlayOffset||Position.WriteOffset))S=STATUS_DEVICE_NOT_READY;
 if(S!=STATUS_SUCCESS){Stream->Release();return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 *Out=Stream;return STATUS_SUCCESS;
}
