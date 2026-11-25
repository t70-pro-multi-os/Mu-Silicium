// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeMappedIo.h"
#include "AfeConfigure.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks,Reads,Writes;
static ULONG Ram[2048];
#define C(X) do{assert(X);++Checks;}while(0)
/* RAM-backed host substitutes, NOT implementations of Windows memory barriers. */
ULONG READ_REGISTER_ULONG(volatile ULONG *P){C(P>=Ram && P<Ram+2048);++Reads;return *P;}
void WRITE_REGISTER_ULONG(volatile ULONG *P,ULONG V){C(P>=Ram && P<Ram+2048);++Writes;*P=V;}
int main(void){
 T70_AFE_MAPPED_IO M={0x11210000,Ram,sizeof Ram};uint32_t V;
 for(uint32_t O=0;O<0x2000;O+=4){
 C(!T70AfeMappedWrite32(&M,O,~O));V=0;C(!T70AfeMappedRead32(&M,O,&V));C(V==~O);
 }
 C(Reads==2048 && Writes==2048);
#define BAD(Offset) do{unsigned R=Reads,W=Writes;V=0x1234;C(T70AfeMappedRead32(&M,Offset,&V)!=0);C(T70AfeMappedWrite32(&M,Offset,7)!=0);C(V==0x1234 && R==Reads && W==Writes);}while(0)
 for(uint32_t O=0;O<0x2000;O++)if(O&3)BAD(O);
 BAD(0x2000);BAD(UINT32_MAX);BAD(0xfffffffc);
 M.PhysicalBase++;BAD(0);M.PhysicalBase--;
 M.Length=0x1fff;BAD(0);M.Length=0x2001;BAD(0);M.Length=SIZE_MAX;BAD(0);M.Length=sizeof Ram;
 M.VirtualBase=(char *)Ram+1;BAD(0);M.VirtualBase=(void *)(UINTPTR_MAX-3);BAD(0);M.VirtualBase=NULL;BAD(0);M.VirtualBase=Ram;
 unsigned R=Reads,W=Writes;V=0x1234;
 C(T70AfeMappedRead32(NULL,0,&V)!=0);C(T70AfeMappedWrite32(NULL,0,7)!=0);
 C(T70AfeMappedRead32(&M,0,NULL)!=0);
 C(T70AfeMappedRead32(&M,0,(uint32_t *)(void *)((char *)&V+1))!=0);
 C(T70AfeMappedRead32(&M,0,(uint32_t *)(UINTPTR_MAX-3))!=0);
 C(T70AfeMappedRead32(&M,0,&Ram[0])!=0);
 C(T70AfeMappedRead32(&M,0,(uint32_t *)(void *)&M)!=0);
 C(R==Reads && W==Writes && V==0x1234);
 /* Actual configure calls linked to the adapter, using mock primitives only. */
 T70_AFE_PREPARED P;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x100000000ULL,512,256,&P));
 T70_AFE_FORMAT_IO Io={&M,T70AfeMappedRead32,T70AfeMappedWrite32};
 memset(Ram,0,sizeof Ram);Reads=Writes=0;
 T70_AFE_CONFIG_RESULT Result=T70AfeDl1Configure(&P,&Io);
 C(Result.Code==T70AfeConfigCallbacksCompleted);C(Reads==4 && Writes==8);
 C(Ram[0x54/4]==0 && Ram[0x50/4]==1 && Ram[0x64/4]==511 && Ram[0x60/4]==1);
 C(Ram[0x4c/4]==(10u<<24));
 M.Length=0;Reads=Writes=0;Result=T70AfeDl1Configure(&P,&Io);
 C(Result.Code==T70AfeConfigAddressFailed && !Result.FormatStarted);C(!Reads && !Writes);
 printf("MappedIoTest: %u checks; host primitive substitutes only\n",Checks);
}
