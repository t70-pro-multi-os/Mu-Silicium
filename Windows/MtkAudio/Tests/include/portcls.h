// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_TEST_PORTCLS_H
#define T70_TEST_PORTCLS_H
#include <wdf.h>
#include <ks.h>
NTSTATUS PcInitializeAdapterDriver(PDRIVER_OBJECT,PUNICODE_STRING,PDRIVER_ADD_DEVICE);
#ifdef __cplusplus
#define STDMETHODIMP_(T) T
typedef struct _KINTERRUPT *PKINTERRUPT;
#define HIGH_LEVEL 15
#define CONNECT_FULLY_SPECIFIED_GROUP 4
#define LevelSensitive 0
struct IO_CONNECT_INTERRUPT_PARAMETERS {
 ULONG Version;
 struct {PDEVICE_OBJECT PhysicalDeviceObject;PKINTERRUPT *InterruptObject;
 BOOLEAN (*ServiceRoutine)(PKINTERRUPT,void *);void *ServiceContext;void *SpinLock;
 unsigned char SynchronizeIrql,Irql;BOOLEAN FloatingSave,ShareVector;
 ULONG Vector;int InterruptMode;uintptr_t ProcessorEnableMask;USHORT Group;} FullySpecified;
};
struct IO_DISCONNECT_INTERRUPT_PARAMETERS {ULONG Version;union {PKINTERRUPT InterruptObject;void *Generic;} ConnectionContext;};
NTSTATUS IoConnectInterruptEx(IO_CONNECT_INTERRUPT_PARAMETERS *);
void IoDisconnectInterruptEx(IO_DISCONNECT_INTERRUPT_PARAMETERS *);

