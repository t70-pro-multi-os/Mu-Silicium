/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>
#include <acpiioct.h>
#include "Hardware.h"
#include "GpsLnaInterface.h"
#include "ConnPinsInterface.h"
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MgDeviceAdd;
EVT_WDF_DRIVER_UNLOAD MgDriverUnload;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS MgPnpPreprocess;
GPIO_CLIENT_PREPARE_CONTROLLER MgPrepare;
GPIO_CLIENT_RELEASE_CONTROLLER MgRelease;
GPIO_CLIENT_START_CONTROLLER MgControllerStart;
GPIO_CLIENT_STOP_CONTROLLER MgControllerStop;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION MgInformation;
GPIO_CLIENT_ENABLE_INTERRUPT MgInterruptEnable;
GPIO_CLIENT_DISABLE_INTERRUPT MgInterruptDisable;
GPIO_CLIENT_MASK_INTERRUPTS MgInterruptMask;
GPIO_CLIENT_UNMASK_INTERRUPT MgInterruptUnmask;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS MgActiveInterrupts;
GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS MgEnabledInterrupts;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS MgClearInterrupts;
GPIO_CLIENT_RECONFIGURE_INTERRUPT MgReconfigure;
GPIO_CLIENT_CONNECT_IO_PINS MgConnectIo;
GPIO_CLIENT_DISCONNECT_IO_PINS MgDisconnectIo;
GPIO_CLIENT_READ_PINS_MASK MgReadPins;
GPIO_CLIENT_WRITE_PINS_MASK MgWritePins;
typedef struct MG_INTERFACE_CONTEXT MG_INTERFACE_CONTEXT;
typedef struct {
  WDFDEVICE Device;
  PUCHAR Registers[MG_SPACES];
  MG_CORE Core;
  KMUTEX ConfigurationLock;
  volatile LONG PowerTransition,ActiveDirql;
  BOOLEAN SeenStart,ReleaseFaulted;
  MG_INTERFACE_CONTEXT *Interfaces;
  ULONGLONG LnaToken[2],ConnPinsToken;
} MG_DEVICE;
/* This WDF context outlives GpioClx resource preparation. Its gate serializes
 * calls with resource release; ConfigurationLock also serializes ordinary GPIO. */
struct MG_INTERFACE_CONTEXT {
  WDFDEVICE Device;
  MG_DEVICE *Controller;
  FAST_MUTEX Gate;
  KSPIN_LOCK ReferenceLock;
  ULONGLONG NextToken;
  ULONG References;
  BOOLEAN SurpriseRemoved;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MG_INTERFACE_CONTEXT,MgGetInterfaceContext)
static VOID MgAcquireConfigurationLock(MG_DEVICE *D) {
  (void)KeWaitForSingleObject(&D->ConfigurationLock,Executive,KernelMode,FALSE,NULL);
}
static VOID MgReleaseConfigurationLock(MG_DEVICE *D) {
  (void)KeReleaseMutex(&D->ConfigurationLock,FALSE);
}
static VOID MgInterfaceReference(PVOID Context) {
  MG_INTERFACE_CONTEXT *Q=Context; KIRQL Irql;
  WdfObjectReference(Q->Device);
  KeAcquireSpinLock(&Q->ReferenceLock,&Irql);
  if (!Q->References++) WdfDeviceSetStaticStopRemove(Q->Device,FALSE);
  KeReleaseSpinLock(&Q->ReferenceLock,Irql);
}
static VOID MgInterfaceDereference(PVOID Context) {
  MG_INTERFACE_CONTEXT *Q=Context; KIRQL Irql;
  KeAcquireSpinLock(&Q->ReferenceLock,&Irql);
  if (!Q->References) { KeReleaseSpinLock(&Q->ReferenceLock,Irql); return; }
  if (!--Q->References) WdfDeviceSetStaticStopRemove(Q->Device,TRUE);
  KeReleaseSpinLock(&Q->ReferenceLock,Irql);
  WdfObjectDereference(Q->Device);
}

static NTSTATUS Convert(MG_STATUS S) {
  switch (S) {
    case MgOk:return STATUS_SUCCESS;
    case MgInvalid:return STATUS_INVALID_PARAMETER;
    case MgNotReady:return STATUS_DEVICE_NOT_READY;
    case MgBusy:return STATUS_DEVICE_BUSY;
    case MgUnsupported:return STATUS_NOT_SUPPORTED;
    default:return STATUS_IO_DEVICE_ERROR;
  }
}
/* No provider MMIO is reachable before start, during rundown, or after detach.
 * A faulted core still permits Release to retry restoring its saved baseline. */
