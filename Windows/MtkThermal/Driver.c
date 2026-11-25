// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <poclass.h>
#include <initguid.h>
#include <Library/MtkThermalCore.h>
#include "ThermalIo.h"

#define AUXADC_TRIGGER_SET 0x08
#define AUXADC_TRIGGER_CLEAR 0x0c
#define AUXADC_FIRST_SAMPLE 0x14
#define AUXADC_MISC 0x94
#define AUXADC_READY (1u << 12)
#define AUXADC_POWER (1u << 14)
#define AUXADC_GATE (1u << 10)
#define AUXADC_TIMEOUT_100NS 100000ULL

#define LVTS_GATE (1u << 10)
#define LVTS_BANK_BYTES 0x100
#define LVTS_MON_ENABLE (1u << 9)
#define LVTS_SENSOR_SELECT 0x40
#define LVTS_CALIBRATION_SCALE 0x48
#define LVTS_FIRST_CALIBRATION 0x54
#define LVTS_FIRST_SAMPLE 0x90
#define LVTS_CLOCK_ENABLE 0xe4
#define LVTS_SAMPLE_VALID (1u << 16)
#define THERMAL_POLL_100NS 1000000ULL
C_ASSERT(T70_SOC_SENSOR_COUNT == MTK_LVTS_SENSOR_COUNT);

typedef enum {
    AdcRegisters, ClockStatus, Calibration, LvtsRegisters, LvtsClockStatus,
    LvtsCalibration, LvtsRcCalibration, ResourceCount
} NTC_RESOURCE;
#define NTC_RESOURCE_COUNT (Calibration + 1)
static const struct {
    ULONG Address;
    ULONG Length;
    USHORT Access;
} NtcResources[ResourceCount] = {
    {0x11001000, 0x1000, CM_RESOURCE_MEMORY_READ_WRITE},
    {0x10001094, 4, CM_RESOURCE_MEMORY_READ_ONLY},
    {0x11c10198, 4, CM_RESOURCE_MEMORY_READ_ONLY},
    {0x1100b000, 0x1000, CM_RESOURCE_MEMORY_READ_ONLY},
    {0x10001090, 4, CM_RESOURCE_MEMORY_READ_ONLY},
    {0x11c101b4, 0x2c, CM_RESOURCE_MEMORY_READ_ONLY},
    {0x11c102f8, 8, CM_RESOURCE_MEMORY_READ_ONLY},
};

typedef struct {
    PULONG Region[ResourceCount];
    LONG Gain;
    LONG Offset;
    BOOLEAN EnabledPower;
    BOOLEAN Online;
    BOOLEAN LegacyLvts;
    WDFQUEUE ThermalWaitQueue;
    WDFTIMER ThermalTimer;
    WDFSPINLOCK ThermalTimerLock;
    BOOLEAN ThermalStopping;
    ULONGLONG ThermalPollDue;
} NTC_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NTC_DEVICE, NtcDevice)

typedef struct {
    ULONG Low;
    ULONG High;
    ULONGLONG Deadline;
} NTC_THERMAL_WAIT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NTC_THERMAL_WAIT, NtcThermalWait)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD NtcAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE NtcPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE NtcRelease;
EVT_WDF_DEVICE_D0_ENTRY NtcD0Entry;
EVT_WDF_DEVICE_D0_EXIT NtcD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL NtcIoControl;
EVT_WDF_TIMER NtcThermalPoll;

static BOOLEAN NtcClockReady(const NTC_DEVICE *Device) {
    return Device->Region[ClockStatus] &&
        !(READ_REGISTER_ULONG(Device->Region[ClockStatus]) & AUXADC_GATE);
}

