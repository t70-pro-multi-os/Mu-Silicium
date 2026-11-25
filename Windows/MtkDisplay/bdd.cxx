/******************************Module*Header*******************************\
* Module Name: bdd.cxx
*
* Basic Display Driver functions implementation
*
*
* Copyright (c) 2010 Microsoft Corporation
\**************************************************************************/


#include "bdd.hxx"

#pragma code_seg("PAGE")


BASIC_DISPLAY_DRIVER::BASIC_DISPLAY_DRIVER(_In_ DEVICE_OBJECT* pPhysicalDeviceObject) : m_pPhysicalDevice(pPhysicalDeviceObject),
                                                                                        m_MonitorPowerState(PowerDeviceD0),
                                                                                        m_AdapterPowerState(PowerDeviceD0)
{
    PAGED_CODE();
    *((UINT*)&m_Flags) = 0;
    m_Flags._LastFlag = TRUE;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_StartInfo, sizeof(m_StartInfo));
    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));
    RtlZeroMemory(&m_Handoff, sizeof(m_Handoff));
    KeInitializeMutex(&m_PresentMutex,0);
    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;
    m_PowerTransition = 1;
    m_LastResumeStatus = STATUS_DEVICE_NOT_READY;


    for (UINT i=0;i<MAX_VIEWS;i++)
    {
        m_HardwareBlt[i].Initialize(this,i);
    }
}

BASIC_DISPLAY_DRIVER::~BASIC_DISPLAY_DRIVER()
{
    PAGED_CODE();


    CleanUp();
}


NTSTATUS BASIC_DISPLAY_DRIVER::StartDevice(_In_  DXGK_START_INFO*   pDxgkStartInfo,
                                           _In_  DXGKRNL_INTERFACE* pDxgkInterface,
                                           _Out_ ULONG*             pNumberOfViews,
                                           _Out_ ULONG*             pNumberOfChildren)
{
    PAGED_CODE();
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) return STATUS_DEVICE_BUSY;
    if (!pDxgkStartInfo || !pDxgkInterface || !pNumberOfViews || !pNumberOfChildren ||
        m_Flags.DriverStarted || !pDxgkInterface->DxgkCbGetDeviceInformation ||
        !pDxgkInterface->DxgkCbEvalAcpiMethod || !pDxgkInterface->DxgkCbAcquirePostDisplayOwnership)
        return STATUS_INVALID_PARAMETER;
    *pNumberOfViews = *pNumberOfChildren = 0;
    RtlCopyMemory(&m_StartInfo,pDxgkStartInfo,sizeof(m_StartInfo));
    RtlCopyMemory(&m_DxgkInterface,pDxgkInterface,sizeof(m_DxgkInterface));
    RtlZeroMemory(m_CurrentModes,sizeof(m_CurrentModes));
    NTSTATUS status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle,&m_DeviceInfo);
    if (!NT_SUCCESS(status)) return status;
    status = CheckHardware();
    if (!NT_SUCCESS(status)) return status;
    MTK_FB_HANDOFF first = m_Handoff;
    DXGK_DISPLAY_INFORMATION *d = &m_CurrentModes[0].DispInfo;
    status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle,d);
    if (!NT_SUCCESS(status)) return status;
    status = CheckHardware();
    if (!NT_SUCCESS(status) || memcmp(&first,&m_Handoff,sizeof(first)) ||
        d->PhysicAddress.QuadPart != m_Handoff.Base || d->Width != m_Handoff.Width ||
        d->Height != m_Handoff.Height || d->Pitch != m_Handoff.Pitch ||
        (d->ColorFormat != D3DDDIFMT_X8R8G8B8 && d->ColorFormat != D3DDDIFMT_A8R8G8B8) || d->TargetId != 0)
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    status = MapFrameBuffer(d->PhysicAddress,(ULONG)((SIZE_T)d->Pitch*d->Height),&m_CurrentModes[0].FrameBuffer.Ptr);
    if (!NT_SUCCESS(status)) return status;
    m_CurrentModes[0].Flags.FrameBufferIsActive = TRUE;
    m_CurrentModes[0].Flags.SourceNotVisible = TRUE;
    RtlZeroMemory(m_CurrentModes[0].FrameBuffer.Ptr,(SIZE_T)d->Pitch*d->Height);
    KeMemoryBarrier();
    m_CurrentModes[0].Flags.IsInternal = TRUE;
    m_CurrentModes[0].Rotation = D3DKMDT_VPPR_IDENTITY;
    m_CurrentModes[0].Scaling = D3DKMDT_VPPS_IDENTITY;
    m_CurrentModes[0].SrcModeWidth = d->Width;
    m_CurrentModes[0].SrcModeHeight = d->Height;
    m_AdapterPowerState = m_MonitorPowerState = PowerDeviceD0;
    m_Flags.DriverStarted = TRUE;
    m_LastResumeStatus = STATUS_SUCCESS;
    InterlockedExchange(&m_PowerTransition,0);
    *pNumberOfViews = MAX_VIEWS;
    *pNumberOfChildren = MAX_CHILDREN;
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::StopDevice(VOID)
{
    PAGED_CODE();
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) return STATUS_DEVICE_BUSY;

    InterlockedExchange(&m_PowerTransition,1);
    CleanUp();
    m_LastResumeStatus = STATUS_DEVICE_NOT_READY;
    m_Flags.DriverStarted = FALSE;

    return STATUS_SUCCESS;
}

