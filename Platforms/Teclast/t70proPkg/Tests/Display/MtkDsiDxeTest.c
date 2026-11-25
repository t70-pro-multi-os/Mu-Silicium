/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <PiDxe.h>
#include "../MediaTekTest.h"
#include "../../Drivers/DsiPlatformDxe/DsiPlatformDxe.c"
#include "MtkDsiFixture.h"
EFI_GUID gMtkDsiHostProtocolGuid,gFdtTableGuid,gEfiEventExitBootServicesGuid;
STATIC EFI_DXE_SERVICES Dxe;
EFI_DXE_SERVICES *gDS = &Dxe;
STATIC VOID *Tree;
STATIC UINTN TreeBytes;
STATIC BOOLEAN BadMarker,FailCreate,FailPublish;
STATIC UINT64 BadPage,MapAttributes;
STATIC UINT32 MapType;
STATIC EFI_TPL CurrentTpl;
STATIC unsigned Reads,Writes,Maps,GuardReads,PhyWrites,Events,Closes,IdleReads;
STATIC unsigned ReenterWrite,ExitWrite;
STATIC EFI_STATUS ReenterStatus;
STATIC BOOLEAN BusyForever;
STATIC UINT32 Spm[1024],Infra[1024],Disp[0x5000],Phy[1024];
STATIC EFI_EVENT_NOTIFY ExitNotify;

