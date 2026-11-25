// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpb.h"
#include "AwProbe.h"
#include <spb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Calls,Irql; static NTSTATUS Result;
static ULONG_PTR Completed; static UCHAR Address; static USHORT Value;
static int MaskMode; static USHORT ReadValue; static NTSTATUS WriteResult;
static ULONG_PTR WriteCompleted;
static WDFIOTARGET Target=(void *)(uintptr_t)0x1234;
#define CHECK(X) do { Checks++; if(!(X)) {fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();} } while(0)
unsigned char KeGetCurrentIrql(void) {return (unsigned char)Irql;}
static void Common(WDFIOTARGET T,void *Request,WDF_REQUEST_SEND_OPTIONS *Options) {
  Calls++; CHECK(T==Target && Request==NULL);
  CHECK(Options->Flags==WDF_REQUEST_SEND_OPTION_TIMEOUT && Options->Timeout==-500000);
}
NTSTATUS WdfIoTargetSendWriteSynchronously(WDFIOTARGET T,void *R,WDF_MEMORY_DESCRIPTOR *In,LONGLONG *Offset,WDF_REQUEST_SEND_OPTIONS *O,ULONG_PTR *Bytes) {
  Common(T,R,O); CHECK(Offset==NULL && In->Length==3);
  UCHAR *B=In->Buffer; CHECK(B[0]==Address && B[1]==(Value>>8) && B[2]==(Value&255));
  *Bytes=MaskMode?WriteCompleted:Completed; return MaskMode?WriteResult:Result;
}
NTSTATUS WdfIoTargetSendIoctlSynchronously(WDFIOTARGET T,void *R,unsigned Code,WDF_MEMORY_DESCRIPTOR *In,WDF_MEMORY_DESCRIPTOR *Out,WDF_REQUEST_SEND_OPTIONS *O,ULONG_PTR *Bytes) {
  Common(T,R,O); CHECK(Code==IOCTL_SPB_EXECUTE_SEQUENCE && Out==NULL);
  SPB_TRANSFER_LIST *L=In->Buffer;
  CHECK(In->Length==sizeof(*L) && L->Count==2);
  CHECK(L->Transfers[0].Direction==SpbTransferDirectionToDevice && L->Transfers[0].Length==1);
  CHECK(L->Transfers[1].Direction==SpbTransferDirectionFromDevice && L->Transfers[1].Length==2);
  CHECK(L->Transfers[0].Delay==0 && L->Transfers[1].Delay==0 && L->Transfers[0].Buffer[0]==Address);
  USHORT V=MaskMode?ReadValue:Value;
  L->Transfers[1].Buffer[0]=(UCHAR)(V>>8); L->Transfers[1].Buffer[1]=(UCHAR)V;
  *Bytes=Completed; return Result;
}
static unsigned ProbePhase,ProbeFailurePhase,ProbeFailure;
static int32_t ProbeRead(void *Context,uint16_t Reg,uint16_t *Out) {
  CHECK(Context==Target && Reg==ProbePhase && ProbePhase<2);
  Address=(UCHAR)Reg;ReadValue=Reg?0x11:0x2113;
  Result=(ProbePhase==ProbeFailurePhase&&ProbeFailure==1)?STATUS_IO_TIMEOUT:STATUS_SUCCESS;
  Completed=(ProbePhase==ProbeFailurePhase&&ProbeFailure==2)?2:3;
  ProbePhase++;
  return T70AwSpbReadRegister(Context,Reg,Out);
}
int main(void) {
  Completed=3;
  for(unsigned V=0;V<65536;V++) {
    Value=(USHORT)V; Address=(UCHAR)V; USHORT Out=0;
    CHECK(T70AwSpbWriteRegister(Target,Address,Value)==STATUS_SUCCESS);
    CHECK(T70AwSpbReadRegister(Target,Address,&Out)==STATUS_SUCCESS && Out==Value);
  }
  for(unsigned Failure=0;Failure<2;Failure++) for(unsigned N=0;N<5;N++) {
    Result=Failure?STATUS_IO_TIMEOUT:STATUS_SUCCESS; Completed=N;
    USHORT Out=0xdead; unsigned Before=Calls;
    NTSTATUS Expected=Failure?Result:(N==3?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR);
    CHECK(T70AwSpbWriteRegister(Target,Address,Value)==Expected && Calls==Before+1);
    CHECK(T70AwSpbReadRegister(Target,Address,&Out)==Expected && Calls==Before+2);
    CHECK(Out==(Expected==STATUS_SUCCESS?Value:0xdead));
  }
  unsigned Before=Calls; USHORT Out=0xdead;
  CHECK(T70AwSpbWriteRegister(NULL,0,0)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbReadRegister(NULL,0,&Out)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbReadRegister(Target,0,NULL)==STATUS_INVALID_PARAMETER);
  for(unsigned R=256;R<65536;R++) {
    CHECK(T70AwSpbWriteRegister(Target,(USHORT)R,0)==STATUS_INVALID_PARAMETER);
    CHECK(T70AwSpbReadRegister(Target,(USHORT)R,&Out)==STATUS_INVALID_PARAMETER);
  }
  Irql=1;
  CHECK(T70AwSpbWriteRegister(Target,0,0)==STATUS_INVALID_DEVICE_STATE);
  CHECK(T70AwSpbReadRegister(Target,0,&Out)==STATUS_INVALID_DEVICE_STATE);
  CHECK(Calls==Before && Out==0xdead);
  Irql=0; MaskMode=1; Address=0x44; ReadValue=0xa5a5;
  for(unsigned F=0;F<5;F++) {
    Result=F==1?STATUS_IO_TIMEOUT:STATUS_SUCCESS;
    Completed=F==2?2:3;
    WriteResult=F==3?STATUS_IO_TIMEOUT:STATUS_SUCCESS;
    WriteCompleted=F==4?2:3;
    Value=0xa53c; Before=Calls;
    NTSTATUS Expected=(F==1 || F==3)?STATUS_IO_TIMEOUT:
                       (F==2 || F==4)?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
    CHECK(T70AwSpbUpdatePreservedBits(Target,Address,0xff00,0x123c)==Expected);
    CHECK(Calls==Before+((F==1 || F==2)?1U:2U));
  }
  Result=WriteResult=STATUS_SUCCESS;Completed=WriteCompleted=3;
  Value=ReadValue; Before=Calls;
  CHECK(T70AwSpbUpdatePreservedBits(Target,Address,0xffff,0)==STATUS_SUCCESS && Calls==Before+2);
  Before=Calls;Irql=1;
  CHECK(T70AwSpbUpdatePreservedBits(Target,Address,0,0)==STATUS_INVALID_DEVICE_STATE);
  CHECK(T70AwSpbUpdatePreservedBits(NULL,Address,0,0)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbUpdatePreservedBits(Target,256,0,0)==STATUS_INVALID_PARAMETER && Calls==Before);
  Irql=0; Address=0;
  for(unsigned Id=0;Id<65536;Id++) {
    ReadValue=(USHORT)Id;Result=STATUS_SUCCESS;Completed=3;USHORT OutId=0xbeef;Before=Calls;
    CHECK(T70AwSpbIdentify88261(Target,&OutId)==(Id==0x2113?STATUS_SUCCESS:STATUS_NO_SUCH_DEVICE));
    CHECK(OutId==(Id==0x2113?0x2113:0xbeef) && Calls==Before+1);
  }
  ReadValue=0x2113;
  for(unsigned F=0;F<2;F++)for(unsigned N=0;N<5;N++){
    Result=F?STATUS_IO_TIMEOUT:STATUS_SUCCESS;Completed=N;USHORT OutId=0xbeef;Before=Calls;
    NTSTATUS E=F?Result:(N==3?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR);
    CHECK(T70AwSpbIdentify88261(Target,&OutId)==E);
    CHECK(OutId==(E==STATUS_SUCCESS?0x2113:0xbeef) && Calls==Before+1);
  }
  Before=Calls;USHORT OutId=0xbeef;Irql=1;
  CHECK(T70AwSpbIdentify88261(Target,&OutId)==STATUS_INVALID_DEVICE_STATE);
  CHECK(T70AwSpbIdentify88261(NULL,&OutId)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbIdentify88261(Target,NULL)==STATUS_INVALID_PARAMETER);
  CHECK(OutId==0xbeef && Calls==Before);
  Irql=0;Address=5;ReadValue=0xa555;Value=0xa440;
  for(unsigned F=0;F<5;F++){
    Result=F==1?STATUS_IO_TIMEOUT:STATUS_SUCCESS;Completed=F==2?2:3;
    WriteResult=F==3?STATUS_IO_TIMEOUT:STATUS_SUCCESS;WriteCompleted=F==4?2:3;
    USHORT Applied=0xbeef;Before=Calls;
    NTSTATUS E=(F==1||F==3)?STATUS_IO_TIMEOUT:(F==2||F==4)?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
    CHECK(T70AwSpbUpdateVolume(Target,47,1,&Applied)==E);
    CHECK(Applied==(E==STATUS_SUCCESS?Value:0xbeef));
    CHECK(Calls==Before+((F==1||F==2)?1U:2U));
  }
  Before=Calls;USHORT Applied=0xbeef;
  CHECK(T70AwSpbUpdateVolume(Target,784,0,&Applied)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbUpdateVolume(Target,0,721,&Applied)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbUpdateVolume(Target,0,0,NULL)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbUpdateVolume(NULL,0,0,&Applied)==STATUS_INVALID_PARAMETER);
  CHECK(Calls==Before && Applied==0xbeef);
  Address=8;ReadValue=0xa555;Value=0xa515;
  for(unsigned F=0;F<5;F++){
    Result=F==1?STATUS_IO_TIMEOUT:STATUS_SUCCESS;Completed=F==2?2:3;
    WriteResult=F==3?STATUS_IO_TIMEOUT:STATUS_SUCCESS;WriteCompleted=F==4?2:3;
    Applied=0xbeef;Before=Calls;
    NTSTATUS E=(F==1||F==3)?STATUS_IO_TIMEOUT:(F==2||F==4)?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
    CHECK(T70AwSpbUpdateTx(Target,0,&Applied)==E);
    CHECK(Applied==(E==STATUS_SUCCESS?Value:0xbeef));
    CHECK(Calls==Before+((F==1||F==2)?1U:2U));
  }
  Result=WriteResult=STATUS_SUCCESS;Completed=WriteCompleted=3;
  Value=0xa555;Applied=0xbeef;
  CHECK(T70AwSpbUpdateTx(Target,1,&Applied)==STATUS_SUCCESS && Applied==Value);
  Before=Calls;Applied=0xbeef;
  CHECK(T70AwSpbUpdateTx(Target,2,&Applied)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbUpdateTx(Target,0,NULL)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbUpdateTx(NULL,0,&Applied)==STATUS_INVALID_PARAMETER);
  Irql=1;CHECK(T70AwSpbUpdateTx(Target,0,&Applied)==STATUS_INVALID_DEVICE_STATE);
  CHECK(Calls==Before && Applied==0xbeef);
  Irql=0;Address=1;ReadValue=0xffff;
  for(unsigned F=0;F<2;F++)for(unsigned N=0;N<5;N++){
    Result=F?STATUS_IO_TIMEOUT:STATUS_SUCCESS;Completed=N;
    T70_AW_CLOCK_SNAPSHOT Snapshot,BeforeSnapshot;
    memset(&Snapshot,0xa5,sizeof(Snapshot));memcpy(&BeforeSnapshot,&Snapshot,sizeof(Snapshot));
    Before=Calls;NTSTATUS E=F?Result:(N==3?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR);
    CHECK(T70AwSpbReadClockStatus(Target,&Snapshot)==E && Calls==Before+1);
    if(E==STATUS_SUCCESS)CHECK(Snapshot.Raw==0xffff && Snapshot.ClockPairSet==1 && Snapshot.UninterpretedBits==0xffee);
    else CHECK(memcmp(&Snapshot,&BeforeSnapshot,sizeof(Snapshot))==0);
  }
  Before=Calls;T70_AW_CLOCK_SNAPSHOT Snapshot;
  CHECK(T70AwSpbReadClockStatus(NULL,&Snapshot)==STATUS_INVALID_PARAMETER);
  CHECK(T70AwSpbReadClockStatus(Target,NULL)==STATUS_INVALID_PARAMETER);
  Irql=1;CHECK(T70AwSpbReadClockStatus(Target,&Snapshot)==STATUS_INVALID_DEVICE_STATE);
  CHECK(Calls==Before);
  Irql=0;MaskMode=1;
  for(unsigned Ch=0;Ch<2;Ch++)for(unsigned Phase=0;Phase<2;Phase++)for(unsigned F=0;F<3;F++) {
    ProbePhase=0;ProbeFailurePhase=Phase;ProbeFailure=F;Before=Calls;
    T70_AW_STATUS_IO Io={Target,ProbeRead};T70_AW_PROBE_SNAPSHOT S;
    memset(&S,0xa5,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
    T70_AW_PROBE_RESULT R=T70Aw88261Probe(0x11e01000,(uint16_t)(0x34+Ch),(uint16_t)Ch,&Io,&S);
    if(!F)CHECK(R.Code==T70AwProbeRead&&S.ChipId==0x2113&&S.Clock.Raw==0x11&&Calls==Before+2);
    else {
      CHECK(R.Code==(Phase?T70AwProbeStatusReadFailed:T70AwProbeIdReadFailed));
      CHECK(R.TransportStatus==(F==1?STATUS_IO_TIMEOUT:STATUS_DEVICE_DATA_ERROR));
      CHECK(Calls==Before+Phase+1&&!memcmp(&S,Old,sizeof S));
    }
    ProbePhase=0;Before=Calls;
    CHECK(T70Aw88261Probe(0x11e01000,0x35,0,&Io,&S).Code==T70AwProbeInvalid);
    CHECK(Calls==Before&&ProbePhase==0);
  }
  printf("AW SPB: %u checks passed\n",Checks); return 0;
}