LONG InterlockedCompareExchange(volatile LONG *,LONG,LONG);
inline void *InterlockedCompareExchangePointer(void *volatile *P,void *V,void *E){return __sync_val_compare_and_swap(P,E,V);}
typedef struct _MDL {struct _MDL *Next;ULONG ByteCount,ByteOffset;} MDL,*PMDL;
#define PAGE_SIZE 4096
#define MAXLONGLONG INT64_MAX
#define MmGetMdlByteCount(M) ((M)->ByteCount)
#define MmGetMdlByteOffset(M) ((M)->ByteOffset)
typedef enum {MmNonCached,MmCached,MmWriteCombined} MEMORY_CACHING_TYPE;
typedef enum {KSSTATE_STOP,KSSTATE_ACQUIRE,KSSTATE_PAUSE,KSSTATE_RUN} KSSTATE;
typedef struct {uint64_t PlayOffset,WriteOffset;} KSAUDIO_POSITION,*PKSAUDIO_POSITION;
typedef struct {uint32_t FifoSize,ChipsetDelay,CodecDelay;} KSRTAUDIO_HWLATENCY;
typedef struct {uint64_t Fields[6];} KSRTAUDIO_HWREGISTER;
/* Deliberately narrow mock vtable; real override signatures compile separately. */
typedef struct _IRP *PIRP;
struct _DEVICE_OBJECT {void *DeviceExtension;};
struct IO_STACK_LOCATION {UCHAR MinorFunction;};
struct _IRP {IO_STACK_LOCATION Stack;struct {NTSTATUS Status;uintptr_t Information;} IoStatus;};
inline IO_STACK_LOCATION *IoGetCurrentIrpStackLocation(PIRP I){return &I->Stack;}
#define PORT_CLASS_DEVICE_EXTENSION_SIZE (64*sizeof(uintptr_t))
#define IRP_MJ_PNP 27
#define IRP_MN_QUERY_REMOVE_DEVICE 1
#define IRP_MN_QUERY_STOP_DEVICE 5
#define IRP_MN_CANCEL_STOP_DEVICE 6
#define IRP_MN_REMOVE_DEVICE 2
#define IRP_MN_CANCEL_REMOVE_DEVICE 3
#define IRP_MN_SURPRISE_REMOVAL 23
NTSTATUS PcDispatchIrp(PDEVICE_OBJECT,PIRP);
void IoCompleteRequest(PIRP,char);
class IUnknown {public:virtual NTSTATUS QueryInterface(const GUID &,void **){return STATUS_NOT_SUPPORTED;}virtual ULONG AddRef()=0;virtual ULONG Release()=0;};
typedef IUnknown *PUNKNOWN;
class IResourceList:public IUnknown {
public:
 virtual ULONG NumberOfEntriesOfType(CM_RESOURCE_TYPE){return 0;}
 virtual PCM_PARTIAL_RESOURCE_DESCRIPTOR FindTranslatedEntry(CM_RESOURCE_TYPE,ULONG){return nullptr;}
 virtual PCM_PARTIAL_RESOURCE_DESCRIPTOR FindUntranslatedEntry(CM_RESOURCE_TYPE,ULONG){return nullptr;}
};
typedef IResourceList *PRESOURCELIST;
typedef unsigned short WCHAR;
class IUnregisterSubdevice:public IUnknown {public:
 virtual NTSTATUS UnregisterSubdevice(PDEVICE_OBJECT,PUNKNOWN)=0;
};
using PUNREGISTERSUBDEVICE=IUnregisterSubdevice *;
NTSTATUS PcRegisterSubdevice(PDEVICE_OBJECT,WCHAR *,PUNKNOWN);
class IUnregisterPhysicalConnection:public IUnknown {public:
 virtual NTSTATUS UnregisterPhysicalConnection(PDEVICE_OBJECT,PUNKNOWN,ULONG,PUNKNOWN,ULONG)=0;
};
using PUNREGISTERPHYSICALCONNECTION=IUnregisterPhysicalConnection *;
NTSTATUS PcRegisterPhysicalConnection(PDEVICE_OBJECT,PUNKNOWN,ULONG,PUNKNOWN,ULONG);
class IPort:public IUnknown {
public:virtual NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)=0;
};
typedef IPort *PPORT;
class IPortTopology:public IPort {};
using PPORTTOPOLOGY=IPortTopology *;
typedef int DMA_WIDTH;
typedef int DMA_SPEED;
typedef uint64_t ULONGLONG;
typedef PHYSICAL_ADDRESS *PPHYSICAL_ADDRESS;
typedef void *PADAPTER_OBJECT;
class IDmaChannel:public IUnknown {public:
 virtual NTSTATUS AllocateBuffer(ULONG,PHYSICAL_ADDRESS *)=0;
 virtual void FreeBuffer()=0;virtual void *SystemAddress()=0;
 virtual ULONG AllocatedBufferSize()=0;virtual void SetBufferSize(ULONG)=0;
 virtual ULONG MaximumBufferSize(){return AllocatedBufferSize();}
 virtual ULONG BufferSize(){return AllocatedBufferSize();}
 virtual PHYSICAL_ADDRESS PhysicalAddress(){PHYSICAL_ADDRESS P={};return P;}
 virtual void CopyTo(void *,void *,ULONG)=0;
 virtual void CopyFrom(void *,void *,ULONG){}
 virtual ULONG TransferCount(){return 0;}
 virtual PADAPTER_OBJECT GetAdapterObject(){return nullptr;}
};
class IServiceGroup;
class IPortWaveCyclic:public IPort {public:
 virtual void Notify(IServiceGroup *){}
 virtual NTSTATUS NewMasterDmaChannel(IDmaChannel **,PUNKNOWN,PRESOURCELIST,ULONG,BOOLEAN,BOOLEAN,DMA_WIDTH,DMA_SPEED)=0;
};
NTSTATUS PcNewPort(PPORT *,const GUID &);
class IPortWaveRTStream {
public:
 virtual ULONG AddRef()=0;virtual ULONG Release()=0;
 virtual PMDL AllocateContiguousPagesForMdl(PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,size_t)=0;
 virtual void *MapAllocatedPages(PMDL,MEMORY_CACHING_TYPE)=0;
 virtual void UnmapAllocatedPages(void *,PMDL)=0;
 virtual void FreePagesFromMdl(PMDL)=0;
 virtual ULONG GetPhysicalPagesCount(PMDL)=0;
};
typedef LONGLONG *PLONGLONG;
typedef IDmaChannel *PDMACHANNEL;
typedef IPortWaveCyclic *PPORTWAVECYCLIC;
class IServiceSink:public IUnknown {public:virtual void RequestService(){}};
typedef IServiceSink *PSERVICESINK;
class IServiceGroup:public IServiceSink {public:
 virtual NTSTATUS AddMember(PSERVICESINK){return STATUS_NOT_SUPPORTED;}
 virtual void RemoveMember(PSERVICESINK){}
 virtual void SupportDelayedService(){}
 virtual void RequestDelayedService(ULONGLONG){}
 virtual void CancelDelayedService(){}
};
typedef IServiceGroup *PSERVICEGROUP;
NTSTATUS PcNewServiceGroup(PSERVICEGROUP *,PUNKNOWN);
typedef int POOL_TYPE;
class IMiniportWaveCyclicStream:public IUnknown {public:
 virtual NTSTATUS SetFormat(PKSDATAFORMAT)=0;
 virtual ULONG SetNotificationFreq(ULONG,PULONG)=0;
 virtual NTSTATUS SetState(KSSTATE)=0;
 virtual NTSTATUS GetPosition(PULONG)=0;
 virtual NTSTATUS NormalizePhysicalPosition(PLONGLONG)=0;
 virtual void Silence(void *,ULONG)=0;
};
typedef IMiniportWaveCyclicStream *PMINIPORTWAVECYCLICSTREAM;
class IMiniportWaveRTStream {
public:
 virtual NTSTATUS QueryInterface(const GUID &,void **)=0;
 virtual ULONG AddRef()=0;
 virtual ULONG Release()=0;
 virtual NTSTATUS SetFormat(KSDATAFORMAT *)=0;
 virtual NTSTATUS SetState(KSSTATE)=0;
 virtual NTSTATUS GetPosition(PKSAUDIO_POSITION)=0;
 virtual NTSTATUS AllocateAudioBuffer(ULONG,PMDL *,ULONG *,ULONG *,MEMORY_CACHING_TYPE *)=0;
 virtual void FreeAudioBuffer(PMDL,ULONG)=0;
 virtual void GetHWLatency(KSRTAUDIO_HWLATENCY *)=0;
 virtual NTSTATUS GetPositionRegister(KSRTAUDIO_HWREGISTER *)=0;
 virtual NTSTATUS GetClockRegister(KSRTAUDIO_HWREGISTER *)=0;
};
typedef IPortWaveRTStream *PPORTWAVERTSTREAM;
typedef IMiniportWaveRTStream *PMINIPORTWAVERTSTREAM;
class IPortWaveRT:public IPort {};
typedef IPortWaveRT *PPORTWAVERT;
typedef const GUID &REFIID;
typedef struct {KSDATAFORMAT DataRange;ULONG MaximumChannels,MinimumBitsPerSample,MaximumBitsPerSample,MinimumSampleFrequency,MaximumSampleFrequency;} KSDATARANGE_AUDIO;
#define KSPIN_DATAFLOW_IN 1
#define KSPIN_COMMUNICATION_SINK 1
#define KSPIN_DATAFLOW_OUT 2
#define KSPIN_COMMUNICATION_NONE 0
#define PCFILTER_NODE ((ULONG)-1)
typedef struct {ULONG DataRangesCount;KSDATAFORMAT **DataRanges;ULONG DataFlow,Communication;const GUID *Category,*Name;} KSPIN_DESCRIPTOR;
typedef struct {ULONG MaxGlobalInstanceCount,MaxFilterInstanceCount,MinFilterInstanceCount;void *AutomationTable;KSPIN_DESCRIPTOR KsPinDescriptor;} PCPIN_DESCRIPTOR;
typedef struct {ULONG Version;void *AutomationTable;ULONG PinSize,PinCount;const PCPIN_DESCRIPTOR *Pins;ULONG NodeSize,NodeCount;const void *Nodes;ULONG ConnectionCount;const void *Connections;ULONG CategoryCount;const GUID *Categories;} PCFILTER_DESCRIPTOR,*PPCFILTER_DESCRIPTOR;
typedef KSDATAFORMAT KSDATARANGE,*PKSDATARANGE;
typedef struct {ULONG FromNode,FromNodePin,ToNode,ToNodePin;} PCCONNECTION_DESCRIPTOR;
typedef struct {ULONG Flags;const void *AutomationTable;const GUID *Type,*Name;} PCNODE_DESCRIPTOR;
struct PCPROPERTY_REQUEST;
typedef PCPROPERTY_REQUEST *PPCPROPERTY_REQUEST;
typedef NTSTATUS (*PCPFNPROPERTY_HANDLER)(PPCPROPERTY_REQUEST);
struct PCPROPERTY_ITEM {const GUID *Set;ULONG Id,Flags;PCPFNPROPERTY_HANDLER Handler;};
struct PCPROPERTY_REQUEST {PUNKNOWN MajorTarget,MinorTarget;ULONG Node;const PCPROPERTY_ITEM *PropertyItem;ULONG Verb,InstanceSize;void *Instance;ULONG ValueSize;void *Value;PIRP Irp;};
struct PCAUTOMATION_TABLE {ULONG PropertyItemSize,PropertyCount;const PCPROPERTY_ITEM *Properties;ULONG MethodItemSize,MethodCount;const void *Methods;ULONG EventItemSize,EventCount;const void *Events;ULONG Reserved;};