static NTSTATUS MgLnaOperation(PVOID Context,ULONG Pin,ULONGLONG *Token,
    MTK_GPS_LNA_STATE State,unsigned Operation) {
  MG_INTERFACE_CONTEXT *Q=Context; MG_DEVICE *D; NTSTATUS S; unsigned Index=0;
  ULONGLONG Lease; BOOLEAN DropReference=FALSE;
  if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
  if (!Q || !Token) return STATUS_INVALID_PARAMETER;
  if (Operation==0) {
    *Token=0;
    if (Pin!=MTK_GPS_LNA_L1_PIN && Pin!=MTK_GPS_LNA_L5_PIN) return STATUS_INVALID_PARAMETER;
    Index=Pin-MTK_GPS_LNA_L1_PIN;
  } else if (!*Token || (Operation==1 && (unsigned)State>MtkGpsLnaOn)) return STATUS_INVALID_PARAMETER;
  ExAcquireFastMutex(&Q->Gate); D=Q->Controller;
  if (Q->SurpriseRemoved || !D) {
    ExReleaseFastMutex(&Q->Gate); return Q->SurpriseRemoved?STATUS_DEVICE_REMOVED:STATUS_DEVICE_NOT_READY;
  }
  MgAcquireConfigurationLock(D);
  if (!D->SeenStart || D->Core.Suspended || D->PowerTransition || !D->Registers[0] || !D->Registers[1]) {
    S=STATUS_DEVICE_NOT_READY; goto Done;
  }
  if (Operation) {
    if (D->LnaToken[0]==*Token) Index=0;
    else if (D->LnaToken[1]==*Token) Index=1;
    else { S=STATUS_INVALID_PARAMETER; goto Done; }
    Pin=MTK_GPS_LNA_L1_PIN+Index;
  } else if (Q->NextToken==MAXULONGLONG) {
    S=STATUS_INSUFFICIENT_RESOURCES; goto Done;
  }
  GPIO_CLX_AcquireInterruptLock(D,0);
  if (!Operation) S=Convert(MgGpsLnaAcquire(&D->Core,Pin));
  else if (Operation==1) S=Convert(MgGpsLnaSet(&D->Core,Pin,(MG_GPS_LNA_STATE)State));
  else S=Convert(MgDisconnect(&D->Core,Pin,0));
  GPIO_CLX_ReleaseInterruptLock(D,0);
  if (NT_SUCCESS(S)) {
    if (!Operation) {
      Lease=++Q->NextToken; D->LnaToken[Index]=Lease; *Token=Lease;
      /* A lease keeps the provider referenced even if a client drops its
       * interface reference too early. Never release the reference at DIRQL. */
      MgInterfaceReference(Q);
    } else if (Operation==2) {
      D->LnaToken[Index]=0; DropReference=TRUE;
    }
  }
Done:
  MgReleaseConfigurationLock(D); ExReleaseFastMutex(&Q->Gate);
  if (DropReference) MgInterfaceDereference(Q);
  return S;
}
static NTSTATUS MgLnaAcquire(PVOID Context,ULONG Pin,PULONGLONG Token) {
  return MgLnaOperation(Context,Pin,Token,MtkGpsLnaOff,0);
}
static NTSTATUS MgLnaSet(PVOID Context,ULONGLONG Token,MTK_GPS_LNA_STATE State) {
  return MgLnaOperation(Context,0,&Token,State,1);
}
static NTSTATUS MgLnaRelease(PVOID Context,ULONGLONG Token) {
  return MgLnaOperation(Context,0,&Token,MtkGpsLnaOff,2);
}
typedef enum { ConnAcquire, ConnRead, ConnWrite, ConnRelease } MG_CONN_OPERATION;
static NTSTATUS MgConnOperation(PVOID Context,MG_CONN_OPERATION Operation,PULONGLONG Token,
    MTK_CONN_PIN_SPACE Space,ULONG Offset,PULONG Value) {
  MG_INTERFACE_CONTEXT *Q=Context;
  BOOLEAN DropReference=FALSE;
  NTSTATUS Status;
  if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
  if (!Q || !Token) return STATUS_INVALID_PARAMETER;
  if (Operation==ConnAcquire) *Token=0;
  else if (!*Token || (Operation==ConnRead && !Value)) return STATUS_INVALID_PARAMETER;

  ExAcquireFastMutex(&Q->Gate);
  MG_DEVICE *D=Q->Controller;
  if (Q->SurpriseRemoved || !D) {
    Status=Q->SurpriseRemoved?STATUS_DEVICE_REMOVED:STATUS_DEVICE_NOT_READY;
    ExReleaseFastMutex(&Q->Gate);
    return Status;
  }
  MgAcquireConfigurationLock(D);
  if (!D->SeenStart || D->PowerTransition || D->Core.Suspended ||
      !D->Registers[MtkConnPinsGpio] || !D->Registers[MtkConnPinsIoCfg]) {
    Status=STATUS_DEVICE_NOT_READY;
  } else if (Operation==ConnAcquire) {
    if (Q->NextToken==MAXULONGLONG) {
      Status=STATUS_INSUFFICIENT_RESOURCES;
    } else {
      Status=Convert(MgConnPinsAcquire(&D->Core));
      if (NT_SUCCESS(Status)) {
        D->ConnPinsToken=++Q->NextToken;
        *Token=D->ConnPinsToken;
        MgInterfaceReference(Q);
      }
    }
  } else if (D->ConnPinsToken!=*Token) {
    Status=STATUS_INVALID_PARAMETER;
  } else if (Operation==ConnRead) {
    uint32_t ReadValue;
    Status=Convert(MgConnPinsRead(&D->Core,Space,Offset,&ReadValue));
    if (NT_SUCCESS(Status)) *Value=ReadValue;
  } else if (Operation==ConnWrite) {
    Status=Convert(MgConnPinsWrite(&D->Core,Space,Offset,*Value));
  } else {
    Status=Convert(MgConnPinsRelease(&D->Core));
    if (NT_SUCCESS(Status)) {
      D->ConnPinsToken=0;
      DropReference=TRUE;
    }
  }
  MgReleaseConfigurationLock(D);
  ExReleaseFastMutex(&Q->Gate);
  if (DropReference) MgInterfaceDereference(Q);
  return Status;
}
static NTSTATUS MgConnAcquire(PVOID Context,PULONGLONG Token) {
  return MgConnOperation(Context,ConnAcquire,Token,0,0,NULL);
}
static NTSTATUS MgConnRead(PVOID Context,ULONGLONG Token,MTK_CONN_PIN_SPACE Space,
    ULONG Offset,PULONG Value) {
  return MgConnOperation(Context,ConnRead,&Token,Space,Offset,Value);
}
static NTSTATUS MgConnWrite(PVOID Context,ULONGLONG Token,MTK_CONN_PIN_SPACE Space,
    ULONG Offset,ULONG Value) {
  return MgConnOperation(Context,ConnWrite,&Token,Space,Offset,&Value);
}
static NTSTATUS MgConnRelease(PVOID Context,ULONGLONG Token) {
  return MgConnOperation(Context,ConnRelease,&Token,0,0,NULL);
}
static MG_STATUS RegisterRead(void *Context,unsigned Space,unsigned Offset,uint32_t *Value) {
  MG_DEVICE *D=Context;
  if (Space>=MG_SPACES || !D->Registers[Space] || !Value || (Offset&3U) || Offset>MG_MMIO_LENGTH-4) return MgInvalid;
  *Value=READ_REGISTER_ULONG((PULONG)(D->Registers[Space]+Offset)); return MgOk;
}
static MG_STATUS RegisterWrite(void *Context,unsigned Space,unsigned Offset,uint32_t Value) {
  MG_DEVICE *D=Context;
  if (Space>=MG_SPACES || !D->Registers[Space] || (Offset&3U) || Offset>MG_MMIO_LENGTH-4) return MgInvalid;
  WRITE_REGISTER_ULONG((PULONG)(D->Registers[Space]+Offset),Value); return MgOk;
}
static VOID CopyAcpiIntegerBytes(PVOID Destination,const __unaligned UCHAR *Source,ULONG Bytes) {
  UCHAR *Output=(UCHAR *)Destination;
  ULONG Index;
  for (Index=0;Index<Bytes;Index++) Output[Index]=Source[Index];
}
static BOOLEAN MgFirmwareInteger32(PACPI_METHOD_ARGUMENT Argument,PULONG Value) {
  ULONGLONG Wide;
  if(!Argument||!Value||Argument->Type!=ACPI_METHOD_ARGUMENT_INTEGER) return FALSE;
  if(Argument->DataLength==sizeof(ULONG)) { CopyAcpiIntegerBytes(Value,Argument->Data,sizeof(*Value)); return TRUE; }
  if(Argument->DataLength!=sizeof(ULONGLONG)) return FALSE;
  CopyAcpiIntegerBytes(&Wide,Argument->Data,sizeof(Wide));
  if(Wide>MAXULONG) return FALSE;
  *Value=(ULONG)Wide; return TRUE;
}
static NTSTATUS ReadyMethod(WDFDEVICE Device) {
  ACPI_EVAL_INPUT_BUFFER In={0};
  UCHAR Storage[sizeof(ACPI_EVAL_OUTPUT_BUFFER)+sizeof(ULONGLONG)]={0};
  PACPI_EVAL_OUTPUT_BUFFER Out=(PACPI_EVAL_OUTPUT_BUFFER)Storage;
  WDF_MEMORY_DESCRIPTOR Input,Output; WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Returned=0; ULONG Value; NTSTATUS S;
  In.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE; RtlCopyMemory(In.MethodName,"GRDY",4);
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,&In,sizeof(In));
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output,Storage,sizeof(Storage));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_SEC(2));
  S=WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device),NULL,
      IOCTL_ACPI_EVAL_METHOD,&Input,&Output,&Options,&Returned);
  if (!NT_SUCCESS(S)) return S;
  if (Returned<sizeof(ACPI_EVAL_OUTPUT_BUFFER) || Returned>sizeof(Storage) ||
      Out->Signature!=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Out->Count!=1 ||
      Out->Length>Returned || Out->Length<sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
      Out->Length<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      Returned<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      !MgFirmwareInteger32(&Out->Argument[0],&Value))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  return Value==1?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}
