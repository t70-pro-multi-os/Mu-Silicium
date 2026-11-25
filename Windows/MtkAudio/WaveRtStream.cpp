// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtStream.h"
#include <string.h>
T70WaveRtStream::T70WaveRtStream(const T70_WAVERT_STREAM_PROVIDER &B,PPORTWAVERTSTREAM S,
 const T70_AFE_PREPARED &P,const KSRTAUDIO_HWLATENCY &L)
 :T70WaveRtComStreamBase(B.Owner,P,&Transaction,S,B.Adapter,B.Afe),Transaction{},Provider(B),Port(S),Latency(L){Port->AddRef();}
T70WaveRtStream::~T70WaveRtStream(){Port->Release();}
NTSTATUS T70WaveRtStream::Create(void *C,PPORTWAVERTSTREAM Port,ULONG Pin,
 PKSDATAFORMAT F,PMINIPORTWAVERTSTREAM *Out){
 if(!Port||!F||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Pin)return STATUS_NOT_SUPPORTED;
 if(!C)return STATUS_DEVICE_NOT_READY;
 auto B=static_cast<const T70_WAVERT_STREAM_PROVIDER *>(C);
 if(!B->Owner||!B->Context||!B->Adapter||!B->Afe||!B->Describe||!B->Pin||!B->Unpin||
    !B->Verify||!B->Remove)return STATUS_DEVICE_NOT_READY;
 ULONG Bytes;memcpy(&Bytes,F,sizeof Bytes);
 if(Bytes!=104)return STATUS_NOT_SUPPORTED;
 T70_AFE_PREPARED P;
 if(T70WaveRtPrepareBufferFormat(F,Bytes,0,B->InitialBufferBytes,B->PeriodBytes,&P)!=T70WavePrepared)
  return STATUS_NOT_SUPPORTED;
 KSRTAUDIO_HWLATENCY L={};
 NTSTATUS S=B->Describe(B->Context,&P,&L);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 auto Object=new T70WaveRtStream(*B,Port,P,L);
 if(!Object)return STATUS_INSUFFICIENT_RESOURCES;
 *Out=Object;return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtStream::SetFormat(PKSDATAFORMAT F){
 if(!F||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 T70_AFE_PREPARED P,Next;
 if(!CopyPlan(&P))return STATUS_INVALID_DEVICE_STATE;
 ULONG Bytes;memcpy(&Bytes,F,sizeof Bytes);
 if(Bytes!=104||T70WaveRtPrepareBufferFormat(F,Bytes,P.DmaBase,P.BufferBytes,P.PeriodBytes,&Next)!=T70WavePrepared)
  return STATUS_NOT_SUPPORTED;
 // Latency/admission were established for this format, not a future one.
 if(Next.Rate!=P.Rate||Next.SampleFormat!=P.SampleFormat||Next.Channels!=P.Channels)return STATUS_NOT_SUPPORTED;
 return T70WaveRtBufferedStreamBase::SetFormat(F);
}
NTSTATUS T70WaveRtStream::GetPosition(PKSAUDIO_POSITION P){
 auto I=KeGetCurrentIrql();
 if(!P||(I!=PASSIVE_LEVEL&&I!=DISPATCH_LEVEL))return STATUS_INVALID_PARAMETER;
 // NewStream requires initial software STOP/zero position. Once any buffer
 // transaction is admitted, no hardware position is fabricated.
 if(Transaction.Terminal)return STATUS_DEVICE_NOT_READY;
 P->PlayOffset=P->WriteOffset=0;return STATUS_SUCCESS;
}
void T70WaveRtStream::GetHWLatency(KSRTAUDIO_HWLATENCY *L){
 // PortCls supplies valid output storage. Immutable provider-confirmed values;
 // units are FIFO bytes and chipset/codec 100 ns, not guessed zero defaults.
 *L=Latency;
}
NTSTATUS T70WaveRtStream::PinRecoveryLifetime(){
 AddRef();NTSTATUS S=Provider.Pin(Provider.Context);
 if(S!=STATUS_SUCCESS)Release();
 return S;
}
void T70WaveRtStream::UnpinRecoveryLifetime(){Provider.Unpin(Provider.Context);Release();}
NTSTATUS T70WaveRtStream::VerifyClientMapping(const T70_WAVERT_DMA_OWNER &D){return Provider.Verify(Provider.Context,&D);}
NTSTATUS T70WaveRtStream::RemoveHardwareReferences(){return Provider.Remove(Provider.Context);}
