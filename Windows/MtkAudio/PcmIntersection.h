// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_PCM_INTERSECTION_H
#define T70_PCM_INTERSECTION_H
#include <portcls.h>
#include <stdint.h>
#include <ksmedia.h>
static_assert(sizeof(KSDATAFORMAT)==64,"KS header size");
static_assert(offsetof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE,WaveFormatExt)==64,"wave offset");
static_assert(sizeof(WAVEFORMATEXTENSIBLE)==40,"wave payload size");
#include <string.h>
static inline void Put16(unsigned char *B,ULONG V){B[0]=(unsigned char)V;B[1]=(unsigned char)(V>>8);}
static inline void Put32(unsigned char *B,ULONG V){Put16(B,V);Put16(B+2,V>>16);}
static inline NTSTATUS T70PcmIntersection(const KSDATARANGE_AUDIO (&Ranges)[11],
 const void *Owner,size_t OwnerSize,bool Ready,ULONG RequestedPin,PKSDATARANGE Client,
 PKSDATARANGE Matching,ULONG Capacity,void *Output,PULONG Length){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Client||!Matching||!Length||(!Output&&Capacity))return STATUS_INVALID_PARAMETER;
 if(!Ready)return STATUS_DEVICE_NOT_READY;
 if(RequestedPin!=0)return STATUS_INVALID_PARAMETER;
 if(Client->FormatSize!=sizeof(KSDATARANGE_AUDIO)||Matching->FormatSize!=sizeof(KSDATARANGE_AUDIO))return STATUS_NO_MATCH;
 // Caller/PortCls supplies readable range objects, not untrusted raw pointers.
 KSDATARANGE_AUDIO C,M;memcpy(&C,Client,sizeof C);memcpy(&M,Matching,sizeof M);
 const KSDATARANGE_AUDIO *Selected=nullptr;
 for(unsigned I=0;I<11;I++){
  const auto &R=Ranges[I];
  if(!memcmp(&M.DataRange,&R.DataRange,sizeof M.DataRange)&&M.MaximumChannels==R.MaximumChannels&&
     M.MinimumBitsPerSample==16&&M.MaximumBitsPerSample==16&&
     M.MinimumSampleFrequency==R.MinimumSampleFrequency&&M.MaximumSampleFrequency==R.MaximumSampleFrequency){Selected=&R;break;}
 }
 if(!Selected)return STATUS_NO_MATCH;
 const auto &Header=Selected->DataRange;
 if(C.DataRange.Flags||C.DataRange.Reserved||memcmp(&C.DataRange.MajorFormat,&Header.MajorFormat,sizeof(GUID))||
    memcmp(&C.DataRange.SubFormat,&Header.SubFormat,sizeof(GUID))||memcmp(&C.DataRange.Specifier,&Header.Specifier,sizeof(GUID))||
    !C.MaximumChannels||C.MinimumBitsPerSample>C.MaximumBitsPerSample||C.MinimumBitsPerSample>16||C.MaximumBitsPerSample<16||
    C.MinimumSampleFrequency>C.MaximumSampleFrequency||C.MinimumSampleFrequency>Selected->MinimumSampleFrequency||
    C.MaximumSampleFrequency<Selected->MinimumSampleFrequency)return STATUS_NO_MATCH;
 ULONG Channels=C.MaximumChannels>=2?2:1;
 // A fixed two-byte PCM frame constrains the intersection to mono even if
 // the client permits two channels. Apply all constraints before choosing.
 if(C.DataRange.SampleSize){
  if(C.DataRange.SampleSize!=2&&C.DataRange.SampleSize!=4)return STATUS_NO_MATCH;
  Channels=C.DataRange.SampleSize/2;
  if(Channels>C.MaximumChannels)return STATUS_NO_MATCH;
 }
 ULONG Align=Channels*2,Rate=Selected->MinimumSampleFrequency;
 // Neither output may overlap an input, the object, or the other output.
 uintptr_t A[]={(uintptr_t)Client,(uintptr_t)Matching,(uintptr_t)Owner,(uintptr_t)Length,(uintptr_t)Output};
 size_t N[]={sizeof C,sizeof M,OwnerSize,sizeof(*Length),Capacity?104u:0u};
 for(unsigned I=0;I<5;I++)if(N[I]>UINTPTR_MAX-A[I])return STATUS_INVALID_PARAMETER;
 for(unsigned I=3;I<5;I++)if(N[I])for(unsigned J=0;J<I;J++)
  if(N[J]&&A[I]<A[J]+N[J]&&A[J]<A[I]+N[I])return STATUS_INVALID_PARAMETER;
 if(!Capacity){*Length=104;return STATUS_BUFFER_OVERFLOW;}
 if(Capacity<104)return STATUS_BUFFER_TOO_SMALL;
 unsigned char B[104]={};Put32(B,104);Put32(B+8,Align);
 memcpy(B+16,&Header.MajorFormat,16);memcpy(B+32,&Header.SubFormat,16);memcpy(B+48,&Header.Specifier,16);
 Put16(B+64,0xfffe);Put16(B+66,Channels);Put32(B+68,Rate);Put32(B+72,Rate*Align);
 Put16(B+76,Align);Put16(B+78,16);Put16(B+80,22);Put16(B+82,16);Put32(B+84,Channels==1?4:3);
 memcpy(B+88,&Header.SubFormat,16);
 memcpy(Output,B,sizeof B);*Length=sizeof B;return STATUS_SUCCESS;
}

#endif