static VOID Unmap(MG_DEVICE *D) {
  unsigned I;
  for (I=0;I<MG_SPACES;I++) if (D->Registers[I]) {
    MmUnmapIoSpace(D->Registers[I],MG_MMIO_LENGTH); D->Registers[I]=NULL;
  }
}
/* DIRQL callbacks cannot wait on ConfigurationLock. This rundown gate stops
 * new callbacks and drains in-flight ones before a PASSIVE power transition. */
static BOOLEAN EnterDirql(MG_DEVICE *D) {
  if (InterlockedCompareExchange(&D->PowerTransition,0,0)) return FALSE;
  InterlockedIncrement(&D->ActiveDirql);
  if (InterlockedCompareExchange(&D->PowerTransition,0,0)) {
    InterlockedDecrement(&D->ActiveDirql); return FALSE;
  }
  return TRUE;
}
static VOID LeaveDirql(MG_DEVICE *D) { InterlockedDecrement(&D->ActiveDirql); }
static VOID BeginTransition(MG_DEVICE *D) {
  InterlockedExchange(&D->PowerTransition,1);
  while (InterlockedCompareExchange(&D->ActiveDirql,0,0))
    KeStallExecutionProcessor(1);
}
static VOID EndTransition(MG_DEVICE *D) { InterlockedExchange(&D->PowerTransition,0); }
/* GpioClx owns the WDF PnP callbacks. Preprocess only surprise removal and
 * return the IRP to WDF; never replace the class extension's power callbacks. */
