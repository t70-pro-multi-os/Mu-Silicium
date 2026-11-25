/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ndis.h>
#include <ntddndis.h>
#include "DpmaifResources.h"
#include "DpmaifLayout.h"
#include "DpmaifBatCore.h"
#include "DpmaifWdmDma.h"
#include "NdisCallbackCore.h"
#include <Library/MtkCcciMemoryHandoff.h>

#define T70_WWAN_TAG 0x57373054U
#define T70_WWAN_MAX_IP_PACKET 65535U
#define T70_WWAN_MTU 1500U
#define T70_WWAN_MMIO_COUNT 4U
#define T70_WWAN_MEMORY_COUNT 5U
#define T70_WWAN_DMA_INTERRUPT_RESOURCES 6U
#define T70_WWAN_REQUEST_BAT_BUFFER_BYTES 3584U
#define T70_WWAN_FRAGMENT_BAT_BUFFER_BYTES 128U
#define T70_WWAN_BAT_BUFFER_ALIGNMENT 128U

/* T70 DT omits dl_bat_entry_size, so the v3 kernel default applies: 16384.
 * The stock driver doubles PIT entries and uses four 2048-entry UL DRBs. */
static const T70_DPMAIF_QUEUE_COUNTS gDpmaifQueueCounts = {
  32768U, 16384U, 16384U, {2048U, 2048U, 2048U, 2048U}, 1U
};

static NDIS_HANDLE gMiniportDriverHandle;
static const CHAR gVendorDescription[] = "T70Pro MediaTek WWAN port";
static const NDIS_OID gSupportedOids[] = {
  OID_GEN_SUPPORTED_LIST,
  OID_GEN_HARDWARE_STATUS,
  OID_GEN_MEDIA_SUPPORTED,
  OID_GEN_MEDIA_IN_USE,
  OID_GEN_MAXIMUM_LOOKAHEAD,
  OID_GEN_MAXIMUM_FRAME_SIZE,
  OID_GEN_LINK_SPEED,
  OID_GEN_TRANSMIT_BUFFER_SPACE,
  OID_GEN_RECEIVE_BUFFER_SPACE,
  OID_GEN_TRANSMIT_BLOCK_SIZE,
  OID_GEN_RECEIVE_BLOCK_SIZE,
  OID_GEN_VENDOR_ID,
  OID_GEN_VENDOR_DESCRIPTION,
  OID_GEN_CURRENT_PACKET_FILTER,
  OID_GEN_CURRENT_LOOKAHEAD,
  OID_GEN_MAXIMUM_TOTAL_SIZE,
  OID_GEN_MAC_OPTIONS,
  OID_GEN_MEDIA_CONNECT_STATUS,
  OID_GEN_PHYSICAL_MEDIUM,
  OID_GEN_DRIVER_VERSION
};