VOID BASIC_DISPLAY_DRIVER::CleanUp(BOOLEAN PreserveVisibility)
{
    PAGED_CODE();

    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;
    for (UINT Source = 0; Source < MAX_VIEWS; ++Source)
    {
        m_CurrentModes[Source].Flags.FrameBufferIsActive = FALSE;
        if (!PreserveVisibility) m_CurrentModes[Source].Flags.SourceNotVisible = TRUE;
        if (m_CurrentModes[Source].FrameBuffer.Ptr)
        {
            UnmapFrameBuffer(m_CurrentModes[Source].FrameBuffer.Ptr, m_CurrentModes[Source].DispInfo.Height * m_CurrentModes[Source].DispInfo.Pitch);
            m_CurrentModes[Source].FrameBuffer.Ptr = NULL;
            m_CurrentModes[Source].Flags.FrameBufferIsActive = FALSE;
        }
    }

}


NTSTATUS BASIC_DISPLAY_DRIVER::DispatchIoRequest(_In_  ULONG                 VidPnSourceId,
                                                 _In_  VIDEO_REQUEST_PACKET* pVideoRequestPacket)
{
    PAGED_CODE();

    BDD_ASSERT(pVideoRequestPacket != NULL);
    BDD_ASSERT(VidPnSourceId < MAX_VIEWS);

    return STATUS_NOT_IMPLEMENTED;
}