class IMiniportTopology:public IUnknown {public:
 virtual NTSTATUS Init(PUNKNOWN,PRESOURCELIST,PPORTTOPOLOGY)=0;
 virtual NTSTATUS GetDescription(PPCFILTER_DESCRIPTOR *)=0;
 virtual NTSTATUS DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,PULONG)=0;
};
// Reduced host model; official-header compilation checks native fields/types.
#define DEVICE_DESCRIPTION_VERSION3 3
#define InterfaceTypeUndefined (-1)
typedef struct _DEVICE_DESCRIPTION {
 ULONG Version; BOOLEAN Master,ScatterGather,DemandMode,AutoInitialize;
 BOOLEAN Dma32BitAddresses,IgnoreCount,Reserved1,Dma64BitAddresses;
 ULONG BusNumber,DmaChannel; int InterfaceType,DmaWidth,DmaSpeed;
 ULONG MaximumLength,DmaPort,DmaAddressWidth,DmaControllerInstance,DmaRequestLine;
 PHYSICAL_ADDRESS DeviceAddress;
} DEVICE_DESCRIPTION,*PDEVICE_DESCRIPTION;
class IMiniportWaveRT:public IUnknown {
public:
 virtual NTSTATUS QueryInterface(const GUID &,void **)=0;
 virtual NTSTATUS GetDescription(PPCFILTER_DESCRIPTOR *)=0;
 virtual NTSTATUS DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,ULONG *)=0;
 virtual NTSTATUS Init(PUNKNOWN,PRESOURCELIST,PPORTWAVERT)=0;
 virtual NTSTATUS NewStream(PMINIPORTWAVERTSTREAM *,PPORTWAVERTSTREAM,ULONG,BOOLEAN,PKSDATAFORMAT)=0;
 virtual NTSTATUS GetDeviceDescription(PDEVICE_DESCRIPTION)=0;
};
NTSTATUS PcGetPhysicalDeviceObject(PDEVICE_OBJECT,PDEVICE_OBJECT *);
typedef NTSTATUS (*PCPFNSTARTDEVICE)(PDEVICE_OBJECT,PIRP,PRESOURCELIST);
NTSTATUS PcAddAdapterDevice(PDRIVER_OBJECT,PDEVICE_OBJECT,PCPFNSTARTDEVICE,ULONG,ULONG);
#endif

