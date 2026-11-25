// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioEndpoint.h"
#include "T70TopologyVolume.h"
static NTSTATUS CreateBound(PUNKNOWN P,const void *B,T70TopologyMiniport **M){
 return T70TopologyMiniport::CreateWithVolume(P,static_cast<const T70_TOPOLOGY_VOLUME_BINDING *>(B),M);
}
NTSTATUS T70AudioEndpointOpenWithVolume(T70_AUDIO_ENDPOINT *O,PDEVICE_OBJECT D,PIRP I,PUNKNOWN P,
 PRESOURCELIST R,T70_WAVECYCLIC_PORT_OWNER *W,const T70_TOPOLOGY_VOLUME_BINDING *B){
 if(!B)return STATUS_INVALID_PARAMETER;
 return T70AudioEndpointOpenUsing(O,D,I,P,R,W,CreateBound,B);
}