static VOID NtcDelayMs(VOID) {
    LARGE_INTEGER Interval;
    Interval.QuadPart = -10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static ULONG NtcResourceLength(ULONG Index, BOOLEAN LegacyLvts) {
    return LegacyLvts && Index == LvtsCalibration ? sizeof(ULONG) : NtcResources[Index].Length;
}

static VOID NtcUnmap(NTC_DEVICE *Device) {
    for (ULONG Index = 0; Index < ResourceCount; Index++) {
        if (Device->Region[Index]) {
            MmUnmapIoSpace(Device->Region[Index], NtcResourceLength(Index, Device->LegacyLvts));
            Device->Region[Index] = NULL;
        }
    }
    Device->LegacyLvts = FALSE;
}

static BOOLEAN NtcResourceMatches(PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor, ULONG Index, ULONG Length) {
    return Descriptor && Descriptor->Type == CmResourceTypeMemory &&
        Descriptor->u.Memory.Start.QuadPart == NtcResources[Index].Address &&
        Descriptor->u.Memory.Length == Length &&
        Descriptor->Flags == NtcResources[Index].Access;
}

NTSTATUS NtcPrepare(WDFDEVICE Handle, WDFCMRESLIST Raw, WDFCMRESLIST Translated) {
    NTC_DEVICE *Device = NtcDevice(Handle);

    ULONG Count = WdfCmResourceListGetCount(Raw);
    BOOLEAN LegacyLvts = Count == LvtsRcCalibration;

    if (Device->Region[AdcRegisters] ||
        (Count != NTC_RESOURCE_COUNT && !LegacyLvts && Count != ResourceCount) ||
        WdfCmResourceListGetCount(Translated) != Count)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (ULONG Index = 0; Index < Count; Index++) {
        ULONG Length = NtcResourceLength(Index, LegacyLvts);
        if (!NtcResourceMatches(WdfCmResourceListGetDescriptor(Raw, Index), Index, Length) ||
            !NtcResourceMatches(WdfCmResourceListGetDescriptor(Translated, Index), Index, Length))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Device->LegacyLvts = LegacyLvts;
    for (ULONG Index = 0; Index < Count; Index++) {
        PHYSICAL_ADDRESS Address;
        ULONG Protection = Index == AdcRegisters ? PAGE_READWRITE : PAGE_READONLY;
        Address.QuadPart = NtcResources[Index].Address;
        Device->Region[Index] = MmMapIoSpaceEx(Address, NtcResourceLength(Index, LegacyLvts), Protection | PAGE_NOCACHE);
        if (!Device->Region[Index]) {
            NtcUnmap(Device);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    return STATUS_SUCCESS;
}

static VOID NtcPowerDown(NTC_DEVICE *Device) {
    Device->Online = FALSE;
    if (Device->EnabledPower && NtcClockReady(Device)) {
        PULONG Misc = Device->Region[AdcRegisters] + AUXADC_MISC / sizeof(ULONG);
        ULONG Value = READ_REGISTER_ULONG(Misc);
        if (Value != MAXULONG) WRITE_REGISTER_ULONG(Misc, Value & ~AUXADC_POWER);
    }
    Device->EnabledPower = FALSE;
}

NTSTATUS NtcRelease(WDFDEVICE Handle, WDFCMRESLIST Translated) {
    NTC_DEVICE *Device = NtcDevice(Handle);
    UNREFERENCED_PARAMETER(Translated);

    NtcPowerDown(Device);
    NtcUnmap(Device);
    return STATUS_SUCCESS;
}

static VOID LvtsRestoreClock(WDFDEVICE Handle, const NTC_DEVICE *Device) {
    if (!Device->Region[LvtsRcCalibration]) return;

    ULONG Clock = READ_REGISTER_ULONG(Device->Region[LvtsClockStatus]);
    if (Clock == MAXULONG || !(Clock & LVTS_GATE)) return;

    ACPI_EVAL_INPUT_BUFFER Input = {0};
    WDF_MEMORY_DESCRIPTOR Buffer;
    WDF_REQUEST_SEND_OPTIONS Options;
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, "LVON", sizeof(Input.MethodName));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Buffer, &Input, sizeof(Input));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options, WDF_REL_TIMEOUT_IN_SEC(1));

    // An unavailable method must leave AUXADC available. LvtsRead checks
    // the live gate and sensor configuration before using this bank.
    (VOID)WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Handle), NULL,
        IOCTL_ACPI_EVAL_METHOD, &Buffer, NULL, &Options, NULL);
}