typedef struct {
  NDIS_HANDLE NdisHandle;
  PDEVICE_OBJECT Pdo;
  PVOID Mmio[T70_WWAN_MMIO_COUNT];
  NDIS_PHYSICAL_ADDRESS MmioPhysical[T70_WWAN_MMIO_COUNT];
  ULONG MmioLength[T70_WWAN_MMIO_COUNT];
  PVOID CcciHandoff;
  PHYSICAL_ADDRESS CcciHandoffPhysical;
  ULONG CcciHandoffLength;
  T70_CCCI_MEMORY_HANDOFF ModemMemory;
  BOOLEAN ModemMemoryValid;
  T70_DPMAIF_RESOURCE_MAP Resources;
  T70_DPMAIF_WDM_DMA Dma;
  T70_DPMAIF_COMMON_BUFFER PitMemory;
  T70_DPMAIF_COMMON_BUFFER RequestBatMemory;
  T70_DPMAIF_COMMON_BUFFER FragmentBatMemory;
  T70_DPMAIF_COMMON_BUFFER TxDrbMemory[T70_DPMAIF_HW_TX_QUEUES];
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR RequestBatBuffers;
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR FragmentBatBuffers;
  T70_DPMAIF_BAT_CORE RequestBatCore;
  T70_DPMAIF_BAT_CORE FragmentBatCore;
  T70_DPMAIF_BAT_SLOT *RequestBatSlots;
  T70_DPMAIF_BAT_SLOT *FragmentBatSlots;
  T70_DPMAIF_QUEUE_LAYOUT QueueLayout;
  T70_DPMAIF_HW_OP StartupPlan[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t StartupPlanCount;
  BOOLEAN StartupPlanBuilt;
  NDIS_SPIN_LOCK OidLock;
  ULONG PacketFilter;
  ULONG Lookahead;
  BOOLEAN LockReady;
  BOOLEAN Running;
} T70_WWAN_ADAPTER;

static NDIS_STATUS
WwReadResources (
  PNDIS_RESOURCE_LIST Raw,
  PCM_RESOURCE_LIST Translated,
  T70_DPMAIF_RESOURCE_MAP *Map,
  PHYSICAL_ADDRESS TranslatedMemory[T70_WWAN_MEMORY_COUNT]
  )
{
  T70_DPMAIF_RESOURCE Resources[T70_WWAN_DMA_INTERRUPT_RESOURCES];
  PCM_PARTIAL_RESOURCE_LIST TranslatedPartial;
  ULONG Index;
  T70_DPMAIF_RESOURCE_STATUS MapStatus;

  if (Raw == NULL || Translated == NULL || Map == NULL ||
      Translated->Count != 1U || Raw->Count != T70_WWAN_DMA_INTERRUPT_RESOURCES) {
    return NDIS_STATUS_FAILURE;
  }
  TranslatedPartial = &Translated->List[0].PartialResourceList;
  if (TranslatedPartial->Count != Raw->Count) {
    return NDIS_STATUS_FAILURE;
  }
  RtlZeroMemory(Resources, sizeof(Resources));
  RtlZeroMemory(TranslatedMemory,
      sizeof(PHYSICAL_ADDRESS) * T70_WWAN_MEMORY_COUNT);

  for (Index = 0U; Index < Raw->Count; ++Index) {
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawDescriptor =
        &Raw->PartialDescriptors[Index];
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *TranslatedDescriptor =
        &TranslatedPartial->PartialDescriptors[Index];
    T70_DPMAIF_RESOURCE *Resource = &Resources[Index];

    if (RawDescriptor->Type != TranslatedDescriptor->Type ||
        RawDescriptor->ShareDisposition !=
            TranslatedDescriptor->ShareDisposition) {
      return NDIS_STATUS_FAILURE;
    }
    if (RawDescriptor->Type == CmResourceTypeMemory) {
      ULONG Slot;
      uint64_t RawBase = (uint64_t)RawDescriptor->u.Memory.Start.QuadPart;

      if (RawDescriptor->u.Memory.Length !=
          TranslatedDescriptor->u.Memory.Length) {
        return NDIS_STATUS_FAILURE;
      }
      Resource->Type = T70DpmaifResourceMemory;
      Resource->Start = RawBase;
      Resource->Length = RawDescriptor->u.Memory.Length;
      Resource->Shared = (uint8_t)(RawDescriptor->ShareDisposition !=
          CmResourceShareDeviceExclusive);
      /* Four slots are source DPMAIF reg[0..3]. Slot four is the reserved
       * 64-byte MD1 handoff data exposed by ACPI, not a device MMIO window. */
      if (RawBase == UINT64_C(0x10014000)) Slot = 0U;
      else if (RawBase == UINT64_C(0x1022D000)) Slot = 1U;
      else if (RawBase == UINT64_C(0x1022C000)) Slot = 2U;
      else if (RawBase == UINT64_C(0x1022E000)) Slot = 3U;
      else if (RawBase == T70_CCCI_MEMORY_HANDOFF_ADDRESS) Slot = 4U;
      else return NDIS_STATUS_FAILURE;
      if (TranslatedMemory[Slot].QuadPart != 0) {
        return NDIS_STATUS_FAILURE;
      }
      TranslatedMemory[Slot] = TranslatedDescriptor->u.Memory.Start;
    } else if (RawDescriptor->Type == CmResourceTypeInterrupt) {
      Resource->Type = T70DpmaifResourceInterrupt;
      Resource->Start = RawDescriptor->u.Interrupt.Vector;
      Resource->Length = 0U;
      /* ACPI HANA6797 is verified level/high/exclusive. CM_RESOURCE flags
       * encode edge as LATCHED; zero therefore means level-triggered. */
      Resource->LevelTriggered = (uint8_t)(
          (RawDescriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED) == 0U);
      Resource->ActiveHigh = 1U;
      Resource->Shared = (uint8_t)(RawDescriptor->ShareDisposition !=
          CmResourceShareDeviceExclusive);
    } else {
      return NDIS_STATUS_FAILURE;
    }
  }

  MapStatus = T70DpmaifMapResources(Resources,
      T70_WWAN_DMA_INTERRUPT_RESOURCES, Map);
  if (MapStatus != T70DpmaifResourceSuccess) {
    return NDIS_STATUS_FAILURE;
  }
  for (Index = 0U; Index < T70_WWAN_MEMORY_COUNT; ++Index) {
    if (TranslatedMemory[Index].QuadPart < 0 ||
        (uint64_t)TranslatedMemory[Index].QuadPart == 0U) {
      return NDIS_STATUS_FAILURE;
    }
  }
  return NDIS_STATUS_SUCCESS;
}

static VOID
WwUnmapResources (
  T70_WWAN_ADAPTER *Adapter
  )
{
  ULONG Index;
  for (Index = 0U; Index < T70_WWAN_MMIO_COUNT; ++Index) {
    if (Adapter->Mmio[Index] != NULL) {
      NdisMUnmapIoSpace(Adapter->NdisHandle, Adapter->Mmio[Index],
          Adapter->MmioLength[Index]);
      Adapter->Mmio[Index] = NULL;
    }
  }
  if (Adapter->CcciHandoff != NULL) {
    NdisMUnmapIoSpace(Adapter->NdisHandle, Adapter->CcciHandoff,
        Adapter->CcciHandoffLength);
    Adapter->CcciHandoff = NULL;
  }
}

static NDIS_STATUS
WwMapResources (
  T70_WWAN_ADAPTER *Adapter,
  PNDIS_RESOURCE_LIST Raw,
  PCM_RESOURCE_LIST Translated
  )
{
  PHYSICAL_ADDRESS TranslatedMemory[T70_WWAN_MEMORY_COUNT];
  NDIS_STATUS Status;
  ULONG Index;

  Status = WwReadResources(Raw, Translated, &Adapter->Resources,
      TranslatedMemory);
  if (Status != NDIS_STATUS_SUCCESS) {
    return Status;
  }
  for (Index = 0U; Index < T70_WWAN_MMIO_COUNT; ++Index) {
    Adapter->MmioPhysical[Index] = TranslatedMemory[Index];
    Adapter->MmioLength[Index] = 0x1000U;
    Status = NdisMMapIoSpace(&Adapter->Mmio[Index], Adapter->NdisHandle,
        Adapter->MmioPhysical[Index], Adapter->MmioLength[Index]);
    if (Status != NDIS_STATUS_SUCCESS) {
      WwUnmapResources(Adapter);
      return Status;
    }
  }
  Adapter->CcciHandoffPhysical = TranslatedMemory[4];
  Adapter->CcciHandoffLength = T70_CCCI_MEMORY_HANDOFF_BYTES;
  Status = NdisMMapIoSpace(&Adapter->CcciHandoff, Adapter->NdisHandle,
      Adapter->CcciHandoffPhysical, Adapter->CcciHandoffLength);
  if (Status != NDIS_STATUS_SUCCESS) {
    WwUnmapResources(Adapter);
    return Status;
  }
  T70WwCopyFromDeviceMemory(&Adapter->ModemMemory,
      (const volatile uint8_t *)Adapter->CcciHandoff,
      sizeof(Adapter->ModemMemory));
  Adapter->ModemMemoryValid =
      (BOOLEAN)T70CcciMemoryHandoffIsValid(&Adapter->ModemMemory);
  return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
WwInitializeBatPool (
  T70_WWAN_ADAPTER *Adapter,
  ULONG EntryCount,
  ULONG BufferBytes,
  UCHAR BufferType,
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Buffers,
  T70_DPMAIF_BAT_CORE *Core,
  T70_DPMAIF_BAT_SLOT **Slots,
  PVOID DescriptorTable,
  ULONG DescriptorTableBytes
  )
{
  ULONG BufferCount;
  ULONG Index;
  SIZE_T SlotBytes;
  SIZE_T AddressBytes;
  uint64_t *Addresses = NULL;
  PVOID BufferVa;
  PHYSICAL_ADDRESS LogicalAddress;
  uint32_t PostedCount = 0U;
  T70_DPMAIF_RING_STATUS RingStatus;
  NTSTATUS Status;

  if (Adapter == NULL || Buffers == NULL || Core == NULL || Slots == NULL ||
      DescriptorTable == NULL || EntryCount < 2U || BufferBytes == 0U ||
      EntryCount > MAXULONG / sizeof(**Slots)) {
    return NDIS_STATUS_INVALID_DATA;
  }
  BufferCount = EntryCount - 1U;
  SlotBytes = (SIZE_T)EntryCount * sizeof(**Slots);
  AddressBytes = (SIZE_T)BufferCount * sizeof(*Addresses);
  *Slots = (T70_DPMAIF_BAT_SLOT *)NdisAllocateMemoryWithTagPriority(
      Adapter->NdisHandle, SlotBytes, T70_WWAN_TAG, NormalPoolPriority);
  if (*Slots == NULL) {
    return NDIS_STATUS_RESOURCES;
  }
  NdisZeroMemory(*Slots, SlotBytes);
  Addresses = (uint64_t *)NdisAllocateMemoryWithTagPriority(
      Adapter->NdisHandle, AddressBytes, T70_WWAN_TAG, NormalPoolPriority);
  if (Addresses == NULL) {
    return NDIS_STATUS_RESOURCES;
  }

  Status = T70DpmaifWdmAllocateCommonBufferVector(&Adapter->Dma,
      BufferCount, BufferBytes, T70_WWAN_BAT_BUFFER_ALIGNMENT, Buffers);
  if (!NT_SUCCESS(Status)) {
    goto Exit;
  }
  for (Index = 0U; Index < BufferCount; ++Index) {
    Status = T70DpmaifWdmGetCommonBufferVectorElement(Buffers, Index,
        &BufferVa, &LogicalAddress);
    if (!NT_SUCCESS(Status)) {
      goto Exit;
    }
    Addresses[Index] = (uint64_t)LogicalAddress.QuadPart;
  }
  RingStatus = T70DpmaifBatPostInitialBuffers(Core, *Slots, EntryCount,
      Addresses, BufferCount, BufferBytes, T70_WWAN_BAT_BUFFER_ALIGNMENT,
      (uint8_t *)DescriptorTable, DescriptorTableBytes, &PostedCount);
  if (RingStatus != T70DpmaifRingSuccess || PostedCount != BufferCount) {
    Status = STATUS_DEVICE_CONFIGURATION_ERROR;
    goto Exit;
  }
  Core->BufferType = BufferType;
  Status = STATUS_SUCCESS;

Exit:
  if (Addresses != NULL) {
    NdisFreeMemory(Addresses, 0U, 0U);
  }
  return (NDIS_STATUS)Status;
}

static NDIS_STATUS
WwInitializeRxBatPools (
  T70_WWAN_ADAPTER *Adapter
  )
{
  T70_DPMAIF_QUEUE_MEMORY_SIZES Sizes;
  NDIS_STATUS Status;

  if (Adapter == NULL ||
      T70DpmaifQueueMemorySizes(&gDpmaifQueueCounts, &Sizes) !=
          T70DpmaifRingSuccess ||
      !gDpmaifQueueCounts.FragmentEnabled) {
    return NDIS_STATUS_FAILURE;
  }
  Status = WwInitializeBatPool(Adapter, gDpmaifQueueCounts.RequestBatEntries,
      T70_WWAN_REQUEST_BAT_BUFFER_BYTES, T70_DPMAIF_BUFFER_TYPE_REQUEST,
      &Adapter->RequestBatBuffers,
      &Adapter->RequestBatCore, &Adapter->RequestBatSlots,
      Adapter->RequestBatMemory.VirtualAddress, Sizes.RequestBatBytes);
  if (Status != NDIS_STATUS_SUCCESS) {
    return Status;
  }
  Status = WwInitializeBatPool(Adapter, gDpmaifQueueCounts.FragmentBatEntries,
      T70_WWAN_FRAGMENT_BAT_BUFFER_BYTES, T70_DPMAIF_BUFFER_TYPE_FRAGMENT,
      &Adapter->FragmentBatBuffers,
      &Adapter->FragmentBatCore, &Adapter->FragmentBatSlots,
      Adapter->FragmentBatMemory.VirtualAddress, Sizes.FragmentBatBytes);
  return Status;
}

static NDIS_STATUS
WwInitializeQueueMemory (
  T70_WWAN_ADAPTER *Adapter
  )
{
  T70_DPMAIF_QUEUE_MEMORY_SIZES Sizes;
  T70_DPMAIF_QUEUE_ADDRESSES Addresses;
  NTSTATUS Status;
  ULONG Index;

  if (Adapter == NULL ||
      T70DpmaifQueueMemorySizes(&gDpmaifQueueCounts,&Sizes) !=
          T70DpmaifRingSuccess) {
    return NDIS_STATUS_FAILURE;
  }
  NdisZeroMemory(&Addresses,sizeof(Addresses));
  Status = T70DpmaifWdmAllocateCommonBuffer(&Adapter->Dma,
      Sizes.PitBytes,64U,FALSE,&Adapter->PitMemory);
  if (!NT_SUCCESS(Status)) return (NDIS_STATUS)Status;
  Status = T70DpmaifWdmAllocateCommonBuffer(&Adapter->Dma,
      Sizes.RequestBatBytes,64U,FALSE,&Adapter->RequestBatMemory);
  if (!NT_SUCCESS(Status)) return (NDIS_STATUS)Status;
  if (Sizes.FragmentBatBytes != 0U) {
    Status = T70DpmaifWdmAllocateCommonBuffer(&Adapter->Dma,
        Sizes.FragmentBatBytes,64U,FALSE,&Adapter->FragmentBatMemory);
    if (!NT_SUCCESS(Status)) return (NDIS_STATUS)Status;
  }
  for (Index=0U;Index<T70_DPMAIF_HW_TX_QUEUES;Index++) {
    Status = T70DpmaifWdmAllocateCommonBuffer(&Adapter->Dma,
        Sizes.TxDrbBytes[Index],64U,FALSE,&Adapter->TxDrbMemory[Index]);
    if (!NT_SUCCESS(Status)) return (NDIS_STATUS)Status;
    Addresses.TxDrbAddress[Index] =
        (uint64_t)Adapter->TxDrbMemory[Index].LogicalAddress.QuadPart;
  }

  Addresses.PitAddress =
      (uint64_t)Adapter->PitMemory.LogicalAddress.QuadPart;
  Addresses.RequestBatAddress =
      (uint64_t)Adapter->RequestBatMemory.LogicalAddress.QuadPart;
  if (Sizes.FragmentBatBytes != 0U) {
    Addresses.FragmentBatAddress =
        (uint64_t)Adapter->FragmentBatMemory.LogicalAddress.QuadPart;
  }
  if (T70DpmaifBuildQueueLayout(&gDpmaifQueueCounts,&Addresses,
          &Adapter->QueueLayout) != T70DpmaifRingSuccess) {
    return NDIS_STATUS_FAILURE;
  }

  T70WwZeroDeviceMemory((volatile uint8_t *)Adapter->PitMemory.VirtualAddress,
      Sizes.PitBytes);
  T70WwZeroDeviceMemory((volatile uint8_t *)Adapter->RequestBatMemory.VirtualAddress,
      Sizes.RequestBatBytes);
  if (Sizes.FragmentBatBytes != 0U) {
    T70WwZeroDeviceMemory((volatile uint8_t *)Adapter->FragmentBatMemory.VirtualAddress,
        Sizes.FragmentBatBytes);
  }
  for (Index=0U;Index<T70_DPMAIF_HW_TX_QUEUES;Index++) {
    T70WwZeroDeviceMemory((volatile uint8_t *)Adapter->TxDrbMemory[Index].VirtualAddress,
        Sizes.TxDrbBytes[Index]);
  }
  return NDIS_STATUS_SUCCESS;
}

static VOID
WwReleaseAdapter (
  T70_WWAN_ADAPTER *Adapter
  )
{
  NTSTATUS DmaStatus;
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Vectors[2];
  T70_DPMAIF_COMMON_BUFFER *Buffers[3U + T70_DPMAIF_HW_TX_QUEUES];
  BOOLEAN Failed = FALSE;
  ULONG Index;
  if (Adapter == NULL) return;

  Vectors[0] = &Adapter->RequestBatBuffers;
  Vectors[1] = &Adapter->FragmentBatBuffers;
  for (Index = 0U; Index < RTL_NUMBER_OF(Vectors); ++Index) {
    if (Vectors[Index]->Owner != NULL) {
      DmaStatus = T70DpmaifWdmFreeCommonBufferVector(&Adapter->Dma,
          Vectors[Index]);
      if (!NT_SUCCESS(DmaStatus)) {
        DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL,
            "MtkWwan: unable to release DPMAIF RX buffer vector %lu: %08lx\\n",
            Index, DmaStatus);
        Failed = TRUE;
      }
    }
  }
  // Vector buffers refer to these slot records until the vector is released.
  if (Adapter->RequestBatBuffers.Owner == NULL && Adapter->RequestBatSlots != NULL) {
    NdisFreeMemory(Adapter->RequestBatSlots, 0U, 0U);
    Adapter->RequestBatSlots = NULL;
  }
  if (Adapter->FragmentBatBuffers.Owner == NULL && Adapter->FragmentBatSlots != NULL) {
    NdisFreeMemory(Adapter->FragmentBatSlots, 0U, 0U);
    Adapter->FragmentBatSlots = NULL;
  }
  Buffers[0] = &Adapter->PitMemory;
  Buffers[1] = &Adapter->RequestBatMemory;
  Buffers[2] = &Adapter->FragmentBatMemory;
  for (Index = 0U; Index < T70_DPMAIF_HW_TX_QUEUES; ++Index)
    Buffers[3U + Index] = &Adapter->TxDrbMemory[Index];
  for (Index = 0U; Index < RTL_NUMBER_OF(Buffers); ++Index) {
    if (Buffers[Index]->Owner != NULL) {
      DmaStatus = T70DpmaifWdmFreeCommonBuffer(&Adapter->Dma,Buffers[Index]);
      if (!NT_SUCCESS(DmaStatus)) {
        DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL,
            "MtkWwan: unable to release DPMAIF table buffer %lu: %08lx\\n",
            Index, DmaStatus);
        Failed = TRUE;
      }
    }
  }
  if (Adapter->Dma.Ready) {
    DmaStatus = T70DpmaifWdmDmaRelease(&Adapter->Dma);
    if (!NT_SUCCESS(DmaStatus)) {
      DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL,
          "MtkWwan: DMA objects remain during adapter teardown: %08lx\\n",
          DmaStatus);
      Failed = TRUE;
    }
  }
  if (Failed) return;
  WwUnmapResources(Adapter);
  if (Adapter->LockReady) {
    NdisFreeSpinLock(&Adapter->OidLock);
    Adapter->LockReady = FALSE;
  }
  NdisFreeMemory(Adapter, 0U, 0U);
}

