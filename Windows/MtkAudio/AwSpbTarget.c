// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbTarget.h"
#include "AwBoard.h"
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>

static NTSTATUS Check(const T70_AW_SPB_TARGET_OWNER *O){
 if(!O)return STATUS_INVALID_PARAMETER;
 return KeGetCurrentIrql()==PASSIVE_LEVEL?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
}
NTSTATUS T70AwSpbTargetAcquire(T70_AW_SPB_TARGET_OWNER *O,WDFDEVICE Device,
 uint64_t Id,uint64_t Base,uint16_t Address,uint16_t Channel){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(!Device||!T70AwBoardTupleValid(Base,Address,Channel))return STATUS_INVALID_PARAMETER;
 if(O->Target||O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 WCHAR Buffer[RESOURCE_HUB_PATH_SIZE/sizeof(WCHAR)];
 UNICODE_STRING Path={0,sizeof(Buffer),Buffer};
 S=RESOURCE_HUB_CREATE_PATH_FROM_ID(&Path,(ULONG)Id,(ULONG)(Id>>32));
 if(!NT_SUCCESS(S))return S;
 WDFIOTARGET Target=NULL;
 S=WdfIoTargetCreate(Device,WDF_NO_OBJECT_ATTRIBUTES,&Target);
 if(!NT_SUCCESS(S))return S;
 WDF_IO_TARGET_OPEN_PARAMS Params;
 WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Params,&Path,GENERIC_READ|GENERIC_WRITE);
 Params.ShareAccess=0;Params.CreateDisposition=FILE_OPEN;Params.FileAttributes=FILE_ATTRIBUTE_NORMAL;
 S=WdfIoTargetOpen(Target,&Params);
 if(!NT_SUCCESS(S)){WdfObjectDelete(Target);return S;}
 /* Opening the resource-hub connection reparses to the MtkI2c SPB target;
  * its TargetConnect callback validates the ACPI descriptor. */
 O->Target=Target;return STATUS_SUCCESS;
}
NTSTATUS T70AwSpbTargetBegin(T70_AW_SPB_TARGET_OWNER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(!O->Target||O->ActiveUses==UINT32_MAX)return STATUS_INVALID_DEVICE_STATE;
 ++O->ActiveUses;return STATUS_SUCCESS;
}
NTSTATUS T70AwSpbTargetEnd(T70_AW_SPB_TARGET_OWNER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(!O->Target||!O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 --O->ActiveUses;return STATUS_SUCCESS;
}
NTSTATUS T70AwSpbTargetRelease(T70_AW_SPB_TARGET_OWNER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 if(!O->Target)return STATUS_SUCCESS;
 WdfIoTargetClose(O->Target);WdfObjectDelete(O->Target);
 O->Target=NULL;return STATUS_SUCCESS;
}