// Reacquire POST on every adapter D0 notification, including hibernate resume.
// Never use a stale mapping or silently change an established VidPN geometry.
NTSTATUS BASIC_DISPLAY_DRIVER::ReacquireFrameBuffer(BOOLEAN MapBuffer)
{
    if (!IsDriverActive() || !m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership)
        return STATUS_DEVICE_NOT_READY;
    NTSTATUS status=CheckHardware();
    if (!NT_SUCCESS(status)) return status;
    MTK_FB_HANDOFF first=m_Handoff;
    DXGK_DISPLAY_INFORMATION d={};
    status=m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle,&d);
    if (!NT_SUCCESS(status)) return status;
    status=CheckHardware();
    auto m=&m_CurrentModes[0];
    if (!NT_SUCCESS(status) || memcmp(&first,&m_Handoff,sizeof(first)) ||
        d.PhysicAddress.QuadPart!=m_Handoff.Base || d.Width!=m_Handoff.Width ||
        d.Height!=m_Handoff.Height || d.Pitch!=m_Handoff.Pitch || d.TargetId!=0 ||
        (d.ColorFormat!=D3DDDIFMT_X8R8G8B8 && d.ColorFormat!=D3DDDIFMT_A8R8G8B8) ||
        d.Width!=m->DispInfo.Width || d.Height!=m->DispInfo.Height)
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    if (!MapBuffer) return STATUS_SUCCESS;
    PVOID mapped=NULL;
    status=MapFrameBuffer(d.PhysicAddress,(ULONG)((SIZE_T)d.Pitch*d.Height),&mapped);
    if (!NT_SUCCESS(status)) return status;
    m->DispInfo=d; m->FrameBuffer.Ptr=mapped;
    m->Flags.FullscreenPresent=TRUE;
    RtlZeroMemory(mapped,(SIZE_T)d.Pitch*d.Height);
    KeMemoryBarrier(); m->Flags.FrameBufferIsActive=TRUE;
    return STATUS_SUCCESS;
}
NTSTATUS BASIC_DISPLAY_DRIVER::SetPowerState(_In_ ULONG HardwareUid,
                                             _In_ DEVICE_POWER_STATE DevicePowerState,
                                             _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ActionType);
    if ((HardwareUid!=DISPLAY_ADAPTER_HW_ID && HardwareUid!=0) ||
        DevicePowerState<PowerDeviceD0 || DevicePowerState>PowerDeviceD3)
        return STATUS_INVALID_PARAMETER;
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) {
        InterlockedExchange(&m_PowerTransition,1);
        m_LastResumeStatus=STATUS_DEVICE_BUSY;
        return STATUS_SUCCESS;
    }
    // This DOD owns inherited scanout only. Logical low power blanks and gates
    // CPU access; panel/clock rail management remains the platform's job.
    if (DevicePowerState!=PowerDeviceD0 && IsScanoutReady()) BlackOutScreen(0);
    InterlockedExchange(&m_PowerTransition,1);
    if (HardwareUid==DISPLAY_ADAPTER_HW_ID) m_AdapterPowerState=DevicePowerState;
    else m_MonitorPowerState=DevicePowerState;
    CleanUp(TRUE);
    m_LastResumeStatus=STATUS_DEVICE_NOT_READY;
    if (DevicePowerState==PowerDeviceD0 && m_AdapterPowerState==PowerDeviceD0 && IsDriverActive()) {
        m_LastResumeStatus=ReacquireFrameBuffer(m_MonitorPowerState==PowerDeviceD0);
        if (NT_SUCCESS(m_LastResumeStatus) && m_MonitorPowerState==PowerDeviceD0)
            InterlockedExchange(&m_PowerTransition,0);
        else if (!NT_SUCCESS(m_LastResumeStatus))
            DbgPrintEx(DPFLTR_IHVVIDEO_ID,DPFLTR_ERROR_LEVEL,
                "MtkDisplay: D0 scanout revalidation failed: 0x%08x\n",m_LastResumeStatus);
    }
    // WDDM requires power notifications to succeed. Failed D0 validation keeps
    // all display writes gated and records the actual error separately.
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR* pChildRelations,
                                                   _In_                             ULONG                  ChildRelationsSize)
{
    PAGED_CODE();
    if (!pChildRelations || ChildRelationsSize < 2*sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(pChildRelations,ChildRelationsSize);
    pChildRelations[0].ChildDeviceType = TypeVideoOutput;
    pChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
    pChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
    pChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    pChildRelations[0].ChildUid = 0;
    pChildRelations[0].AcpiUid = 0;
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS* pChildStatus,
                                                _In_    BOOLEAN            NonDestructiveOnly)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    if (!pChildStatus || pChildStatus->ChildUid != 0) return STATUS_INVALID_PARAMETER;

    switch (pChildStatus->Type)
    {
        case StatusConnection:
        {
            // HpdAwarenessInterruptible was reported since HpdAwarenessNone is deprecated.
            // However, BDD has no knowledge of HotPlug events, so just always return connected.
            pChildStatus->HotPlug.Connected = IsDriverActive();
            return STATUS_SUCCESS;
        }

        case StatusRotation:
        {
            // D3DKMDT_MOA_NONE was reported, so this should never be called
            BDD_LOG_ERROR0("Child status being queried for StatusRotation even though D3DKMDT_MOA_NONE was reported");
            return STATUS_INVALID_PARAMETER;
        }

        default:
        {
            BDD_LOG_WARNING1("Unknown pChildStatus->Type (0x%I64x) requested.", pChildStatus->Type);
            return STATUS_NOT_SUPPORTED;
        }
    }
}