NTSTATUS NtcD0Entry(WDFDEVICE Handle, WDF_POWER_DEVICE_STATE Previous) {
    NTC_DEVICE *Device = NtcDevice(Handle);
    PULONG Misc;
    ULONG Fuse, Value;
    UNREFERENCED_PARAMETER(Previous);

    /* ACPI owns the clock. Confirm it before touching the ADC. */
    if (!Device->Region[AdcRegisters] || !Device->Region[Calibration] || !NtcClockReady(Device))
        return STATUS_DEVICE_NOT_READY;
    Fuse = READ_REGISTER_ULONG(Device->Region[Calibration]);
    if (Fuse == MAXULONG) return STATUS_DEVICE_DATA_ERROR;
    Device->Gain = 0;
    Device->Offset = 0;
    if (Fuse & (1u << 20)) {
        Device->Gain = (LONG)((Fuse >> 10) & 0x3ffu) - 512;
        Device->Offset = (LONG)(Fuse & 0x3ffu) - 512;
    }

    Misc = Device->Region[AdcRegisters] + AUXADC_MISC / sizeof(ULONG);
    Value = READ_REGISTER_ULONG(Misc);
    if (Value == MAXULONG) return STATUS_DEVICE_DATA_ERROR;
    if (!(Value & AUXADC_POWER)) {
        WRITE_REGISTER_ULONG(Misc, Value | AUXADC_POWER);
        Device->EnabledPower = TRUE;
        NtcDelayMs();
        if (!NtcClockReady(Device)) {
            NtcPowerDown(Device);
            return STATUS_DEVICE_POWER_FAILURE;
        }
        Value = READ_REGISTER_ULONG(Misc);
        if (Value == MAXULONG || !(Value & AUXADC_POWER)) {
            NtcPowerDown(Device);
            return STATUS_DEVICE_POWER_FAILURE;
        }
    }
    LvtsRestoreClock(Handle, Device);
    Device->Online = TRUE;
    if (Device->ThermalWaitQueue) {
        WdfIoQueueStart(Device->ThermalWaitQueue);
        WdfSpinLockAcquire(Device->ThermalTimerLock);
        Device->ThermalStopping = FALSE;
        WdfSpinLockRelease(Device->ThermalTimerLock);
    }
    return STATUS_SUCCESS;
}

NTSTATUS NtcD0Exit(WDFDEVICE Handle, WDF_POWER_DEVICE_STATE Target) {
    UNREFERENCED_PARAMETER(Target);
    NTC_DEVICE *Device = NtcDevice(Handle);
    if (Device->ThermalWaitQueue) {
        WdfSpinLockAcquire(Device->ThermalTimerLock);
        Device->ThermalStopping = TRUE;
        Device->ThermalPollDue = 0;
        WdfSpinLockRelease(Device->ThermalTimerLock);
        WdfTimerStop(Device->ThermalTimer, TRUE);
        WdfIoQueuePurgeSynchronously(Device->ThermalWaitQueue);
    }
    NtcPowerDown(Device);
    return STATUS_SUCCESS;
}

