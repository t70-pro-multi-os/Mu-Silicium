// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtStreamBase.h"
#include <string.h>
T70WaveRtStreamBase::T70WaveRtStreamBase(const T70_AFE_PREPARED &Initial):Plan{},Valid(false),ConfigurationAttempted(false){
 if(T70AfeDl1PreparedValid(&Initial)&&Initial.SampleFormat==T70AfeS16Le){Plan=Initial;Valid=true;}
}
NTSTATUS T70WaveRtStreamBase::SetFormat(PKSDATAFORMAT F){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!F)return STATUS_INVALID_PARAMETER;
 if(!Valid||ConfigurationAttempted)return STATUS_INVALID_DEVICE_STATE;
 /* PortCls supplies a valid readable format object; FormatSize is not a
  * substitute for proving arbitrary user/kernel pointer accessibility. */
 ULONG Size;memcpy(&Size,F,sizeof(Size));
 if(Size!=104)return STATUS_NOT_SUPPORTED;
 T70_AFE_PREPARED Next;
 if(T70WaveRtPrepareBufferFormat(F,Size,Plan.DmaBase,Plan.BufferBytes,Plan.PeriodBytes,&Next)!=T70WavePrepared)
  return STATUS_NOT_SUPPORTED;
 Plan=Next;return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtStreamBase::SetState(KSSTATE State){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!Valid)return STATUS_INVALID_DEVICE_STATE;
 if(State<KSSTATE_STOP||State>KSSTATE_RUN)return STATUS_INVALID_PARAMETER;
 /* This object has never admitted a higher state or touched hardware. STOP
  * acknowledges only this software state, never stopping another DMA owner. */
 return State==KSSTATE_STOP?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
}
NTSTATUS T70WaveRtStreamBase::GetPosition(PKSAUDIO_POSITION P){
 /* PortCls permits this query at PASSIVE_LEVEL or DISPATCH_LEVEL. No
  * passive-only mapping/lease routine may be introduced on this path.
  * Missing coherent play/write provider is readiness, not caller error. */
 const auto Irql=KeGetCurrentIrql();
 if(!P||(Irql!=PASSIVE_LEVEL&&Irql!=DISPATCH_LEVEL))return STATUS_INVALID_PARAMETER;
 return STATUS_DEVICE_NOT_READY;
}
NTSTATUS T70WaveRtStreamBase::GetPositionRegister(KSRTAUDIO_HWREGISTER *R){
 if(!R||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 return STATUS_NOT_SUPPORTED;
}
NTSTATUS T70WaveRtStreamBase::GetClockRegister(KSRTAUDIO_HWREGISTER *R){
 if(!R||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 return STATUS_NOT_SUPPORTED;
}
bool T70WaveRtStreamBase::CopyPlan(T70_AFE_PREPARED *Out)const{
 if(!Out||!Valid||KeGetCurrentIrql()!=PASSIVE_LEVEL)return false;
 *Out=Plan;return true;
}