#ifdef __cplusplus
class IMiniportWaveCyclic:public IUnknown {public:
 virtual NTSTATUS GetDescription(PPCFILTER_DESCRIPTOR *)=0;
 virtual NTSTATUS DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,PULONG)=0;
 virtual NTSTATUS Init(PUNKNOWN,PRESOURCELIST,PPORTWAVECYCLIC)=0;
 virtual NTSTATUS NewStream(PMINIPORTWAVECYCLICSTREAM *,PUNKNOWN,POOL_TYPE,ULONG,BOOLEAN,PKSDATAFORMAT,PDMACHANNEL *,PSERVICEGROUP *)=0;
};
#endif

#endif

#ifdef __cplusplus
BOOLEAN KeSynchronizeExecution(PKINTERRUPT,BOOLEAN (*)(void *),void *);
#endif
#if defined(__cplusplus) && !defined(T70_TEST_RECOVERY_OS)
#define T70_TEST_RECOVERY_OS
struct KDPC {void (*Routine)(KDPC *,void *,void *,void *);void *Context;};
struct KEVENT {void *Host;};
struct KMUTEX {void *Host;};
struct IO_WORKITEM;typedef IO_WORKITEM *PIO_WORKITEM;
enum EVENT_TYPE {NotificationEvent};
enum KWAIT_REASON {Executive};
enum KPROCESSOR_MODE {KernelMode};
enum WORK_QUEUE_TYPE {DelayedWorkQueue};
#define IO_NO_INCREMENT 0
void KeInitializeDpc(KDPC *,void (*)(KDPC *,void *,void *,void *),void *);
BOOLEAN KeInsertQueueDpc(KDPC *,void *,void *);
void KeFlushQueuedDpcs();
void KeInitializeEvent(KEVENT *,EVENT_TYPE,BOOLEAN);
LONG KeSetEvent(KEVENT *,LONG,BOOLEAN);
void KeInitializeMutex(KMUTEX *,ULONG);
LONG KeReleaseMutex(KMUTEX *,BOOLEAN);
NTSTATUS KeWaitForSingleObject(void *,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,void *);
PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT);
void IoFreeWorkItem(PIO_WORKITEM);
void IoQueueWorkItem(PIO_WORKITEM,void (*)(PDEVICE_OBJECT,void *),WORK_QUEUE_TYPE,void *);
#endif