// EDID retrieval
NTSTATUS BASIC_DISPLAY_DRIVER::QueryDeviceDescriptor(_In_    ULONG                   ChildUid,
                                                     _Inout_ DXGK_DEVICE_DESCRIPTOR* pDeviceDescriptor)
{
    PAGED_CODE();

    if (!pDeviceDescriptor || ChildUid != 0) return STATUS_INVALID_PARAMETER;

    // If we haven't successfully retrieved an EDID yet (invalid ones are ok, so long as it was retrieved)
    if (!m_Flags.EDID_Attempted)
    {
        GetEdid(ChildUid);
    }

    if (!m_Flags.EDID_Retrieved || !m_Flags.EDID_ValidHeader || !m_Flags.EDID_ValidChecksum)
    {
        // Report no EDID if a valid one wasn't retrieved
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    else if (pDeviceDescriptor->DescriptorOffset == 0)
    {
        // Only the base block is supported
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer,
                      m_EDIDs[ChildUid],
                      min(pDeviceDescriptor->DescriptorLength, EDID_V1_BLOCK_SIZE));

        return STATUS_SUCCESS;
    }
    else
    {
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO* pQueryAdapterInfo)
{
    PAGED_CODE();

    if (!pQueryAdapterInfo || !pQueryAdapterInfo->pOutputData) return STATUS_INVALID_PARAMETER;

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_DRIVERCAPS:
        {
            if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
            {
                BDD_LOG_ERROR2("pQueryAdapterInfo->OutputDataSize (0x%I64x) is smaller than sizeof(DXGK_DRIVERCAPS) (0x%I64x)", pQueryAdapterInfo->OutputDataSize, sizeof(DXGK_DRIVERCAPS));
                return STATUS_BUFFER_TOO_SMALL;
            }

            DXGK_DRIVERCAPS* pDriverCaps = (DXGK_DRIVERCAPS*)pQueryAdapterInfo->pOutputData;

            // Nearly all fields must be initialized to zero, so zero out to start and then change those that are non-zero.
            // Fields are zero since BDD is Display-Only and therefore does not support any of the render related fields.
            // It also doesn't support hardware interrupts, gamma ramps, etc.
            RtlZeroMemory(pDriverCaps, sizeof(DXGK_DRIVERCAPS));

            pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_2;
            pDriverCaps->HighestAcceptableAddress.QuadPart = -1;

            pDriverCaps->SupportNonVGA = TRUE;
            pDriverCaps->SupportSmoothRotation = TRUE;

            return STATUS_SUCCESS;
        }

        case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        {
            DXGK_DISPLAY_DRIVERCAPS_EXTENSION* pDriverDisplayCaps;

            if (pQueryAdapterInfo->OutputDataSize < sizeof(*pDriverDisplayCaps))
            {
                BDD_LOG_ERROR2("pQueryAdapterInfo->OutputDataSize (0x%I64x) is smaller than sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION) (0x%I64x)",
                               pQueryAdapterInfo->OutputDataSize,
                               sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION));

                return STATUS_INVALID_PARAMETER;
            }

            pDriverDisplayCaps = (DXGK_DISPLAY_DRIVERCAPS_EXTENSION*)pQueryAdapterInfo->pOutputData;

            // Reset all caps values
            RtlZeroMemory(pDriverDisplayCaps, pQueryAdapterInfo->OutputDataSize);

            // We claim to support virtual display mode.
            pDriverDisplayCaps->VirtualModeSupport = 0;

            return STATUS_SUCCESS;
        }

        default:
        {
            // BDD does not need to support any other adapter information types
            BDD_LOG_WARNING1("Unknown QueryAdapterInfo Type (0x%I64x) requested", pQueryAdapterInfo->Type);
            return STATUS_NOT_SUPPORTED;
        }
    }
}


NTSTATUS BASIC_DISPLAY_DRIVER::CheckHardware()
{
    PAGED_CODE();
    ACPI_EVAL_INPUT_BUFFER_COMPLEX in = {};
    in.Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    in.Size = sizeof(in);
    in.MethodNameAsUlong = ACPI_METHOD_HARDWARE_ID;
    union { ULONG64 Align; BYTE Bytes[128]; } storage = {};
    auto out = reinterpret_cast<ACPI_EVAL_OUTPUT_BUFFER*>(storage.Bytes);
    NTSTATUS status = m_DxgkInterface.DxgkCbEvalAcpiMethod(m_DxgkInterface.DeviceHandle,
        DISPLAY_ADAPTER_HW_ID,&in,sizeof(in),out,sizeof(storage));
    const ULONG head = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data);
    if (!NT_SUCCESS(status)) return status;
    if (out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || out->Count != 1 ||
        out->Length != head+9 || out->Argument[0].Type != ACPI_METHOD_ARGUMENT_STRING ||
        out->Argument[0].DataLength != 9 || memcmp(out->Argument[0].Data,"HANA678C",9))
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    RtlZeroMemory(storage.Bytes,sizeof(storage));
    in.MethodNameAsUlong = 'IBFD';
    status = m_DxgkInterface.DxgkCbEvalAcpiMethod(m_DxgkInterface.DeviceHandle,
        DISPLAY_ADAPTER_HW_ID,&in,sizeof(in),out,sizeof(storage));
    if (!NT_SUCCESS(status)) return status;
    MTK_FB_HANDOFF h = {};
    if (out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || out->Count != 1 ||
        out->Length != head+sizeof(h) || out->Argument[0].Type != ACPI_METHOD_ARGUMENT_BUFFER ||
        out->Argument[0].DataLength != sizeof(h)) return STATUS_ACPI_INVALID_DATA;
    RtlCopyMemory(&h,out->Argument[0].Data,sizeof(h));
    if (!MtkFbValidateHandoff(&h)) return STATUS_DEVICE_NOT_READY;
    m_Handoff = h;
    return STATUS_SUCCESS;
}

