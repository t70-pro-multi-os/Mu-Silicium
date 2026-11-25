// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CpuPpmCore.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define CHECK(X) do { Checks++;assert(X); } while(0)
int main(void) {
  uint32_t F[T70_CPU_PPM_DOMAINS][T70_CPU_PPM_MAX_OPPS]={{1800000,1200000,600000},{2400000,1600000,800000}};
  uint32_t N[T70_CPU_PPM_DOMAINS]={3,3},C[T70_CPU_PPM_DOMAINS]={383,1024};
  T70_CPU_PPM_DOMAIN D[T70_CPU_PPM_DOMAINS]={{0}},Saved[T70_CPU_PPM_DOMAINS];
  uint32_t Frequency=0xa5a5a5a5,Uid=99;
  CHECK(T70CpuPpmBuild(F,N,C,D));
  CHECK(D[0].Performance[0]==689400&&D[0].Performance[1]==459600&&D[0].Performance[2]==229800);
  CHECK(D[1].Performance[0]==2457600&&D[1].Performance[1]==1638400&&D[1].Performance[2]==819200);
  CHECK(D[0].HighestPerformance==D[0].NominalPerformance);
  CHECK(D[1].LowestPerformance==D[1].LowestNonlinearPerformance);
  CHECK(T70CpuPpmSelect(&D[0],459600,500000,689400,&Frequency)&&Frequency==1200000);
  CHECK(T70CpuPpmSelect(&D[1],819200,1700000,2457600,&Frequency)&&Frequency==1600000);
  Frequency=0xa5a5a5a5;
  CHECK(!T70CpuPpmSelect(&D[0],500000,600000,650000,&Frequency)&&Frequency==0xa5a5a5a5);
  CHECK(!T70CpuPpmSelect(&D[0],800000,850000,900000,&Frequency)&&Frequency==0xa5a5a5a5);
  CHECK(!T70CpuPpmSelect(&D[0],0,100000,200000,&Frequency)&&Frequency==0xa5a5a5a5);
  CHECK(T70CpuPpmSelect(&D[0],0,650000,650000,&Frequency)&&Frequency==1200000);
  CHECK(T70CpuPpmSelect(&D[1],0,1700000,3000000,&Frequency)&&Frequency==1600000);
  CHECK(T70CpuPpmSelect(&D[0],459600,0,689400,&Frequency)&&Frequency==1200000);
  Frequency=0xa5a5a5a5;
  CHECK(!T70CpuPpmSelect(&D[0],800000,700000,600000,&Frequency)&&Frequency==0xa5a5a5a5);
  memcpy(Saved,D,sizeof(D));
  F[0][1]=1800000;
  CHECK(!T70CpuPpmBuild(F,N,C,D)&&memcmp(D,Saved,sizeof(D))==0);
  F[0][1]=1200000;N[1]=33;
  CHECK(!T70CpuPpmBuild(F,N,C,D)&&memcmp(D,Saved,sizeof(D))==0);
  N[1]=3;C[1]=0;
  CHECK(!T70CpuPpmBuild(F,N,C,D)&&memcmp(D,Saved,sizeof(D))==0);
  static const uint16_t Namespace0[]={ '\\','_','S','B','.','C','P','U','0' };
  static const uint16_t Namespace7[]={ '\\','_','S','B','.','C','P','U','7' };
  static const uint16_t Pnp6[]={ 'A','C','P','I','\\','A','C','P','I','0','0','0','7','\\','6' };
  static const uint16_t Bad[]={ 'A','C','P','I','\\','O','T','H','E','R','\\','6' };
  static const uint16_t Pnp8[]={ 'A','C','P','I','\\','A','C','P','I','0','0','0','7','\\','8' };
  CHECK(T70CpuPpmUidFromDeviceId(Namespace0,sizeof(Namespace0)/2,&Uid)&&Uid==0);
  CHECK(T70CpuPpmUidFromDeviceId(Namespace7,sizeof(Namespace7)/2,&Uid)&&Uid==7);
  CHECK(T70CpuPpmUidFromDeviceId(Pnp6,sizeof(Pnp6)/2,&Uid)&&Uid==6);
  Uid=99;CHECK(!T70CpuPpmUidFromDeviceId(Bad,sizeof(Bad)/2,&Uid)&&Uid==99);
  Uid=99;CHECK(!T70CpuPpmUidFromDeviceId(Pnp8,sizeof(Pnp8)/2,&Uid)&&Uid==99);
  printf("CPU PPM scale/domain mapping: %u checks passed\n",Checks);
  return 0;
}
