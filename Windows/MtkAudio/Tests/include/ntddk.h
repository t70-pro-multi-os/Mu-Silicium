/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Focused host declarations, not a Windows ABI or kernel implementation. */
#ifndef AW_TEST_NTDDK_H
#define AW_TEST_NTDDK_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>
typedef int32_t NTSTATUS;
typedef uint32_t ULONG;typedef ULONG *PULONG;typedef int32_t LONG;
#define STATUS_NO_MATCH ((NTSTATUS)0xc0000272)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023)
#define STATUS_NOINTERFACE ((NTSTATUS)0xc00002b9)
#define POOL_FLAG_NON_PAGED 0x40ULL
void *ExAllocatePool2(uint64_t,size_t,ULONG);
void ExFreePoolWithTag(void *,ULONG);
LONG InterlockedIncrement(volatile LONG *);
LONG InterlockedDecrement(volatile LONG *);
ULONG READ_REGISTER_ULONG(volatile ULONG *Register);
void WRITE_REGISTER_ULONG(volatile ULONG *Register,ULONG Value);
typedef uint8_t UCHAR; typedef uint16_t USHORT;
typedef uintptr_t ULONG_PTR; typedef int64_t LONGLONG;
#define STATUS_NO_SUCH_DEVICE ((NTSTATUS)0xc000000e)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xc000009c)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xc00000b5)
#define NT_SUCCESS(S) ((NTSTATUS)(S)>=0)
typedef union { int64_t QuadPart; } PHYSICAL_ADDRESS;
typedef UCHAR CM_RESOURCE_TYPE;
#define CmResourceTypeMemory 3
#define CmResourceTypeInterrupt 2
#define CmResourceTypeConnection 132
#define CM_RESOURCE_CONNECTION_CLASS_SERIAL 1
#define CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C 1
#define CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE 0
#define CmResourceShareDeviceExclusive 1
#define CM_RESOURCE_MEMORY_READ_WRITE 0
/* Reduced host shape, not an ABI declaration. */
typedef struct {UCHAR Type,ShareDisposition;USHORT Flags;union{struct{PHYSICAL_ADDRESS Start;ULONG Length;} Memory;struct{ULONG Level,Vector;uintptr_t Affinity;} Interrupt;struct{UCHAR Class,Type;USHORT Reserved;ULONG IdLowPart,IdHighPart;} Connection;} u;} CM_PARTIAL_RESOURCE_DESCRIPTOR,*PCM_PARTIAL_RESOURCE_DESCRIPTOR;
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS,size_t,ULONG);
void MmUnmapIoSpace(void *,size_t);
#define PAGE_READWRITE 0x04u
#define PAGE_NOCACHE 0x200u
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define DISPATCH_LEVEL 2
#define PASSIVE_LEVEL 0
unsigned char KeGetCurrentIrql(void);
typedef uint8_t BOOLEAN;
#define FALSE 0
#define TRUE 1
struct _DMA_ADAPTER;
typedef struct _DMA_ADAPTER DMA_ADAPTER,*PDMA_ADAPTER;
typedef void (*PFREE_COMMON_BUFFER)(PDMA_ADAPTER,ULONG,PHYSICAL_ADDRESS,void *,BOOLEAN);
typedef void *(*PALLOCATE_COMMON_BUFFER_EX)(PDMA_ADAPTER,PHYSICAL_ADDRESS *,ULONG,PHYSICAL_ADDRESS *,BOOLEAN,ULONG);
struct _MDL;
typedef enum {CommonBufferConfigTypeLogicalAddressLimits} DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION_TYPE;
typedef struct {DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION_TYPE ConfigType;struct {PHYSICAL_ADDRESS MinimumAddress,MaximumAddress;} LogicalAddressLimits;} DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION;
typedef NTSTATUS (*PCREATE_COMMON_BUFFER_FROM_MDL)(PDMA_ADAPTER,struct _MDL *,DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION *,ULONG,PHYSICAL_ADDRESS *);
/* Deliberately reduced layout: NOT an ABI declaration. Official compile is separate. */
typedef struct {ULONG Size;PFREE_COMMON_BUFFER FreeCommonBuffer;PALLOCATE_COMMON_BUFFER_EX AllocateCommonBufferEx;PCREATE_COMMON_BUFFER_FROM_MDL CreateCommonBufferFromMdl;void (*PutDmaAdapter)(PDMA_ADAPTER);} DMA_OPERATIONS,*PDMA_OPERATIONS;
struct _DMA_ADAPTER {USHORT Version,Size;PDMA_OPERATIONS DmaOperations;};
volatile void *RtlCopyDeviceMemory(volatile void *,volatile const void *,size_t);
struct _DRIVER_OBJECT; struct _DEVICE_OBJECT; struct _IRP;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT,*PDRIVER_OBJECT;
typedef struct _DEVICE_OBJECT *PDEVICE_OBJECT;
struct _DEVICE_DESCRIPTION;
PDMA_ADAPTER IoGetDmaAdapter(PDEVICE_OBJECT,struct _DEVICE_DESCRIPTION *,PULONG);
void ObReferenceObject(void *);
void ObDereferenceObject(void *);
typedef void (*PDRIVER_UNLOAD)(PDRIVER_OBJECT);
typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(PDRIVER_OBJECT,PDEVICE_OBJECT);
typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT,struct _IRP *);
#define IRP_MJ_MAXIMUM_FUNCTION 27
typedef struct {PDRIVER_ADD_DEVICE AddDevice;} DRIVER_EXTENSION;
struct _DRIVER_OBJECT {PDRIVER_UNLOAD DriverUnload;DRIVER_EXTENSION *DriverExtension;PDRIVER_DISPATCH MajorFunction[28];};
#ifdef __cplusplus
}
#endif
#endif
