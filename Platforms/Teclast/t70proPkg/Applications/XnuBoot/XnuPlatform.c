// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "XnuPlatform.h"
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <DarwinBootPkg/Driver/BootVideo.h>

STATIC BOOLEAN StringIs(CONST VOID *Tree,INT32 Node,CONST CHAR8 *Key,CONST CHAR8 *Value,UINTN Bytes) {
  INT32 Length;CONST VOID *P=FdtGetProp(Tree,Node,Key,&Length);
  return P && Length==(INT32)Bytes && CompareMem(P,Value,Bytes)==0;
}
STATIC BOOLEAN Enabled(CONST VOID *Tree,INT32 Node) {
  INT32 Length;CONST VOID *P=FdtGetProp(Tree,Node,"status",&Length);
  return !P || StringIs(Tree,Node,"status","okay",sizeof("okay")) || StringIs(Tree,Node,"status","ok",sizeof("ok"));
}
STATIC BOOLEAN CellsAre(CONST VOID *Tree,INT32 Node,CONST CHAR8 *Key,CONST UINT32 *Values,UINTN Count) {
  INT32 Length;CONST UINT8 *P=FdtGetProp(Tree,Node,Key,&Length);UINT32 Value;
  if (!P || Length!=(INT32)(Count*4)) return FALSE;
  for (UINTN I=0;I<Count;I++) {
    CopyMem(&Value,P+I*4,4);
    if (Fdt32ToCpu(Value)!=Values[I]) return FALSE;
  }
  return TRUE;
}
EFI_STATUS T70XnuValidatePlatform(CONST VOID *Tree,UINTN Bytes,
  CONST EFI_MEMORY_REGION_DESCRIPTOR *Regions,UINT8 Count,
  CONST EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,UINT64 Mpidr,UINT32 TimerFrequency) {
  INT32 Cpus,Node,Gic,Psci,Timer,Length;UINT32 Seen=0,Reg;
  CONST VOID *Property;UINT64 Video[6],End;EFI_STATUS Status;BOOLEAN Protected=FALSE;
  STATIC CONST UINT32 One[]={1},Zero[]={0},Four[]={4};
  STATIC CONST UINT32 GicReg[]={0,0x0c000000,0,0x40000,0,0x0c040000,0,0x200000};
  STATIC CONST UINT32 GicIrq[]={1,9,4,0};
  STATIC CONST UINT32 TimerIrq[]={1,13,4,0,1,14,4,0,1,11,4,0,1,10,4,0};
  STATIC CONST UINT32 Frequency[]={13000000};
  if (!Tree || Bytes<sizeof(FDT_HEADER) || Bytes>0x200000 ||
      FdtCheckFull(Tree,Bytes) || FdtTotalSize(Tree)!=Bytes) return EFI_COMPROMISED_DATA;
  if (!StringIs(Tree,0,"compatible","mediatek,MT6789",sizeof("mediatek,MT6789")) ||
      (Mpidr & 0xff00ffffffULL)!=0 || TimerFrequency!=13000000) return EFI_UNSUPPORTED;
  Cpus=FdtPathOffset(Tree,"/cpus");Gic=FdtPathOffset(Tree,"/interrupt-controller");
  Psci=FdtPathOffset(Tree,"/psci");Timer=FdtPathOffset(Tree,"/timer");
  if (Cpus<0 || Gic<0 || Psci<0 || Timer<0 || !Enabled(Tree,Cpus) ||
      !Enabled(Tree,Gic) || !Enabled(Tree,Psci) || !Enabled(Tree,Timer)) return EFI_UNSUPPORTED;
  if (!CellsAre(Tree,Cpus,"#address-cells",One,1) || !CellsAre(Tree,Cpus,"#size-cells",Zero,1))
    return EFI_UNSUPPORTED;
  FdtForEachSubnode(Node,Tree,Cpus) {
    if (!StringIs(Tree,Node,"device_type","cpu",sizeof("cpu"))) continue;
    Property=FdtGetProp(Tree,Node,"reg",&Length);
    if (!Property || Length!=4 || !Enabled(Tree,Node)) return EFI_UNSUPPORTED;
    CopyMem(&Reg,Property,4);Reg=Fdt32ToCpu(Reg);
    if (Reg>0x700 || (Reg & 0xff)!=0 || (Seen & (1U<<(Reg>>8)))) return EFI_UNSUPPORTED;
    if (!StringIs(Tree,Node,"compatible",Reg<0x600?"arm,cortex-a55":"arm,cortex-a76",sizeof("arm,cortex-a55")) ||
        !StringIs(Tree,Node,"enable-method","psci",sizeof("psci"))) return EFI_UNSUPPORTED;
    Seen|=1U<<(Reg>>8);
  }
  if (Seen!=0xff || !StringIs(Tree,Psci,"compatible","arm,psci-1.0",sizeof("arm,psci-1.0")) ||
      !StringIs(Tree,Psci,"method","smc",sizeof("smc")) || !StringIs(Tree,Gic,"compatible","arm,gic-v3",sizeof("arm,gic-v3")) ||
      !CellsAre(Tree,Gic,"reg",GicReg,8) || !CellsAre(Tree,Gic,"#interrupt-cells",Four,1) ||
      !CellsAre(Tree,Gic,"#redistributor-regions",One,1) || !CellsAre(Tree,Gic,"interrupts",GicIrq,4) ||
      FdtGetProp(Tree,Gic,"redistributor-stride",&Length) ||
      !StringIs(Tree,Timer,"compatible","arm,armv8-timer",sizeof("arm,armv8-timer")) ||
      !CellsAre(Tree,Timer,"clock-frequency",Frequency,1) || !CellsAre(Tree,Timer,"interrupts",TimerIrq,16))
    return EFI_UNSUPPORTED;
  if (!Gop) return EFI_NOT_READY;
  Status=DarwinBootVideoFromGop(Gop,Video);if (EFI_ERROR(Status)) return Status;
  if (!Regions || !Count || Count>=MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT) return EFI_NOT_READY;
  End=Gop->Mode->FrameBufferBase+Gop->Mode->FrameBufferSize;
  for (UINTN I=0;I<Count;I++) {
    CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&Regions[I];
    if (!R->Length || R->Address>MAX_UINT64-R->Length) return EFI_COMPROMISED_DATA;
    if (R->Address<End && Video[0]<R->Address+R->Length) {
      // Only SEC's validated display reservation can authorize this scanout.
      if (R->MemoryType!=EfiReservedMemoryType ||
          CompareMem(R->Name,"Runtime FrameBuffer",sizeof("Runtime FrameBuffer"))!=0)
        return EFI_ACCESS_DENIED;
      if (R->Address<=Video[0] && R->Length>=End-R->Address) Protected=TRUE;
    }
  }
  return Protected?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
