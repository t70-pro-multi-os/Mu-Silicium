// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkVcn28Core.h>
static MTK_VCN28_STATUS_CODE Read(MTK_VCN28 *D,unsigned A,unsigned *V) {
  MTK_VCN28_STATUS_CODE S=D->Io.Read16(D->Io.Context,A,V);
  if (S!=MtkVcn28Ok) return MtkVcn28IoError;
  return *V<=0xffffU?MtkVcn28Ok:MtkVcn28IoError;
}
static MTK_VCN28_STATUS_CODE WriteChecked(MTK_VCN28 *D,unsigned A,unsigned V) {
  unsigned R;
  MTK_VCN28_STATUS_CODE S=Read(D,A,&R);
  if (S!=MtkVcn28Ok || R==V) return S;
  if (D->Io.Write16(D->Io.Context,A,V)!=MtkVcn28Ok) return MtkVcn28IoError;
  S=Read(D,A,&R);
  return S!=MtkVcn28Ok?S:(R==V?MtkVcn28Ok:MtkVcn28IoError);
}
static MTK_VCN28_STATUS_CODE WaitOn(MTK_VCN28 *D) {
  unsigned I,V;
  for (I=0;I<MTK_VCN28_WAIT_POLLS;I++) {
    MTK_VCN28_STATUS_CODE S=Read(D,MTK_VCN28_STATUS,&V);
    if (S!=MtkVcn28Ok) return S;
    if (V&0x8000U) return MtkVcn28Ok;
    if (D->Io.DelayUs(D->Io.Context,10)!=MtkVcn28Ok) return MtkVcn28IoError;
  }
  return MtkVcn28Timeout;
}
static MTK_VCN28_STATUS_CODE Settle(MTK_VCN28 *D) {
  if (D->DelayPending) {
    if (D->Io.DelayUs(D->Io.Context,MTK_VCN28_ENABLE_DELAY_US)!=MtkVcn28Ok)
      return MtkVcn28IoError;
    D->DelayPending=0;
  }
  return MtkVcn28Ok;
}
static MTK_VCN28_STATUS_CODE Ready(MTK_VCN28 *D) {
  unsigned E,M,S;
  MTK_VCN28_STATUS_CODE R=Read(D,MTK_VCN28_ENABLE,&E);
  if (R!=MtkVcn28Ok) return R;
  R=Read(D,MTK_VCN28_OP_ENABLE,&M); if (R!=MtkVcn28Ok) return R;
  R=Read(D,MTK_VCN28_OP_CONFIG,&S); if (R!=MtkVcn28Ok) return R;
  if (!(E&1U) || M!=3 || S!=0) return MtkVcn28Busy;
  return WaitOn(D);
}
MTK_VCN28_STATUS_CODE MtkVcn28Initialize(MTK_VCN28 *D,
  const MTK_VCN28_IO *Io,unsigned AdieChipId) {
  if (!D || !Io || !Io->Authorized || !AdieChipId || AdieChipId>=0xffffU ||
      (AdieChipId!=0x6635U && (!Io->Read16 || !Io->Write16 || !Io->DelayUs)))
    return MtkVcn28Invalid;
  if (D->Initialized || D->Users || D->Captured || D->Faulted || D->DelayPending) return MtkVcn28Busy;
  D->Io=*Io; D->AdieChipId=AdieChipId; D->Initialized=1;
  return MtkVcn28Ok;
}
MTK_VCN28_STATUS_CODE MtkVcn28Set(MTK_VCN28 *D,unsigned User,unsigned Enable) {
  MTK_VCN28_STATUS_CODE R;
  unsigned E,M,S;
  if (!D || !D->Initialized || (User!=MTK_VCN28_GPS && User!=MTK_VCN28_FM) ||
      Enable>1 || (D->Users&~3U)) return MtkVcn28Invalid;
  if (!D->Io.Authorized(D->Io.Context)) return MtkVcn28Offline;
  if (D->AdieChipId==0x6635U) {
    if (Enable) D->Users|=User; else D->Users&=~User;
    return MtkVcn28Ok;
  }
  if (Enable) {
    if (D->Faulted) return MtkVcn28Uncertain;
    if (D->Users) {
      R=Ready(D); if (R!=MtkVcn28Ok) goto Failed;
      D->Users|=User; return MtkVcn28Ok;
    }
    if (D->Captured) return MtkVcn28Uncertain;
    R=Read(D,MTK_VCN28_ENABLE,&E); if (R!=MtkVcn28Ok) return R;
    R=Read(D,MTK_VCN28_OP_ENABLE,&M); if (R!=MtkVcn28Ok) return R;
    R=Read(D,MTK_VCN28_OP_CONFIG,&S); if (R!=MtkVcn28Ok) return R;
    D->SavedEnable=E; D->SavedOpEnable=M; D->SavedOpConfig=S;
    /* Save ownership before any write that might reach the physical rail. */
    D->Captured=1; D->Users=User;
    R=WriteChecked(D,MTK_VCN28_OP_ENABLE,3); if (R!=MtkVcn28Ok) goto Failed;
    R=WriteChecked(D,MTK_VCN28_OP_CONFIG,0); if (R!=MtkVcn28Ok) goto Failed;
    if (!(E&1U)) D->DelayPending=1;
    R=WriteChecked(D,MTK_VCN28_ENABLE,E|1U); if (R!=MtkVcn28Ok) goto Failed;
    R=Settle(D); if (R!=MtkVcn28Ok) goto Failed;
    R=Ready(D); if (R!=MtkVcn28Ok) goto Failed;
    return MtkVcn28Ok;
  }
  if (!(D->Users&User)) return MtkVcn28Ok;
  if (D->Users&~User) { D->Users&=~User; return MtkVcn28Ok; }
  if (!D->Captured) return MtkVcn28Uncertain;
  /* Unlike the original unconditional mode=0/regulator_disable path, preserve
   * the inherited mode and the board's always-on supply. Stable writes can be
   * retried by the same exclusive owner after a failed/ignored PMIC access. */
  R=WriteChecked(D,MTK_VCN28_OP_ENABLE,D->SavedOpEnable); if (R!=MtkVcn28Ok) goto Failed;
  R=WriteChecked(D,MTK_VCN28_OP_CONFIG,D->SavedOpConfig); if (R!=MtkVcn28Ok) goto Failed;
  R=Read(D,MTK_VCN28_ENABLE,&E); if (R!=MtkVcn28Ok) goto Failed;
  if (!(E&1U)) D->DelayPending=1;
  R=WriteChecked(D,MTK_VCN28_ENABLE,D->SavedEnable|1U); if (R!=MtkVcn28Ok) goto Failed;
  R=Settle(D); if (R!=MtkVcn28Ok) goto Failed;
  R=WaitOn(D); if (R!=MtkVcn28Ok) goto Failed;
  D->Users=D->Captured=D->Faulted=0;
  D->SavedEnable=D->SavedOpEnable=D->SavedOpConfig=0;
  return MtkVcn28Ok;
Failed:
  D->Faulted=1; return R;
}
