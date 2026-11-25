/* SPDX-License-Identifier: GPL-2.0-only */
/* WDM DMA adapter setup and lifetime for the T70 DPMAIF bus-master device. */
#include "DpmaifWdmDma.h"
#include "DpmaifSgCore.h"

#define DMA_STATE_CLOSE UINT32_C(0x40000000)
#define DMA_STATE_RELEASE UINT32_C(0x80000000)
#define DMA_STATE_COUNT UINT32_C(0x3fffffff)

static NTSTATUS
DmaAcquire (
  T70_DPMAIF_WDM_DMA *Dma
  )
{
  LONG State, Desired;

  if (Dma == NULL || !Dma->Ready) return STATUS_DELETE_PENDING;
  for (;;) {
    State=InterlockedCompareExchange(&Dma->Closing,0,0);
    if (((ULONG)State & (DMA_STATE_CLOSE|DMA_STATE_RELEASE)) != 0U)
      return STATUS_DELETE_PENDING;
    if (((ULONG)State & DMA_STATE_COUNT) == DMA_STATE_COUNT)
      return STATUS_INSUFFICIENT_RESOURCES;
    Desired=State+1;
    if (InterlockedCompareExchange(&Dma->Closing,Desired,State)==State)
      return STATUS_SUCCESS;
  }
}

static VOID
DmaReleaseReference (
  T70_DPMAIF_WDM_DMA *Dma
  )
{
  LONG State, Desired;
  for (;;) {
    State=InterlockedCompareExchange(&Dma->Closing,0,0);
    if (((ULONG)State & DMA_STATE_COUNT)==0U) return;
    Desired=State-1;
    if (InterlockedCompareExchange(&Dma->Closing,Desired,State)==State) return;
  }
}

NTSTATUS
T70DpmaifWdmDmaInitialize (
  T70_DPMAIF_WDM_DMA *Dma,
  PDEVICE_OBJECT PhysicalDeviceObject,
  ULONG MaximumTransferBytes
  )
{
  DEVICE_DESCRIPTION Description;
  DMA_ADAPTER_INFO AdapterInfo;
  NTSTATUS Status;
  ULONG MapRegisterCount = 0U;
  PDMA_ADAPTER Adapter;

  if (Dma == NULL || PhysicalDeviceObject == NULL ||
      MaximumTransferBytes == 0U) {
    return STATUS_INVALID_PARAMETER;
  }
  if (Dma->Ready || Dma->Adapter != NULL) {
    return STATUS_DEVICE_BUSY;
  }
  RtlZeroMemory(Dma, sizeof(*Dma));
  RtlZeroMemory(&Description, sizeof(Description));
  Description.Version = DEVICE_DESCRIPTION_VERSION3;
  Description.Master = TRUE;
  Description.ScatterGather = TRUE;
  Description.Dma64BitAddresses = TRUE;
  Description.DmaAddressWidth = T70_DPMAIF_DMA_MASK_BITS;
  Description.MaximumLength = MaximumTransferBytes;

  Adapter = IoGetDmaAdapter(PhysicalDeviceObject, &Description,
      &MapRegisterCount);
  if (Adapter == NULL) {
    return STATUS_NOT_SUPPORTED;
  }
  if (Adapter->DmaOperations == NULL ||
      Adapter->DmaOperations->PutDmaAdapter == NULL) {
    return STATUS_NOT_SUPPORTED;
  }
  if (Adapter->DmaOperations->GetDmaAdapterInfo == NULL ||
      Adapter->DmaOperations->AllocateCommonBuffer == NULL ||
      Adapter->DmaOperations->FreeCommonBuffer == NULL ||
      Adapter->DmaOperations->GetScatterGatherList == NULL ||
      Adapter->DmaOperations->PutScatterGatherList == NULL) {
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
    return STATUS_NOT_SUPPORTED;
  }

  RtlZeroMemory(&AdapterInfo, sizeof(AdapterInfo));
  AdapterInfo.Version = DMA_ADAPTER_INFO_VERSION1;
  Status = Adapter->DmaOperations->GetDmaAdapterInfo(Adapter, &AdapterInfo);
  if (!NT_SUCCESS(Status) ||
      AdapterInfo.V1.DmaAddressWidth < T70_DPMAIF_DMA_MASK_BITS) {
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
    return NT_SUCCESS(Status) ? STATUS_NOT_SUPPORTED : Status;
  }

  Dma->Adapter = Adapter;
  Dma->PhysicalDeviceObject = PhysicalDeviceObject;
  Dma->MapRegisterCount = MapRegisterCount;
  Dma->MaximumTransferBytes = MaximumTransferBytes;
  Dma->Ready = TRUE;
  return STATUS_SUCCESS;
}

