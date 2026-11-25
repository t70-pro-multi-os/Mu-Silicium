// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioDomainOwner.h"
#include <cassert>
#include <cstdio>
#include <thread>
static PDEVICE_OBJECT Device=(PDEVICE_OBJECT)1,Pdo=(PDEVICE_OBJECT)2;
static NTSTATUS Status;static unsigned char Irql;static volatile LONG Refs;
unsigned char KeGetCurrentIrql(){return Irql;}
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG C){return __sync_val_compare_and_swap(P,C,V);}
LONG InterlockedExchange(volatile LONG *P,LONG V){return __sync_lock_test_and_set(P,V);}
void ObReferenceObject(void *P){assert(P==Device||P==Pdo);__sync_add_and_fetch(&Refs,1);}
void ObDereferenceObject(void *P){assert(P==Device||P==Pdo);assert(__sync_sub_and_fetch(&Refs,1)>=0);}
NTSTATUS PcGetPhysicalDeviceObject(PDEVICE_OBJECT D,PDEVICE_OBJECT *P){assert(D==Device);*P=Pdo;return Status;}
int main(){
 T70_AUDIO_DOMAIN_OWNER A,B;
 assert(T70AudioDomainAcquire(&A,Device,(PDEVICE_OBJECT)3)==STATUS_NO_SUCH_DEVICE&&!Refs);
 Status=0x103;assert(T70AudioDomainAcquire(&A,Device,Pdo)==STATUS_DEVICE_NOT_READY&&!Refs);Status=0;
 assert(T70AudioDomainAcquire(&A,Device,Pdo)==0&&Refs==2);
 assert(T70AudioDomainAcquire(&B,Device,Pdo)==STATUS_DEVICE_NOT_READY&&!B.Held&&Refs==2);
 assert(T70AudioDomainRelease(&B)==0&&Refs==2);
 assert(T70AudioDomainAcquire(&A,Device,Pdo)==STATUS_INVALID_DEVICE_STATE);
 assert(T70AudioDomainRelease(&A)==0&&!Refs&&T70AudioDomainRelease(&A)==0);
 for(unsigned I=0;I<1000;I++){
  NTSTATUS S1=0,S2=0;std::thread T1([&]{S1=T70AudioDomainAcquire(&A,Device,Pdo);});
  std::thread T2([&]{S2=T70AudioDomainAcquire(&B,Device,Pdo);});T1.join();T2.join();
  assert((S1==0)!=(S2==0));assert(Refs==2);
  assert(T70AudioDomainRelease(&A)==0&&T70AudioDomainRelease(&B)==0&&!Refs);
 }
 Irql=2;assert(T70AudioDomainAcquire(&A,Device,Pdo)==STATUS_INVALID_PARAMETER);assert(T70AudioDomainRelease(&A)==STATUS_INVALID_PARAMETER);
 puts("Audio domain: PDO identity, retained OS references, 1000 competing claims passed; no hardware ownership claim");
}
