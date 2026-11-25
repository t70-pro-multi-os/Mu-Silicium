// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Applications/XnuBoot/XnuPlatform.c"
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) { return memmove(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D,UINTN N) { return memset(D,0,N); }
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) { return memcmp(A,B,N); }
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static EFI_MEMORY_REGION_DESCRIPTOR Regions[2];
static unsigned Checks;
static void Reset(void) {
  memset(&Info,0,sizeof(Info));memset(&Mode,0,sizeof(Mode));memset(&Gop,0,sizeof(Gop));
  memset(Regions,0,sizeof(Regions));
  Info.HorizontalResolution=1280;Info.VerticalResolution=800;Info.PixelsPerScanLine=1280;
  Info.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;Mode.Info=&Info;Mode.SizeOfInfo=sizeof(Info);
  Mode.FrameBufferBase=0x80000000;Mode.FrameBufferSize=0x800000;Gop.Mode=&Mode;
  Regions[0].Address=0x50000000;Regions[0].Length=0x20000000;Regions[0].MemoryType=EfiConventionalMemory;
  Regions[1].Address=0x80000000;Regions[1].Length=0x1000000;Regions[1].MemoryType=EfiReservedMemoryType;
  memcpy(Regions[1].Name,"Runtime FrameBuffer",sizeof("Runtime FrameBuffer"));
}
static EFI_STATUS Check(const void *Tree,UINTN Size) {
  return T70XnuValidatePlatform(Tree,Size,Regions,2,&Gop,0,13000000);
}
static void Result(EFI_STATUS Actual,EFI_STATUS Expected) {
  Checks++;if(Actual!=Expected) fprintf(stderr,"check %u actual=%llx expected=%llx\n",Checks,(unsigned long long)Actual,(unsigned long long)Expected);
  assert(Actual==Expected);
}
static void Cell(void *Tree,INT32 Node,const char *Name,UINT32 Value) {
  Value=CpuToFdt32(Value);assert(FdtSetProp(Tree,Node,Name,&Value,4)==0);
}
static void Text(void *Tree,INT32 Node,const char *Name,const char *Value) {
  assert(FdtSetProp(Tree,Node,Name,Value,strlen(Value)+1)==0);
}
int main(int Argc,char **Argv) {
  assert(Argc==2);FILE *File=fopen(Argv[1],"rb");assert(File);assert(fseek(File,0,SEEK_END)==0);
  long Size=ftell(File);assert(Size>40 && Size<=0x200000);rewind(File);
  UINT8 *Original=malloc(Size),*Tree=malloc(Size+4096);assert(Original && Tree);
  assert(fread(Original,1,Size,File)==(size_t)Size);fclose(File);UINTN Capacity=Size+4096;
  Reset();Result(Check(Original,Size),EFI_SUCCESS);
  Result(T70XnuValidatePlatform(Original,Size,Regions,2,&Gop,0x80000000,13000000),EFI_SUCCESS);
  Result(Check(NULL,Size),EFI_COMPROMISED_DATA);Result(Check(Original,39),EFI_COMPROMISED_DATA);
  Result(Check(Original,Size-1),EFI_COMPROMISED_DATA);
  Result(T70XnuValidatePlatform(Original,Size,Regions,2,&Gop,0x100,13000000),EFI_UNSUPPORTED);
  Result(T70XnuValidatePlatform(Original,Size,Regions,2,&Gop,0,24000000),EFI_UNSUPPORTED);
  for (unsigned Fault=0;Fault<15;Fault++) {
    assert(FdtOpenInto(Original,Tree,Capacity)==0);INT32 N;
    if(Fault==0) Text(Tree,0,"compatible","other,board");
    if(Fault==1 || Fault==2 || Fault==3 || Fault==4 || Fault==5) {
      N=FdtPathOffset(Tree,"/cpus/cpu@001");assert(N>=0);
      if(Fault==1) Cell(Tree,N,"reg",1);
      if(Fault==2) Cell(Tree,N,"reg",0);
      if(Fault==3) Text(Tree,N,"status","disabled");
      if(Fault==4) Text(Tree,N,"compatible","arm,cortex-a76");
      if(Fault==5) Text(Tree,N,"enable-method","spin-table");
    }
    if(Fault==6) Text(Tree,FdtPathOffset(Tree,"/psci"),"method","hvc");
    if(Fault>=7 && Fault<=10) {
      N=FdtPathOffset(Tree,"/interrupt-controller");assert(N>=0);
      if(Fault==7) Cell(Tree,N,"reg",0);
      if(Fault==8) Cell(Tree,N,"redistributor-stride",0x20000);
      if(Fault==9) Cell(Tree,N,"#redistributor-regions",2);
      if(Fault==10) Text(Tree,N,"status","disabled");
    }
    if(Fault==11) Cell(Tree,FdtPathOffset(Tree,"/timer"),"interrupts",0);
    if(Fault==12) Cell(Tree,FdtPathOffset(Tree,"/timer"),"clock-frequency",24000000);
    if(Fault==13) Cell(Tree,FdtPathOffset(Tree,"/cpus"),"#address-cells",2);
    if(Fault==14) Cell(Tree,FdtPathOffset(Tree,"/interrupt-controller"),"interrupts",0);
    Result(Check(Tree,Capacity),EFI_UNSUPPORTED);
  }
  Result(T70XnuValidatePlatform(Original,Size,Regions,0,&Gop,0,13000000),EFI_NOT_READY);
  Result(T70XnuValidatePlatform(Original,Size,Regions,MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT,&Gop,0,13000000),EFI_NOT_READY);
  Result(T70XnuValidatePlatform(Original,Size,Regions,2,NULL,0,13000000),EFI_NOT_READY);
  Info.PixelFormat=PixelRedGreenBlueReserved8BitPerColor;Result(Check(Original,Size),EFI_UNSUPPORTED);Reset();
  Mode.FrameBufferSize=4096;Result(Check(Original,Size),EFI_COMPROMISED_DATA);Reset();
  Regions[1].MemoryType=EfiConventionalMemory;Result(Check(Original,Size),EFI_ACCESS_DENIED);Reset();
  Regions[1].Name[0]='x';Result(Check(Original,Size),EFI_ACCESS_DENIED);Reset();
  Regions[1].Length=4096;Result(Check(Original,Size),EFI_ACCESS_DENIED);Reset();
  Regions[1].Address=MAX_UINT64-1;Result(Check(Original,Size),EFI_COMPROMISED_DATA);Reset();
  Mode.SizeOfInfo=1;Result(Check(Original,Size),EFI_COMPROMISED_DATA);Reset();
  Gop.Mode=NULL;Result(Check(Original,Size),EFI_COMPROMISED_DATA);Reset();
  Mode.FrameBufferBase=MAX_UINT64-3;Result(Check(Original,Size),EFI_COMPROMISED_DATA);Reset();
  Info.HorizontalResolution=0;Result(Check(Original,Size),EFI_COMPROMISED_DATA);Reset();
  free(Tree);free(Original);
  printf("PASS: %u production runtime platform checks with stock CPU/GIC/timer input and synthetic GOP reservations\n",Checks);
  return 0;
}
