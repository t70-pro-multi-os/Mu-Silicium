// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbProfileVolume.h"
#include "AwSpbProfileSerial.h"
#include <spb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static unsigned Checks,Calls,Fail,Kind,Irql,Serial;static uint16_t SerialValues[2];static uint16_t Existing,Expected,Id=0x2113;
#define C(X) do{++Checks;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
static NTSTATUS Status(ULONG_PTR *N){++Calls;*N=Calls==Fail&&Kind==2?2:3;return Calls==Fail&&Kind==1?STATUS_IO_TIMEOUT:Calls==Fail&&Kind==3?STATUS_DEVICE_NOT_READY:0;}
NTSTATUS WdfIoTargetSendIoctlSynchronously(WDFIOTARGET T,void *R,unsigned Code,WDF_MEMORY_DESCRIPTOR *In,WDF_MEMORY_DESCRIPTOR *Out,WDF_REQUEST_SEND_OPTIONS *O,ULONG_PTR *Bytes){
 C(T==(void *)1&&!R&&!Out&&Code==IOCTL_SPB_EXECUTE_SEQUENCE&&O->Timeout==-500000);
 SPB_TRANSFER_LIST *L=In->Buffer;C(In->Length==sizeof(*L)&&L->Count==2);
 C(L->Transfers[0].Length==1&&L->Transfers[1].Length==2);
 C(L->Transfers[0].Buffer[0]==(Calls==0?0:Serial?(Calls==1?6:7):5));uint16_t V=Calls==0?Id:Existing;
 L->Transfers[1].Buffer[0]=(UCHAR)(V>>8);L->Transfers[1].Buffer[1]=(UCHAR)V;return Status(Bytes);
}
NTSTATUS WdfIoTargetSendWriteSynchronously(WDFIOTARGET T,void *R,WDF_MEMORY_DESCRIPTOR *In,LONGLONG *Offset,WDF_REQUEST_SEND_OPTIONS *O,ULONG_PTR *Bytes){
 C(T==(void *)1&&!R&&!Offset&&In->Length==3&&O->Timeout==-500000&&(Serial?(Calls==2||Calls==4):Calls==2));
 const UCHAR *P=In->Buffer;
 uint16_t E=Serial?(uint16_t)((Existing&~(Calls==2?0x0fff:0x7fff))|(SerialValues[Calls==2?0:1]&(Calls==2?0x0fff:0x7fff))):Expected;
 C(P[0]==(Serial?(Calls==2?6:7):5)&&P[1]==(E>>8)&&P[2]==(E&255));return Status(Bytes);
}
static uint32_t U32(const uint8_t *P){return P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);}
static void Put32(uint8_t *P,uint32_t V){for(unsigned I=0;I<4;I++)P[I]=(uint8_t)(V>>(8*I));}
static uint8_t Crc(const uint8_t *P,size_t N){uint8_t V=0;for(size_t I=0;I<N;I++){V^=P[I];for(unsigned J=0;J<8;J++)V=(uint8_t)((V>>1)^((V&1)?0x8c:0));}return V;}
static void Repair(uint8_t *B,unsigned D){uint8_t *H=B+80+64*D;uint32_t Offset=U32(H+36),Len=U32(H+32),Sum=0;for(uint32_t I=4;I<Len;I++)Sum+=B[Offset+I];Put32(B+Offset,Sum);H[40]=Crc(B+Offset,Len);}
static T70_AW_PROFILE_VOLUME_RESULT Apply(const void *B,size_t N,unsigned Ch,unsigned Profile,uint32_t Requested,uint16_t *Out){return T70AwSpbApplyProfileVolume(B,N,0x11e01000,(uint16_t)(0x34+Ch),(uint16_t)Ch,(uint16_t)Profile,(void *)1,Requested,Out);}
int main(int argc,char **argv){
 C(argc==2);FILE *F=fopen(argv[1],"rb");C(F);uint8_t B[3857],Copy[3857];size_t N=fread(B,1,sizeof B,F);fclose(F);C(N==3856);
 for(unsigned Ch=0;Ch<2;Ch++)for(unsigned Profile=0;Profile<=10;Profile+=10){
  T70_AW_PROFILE_VIEW V;C(T70AwProfileView(B,N,0x11e01000,(uint16_t)(0x34+Ch),(uint16_t)Ch,(uint16_t)Profile,&V));
  uint16_t Raw=0;for(unsigned I=0;I<V.RecordCount;I++){const uint8_t *P=V.Section.Data+64+I*4;if(P[0]==5&&!P[1])Raw=(uint16_t)(P[2]|((uint16_t)P[3]<<8));}
  for(unsigned Request=0;Request<=720;Request+=120)for(unsigned Bits=0;Bits<64;Bits++){
   Existing=(uint16_t)((Bits<<10)|0x2d5);Expected=T70Aw88261ComposeVolume(Existing,T70Aw88261DecodeInitialVolume(Raw),Request);Calls=Fail=Kind=0;uint16_t Out=0xbeef;
   T70_AW_PROFILE_VOLUME_RESULT R=Apply(B,N,Ch,Profile,Request,&Out);C(R.Code==T70AwProfileVolumeApplied&&R.WriteAttempted==1&&R.TransportStatus==0&&Calls==3&&Out==Expected);
  }
  for(unsigned K=1;K<=3;K++)for(unsigned At=1;At<=3;At++){
   Calls=0;Kind=K;Fail=At;Expected=T70Aw88261ComposeVolume(Existing,T70Aw88261DecodeInitialVolume(Raw),0);uint16_t Out=0xbeef;
   T70_AW_PROFILE_VOLUME_RESULT R=Apply(B,N,Ch,Profile,0,&Out);C(Calls==At&&Out==0xbeef&&R.WriteAttempted==(At==3));
   C(R.Code==(At==1?T70AwProfileVolumeIdReadFailed:At==2?T70AwProfileVolumeReadFailed:T70AwProfileVolumeWriteFailed));
   C(R.TransportStatus==(K==1?STATUS_IO_TIMEOUT:K==2?STATUS_DEVICE_DATA_ERROR:STATUS_DEVICE_NOT_READY));
  }
  Kind=Fail=0;Calls=0;Id=0;uint16_t Out=0xbeef;C(Apply(B,N,Ch,Profile,0,&Out).Code==T70AwProfileVolumeWrongId&&Calls==1&&Out==0xbeef);Id=0x2113;
 }
 for(size_t Len=0;Len<N;Len++){Calls=0;uint16_t Out=7;C(Apply(B,Len,0,0,0,&Out).Code!=T70AwProfileVolumeApplied&&!Calls&&Out==7);}
 _Alignas(2) unsigned char Misaligned[4]={0};
 for(unsigned Bad=0;Bad<12;Bad++){
  Calls=0;Irql=Bad==0?1:0;uint16_t Out=7;
  T70_AW_PROFILE_VOLUME_RESULT R=T70AwSpbApplyProfileVolume(Bad==11?NULL:B,Bad==10?SIZE_MAX:N,Bad==1?0:0x11e01000,Bad==2?0x35:0x34,Bad==3?2:0,Bad==4?1:0,Bad==5?NULL:(void *)1,Bad==6?721:0,Bad==7?NULL:Bad==8?(uint16_t *)B:Bad==9?(uint16_t *)(void *)(Misaligned+1):&Out);
  C(R.Code!=T70AwProfileVolumeApplied&&!Calls&&Out==7);
 }Irql=0;
 // Valid CRC/checksum with missing or duplicated volume record: reject before I/O.
 for(unsigned D=0;D<12;D++){
  const uint8_t *H=B+80+D*64;if(H[20]>1||U32(H+28)!=4)continue;
  uint32_t Offset=U32(H+36);unsigned Vol=0,Other=0;
  for(unsigned I=0;I<57;I++){const uint8_t *P=B+Offset+64+4*I;if(P[0]==5&&!P[1])Vol=I;else Other=I;}
  for(unsigned Dup=0;Dup<2;Dup++){
   memcpy(Copy,B,N);uint8_t *P=Copy+Offset+64+4*(Dup?Other:Vol);P[0]=Dup?5:0x7f;P[1]=0;Repair(Copy,D);Calls=0;uint16_t Out=7;
   C(Apply(Copy,N,H[20],H[26],0,&Out).Code==T70AwProfileVolumeBadProfile&&!Calls&&Out==7);
  }
 }
 Serial=1;
 for(unsigned Profile=0;Profile<=10;Profile+=10)for(unsigned Ch=0;Ch<2;Ch++){
  T70_AW_PAIR_SERIAL V;C(T70AwPairSerialObserve(B,N,0x11e01000,0x34,0x35,(uint16_t)Profile,&V));
  Calls=0;C(T70AwSpbValidateProfileSerial(B,N,0x11e01000,(uint16_t)(0x34+Ch),(uint16_t)Ch,(uint16_t)Profile));C(!Calls);
  SerialValues[0]=V.Channel[Ch].Reg06;SerialValues[1]=V.Channel[Ch].Reg07;
  for(unsigned Upper=0;Upper<16;Upper++){
   Calls=Fail=Kind=0;Existing=(uint16_t)(Upper*0x1111);
   T70_AW_SERIAL_APPLY_RESULT R=T70AwSpbApplyProfileSerial(B,N,0x11e01000,(uint16_t)(0x34+Ch),(uint16_t)Ch,(uint16_t)Profile,(void *)1);
   C(R.Code==T70AwSerialFieldsWritten&&Calls==5&&R.WritesAttempted==2&&R.WritesCompleted==2&&!R.FailedRegister);
  }
  for(unsigned At=1;At<=5;At++)for(unsigned K=1;K<=2;K++){
   Calls=0;Fail=At;Kind=K;
   T70_AW_SERIAL_APPLY_RESULT R=T70AwSpbApplyProfileSerial(B,N,0x11e01000,(uint16_t)(0x34+Ch),(uint16_t)Ch,(uint16_t)Profile,(void *)1);
   C(Calls==At&&R.WritesAttempted==(At>=3)+(At>=5)&&R.WritesCompleted==(At>3));
   C(R.Code==(At==1?T70AwSerialIdFailed:(At==3||At==5)?T70AwSerialWriteFailed:T70AwSerialReadFailed));
   C(R.TransportStatus==(K==1?STATUS_IO_TIMEOUT:STATUS_DEVICE_DATA_ERROR));
  }
 }
 Calls=Fail=Kind=0;Id=0;C(T70AwSpbApplyProfileSerial(B,N,0x11e01000,0x34,0,0,(void *)1).Code==T70AwSerialWrongId&&Calls==1);Id=0x2113;
 for(size_t Len=0;Len<N;Len++){Calls=0;C(T70AwSpbApplyProfileSerial(B,Len,0x11e01000,0x34,0,0,(void *)1).Code!=T70AwSerialFieldsWritten&&!Calls);}
 for(unsigned D=0;D<12;D++){
  const uint8_t *H=B+80+D*64;if(H[20]>1||U32(H+28)!=4)continue;
  uint32_t O=U32(H+36);unsigned A=0,Z=0;
  for(unsigned I=0;I<57;I++){if(B[O+64+4*I]==6)A=I;if(B[O+64+4*I]==7)Z=I;}
  for(unsigned Bad=0;Bad<3;Bad++){
   memcpy(Copy,B,N);
   if(Bad==0)Copy[O+64+4*A+3]|=3; // invalid mode encoding
   if(Bad==1){uint8_t T[4];memcpy(T,Copy+O+64+4*A,4);memcpy(Copy+O+64+4*A,Copy+O+64+4*Z,4);memcpy(Copy+O+64+4*Z,T,4);}
   if(Bad==2)Copy[O+64+4*A]=7; // duplicate07/missing06
   Repair(Copy,D);Calls=0;
   C(!T70AwSpbValidateProfileSerial(Copy,N,0x11e01000,(uint16_t)(0x34+H[20]),H[20],H[26]));C(!Calls);
   C(T70AwSpbApplyProfileSerial(Copy,N,0x11e01000,(uint16_t)(0x34+H[20]),H[20],H[26],(void *)1).Code==T70AwSerialBadProfile&&!Calls);
  }
 }
 for(unsigned Bad=0;Bad<8;Bad++){
  Calls=0;Irql=Bad==0?1:0;
  T70_AW_SERIAL_APPLY_RESULT R=T70AwSpbApplyProfileSerial(Bad==1?NULL:B,Bad==2?SIZE_MAX:N,
   Bad==3?0:0x11e01000,Bad==4?0x35:0x34,Bad==5?2:0,Bad==6?1:0,Bad==7?NULL:(void *)1);
  C(R.Code!=T70AwSerialFieldsWritten&&!Calls&&!R.WritesAttempted&&!R.WritesCompleted);
 }Irql=0;
 printf("SpbProfileVolume: %u checks; volume+serial actual transport with fake WDF, no hardware\n",Checks);
}
