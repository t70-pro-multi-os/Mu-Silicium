// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "LinuxLid.h"
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>

STATIC BOOLEAN Property(CONST VOID *Tree,INT32 Node,CONST CHAR8 *Name,CONST VOID *Value,UINTN Bytes) {
  INT32 Size;CONST VOID *Data=FdtGetProp(Tree,Node,Name,&Size);
  return Size==(INT32)Bytes && (Bytes==0 || (Data && CompareMem(Data,Value,Bytes)==0));
}
STATIC BOOLEAN Enabled(CONST VOID *Tree,INT32 Node) {
  INT32 Size;CONST VOID *Data=FdtGetProp(Tree,Node,"status",&Size);
  return Node>=0 && ((!Data && Size==-FDT_ERR_NOTFOUND) ||
    Property(Tree,Node,"status","okay",5) || Property(Tree,Node,"status","ok",3));
}
STATIC INT32 Cell(VOID *Tree,INT32 Node,CONST CHAR8 *Name,UINT32 Value) {
  UINT32 Big=Fdt32ToCpu(Value);return FdtSetProp(Tree,Node,Name,&Big,sizeof(Big));
}
EFI_STATUS T70PrepareLinuxLidTree(CONST VOID *Original,UINTN Bytes,VOID *Output,UINTN Capacity) {
  INT32 Hall,Gpio,Lid,Switch,Size;CONST UINT32 *Pins;UINT32 Handle,NewPins[3],Zero=0,Two;
  UINTN Source=(UINTN)Original,Destination=(UINTN)Output;
  if (!Original || !Output || (Source&7) || (Destination&7) || Bytes<sizeof(FDT_HEADER) || Bytes>T70_LID_TREE_MAX-T70_LID_TREE_SLACK ||
      Capacity<Bytes+T70_LID_TREE_SLACK || Capacity>T70_LID_TREE_MAX ||
      Source>MAX_UINTN-Bytes || Destination>MAX_UINTN-Capacity ||
      (Source<Destination+Capacity && Destination<Source+Bytes)) return EFI_INVALID_PARAMETER;
  if (FdtTotalSize(Original)!=Bytes || FdtCheckFull(Original,Bytes)!=0) return EFI_COMPROMISED_DATA;
  Hall=FdtPathOffset(Original,"/soc/odm/hall_para");Gpio=FdtPathOffset(Original,"/soc/pinctrl");
  Two=Fdt32ToCpu(2);
  if (!Property(Original,0,"compatible","mediatek,MT6789",16) ||
      !Enabled(Original,FdtPathOffset(Original,"/soc")) ||
      !Enabled(Original,FdtPathOffset(Original,"/soc/odm")) ||
      !Enabled(Original,Hall) || !Enabled(Original,Gpio) ||
      !Property(Original,Gpio,"compatible","mediatek,mt6789-pinctrl",24) ||
      !Property(Original,Gpio,"gpio-controller",NULL,0) ||
      !Property(Original,Gpio,"#gpio-cells",&Two,sizeof(Two)) ||
      !Property(Original,Hall,"hall_name","MH248",6) ||
      !Property(Original,Hall,"hall_far_gpvalue",&Zero,sizeof(Zero)) ||
      !Property(Original,Hall,"wakeup-source",NULL,0) ||
      FdtPathOffset(Original,"/teclast-cover-lid")!=-FDT_ERR_NOTFOUND) return EFI_UNSUPPORTED;
  Pins=FdtGetProp(Original,Hall,"hall_int_port",&Size);
  Handle=FdtGetPhandle(Original,Gpio);
  if (!Pins || Size!=12 || !Handle || Handle==MAX_UINT32 ||
      FdtNodeOffsetByPhandle(Original,Handle)!=Gpio || Fdt32ToCpu(Pins[0])!=Handle ||
      Fdt32ToCpu(Pins[1])!=0 || Fdt32ToCpu(Pins[2])!=1) return EFI_UNSUPPORTED;
  /* MH248's shipped code reads raw level: 0=open, 1=closed. Its DT active-low
   * flag describes the old key interface, not the SW_LID state polarity. */
  NewPins[0]=Pins[0];NewPins[1]=0;NewPins[2]=0;
  if (FdtOpenInto(Original,Output,(INT32)Capacity)!=0) return EFI_OUT_OF_RESOURCES;
  Hall=FdtPathOffset(Output,"/soc/odm/hall_para");
  /* Disable the old direct-init module's parameter node before adding the
   * native switch. The original runtime tree is never modified. */
  if (FdtSetProp(Output,Hall,"status","disabled",9)!=0) return EFI_OUT_OF_RESOURCES;
  Lid=FdtAddSubnode(Output,0,"teclast-cover-lid");
  if (Lid<0 || FdtSetProp(Output,Lid,"compatible","gpio-keys",10)!=0 ||
      FdtSetProp(Output,Lid,"label","T70Pro magnetic cover",22)!=0) return EFI_OUT_OF_RESOURCES;
  Switch=FdtAddSubnode(Output,Lid,"lid-switch");
  if (Switch<0 || FdtSetProp(Output,Switch,"gpios",NewPins,sizeof(NewPins))!=0 ||
      Cell(Output,Switch,"linux,input-type",5)!=0 || Cell(Output,Switch,"linux,code",0)!=0 ||
      Cell(Output,Switch,"debounce-interval",10)!=0 ||
      FdtSetProp(Output,Switch,"wakeup-source",NULL,0)!=0 ||
      Cell(Output,Switch,"wakeup-event-action",2)!=0 || FdtPack(Output)!=0 ||
      FdtCheckFull(Output,FdtTotalSize(Output))!=0) return EFI_OUT_OF_RESOURCES;
  return EFI_SUCCESS;
}
