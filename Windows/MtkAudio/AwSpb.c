// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpb.h"
#include <spb.h>
#define AW_TIMEOUT_MS 50
NTSTATUS T70AwSpbWriteRegister(WDFIOTARGET Target,USHORT Register,USHORT Value) {
  UCHAR Wire[3]; WDF_MEMORY_DESCRIPTOR Input; WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Written=0; NTSTATUS Status;
  if(!Target || Register>0xff) return STATUS_INVALID_PARAMETER;
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
  Wire[0]=(UCHAR)Register; Wire[1]=(UCHAR)(Value>>8); Wire[2]=(UCHAR)Value;
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,Wire,sizeof(Wire));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_MS(AW_TIMEOUT_MS));
  Status=WdfIoTargetSendWriteSynchronously(Target,NULL,&Input,NULL,&Options,&Written);
  if(!NT_SUCCESS(Status)) return Status;
  return Written==sizeof(Wire)?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR;
}
NTSTATUS T70AwSpbReadRegister(WDFIOTARGET Target,USHORT Register,USHORT *Value) {
  UCHAR Address=(UCHAR)Register,Data[2]={0};
  SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence={0};
  WDF_MEMORY_DESCRIPTOR Input; WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Transferred=0; NTSTATUS Status;
  if(!Target || !Value || Register>0xff) return STATUS_INVALID_PARAMETER;
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
  SPB_TRANSFER_LIST_INIT(&Sequence.List,2);
  Sequence.List.Transfers[0]=SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
    SpbTransferDirectionToDevice,0,&Address,1);
  Sequence.List.Transfers[1]=SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
    SpbTransferDirectionFromDevice,0,Data,sizeof(Data));
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,&Sequence,sizeof(Sequence));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_MS(AW_TIMEOUT_MS));
  Status=WdfIoTargetSendIoctlSynchronously(Target,NULL,IOCTL_SPB_EXECUTE_SEQUENCE,
                                         &Input,NULL,&Options,&Transferred);
  if(!NT_SUCCESS(Status)) return Status;
  if(Transferred!=3) return STATUS_DEVICE_DATA_ERROR;
  *Value=(USHORT)(((USHORT)Data[0]<<8)|Data[1]);
  return STATUS_SUCCESS;
}

/* Adapt the pure masking core to the real, exact-length SPB transport. */
#include "AwMask.h"
typedef struct { WDFIOTARGET Target; NTSTATUS Status; } AW_MASK_CONTEXT;
static int AwMaskRead(void *Context,uint16_t Register,uint16_t *Value) {
  AW_MASK_CONTEXT *C=Context;
  C->Status=T70AwSpbReadRegister(C->Target,Register,Value);
  return NT_SUCCESS(C->Status);
}
static int AwMaskWrite(void *Context,uint16_t Register,uint16_t Value) {
  AW_MASK_CONTEXT *C=Context;
  C->Status=T70AwSpbWriteRegister(C->Target,Register,Value);
  return NT_SUCCESS(C->Status);
}
NTSTATUS T70AwSpbUpdatePreservedBits(WDFIOTARGET Target,USHORT Register,
                                    USHORT PreserveMask,USHORT Value) {
  AW_MASK_CONTEXT C={Target,STATUS_INVALID_PARAMETER};
  T70_AW_REGISTER_IO Io={&C,AwMaskRead,AwMaskWrite};
  if(!Target || Register>0xff)return STATUS_INVALID_PARAMETER;
  if(!T70AwUpdatePreservedBits(&Io,Register,PreserveMask,Value))return C.Status;
  return STATUS_SUCCESS;
}

#include "AwIdentity.h"
NTSTATUS T70AwSpbIdentify88261(WDFIOTARGET Target,USHORT *ChipId) {
  AW_MASK_CONTEXT C={Target,STATUS_INVALID_PARAMETER};
  T70_AW_REGISTER_IO Io={&C,AwMaskRead,NULL};
  if(!Target || !ChipId)return STATUS_INVALID_PARAMETER;
  if(!T70Aw88261Identify(&Io,ChipId))
    return NT_SUCCESS(C.Status)?STATUS_NO_SUCH_DEVICE:C.Status;
  return STATUS_SUCCESS;
}

#include "AwVolumeUpdate.h"
static int32_t AwStatusRead(void *Context,uint16_t Register,uint16_t *Value) {
  return T70AwSpbReadRegister((WDFIOTARGET)Context,Register,Value);
}
static int32_t AwStatusWrite(void *Context,uint16_t Register,uint16_t Value) {
  return T70AwSpbWriteRegister((WDFIOTARGET)Context,Register,Value);
}
NTSTATUS T70AwSpbUpdateVolume(WDFIOTARGET Target,USHORT Initial,
                             uint32_t Requested,USHORT *AppliedRegister) {
  T70_AW_VOLUME_IO Io={Target,AwStatusRead,AwStatusWrite};
  T70_AW_VOLUME_UPDATE_RESULT Result;
  if(!Target)return STATUS_INVALID_PARAMETER;
  Result=T70Aw88261UpdateVolume(&Io,Initial,Requested,AppliedRegister);
  if(Result.Code==T70AwVolumeInvalid)return STATUS_INVALID_PARAMETER;
  return Result.Code==T70AwVolumeUpdated?STATUS_SUCCESS:Result.TransportStatus;
}

#include "AwTxUpdate.h"
NTSTATUS T70AwSpbUpdateTx(WDFIOTARGET Target,unsigned Enabled,USHORT *AppliedRegister) {
  T70_AW_TX_IO Io={Target,AwStatusRead,AwStatusWrite};
  T70_AW_TX_UPDATE_RESULT Result;
  if(!Target)return STATUS_INVALID_PARAMETER;
  Result=T70Aw88261UpdateTx(&Io,Enabled,AppliedRegister);
  if(Result.Code==T70AwTxInvalid)return STATUS_INVALID_PARAMETER;
  return Result.Code==T70AwTxUpdated?STATUS_SUCCESS:Result.TransportStatus;
}

NTSTATUS T70AwSpbReadClockStatus(WDFIOTARGET Target,T70_AW_CLOCK_SNAPSHOT *Snapshot) {
  T70_AW_STATUS_IO Io={Target,AwStatusRead};T70_AW_STATUS_RESULT Result;
  if(!Target)return STATUS_INVALID_PARAMETER;
  Result=T70Aw88261ReadClockStatus(&Io,Snapshot);
  if(Result.Code==T70AwStatusInvalid)return STATUS_INVALID_PARAMETER;
  return Result.Code==T70AwStatusRead?STATUS_SUCCESS:Result.TransportStatus;
}
