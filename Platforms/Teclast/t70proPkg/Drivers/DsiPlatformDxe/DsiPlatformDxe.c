/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>
#include <Protocol/MtkDsiHost.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC MTK_DSI_CORE mCore;
STATIC MTK_DSI_HOST_PROTOCOL mHost;
STATIC EFI_HANDLE mHandle, mOwner;
STATIC EFI_EVENT mExitEvent;
STATIC UINT64 mToken, mNextToken;
STATIC volatile UINT32 mBusy;
STATIC BOOLEAN mExiting, mRegistered;

STATIC UINT64
Cells (CONST VOID *Value, UINTN Bytes)
{
  CONST UINT8 *P = Value;
  UINT64 V = 0;
  for (UINTN I = 0; I < Bytes; ++I) V = (V << 8) | P[I];
  return V;
}

STATIC BOOLEAN
Enabled (CONST VOID *Tree, INT32 Node)
{
  INT32 Length;
  CONST CHAR8 *P;
  for (; Node >= 0; Node = FdtParentOffset (Tree, Node)) {
    P = FdtGetProp (Tree, Node, "status", &Length);
    if (P != NULL && !((Length == 5 && CompareMem (P,"okay",5) == 0) ||
                       (Length == 3 && CompareMem (P,"ok",3) == 0))) return FALSE;
  }
  return TRUE;
}

