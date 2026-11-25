#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "UsbMass.h"
UINT64 EFIAPI MultU64x32(UINT64 A,UINT32 B){return A*B;}
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
static UINT64 Now,Deadline;static UINTN Calls,CloseCount,ReadyAfter;static UINT8 Asc;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID T70UsbStorageDiagnostic(EFI_HANDLE H,UINT32 S,EFI_STATUS E,UINT8 O,UINT8 L,UINT32 R,UINT8 K,UINT8 A,UINT8 Q){}
static EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY F,VOID *C,EFI_EVENT *E){*E=(VOID*)1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY T,UINT64 D){Deadline=Now+D;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Check(EFI_EVENT E){return Now>=Deadline?EFI_SUCCESS:EFI_NOT_READY;}
static EFI_STATUS EFIAPI Close(EFI_EVENT E){CloseCount++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Exec(VOID *C,VOID *Cmd,UINT8 Len,EFI_USB_DATA_DIRECTION Dir,VOID *Data,UINT32 Size,UINT8 Lun,UINT32 Timeout,UINT32 *Result){
 Now+=1000000; /* model 100 ms per command, independent of retry loop */
 *Result=USB_MASS_CMD_SUCCESS;
 if(*(UINT8*)Cmd==3){USB_BOOT_REQUEST_SENSE_DATA *S=Data;memset(S,0,Size);S->SenseKey=2;S->Asc=Asc;return EFI_SUCCESS;}
 Calls++;if(ReadyAfter && Calls>=ReadyAfter)return EFI_SUCCESS;
 *Result=USB_MASS_CMD_FAIL;return EFI_SUCCESS;
}
EFI_STATUS UsbBootExecCmdWithRetry(USB_MASS_DEVICE*,VOID*,UINT8,EFI_USB_DATA_DIRECTION,VOID*,UINT32,UINT32);
#define CHECK(X) do{if(!(X)){fprintf(stderr,"failed line %d\n",__LINE__);exit(1);}}while(0)
static EFI_STATUS Run(UINT8 Sense,UINTN Recover){
 Now=Calls=CloseCount=0;ReadyAfter=Recover;Asc=Sense;
 USB_MASS_TRANSPORT T={0};T.ExecCommand=Exec;USB_MASS_DEVICE D={0};D.Transport=&T;UINT8 Cmd[6]={0};
 EFI_STATUS S=UsbBootExecCmdWithRetry(&D,Cmd,6,EfiUsbNoData,NULL,0,5000000);
 CHECK(CloseCount==1);return S;
}
int main(void){Services.CreateEvent=Create;Services.SetTimer=Timer;Services.CheckEvent=Check;Services.CloseEvent=Close;
 CHECK(Run(4,0)==EFI_NOT_READY);CHECK(Now<=22000000);CHECK(Calls==10);
 CHECK(Run(4,4)==EFI_SUCCESS);CHECK(Calls==4);
 CHECK(Run(0x3a,0)==EFI_NO_MEDIA);CHECK(Calls==1);
 puts("PASS: permanent NOT_READY bounded, recovering medium succeeds, NO_MEDIA exits immediately; actual UsbMassBoot.c, modeled transport and timer");return 0;}
