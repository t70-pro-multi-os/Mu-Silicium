// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#define PEP_KERNEL_INFORMATION_VERSION PEP_KERNEL_INFORMATION_V3
#include <pep_x.h>
#include <initguid.h>
#include "CpuIo.h"
#include "CpuPpmCore.h"

// Stock mediatek-cpufreq-hw.ko, cpufreq_mtk_offsets.
// The two domain windows and the CSRAM range come from the effective T70 DTB.
#define CPU_LUT 0x00
#define CPU_ENABLE 0x84
#define CPU_PERFORMANCE 0x88
#define CPU_STATUS 0x8c
#define CPU_TABLE_VERSION 2
#define CPU_READY_MASK 3u

static const struct {
    ULONG Address;
    ULONG Length;
    USHORT Access;
} CpuResources[] = {
    {0x0011bc10, 0x120, CM_RESOURCE_MEMORY_READ_WRITE},
    {0x0011bd30, 0x120, CM_RESOURCE_MEMORY_READ_WRITE},
    {0x00114ffc, sizeof(ULONG), CM_RESOURCE_MEMORY_READ_ONLY}, // USRAM + 0xbfc.
    {0x0011c0cc, sizeof(ULONG), CM_RESOURCE_MEMORY_READ_ONLY}, // CSRAM + 0x4cc.
};
#define CPU_VERSION_RESOURCE T70_CPU_DOMAIN_COUNT
#define CPU_MODE_RESOURCE (CPU_VERSION_RESOURCE + 1)
C_ASSERT(ARRAYSIZE(CpuResources) == CPU_MODE_RESOURCE + 1);
C_ASSERT(CPU_LUT + T70_CPU_MAX_OPPS * sizeof(ULONG) <= CPU_ENABLE);

typedef struct {
    PULONG Region[ARRAYSIZE(CpuResources)];
} CPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CPU_DEVICE, CpuDevice)

#define CPU_PPM_PROCESSOR_SIGNATURE 0x70303763u
#define CPU_PPM_PROCESSOR_COUNT 8u
#define CPU_PPM_EARLY_HIGH_LITTLE 766000U
#define CPU_PPM_EARLY_LOW_LITTLE 574500U
#define CPU_PPM_EARLY_HIGH_BIG 2252800U
#define CPU_PPM_EARLY_LOW_BIG 1740800U
typedef struct {
    ULONG Signature;
    ULONG Uid;
    POHANDLE KernelHandle;
    BOOLEAN Registered;
    BOOLEAN Started;
    BOOLEAN RefreshPending;
    BOOLEAN RefreshOutstanding;
    BOOLEAN RefreshComplete;
} CPU_PPM_PROCESSOR;

static KSPIN_LOCK CpuPpmLock;
static CPU_DEVICE *CpuPpmDevice;
static CPU_PPM_PROCESSOR CpuPpmProcessors[CPU_PPM_PROCESSOR_COUNT];
static T70_CPU_PPM_DOMAIN CpuPpmDomainCache[T70_CPU_PPM_DOMAINS];
static BOOLEAN CpuPpmDomainCacheValid;
static PEP_KERNEL_INFORMATION CpuPpmKernelInformation;
static ULONG CpuPpmRefreshCursor;
static const GUID CpuPpmPerfConstraintChange =
    {0x29181fa1,0x4bf3,0x4c2e,{0xb3,0x14,0xa6,0xd2,0x26,0x32,0x2b,0x00}};

static VOID CpuPpmRequestNextRefresh(VOID);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD CpuAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE CpuPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE CpuRelease;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL CpuIoControl;

static VOID CpuUnmap(CPU_DEVICE *Device) {
    for (ULONG Index = 0; Index < ARRAYSIZE(CpuResources); ++Index) {
        if (Device->Region[Index]) {
            MmUnmapIoSpace(Device->Region[Index], CpuResources[Index].Length);
            Device->Region[Index] = NULL;
        }
    }
}

static BOOLEAN CpuResourceMatches(PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource, ULONG Index) {
    return Resource && Resource->Type == CmResourceTypeMemory &&
        Resource->Flags == CpuResources[Index].Access &&
        Resource->u.Memory.Start.QuadPart == CpuResources[Index].Address &&
        Resource->u.Memory.Length == CpuResources[Index].Length;
}

