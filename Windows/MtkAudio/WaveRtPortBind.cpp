// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtPortBind.h"
NTSTATUS T70WaveRtPortBind(PDEVICE_OBJECT Device,PIRP Irp,PUNKNOWN Miniport,
 PUNKNOWN Adapter,PRESOURCELIST Resources,PPORT *Out){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Device||!Irp||!Miniport||!Resources||!Out)
  return STATUS_INVALID_PARAMETER;
 // CLSID_PortWaveRT from the official portcls.h; local value avoids requiring
 // a separate GUID-definition translation unit in the integrating driver.
 static const GUID WaveRt={0xcc9be57a,0xeb9e,0x42b4,{0x94,0xfc,0x0c,0xad,0x3d,0xbc,0xe7,0xfa}};
 PPORT Port=nullptr;
 NTSTATUS S=PcNewPort(&Port,WaveRt);
 if(!NT_SUCCESS(S))return S; // Official failed out-interface is not owned.
 if(!Port)return STATUS_DEVICE_NOT_READY;
 S=Port->Init(Device,Irp,Miniport,Adapter,Resources);
 if(!NT_SUCCESS(S)){Port->Release();return S;}
 *Out=Port;return STATUS_SUCCESS;
}