static NTSTATUS NtcWaitSample(NTC_DEVICE *Device, PULONG Register, BOOLEAN Ready, PULONG Value) {
    ULONGLONG Counter;
    ULONGLONG Start = KeQueryInterruptTimePrecise(&Counter);
    do {
        ULONG Sample;
        if (!NtcClockReady(Device)) return STATUS_DEVICE_POWER_FAILURE;
        Sample = READ_REGISTER_ULONG(Register);
        if (Sample == MAXULONG) return STATUS_DEVICE_DATA_ERROR;
        if (((Sample & AUXADC_READY) != 0) == Ready) {
            *Value = Sample;
            return STATUS_SUCCESS;
        }
        NtcDelayMs();
    } while (KeQueryInterruptTimePrecise(&Counter) - Start < AUXADC_TIMEOUT_100NS);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS NtcRead(NTC_DEVICE *Device, ULONG Channel, T70_NTC_READING *Reading) {
    PULONG Adc, SampleReg;
    ULONG Trigger = 1u << Channel, Code;
    NTSTATUS Status;
    MTK_THERMAL_SAMPLE Sample;
    LONG Calibrated, ProcessedMv;
    unsigned int DeciKelvin;

    if (!Device->Online || !NtcClockReady(Device)) return STATUS_DEVICE_NOT_READY;
    Adc = Device->Region[AdcRegisters];
    SampleReg = Adc + AUXADC_FIRST_SAMPLE / sizeof(ULONG) + Channel;
    WRITE_REGISTER_ULONG(Adc + AUXADC_TRIGGER_CLEAR / sizeof(ULONG), Trigger);
    Status = NtcWaitSample(Device, SampleReg, FALSE, &Code);
    if (!NT_SUCCESS(Status)) return Status;
    WRITE_REGISTER_ULONG(Adc + AUXADC_TRIGGER_SET / sizeof(ULONG), Trigger);
    KeStallExecutionProcessor(25);
    Status = NtcWaitSample(Device, SampleReg, TRUE, &Code);
    if (NtcClockReady(Device))
        WRITE_REGISTER_ULONG(Adc + AUXADC_TRIGGER_CLEAR / sizeof(ULONG), Trigger);
    if (!NT_SUCCESS(Status)) return Status;

    Code &= 0xfffu;
    // Both DTB channels have the same lookup table. The MT6765-compatible
    // vendor ADC returns processed millivolts after gain and offset correction.
    Calibrated = 4096 * ((LONG)Code - Device->Offset) / (4096 + Device->Gain);
    ProcessedMv = Calibrated * 1500 / 4096;
    if (!MtkThermalApNtcConvert(ProcessedMv, &Sample) || Sample.Clamped ||
        !MtkThermalToAcpi(Sample.MilliCelsius, &DeciKelvin))
        return STATUS_DEVICE_DATA_ERROR;
    Reading->RawCode = Code;
    Reading->MilliCelsius = Sample.MilliCelsius;
    Reading->DeciKelvin = DeciKelvin;
    return STATUS_SUCCESS;
}

static BOOLEAN LvtsClockReady(const NTC_DEVICE *Device) {
    return Device->Region[LvtsClockStatus] &&
        !(READ_REGISTER_ULONG(Device->Region[LvtsClockStatus]) & LVTS_GATE);
}

static NTSTATUS LvtsSample(NTC_DEVICE *Device, ULONG Index, const MTK_LVTS_CALIBRATION *Trim,
                           T70_NTC_READING *Reading) {
    PULONG Bank = Device->Region[LvtsRegisters] +
        (Index / 4) * (LVTS_BANK_BYTES / sizeof(ULONG));
    ULONG Slot = Index % 4;
    PULONG CalibrationRegister = Bank + LVTS_FIRST_CALIBRATION / sizeof(ULONG) + Slot;
    ULONG Monitor, CalibrationCode, Sample;
    int MilliCelsius;
    unsigned int DeciKelvin;

    if (!LvtsClockReady(Device)) return STATUS_DEVICE_NOT_READY;
    Monitor = READ_REGISTER_ULONG(Bank);
    if (Monitor == MAXULONG || (Monitor & (LVTS_MON_ENABLE | (1u << Slot))) !=
            (LVTS_MON_ENABLE | (1u << Slot)) ||
        READ_REGISTER_ULONG(Bank + LVTS_CLOCK_ENABLE / 4) != 1 ||
        READ_REGISTER_ULONG(Bank + LVTS_SENSOR_SELECT / 4) != 0x13121110 ||
        READ_REGISTER_ULONG(Bank + LVTS_CALIBRATION_SCALE / 4) != 0x300)
        return STATUS_DEVICE_NOT_READY;
    CalibrationCode = READ_REGISTER_ULONG(CalibrationRegister);
    // MT6789 uses the factory count_r for every active slot (feature bit 0
    // in mt6789_lvts_data selects it in the stock set_calibration_data_v4).
    if (!CalibrationCode || CalibrationCode > 0xffffff ||
        (!Device->LegacyLvts && CalibrationCode != Trim->CountR[Index]))
        return STATUS_DEVICE_DATA_ERROR;
    Sample = READ_REGISTER_ULONG(Bank + LVTS_FIRST_SAMPLE / 4 + Slot);
    if (!LvtsClockReady(Device) || READ_REGISTER_ULONG(Bank) != Monitor ||
        READ_REGISTER_ULONG(CalibrationRegister) != CalibrationCode ||
        !(Sample & LVTS_SAMPLE_VALID) || Sample == MAXULONG)
        return STATUS_DEVICE_NOT_READY;
    Sample &= 0xffffu;
    if (!MtkThermalLvtsConvert(Sample, Trim->Golden, &MilliCelsius) ||
        !MtkThermalToAcpi(MilliCelsius, &DeciKelvin))
        return STATUS_DEVICE_DATA_ERROR;
    *Reading = (T70_NTC_READING){Sample, MilliCelsius, DeciKelvin};
    return STATUS_SUCCESS;
}

static BOOLEAN LvtsReadCalibration(const NTC_DEVICE *Device, MTK_LVTS_CALIBRATION *Trim) {
    unsigned int Words[MTK_LVTS_EFUSE_WORDS];
    if (Device->LegacyLvts) {
        ULONG Word = READ_REGISTER_ULONG(Device->Region[LvtsCalibration]);
        if (Word == MAXULONG || !(Word >> 24)) return FALSE;
        Trim->Golden = Word >> 24;
        return TRUE;
    }

    for (ULONG Index = 0; Index < MTK_LVTS_EFUSE_DATA1_WORDS; Index++) {
        Words[Index] = READ_REGISTER_ULONG(Device->Region[LvtsCalibration] + Index);
    }
    Words[MTK_LVTS_EFUSE_DATA1_WORDS] = READ_REGISTER_ULONG(Device->Region[LvtsRcCalibration]);
    return MtkThermalLvtsCalibration(Words, Trim) != 0;
}

static NTSTATUS LvtsRead(NTC_DEVICE *Device, ULONG SensorId, T70_NTC_READING *Reading) {
    ULONG First = SensorId ? SensorId - 1 : 0;
    ULONG End = SensorId ? SensorId : T70_SOC_SENSOR_COUNT;
    MTK_LVTS_CALIBRATION Trim;
    T70_NTC_READING Best;

    if (!Device->Online) return STATUS_DEVICE_NOT_READY;
    if (!Device->Region[LvtsRegisters]) return STATUS_NOT_SUPPORTED;
    if (!LvtsClockReady(Device)) return STATUS_DEVICE_NOT_READY;
    if (!LvtsReadCalibration(Device, &Trim)) return STATUS_DEVICE_DATA_ERROR;
    for (ULONG Index = First; Index < End; Index++) {
        T70_NTC_READING Sample;
        NTSTATUS Status = LvtsSample(Device, Index, &Trim, &Sample);
        if (!NT_SUCCESS(Status)) return Status;
        if (Index == First || Sample.MilliCelsius > Best.MilliCelsius) Best = Sample;
    }
    *Reading = Best;
    return STATUS_SUCCESS;
}

static ULONGLONG NtcThermalNow(VOID) {
    ULONGLONG Counter;
    return KeQueryInterruptTimePrecise(&Counter);
}

static BOOLEAN NtcThermalDue(const NTC_THERMAL_WAIT *Wait, ULONG Temperature, ULONGLONG Now) {
    return Temperature < Wait->Low || Temperature > Wait->High || Now >= Wait->Deadline;
}

static VOID NtcScheduleThermalPoll(NTC_DEVICE *Device, ULONGLONG Deadline) {
    WdfSpinLockAcquire(Device->ThermalTimerLock);
    if (!Device->ThermalStopping) {
        ULONGLONG Now = NtcThermalNow();
        ULONGLONG Due = min(Deadline, Now + THERMAL_POLL_100NS);
        if (!Device->ThermalPollDue || Due < Device->ThermalPollDue) {
            Device->ThermalPollDue = Due;
            WdfTimerStart(Device->ThermalTimer, Due > Now ? -(LONGLONG)(Due - Now) : -1);
        }
    }
    WdfSpinLockRelease(Device->ThermalTimerLock);
}

static VOID NtcThermalComplete(WDFREQUEST Request, NTSTATUS Status, ULONG Temperature) {
    PULONG Output;
    if (NT_SUCCESS(Status)) {
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID *)&Output, NULL);
        if (NT_SUCCESS(Status)) *Output = Temperature;
    }
    WdfRequestCompleteWithInformation(Request, Status, NT_SUCCESS(Status) ? sizeof(Temperature) : 0);
}