// Even though Sample Basic Display Driver does not support hardware cursors, and reports such
// in QueryAdapterInfo. This function can still be called to set the pointer to not visible
NTSTATUS BASIC_DISPLAY_DRIVER::SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION* pSetPointerPosition)
{
    PAGED_CODE();

    BDD_ASSERT(pSetPointerPosition != NULL);
    BDD_ASSERT(pSetPointerPosition->VidPnSourceId < MAX_VIEWS);

    if (!(pSetPointerPosition->Flags.Visible))
    {
        return STATUS_SUCCESS;
    }
    else
    {
        BDD_LOG_ASSERTION0("SetPointerPosition should never be called to set the pointer to visible since BDD doesn't support hardware cursors.");
        return STATUS_UNSUCCESSFUL;
    }
}

// Basic Sample Display Driver does not support hardware cursors, and reports such
// in QueryAdapterInfo. Therefore this function should never be called.
NTSTATUS BASIC_DISPLAY_DRIVER::SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE* pSetPointerShape)
{
    PAGED_CODE();

    BDD_ASSERT(pSetPointerShape != NULL);
    BDD_LOG_ASSERTION0("SetPointerShape should never be called since BDD doesn't support hardware cursors.");

    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS BASIC_DISPLAY_DRIVER::PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY* pPresentDisplayOnly)
{
    PAGED_CODE();
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) return STATUS_DEVICE_BUSY;
    if (!pPresentDisplayOnly || pPresentDisplayOnly->VidPnSourceId != 0 ||
        !IsDriverActive() || pPresentDisplayOnly->BytesPerPixel != 4 ||
        pPresentDisplayOnly->Flags.Value & ~1U) return STATUS_INVALID_PARAMETER;
    CURRENT_BDD_MODE *m = &m_CurrentModes[0];
    if (!IsScanoutReady()) return STATUS_DEVICE_NOT_READY;
    if (m->Flags.SourceNotVisible) return STATUS_SUCCESS;
    auto rotation = pPresentDisplayOnly->Flags.Rotate ? m->Rotation : D3DKMDT_VPPR_IDENTITY;
    NTSTATUS status = m_HardwareBlt[0].ExecutePresentDisplayOnly((BYTE*)m->FrameBuffer.Ptr,32,
        (BYTE*)pPresentDisplayOnly->pSource,4,pPresentDisplayOnly->Pitch,
        pPresentDisplayOnly->NumMoves,pPresentDisplayOnly->pMoves,
        pPresentDisplayOnly->NumDirtyRects,pPresentDisplayOnly->pDirtyRect,rotation);
    if (NT_SUCCESS(status)) m->Flags.FullscreenPresent = FALSE;
    return status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::StopDeviceAndReleasePostDisplayOwnership(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                        _Out_ DXGK_DISPLAY_INFORMATION*      pDisplayInfo)
{
    PAGED_CODE();
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) return STATUS_DEVICE_BUSY;
    if (TargetId != 0 || !pDisplayInfo || !IsDriverActive()) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(pDisplayInfo,sizeof(*pDisplayInfo));
    if (!IsScanoutReady()) return STATUS_DEVICE_NOT_READY;
    BlackOutScreen(0);
    *pDisplayInfo = m_CurrentModes[0].DispInfo;
    return StopDevice();
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY* pVidPnHWCaps)
{
    PAGED_CODE();

    BDD_ASSERT(pVidPnHWCaps != NULL);
    BDD_ASSERT(pVidPnHWCaps->SourceId < MAX_VIEWS);
    BDD_ASSERT(pVidPnHWCaps->TargetId < MAX_CHILDREN);

    pVidPnHWCaps->VidPnHWCaps.DriverRotation             = 1; // BDD does rotation in software
    pVidPnHWCaps->VidPnHWCaps.DriverScaling              = 0; // BDD does not support scaling
    pVidPnHWCaps->VidPnHWCaps.DriverCloning              = 0; // BDD does not support clone
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert         = 0; // BDD does color conversions in software
    pVidPnHWCaps->VidPnHWCaps.DriverLinkedAdapaterOutput = 0; // BDD does not support linked adapters
    pVidPnHWCaps->VidPnHWCaps.DriverRemoteDisplay        = 0; // BDD does not support remote displays

    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::GetEdid(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId)
{
    PAGED_CODE();

    BDD_ASSERT_CHK(!m_Flags.EDID_Attempted);

    NTSTATUS Status = STATUS_SUCCESS;
    RtlZeroMemory(m_EDIDs[TargetId], sizeof(m_EDIDs[TargetId]));


    m_Flags.EDID_Attempted = TRUE;

    return Status;
}

VOID BASIC_DISPLAY_DRIVER::BlackOutScreen(D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();
    if (SourceId >= MAX_VIEWS) return;
    auto m = &m_CurrentModes[SourceId];
    if (IsScanoutReady() && m->Flags.FrameBufferIsActive && m->FrameBuffer.Ptr) {
        RtlZeroMemory(m->FrameBuffer.Ptr,(SIZE_T)m->DispInfo.Pitch*m->DispInfo.Height);
        KeMemoryBarrier();
    }
}

#pragma code_seg(push)
#pragma code_seg()
D3DDDI_VIDEO_PRESENT_SOURCE_ID BASIC_DISPLAY_DRIVER::FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, BOOLEAN DefaultToZero)
{
    UNREFERENCED_PARAMETER(TargetId);
    BDD_ASSERT_CHK(TargetId < MAX_CHILDREN);

    for (UINT SourceId = 0; SourceId < MAX_VIEWS; ++SourceId)
    {
        if (m_CurrentModes[SourceId].FrameBuffer.Ptr != NULL)
        {
            return SourceId;
        }
    }

    return DefaultToZero ? 0 : D3DDDI_ID_UNINITIALIZED;
}