NTSTATUS
T70DpmaifWdmMappingInitialize (
  T70_DPMAIF_WDM_MAPPING *Mapping
  )
{
  LONG State;

  if (Mapping == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  State = InterlockedCompareExchange(&Mapping->State, 0, 0);
  if (State != T70DpmaifMappingEmpty &&
      State != T70DpmaifMappingReleased &&
      State != T70DpmaifMappingFailed) {
    return STATUS_DEVICE_BUSY;
  }
  RtlZeroMemory(Mapping, sizeof(*Mapping));
  return STATUS_SUCCESS;
}

NTSTATUS
T70DpmaifWdmDmaRelease (
  T70_DPMAIF_WDM_DMA *Dma
  )
{
  PDMA_ADAPTER Adapter;
  LONG State, Desired;

  if (Dma == NULL) return STATUS_INVALID_PARAMETER;
  if (!Dma->Ready) return STATUS_SUCCESS;
  for (;;) {
    State=InterlockedCompareExchange(&Dma->Closing,0,0);
    if (((ULONG)State & DMA_STATE_RELEASE)!=0U) return STATUS_DEVICE_BUSY;
    if (((ULONG)State & DMA_STATE_CLOSE)==0U) {
      Desired=(LONG)((ULONG)State|DMA_STATE_CLOSE);
      if (InterlockedCompareExchange(&Dma->Closing,Desired,State)!=State) continue;
      State=Desired;
    }
    if (((ULONG)State & DMA_STATE_COUNT)!=0U) return STATUS_DEVICE_BUSY;
    Desired=(LONG)DMA_STATE_RELEASE;
    if (InterlockedCompareExchange(&Dma->Closing,Desired,State)==State) break;
  }

  Adapter=Dma->Adapter;
  if (Adapter==NULL || Adapter->DmaOperations==NULL ||
      Adapter->DmaOperations->PutDmaAdapter==NULL)
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  Adapter->DmaOperations->PutDmaAdapter(Adapter);
  RtlZeroMemory(Dma,sizeof(*Dma));
  return STATUS_SUCCESS;
}

NTSTATUS
T70DpmaifWdmCommonBufferInitialize (
  T70_DPMAIF_COMMON_BUFFER *Buffer
  )
{
  if (Buffer == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  if (Buffer->Owner != NULL || Buffer->VirtualAddress != NULL) {
    return STATUS_DEVICE_BUSY;
  }
  RtlZeroMemory(Buffer, sizeof(*Buffer));
  return STATUS_SUCCESS;
}

NTSTATUS
T70DpmaifWdmAllocateCommonBuffer (
  T70_DPMAIF_WDM_DMA *Dma,
  ULONG Length,
  ULONG Alignment,
  BOOLEAN CacheEnabled,
  T70_DPMAIF_COMMON_BUFFER *Buffer
  )
{
  PHYSICAL_ADDRESS LogicalAddress;
  PVOID VirtualAddress;
  ULONG_PTR AlignmentMask;
  NTSTATUS Status;

  if (Dma == NULL || Buffer == NULL || Length == 0U || Alignment == 0U ||
      (Alignment & (Alignment - 1U)) != 0U) {
    return STATUS_INVALID_PARAMETER;
  }
  if (Buffer->Owner != NULL || Buffer->VirtualAddress != NULL ||
      Buffer->Length != 0U) {
    return STATUS_DEVICE_BUSY;
  }
  Status = DmaAcquire(Dma);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }
  LogicalAddress.QuadPart = 0;
  VirtualAddress = Dma->Adapter->DmaOperations->AllocateCommonBuffer(
      Dma->Adapter, Length, &LogicalAddress, CacheEnabled);
  if (VirtualAddress == NULL) {
    DmaReleaseReference(Dma);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  AlignmentMask = (ULONG_PTR)Alignment - 1U;
  if (!T70DpmaifDmaRangeValid((uint64_t)LogicalAddress.QuadPart, Length) ||
      (((ULONG_PTR)VirtualAddress & AlignmentMask) != 0U) ||
      (((ULONG_PTR)LogicalAddress.QuadPart & AlignmentMask) != 0U)) {
    Dma->Adapter->DmaOperations->FreeCommonBuffer(Dma->Adapter, Length,
        LogicalAddress, VirtualAddress, CacheEnabled);
    DmaReleaseReference(Dma);
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  Buffer->Owner = Dma;
  Buffer->VirtualAddress = VirtualAddress;
  Buffer->LogicalAddress = LogicalAddress;
  Buffer->Length = Length;
  Buffer->CacheEnabled = CacheEnabled;
  return STATUS_SUCCESS;
}

NTSTATUS
T70DpmaifWdmFreeCommonBuffer (
  T70_DPMAIF_WDM_DMA *Dma,
  T70_DPMAIF_COMMON_BUFFER *Buffer
  )
{
  if (Dma == NULL || Buffer == NULL || Buffer->Owner != Dma ||
      Buffer->VirtualAddress == NULL || Buffer->Length == 0U ||
      Dma->Adapter == NULL || Dma->Adapter->DmaOperations == NULL ||
      Dma->Adapter->DmaOperations->FreeCommonBuffer == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  Dma->Adapter->DmaOperations->FreeCommonBuffer(Dma->Adapter, Buffer->Length,
      Buffer->LogicalAddress, Buffer->VirtualAddress, Buffer->CacheEnabled);
  RtlZeroMemory(Buffer, sizeof(*Buffer));
  DmaReleaseReference(Dma);
  return STATUS_SUCCESS;
}

static VOID
DmaListComplete (
  PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  PSCATTER_GATHER_LIST ScatterGather,
  PVOID Context
  )
{
  T70_DPMAIF_WDM_MAPPING *Mapping = (T70_DPMAIF_WDM_MAPPING *)Context;
  LONG Previous;
  T70_DPMAIF_WDM_DMA *Dma;

  if (Mapping == NULL) {
    return;
  }
  Previous = InterlockedCompareExchange(&Mapping->State,
      T70DpmaifMappingCompleting, T70DpmaifMappingPending);
  if (Previous != T70DpmaifMappingPending) {
    return;
  }
  if (ScatterGather == NULL) {
    PDRIVER_LIST_CONTROL Completion=Mapping->Completion;
    PVOID UserContext=Mapping->UserContext;
    Dma=Mapping->Owner;
    Mapping->Owner=NULL; Mapping->Completion=NULL; Mapping->UserContext=NULL;
    InterlockedExchange(&Mapping->State,T70DpmaifMappingFailed);
    if (Dma!=NULL) DmaReleaseReference(Dma);
    if (Completion!=NULL) Completion(DeviceObject,Irp,NULL,UserContext);
    return;
  }

  Mapping->ScatterGather = ScatterGather;
  InterlockedExchange(&Mapping->State, T70DpmaifMappingReady);
  Mapping->Completion(DeviceObject, Irp, ScatterGather, Mapping->UserContext);
}

NTSTATUS
T70DpmaifWdmGetScatterGatherList (
  T70_DPMAIF_WDM_DMA *Dma,
  PDEVICE_OBJECT TargetDeviceObject,
  PMDL Mdl,
  PVOID CurrentVa,
  ULONG Length,
  PDRIVER_LIST_CONTROL Completion,
  PVOID UserContext,
  BOOLEAN WriteToDevice,
  T70_DPMAIF_WDM_MAPPING *Mapping
  )
{
  NTSTATUS Status;

  if (Dma == NULL || TargetDeviceObject == NULL || Mdl == NULL ||
      CurrentVa == NULL || Length == 0U || Completion == NULL ||
      Mapping == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  Status = DmaAcquire(Dma);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }
  if (Length > Dma->MaximumTransferBytes) {
    DmaReleaseReference(Dma);
    return STATUS_INVALID_BUFFER_SIZE;
  }
  if (InterlockedCompareExchange(&Mapping->State, T70DpmaifMappingPending,
        T70DpmaifMappingEmpty) != T70DpmaifMappingEmpty) {
    DmaReleaseReference(Dma);
    return STATUS_DEVICE_BUSY;
  }

  Mapping->Owner = Dma;
  Mapping->TargetDeviceObject = TargetDeviceObject;
  Mapping->ScatterGather = NULL;
  Mapping->Completion = Completion;
  Mapping->UserContext = UserContext;
  Mapping->WriteToDevice = WriteToDevice;
  InterlockedExchange(&Mapping->State, T70DpmaifMappingPending);

  Status = Dma->Adapter->DmaOperations->GetScatterGatherList(
      Dma->Adapter, TargetDeviceObject, Mdl, CurrentVa, Length,
      DmaListComplete, Mapping, WriteToDevice);
  if (!NT_SUCCESS(Status)) {
    if (InterlockedCompareExchange(&Mapping->State, T70DpmaifMappingFailed,
          T70DpmaifMappingPending) == T70DpmaifMappingPending) {
      Mapping->Owner = NULL;
      DmaReleaseReference(Dma);
    }
  }
  return Status;
}

NTSTATUS
T70DpmaifWdmPutScatterGatherList (
  T70_DPMAIF_WDM_MAPPING *Mapping
  )
{
  T70_DPMAIF_WDM_DMA *Dma;
  PSCATTER_GATHER_LIST ScatterGather;
  KIRQL OldIrql,CurrentIrql;

  if (Mapping == NULL ||
      InterlockedCompareExchange(&Mapping->State, 0, 0) !=
        T70DpmaifMappingReady) {
    return STATUS_INVALID_DEVICE_STATE;
  }
  if (InterlockedCompareExchange(&Mapping->State, T70DpmaifMappingReleasing,
        T70DpmaifMappingReady) != T70DpmaifMappingReady) {
    return STATUS_DEVICE_BUSY;
  }
  Dma = Mapping->Owner;
  ScatterGather = Mapping->ScatterGather;
  if (Dma == NULL || ScatterGather == NULL || Dma->Adapter == NULL ||
      Dma->Adapter->DmaOperations == NULL ||
      Dma->Adapter->DmaOperations->PutScatterGatherList == NULL) {
    InterlockedExchange(&Mapping->State, T70DpmaifMappingReady);
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  CurrentIrql=KeGetCurrentIrql();
  if (CurrentIrql>DISPATCH_LEVEL) {
    InterlockedExchange(&Mapping->State,T70DpmaifMappingReady);
    return STATUS_INVALID_DEVICE_STATE;
  }
  if (CurrentIrql<DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL,&OldIrql);
  Dma->Adapter->DmaOperations->PutScatterGatherList(Dma->Adapter,
      ScatterGather,Mapping->WriteToDevice);
  if (CurrentIrql<DISPATCH_LEVEL) KeLowerIrql(OldIrql);
  Mapping->Owner = NULL;
  Mapping->ScatterGather = NULL;
  InterlockedExchange(&Mapping->State, T70DpmaifMappingReleased);
  DmaReleaseReference(Dma);
  return STATUS_SUCCESS;
}

T70_DPMAIF_RING_STATUS
T70DpmaifWdmNormalizeScatterGatherList (
  PSCATTER_GATHER_LIST ScatterGather,
  uint32_t ExpectedBytes,
  T70_DPMAIF_TX_SEGMENT *Segments,
  uint32_t Capacity,
  uint32_t *NormalizedCount
  )
{
  uint64_t TotalBytes = 0U;
  uint32_t Index;

  if (ScatterGather == NULL || Segments == NULL || NormalizedCount == NULL ||
      ScatterGather->NumberOfElements == 0U ||
      ScatterGather->NumberOfElements > Capacity || ExpectedBytes == 0U ||
      ExpectedBytes > UINT16_MAX) {
    return ScatterGather != NULL &&
        ScatterGather->NumberOfElements > Capacity ? T70DpmaifRingNoSpace :
        T70DpmaifRingInvalid;
  }
  for (Index = 0U; Index < ScatterGather->NumberOfElements; ++Index) {
    const SCATTER_GATHER_ELEMENT *Element = &ScatterGather->Elements[Index];
    uint64_t Address = (uint64_t)Element->Address.QuadPart;
    if (Element->Length == 0U || Element->Length > UINT16_MAX ||
        !T70DpmaifDmaRangeValid(Address, Element->Length)) {
      return T70DpmaifRingInvalid;
    }
    TotalBytes += Element->Length;
    if (TotalBytes > ExpectedBytes) {
      return T70DpmaifRingInvalid;
    }
  }
  if (TotalBytes != ExpectedBytes) {
    return T70DpmaifRingInvalid;
  }

  for (Index = 0U; Index < ScatterGather->NumberOfElements; ++Index) {
    const SCATTER_GATHER_ELEMENT *Element = &ScatterGather->Elements[Index];
    Segments[Index].DmaAddress = (uint64_t)Element->Address.QuadPart;
    Segments[Index].Length = Element->Length;
  }
  return T70DpmaifNormalizeSgSegments(Segments,
      ScatterGather->NumberOfElements, ExpectedBytes, Capacity,
      NormalizedCount);
}

static BOOLEAN
DmaHasOperation (
  const DMA_OPERATIONS *Operations,
  size_t Offset,
  size_t Size
  )
{
  return Operations != NULL && Operations->Size >= Offset + Size;
}

NTSTATUS
T70DpmaifWdmAllocateCommonBufferVector (
  T70_DPMAIF_WDM_DMA *Dma,
  ULONG NumberOfElements,
  ULONG ElementBytes,
  ULONG ElementAlignment,
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Vector
  )
{
  PDMA_OPERATIONS Operations;
  PDMA_COMMON_BUFFER_VECTOR NativeVector = NULL;
  PHYSICAL_ADDRESS LowAddress, HighAddress;
  NTSTATUS Status;

  if (Dma == NULL || Vector == NULL || NumberOfElements == 0U ||
      ElementBytes == 0U || ElementAlignment == 0U ||
      (ElementAlignment & (ElementAlignment - 1U)) != 0U ||
      (uint64_t)NumberOfElements >
          UINT64_MAX / (uint64_t)ElementBytes) {
    return STATUS_INVALID_PARAMETER;
  }
  if (Vector->Owner != NULL || Vector->Vector != NULL || Vector->Ready) {
    return STATUS_DEVICE_BUSY;
  }
  Status = DmaAcquire(Dma);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }
  Operations = Dma->Adapter->DmaOperations;
  if (!DmaHasOperation(Operations,
          FIELD_OFFSET(DMA_OPERATIONS, AllocateCommonBufferVector),
          sizeof(Operations->AllocateCommonBufferVector)) ||
      Operations->AllocateCommonBufferVector == NULL ||
      !DmaHasOperation(Operations,
          FIELD_OFFSET(DMA_OPERATIONS, GetCommonBufferFromVectorByIndex),
          sizeof(Operations->GetCommonBufferFromVectorByIndex)) ||
      Operations->GetCommonBufferFromVectorByIndex == NULL ||
      !DmaHasOperation(Operations,
          FIELD_OFFSET(DMA_OPERATIONS, FreeCommonBufferVector),
          sizeof(Operations->FreeCommonBufferVector)) ||
      Operations->FreeCommonBufferVector == NULL) {
    DmaReleaseReference(Dma);
    return STATUS_NOT_SUPPORTED;
  }

  LowAddress.QuadPart = 0U;
  HighAddress.QuadPart = (LONGLONG)(T70_DPMAIF_DMA_MASK_LIMIT - 1U);
  Status = Operations->AllocateCommonBufferVector(Dma->Adapter,
      LowAddress, HighAddress, MmWriteCombined, MM_ANY_NODE_OK, 0U,
      NumberOfElements, ElementBytes, &NativeVector);
  if (!NT_SUCCESS(Status) || NativeVector == NULL) {
    DmaReleaseReference(Dma);
    return NT_SUCCESS(Status) ? STATUS_INSUFFICIENT_RESOURCES : Status;
  }

  Vector->Owner = Dma;
  Vector->Vector = NativeVector;
  Vector->NumberOfElements = NumberOfElements;
  Vector->ElementBytes = ElementBytes;
  Vector->ElementAlignment = ElementAlignment;
  Vector->Ready = TRUE;
  return STATUS_SUCCESS;
}

NTSTATUS
T70DpmaifWdmGetCommonBufferVectorElement (
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Vector,
  ULONG Index,
  PVOID *VirtualAddress,
  PHYSICAL_ADDRESS *LogicalAddress
  )
{
  T70_DPMAIF_WDM_DMA *Dma;
  PDMA_OPERATIONS Operations;
  PVOID LocalVirtual = NULL;
  PHYSICAL_ADDRESS LocalLogical;

  if (Vector == NULL || !Vector->Ready || Vector->Owner == NULL ||
      Vector->Vector == NULL || Index >= Vector->NumberOfElements ||
      VirtualAddress == NULL || LogicalAddress == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  Dma = Vector->Owner;
  if (!Dma->Ready || Dma->Adapter == NULL ||
      Dma->Adapter->DmaOperations == NULL) {
    return STATUS_DELETE_PENDING;
  }
  Operations = Dma->Adapter->DmaOperations;
  if (!DmaHasOperation(Operations,
          FIELD_OFFSET(DMA_OPERATIONS, GetCommonBufferFromVectorByIndex),
          sizeof(Operations->GetCommonBufferFromVectorByIndex)) ||
      Operations->GetCommonBufferFromVectorByIndex == NULL) {
    return STATUS_NOT_SUPPORTED;
  }
  LocalLogical.QuadPart = 0;
  Operations->GetCommonBufferFromVectorByIndex(Dma->Adapter,Vector->Vector,
      Index,&LocalVirtual,&LocalLogical);
  if (LocalVirtual == NULL || LocalLogical.QuadPart < 0 ||
      (((uint64_t)LocalLogical.QuadPart &
        (Vector->ElementAlignment - 1U)) != 0U) ||
      !T70DpmaifDmaRangeValid((uint64_t)LocalLogical.QuadPart,
          Vector->ElementBytes)) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  *VirtualAddress = LocalVirtual;
  *LogicalAddress = LocalLogical;
  return STATUS_SUCCESS;
}

NTSTATUS
T70DpmaifWdmFreeCommonBufferVector (
  T70_DPMAIF_WDM_DMA *Dma,
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Vector
  )
{
  PDMA_OPERATIONS Operations;
  if (Dma == NULL || Vector == NULL || Vector->Owner != Dma ||
      Vector->Vector == NULL || !Vector->Ready ||
      Dma->Adapter == NULL || Dma->Adapter->DmaOperations == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  Operations = Dma->Adapter->DmaOperations;
  if (!DmaHasOperation(Operations,
          FIELD_OFFSET(DMA_OPERATIONS, FreeCommonBufferVector),
          sizeof(Operations->FreeCommonBufferVector)) ||
      Operations->FreeCommonBufferVector == NULL) {
    return STATUS_NOT_SUPPORTED;
  }
  Operations->FreeCommonBufferVector(Dma->Adapter,Vector->Vector);
  RtlZeroMemory(Vector,sizeof(*Vector));
  DmaReleaseReference(Dma);
  return STATUS_SUCCESS;
}
