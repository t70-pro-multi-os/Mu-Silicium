// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Library/FdtLib.h>
#include "../Applications/LinuxBoot/LinuxLid.c"
static void Value(CONST VOID *T,CONST CHAR8 *Path,CONST CHAR8 *Name,UINT32 Expected) {
  INT32 N=FdtPathOffset(T,Path),L;CONST UINT32 *P=FdtGetProp(T,N,Name,&L);
  CHECK(N>=0 && P && L==4 && Fdt32ToCpu(*P)==Expected);
}
static void Preserved(CONST VOID *Before,CONST VOID *After) {
  INT32 Node=-1,Depth=0,Other=-1,OtherDepth=0,Prop,Length,NewLength;CHAR8 Path[1024],OtherPath[1024];
  while ((Node=FdtNextNode(Before,Node,&Depth))>=0 && Depth>=0) {
    CHECK(FdtGetPath(Before,Node,Path,sizeof(Path))==0);
    do {
      Other=FdtNextNode(After,Other,&OtherDepth);CHECK(Other>=0 && OtherDepth>=0);
      CHECK(!FdtGetPath(After,Other,OtherPath,sizeof(OtherPath)));
    } while (!strcmp(OtherPath,"/teclast-cover-lid") || !strncmp(OtherPath,"/teclast-cover-lid/",19));
    CHECK(!strcmp(Path,OtherPath));
    FdtForEachPropertyOffset(Prop,Before,Node) {
      CONST FDT_PROPERTY *P=FdtGetPropertyByOffset(Before,Prop,&Length);CHECK(P!=NULL);
      CONST CHAR8 *Name=FdtGetString(Before,Fdt32ToCpu(P->NameOffset),NULL);CHECK(Name!=NULL);
      if (!strcmp(Path,"/soc/odm/hall_para") && !strcmp(Name,"status")) continue;
      CONST VOID *A=FdtGetProp(Before,Node,Name,&Length),*B=FdtGetProp(After,Other,Name,&NewLength);
      if (!(A && B && Length==NewLength && !memcmp(A,B,Length))) fprintf(stderr,"Changed property: %s %s lengths %d/%d\n",Path,Name,Length,NewLength);
      CHECK(A && B && Length==NewLength && !memcmp(A,B,Length));
    }
  }
  CHECK(FdtGetNumberOfReserveMapEntries(Before)==FdtGetNumberOfReserveMapEntries(After));
  for (INT32 I=0;I<FdtGetNumberOfReserveMapEntries(Before);I++) {
    UINT64 A,S,B,T;CHECK(!FdtGetReserveMapEntry(Before,I,&A,&S) && !FdtGetReserveMapEntry(After,I,&B,&T));
    CHECK(A==B && S==T);
  }
}
int main(int argc,char **argv) {
  CHECK(argc==2);FILE *F=fopen(argv[1],"rb");CHECK(F && !fseek(F,0,SEEK_END));
  long Size=ftell(F);rewind(F);CHECK(Size>(long)sizeof(FDT_HEADER));
  UINTN N=(UINTN)Size,Capacity=N+2*T70_LID_TREE_SLACK;UINT8 *Original=malloc(N),*Input=malloc(Capacity),*Output=malloc(Capacity);
  CHECK(Original && Input && Output && fread(Original,1,N,F)==N);fclose(F);memcpy(Input,Original,N);
  CHECK(T70PrepareLinuxLidTree(Input,N,Output,Capacity)==EFI_SUCCESS);
  CHECK(!memcmp(Input,Original,N) && FdtCheckFull(Output,FdtTotalSize(Output))==0);
  INT32 Node=FdtPathOffset(Output,"/teclast-cover-lid/lid-switch"),L;CONST UINT32 *Pins=FdtGetProp(Output,Node,"gpios",&L);
  CHECK(Node>=0 && Pins && L==12 && Fdt32ToCpu(Pins[0])==0x48 && !Pins[1] && !Pins[2]);
  Value(Output,"/teclast-cover-lid/lid-switch","linux,input-type",5);
  Value(Output,"/teclast-cover-lid/lid-switch","linux,code",0);
  Value(Output,"/teclast-cover-lid/lid-switch","debounce-interval",10);
  Value(Output,"/teclast-cover-lid/lid-switch","wakeup-event-action",2);
  CHECK(Property(Output,FdtPathOffset(Output,"/teclast-cover-lid"),"compatible","gpio-keys",10));
  CHECK(Property(Output,Node,"wakeup-source",NULL,0));
  CHECK(Property(Output,FdtPathOffset(Output,"/soc/odm/hall_para"),"status","disabled",9));
  Preserved(Input,Output);
  CHECK(T70PrepareLinuxLidTree(NULL,N,Output,Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,N,NULL,Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input+1,N,Output,Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,N,Output+1,Capacity-1)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree((VOID *)(MAX_UINTN-7),N,Output,Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,N,(VOID *)(MAX_UINTN-7),Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,N,Input,Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,N,Input+8,Capacity-8)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,N,Output,N+T70_LID_TREE_SLACK-1)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,1,Output,Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Input,T70_LID_TREE_MAX,Output,Capacity)==EFI_INVALID_PARAMETER);
  CHECK(T70PrepareLinuxLidTree(Output,FdtTotalSize(Output),Input,Capacity)==EFI_UNSUPPORTED);
  for (unsigned I=0;I<18;I++) {
    CHECK(!FdtOpenInto(Original,Input,(INT32)Capacity));
    INT32 Hall=FdtPathOffset(Input,"/soc/odm/hall_para"),Gpio=FdtPathOffset(Input,"/soc/pinctrl");
    UINT32 P[3]={Fdt32ToCpu(0x48),0,Fdt32ToCpu(1)};UINT32 Big=Fdt32ToCpu(1);
    switch (I) {
      case 0:CHECK(!FdtSetProp(Input,0,"compatible","other",6));break;
      case 1:CHECK(!FdtSetProp(Input,Hall,"hall_name","MH249",6));break;
      case 2:CHECK(!FdtSetProp(Input,Hall,"hall_far_gpvalue",&Big,4));break;
      case 3:P[1]=Big;CHECK(!FdtSetProp(Input,Hall,"hall_int_port",P,12));break;
      case 4:P[2]=0;CHECK(!FdtSetProp(Input,Hall,"hall_int_port",P,12));break;
      case 5:P[0]=Fdt32ToCpu(0x49);CHECK(!FdtSetProp(Input,Hall,"hall_int_port",P,12));break;
      case 6:CHECK(!FdtSetProp(Input,Hall,"hall_int_port",P,8));break;
      case 7:CHECK(!FdtDelProp(Input,Hall,"wakeup-source"));break;
      case 8:CHECK(!FdtSetProp(Input,Hall,"wakeup-source",&Big,4));break;
      case 9:CHECK(!FdtSetProp(Input,Hall,"status","disabled",9));break;
      case 10:CHECK(!FdtSetProp(Input,Gpio,"status","disabled",9));break;
      case 11:CHECK(!FdtSetProp(Input,Gpio,"#gpio-cells",&Big,4));break;
      case 12:CHECK(!FdtDelProp(Input,Gpio,"gpio-controller"));break;
      case 13:CHECK(!FdtSetProp(Input,Gpio,"compatible","other",6));break;
      case 14:CHECK(FdtAddSubnode(Input,0,"teclast-cover-lid")>=0);break;
      case 15:CHECK(!FdtSetProp(Input,FdtPathOffset(Input,"/soc"),"status","disabled",9));break;
      case 16:CHECK(!FdtDelNode(Input,Hall));break;
      case 17:CHECK(!FdtSetProp(Input,FdtPathOffset(Input,"/soc/odm"),"status","disabled",9));break;
    }
    CHECK(!FdtPack(Input));UINTN Bytes=FdtTotalSize(Input);UINT8 *Snapshot=malloc(Bytes);CHECK(Snapshot!=NULL);memcpy(Snapshot,Input,Bytes);
    memset(Output,0xa5,Capacity);
    CHECK(T70PrepareLinuxLidTree(Input,Bytes,Output,Capacity)==EFI_UNSUPPORTED);
    CHECK(!memcmp(Input,Snapshot,Bytes) && Output[0]==0xa5 && Output[Capacity-1]==0xa5);free(Snapshot);
  }
  memcpy(Input,Original,N);Input[0]=0;CHECK(T70PrepareLinuxLidTree(Input,N,Output,Capacity)==EFI_COMPROMISED_DATA);
  free(Original);free(Input);free(Output);
  printf("Linux lid DT: %u checks passed (native SW_LID, open-edge wake, immutable input and every unrelated property/reservation preserved)\n",Checks);
  return 0;
}