#if defined(__cplusplus) && !defined(T70_TEST_SYSTEM_THREAD_OS)
#define T70_TEST_SYSTEM_THREAD_OS
typedef void *HANDLE;typedef HANDLE *PHANDLE;
typedef unsigned char KIRQL;
typedef uintptr_t KSPIN_LOCK;
typedef struct {ULONG Length,Attributes;void *RootDirectory,*ObjectName,*SecurityDescriptor,*SecurityQualityOfService;} OBJECT_ATTRIBUTES,*POBJECT_ATTRIBUTES;
#define OBJ_KERNEL_HANDLE 0x200u
#define THREAD_ALL_ACCESS 0x1fffffu
#define InitializeObjectAttributes(P,N,A,R,S) do{(P)->Length=sizeof(*(P));(P)->RootDirectory=(R);(P)->Attributes=(A);(P)->ObjectName=(N);(P)->SecurityDescriptor=(S);(P)->SecurityQualityOfService=nullptr;}while(0)
NTSTATUS IoCreateSystemThread(void *,PHANDLE,ULONG,POBJECT_ATTRIBUTES,HANDLE,void *,void (*)(void *),void *);
NTSTATUS ZwClose(HANDLE);
void KeClearEvent(KEVENT *);
void KeInitializeSpinLock(KSPIN_LOCK *);
void KeAcquireSpinLock(KSPIN_LOCK *,KIRQL *);
void KeReleaseSpinLock(KSPIN_LOCK *,KIRQL);
#endif