VOID NtcThermalPoll(WDFTIMER Timer) {
    NTC_DEVICE *Device = NtcDevice(WdfTimerGetParentObject(Timer));
    WDFREQUEST Found, Request;
    T70_NTC_READING Reading = {0};
    NTSTATUS Status;
    BOOLEAN Stopping;

    WdfSpinLockAcquire(Device->ThermalTimerLock);
    Device->ThermalPollDue = 0;
    Stopping = Device->ThermalStopping;
    WdfSpinLockRelease(Device->ThermalTimerLock);
    if (Stopping || !NT_SUCCESS(WdfIoQueueFindRequest(Device->ThermalWaitQueue, NULL, NULL, NULL, &Found)))
        return;

    NTC_THERMAL_WAIT Wait = *NtcThermalWait(Found);
    Status = LvtsRead(Device, 0, &Reading);
    if (!NT_SUCCESS(Status) || NtcThermalDue(&Wait, Reading.DeciKelvin, NtcThermalNow())) {
        // Cancellation can remove a request between Find and Retrieve.
        if (NT_SUCCESS(WdfIoQueueRetrieveFoundRequest(Device->ThermalWaitQueue, Found, &Request)))
            NtcThermalComplete(Request, Status, Reading.DeciKelvin);
    } else {
        NtcScheduleThermalPoll(Device, Wait.Deadline);
    }
    WdfObjectDereference(Found);
}