UINT32 EFIAPI InterlockedCompareExchange32 (volatile UINT32 *Value,UINT32 Compare,UINT32 Exchange) {
  return __sync_val_compare_and_swap(Value,Compare,Exchange);
}
VOID EFIAPI MemoryFence (VOID) { __sync_synchronize(); }
STATIC UINT32 *Register (UINTN A) {
  CHECK(!(A&3U));
  if(A>=0x10006000 && A<0x10007000)return &Spm[(A-0x10006000)/4];
  if(A>=0x10001000 && A<0x10002000)return &Infra[(A-0x10001000)/4];
  if(A>=0x14000000 && A<0x14014000)return &Disp[(A-0x14000000)/4];
  if(A>=0x11F60000 && A<0x11F61000)return &Phy[(A-0x11F60000)/4];
  CHECK(0);return NULL;
}
UINT64 EFIAPI MmioRead64 (UINTN A) {
  if(A==0x43F7F000)return BadMarker?0:0x0000000148303754ULL;
  if(A==0x43F7F048)return (UINT64)(UINTN)Tree;
  if(A==0x43F7F050)return TreeBytes;
  if(A==0x43F7F058)return EFI_SUCCESS;
  CHECK(0);return 0;
}
UINT32 EFIAPI MmioRead32 (UINTN A) {
  Reads++;if(A<0x14000000 && !(A>=0x11F60000 && A<0x11F61000))GuardReads++;
  if(A==0x1401300C) {IdleReads++;if(BusyForever)return *Register(A)|0x80000000U;}
  return *Register(A);
}
UINT32 EFIAPI MmioWrite32 (UINTN A,UINT32 Value) {
  Writes++;if(A>=0x11F60000 && A<0x11F61000)PhyWrites++;
  *Register(A)=Value;
  if(Writes==ReenterWrite)ReenterStatus=mHost.StartVideo(&mHost,mToken);
  if(Writes==ExitWrite)ExitNotify(NULL,NULL);
  return Value;
}
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable (EFI_GUID *G,VOID **Value) {
  CHECK(G==&gFdtTableGuid);*Value=Tree;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Descriptor (EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  Maps++;ZeroMem(D,sizeof(*D));D->BaseAddress=A;D->Length=0x1000;
  D->GcdMemoryType=(A==BadPage)?EfiGcdMemoryTypeSystemMemory:(EFI_GCD_MEMORY_TYPE)MapType;
  D->Attributes=MapAttributes;return EFI_SUCCESS;
}
STATIC EFI_TPL EFIAPI Raise (EFI_TPL New) {EFI_TPL Old=CurrentTpl;CurrentTpl=New;return Old;}
STATIC VOID EFIAPI Restore (EFI_TPL Old) {CurrentTpl=Old;}
STATIC EFI_STATUS EFIAPI Create (UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Fn,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event) {
  CHECK(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && Context==NULL);
  CHECK(Group==&gEfiEventExitBootServicesGuid);Events++;
  if(FailCreate)return EFI_OUT_OF_RESOURCES;
  ExitNotify=Fn;*Event=(VOID *)0x123;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Publish (EFI_HANDLE *Handle,...) {
  Installs++;if(FailPublish)return EFI_DEVICE_ERROR;
  *Handle=(VOID *)0x456;return EFI_SUCCESS;
}
STATIC void Reset (void) {
  ZeroMem(&mCore,sizeof(mCore));mHandle=mOwner=NULL;mToken=mNextToken=0;
  mBusy=0;mExiting=mRegistered=FALSE;mExitEvent=NULL;
  ZeroMem(Spm,sizeof(Spm));ZeroMem(Infra,sizeof(Infra));ZeroMem(Disp,sizeof(Disp));ZeroMem(Phy,sizeof(Phy));
  Spm[0x16C/4]=Spm[0x170/4]=0x200000;Spm[0x354/4]=0xD;
  BadMarker=FailCreate=FailPublish=BusyForever=FALSE;BadPage=0;
  MapType=EfiGcdMemoryTypeMemoryMappedIo;MapAttributes=EFI_MEMORY_UC;
  Reads=Writes=Maps=GuardReads=PhyWrites=Events=Closes=IdleReads=Installs=0;
  ReenterWrite=ExitWrite=0;CurrentTpl=TPL_APPLICATION;TimeNs=0;
  Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;Services.CreateEventEx=Create;
  Services.InstallMultipleProtocolInterfaces=Publish;Dxe.GetMemorySpaceDescriptor=Descriptor;
}
STATIC UINT64 Own (void) {
  UINT64 Token=0;
  CHECK(mHost.Acquire(&mHost,(VOID *)0x999,&Token)==EFI_SUCCESS && Token);
  return Token;
}
STATIC void ConfigureReady (UINT64 Token) {
  MTK_DSI_OP7_PROFILE P=SupportedProfile();
  CHECK(mHost.Configure(&mHost,Token,&P)==EFI_SUCCESS);
}
int main (int argc,char **argv) {
  CHECK(argc==2);FILE *F=fopen(argv[1],"rb");CHECK(F);
  CHECK(!fseek(F,0,SEEK_END));long Bytes=ftell(F);CHECK(Bytes>0);rewind(F);
  Tree=malloc((size_t)Bytes);CHECK(Tree);TreeBytes=(UINTN)Bytes;
  CHECK(fread(Tree,1,(size_t)Bytes,F)==(size_t)Bytes);fclose(F);
  CHECK(ValidateTree(Tree,TreeBytes)==EFI_SUCCESS);
  CHECK(ValidateTree(Tree,16)==EFI_COMPROMISED_DATA);
  Reset();CHECK(T70DsiPlatformEntry(NULL,NULL)==EFI_SUCCESS);
  CHECK(Events==1 && Installs==1 && Reads==0 && Writes==0 && Maps==0);
  CHECK(T70DsiPlatformEntry(NULL,NULL)==EFI_ALREADY_STARTED);
  Reset();FailCreate=TRUE;CHECK(T70DsiPlatformEntry(NULL,NULL)==EFI_OUT_OF_RESOURCES && Installs==0);
  Reset();FailPublish=TRUE;CHECK(T70DsiPlatformEntry(NULL,NULL)==EFI_SUCCESS && mRegistered);
  ExitNotify(NULL,NULL);CHECK(mExiting);
  Reset();CHECK(T70DsiPlatformEntry(NULL,NULL)==EFI_SUCCESS);
  UINT64 Token=Own(),Other=0xABCD;
  CHECK(mHost.Acquire(&mHost,(VOID *)0x888,&Other)==EFI_ACCESS_DENIED && Other==0xABCD);
  MTK_DSI_OP7_PROFILE P=SupportedProfile();
  CHECK(mHost.Configure(&mHost,Token+1,&P)==EFI_ACCESS_DENIED);
  CHECK(mHost.Release(&mHost,Token)==EFI_SUCCESS);
  CHECK(mHost.Configure(&mHost,Token,&P)==EFI_ACCESS_DENIED);
  Other=Own();CHECK(Other!=Token);Token=Other;
  CHECK(mHost.StartVideo(&mHost,Token)==EFI_NOT_READY);
  P.Mode=1;unsigned Before=Writes;
  CHECK(mHost.Configure(&mHost,Token,&P)==EFI_UNSUPPORTED && Writes==Before);
  P=SupportedProfile();*Register(0x14013000)=1;
  CHECK(mHost.Configure(&mHost,Token,&P)==EFI_ACCESS_DENIED && Writes==Before);
  *Register(0x14013000)=0;
  ConfigureReady(Token);CHECK(mCore.State==MtkDsiStateOp7Complete);
  for(unsigned Cycle=0;Cycle<3;Cycle++) {
    if(Cycle)CHECK(mHost.Resume(&mHost,Token)==EFI_SUCCESS);
    CHECK(mHost.Release(&mHost,Token)==EFI_NOT_READY);
    CHECK(mHost.StartVideo(&mHost,Token)==EFI_SUCCESS);
    CHECK(mHost.Suspend(&mHost,Token)==EFI_SUCCESS);
    CHECK(mCore.State==MtkDsiStateSuspended);
  }
  CHECK(mHost.Release(&mHost,Token)==EFI_SUCCESS);
  Token=Own();CHECK(mHost.Resume(&mHost,Token)==EFI_SUCCESS);
  CHECK(mHost.StartVideo(&mHost,Token)==EFI_SUCCESS);
  CHECK(mHost.Suspend(&mHost,Token)==EFI_SUCCESS);
  CHECK(mHost.Release(&mHost,Token)==EFI_SUCCESS);

  /* Every mapping/attribute refusal occurs before peripheral access. */
  STATIC CONST UINT64 Pages[]={0x10006000,0x10001000,0x14000000,0x14013000,0x11F60000};
  for(UINTN I=0;I<ARRAY_SIZE(Pages);I++) {
    Reset();BadPage=Pages[I];Other=123;
    CHECK(EFI_ERROR(mHost.Acquire(&mHost,(VOID *)1,&Other)) && Other==123);
    CHECK(Reads==0 && Writes==0 && mOwner==NULL);
  }
  STATIC CONST UINT64 BadAttrs[]={0,EFI_MEMORY_WB,EFI_MEMORY_UC|EFI_MEMORY_RO,EFI_MEMORY_UC|EFI_MEMORY_RP,EFI_MEMORY_UC|EFI_MEMORY_WC};
  for(UINTN I=0;I<ARRAY_SIZE(BadAttrs);I++) {
    Reset();MapAttributes=BadAttrs[I];
    CHECK(EFI_ERROR(mHost.Acquire(&mHost,(VOID *)1,&Other)) && Reads==0 && Writes==0);
  }
  STATIC CONST CHAR8 *Names[]={"mediatek,mt6789-scpsys","mediatek,mt6789-infracfg_ao",
    "mediatek,mt6789-mmsys","mediatek,dsi0","mediatek,mipi_tx_config0"};
  for(UINTN I=0;I<ARRAY_SIZE(Names);I++) {
    INT32 Node=FdtNodeOffsetByCompatible(Tree,-1,Names[I]),Length;
    UINT8 *Reg=(UINT8 *)FdtGetProp(Tree,Node,"reg",&Length);
    CHECK(Node>=0 && Reg && Length==16);
    UINT8 Original=Reg[7];Reg[7]^=4;
    Reset();CHECK(EFI_ERROR(mHost.Acquire(&mHost,(VOID *)1,&Other)));
    CHECK(Reads==0 && Writes==0 && Maps==0 && mOwner==NULL);
    Reg[7]=Original;
  }
  INT32 Length;
  UINT8 *Compatible=(UINT8 *)FdtGetProp(Tree,0,"compatible",&Length);
  CHECK(Compatible && Length>0);UINT8 First=Compatible[0];Compatible[0]='x';
  Reset();CHECK(EFI_ERROR(mHost.Acquire(&mHost,(VOID *)1,&Other)) && Reads==0 && Writes==0);
  Compatible[0]=First;
  Reset();BadMarker=TRUE;CHECK(EFI_ERROR(mHost.Acquire(&mHost,(VOID *)1,&Other)) && Reads==0 && Maps==0);
  Reset();Spm[0x16C/4]=0;
  CHECK(mHost.Acquire(&mHost,(VOID *)1,&Other)==EFI_NOT_READY && Reads==1 && Writes==0);
  Reset();Spm[0x170/4]=0;
  CHECK(mHost.Acquire(&mHost,(VOID *)1,&Other)==EFI_NOT_READY && Reads==2 && Writes==0);
  Reset();Token=Own();ConfigureReady(Token);*Register(0x14000100)=1U<<19;
  Before=Writes;CHECK(mHost.StartVideo(&mHost,Token)==EFI_NOT_READY && Writes==Before);
  *Register(0x14000100)=0;CHECK(mHost.StartVideo(&mHost,Token)==EFI_SUCCESS);
  BusyForever=TRUE;Before=PhyWrites;
  CHECK(mHost.Suspend(&mHost,Token)==EFI_TIMEOUT && PhyWrites==Before);
  CHECK(mCore.State==MtkDsiStateQuarantined);
  Before=Reads+Writes+Maps;
  CHECK(mHost.Release(&mHost,Token)==EFI_NOT_READY);
  CHECK(mHost.Resume(&mHost,Token)==EFI_DEVICE_ERROR && Before==Reads+Writes+Maps);
  Reset();Token=Own();ConfigureReady(Token);ReenterWrite=Writes+1;
  CHECK(mHost.StartVideo(&mHost,Token)==EFI_SUCCESS && ReenterStatus==EFI_NOT_READY);
  Reset();CHECK(T70DsiPlatformEntry(NULL,NULL)==EFI_SUCCESS);Token=Own();ConfigureReady(Token);
  ExitWrite=Writes+1;CHECK(mHost.StartVideo(&mHost,Token)==EFI_ABORTED);
  CHECK(mCore.State==MtkDsiStateQuarantined);Before=Reads+Writes+Maps;
  CHECK(mHost.Resume(&mHost,Token)==EFI_ABORTED && Before==Reads+Writes+Maps);
  Reset();CurrentTpl=TPL_NOTIFY;CHECK(mHost.Acquire(&mHost,(VOID *)1,&Other)==EFI_NOT_READY && Reads==0);
  Reset();mNextToken=MAX_UINT64;
  CHECK(mHost.Acquire(&mHost,(VOID *)1,&Other)==EFI_OUT_OF_RESOURCES && Reads==0);
  free(Tree);printf("MtkDsiDxeTest: %u checks\n",Checks);return 0;
}
