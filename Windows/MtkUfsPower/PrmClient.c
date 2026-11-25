/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PrmClient.h"
#include <prminterface.h>

typedef enum {
  MtkUfsPrmClientUninitialized = 0,
  MtkUfsPrmClientPinned = 1,
  MtkUfsPrmClientQuarantined = 2
} MTK_UFS_PRM_CLIENT_STATE;

static const GUID mHandlerGuid = {
  0xc9aef12d, 0xe26c, 0x4be7, { 0xb3, 0x45, 0x62, 0xc5, 0xf3, 0x06, 0xd1, 0x84 }
};
static PRM_INTERFACE mInterface;
static volatile LONG mClientState = MtkUfsPrmClientUninitialized;
static volatile LONG mClientBusy;

static NTSTATUS MtkUfsPrmExactStatus(NTSTATUS Status) {
  return Status==STATUS_SUCCESS?STATUS_SUCCESS:
    (NT_SUCCESS(Status)?STATUS_UNSUCCESSFUL:Status);
}

static NTSTATUS MtkUfsPrmAcquireSession(VOID) {
  PRM_INTERFACE Interface;
  BOOLEAN Found=FALSE;
  NTSTATUS Status,UnlockStatus;
  GUID Guid=mHandlerGuid;

  RtlZeroMemory(&Interface,sizeof(Interface));
  Status=MtkUfsPrmExactStatus(ExGetPrmInterface(PRM_INTERFACE_V1,&Interface));
  if(Status!=STATUS_SUCCESS) return Status;
  if(Interface.Version!=PRM_INTERFACE_V1 || !Interface.LockModule || !Interface.UnlockModule ||
     !Interface.QueryHandler || !Interface.InvokeHandler) return STATUS_NOT_SUPPORTED;

  Status=MtkUfsPrmExactStatus(Interface.LockModule(&Guid));
  if(Status!=STATUS_SUCCESS) return Status;
  Status=MtkUfsPrmExactStatus(Interface.QueryHandler(&Guid,&Found));
  if(Status==STATUS_SUCCESS && Found!=TRUE) Status=STATUS_NOT_SUPPORTED;
  if(Status!=STATUS_SUCCESS) {
    UnlockStatus=MtkUfsPrmExactStatus(Interface.UnlockModule(&Guid));
    if(UnlockStatus!=STATUS_SUCCESS) {
      /* The OS may still hold the update pin; never retry or forget it. */
      mClientState=MtkUfsPrmClientQuarantined;
      return UnlockStatus;
    }
    return Status;
  }

  mInterface=Interface;
  mClientState=MtkUfsPrmClientPinned;
  return STATUS_SUCCESS;
}

NTSTATUS MtkUfsPrmInvoke(MTK_UFS_POWER_REQUEST *Request) {
  ULONG64 EfiStatus=MAXULONGLONG;
  NTSTATUS Status;
  GUID Guid=mHandlerGuid;
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Request ||
     Request->Size!=sizeof(*Request) || Request->Version!=MTK_UFS_POWER_VERSION ||
     Request->Operation>MtkUfsPowerRestore) return STATUS_INVALID_PARAMETER;
  if(InterlockedCompareExchange(&mClientBusy,1,0)!=0) return STATUS_DEVICE_BUSY;

  if(mClientState==MtkUfsPrmClientQuarantined) {
    Status=STATUS_DEVICE_NOT_READY;
    goto Exit;
  }
  if(mClientState==MtkUfsPrmClientUninitialized) {
    Status=MtkUfsPrmAcquireSession();
    if(Status!=STATUS_SUCCESS) goto Exit;
  }

  Status=MtkUfsPrmExactStatus(mInterface.InvokeHandler(&Guid,Request,0,&EfiStatus));
  if(Status!=STATUS_SUCCESS || EfiStatus!=0) {
    /* An invocation may have reached hardware even when its reply is unusable. */
    mClientState=MtkUfsPrmClientQuarantined;
    if(Status==STATUS_SUCCESS) Status=STATUS_UNSUCCESSFUL;
    goto Exit;
  }
  Status=STATUS_SUCCESS;

Exit:
  InterlockedExchange(&mClientBusy,0);
  return Status;
}