VOID BASIC_DISPLAY_DRIVER::DpcRoutine(VOID)
{
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
}

BOOLEAN BASIC_DISPLAY_DRIVER::InterruptRoutine(_In_  ULONG MessageNumber)
{
    UNREFERENCED_PARAMETER(MessageNumber);

    // BDD cannot handle interrupts
    return FALSE;
}

VOID BASIC_DISPLAY_DRIVER::ResetDevice(VOID)
{
}

// Must be Non-Paged, as it sets up the display for a bugcheck
NTSTATUS BASIC_DISPLAY_DRIVER::SystemDisplayEnable(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   _In_  PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                                   _Out_ UINT* pWidth,
                                                   _Out_ UINT* pHeight,
                                                   _Out_ D3DDDIFORMAT* pColorFormat)
{
    UNREFERENCED_PARAMETER(Flags);
    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;
    if (!pWidth || !pHeight || !pColorFormat || (TargetId != 0 && TargetId != D3DDDI_ID_UNINITIALIZED) ||
        !IsScanoutReady()) return STATUS_DEVICE_NOT_READY;
    m_SystemDisplaySourceId = 0;
    *pWidth = m_CurrentModes[0].DispInfo.Width;
    *pHeight = m_CurrentModes[0].DispInfo.Height;
    *pColorFormat = m_CurrentModes[0].DispInfo.ColorFormat;
    return STATUS_SUCCESS;
}

// Must be Non-Paged, as it is called to display the bugcheck screen
VOID BASIC_DISPLAY_DRIVER::SystemDisplayWrite(_In_reads_bytes_(SourceHeight * SourceStride) VOID* pSource,
                                              _In_ UINT SourceWidth,
                                              _In_ UINT SourceHeight,
                                              _In_ UINT SourceStride,
                                              _In_ INT PositionX,
                                              _In_ INT PositionY)
{
    if (m_SystemDisplaySourceId != 0 || !IsScanoutReady()) return;
    const auto m = &m_CurrentModes[0];
    MtkFbWrite(m->FrameBuffer.Ptr,m->DispInfo.Width,m->DispInfo.Height,m->DispInfo.Pitch,
        (SIZE_T)m->DispInfo.Pitch*m->DispInfo.Height,pSource,SourceWidth,SourceHeight,SourceStride,
        (SIZE_T)SourceStride*SourceHeight,PositionX,PositionY);
    KeMemoryBarrier();
}

#pragma code_seg(pop) // End Non-Paged Code