static NDIS_STATUS
WwInitialize (
  NDIS_HANDLE NdisMiniportHandle,
  NDIS_HANDLE MiniportDriverContext,
  PNDIS_MINIPORT_INIT_PARAMETERS InitParameters
  )
{
  T70_WWAN_ADAPTER *Adapter = NULL;
  NDIS_MINIPORT_ADAPTER_ATTRIBUTES Attributes;
  PDEVICE_OBJECT Pdo = NULL;
  PCM_RESOURCE_LIST RawResources = NULL;
  PCM_RESOURCE_LIST Translated = NULL;
  NDIS_STATUS Status;
  UNREFERENCED_PARAMETER(MiniportDriverContext);

  if (InitParameters == NULL || InitParameters->AllocatedResources == NULL) {
    return NDIS_STATUS_FAILURE;
  }
  Adapter = (T70_WWAN_ADAPTER *)NdisAllocateMemoryWithTagPriority(
      NdisMiniportHandle, sizeof(*Adapter), T70_WWAN_TAG, NormalPoolPriority);
  if (Adapter == NULL) {
    return NDIS_STATUS_RESOURCES;
  }
  NdisZeroMemory(Adapter, sizeof(*Adapter));
  Adapter->NdisHandle = NdisMiniportHandle;
  NdisAllocateSpinLock(&Adapter->OidLock);
  Adapter->LockReady = TRUE;
  Adapter->Lookahead = 0U;

  NdisZeroMemory(&Attributes, sizeof(Attributes));
  Attributes.RegistrationAttributes.Header.Type =
      NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
  Attributes.RegistrationAttributes.Header.Revision =
      NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
  Attributes.RegistrationAttributes.Header.Size =
      NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
  Attributes.RegistrationAttributes.MiniportAdapterContext = Adapter;
  Attributes.RegistrationAttributes.AttributeFlags =
      NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE |
      NDIS_MINIPORT_ATTRIBUTES_NDIS_WDM |
      NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER;
  Attributes.RegistrationAttributes.CheckForHangTimeInSeconds = 0U;
  Attributes.RegistrationAttributes.InterfaceType = NdisInterfaceInternal;
  Status = NdisMSetMiniportAttributes(NdisMiniportHandle, &Attributes);
  if (Status != NDIS_STATUS_SUCCESS) {
    WwReleaseAdapter(Adapter);
    return Status;
  }

  NdisZeroMemory(&Attributes, sizeof(Attributes));
  Attributes.GeneralAttributes.Header.Type =
      NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
  Attributes.GeneralAttributes.Header.Revision =
      NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
  Attributes.GeneralAttributes.Header.Size =
      NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
  Attributes.GeneralAttributes.MediaType = NdisMediumWirelessWan;
  Attributes.GeneralAttributes.PhysicalMediumType = NdisPhysicalMediumWirelessWan;
  Attributes.GeneralAttributes.MtuSize = T70_WWAN_MTU;
  Attributes.GeneralAttributes.MaxXmitLinkSpeed = 0U;
  Attributes.GeneralAttributes.XmitLinkSpeed = 0U;
  Attributes.GeneralAttributes.MaxRcvLinkSpeed = 0U;
  Attributes.GeneralAttributes.RcvLinkSpeed = 0U;
  Attributes.GeneralAttributes.MediaConnectState = MediaConnectStateDisconnected;
  Attributes.GeneralAttributes.MediaDuplexState = MediaDuplexStateUnknown;
  Attributes.GeneralAttributes.LookaheadSize = 0U;
  Attributes.GeneralAttributes.MacOptions = NDIS_MAC_OPTION_NO_LOOPBACK;
  Attributes.GeneralAttributes.SupportedPacketFilters = T70_WWAN_PACKET_FILTER_SUPPORTED;
  Attributes.GeneralAttributes.MaxMulticastListSize = 0U;
  Attributes.GeneralAttributes.MacAddressLength = 0U;
  Attributes.GeneralAttributes.AccessType = NET_IF_ACCESS_POINT_TO_POINT;
  Attributes.GeneralAttributes.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
  Attributes.GeneralAttributes.ConnectionType = NET_IF_CONNECTION_DEDICATED;
  Attributes.GeneralAttributes.IfType = IF_TYPE_WWANPP;
  Attributes.GeneralAttributes.IfConnectorPresent = FALSE;
  Attributes.GeneralAttributes.SupportedStatistics = 0U;
  Attributes.GeneralAttributes.SupportedOidList = (PNDIS_OID)gSupportedOids;
  Attributes.GeneralAttributes.SupportedOidListLength = sizeof(gSupportedOids);
  Status = NdisMSetMiniportAttributes(NdisMiniportHandle, &Attributes);
  if (Status != NDIS_STATUS_SUCCESS) {
    WwReleaseAdapter(Adapter);
    return Status;
  }

  NdisMGetDeviceProperty(NdisMiniportHandle, &Pdo, NULL, NULL,
      &RawResources, &Translated);
  if (Pdo == NULL || RawResources == NULL || Translated == NULL ||
      RawResources->Count != 1U || RawResources->List[0].PartialResourceList.Count == 0U) {
    WwReleaseAdapter(Adapter);
    return NDIS_STATUS_FAILURE;
  }
  Adapter->Pdo = Pdo;
  Status = WwMapResources(Adapter,
      (PNDIS_RESOURCE_LIST)&RawResources->List[0].PartialResourceList,
      Translated);
  if (Status != NDIS_STATUS_SUCCESS) {
    WwReleaseAdapter(Adapter);
    return Status;
  }
  Status = (NDIS_STATUS)T70DpmaifWdmDmaInitialize(&Adapter->Dma, Pdo,
      T70_WWAN_MAX_IP_PACKET);
  if (Status != NDIS_STATUS_SUCCESS) {
    WwReleaseAdapter(Adapter);
    return Status;
  }
  Status = WwInitializeQueueMemory(Adapter);
  if (Status != NDIS_STATUS_SUCCESS) {
    WwReleaseAdapter(Adapter);
    return Status;
  }
  if (Adapter->ModemMemoryValid &&
      (Adapter->ModemMemory.Flags &
       (T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK |
        T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED)) ==
          (T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK |
           T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED)) {
    T70_DPMAIF_MODEM_MEMORY Memory;
    Memory.ModemMemoryBase = Adapter->ModemMemory.ModemBase;
    Memory.ModemMemoryBytes = Adapter->ModemMemory.ModemBytes;
    Status = WwInitializeRxBatPools(Adapter);
    if (Status != NDIS_STATUS_SUCCESS) {
      WwReleaseAdapter(Adapter);
      return Status;
    }
    Adapter->QueueLayout.Queues.RequestBatPostedEntries =
        Adapter->RequestBatCore.WriteIndex;
    Adapter->QueueLayout.Queues.FragmentBatPostedEntries =
        Adapter->FragmentBatCore.WriteIndex;
    if (T70DpmaifBuildStartupPlan(&Memory, &Adapter->QueueLayout.Queues,
            Adapter->StartupPlan, T70_DPMAIF_HW_MAX_PLAN_OPS,
            &Adapter->StartupPlanCount) != T70DpmaifRingSuccess) {
      WwReleaseAdapter(Adapter);
      return NDIS_STATUS_FAILURE;
    }
    Adapter->StartupPlanBuilt = TRUE;
  }
  return NDIS_STATUS_SUCCESS;
}

