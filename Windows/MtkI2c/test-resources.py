#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the exact production resource validator with host-only WDF mocks."""
import os
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parent
source=(root/'Driver.c').read_text()
start=source.index('static NTSTATUS ResourcesValid(')
end=source.index('\nstatic void ReleaseResources(',start)
function=source[start:end]
harness=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t ULONG; typedef uint64_t ULONGLONG; typedef int32_t NTSTATUS;
typedef void *WDFCMRESLIST;
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_CONFIGURATION_ERROR (-1)
#define CmResourceTypeMemory 1
#define CmResourceTypeInterrupt 2
#define CmResourceTypeConnection 3
#define CmResourceShareDeviceExclusive 1
#define CmResourceShareDriverExclusive 2
#define CM_RESOURCE_MEMORY_READ_WRITE 0
#define CM_RESOURCE_MEMORY_READ_ONLY 1
#define CM_RESOURCE_MEMORY_WRITE_ONLY 2
#define CM_RESOURCE_MEMORY_WRITEABILITY_MASK 3
#define CM_RESOURCE_INTERRUPT_LATCHED 1
#define CM_RESOURCE_INTERRUPT_MESSAGE 2
#define CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT 4
struct Resource { unsigned Type,Flags,ShareDisposition; union {
 struct { struct { ULONGLONG QuadPart; } Start; ULONG Length; } Memory;
 struct { ULONG Vector,Level; ULONGLONG Affinity; } Interrupt;
} u; };
typedef struct Resource *PCM_PARTIAL_RESOURCE_DESCRIPTOR;
#define I2C6_BASE 0x1101a000ULL
static struct Resource R[2][3]; static unsigned Count[2],Missing,Checks;
static ULONG WdfCmResourceListGetCount(WDFCMRESLIST L) {return Count[(uintptr_t)L-1];}
static PCM_PARTIAL_RESOURCE_DESCRIPTOR WdfCmResourceListGetDescriptor(WDFCMRESLIST L,ULONG I) {
 return Missing==((uintptr_t)L-1)*3+I+1?NULL:&R[(uintptr_t)L-1][I];
}
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"line %d: %s\n",__LINE__,#X);abort();}}while(0)
'''
tests=r'''
static void Setup(unsigned Instance) {
 const ULONGLONG Bases[]={0x11e00000,0x11e01000,0x1101a000};
 const ULONGLONG Dmas[]={0x10217080,0x10217100,0x10217480};
 const ULONG Gsis[]={176,177,182};
 memset(R,0,sizeof(R)); Count[0]=Count[1]=3; Missing=0;
 for(unsigned P=0;P<2;P++) {
  R[P][0].Type=R[P][1].Type=CmResourceTypeMemory;
  R[P][0].u.Memory.Start.QuadPart=Bases[Instance]; R[P][0].u.Memory.Length=0x1000;
  R[P][1].u.Memory.Start.QuadPart=Dmas[Instance]; R[P][1].u.Memory.Length=0x80;
  R[P][2].Type=CmResourceTypeInterrupt;R[P][2].ShareDisposition=CmResourceShareDeviceExclusive;
  R[P][2].u.Interrupt.Vector=P?0x1234:Gsis[Instance];
 }
}
int main(void) {
 for(unsigned I=0;I<3;I++) {
  Setup(I); CHECK(ResourcesValid((void*)1,(void*)2)==0);
  CHECK(ResourcesValid(NULL,(void*)2)!=0);CHECK(ResourcesValid((void*)1,NULL)!=0);
  for(unsigned M=1;M<=6;M++){Missing=M;CHECK(ResourcesValid((void*)1,(void*)2)!=0);}Missing=0;
  for(unsigned P=0;P<2;P++) {
   for(unsigned C=0;C<6;C++)if(C!=3){Setup(I);Count[P]=C;CHECK(ResourcesValid((void*)1,(void*)2)!=0);}
   for(unsigned N=0;N<2;N++) for(unsigned F=0;F<5;F++) {
    Setup(I);
    if(F==0)R[P][N].u.Memory.Start.QuadPart++;
    if(F==1)R[P][N].u.Memory.Length--;
    if(F==2)R[P][N].Flags=1;
    if(F==3)R[P][N].Type=CmResourceTypeConnection;
    if(F==4)R[P][N].Flags=0x10; /* non-writeability attributes are allowed */
    CHECK((F==4)==(ResourcesValid((void*)1,(void*)2)==0));
   }
   for(unsigned Flag=1;Flag<=4;Flag<<=1){Setup(I);R[P][2].Flags=Flag;CHECK(ResourcesValid((void*)1,(void*)2)!=0);}
   Setup(I);R[P][2].ShareDisposition=3;CHECK(ResourcesValid((void*)1,(void*)2)!=0);
  }
  Setup(I);R[0][2].u.Interrupt.Vector++;CHECK(ResourcesValid((void*)1,(void*)2)!=0);
  Setup(I);R[1][2].u.Interrupt.Vector=0xabcd;CHECK(ResourcesValid((void*)1,(void*)2)==0);
 }
 printf("I2C resource validator: %u checks passed\n",Checks);
}
'''
with tempfile.TemporaryDirectory(prefix='t70-i2c-resources-') as d:
 c=Path(d)/'test.c';exe=Path(d)/'test';c.write_text(harness+function+tests)
 subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
