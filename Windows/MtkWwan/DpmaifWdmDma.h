/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_DPMAIF_WDM_DMA_H
#define T70_DPMAIF_WDM_DMA_H

#include <ntddk.h>
#include "DpmaifDma.h"
#include "DpmaifTxPacket.h"

typedef struct {
  PDMA_ADAPTER Adapter;
  PDEVICE_OBJECT PhysicalDeviceObject;
  ULONG MapRegisterCount;
  ULONG MaximumTransferBytes;
  /* Closing packs an atomic reference count with close/release state bits. */
  volatile LONG Closing;
  BOOLEAN Ready;
} T70_DPMAIF_WDM_DMA;
#define T70_DPMAIF_WDM_DMA_INITIALIZER {0}

typedef struct {
  T70_DPMAIF_WDM_DMA *Owner;
  PVOID VirtualAddress;
  PHYSICAL_ADDRESS LogicalAddress;
  ULONG Length;
  BOOLEAN CacheEnabled;
} T70_DPMAIF_COMMON_BUFFER;
#define T70_DPMAIF_COMMON_BUFFER_INITIALIZER {0}

typedef struct {
  T70_DPMAIF_WDM_DMA *Owner;
  PDMA_COMMON_BUFFER_VECTOR Vector;
  ULONG NumberOfElements;
  ULONG ElementBytes;
  ULONG ElementAlignment;
  BOOLEAN Ready;
} T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR;
#define T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR_INITIALIZER {0}

typedef enum {
  T70DpmaifMappingEmpty = 0,
  T70DpmaifMappingPending,
  T70DpmaifMappingCompleting,
  T70DpmaifMappingReady,
  T70DpmaifMappingReleasing,
  T70DpmaifMappingReleased,
  T70DpmaifMappingFailed
} T70_DPMAIF_MAPPING_STATE;

typedef struct {
  T70_DPMAIF_WDM_DMA *Owner;
  PDEVICE_OBJECT TargetDeviceObject;
  PSCATTER_GATHER_LIST ScatterGather;
  PDRIVER_LIST_CONTROL Completion;
  PVOID UserContext;
  BOOLEAN WriteToDevice;
  volatile LONG State;
} T70_DPMAIF_WDM_MAPPING;
#define T70_DPMAIF_WDM_MAPPING_INITIALIZER {0}

/* Dma must use T70_DPMAIF_WDM_DMA_INITIALIZER. Use a PnP PDO. DEVICE_DESCRIPTION_VERSION3
 * requests the source-derived 36-bit address width. Release returns
 * DEVICE_BUSY until all common buffers and SG mappings have been freed. */
NTSTATUS T70DpmaifWdmDmaInitialize(
  T70_DPMAIF_WDM_DMA *Dma,
  PDEVICE_OBJECT PhysicalDeviceObject,
  ULONG MaximumTransferBytes
  );
NTSTATUS T70DpmaifWdmDmaRelease(T70_DPMAIF_WDM_DMA *Dma);
NTSTATUS T70DpmaifWdmMappingInitialize(T70_DPMAIF_WDM_MAPPING *Mapping);
NTSTATUS T70DpmaifWdmCommonBufferInitialize(
  T70_DPMAIF_COMMON_BUFFER *Buffer
  );
NTSTATUS T70DpmaifWdmAllocateCommonBuffer(
  T70_DPMAIF_WDM_DMA *Dma,
  ULONG Length,
  ULONG Alignment,
  BOOLEAN CacheEnabled,
  T70_DPMAIF_COMMON_BUFFER *Buffer
  );
NTSTATUS T70DpmaifWdmFreeCommonBuffer(
  T70_DPMAIF_WDM_DMA *Dma,
  T70_DPMAIF_COMMON_BUFFER *Buffer
  );
NTSTATUS T70DpmaifWdmAllocateCommonBufferVector(
  T70_DPMAIF_WDM_DMA *Dma,
  ULONG NumberOfElements,
  ULONG ElementBytes,
  ULONG ElementAlignment,
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Vector
  );
NTSTATUS T70DpmaifWdmGetCommonBufferVectorElement(
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Vector,
  ULONG Index,
  PVOID *VirtualAddress,
  PHYSICAL_ADDRESS *LogicalAddress
  );
NTSTATUS T70DpmaifWdmFreeCommonBufferVector(
  T70_DPMAIF_WDM_DMA *Dma,
  T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR *Vector
  );
/* Initialize each mapping before the first use, then reinitialize it only
 * after its previous map was released or failed. Mapping and callback context
 * must remain nonpaged and valid until the completion callback runs. The
 * callback receives UserContext as supplied. */
NTSTATUS T70DpmaifWdmGetScatterGatherList(
  T70_DPMAIF_WDM_DMA *Dma,
  PDEVICE_OBJECT TargetDeviceObject,
  PMDL Mdl,
  PVOID CurrentVa,
  ULONG Length,
  PDRIVER_LIST_CONTROL Completion,
  PVOID UserContext,
  BOOLEAN WriteToDevice,
  T70_DPMAIF_WDM_MAPPING *Mapping
  );
/* Call exactly once after the device stops using the mapping. */
NTSTATUS T70DpmaifWdmPutScatterGatherList(
  T70_DPMAIF_WDM_MAPPING *Mapping
  );
T70_DPMAIF_RING_STATUS T70DpmaifWdmNormalizeScatterGatherList(
  PSCATTER_GATHER_LIST ScatterGather,
  uint32_t ExpectedBytes,
  T70_DPMAIF_TX_SEGMENT *Segments,
  uint32_t Capacity,
  uint32_t *NormalizedCount
  );

#endif