static VOID
WwHalt (
  NDIS_HANDLE MiniportAdapterContext,
  NDIS_HALT_ACTION HaltAction
  )
{
  UNREFERENCED_PARAMETER(HaltAction);
  WwReleaseAdapter((T70_WWAN_ADAPTER *)MiniportAdapterContext);
}

static NDIS_STATUS
WwPause (
  NDIS_HANDLE MiniportAdapterContext,
  PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters
  )
{
  T70_WWAN_ADAPTER *Adapter = (T70_WWAN_ADAPTER *)MiniportAdapterContext;
  UNREFERENCED_PARAMETER(PauseParameters);
  NdisAcquireSpinLock(&Adapter->OidLock);
  Adapter->Running = FALSE;
  NdisReleaseSpinLock(&Adapter->OidLock);
  return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
WwRestart (
  NDIS_HANDLE MiniportAdapterContext,
  PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters
  )
{
  T70_WWAN_ADAPTER *Adapter = (T70_WWAN_ADAPTER *)MiniportAdapterContext;
  UNREFERENCED_PARAMETER(RestartParameters);
  NdisAcquireSpinLock(&Adapter->OidLock);
  Adapter->Running = TRUE;
  NdisReleaseSpinLock(&Adapter->OidLock);
  return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
WwCopyQuery (
  PNDIS_OID_REQUEST OidRequest,
  const VOID *Value,
  ULONG ValueBytes
  )
{
  if (OidRequest->DATA.QUERY_INFORMATION.InformationBuffer == NULL ||
      OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < ValueBytes) {
    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = ValueBytes;
    return NDIS_STATUS_BUFFER_TOO_SHORT;
  }
  RtlCopyMemory(OidRequest->DATA.QUERY_INFORMATION.InformationBuffer,
      Value, ValueBytes);
  OidRequest->DATA.QUERY_INFORMATION.BytesWritten = ValueBytes;
  return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
WwQueryOid (
  T70_WWAN_ADAPTER *Adapter,
  PNDIS_OID_REQUEST OidRequest,
  NDIS_OID Oid
  )
{
  ULONG UlongValue = 0U;
  USHORT UshortValue;
  NDIS_MEDIUM Medium = NdisMediumWirelessWan;
  NDIS_PHYSICAL_MEDIUM PhysicalMedium = NdisPhysicalMediumWirelessWan;
  NDIS_HARDWARE_STATUS HardwareStatus = NdisHardwareStatusNotReady;
  NDIS_MEDIA_STATE MediaState = NdisMediaStateDisconnected;
  UCHAR Vendor[sizeof(gVendorDescription)];

  switch (Oid) {
  case OID_GEN_SUPPORTED_LIST:
    return WwCopyQuery(OidRequest, gSupportedOids, sizeof(gSupportedOids));
  case OID_GEN_HARDWARE_STATUS:
    return WwCopyQuery(OidRequest, &HardwareStatus, sizeof(HardwareStatus));
  case OID_GEN_MEDIA_SUPPORTED:
  case OID_GEN_MEDIA_IN_USE:
    return WwCopyQuery(OidRequest, &Medium, sizeof(Medium));
  case OID_GEN_MAXIMUM_LOOKAHEAD:
  case OID_GEN_MAXIMUM_FRAME_SIZE:
    UlongValue = (Oid == OID_GEN_MAXIMUM_FRAME_SIZE) ? T70_WWAN_MTU : 0U;
    return WwCopyQuery(OidRequest, &UlongValue, sizeof(UlongValue));
  case OID_GEN_LINK_SPEED:
  case OID_GEN_TRANSMIT_BUFFER_SPACE:
  case OID_GEN_RECEIVE_BUFFER_SPACE:
  case OID_GEN_TRANSMIT_BLOCK_SIZE:
  case OID_GEN_RECEIVE_BLOCK_SIZE:
  case OID_GEN_VENDOR_ID:
    return WwCopyQuery(OidRequest, &UlongValue, sizeof(UlongValue));
  case OID_GEN_VENDOR_DESCRIPTION:
    RtlCopyMemory(Vendor, gVendorDescription, sizeof(Vendor));
    return WwCopyQuery(OidRequest, Vendor, sizeof(Vendor));
  case OID_GEN_CURRENT_PACKET_FILTER:
    NdisAcquireSpinLock(&Adapter->OidLock);
    UlongValue = Adapter->PacketFilter;
    NdisReleaseSpinLock(&Adapter->OidLock);
    return WwCopyQuery(OidRequest, &UlongValue, sizeof(UlongValue));
  case OID_GEN_CURRENT_LOOKAHEAD:
    NdisAcquireSpinLock(&Adapter->OidLock);
    UlongValue = Adapter->Lookahead;
    NdisReleaseSpinLock(&Adapter->OidLock);
    return WwCopyQuery(OidRequest, &UlongValue, sizeof(UlongValue));
  case OID_GEN_MAXIMUM_TOTAL_SIZE:
    UlongValue = T70_WWAN_MTU;
    return WwCopyQuery(OidRequest, &UlongValue, sizeof(UlongValue));
  case OID_GEN_MAC_OPTIONS:
    UlongValue = NDIS_MAC_OPTION_NO_LOOPBACK;
    return WwCopyQuery(OidRequest, &UlongValue, sizeof(UlongValue));
  case OID_GEN_MEDIA_CONNECT_STATUS:
    return WwCopyQuery(OidRequest, &MediaState, sizeof(MediaState));
  case OID_GEN_PHYSICAL_MEDIUM:
    return WwCopyQuery(OidRequest, &PhysicalMedium, sizeof(PhysicalMedium));
  case OID_GEN_DRIVER_VERSION:
    UshortValue = (USHORT)((NDIS_MINIPORT_MAJOR_VERSION << 8) |
        NDIS_MINIPORT_MINOR_VERSION);
    return WwCopyQuery(OidRequest, &UshortValue, sizeof(UshortValue));
  case OID_WWAN_DRIVER_CAPS:
    /* The driver model is not implemented end-to-end. Do not qualify this
     * adapter for the MB Service until its control and packet paths exist. */
    return NDIS_STATUS_NOT_SUPPORTED;
  case OID_WWAN_DEVICE_CAPS:
    /* Do not claim device capabilities until the CCCI control path can read
     * them from the modem. The MB service will ignore this adapter for now. */
    return NDIS_STATUS_NOT_SUPPORTED;
  default:
    return NDIS_STATUS_NOT_SUPPORTED;
  }
}

static NDIS_STATUS
WwSetOid (
  T70_WWAN_ADAPTER *Adapter,
  PNDIS_OID_REQUEST OidRequest,
  NDIS_OID Oid
  )
{
  ULONG Value;
  uint32_t PacketFilter;
  if (Oid != OID_GEN_CURRENT_PACKET_FILTER &&
      Oid != OID_GEN_CURRENT_LOOKAHEAD) {
    return NDIS_STATUS_NOT_SUPPORTED;
  }
  if (OidRequest->DATA.SET_INFORMATION.InformationBuffer == NULL ||
      OidRequest->DATA.SET_INFORMATION.InformationBufferLength < sizeof(Value)) {
    OidRequest->DATA.SET_INFORMATION.BytesNeeded = sizeof(Value);
    return NDIS_STATUS_INVALID_LENGTH;
  }
  RtlCopyMemory(&Value, OidRequest->DATA.SET_INFORMATION.InformationBuffer,
      sizeof(Value));
  switch (Oid) {
  case OID_GEN_CURRENT_PACKET_FILTER:
    if (!T70WwPacketFilterStore(Value, &PacketFilter)) {
      return NDIS_STATUS_NOT_SUPPORTED;
    }
    NdisAcquireSpinLock(&Adapter->OidLock);
    Adapter->PacketFilter = (ULONG)PacketFilter;
    NdisReleaseSpinLock(&Adapter->OidLock);
    OidRequest->DATA.SET_INFORMATION.BytesRead = sizeof(Value);
    return NDIS_STATUS_SUCCESS;
  case OID_GEN_CURRENT_LOOKAHEAD:
    if (Value != 0U) {
      return NDIS_STATUS_INVALID_DATA;
    }
    NdisAcquireSpinLock(&Adapter->OidLock);
    Adapter->Lookahead = Value;
    NdisReleaseSpinLock(&Adapter->OidLock);
    OidRequest->DATA.SET_INFORMATION.BytesRead = sizeof(Value);
    return NDIS_STATUS_SUCCESS;
  default:
    return NDIS_STATUS_NOT_SUPPORTED;
  }
}

static NDIS_STATUS
WwOidRequest (
  NDIS_HANDLE MiniportAdapterContext,
  PNDIS_OID_REQUEST OidRequest
  )
{
  T70_WWAN_ADAPTER *Adapter = (T70_WWAN_ADAPTER *)MiniportAdapterContext;
  NDIS_STATUS Status;
  if (Adapter == NULL || OidRequest == NULL) {
    return NDIS_STATUS_INVALID_DATA;
  }
  switch (OidRequest->RequestType) {
  case NdisRequestQueryInformation:
  case NdisRequestQueryStatistics:
    OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0U;
    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = 0U;
    Status = WwQueryOid(Adapter, OidRequest,
        OidRequest->DATA.QUERY_INFORMATION.Oid);
    return Status;
  case NdisRequestSetInformation:
    OidRequest->DATA.SET_INFORMATION.BytesRead = 0U;
    OidRequest->DATA.SET_INFORMATION.BytesNeeded = 0U;
    return WwSetOid(Adapter, OidRequest,
        OidRequest->DATA.SET_INFORMATION.Oid);
  default:
    return NDIS_STATUS_NOT_SUPPORTED;
  }
}

static VOID
WwSendNetBufferLists (
  NDIS_HANDLE MiniportAdapterContext,
  PNET_BUFFER_LIST NetBufferLists,
  NDIS_PORT_NUMBER PortNumber,
  ULONG SendFlags
  )
{
  T70_WWAN_ADAPTER *Adapter =
      (T70_WWAN_ADAPTER *)MiniportAdapterContext;
  PNET_BUFFER_LIST Current;
  ULONG CompleteFlags;
  UNREFERENCED_PARAMETER(PortNumber);
  if (Adapter == NULL || NetBufferLists == NULL) return;
  for (Current = NetBufferLists; Current != NULL;
       Current = NET_BUFFER_LIST_NEXT_NBL(Current)) {
    NET_BUFFER_LIST_STATUS(Current) = NDIS_STATUS_MEDIA_DISCONNECTED;
  }
  CompleteFlags = T70WwSendCompleteFlags(
      NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags),
      NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL);
  NdisMSendNetBufferListsComplete(Adapter->NdisHandle, NetBufferLists,
      CompleteFlags);
}

static VOID
WwReturnNetBufferLists (
  NDIS_HANDLE MiniportAdapterContext,
  PNET_BUFFER_LIST NetBufferLists,
  ULONG ReturnFlags
  )
{
  UNREFERENCED_PARAMETER(MiniportAdapterContext);
  UNREFERENCED_PARAMETER(NetBufferLists);
  UNREFERENCED_PARAMETER(ReturnFlags);
}

static VOID
WwCancelSend (
  NDIS_HANDLE MiniportAdapterContext,
  PVOID CancelId
  )
{
  UNREFERENCED_PARAMETER(MiniportAdapterContext);
  UNREFERENCED_PARAMETER(CancelId);
}

static BOOLEAN
WwCheckForHang (
  NDIS_HANDLE MiniportAdapterContext
  )
{
  UNREFERENCED_PARAMETER(MiniportAdapterContext);
  return FALSE;
}

static NDIS_STATUS
WwReset (
  NDIS_HANDLE MiniportAdapterContext,
  PBOOLEAN AddressingReset
  )
{
  UNREFERENCED_PARAMETER(MiniportAdapterContext);
  if (AddressingReset != NULL) {
    *AddressingReset = FALSE;
  }
  return NDIS_STATUS_SUCCESS;
}

static VOID
WwDevicePnPEventNotify (
  NDIS_HANDLE MiniportAdapterContext,
  PNET_DEVICE_PNP_EVENT NetDevicePnPEvent
  )
{
  UNREFERENCED_PARAMETER(MiniportAdapterContext);
  UNREFERENCED_PARAMETER(NetDevicePnPEvent);
}

static VOID
WwShutdown (
  NDIS_HANDLE MiniportAdapterContext,
  NDIS_SHUTDOWN_ACTION ShutdownAction
  )
{
  UNREFERENCED_PARAMETER(MiniportAdapterContext);
  UNREFERENCED_PARAMETER(ShutdownAction);
}

static VOID
WwCancelOidRequest (
  NDIS_HANDLE MiniportAdapterContext,
  PVOID RequestId
  )
{
  UNREFERENCED_PARAMETER(MiniportAdapterContext);
  UNREFERENCED_PARAMETER(RequestId);
}

static VOID
WwUnload (
  PDRIVER_OBJECT DriverObject
  )
{
  UNREFERENCED_PARAMETER(DriverObject);
  if (gMiniportDriverHandle != NULL) {
    NdisMDeregisterMiniportDriver(gMiniportDriverHandle);
    gMiniportDriverHandle = NULL;
  }
}

NTSTATUS
DriverEntry (
  PDRIVER_OBJECT DriverObject,
  PUNICODE_STRING RegistryPath
  )
{
  NDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics;
  NDIS_STATUS Status;

  NdisZeroMemory(&Characteristics, sizeof(Characteristics));
  Characteristics.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
  Characteristics.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
  Characteristics.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
  Characteristics.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
  Characteristics.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
  Characteristics.MajorDriverVersion = 1U;
  Characteristics.MinorDriverVersion = 0U;
  Characteristics.SetOptionsHandler = NULL;
  Characteristics.InitializeHandlerEx = WwInitialize;
  Characteristics.HaltHandlerEx = WwHalt;
  Characteristics.UnloadHandler = WwUnload;
  Characteristics.PauseHandler = WwPause;
  Characteristics.RestartHandler = WwRestart;
  Characteristics.OidRequestHandler = WwOidRequest;
  Characteristics.SendNetBufferListsHandler = WwSendNetBufferLists;
  Characteristics.ReturnNetBufferListsHandler = WwReturnNetBufferLists;
  Characteristics.CancelSendHandler = WwCancelSend;
  Characteristics.CheckForHangHandlerEx = WwCheckForHang;
  Characteristics.ResetHandlerEx = WwReset;
  Characteristics.DevicePnPEventNotifyHandler = WwDevicePnPEventNotify;
  Characteristics.ShutdownHandlerEx = WwShutdown;
  Characteristics.CancelOidRequestHandler = WwCancelOidRequest;
  Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
      &Characteristics, &gMiniportDriverHandle);
  return (NTSTATUS)Status;
}