NTSTATUS MgPnpPreprocess(WDFDEVICE Device,PIRP Irp) {
  MG_INTERFACE_CONTEXT *Q=MgGetInterfaceContext(Device); MG_DEVICE *D; unsigned Drop=0,I;
  if (IoGetCurrentIrpStackLocation(Irp)->MinorFunction==IRP_MN_SURPRISE_REMOVAL) {
    ExAcquireFastMutex(&Q->Gate); Q->SurpriseRemoved=TRUE; D=Q->Controller;
    if (D) {
      MgAcquireConfigurationLock(D); BeginTransition(D);
      D->Core.Active=0; D->SeenStart=FALSE;
      for (I=0;I<2;I++) if (D->LnaToken[I]) { D->LnaToken[I]=0; Drop++; }
      if (D->ConnPinsToken) { D->ConnPinsToken=0; Drop++; }
      MgReleaseConfigurationLock(D);
    }
    ExReleaseFastMutex(&Q->Gate);
    /* The hardware no longer exists. Do not attempt a pin restore or any MMIO. */
    while (Drop) { Drop--; MgInterfaceDereference(Q); }
  }
  IoSkipCurrentIrpStackLocation(Irp);
  return WdfDeviceWdmDispatchPreprocessedIrp(Device,Irp);
}
NTSTATUS MgPrepare(WDFDEVICE Device,PVOID Context,WDFCMRESLIST Raw,WDFCMRESLIST Translated) {
  MG_DEVICE *D=Context; ULONG I,Mem=0,Irqs=0,Count; PHYSICAL_ADDRESS Bases[MG_SPACES]; MG_IO Io;
  const ULONGLONG Expected[MG_SPACES]={0x10005000ULL,0x1000B000ULL,0x11C30000ULL,0x11D10000ULL,0x11D40000ULL,0x11E20000ULL,0x11E60000ULL,0x11EA0000ULL};
  if (!D) return STATUS_INVALID_DEVICE_STATE;
  /* Never erase an unresolved pin/IRQ journal with MgInitialize after detach. */
  if (D->ReleaseFaulted) return STATUS_DEVICE_NOT_READY;
  for (I=0;I<MG_SPACES;I++) if (D->Registers[I]) return STATUS_INVALID_DEVICE_STATE;
  Count=WdfCmResourceListGetCount(Translated);
  if (WdfCmResourceListGetCount(Raw)!=Count) return STATUS_DEVICE_CONFIGURATION_ERROR;
  /* Validate the full _CRS before mapping: GPIO, EINT, all six IOCFG blocks, then one shared parent
   * interrupt resource for each of the six logical 32-pin banks. */
  for (I=0;I<Count;I++) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR R=WdfCmResourceListGetDescriptor(Translated,I);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Q=WdfCmResourceListGetDescriptor(Raw,I);
    if (!R || !Q || R->Type!=Q->Type) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (R->Type==CmResourceTypeMemory) {
      if (Mem>=MG_SPACES || Irqs || Q->u.Memory.Start.QuadPart!=Expected[Mem] ||
          R->u.Memory.Start.QuadPart!=Expected[Mem] || Q->u.Memory.Length!=MG_MMIO_LENGTH ||
          R->u.Memory.Length!=MG_MMIO_LENGTH) return STATUS_DEVICE_CONFIGURATION_ERROR;
      Bases[Mem++]=R->u.Memory.Start;
    } else if (R->Type==CmResourceTypeInterrupt) {
      if (Mem!=MG_SPACES || ++Irqs>MG_BANKS || Q->u.Interrupt.Vector!=276 ||
          (Q->Flags&CM_RESOURCE_INTERRUPT_LATCHED) || (R->Flags&CM_RESOURCE_INTERRUPT_LATCHED) ||
          Q->ShareDisposition!=CmResourceShareShared || R->ShareDisposition!=CmResourceShareShared)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    } else if (R->Type!=CmResourceTypeDevicePrivate) return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  if (Mem!=MG_SPACES || Irqs!=MG_BANKS) return STATUS_DEVICE_CONFIGURATION_ERROR;
  KeInitializeMutex(&D->ConfigurationLock,0);
  D->Device=Device; D->Interfaces=MgGetInterfaceContext(Device);
  if (!D->Interfaces) return STATUS_INVALID_DEVICE_STATE;
  if (D->Interfaces->SurpriseRemoved) return STATUS_DEVICE_REMOVED;
  InterlockedExchange(&D->PowerTransition,1);
  InterlockedExchange(&D->ActiveDirql,0); D->SeenStart=FALSE;
  for (I=0;I<MG_SPACES;I++) {
    D->Registers[I]=MmMapIoSpaceEx(Bases[I],MG_MMIO_LENGTH,PAGE_READWRITE|PAGE_NOCACHE);
    if (!D->Registers[I]) { Unmap(D); return STATUS_INSUFFICIENT_RESOURCES; }
  }
  Io.Context=D; Io.Read=RegisterRead; Io.Write=RegisterWrite;
  {
    NTSTATUS S=Convert(MgInitialize(&D->Core,&Io));
    if (NT_SUCCESS(S)) {
      ExAcquireFastMutex(&D->Interfaces->Gate);
      D->LnaToken[0]=D->LnaToken[1]=0;
      D->ConnPinsToken=0; D->Interfaces->Controller=D;
      ExReleaseFastMutex(&D->Interfaces->Gate);
    }
    return S;
  }
}
static BOOLEAN HasOwnedState(const MG_DEVICE *D) {
  unsigned I;
  if (D->SeenStart || D->Core.Active || D->Core.Suspended ||
      D->Core.SharedCount || D->Core.PmicInterrupt.Saved ||
      D->Core.PmicInterrupt.ContextValid || D->LnaToken[0] || D->LnaToken[1] ||
      D->Core.ConnPinsOwned || D->ConnPinsToken)
    return TRUE;
  for (I=0;I<MG_BANKS;I++) if (D->Core.Owned[I]) return TRUE;
  for (I=0;I<MG_PINS;I++)
    if (D->Core.Pin[I].Saved || D->Core.Pin[I].ContextValid) return TRUE;
  return FALSE;
}
NTSTATUS MgRelease(WDFDEVICE Device,PVOID Context) {
  MG_DEVICE *D=Context; MG_INTERFACE_CONTEXT *Q; unsigned I,Drop=0; NTSTATUS S;
  UNREFERENCED_PARAMETER(Device);
  if (!D || !D->Interfaces) return STATUS_INVALID_PARAMETER;
  Q=D->Interfaces; ExAcquireFastMutex(&Q->Gate);
  MgAcquireConfigurationLock(D); BeginTransition(D);
  /* GpioClx calls CLIENT_ReleaseController after hardware becomes inaccessible.
   * Pin/IRQ restoration belongs in CLIENT_StopController while D0 is valid.
   * Even after a failed/missing stop, detach and unmap without another MMIO.
   * Preserve the complete core journal and block resource reuse if ownership
   * was unresolved; retaining an old mapping cannot extend powered access. */
  if (!Q->SurpriseRemoved && HasOwnedState(D)) D->ReleaseFaulted=TRUE;
  D->Core.Active=0; D->SeenStart=FALSE; Q->Controller=NULL;
  for (I=0;I<2;I++) if (D->LnaToken[I]) {
    D->LnaToken[I]=0; Drop++;
  }
  if (D->ConnPinsToken) { D->ConnPinsToken=0; Drop++; }
  Unmap(D);
  S=D->ReleaseFaulted?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
  MgReleaseConfigurationLock(D); ExReleaseFastMutex(&Q->Gate);
  /* Revoke only the detached lease references. Saved pin state remains above,
   * and a retained query-interface reference continues to protect its object. */
  while (Drop) { Drop--; MgInterfaceDereference(Q); }
  return S;
}
NTSTATUS MgControllerStart(PVOID Context,BOOLEAN Restore,WDF_POWER_DEVICE_STATE Previous) {
  MG_DEVICE *D=Context; NTSTATUS S;
  if (!D || !D->Registers[0] || !D->Registers[1]) return STATUS_DEVICE_NOT_READY;
  MgAcquireConfigurationLock(D);
  if (D->Interfaces->SurpriseRemoved) {
    MgReleaseConfigurationLock(D); return STATUS_DEVICE_REMOVED;
  }
  if (Restore) {
    if (!D->SeenStart || !D->Core.Suspended ||
        (Previous!=WdfPowerDeviceD1 && Previous!=WdfPowerDeviceD2 && Previous!=WdfPowerDeviceD3)) {
      MgReleaseConfigurationLock(D);
      return STATUS_NOT_SUPPORTED;
    }
    S=ReadyMethod(D->Device);
    if (NT_SUCCESS(S)) S=Convert(MgResumeRetained(&D->Core));
    if (NT_SUCCESS(S)) EndTransition(D);
    MgReleaseConfigurationLock(D);
    return S;
  }
  if (D->SeenStart || D->Core.Suspended) {
    MgReleaseConfigurationLock(D); return STATUS_NOT_SUPPORTED;
  }
  BeginTransition(D);
  S=ReadyMethod(D->Device);
  if (NT_SUCCESS(S)) S=Convert(MgStart(&D->Core));
  if (NT_SUCCESS(S)) { D->SeenStart=TRUE; EndTransition(D); }
  MgReleaseConfigurationLock(D);
  return S;
}
NTSTATUS MgControllerStop(PVOID Context,BOOLEAN Save,WDF_POWER_DEVICE_STATE Target) {
  MG_DEVICE *D=Context; NTSTATUS S;
  if (!D) return STATUS_INVALID_PARAMETER;
  MgAcquireConfigurationLock(D);
  if (D->Interfaces->SurpriseRemoved) {
    MgReleaseConfigurationLock(D); return STATUS_SUCCESS;
  }
  if (D->ConnPinsToken) {
    MgReleaseConfigurationLock(D); return STATUS_DEVICE_BUSY;
  }
  if (D->ReleaseFaulted || !D->Registers[0] || !D->Registers[1]) {
    MgReleaseConfigurationLock(D); return STATUS_DEVICE_NOT_READY;
  }
  if (Save && (Target!=WdfPowerDeviceD1 && Target!=WdfPowerDeviceD2 && Target!=WdfPowerDeviceD3)) {
    MgReleaseConfigurationLock(D);
    return STATUS_NOT_SUPPORTED;
  }
  if (D->LnaToken[0] || D->LnaToken[1]) {
    MgReleaseConfigurationLock(D); return STATUS_DEVICE_BUSY;
  }
  BeginTransition(D);
  if (Save) {
    // GpioClx supplies no wake selection here. Save and mask the owned lines.
    S=Convert(MgSuspend(&D->Core));
    if (!D->Core.Suspended && D->Core.Active) EndTransition(D);
  } else {
    S=Convert(MgStop(&D->Core));
    if (NT_SUCCESS(S)) D->SeenStart=FALSE;
  }
  MgReleaseConfigurationLock(D);
  return S;
}
NTSTATUS MgInformation(PVOID Context,PCLIENT_CONTROLLER_BASIC_INFORMATION Info) {
  UNREFERENCED_PARAMETER(Context);
  if (!Info) return STATUS_INVALID_PARAMETER;
  RtlZeroMemory(Info,sizeof(*Info)); Info->Version=GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
  Info->Size=sizeof(*Info); Info->TotalPins=MG_TOTAL_PINS; Info->NumberOfPinsPerBank=32;
  Info->Flags.MemoryMappedController=TRUE; Info->Flags.FormatIoRequestsAsMasks=TRUE;
  Info->Flags.EmulateDebouncing=TRUE; Info->Flags.EmulateActiveBoth=TRUE;
  return STATUS_SUCCESS;
}
static NTSTATUS Pin(unsigned Bank,unsigned Number,unsigned *Global) {
  if (Bank>=MG_BANKS || Number>=32 || Bank*32U+Number>=MG_PINS) return STATUS_INVALID_PARAMETER;
  *Global=Bank*32U+Number; return STATUS_SUCCESS;
}
static NTSTATUS InterruptPin(unsigned Bank,unsigned Number,unsigned *Global) {
  unsigned N;
  if (!Global || Bank>=MG_BANKS || Number>=32) return STATUS_INVALID_PARAMETER;
  N=Bank*32U+Number;
  if (N>=MG_PINS && N!=MG_PMIC_IRQ_PIN) return STATUS_INVALID_PARAMETER;
  *Global=N; return STATUS_SUCCESS;
}
static NTSTATUS Trigger(KINTERRUPT_MODE Mode,KINTERRUPT_POLARITY Polarity,int *Level,int *High) {
  if ((Mode!=LevelSensitive && Mode!=Latched) ||
      (Polarity!=InterruptActiveHigh && Polarity!=InterruptActiveLow)) return STATUS_NOT_SUPPORTED;
  *Level=Mode==LevelSensitive; *High=Polarity==InterruptActiveHigh; return STATUS_SUCCESS;
}
NTSTATUS MgInterruptEnable(PVOID Context,PGPIO_ENABLE_INTERRUPT_PARAMETERS P) {
  MG_DEVICE *D=Context; unsigned N; int Level,High; NTSTATUS S;
  if (!D || !P) return STATUS_INVALID_PARAMETER;
  S=InterruptPin(P->BankId,P->PinNumber,&N); if (!NT_SUCCESS(S)) return S;
  S=Trigger(P->InterruptMode,P->Polarity,&Level,&High); if (!NT_SUCCESS(S)) return S;
  if (P->Flags.AsULONG || P->PullConfiguration>GPIO_PIN_PULL_CONFIGURATION_NONE || P->VendorDataLength)
    return STATUS_NOT_SUPPORTED;
  MgAcquireConfigurationLock(D);
  if (D->Interfaces->SurpriseRemoved) {
    MgReleaseConfigurationLock(D); return STATUS_DEVICE_REMOVED;
  }
  GPIO_CLX_AcquireInterruptLock(D,P->BankId);
  S=Convert(MgEnableEx(&D->Core,N,Level,High,(MG_PULL)P->PullConfiguration));
  GPIO_CLX_ReleaseInterruptLock(D,P->BankId);
  MgReleaseConfigurationLock(D); return S;
}
NTSTATUS MgInterruptDisable(PVOID Context,PGPIO_DISABLE_INTERRUPT_PARAMETERS P) {
  MG_DEVICE *D=Context; unsigned N; NTSTATUS S;
  if (!D || !P) return STATUS_INVALID_PARAMETER;
  S=InterruptPin(P->BankId,P->PinNumber,&N); if (!NT_SUCCESS(S)) return S;
  if (N!=MG_PMIC_IRQ_PIN && N>=MG_IRQ_PINS) return STATUS_INVALID_PARAMETER;
  MgAcquireConfigurationLock(D);
  if (D->Interfaces->SurpriseRemoved) {
    MgReleaseConfigurationLock(D); return STATUS_DEVICE_REMOVED;
  }
  if ((N==MG_PMIC_IRQ_PIN && !D->Core.PmicInterrupt.Saved) ||
      (N!=MG_PMIC_IRQ_PIN && D->Core.Pin[N].Kind!=3)) {
    MgReleaseConfigurationLock(D); return STATUS_INVALID_PARAMETER;
  }
  GPIO_CLX_AcquireInterruptLock(D,P->BankId);
  S=Convert(MgDisableInterrupt(&D->Core,N));
  GPIO_CLX_ReleaseInterruptLock(D,P->BankId);
  MgReleaseConfigurationLock(D); return S;
}
NTSTATUS MgInterruptUnmask(PVOID Context,PGPIO_ENABLE_INTERRUPT_PARAMETERS P) {
  MG_DEVICE *D=Context; unsigned N; NTSTATUS S;
  if (!D || !P) return STATUS_INVALID_PARAMETER;
  S=InterruptPin(P->BankId,P->PinNumber,&N); if (!NT_SUCCESS(S)) return S;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgUnmask(&D->Core,N)); LeaveDirql(D); return S;
}
NTSTATUS MgInterruptMask(PVOID Context,PGPIO_MASK_INTERRUPT_PARAMETERS P) {
  MG_DEVICE *D=Context; NTSTATUS S;
  if (!D || !P) return STATUS_INVALID_PARAMETER;
  P->FailedMask=P->PinMask;
  if (P->PinMask>>32) return STATUS_INVALID_PARAMETER;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgMask(&D->Core,P->BankId,(uint32_t)P->PinMask));
  LeaveDirql(D); if (NT_SUCCESS(S)) P->FailedMask=0; return S;
}
NTSTATUS MgActiveInterrupts(PVOID Context,PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS P) {
  MG_DEVICE *D=Context; uint32_t Active,Enabled; NTSTATUS S;
  if (!D || !P || P->EnabledMask>>32) return STATUS_INVALID_PARAMETER;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgQuery(&D->Core,P->BankId,(uint32_t)P->EnabledMask,&Active,&Enabled));
  LeaveDirql(D); if (NT_SUCCESS(S)) P->ActiveMask=Active; return S;
}
NTSTATUS MgEnabledInterrupts(PVOID Context,PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS P) {
  MG_DEVICE *D=Context; uint32_t Active,Enabled; NTSTATUS S;
  if (!D || !P) return STATUS_INVALID_PARAMETER;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgQuery(&D->Core,P->BankId,MgValidMask(P->BankId),&Active,&Enabled));
  LeaveDirql(D); if (NT_SUCCESS(S)) P->EnabledMask=Enabled; return S;
}
NTSTATUS MgClearInterrupts(PVOID Context,PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS P) {
  MG_DEVICE *D=Context; NTSTATUS S;
  if (!D || !P) return STATUS_INVALID_PARAMETER;
  P->FailedClearMask=P->ClearActiveMask;
  if (P->ClearActiveMask>>32) return STATUS_INVALID_PARAMETER;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgAck(&D->Core,P->BankId,(uint32_t)P->ClearActiveMask));
  LeaveDirql(D); if (NT_SUCCESS(S)) P->FailedClearMask=0; return S;
}
NTSTATUS MgReconfigure(PVOID Context,PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS P) {
  MG_DEVICE *D=Context; unsigned N; int Level,High; NTSTATUS S;
  if (!D || !P) return STATUS_INVALID_PARAMETER;
  S=InterruptPin(P->BankId,P->PinNumber,&N); if (!NT_SUCCESS(S)) return S;
  S=Trigger(P->InterruptMode,P->Polarity,&Level,&High); if (!NT_SUCCESS(S)) return S;
  if (P->Flags.AsULONG) return STATUS_NOT_SUPPORTED;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgConfigureInterrupt(&D->Core,N,Level,High)); LeaveDirql(D); return S;
}
static NTSTATUS IoPins(MG_DEVICE *D,unsigned Bank,const PIN_NUMBER *Pins,ULONG Count,int Connect,int Preserve,int Mode,MG_PULL Pull) {
  ULONG I,J; unsigned N; NTSTATUS S;
  if (!D || !Pins || !Count || Count>32 || Bank>=MG_BANKS) return STATUS_INVALID_PARAMETER;
  MgAcquireConfigurationLock(D);
  if (D->Interfaces->SurpriseRemoved) { S=STATUS_DEVICE_REMOVED; goto Done; }
  /* Reject the entire malformed/duplicate request before touching any pin. */
  for (I=0;I<Count;I++) {
    S=Pin(Bank,Pins[I],&N); if (!NT_SUCCESS(S)) goto Done;
    if (Connect && MgConnPinsOwns(&D->Core,N)) { S=STATUS_DEVICE_BUSY; goto Done; }
    MG_PIN *P=&D->Core.Pin[N];
    if (Connect ? (P->Saved && !(Mode==1 && P->Kind==3 && !P->IoAlias &&
            P->Pull==(unsigned)Pull && (D->Core.Owned[N/32U]&(1U<<(N%32U))))) :
        (P->Kind!=1 && P->Kind!=2 && !(P->Kind==3 && P->IoAlias))) {
      S=STATUS_INVALID_PARAMETER; goto Done;
    }
    for (J=0;J<I;J++) if (Pins[I]==Pins[J]) { S=STATUS_INVALID_PARAMETER; goto Done; }
  }
  GPIO_CLX_AcquireInterruptLock(D,(BANK_ID)Bank);
  for (I=0;I<Count;I++) {
    N=Bank*32U+Pins[I];
    S=Convert(Connect?MgConnectEx(&D->Core,N,Mode,Pull):MgDisconnectInputOutput(&D->Core,N,Preserve));
    if (!NT_SUCCESS(S)) break;
  }
  if (!NT_SUCCESS(S) && Connect) {
    /* Include a partly configured current pin if its snapshot was taken. */
    for (J=0;J<=I && J<Count;J++) {
      N=Bank*32U+Pins[J];
      if (D->Core.Pin[N].Saved) {
        if (!D->Core.Pin[N].Kind) (void)MgDisconnect(&D->Core,N,0);
        else if (J<I || D->Core.Pin[N].Kind!=3 || D->Core.Pin[N].IoAlias)
          (void)MgDisconnectInputOutput(&D->Core,N,0);
      }
    }
  }
  GPIO_CLX_ReleaseInterruptLock(D,(BANK_ID)Bank);
Done:
  MgReleaseConfigurationLock(D); return S;
}
NTSTATUS MgConnectIo(PVOID Context,PGPIO_CONNECT_IO_PINS_PARAMETERS P) {
  if (!P) return STATUS_INVALID_PARAMETER;
  if ((P->ConnectMode!=ConnectModeInput && P->ConnectMode!=ConnectModeOutput) ||
      P->PullConfiguration>GPIO_PIN_PULL_CONFIGURATION_NONE ||
      P->DebounceTimeout || P->VendorDataLength || P->ConnectFlags.AsULONG) return STATUS_NOT_SUPPORTED;
  if (P->DriveStrength) return STATUS_NOT_SUPPORTED;
  return IoPins(Context,P->BankId,P->PinNumberTable,P->PinCount,1,0,P->ConnectMode==ConnectModeOutput?2:1,(MG_PULL)P->PullConfiguration);
}
NTSTATUS MgDisconnectIo(PVOID Context,PGPIO_DISCONNECT_IO_PINS_PARAMETERS P) {
  if (!P || (P->DisconnectFlags.AsULONG&~1U)) return STATUS_INVALID_PARAMETER;
  return IoPins(Context,P->BankId,P->PinNumberTable,P->PinCount,0,P->DisconnectFlags.PreserveConfiguration,0,MgPullDefault);
}
NTSTATUS MgReadPins(PVOID Context,PGPIO_READ_PINS_MASK_PARAMETERS P) {
  MG_DEVICE *D=Context; uint32_t Value; NTSTATUS S;
  if (!D || !P || !P->PinValues || P->Flags.Reserved0) return STATUS_INVALID_PARAMETER;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgRead(&D->Core,P->BankId,P->Flags.WriteConfiguredPins,&Value));
  LeaveDirql(D); if (NT_SUCCESS(S)) *P->PinValues=Value; return S;
}
NTSTATUS MgWritePins(PVOID Context,PGPIO_WRITE_PINS_MASK_PARAMETERS P) {
  MG_DEVICE *D=Context;
  NTSTATUS S;
  if (!D || !P || P->Flags.AsULONG || ((P->SetMask|P->ClearMask)>>32)) return STATUS_INVALID_PARAMETER;
  if (!EnterDirql(D)) return STATUS_DEVICE_NOT_READY;
  S=Convert(MgWrite(&D->Core,P->BankId,(uint32_t)P->SetMask,(uint32_t)P->ClearMask));
  LeaveDirql(D); return S;
}
NTSTATUS MgDeviceAdd(WDFDRIVER Driver,PWDFDEVICE_INIT Init) {
  WDF_OBJECT_ATTRIBUTES A; WDFDEVICE Device; NTSTATUS S;
  MG_INTERFACE_CONTEXT *Q; MTK_GPS_LNA_INTERFACE Interface={0}; WDF_QUERY_INTERFACE_CONFIG Query;
  MTK_CONN_PINS_INTERFACE ConnPins={0};
  UCHAR Minor=IRP_MN_SURPRISE_REMOVAL;
  S=GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver,Init,&A); if (!NT_SUCCESS(S)) return S;
  S=WdfDeviceInitAssignWdmIrpPreprocessCallback(Init,MgPnpPreprocess,IRP_MJ_PNP,&Minor,1);
  if (!NT_SUCCESS(S)) return S;
  S=WdfDeviceCreate(&Init,&A,&Device); if (!NT_SUCCESS(S)) return S;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,MG_INTERFACE_CONTEXT);
  S=WdfObjectAllocateContext(Device,&A,(PVOID *)&Q); if (!NT_SUCCESS(S)) return S;
  Q->Device=Device; ExInitializeFastMutex(&Q->Gate); KeInitializeSpinLock(&Q->ReferenceLock);
  S=GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver,Device); if (!NT_SUCCESS(S)) return S;
  Interface.Header.Size=sizeof(Interface); Interface.Header.Version=MTK_GPS_LNA_INTERFACE_VERSION;
  Interface.Header.Context=Q; Interface.Header.InterfaceReference=MgInterfaceReference;
  Interface.Header.InterfaceDereference=MgInterfaceDereference;
  Interface.Acquire=MgLnaAcquire; Interface.Set=MgLnaSet; Interface.Release=MgLnaRelease;
  WDF_QUERY_INTERFACE_CONFIG_INIT(&Query,(PINTERFACE)&Interface,&GUID_MTK_GPS_LNA_INTERFACE_V1,NULL);
  S=WdfDeviceAddQueryInterface(Device,&Query); if (!NT_SUCCESS(S)) return S;
  ConnPins.Header.Size=sizeof(ConnPins); ConnPins.Header.Version=MTK_CONN_PINS_INTERFACE_VERSION;
  ConnPins.Header.Context=Q; ConnPins.Header.InterfaceReference=MgInterfaceReference;
  ConnPins.Header.InterfaceDereference=MgInterfaceDereference;
  ConnPins.Acquire=MgConnAcquire; ConnPins.Read=MgConnRead;
  ConnPins.Write=MgConnWrite; ConnPins.Release=MgConnRelease;
  WDF_QUERY_INTERFACE_CONFIG_INIT(&Query,(PINTERFACE)&ConnPins,&GUID_MTK_CONN_PINS_INTERFACE_V1,NULL);
  S=WdfDeviceAddQueryInterface(Device,&Query); if (!NT_SUCCESS(S)) return S;
  return WdfDeviceCreateDeviceInterface(Device,&GUID_DEVINTERFACE_MTK_CONN_PINS,NULL);
}
VOID MgDriverUnload(WDFDRIVER Driver) { (void)GPIO_CLX_UnregisterClient(Driver); }
NTSTATUS DriverEntry(PDRIVER_OBJECT Object,PUNICODE_STRING Path) {
  WDF_DRIVER_CONFIG Config; WDFDRIVER Driver; GPIO_CLIENT_REGISTRATION_PACKET R; NTSTATUS S;
  WDF_DRIVER_CONFIG_INIT(&Config,MgDeviceAdd); Config.EvtDriverUnload=MgDriverUnload;
  S=WdfDriverCreate(Object,Path,WDF_NO_OBJECT_ATTRIBUTES,&Config,&Driver); if (!NT_SUCCESS(S)) return S;
  RtlZeroMemory(&R,sizeof(R)); R.Version=GPIO_CLIENT_VERSION; R.Size=sizeof(R); R.ControllerContextSize=sizeof(MG_DEVICE);
  R.CLIENT_PrepareController=MgPrepare; R.CLIENT_ReleaseController=MgRelease;
  R.CLIENT_StartController=MgControllerStart; R.CLIENT_StopController=MgControllerStop;
  R.CLIENT_QueryControllerBasicInformation=MgInformation;
  R.CLIENT_EnableInterrupt=MgInterruptEnable; R.CLIENT_DisableInterrupt=MgInterruptDisable;
  R.CLIENT_MaskInterrupts=MgInterruptMask; R.CLIENT_UnmaskInterrupt=MgInterruptUnmask;
  R.CLIENT_QueryActiveInterrupts=MgActiveInterrupts; R.CLIENT_QueryEnabledInterrupts=MgEnabledInterrupts;
  R.CLIENT_ClearActiveInterrupts=MgClearInterrupts; R.CLIENT_ReconfigureInterrupt=MgReconfigure;
  R.CLIENT_ConnectIoPins=MgConnectIo; R.CLIENT_DisconnectIoPins=MgDisconnectIo;
  R.CLIENT_ReadGpioPinsUsingMask=MgReadPins; R.CLIENT_WriteGpioPinsUsingMask=MgWritePins;
  return GPIO_CLX_RegisterClient(Driver,&R,Path);
}