static VOID NtcThermalRead(NTC_DEVICE *Device, WDFREQUEST Request, size_t OutputLength, size_t InputLength) {
    PTHERMAL_WAIT_READ Input;
    NTC_THERMAL_WAIT Wait;
    T70_NTC_READING Reading = {0};
    NTSTATUS Status;
    ULONG Pending;

    if (InputLength < sizeof(*Input) || OutputLength < sizeof(ULONG)) {
        NtcThermalComplete(Request, STATUS_BUFFER_TOO_SMALL, 0);
        return;
    }
    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID *)&Input, NULL);
    if (!NT_SUCCESS(Status) || Input->LowTemperature > Input->HighTemperature) {
        NtcThermalComplete(Request, NT_SUCCESS(Status) ? STATUS_INVALID_PARAMETER : Status, 0);
        return;
    }
    Wait.Low = Input->LowTemperature;
    Wait.High = Input->HighTemperature;
    Wait.Deadline = Input->Timeout == THERMAL_WAIT_READ_TIMEOUT_NONE ? MAXULONGLONG :
        NtcThermalNow() + (ULONGLONG)Input->Timeout * 10000;
    Status = LvtsRead(Device, 0, &Reading);
    if (!NT_SUCCESS(Status) || NtcThermalDue(&Wait, Reading.DeciKelvin, NtcThermalNow())) {
        NtcThermalComplete(Request, Status, Reading.DeciKelvin);
        return;
    }

    // The default queue is sequential. A threshold wait moves to its own
    // queue so ACPI can still ask for an immediate sample while it is pending.
    WdfIoQueueGetState(Device->ThermalWaitQueue, &Pending, NULL);
    if (Pending) {
        NtcThermalComplete(Request, STATUS_DEVICE_BUSY, 0);
        return;
    }
    *NtcThermalWait(Request) = Wait;
    Status = WdfRequestForwardToIoQueue(Request, Device->ThermalWaitQueue);
    if (!NT_SUCCESS(Status)) {
        NtcThermalComplete(Request, Status, 0);
        return;
    }
    NtcScheduleThermalPoll(Device, Wait.Deadline);
}