STATIC EFI_STATUS
ValidateTree (CONST VOID *Tree, UINTN Bytes)
{
  STATIC CONST struct { CONST CHAR8 *Name; UINT32 Base; } Nodes[] = {
    {"mediatek,mt6789-scpsys",0x10006000},
    {"mediatek,mt6789-infracfg_ao",0x10001000},
    {"mediatek,mt6789-mmsys",0x14000000},
    {"mediatek,dsi0",0x14013000},
    {"mediatek,mipi_tx_config0",0x11F60000}
  };
  if (Tree == NULL || Bytes < sizeof (FDT_HEADER) || Bytes > 0x200000 ||
      FdtCheckFull (Tree,Bytes) || FdtTotalSize (Tree) != Bytes) return EFI_COMPROMISED_DATA;
  INT32 Length;
  CONST VOID *P = FdtGetProp (Tree,0,"compatible",&Length);
  if (P == NULL || FdtStringListContains (P,Length,"mediatek,MT6789") != 1) return EFI_UNSUPPORTED;
  for (UINTN I = 0; I < ARRAY_SIZE (Nodes); ++I) {
    INT32 Node = FdtNodeOffsetByCompatible (Tree,-1,Nodes[I].Name);
    if (Node < 0 || FdtNodeOffsetByCompatible (Tree,Node,Nodes[I].Name) >= 0 ||
        !Enabled (Tree,Node)) return EFI_UNSUPPORTED;
    INT32 Parent = FdtParentOffset (Tree,Node);
    if (Parent < 0 || FdtAddressCells (Tree,Parent) != 2 ||
        FdtSizeCells (Tree,Parent) != 2) return EFI_UNSUPPORTED;
    P = FdtGetProp (Tree,Parent,"ranges",&Length);
    if (Parent != 0 && (P == NULL || Length != 0)) return EFI_UNSUPPORTED;
    /* A nonempty ancestor ranges could translate even a literal child reg. */
    for (INT32 Ancestor = Parent; Ancestor > 0; Ancestor = FdtParentOffset (Tree,Ancestor)) {
      P = FdtGetProp (Tree,Ancestor,"ranges",&Length);
      if (P == NULL || Length != 0) return EFI_UNSUPPORTED;
    }
    P = FdtGetProp (Tree,Node,"reg",&Length);
    if (P == NULL || Length != 16 || Cells (P,8) != Nodes[I].Base ||
        Cells ((CONST UINT8 *)P+8,8) != 0x1000) return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC BOOLEAN
Mapped (UINT64 Base)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  if (gDS == NULL || gDS->GetMemorySpaceDescriptor == NULL ||
      EFI_ERROR (gDS->GetMemorySpaceDescriptor (Base,&D))) return FALSE;
  return D.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo && Base >= D.BaseAddress &&
    Base-D.BaseAddress <= D.Length && 0x1000 <= D.Length-(Base-D.BaseAddress) &&
    (D.Attributes & EFI_MEMORY_UC) &&
    !(D.Attributes & (EFI_MEMORY_WB|EFI_MEMORY_WT|EFI_MEMORY_WC|EFI_MEMORY_RO|EFI_MEMORY_RP));
}

STATIC EFI_STATUS
Ready (VOID)
{
  STATIC CONST UINT64 Pages[] = {0x10006000,0x10001000,0x14000000,0x14013000,0x11F60000};
  STATIC CONST struct { UINTN Address; UINT32 Mask, Value; } Guards[] = {
    {0x1000616C,0x200000,0x200000},{0x10006170,0x200000,0x200000},
    {0x10006354,0x111F,0xD},{0x100012D0,0x1400,0},{0x100012EC,0x1400,0},
    {0x10001220,0x800040,0},{0x10001228,0x800040,0},
    {0x14000100,0x014A080A,0},{0x140001A0,1,0}
  };
  CONST VOID *Tree;
  EFI_STATUS Status = EfiGetSystemConfigurationTable (&gFdtTableGuid,(VOID **)&Tree);
  if (EFI_ERROR (Status)) return Status;
  if (MmioRead64 (0x43F7F000) != 0x0000000148303754ULL ||
      MmioRead64 (0x43F7F048) != (UINT64)(UINTN)Tree ||
      EFI_ERROR ((EFI_STATUS)MmioRead64 (0x43F7F058))) return EFI_NOT_READY;
  UINT64 Bytes = MmioRead64 (0x43F7F050);
  if (Bytes > MAX_UINTN) return EFI_COMPROMISED_DATA;
  Status = ValidateTree (Tree,(UINTN)Bytes);
  if (EFI_ERROR (Status)) return Status;
  /* All mappings are checked before the first peripheral MMIO read. */
  for (UINTN I = 0; I < ARRAY_SIZE (Pages); ++I) if (!Mapped (Pages[I])) return EFI_UNSUPPORTED;
  /* SPM/INFRA first; MMSYS reads only after DISP access is proven live. */
  for (UINTN I = 0; I < ARRAY_SIZE (Guards); ++I)
    if ((MmioRead32 (Guards[I].Address) & Guards[I].Mask) != Guards[I].Value) return EFI_NOT_READY;
  return mExiting ? EFI_ABORTED : EFI_SUCCESS;
}

STATIC EFI_STATUS
Enter (MTK_DSI_HOST_PROTOCOL *This, UINT64 Token, BOOLEAN RequireOwner)
{
  if (This != &mHost || This->Revision != MTK_DSI_HOST_REVISION) return EFI_INVALID_PARAMETER;
  if (mExiting || gBS == NULL || gBS->RaiseTPL == NULL || gBS->RestoreTPL == NULL) return EFI_ABORTED;
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  if (Tpl > TPL_CALLBACK) return EFI_NOT_READY;
  if (InterlockedCompareExchange32 (&mBusy,0,1) != 0) return EFI_NOT_READY;
  if (mExiting) { (VOID)InterlockedCompareExchange32 (&mBusy,1,0); return EFI_ABORTED; }
  if (RequireOwner && (Token == 0 || Token != mToken || mOwner == NULL)) {
    (VOID)InterlockedCompareExchange32 (&mBusy,1,0);
    return EFI_ACCESS_DENIED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Leave (EFI_STATUS Status)
{
  (VOID)InterlockedCompareExchange32 (&mBusy,1,0);
  return Status;
}

STATIC EFI_STATUS
CoreResult (MTK_DSI_STATUS Status)
{
  if (mExiting) return EFI_ABORTED;
  if (Status == MtkDsiSuccess) return EFI_SUCCESS;
  if (Status == MtkDsiInvalid) return EFI_INVALID_PARAMETER;
  if (Status == MtkDsiTimeout) return EFI_TIMEOUT;
  if (Status == MtkDsiWrongState || Status == MtkDsiBusy) return EFI_NOT_READY;
  return EFI_DEVICE_ERROR;
}

STATIC BOOLEAN
Address (MTK_DSI_BANK Bank, UINT32 Offset, UINTN *Result)
{
  if ((Offset & 3U) != 0) return FALSE;
  if (Bank == MtkDsiBankDsi0 &&
      (Offset == 0x108 || Offset == 0x1A8 || (Offset >= 0x13000 && Offset < 0x13400))) {
    *Result = 0x14000000U + Offset; return TRUE;
  }
  if (Bank == MtkDsiBankMipiTx && Offset < 0x1000) {
    *Result = 0x11F60000U + Offset; return TRUE;
  }
  return FALSE;
}

STATIC MTK_DSI_STATUS
Read32 (VOID *Context, MTK_DSI_BANK Bank, UINT32 Offset, UINT32 *Value)
{
  UINTN A;
  if (Context != &mHost || mExiting || Value == NULL || !Address (Bank,Offset,&A)) return MtkDsiIoError;
  *Value = MmioRead32 (A); return mExiting ? MtkDsiIoError : MtkDsiSuccess;
}

STATIC MTK_DSI_STATUS
Write32 (VOID *Context, MTK_DSI_BANK Bank, UINT32 Offset, UINT32 Value)
{
  UINTN A;
  if (Context != &mHost || mExiting || !Address (Bank,Offset,&A)) return MtkDsiIoError;
  MmioWrite32 (A,Value); MemoryFence (); return mExiting ? MtkDsiIoError : MtkDsiSuccess;
}

STATIC MTK_DSI_STATUS
DelayUs (VOID *Context, UINT32 Us)
{
  if (Context != &mHost || mExiting) return MtkDsiDelayError;
  MicroSecondDelay (Us);
  return mExiting ? MtkDsiDelayError : MtkDsiSuccess;
}

STATIC EFI_STATUS EFIAPI
Acquire (MTK_DSI_HOST_PROTOCOL *This, EFI_HANDLE Agent, UINT64 *Token)
{
  if (Agent == NULL || Token == NULL) return EFI_INVALID_PARAMETER;
  EFI_STATUS Status = Enter (This,0,FALSE);
  if (EFI_ERROR (Status)) return Status;
  if (mOwner != NULL) return Leave (EFI_ACCESS_DENIED);
  if (mCore.State == MtkDsiStateQuarantined) return Leave (EFI_DEVICE_ERROR);
  if (mNextToken == MAX_UINT64) return Leave (EFI_OUT_OF_RESOURCES);
  Status = Ready ();
  if (EFI_ERROR (Status)) return Leave (Status);
  mOwner = Agent; mToken = ++mNextToken; *Token = mToken;
  return Leave (EFI_SUCCESS);
}

STATIC EFI_STATUS EFIAPI
Configure (MTK_DSI_HOST_PROTOCOL *This, UINT64 Token, CONST MTK_DSI_OP7_PROFILE *Profile)
{
  MTK_DSI_OP7_PROFILE Local;
  MTK_DSI_PORT_SHADOW Checked;
  MTK_DSI_CORE_IO Io = {&mHost,Read32,Write32,DelayUs};
  EFI_STATUS Status = Enter (This,Token,TRUE);
  if (EFI_ERROR (Status)) return Status;
  if (Profile == NULL) return Leave (EFI_INVALID_PARAMETER);
  if (mCore.State == MtkDsiStateQuarantined) return Leave (EFI_DEVICE_ERROR);
  if (mCore.Signature != 0 && mCore.State != MtkDsiStateIdle) return Leave (EFI_NOT_READY);
  Local = *Profile;
  ZeroMem (&Local.Initial,sizeof (Local.Initial));
  if (MtkDsiCoreBuildAlongCyxConfigShadow (&Local,&Local.Initial,&Checked) != MtkDsiSuccess)
    return Leave (EFI_UNSUPPORTED);
  Status = Ready ();
  if (EFI_ERROR (Status)) return Leave (Status);
  /* Never reset an inherited LK video stream. The owner must stop it first
   * through its original controller; this provider does not silently adopt it. */
  if ((MmioRead32 (0x14013000) & 1U) || (MmioRead32 (0x14013014) & 3U) ||
      (MmioRead32 (0x1401300C) & 0x80000000U)) return Leave (EFI_ACCESS_DENIED);
  Local.Initial.Reg08 = MmioRead32 (0x14013008);
  Local.Initial.Reg14 = MmioRead32 (0x14013014);
  Local.Initial.Reg18 = MmioRead32 (0x14013018);
  Local.Initial.Reg1C = MmioRead32 (0x1401301C);
  Local.Initial.Reg64 = MmioRead32 (0x14013064);
  Local.Initial.Reg200 = MmioRead32 (0x14013200);
  if (mCore.Signature == 0) {
    MTK_DSI_STATUS Result = MtkDsiCoreInitialize (&mCore,&Io);
    if (Result != MtkDsiSuccess) return Leave (CoreResult (Result));
  }
  return Leave (CoreResult (MtkDsiCoreRunAlongCyxOp7 (&mCore,&Local)));
}

STATIC EFI_STATUS
Transition (MTK_DSI_HOST_PROTOCOL *This, UINT64 Token, UINTN Operation)
{
  EFI_STATUS Status = Enter (This,Token,TRUE);
  if (EFI_ERROR (Status)) return Status;
  if (mCore.State == MtkDsiStateQuarantined) return Leave (EFI_DEVICE_ERROR);
  MTK_DSI_CORE_STATE Expected = Operation == 0 ? MtkDsiStateOp7Complete :
                                 Operation == 1 ? MtkDsiStateStreaming : MtkDsiStateSuspended;
  if (mCore.State != Expected) return Leave (EFI_NOT_READY);
  Status = Ready ();
  if (EFI_ERROR (Status)) return Leave (Status);
  MTK_DSI_STATUS Result = Operation == 0 ? MtkDsiCoreStartVideo (&mCore) :
                         Operation == 1 ? MtkDsiCoreSuspend (&mCore,1000) : MtkDsiCoreResume (&mCore);
  return Leave (CoreResult (Result));
}
STATIC EFI_STATUS EFIAPI StartVideo (MTK_DSI_HOST_PROTOCOL *T, UINT64 K) { return Transition (T,K,0); }
STATIC EFI_STATUS EFIAPI Suspend (MTK_DSI_HOST_PROTOCOL *T, UINT64 K) { return Transition (T,K,1); }
STATIC EFI_STATUS EFIAPI Resume (MTK_DSI_HOST_PROTOCOL *T, UINT64 K) { return Transition (T,K,2); }

STATIC EFI_STATUS EFIAPI
Release (MTK_DSI_HOST_PROTOCOL *This, UINT64 Token)
{
  EFI_STATUS Status = Enter (This,Token,TRUE);
  if (EFI_ERROR (Status)) return Status;
  if (mCore.State != MtkDsiStateUninitialized && mCore.State != MtkDsiStateIdle &&
      mCore.State != MtkDsiStateSuspended) return Leave (EFI_NOT_READY);
  mOwner = NULL; mToken = 0;
  return Leave (EFI_SUCCESS);
}

STATIC EFI_STATUS EFIAPI
QueryState (MTK_DSI_HOST_PROTOCOL *This, MTK_DSI_CORE_STATE *State)
{
  if (State == NULL) return EFI_INVALID_PARAMETER;
  EFI_STATUS Status = Enter (This,0,FALSE);
  if (EFI_ERROR (Status)) return Status;
  *State = mCore.State;
  return Leave (EFI_SUCCESS);
}

STATIC MTK_DSI_HOST_PROTOCOL mHost = {
  MTK_DSI_HOST_REVISION,Acquire,Configure,StartVideo,Suspend,Resume,Release,QueryState
};
STATIC VOID EFIAPI
Exit (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context; mExiting = TRUE;
}
EFI_STATUS EFIAPI
T70DsiPlatformEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  (VOID)ImageHandle; (VOID)SystemTable;
  if (mRegistered) return EFI_ALREADY_STARTED;
  EFI_STATUS Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,
                                         &gEfiEventExitBootServicesGuid,&mExitEvent);
  if (EFI_ERROR (Status)) return Status;
  mRegistered = TRUE;
  (VOID)gBS->InstallMultipleProtocolInterfaces (&mHandle,&gMtkDsiHostProtocolGuid,&mHost,NULL);
  /* The live event owns this image even if publication fails. */
  return EFI_SUCCESS;
}