static VOID CpuInitializePpm(VOID) {
    for (ULONG Uid = 0; Uid < CPU_PPM_PROCESSOR_COUNT; ++Uid) {
        CpuPpmProcessors[Uid].Signature = CPU_PPM_PROCESSOR_SIGNATURE;
        CpuPpmProcessors[Uid].Uid = Uid;
        CpuPpmProcessors[Uid].KernelHandle = NULL;
        CpuPpmProcessors[Uid].Registered = FALSE;
        CpuPpmProcessors[Uid].Started = FALSE;
        CpuPpmProcessors[Uid].RefreshPending = FALSE;
        CpuPpmProcessors[Uid].RefreshOutstanding = FALSE;
        CpuPpmProcessors[Uid].RefreshComplete = FALSE;
    }
    RtlZeroMemory(CpuPpmDomainCache, sizeof(CpuPpmDomainCache));
    CpuPpmDomainCacheValid = FALSE;
    RtlZeroMemory(&CpuPpmKernelInformation, sizeof(CpuPpmKernelInformation));
    CpuPpmRefreshCursor = 0;
}

NTSTATUS CpuPrepare(WDFDEVICE Handle, WDFCMRESLIST Raw, WDFCMRESLIST Translated) {
    CPU_DEVICE *Device = CpuDevice(Handle);
    if (Device->Region[0] || WdfCmResourceListGetCount(Raw) != ARRAYSIZE(CpuResources) ||
        WdfCmResourceListGetCount(Translated) != ARRAYSIZE(CpuResources)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (ULONG Index = 0; Index < ARRAYSIZE(CpuResources); ++Index) {
        if (!CpuResourceMatches(WdfCmResourceListGetDescriptor(Raw, Index), Index) ||
            !CpuResourceMatches(WdfCmResourceListGetDescriptor(Translated, Index), Index)) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    for (ULONG Index = 0; Index < ARRAYSIZE(CpuResources); ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource = WdfCmResourceListGetDescriptor(Translated, Index);
        ULONG Protection = CpuResources[Index].Access == CM_RESOURCE_MEMORY_READ_WRITE ?
            PAGE_READWRITE : PAGE_READONLY;
        Device->Region[Index] = MmMapIoSpaceEx(Resource->u.Memory.Start, CpuResources[Index].Length,
            Protection | PAGE_NOCACHE);
        if (!Device->Region[Index]) {
            CpuUnmap(Device);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    KIRQL OldIrql;
    KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
    if (CpuPpmDevice != NULL) {
        KeReleaseSpinLock(&CpuPpmLock, OldIrql);
        CpuUnmap(Device);
        return STATUS_DEVICE_BUSY;
    }
    CpuPpmDevice = Device;
    CpuPpmDomainCacheValid = FALSE;
    KeReleaseSpinLock(&CpuPpmLock, OldIrql);
    return STATUS_SUCCESS;
}

NTSTATUS CpuRelease(WDFDEVICE Handle, WDFCMRESLIST Translated) {
    UNREFERENCED_PARAMETER(Translated);
    CPU_DEVICE *Device = CpuDevice(Handle);
    KIRQL OldIrql;
    KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
    if (CpuPpmDevice == Device) {
        CpuPpmDevice = NULL;
        CpuPpmDomainCacheValid = FALSE;
        CpuPpmRefreshCursor = 0;
        for (ULONG Uid = 0; Uid < CPU_PPM_PROCESSOR_COUNT; ++Uid) {
            CpuPpmProcessors[Uid].RefreshPending = FALSE;
            CpuPpmProcessors[Uid].RefreshOutstanding = FALSE;
            CpuPpmProcessors[Uid].RefreshComplete = FALSE;
            CpuPpmProcessors[Uid].Started = FALSE;
        }
    }
    KeReleaseSpinLock(&CpuPpmLock, OldIrql);
    CpuUnmap(Device);
    return STATUS_SUCCESS;
}

static BOOLEAN CpuReady(PULONG Registers) {
    ULONG Enable = READ_REGISTER_ULONG(Registers + CPU_ENABLE / sizeof(ULONG));
    ULONG Status = READ_REGISTER_ULONG(Registers + CPU_STATUS / sizeof(ULONG));
    return Enable == 1 && Status != MAXULONG && (Status & CPU_READY_MASK) == CPU_READY_MASK;
}

static NTSTATUS CpuReadLocked(const CPU_DEVICE *Device, ULONG Domain, T70_CPU_READING *Reading,
                              PULONG TableVersion) {
    if (Domain >= T70_CPU_DOMAIN_COUNT) return STATUS_INVALID_PARAMETER;
    PULONG Registers = Device->Region[Domain];
    PULONG VersionRegister = Device->Region[CPU_VERSION_RESOURCE];
    if (!Registers || !VersionRegister || !CpuReady(Registers)) return STATUS_DEVICE_NOT_READY;

    ULONG Version = READ_REGISTER_ULONG(VersionRegister);
    if (Version > CPU_TABLE_VERSION) return STATUS_DEVICE_NOT_READY;
    ULONG Value = READ_REGISTER_ULONG(Registers + CPU_PERFORMANCE / sizeof(ULONG));
    ULONG Mask = Version == CPU_TABLE_VERSION ? 0x3fffu : 0xfffu;

    // The first repeated frequency terminates the stock SRAM table. Validate
    // the order used by the stock raw-frequency bounds before using it.
    ULONG Count = 0;
    ULONG Previous = 0;
    for (; Count < T70_CPU_MAX_OPPS; ++Count) {
        ULONG Entry = READ_REGISTER_ULONG(Registers + CPU_LUT / sizeof(ULONG) + Count);
        ULONG Khz = (Entry & Mask) * 1000u;
        if (Entry == MAXULONG || !Khz) return STATUS_DEVICE_DATA_ERROR;
        if (Khz == Previous) break;
        if (Count && Khz > Previous) return STATUS_DEVICE_DATA_ERROR;

        Reading->FrequenciesKhz[Count] = Khz;
        Previous = Khz;
    }

    // mtk_cpufreq_hw_get interprets 0..31 as an index. Larger words are kHz.
    if (Value < T70_CPU_MAX_OPPS) {
        if (Value >= Count) return STATUS_DEVICE_DATA_ERROR;
        Reading->RequestedKhz = Reading->FrequenciesKhz[Value];
    } else {
        if (Value > Reading->FrequenciesKhz[0] || Value < Previous) return STATUS_DEVICE_DATA_ERROR;
        Reading->RequestedKhz = Value;
    }

    if (!CpuReady(Registers) || READ_REGISTER_ULONG(VersionRegister) != Version ||
        READ_REGISTER_ULONG(Registers + CPU_PERFORMANCE / sizeof(ULONG)) != Value) {
        return STATUS_RETRY;
    }

    Reading->ControllerValue = Value;
    Reading->FrequencyCount = Count;
    if (TableVersion) *TableVersion = Version;
    return STATUS_SUCCESS;
}

static NTSTATUS CpuSetOppLocked(const CPU_DEVICE *Device, const T70_CPU_TARGET *Target) {
    if (Target->Domain >= T70_CPU_DOMAIN_COUNT || !Target->FrequencyKhz)
        return STATUS_INVALID_PARAMETER;
    PULONG Mode = Device->Region[CPU_MODE_RESOURCE];
    if (!Mode) return STATUS_DEVICE_NOT_READY;
    // Stock mtk_cpufreq_hw_target_index takes the table index only when
    // freq_scaling_disabled (set by the CSRAM probe) is zero.
    if (READ_REGISTER_ULONG(Mode) != 0) return STATUS_NOT_SUPPORTED;

    T70_CPU_READING Reading;
    ULONG Version;
    NTSTATUS Status = CpuReadLocked(Device, Target->Domain, &Reading, &Version);
    if (!NT_SUCCESS(Status)) return Status;
    if (Reading.ControllerValue >= T70_CPU_MAX_OPPS) return STATUS_NOT_SUPPORTED;

    ULONG Index = 0;
    while (Index < Reading.FrequencyCount && Reading.FrequenciesKhz[Index] != Target->FrequencyKhz)
        ++Index;
    if (Index == Reading.FrequencyCount) return STATUS_INVALID_PARAMETER;

    PULONG Registers = Device->Region[Target->Domain];
    PULONG Performance = Registers + CPU_PERFORMANCE / sizeof(ULONG);
    ULONG Entry = READ_REGISTER_ULONG(Registers + CPU_LUT / sizeof(ULONG) + Index);
    ULONG Mask = Version == CPU_TABLE_VERSION ? 0x3fffu : 0xfffu;
    if (Entry == MAXULONG || (Entry & Mask) * 1000u != Target->FrequencyKhz ||
        !CpuReady(Registers) || READ_REGISTER_ULONG(Mode) != 0 ||
        READ_REGISTER_ULONG(Device->Region[CPU_VERSION_RESOURCE]) != Version ||
        READ_REGISTER_ULONG(Performance) != Reading.ControllerValue)
        return STATUS_RETRY;

    if (Index == Reading.ControllerValue) return STATUS_SUCCESS;
    WRITE_REGISTER_ULONG(Performance, Index);
    if (!CpuReady(Registers)) return STATUS_DEVICE_POWER_FAILURE;
    if (READ_REGISTER_ULONG(Mode) != 0 || READ_REGISTER_ULONG(Device->Region[CPU_VERSION_RESOURCE]) != Version)
        return STATUS_RETRY;
    return READ_REGISTER_ULONG(Performance) == Index ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS CpuRead(const CPU_DEVICE *Device, ULONG Domain, T70_CPU_READING *Reading,
                        PULONG TableVersion) {
    KIRQL OldIrql;
    KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
    NTSTATUS Status = Device == CpuPpmDevice ?
        CpuReadLocked(Device, Domain, Reading, TableVersion) : STATUS_DEVICE_NOT_READY;
    KeReleaseSpinLock(&CpuPpmLock, OldIrql);
    return Status;
}

static NTSTATUS CpuSetOpp(const CPU_DEVICE *Device, const T70_CPU_TARGET *Target) {
    KIRQL OldIrql;
    KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
    NTSTATUS Status = Device == CpuPpmDevice ?
        CpuSetOppLocked(Device, Target) : STATUS_DEVICE_NOT_READY;
    KeReleaseSpinLock(&CpuPpmLock, OldIrql);
    return Status;
}

static BOOLEAN CpuPpmUid(PCUNICODE_STRING DeviceId, PULONG Uid) {
    uint32_t ParsedUid;
    if (DeviceId == NULL || DeviceId->Buffer == NULL || Uid == NULL ||
        (DeviceId->Length % sizeof(WCHAR)) != 0)
        return FALSE;
    if (!T70CpuPpmUidFromDeviceId((const uint16_t *)DeviceId->Buffer,
        DeviceId->Length / sizeof(WCHAR), &ParsedUid))
        return FALSE;
    *Uid = (ULONG)ParsedUid;
    return TRUE;
}

static CPU_PPM_PROCESSOR *CpuPpmProcessorFromHandle(PEPHANDLE Handle) {
    for (ULONG Uid = 0; Uid < CPU_PPM_PROCESSOR_COUNT; ++Uid) {
        if (Handle == (PEPHANDLE)&CpuPpmProcessors[Uid] &&
            CpuPpmProcessors[Uid].Signature == CPU_PPM_PROCESSOR_SIGNATURE &&
            CpuPpmProcessors[Uid].Registered)
            return &CpuPpmProcessors[Uid];
    }
    return NULL;
}

static VOID CpuPpmRequestNextRefresh(VOID) {
    KIRQL OldIrql;
    POHANDLE KernelHandle = NULL;
    if (CpuPpmDevice == NULL || CpuPpmKernelInformation.RequestWorker == NULL) return;
    KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
    for (ULONG Offset = 0; Offset < CPU_PPM_PROCESSOR_COUNT; ++Offset) {
        ULONG Uid = (CpuPpmRefreshCursor + Offset) % CPU_PPM_PROCESSOR_COUNT;
        CPU_PPM_PROCESSOR *Processor = &CpuPpmProcessors[Uid];
        if (!Processor->Registered || !Processor->Started || Processor->RefreshPending ||
            Processor->RefreshOutstanding || Processor->RefreshComplete ||
            Processor->KernelHandle == NULL) continue;
        Processor->RefreshPending = TRUE;
        CpuPpmRefreshCursor = Uid;
        KernelHandle = Processor->KernelHandle;
        break;
    }
    KeReleaseSpinLock(&CpuPpmLock, OldIrql);
    if (KernelHandle != NULL)
        CpuPpmKernelInformation.RequestWorker(KernelHandle);
}

static BOOLEAN CpuPpmQueueRefreshWork(PEP_WORK *Work) {
    KIRQL OldIrql;
    CPU_PPM_PROCESSOR *Processor = NULL;
    if (Work == NULL) return FALSE;
    Work->WorkInformation = NULL;
    Work->NeedWork = FALSE;
    KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
    for (ULONG Uid = 0; Uid < CPU_PPM_PROCESSOR_COUNT; ++Uid) {
        if (CpuPpmProcessors[Uid].Registered && CpuPpmProcessors[Uid].RefreshPending) {
            Processor = &CpuPpmProcessors[Uid];
            Processor->RefreshPending = FALSE;
            Processor->RefreshOutstanding = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&CpuPpmLock, OldIrql);
    if (Processor == NULL) return TRUE;

    static PEP_WORK_INFORMATION WorkInformation;
    RtlZeroMemory(&WorkInformation, sizeof(WorkInformation));
    WorkInformation.WorkType = PepWorkRequestPowerControl;
    WorkInformation.PowerControl.DeviceHandle = Processor->KernelHandle;
    WorkInformation.PowerControl.PowerControlCode = &CpuPpmPerfConstraintChange;
    WorkInformation.PowerControl.RequestContext = (PVOID)(ULONG_PTR)(Processor->Uid + 1U);
    WorkInformation.PowerControl.InBuffer = NULL;
    WorkInformation.PowerControl.InBufferSize = 0;
    WorkInformation.PowerControl.OutBuffer = NULL;
    WorkInformation.PowerControl.OutBufferSize = 0;
    Work->WorkInformation = &WorkInformation;
    Work->NeedWork = TRUE;
    return TRUE;
}

static BOOLEAN CpuPpmBuildDomainsLocked(CPU_DEVICE *Device,
    T70_CPU_PPM_DOMAIN Domains[T70_CPU_PPM_DOMAINS]) {
    static const uint32_t CapacityDmipsMhz[T70_CPU_PPM_DOMAINS] = {383, 1024};
    uint32_t Frequencies[T70_CPU_PPM_DOMAINS][T70_CPU_PPM_MAX_OPPS] = {{0}};
    uint32_t Counts[T70_CPU_PPM_DOMAINS] = {0};
    ULONG TableVersion = MAXULONG;
    if (Device == NULL || Device != CpuPpmDevice ||
        Device->Region[CPU_MODE_RESOURCE] == NULL ||
        READ_REGISTER_ULONG(Device->Region[CPU_MODE_RESOURCE]) != 0)
        return FALSE;
    if (CpuPpmDomainCacheValid) {
        RtlCopyMemory(Domains, CpuPpmDomainCache, sizeof(CpuPpmDomainCache));
        return TRUE;
    }
    for (ULONG Domain = 0; Domain < T70_CPU_PPM_DOMAINS; ++Domain) {
        T70_CPU_READING Reading = {0};
        ULONG Version;
        if (!NT_SUCCESS(CpuReadLocked(Device, Domain, &Reading, &Version)) ||
            Reading.ControllerValue >= T70_CPU_MAX_OPPS ||
            (Domain != 0 && Version != TableVersion))
            return FALSE;
        if (Domain == 0) TableVersion = Version;
        Counts[Domain] = (uint32_t)Reading.FrequencyCount;
        for (ULONG Index = 0; Index < Reading.FrequencyCount; ++Index)
            Frequencies[Domain][Index] = (uint32_t)Reading.FrequenciesKhz[Index];
    }
    if (READ_REGISTER_ULONG(Device->Region[CPU_MODE_RESOURCE]) != 0 ||
        READ_REGISTER_ULONG(Device->Region[CPU_VERSION_RESOURCE]) != TableVersion)
        return FALSE;
    if (!T70CpuPpmBuild(Frequencies, Counts, CapacityDmipsMhz, CpuPpmDomainCache))
        return FALSE;
    RtlCopyMemory(Domains, CpuPpmDomainCache, sizeof(CpuPpmDomainCache));
    CpuPpmDomainCacheValid = TRUE;
    return TRUE;
}

static BOOLEAN CpuDpmNotify(ULONG Notification, PVOID Data) {
    ULONG Uid;
    KIRQL OldIrql;
    if (Data == NULL) return FALSE;
    switch (Notification) {
    case PEP_DPM_PREPARE_DEVICE: {
        PPEP_PREPARE_DEVICE Prepare = (PPEP_PREPARE_DEVICE)Data;
        Prepare->DeviceAccepted = CpuPpmUid(Prepare->DeviceId, &Uid);
        return TRUE;
    }
    case PEP_DPM_ABANDON_DEVICE: {
        PPEP_ABANDON_DEVICE Abandon = (PPEP_ABANDON_DEVICE)Data;
        Abandon->DeviceAccepted = CpuPpmUid(Abandon->DeviceId, &Uid);
        return TRUE;
    }
    case PEP_DPM_REGISTER_DEVICE: {
        PPEP_REGISTER_DEVICE Register = (PPEP_REGISTER_DEVICE)Data;
        Register->DeviceHandle = NULL;
        Register->DeviceAccepted = PepDeviceNotAccepted;
        if (!CpuPpmUid(Register->DeviceId, &Uid)) return TRUE;
        KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
        CPU_PPM_PROCESSOR *Processor = &CpuPpmProcessors[Uid];
        if (Processor->Signature == CPU_PPM_PROCESSOR_SIGNATURE &&
            (!Processor->Registered || Processor->KernelHandle == Register->KernelHandle)) {
            Processor->Uid = Uid;
            Processor->KernelHandle = Register->KernelHandle;
            Processor->Registered = TRUE;
            Processor->Started = FALSE;
            Processor->RefreshPending = FALSE;
            Processor->RefreshOutstanding = FALSE;
            Processor->RefreshComplete = FALSE;
            Register->DeviceHandle = (PEPHANDLE)Processor;
            Register->DeviceAccepted = PepDeviceAccepted;
        }
        KeReleaseSpinLock(&CpuPpmLock, OldIrql);
        return TRUE;
    }
    case PEP_DPM_UNREGISTER_DEVICE: {
        PPEP_UNREGISTER_DEVICE Unregister = (PPEP_UNREGISTER_DEVICE)Data;
        KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
        CPU_PPM_PROCESSOR *Processor = CpuPpmProcessorFromHandle(Unregister->DeviceHandle);
        if (Processor != NULL) {
            Processor->Registered = FALSE;
            Processor->Started = FALSE;
            Processor->KernelHandle = NULL;
            Processor->RefreshPending = FALSE;
            Processor->RefreshOutstanding = FALSE;
            Processor->RefreshComplete = FALSE;
        }
        KeReleaseSpinLock(&CpuPpmLock, OldIrql);
        return Processor != NULL;
    }
    case PEP_DPM_DEVICE_STARTED: {
        PPEP_DEVICE_STARTED Started = (PPEP_DEVICE_STARTED)Data;
        KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
        CPU_PPM_PROCESSOR *Processor = CpuPpmProcessorFromHandle(Started->DeviceHandle);
        if (Processor != NULL) Processor->Started = TRUE;
        KeReleaseSpinLock(&CpuPpmLock, OldIrql);
        return Processor != NULL;
    }
    case PEP_DPM_DEVICE_POWER_STATE: {
        PPEP_DEVICE_POWER_STATE Power = (PPEP_DEVICE_POWER_STATE)Data;
        BOOLEAN Refresh = FALSE;
        KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
        CPU_PPM_PROCESSOR *Processor = CpuPpmProcessorFromHandle(Power->DeviceHandle);
        if (Processor != NULL && Power->Complete && Power->PowerState == PowerDeviceD0) {
            CpuPpmDomainCacheValid = FALSE;
            Processor->RefreshComplete = FALSE;
            Refresh = TRUE;
        }
        KeReleaseSpinLock(&CpuPpmLock, OldIrql);
        if (Refresh) CpuPpmRequestNextRefresh();
        return Processor != NULL;
    }
    case PEP_DPM_WORK:
        return CpuPpmQueueRefreshWork((PEP_WORK *)Data);
    case PEP_DPM_POWER_CONTROL_COMPLETE: {
        PEP_POWER_CONTROL_COMPLETE *Complete = (PEP_POWER_CONTROL_COMPLETE *)Data;
        ULONG EncodedUid = (ULONG)(ULONG_PTR)Complete->RequestContext;
        if (Complete->PowerControlCode != &CpuPpmPerfConstraintChange ||
            EncodedUid == 0 || EncodedUid > CPU_PPM_PROCESSOR_COUNT) return FALSE;
        KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
        CPU_PPM_PROCESSOR *Processor = &CpuPpmProcessors[EncodedUid - 1U];
        if (!Processor->Registered ||
            Complete->DeviceHandle != (PEPHANDLE)Processor ||
            !Processor->RefreshOutstanding) {
            KeReleaseSpinLock(&CpuPpmLock, OldIrql);
            return FALSE;
        }
        Processor->RefreshOutstanding = FALSE;
        Processor->RefreshComplete = TRUE;
        CpuPpmRefreshCursor = Processor->Uid + 1U;
        KeReleaseSpinLock(&CpuPpmLock, OldIrql);
        CpuPpmRequestNextRefresh();
        return TRUE;
    }
    default:
        return FALSE;
    }
}

static BOOLEAN CpuPpmNotify(PEPHANDLE Handle, ULONG Notification, PVOID Data) {
    KIRQL OldIrql;
    BOOLEAN Accepted = FALSE;
    if (Data == NULL) return FALSE;
    KeAcquireSpinLock(&CpuPpmLock, &OldIrql);
    CPU_PPM_PROCESSOR *Processor = CpuPpmProcessorFromHandle(Handle);
    T70_CPU_PPM_DOMAIN Domains[T70_CPU_PPM_DOMAINS];
    if (Processor == NULL) goto Done;
    if (Notification == PEP_NOTIFY_PPM_ENTER_SYSTEM_STATE) {
        Accepted = TRUE;
        goto Done;
    }
    if (Notification == PEP_NOTIFY_PPM_RESUME_FROM_SYSTEM_STATE) {
        CpuPpmDomainCacheValid = FALSE;
        Accepted = TRUE;
        goto Done;
    }
    if (Notification == PEP_NOTIFY_PPM_QUERY_CAPABILITIES) {
        PPEP_PPM_QUERY_CAPABILITIES Capabilities =
            (PPEP_PPM_QUERY_CAPABILITIES)Data;
        Capabilities->FeedbackCounterCount = 0;
        Capabilities->IdleStateCount = 0;
        Capabilities->PerformanceStatesSupported = TRUE;
        Capabilities->ParkingSupported = FALSE;
        Capabilities->DiscretePerformanceStateCount = 0;
        Capabilities->Reserved = 0;
        Accepted = TRUE;
        goto Done;
    }
    ULONG Domain = Processor->Uid < 6U ? 0U : 1U;
    if (Notification == PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES &&
        CpuPpmDevice == NULL) {
        PPEP_PPM_QUERY_PERF_CAPABILITIES Capabilities =
            (PPEP_PPM_QUERY_PERF_CAPABILITIES)Data;
        Capabilities->HighestPerformance = Domain == 0U ?
            CPU_PPM_EARLY_HIGH_LITTLE : CPU_PPM_EARLY_HIGH_BIG;
        Capabilities->NominalPerformance = Capabilities->HighestPerformance;
        Capabilities->LowestNonlinearPerformance = Domain == 0U ?
            CPU_PPM_EARLY_LOW_LITTLE : CPU_PPM_EARLY_LOW_BIG;
        Capabilities->LowestPerformance = Capabilities->LowestNonlinearPerformance;
        Capabilities->DomainId = Domain;
        Capabilities->DomainMembers = Domain == 0U ? 6U : 2U;
        Accepted = TRUE;
        goto Done;
    }
    if (!CpuPpmBuildDomainsLocked(CpuPpmDevice, Domains)) goto Done;

    switch (Notification) {
    case PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES: {
        PPEP_PPM_QUERY_PERF_CAPABILITIES Capabilities =
            (PPEP_PPM_QUERY_PERF_CAPABILITIES)Data;
        Capabilities->HighestPerformance = Domains[Domain].HighestPerformance;
        Capabilities->NominalPerformance = Domains[Domain].NominalPerformance;
        Capabilities->LowestNonlinearPerformance = Domains[Domain].LowestNonlinearPerformance;
        Capabilities->LowestPerformance = Domains[Domain].LowestPerformance;
        Capabilities->DomainId = Domain;
        Capabilities->DomainMembers = Domain == 0U ? 6U : 2U;
        Accepted = TRUE;
        break;
    }
    case PEP_NOTIFY_PPM_PERF_CONSTRAINTS: {
        PPEP_PPM_PERF_CONSTRAINTS Constraints = (PPEP_PPM_PERF_CONSTRAINTS)Data;
        // The stock OPP data does not specify a sustained thermal/power threshold.
        Constraints->GuaranteedPerformanceLimit = 0;
        Constraints->LimitReasons = 0;
        Accepted = TRUE;
        break;
    }
    case PEP_NOTIFY_PPM_PERF_SET: {
        PPEP_PPM_PERF_SET Request = (PPEP_PPM_PERF_SET)Data;
        uint32_t FrequencyKhz;
        uint32_t Minimum = Request->MinimumPerformance > Request->PerformanceTolerance ?
            Request->MinimumPerformance : Request->PerformanceTolerance;
        if (T70CpuPpmSelect(&Domains[Domain], Minimum,
            Request->DesiredPerformance, Request->MaximumPerformance, &FrequencyKhz)) {
            T70_CPU_TARGET Target = {Domain, (ULONG)FrequencyKhz};
            Accepted = NT_SUCCESS(CpuSetOppLocked(CpuPpmDevice, &Target));
        }
        break;
    }
    default:
        break;
    }
Done:
    KeReleaseSpinLock(&CpuPpmLock, OldIrql);
    return Accepted;
}

VOID CpuIoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG Code) {
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Completed = 0;
    if (Code == IOCTL_T70_CPU_SET_OPP) {
        if (InputLength != sizeof(T70_CPU_TARGET) || OutputLength) {
            Status = STATUS_INVALID_PARAMETER;
        } else {
            T70_CPU_TARGET *Target;
            Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Target), (PVOID *)&Target, NULL);
            if (NT_SUCCESS(Status))
                Status = CpuSetOpp(CpuDevice(WdfIoQueueGetDevice(Queue)), Target);
        }
    } else if (Code == IOCTL_T70_CPU_QUERY_DOMAIN) {
        if (InputLength != sizeof(ULONG)) {
            Status = STATUS_INVALID_PARAMETER;
        } else if (OutputLength < sizeof(T70_CPU_READING)) {
            Status = STATUS_BUFFER_TOO_SMALL;
        } else {
            PVOID Input;
            Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &Input, NULL);
            if (NT_SUCCESS(Status)) {
                ULONG Domain = *(PULONG)Input;
                PVOID Output;
                Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(T70_CPU_READING), &Output, NULL);
                if (NT_SUCCESS(Status)) {
                    T70_CPU_READING Reading = {0};
                    Status = CpuRead(CpuDevice(WdfIoQueueGetDevice(Queue)), Domain, &Reading, NULL);
                    if (NT_SUCCESS(Status)) {
                        RtlCopyMemory(Output, &Reading, sizeof(Reading));
                        Completed = sizeof(Reading);
                    }
                }
            }
        }
    }
    WdfRequestCompleteWithInformation(Request, Status, Completed);
}

NTSTATUS CpuAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init) {
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS Power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Power);
    Power.EvtDevicePrepareHardware = CpuPrepare;
    Power.EvtDeviceReleaseHardware = CpuRelease;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Power);

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, CPU_DEVICE);
    WDFDEVICE Device;
    NTSTATUS Status = WdfDeviceCreate(&Init, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_IO_QUEUE_CONFIG Queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchSequential);
    Queue.PowerManaged = WdfTrue;
    Queue.EvtIoDeviceControl = CpuIoControl;
    Status = WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) return Status;

    return WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_T70_CPU, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, CpuAdd);
    KeInitializeSpinLock(&CpuPpmLock);
    CpuInitializePpm();
    NTSTATUS Status = WdfDriverCreate(Driver, Path, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) return Status;

    PEP_INFORMATION PepInformation = {0};
    PEP_KERNEL_INFORMATION KernelInformation = {0};
    PepInformation.Version = PEP_INFORMATION_VERSION;
    PepInformation.Size = sizeof(PepInformation);
    PepInformation.AcceptDeviceNotification = CpuDpmNotify;
    PepInformation.AcceptProcessorNotification = CpuPpmNotify;
    KernelInformation.Version = PEP_KERNEL_INFORMATION_VERSION;
    KernelInformation.Size = sizeof(KernelInformation);
    Status = PoFxRegisterPluginEx(&PepInformation, 0, &KernelInformation);
    if (NT_SUCCESS(Status)) {
        CpuPpmKernelInformation = KernelInformation;
        /* PoFx has no PEP-unregister API; keep its callback image resident. */
        Driver->DriverUnload = NULL;
    }
    return Status;
}