VOID NtcIoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG Code) {
    NTSTATUS Status;
    ULONG Channel;
    PULONG Input;
    T70_NTC_READING Reading;
    T70_NTC_READING *Output;

    if (Code == IOCTL_THERMAL_READ_TEMPERATURE) {
        NtcThermalRead(NtcDevice(WdfIoQueueGetDevice(Queue)), Request, OutputLength, InputLength);
        return;
    }

    if (Code != IOCTL_T70_READ_NTC && Code != IOCTL_T70_READ_SOC_TEMP) {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    } else if (InputLength != sizeof(ULONG)) {
        Status = STATUS_INVALID_PARAMETER;
    } else if (OutputLength < sizeof(Reading)) {
        Status = STATUS_BUFFER_TOO_SMALL;
    } else {
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID *)&Input, NULL);
        if (NT_SUCCESS(Status)) {
            Channel = *Input;
            ULONG MaxChannel = Code == IOCTL_T70_READ_NTC ? 1 : T70_SOC_SENSOR_COUNT;
            Status = Channel > MaxChannel ? STATUS_INVALID_PARAMETER :
                WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID *)&Output, NULL);
            if (NT_SUCCESS(Status)) {
                NTC_DEVICE *Device = NtcDevice(WdfIoQueueGetDevice(Queue));
                Status = Code == IOCTL_T70_READ_NTC ? NtcRead(Device, Channel, &Reading) :
                    LvtsRead(Device, Channel, &Reading);
                if (NT_SUCCESS(Status)) *Output = Reading;
            }
        }
    }
    WdfRequestCompleteWithInformation(Request, Status, NT_SUCCESS(Status) ? sizeof(Reading) : 0);
}

NTSTATUS NtcAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init) {
    WDF_PNPPOWER_EVENT_CALLBACKS Power;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG Queue;
    WDF_TIMER_CONFIG Timer;
    WDFDEVICE Device;
    NTC_DEVICE *State;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Power);
    Power.EvtDevicePrepareHardware = NtcPrepare;
    Power.EvtDeviceReleaseHardware = NtcRelease;
    Power.EvtDeviceD0Entry = NtcD0Entry;
    Power.EvtDeviceD0Exit = NtcD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Power);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, NTC_THERMAL_WAIT);
    WdfDeviceInitSetRequestAttributes(Init, &Attributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, NTC_DEVICE);
    Status = WdfDeviceCreate(&Init, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchSequential);
    Queue.PowerManaged = WdfTrue;
    Queue.EvtIoDeviceControl = NtcIoControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfIoQueueCreate(Device, &Queue, &Attributes, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) return Status;

    State = NtcDevice(Device);
    State->ThermalStopping = TRUE;
    WDF_IO_QUEUE_CONFIG_INIT(&Queue, WdfIoQueueDispatchManual);
    Queue.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, &State->ThermalWaitQueue);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfSpinLockCreate(&Attributes, &State->ThermalTimerLock);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_TIMER_CONFIG_INIT(&Timer, NtcThermalPoll);
    Timer.AutomaticSerialization = FALSE;
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfTimerCreate(&Timer, &Attributes, &State->ThermalTimer);
    if (!NT_SUCCESS(Status)) return Status;

    return WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_T70_NTC, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, NtcAdd);
    return WdfDriverCreate(Driver, Path, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
