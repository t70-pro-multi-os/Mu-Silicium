/******************************Module*Header*******************************\
* Module Name: bdd_dmm.hxx
*
* Basic Display Driver display-mode management (DMM) function implementations
*
*
* Copyright (c) 2010 Microsoft Corporation
\**************************************************************************/

#include "bdd.hxx"

#pragma code_seg("PAGE")

// Display-Only Devices can only return display modes of D3DDDIFMT_A8R8G8B8.
// Color conversion takes place if the app's fullscreen backbuffer has different format.
// Full display drivers can add more if the hardware supports them.
D3DDDIFORMAT gBddPixelFormats[] = {
    D3DDDIFMT_A8R8G8B8
};

// Validate the complete single-panel topology and any pinned source/target modes.
NTSTATUS BASIC_DISPLAY_DRIVER::IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN* pIsSupportedVidPn)
{
    PAGED_CODE();
    if (!pIsSupportedVidPn) return STATUS_INVALID_PARAMETER;
    pIsSupportedVidPn->IsVidPnSupported = FALSE;
    auto vidpn = pIsSupportedVidPn->hDesiredVidPn;
    if (!vidpn) { pIsSupportedVidPn->IsVidPnSupported = TRUE; return STATUS_SUCCESS; }
    const DXGK_VIDPN_INTERFACE *v = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *t = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *ss = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ts = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *source = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE *target = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET sources = 0;
    D3DKMDT_HVIDPNTARGETMODESET targets = 0;
    NTSTATUS status = [&]()->NTSTATUS {
        NTSTATUS st = m_DxgkInterface.DxgkCbQueryVidPnInterface(vidpn,DXGK_VIDPN_INTERFACE_VERSION_V1,&v);
        if (!NT_SUCCESS(st)) return st;
        st = v->pfnGetTopology(vidpn,&topology,&t);
        if (!NT_SUCCESS(st)) return st;
        SIZE_T count = 0;
        st = t->pfnGetNumPaths(topology,&count);
        if (!NT_SUCCESS(st)) return st;
        if (!count) { pIsSupportedVidPn->IsVidPnSupported = TRUE; return STATUS_SUCCESS; }
        if (count != 1) return STATUS_SUCCESS;
        st = t->pfnAcquireFirstPathInfo(topology,&path);
        if (!NT_SUCCESS(st)) return st;
        auto local = *path;
        if (local.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED)
            local.ContentTransformation.Scaling = D3DKMDT_VPPS_IDENTITY;
        if (local.ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)
            local.ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
        if (!NT_SUCCESS(IsVidPnPathFieldsValid(&local))) return STATUS_SUCCESS;
        st = v->pfnAcquireSourceModeSet(vidpn,0,&sources,&ss);
        if (!NT_SUCCESS(st)) return st;
        st = ss->pfnAcquirePinnedModeInfo(sources,&source);
        if (!NT_SUCCESS(st)) return st;
        if (source) {
            if (!NT_SUCCESS(IsVidPnSourceModeFieldsValid(source))) return STATUS_SUCCESS;
            if (path->ContentTransformation.Rotation != D3DKMDT_VPPR_UNPINNED) {
                bool tr = local.ContentTransformation.Rotation == D3DKMDT_VPPR_ROTATE90 ||
                    local.ContentTransformation.Rotation == D3DKMDT_VPPR_ROTATE270;
                if (source->Format.Graphics.PrimSurfSize.cx != (tr ? m_Handoff.Height : m_Handoff.Width) ||
                    source->Format.Graphics.PrimSurfSize.cy != (tr ? m_Handoff.Width : m_Handoff.Height)) return STATUS_SUCCESS;
            }
        }
        st = v->pfnAcquireTargetModeSet(vidpn,0,&targets,&ts);
        if (!NT_SUCCESS(st)) return st;
        st = ts->pfnAcquirePinnedModeInfo(targets,&target);
        if (!NT_SUCCESS(st)) return st;
        if (target) {
            const auto signal = &target->VideoSignalInfo;
            if (signal->ActiveSize.cx != m_Handoff.Width || signal->ActiveSize.cy != m_Handoff.Height ||
                signal->TotalSize.cx != m_Handoff.Width || signal->TotalSize.cy != m_Handoff.Height ||
                signal->PixelRate != D3DKMDT_FREQUENCY_NOTSPECIFIED ||
                signal->HSyncFreq.Numerator != D3DKMDT_FREQUENCY_NOTSPECIFIED ||
                signal->HSyncFreq.Denominator != D3DKMDT_FREQUENCY_NOTSPECIFIED ||
                signal->VSyncFreq.Numerator != D3DKMDT_FREQUENCY_NOTSPECIFIED ||
                signal->VSyncFreq.Denominator != D3DKMDT_FREQUENCY_NOTSPECIFIED ||
                signal->ScanLineOrdering != D3DDDI_VSSLO_PROGRESSIVE) return STATUS_SUCCESS;
        }
        pIsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }();
    auto cleanup = [&](NTSTATUS st) { if (!NT_SUCCESS(st) && NT_SUCCESS(status)) status = st; };
    if (target) cleanup(ts->pfnReleaseModeInfo(targets,target));
    if (targets) cleanup(v->pfnReleaseTargetModeSet(vidpn,targets));
    if (source) cleanup(ss->pfnReleaseModeInfo(sources,source));
    if (sources) cleanup(v->pfnReleaseSourceModeSet(vidpn,sources));
    if (path) cleanup(t->pfnReleasePathInfo(topology,path));
    if (!NT_SUCCESS(status)) pIsSupportedVidPn->IsVidPnSupported = FALSE;
    return status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN* CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(pRecommendFunctionalVidPn);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY* CONST pRecommendVidPnTopology)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(pRecommendVidPnTopology);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    if (!pRecommendMonitorModes || pRecommendMonitorModes->VideoPresentTargetId != 0)
        return STATUS_INVALID_PARAMETER;
    // This is always called to recommend modes for the monitor. The sample driver doesn't provide EDID for a monitor, so 
    // the OS prefills the list with default monitor modes. Since the required mode might not be in the list, it should 
    // be provided as a recommended mode.
    return AddSingleMonitorMode(pRecommendMonitorModes);
}

// Tell DMM about all the modes, etc. that are supported
NTSTATUS BASIC_DISPLAY_DRIVER::EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY* CONST pEnumCofuncModality)
{
    PAGED_CODE();

    if (!pEnumCofuncModality) return STATUS_INVALID_PARAMETER;

    D3DKMDT_HVIDPNTOPOLOGY                   hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET              hVidPnSourceModeSet = 0;
    D3DKMDT_HVIDPNTARGETMODESET              hVidPnTargetModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE*              pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE*      pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface = NULL;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pVidPnTargetModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH*        pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH*        pVidPnPresentPathTemp = NULL; // Used for AcquireNextPathInfo
    CONST D3DKMDT_VIDPN_SOURCE_MODE*         pVidPnPinnedSourceModeInfo = NULL;
    CONST D3DKMDT_VIDPN_TARGET_MODE*         pVidPnPinnedTargetModeInfo = NULL;
    BOOLEAN SourceAllowsNative = TRUE, SourceAllowsTranspose = TRUE;

    // Get the VidPn Interface so we can get the 'Source Mode Set', 'Target Mode Set' and 'VidPn Topology' interfaces
    NTSTATUS Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pEnumCofuncModality->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("DxgkCbQueryVidPnInterface failed with Status = 0x%I64x, hFunctionalVidPn = 0x%I64x", Status, pEnumCofuncModality->hConstrainingVidPn);
        return Status;
    }

    // Get the VidPn Topology interface so we can enumerate all paths
    Status = pVidPnInterface->pfnGetTopology(pEnumCofuncModality->hConstrainingVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("pfnGetTopology failed with Status = 0x%I64x, hFunctionalVidPn = 0x%I64x", Status, pEnumCofuncModality->hConstrainingVidPn);
        return Status;
    }

    // Get the first path before we start looping through them
    Status = pVidPnTopologyInterface->pfnAcquireFirstPathInfo(hVidPnTopology, &pVidPnPresentPath);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("pfnAcquireFirstPathInfo failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x", Status, hVidPnTopology);
        return Status;
    }

    // Loop through all available paths.
    while (Status != STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        if (pVidPnPresentPath->VidPnSourceId != 0 || pVidPnPresentPath->VidPnTargetId != 0) {
            Status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; break;
        }
        // Get the Source Mode Set interface so the pinned mode can be retrieved
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                          pVidPnPresentPath->VidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR3("pfnAcquireSourceModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, SourceId = 0x%I64x",
                           Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId);
            break;
        }

        // Get the pinned mode, needed when VidPnSource isn't pivot, and when VidPnTarget isn't pivot
        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet, &pVidPnPinnedSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR2("pfnAcquirePinnedModeInfo failed with Status = 0x%I64x, hVidPnSourceModeSet = 0x%I64x", Status, hVidPnSourceModeSet);
            break;
        }

        SourceAllowsNative = SourceAllowsTranspose = TRUE;
        if (pVidPnPinnedSourceModeInfo != NULL) {
            Status = IsVidPnSourceModeFieldsValid(pVidPnPinnedSourceModeInfo);
            if (!NT_SUCCESS(Status)) break;
            const auto size = &pVidPnPinnedSourceModeInfo->Format.Graphics.PrimSurfSize;
            SourceAllowsNative = size->cx == m_Handoff.Width && size->cy == m_Handoff.Height;
            SourceAllowsTranspose = size->cx == m_Handoff.Height && size->cy == m_Handoff.Width;
        }
        // SOURCE MODES: If this source mode isn't the pivot point, do work on the source mode set
        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId)))
        {
            // If there's no pinned source add possible modes (otherwise they've already been added)
            if (pVidPnPinnedSourceModeInfo == NULL)
            {
                // Release the acquired source mode set, since going to create a new one to put all modes in
                Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR3("pfnReleaseSourceModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, hVidPnSourceModeSet = 0x%I64x",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
                    break;
                }
                hVidPnSourceModeSet = 0; // Successfully released it

                // Create a new source mode set which will be added to the constraining VidPn with all the possible modes
                Status = pVidPnInterface->pfnCreateNewSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnSourceId,
                                                                    &hVidPnSourceModeSet,
                                                                    &pVidPnSourceModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR3("pfnCreateNewSourceModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, SourceId = 0x%I64x",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId);
                    break;
                }

                // Add the appropriate modes to the source mode set
                {
                    Status = AddSingleSourceMode(pVidPnSourceModeSetInterface, hVidPnSourceModeSet, pVidPnPresentPath->VidPnSourceId, pVidPnPresentPath->ContentTransformation.Rotation);
                }

                if (!NT_SUCCESS(Status))
                {
                    break;
                }

                // Give DMM back the source modes just populated
                Status = pVidPnInterface->pfnAssignSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId, hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR4("pfnAssignSourceModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, SourceId = 0x%I64x, hVidPnSourceModeSet = 0x%I64x",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId, hVidPnSourceModeSet);
                    break;
                }
                hVidPnSourceModeSet = 0; // Successfully assigned it (equivalent to releasing it)
            }
        }// End: SOURCE MODES

        // TARGET MODES: If this target mode isn't the pivot point, do work on the target mode set
        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            // Get the Target Mode Set interface so modes can be added if necessary
            Status = pVidPnInterface->pfnAcquireTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              pVidPnPresentPath->VidPnTargetId,
                                                              &hVidPnTargetModeSet,
                                                              &pVidPnTargetModeSetInterface);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR3("pfnAcquireTargetModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, TargetId = 0x%I64x",
                               Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId);
                break;
            }

            Status = pVidPnTargetModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnTargetModeSet, &pVidPnPinnedTargetModeInfo);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR2("pfnAcquirePinnedModeInfo failed with Status = 0x%I64x, hVidPnTargetModeSet = 0x%I64x", Status, hVidPnTargetModeSet);
                break;
            }

            // If there's no pinned target add possible modes (otherwise they've already been added)
            if (pVidPnPinnedTargetModeInfo == NULL)
            {
                // Release the acquired target mode set, since going to create a new one to put all modes in
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ASSERTION3("pfnReleaseTargetModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, hVidPnTargetModeSet = 0x%I64x",
                                       Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                    break;
                }
                hVidPnTargetModeSet = 0; // Successfully released it

                // Create a new target mode set which will be added to the constraining VidPn with all the possible modes
                Status = pVidPnInterface->pfnCreateNewTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnTargetId,
                                                                    &hVidPnTargetModeSet,
                                                                    &pVidPnTargetModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR3("pfnCreateNewTargetModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, TargetId = 0x%I64x",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId);
                    break;
                }

                Status = AddSingleTargetMode(pVidPnTargetModeSetInterface, hVidPnTargetModeSet, pVidPnPinnedSourceModeInfo, pVidPnPresentPath->VidPnSourceId);

                if (!NT_SUCCESS(Status))
                {
                    break;
                }

                // Give DMM back the source modes just populated
                Status = pVidPnInterface->pfnAssignTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId, hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR4("pfnAssignTargetModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, TargetId = 0x%I64x, hVidPnTargetModeSet = 0x%I64x",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId, hVidPnTargetModeSet);
                    break;
                }
                hVidPnTargetModeSet = 0; // Successfully assigned it (equivalent to releasing it)
            }
            else
            {
                // Release the pinned target as there's no other work to do
                Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ASSERTION3("pfnReleaseModeInfo failed with Status = 0x%I64x, hVidPnTargetModeSet = 0x%I64x, pVidPnPinnedTargetModeInfo = 0x%I64x",
                                        Status, hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
                    break;
                }
                pVidPnPinnedTargetModeInfo = NULL; // Successfully released it

                // Release the acquired target mode set, since it is no longer needed
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ASSERTION3("pfnReleaseTargetModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, hVidPnTargetModeSet = 0x%I64x",
                                       Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                    break;
                }
                hVidPnTargetModeSet = 0; // Successfully released it
            }
        }// End: TARGET MODES

        // Nothing else needs the pinned source mode so release it
        if (pVidPnPinnedSourceModeInfo != NULL)
        {
            Status = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ASSERTION3("pfnReleaseModeInfo failed with Status = 0x%I64x, hVidPnSourceModeSet = 0x%I64x, pVidPnPinnedSourceModeInfo = 0x%I64x",
                                    Status, hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
                break;
            }
            pVidPnPinnedSourceModeInfo = NULL; // Successfully released it
        }

        // With the pinned source mode now released, if the source mode set hasn't been released, release that as well
        if (hVidPnSourceModeSet != 0)
        {
            Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR3("pfnReleaseSourceModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, hVidPnSourceModeSet = 0x%I64x",
                               Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
                break;
            }
            hVidPnSourceModeSet = 0; // Successfully released it
        }

        // If modifying support fields, need to modify a local version of a path structure since the retrieved one is const
        D3DKMDT_VIDPN_PRESENT_PATH LocalVidPnPresentPath = *pVidPnPresentPath;
        BOOLEAN SupportFieldsModified = FALSE;

        // SCALING: If this path's scaling isn't the pivot point, do work on the scaling support
        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_SCALING) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            // If the scaling is unpinned, then modify the scaling support field
            if (pVidPnPresentPath->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED)
            {
                // Identity and centered scaling are supported, but not any stretch modes
                RtlZeroMemory(&(LocalVidPnPresentPath.ContentTransformation.ScalingSupport), sizeof(D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT));
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Centered = 0;
                SupportFieldsModified = TRUE;
            }
        } // End: SCALING

        // ROTATION: If this path's rotation isn't the pivot point, do work on the rotation support
        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_ROTATION) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            // If the rotation is unpinned, then modify the rotation support field
            if (pVidPnPresentPath->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)
            {
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Identity = SourceAllowsNative;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate90 = SourceAllowsTranspose;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate180 = SourceAllowsNative;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate270 = SourceAllowsTranspose;



                SupportFieldsModified = TRUE;
            }
        } // End: ROTATION

        if (SupportFieldsModified)
        {
            // The correct path will be found by this function and the appropriate fields updated
            Status = pVidPnTopologyInterface->pfnUpdatePathSupportInfo(hVidPnTopology, &LocalVidPnPresentPath);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR2("pfnUpdatePathSupportInfo failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x", Status, hVidPnTopology);
                break;
            }
        }

        // Get the next path...
        // (NOTE: This is the value of Status that will return STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET when it's time to quit the loop)
        pVidPnPresentPathTemp = pVidPnPresentPath;
        Status = pVidPnTopologyInterface->pfnAcquireNextPathInfo(hVidPnTopology, pVidPnPresentPathTemp, &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR3("pfnAcquireNextPathInfo failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x, pVidPnPresentPathTemp = 0x%I64x", Status, hVidPnTopology, pVidPnPresentPathTemp);
            break;
        }

        // ...and release the last path
        NTSTATUS TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        if (!NT_SUCCESS(TempStatus))
        {
            BDD_LOG_ERROR3("pfnReleasePathInfo failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x, pVidPnPresentPathTemp = 0x%I64x", TempStatus, hVidPnTopology, pVidPnPresentPathTemp);
            Status = TempStatus;
            break;
        }
        pVidPnPresentPathTemp = NULL; // Successfully released it
    }// End: while loop for paths in topology

    // If quit the while loop normally, set the return value to success
    if (Status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = STATUS_SUCCESS;
    }

    // Release any resources hanging around because the loop was quit early.
    // Since in normal execution everything should be released by this point, TempStatus is initialized to a bogus error to be used as an
    //  assertion that if anything had to be released now (TempStatus changing) Status isn't successful.
    NTSTATUS TempStatus = STATUS_NOT_FOUND;

    if ((pVidPnSourceModeSetInterface != NULL) &&
        (pVidPnPinnedSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTargetModeSetInterface != NULL) &&
        (pVidPnPinnedTargetModeInfo != NULL))
    {
        TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPath != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPathTemp != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnSourceModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnTargetModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (!NT_SUCCESS(TempStatus) && TempStatus != STATUS_NOT_FOUND && NT_SUCCESS(Status)) Status = TempStatus;

    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY* pSetVidPnSourceVisibility)
{
    PAGED_CODE();
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) return STATUS_DEVICE_BUSY;

    if (!pSetVidPnSourceVisibility || (pSetVidPnSourceVisibility->VidPnSourceId != 0 &&
        pSetVidPnSourceVisibility->VidPnSourceId != D3DDDI_ID_ALL)) return STATUS_INVALID_PARAMETER;
    if (pSetVidPnSourceVisibility->Visible && !IsScanoutReady()) return STATUS_DEVICE_NOT_READY;
    BDD_ASSERT((pSetVidPnSourceVisibility->VidPnSourceId < MAX_VIEWS) ||
               (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL));

    UINT StartVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? 0 : pSetVidPnSourceVisibility->VidPnSourceId;
    UINT MaxVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? MAX_VIEWS : pSetVidPnSourceVisibility->VidPnSourceId + 1;

    for (UINT SourceId = StartVidPnSourceId; SourceId < MaxVidPnSourceId; ++SourceId)
    {
        if (pSetVidPnSourceVisibility->Visible)
        {
            m_CurrentModes[SourceId].Flags.FullscreenPresent = TRUE;
        }
        else
        {
            BlackOutScreen(SourceId);
        }

        // Store current visibility so it can be dealt with during Present call
        m_CurrentModes[SourceId].Flags.SourceNotVisible = !(pSetVidPnSourceVisibility->Visible);
    }

    return STATUS_SUCCESS;
}

// NOTE: The value of pCommitVidPn->MonitorConnectivityChecks is ignored, since BDD is unable to recognize whether a monitor is connected or not
// The value of pCommitVidPn->hPrimaryAllocation is also ignored, since BDD is a display only driver and does not deal with allocations
NTSTATUS BASIC_DISPLAY_DRIVER::CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN* CONST pCommitVidPn)
{
    PAGED_CODE();
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) return STATUS_DEVICE_BUSY;

    if (!pCommitVidPn || pCommitVidPn->AffectedVidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (pCommitVidPn->hFunctionalVidPn && !IsScanoutAllowed()) return STATUS_DEVICE_NOT_READY;
    DXGKARG_ISSUPPORTEDVIDPN supported = {};
    supported.hDesiredVidPn = pCommitVidPn->hFunctionalVidPn;
    NTSTATUS supportStatus = IsSupportedVidPn(&supported);
    if (!NT_SUCCESS(supportStatus)) return supportStatus;
    if (!supported.IsVidPnSupported) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    if (!pCommitVidPn->hFunctionalVidPn) { CleanUp(); return STATUS_SUCCESS; }

    NTSTATUS                                 Status;
    SIZE_T                                   NumPaths = 0;
    SIZE_T NumPathsFromSource = 0;
    D3DKMDT_HVIDPNTOPOLOGY                   hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET              hVidPnSourceModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE*              pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE*      pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH*        pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE*         pPinnedVidPnSourceModeInfo = NULL;

    // Check this CommitVidPn is for the mode change notification when monitor is in power off state.
    if (pCommitVidPn->Flags.PathPoweredOff)
    {
        // Ignore the commitVidPn call for the mode change notification when monitor is in power off state.
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    // Get the VidPn Interface so we can get the 'Source Mode Set' and 'VidPn Topology' interfaces
    Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pCommitVidPn->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("DxgkCbQueryVidPnInterface failed with Status = 0x%I64x, hFunctionalVidPn = 0x%I64x", Status, pCommitVidPn->hFunctionalVidPn);
        goto CommitVidPnExit;
    }

    // Get the VidPn Topology interface so can enumerate paths from source
    Status = pVidPnInterface->pfnGetTopology(pCommitVidPn->hFunctionalVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("pfnGetTopology failed with Status = 0x%I64x, hFunctionalVidPn = 0x%I64x", Status, pCommitVidPn->hFunctionalVidPn);
        goto CommitVidPnExit;
    }

    // Find out the number of paths now, if it's 0 don't bother with source mode set and pinned mode, just clear current and then quit
    Status = pVidPnTopologyInterface->pfnGetNumPaths(hVidPnTopology, &NumPaths);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("pfnGetNumPaths failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x", Status, hVidPnTopology);
        goto CommitVidPnExit;
    }

    if (NumPaths > 1) { Status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; goto CommitVidPnExit; }
    if (NumPaths != 0)
    {
        // Get the Source Mode Set interface so we can get the pinned mode
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pCommitVidPn->hFunctionalVidPn,
                                                          pCommitVidPn->AffectedVidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR3("pfnAcquireSourceModeSet failed with Status = 0x%I64x, hFunctionalVidPn = 0x%I64x, SourceId = 0x%I64x", Status, pCommitVidPn->hFunctionalVidPn, pCommitVidPn->AffectedVidPnSourceId);
            goto CommitVidPnExit;
        }

        // Get the mode that is being pinned
        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet, &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR2("pfnAcquirePinnedModeInfo failed with Status = 0x%I64x, hFunctionalVidPn = 0x%I64x", Status, pCommitVidPn->hFunctionalVidPn);
            goto CommitVidPnExit;
        }
    }
    else
    {
        // This will cause the successful quit below
        pPinnedVidPnSourceModeInfo = NULL;
    }

    if (pPinnedVidPnSourceModeInfo == NULL)
    {
        CleanUp();
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = IsVidPnSourceModeFieldsValid(pPinnedVidPnSourceModeInfo);
    if (!NT_SUCCESS(Status))
    {
        goto CommitVidPnExit;
    }

    // Get the number of paths from this source so we can loop through all paths
    Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, &NumPathsFromSource);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("pfnGetNumPathsFromSource failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x", Status, hVidPnTopology);
        goto CommitVidPnExit;
    }

    if (NumPathsFromSource != 1) { Status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; goto CommitVidPnExit; }
    // Loop through all paths to set this mode
    for (SIZE_T PathIndex = 0; PathIndex < NumPathsFromSource; ++PathIndex)
    {
        // Get the target id for this path
        D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId = D3DDDI_ID_UNINITIALIZED;
        Status = pVidPnTopologyInterface->pfnEnumPathTargetsFromSource(hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, PathIndex, &TargetId);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR4("pfnEnumPathTargetsFromSource failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x, SourceId = 0x%I64x, PathIndex = 0x%I64x",
                            Status, hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, PathIndex);
            goto CommitVidPnExit;
        }

        // Get the actual path info
        Status = pVidPnTopologyInterface->pfnAcquirePathInfo(hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, TargetId, &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR4("pfnAcquirePathInfo failed with Status = 0x%I64x, hVidPnTopology = 0x%I64x, SourceId = 0x%I64x, TargetId = 0x%I64x",
                            Status, hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, TargetId);
            goto CommitVidPnExit;
        }

        Status = IsVidPnPathFieldsValid(pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = SetSourceModeAndPath(pPinnedVidPnSourceModeInfo, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR3("pfnReleasePathInfo failed with Status = 0x%I64x, hVidPnTopoogy = 0x%I64x, pVidPnPresentPath = 0x%I64x",
                            Status, hVidPnTopology, pVidPnPresentPath);
            goto CommitVidPnExit;
        }
        pVidPnPresentPath = NULL; // Successfully released it
    }

CommitVidPnExit:

    NTSTATUS TempStatus;

    if ((pVidPnSourceModeSetInterface != NULL) &&
        (hVidPnSourceModeSet != 0) &&
        (pPinnedVidPnSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(TempStatus) && NT_SUCCESS(Status)) Status = TempStatus;
    }

    if ((pVidPnInterface != NULL) &&
        (pCommitVidPn->hFunctionalVidPn != 0) &&
        (hVidPnSourceModeSet != 0))
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pCommitVidPn->hFunctionalVidPn, hVidPnSourceModeSet);
        if (!NT_SUCCESS(TempStatus) && NT_SUCCESS(Status)) Status = TempStatus;
    }

    if ((pVidPnTopologyInterface != NULL) &&
        (hVidPnTopology != 0) &&
        (pVidPnPresentPath != NULL))
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        if (!NT_SUCCESS(TempStatus) && NT_SUCCESS(Status)) Status = TempStatus;
    }

    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();
    MTK_DISPLAY_GUARD guard(&m_PresentMutex);
    if (!guard.Acquired) return STATUS_DEVICE_BUSY;

    if (!pUpdateActiveVidPnPresentPath) return STATUS_INVALID_PARAMETER;

    NTSTATUS Status = IsVidPnPathFieldsValid(&(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Mark the next present as fullscreen to make sure the full rotation comes through
    m_CurrentModes[pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId].Flags.FullscreenPresent = TRUE;

    auto m = &m_CurrentModes[0];
    auto r = pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.ContentTransformation.Rotation;
    if (r == D3DKMDT_VPPR_NOTSPECIFIED || r == D3DKMDT_VPPR_UNINITIALIZED) r = D3DKMDT_VPPR_IDENTITY;
    bool transpose = r == D3DKMDT_VPPR_ROTATE90 || r == D3DKMDT_VPPR_ROTATE270;
    if (m->SrcModeWidth != (transpose ? m_Handoff.Height : m_Handoff.Width) ||
        m->SrcModeHeight != (transpose ? m_Handoff.Width : m_Handoff.Height))
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    m->Rotation = r;

    return STATUS_SUCCESS;
}

//
// Private BDD DMM functions
//

NTSTATUS BASIC_DISPLAY_DRIVER::SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode,
                                                    CONST D3DKMDT_VIDPN_PRESENT_PATH* pPath)
{
    PAGED_CODE();

    if (!IsScanoutAllowed()) return STATUS_DEVICE_NOT_READY;
    NTSTATUS valid = IsVidPnPathFieldsValid(pPath);
    if (!NT_SUCCESS(valid)) return valid;
    valid = IsVidPnSourceModeFieldsValid(pSourceMode);
    if (!NT_SUCCESS(valid)) return valid;
    CURRENT_BDD_MODE* pCurrentBddMode = &m_CurrentModes[pPath->VidPnSourceId];
    auto rotation = pPath->ContentTransformation.Rotation;
    if (rotation == D3DKMDT_VPPR_NOTSPECIFIED || rotation == D3DKMDT_VPPR_UNINITIALIZED)
        rotation = D3DKMDT_VPPR_IDENTITY;
    const bool transpose = rotation == D3DKMDT_VPPR_ROTATE90 || rotation == D3DKMDT_VPPR_ROTATE270;
    if (pSourceMode->Format.Graphics.PrimSurfSize.cx != (transpose ? m_Handoff.Height : m_Handoff.Width) ||
        pSourceMode->Format.Graphics.PrimSurfSize.cy != (transpose ? m_Handoff.Width : m_Handoff.Height))
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;

    NTSTATUS Status = STATUS_SUCCESS;
    pCurrentBddMode->Scaling = D3DKMDT_VPPS_IDENTITY;
    pCurrentBddMode->SrcModeWidth = pSourceMode->Format.Graphics.PrimSurfSize.cx;
    pCurrentBddMode->SrcModeHeight = pSourceMode->Format.Graphics.PrimSurfSize.cy;
    pCurrentBddMode->Rotation = rotation;


    if (!pCurrentBddMode->Flags.DoNotMapOrUnmap && !pCurrentBddMode->FrameBuffer.Ptr)
    {
        // Map the new frame buffer
        BDD_ASSERT(pCurrentBddMode->FrameBuffer.Ptr == NULL);
        Status = MapFrameBuffer(pCurrentBddMode->DispInfo.PhysicAddress,
                                pCurrentBddMode->DispInfo.Pitch * pCurrentBddMode->DispInfo.Height,
                                &(pCurrentBddMode->FrameBuffer.Ptr));
    }

    if (NT_SUCCESS(Status))
    {

        pCurrentBddMode->Flags.FrameBufferIsActive = TRUE;
        BlackOutScreen(pPath->VidPnSourceId);

        // Mark that the next present should be fullscreen so the screen doesn't go from black to actual pixels one dirty rect at a time.
        pCurrentBddMode->Flags.FullscreenPresent = TRUE;
    }

    return Status;
}


NTSTATUS BASIC_DISPLAY_DRIVER::IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH* pPath) const
{
    PAGED_CODE();

    if (!pPath) return STATUS_INVALID_PARAMETER;
    if (pPath->VidPnSourceId >= MAX_VIEWS)
    {
        BDD_LOG_ERROR2("VidPnSourceId is 0x%I64x is too high (MAX_VIEWS is 0x%I64x)",
                        pPath->VidPnSourceId, MAX_VIEWS);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    else if (pPath->VidPnTargetId >= MAX_CHILDREN)
    {
        BDD_LOG_ERROR2("VidPnTargetId is 0x%I64x is too high (MAX_CHILDREN is 0x%I64x)",
                        pPath->VidPnTargetId, MAX_CHILDREN);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    else if (pPath->GammaRamp.Type != D3DDDI_GAMMARAMP_DEFAULT)
    {
        BDD_LOG_ERROR1("pPath contains a gamma ramp (0x%I64x)", pPath->GammaRamp.Type);
        return STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_UNINITIALIZED))
    {
        BDD_LOG_ERROR1("pPath contains a non-identity scaling (0x%I64x)", pPath->ContentTransformation.Scaling);
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE90) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE180) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE270) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED))
    {
        BDD_LOG_ERROR1("pPath contains a not-supported rotation (0x%I64x)", pPath->ContentTransformation.Rotation);
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->VidPnTargetColorBasis != D3DKMDT_CB_SCRGB) &&
             (pPath->VidPnTargetColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        BDD_LOG_ERROR1("pPath has a non-linear RGB color basis (0x%I64x)", pPath->VidPnTargetColorBasis);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        return STATUS_SUCCESS;
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode) const
{
    PAGED_CODE();

    if (!pSourceMode) return STATUS_INVALID_PARAMETER;
    const auto g = &pSourceMode->Format.Graphics;
    const auto d = &m_CurrentModes[0].DispInfo;
    if (!((g->PrimSurfSize.cx == d->Width && g->PrimSurfSize.cy == d->Height) ||
          (g->PrimSurfSize.cx == d->Height && g->PrimSurfSize.cy == d->Width)) ||
        g->VisibleRegionSize.cx != g->PrimSurfSize.cx || g->VisibleRegionSize.cy != g->PrimSurfSize.cy ||
        g->Stride < g->PrimSurfSize.cx*4 || g->Stride > 65536 || (g->Stride & 3))
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    if (pSourceMode->Type != D3DKMDT_RMT_GRAPHICS)
    {
        BDD_LOG_ERROR1("pSourceMode is a non-graphics mode (0x%I64x)", pSourceMode->Type);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if ((pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_SCRGB) &&
             (pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        BDD_LOG_ERROR1("pSourceMode has a non-linear RGB color basis (0x%I64x)", pSourceMode->Format.Graphics.ColorBasis);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if (pSourceMode->Format.Graphics.PixelValueAccessMode != D3DKMDT_PVAM_DIRECT)
    {
        BDD_LOG_ERROR1("pSourceMode has a palettized access mode (0x%I64x)", pSourceMode->Format.Graphics.PixelValueAccessMode);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        for (UINT PelFmtIdx = 0; PelFmtIdx < ARRAYSIZE(gBddPixelFormats); ++PelFmtIdx)
        {
            if (pSourceMode->Format.Graphics.PixelFormat == gBddPixelFormats[PelFmtIdx])
            {
                return STATUS_SUCCESS;
            }
        }

        BDD_LOG_ERROR1("pSourceMode has an unknown pixel format (0x%I64x)", pSourceMode->Format.Graphics.PixelFormat);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
}



NTSTATUS BASIC_DISPLAY_DRIVER::AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface,
                                                   D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                                   D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId,
                                                   D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation)
{
    PAGED_CODE();
    if (SourceId != 0) return STATUS_INVALID_PARAMETER;
    for (unsigned transpose=0;transpose<2;transpose++) {
        auto d = &m_CurrentModes[0].DispInfo;
        const bool pinned = Rotation != D3DKMDT_VPPR_UNPINNED;
        const bool tr = Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270;
        if (pinned && transpose != tr) continue;
        if (transpose && d->Width == d->Height) break;
        D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
        NTSTATUS status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet,&mode);
        if (!NT_SUCCESS(status)) return status;
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        auto g = &mode->Format.Graphics;
        g->PrimSurfSize.cx = transpose ? d->Height : d->Width;
        g->PrimSurfSize.cy = transpose ? d->Width : d->Height;
        g->VisibleRegionSize = g->PrimSurfSize;
        g->Stride = g->PrimSurfSize.cx*4;
        g->PixelFormat = D3DDDIFMT_A8R8G8B8;
        g->ColorBasis = D3DKMDT_CB_SCRGB;
        g->PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet,mode);
        if (!NT_SUCCESS(status)) {
            auto cleanup = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet,mode);
            if (!NT_SUCCESS(cleanup)) return cleanup;
            if (status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) return status;
        }
    }
    return STATUS_SUCCESS;
}


// Add the current mode information (acquired from the POST frame buffer) as the target mode.
NTSTATUS BASIC_DISPLAY_DRIVER::AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pVidPnTargetModeSetInterface,
                                                   D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                                   _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE* pVidPnPinnedSourceModeInfo,
                                                   D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    D3DKMDT_VIDPN_TARGET_MODE* pVidPnTargetModeInfo = NULL;
    NTSTATUS Status = pVidPnTargetModeSetInterface->pfnCreateNewModeInfo(hVidPnTargetModeSet, &pVidPnTargetModeInfo);
    if (!NT_SUCCESS(Status))
    {
        // If failed to create a new mode info, mode doesn't need to be released since it was never created
        BDD_LOG_ERROR2("pfnCreateNewModeInfo failed with Status = 0x%I64x, hVidPnTargetModeSet = 0x%I64x", Status, hVidPnTargetModeSet);
        return Status;
    }

    pVidPnTargetModeInfo->VideoSignalInfo.VideoStandard = D3DKMDT_VSS_OTHER;
    UNREFERENCED_PARAMETER(pVidPnPinnedSourceModeInfo);
    pVidPnTargetModeInfo->VideoSignalInfo.TotalSize.cx = m_CurrentModes[SourceId].DispInfo.Width;
    pVidPnTargetModeInfo->VideoSignalInfo.TotalSize.cy = m_CurrentModes[SourceId].DispInfo.Height;
    pVidPnTargetModeInfo->VideoSignalInfo.ActiveSize = pVidPnTargetModeInfo->VideoSignalInfo.TotalSize;
    pVidPnTargetModeInfo->VideoSignalInfo.VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
    // We add this as PREFERRED since it is the only supported target
    pVidPnTargetModeInfo->Preference = D3DKMDT_MP_PREFERRED;

    Status = pVidPnTargetModeSetInterface->pfnAddMode(hVidPnTargetModeSet, pVidPnTargetModeInfo);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            BDD_LOG_ERROR3("pfnAddMode failed with Status = 0x%I64x, hVidPnTargetModeSet = 0x%I64x, pVidPnTargetModeInfo = 0x%I64x", Status, hVidPnTargetModeSet, pVidPnTargetModeInfo);
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        // If adding the mode failed, release the mode, if this doesn't work there is nothing that can be done, some memory will get leaked
        NTSTATUS TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnTargetModeInfo);
        UNREFERENCED_PARAMETER(TempStatus);
        if (!NT_SUCCESS(TempStatus) && NT_SUCCESS(Status)) Status = TempStatus;
        return Status;
    }
    else
    {
        // If AddMode succeeded with something other than STATUS_SUCCESS treat it as such anyway when propagating up
        return STATUS_SUCCESS;
    }
}


NTSTATUS BASIC_DISPLAY_DRIVER::AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    D3DKMDT_MONITOR_SOURCE_MODE* pMonitorSourceMode = NULL;
    NTSTATUS Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet, &pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        // If failed to create a new mode info, mode doesn't need to be released since it was never created
        BDD_LOG_ERROR2("pfnCreateNewModeInfo failed with Status = 0x%I64x, hMonitorSourceModeSet = 0x%I64x", Status, pRecommendMonitorModes->hMonitorSourceModeSet);
        return Status;
    }

    D3DDDI_VIDEO_PRESENT_SOURCE_ID CorrespondingSourceId = FindSourceForTarget(pRecommendMonitorModes->VideoPresentTargetId, TRUE);

    // Since we don't know the real monitor timing information, just use the current display mode (from the POST device) with unknown frequencies
    pMonitorSourceMode->VideoSignalInfo.VideoStandard = D3DKMDT_VSS_OTHER;
    pMonitorSourceMode->VideoSignalInfo.TotalSize.cx = m_CurrentModes[CorrespondingSourceId].DispInfo.Width;
    pMonitorSourceMode->VideoSignalInfo.TotalSize.cy = m_CurrentModes[CorrespondingSourceId].DispInfo.Height;
    pMonitorSourceMode->VideoSignalInfo.ActiveSize = pMonitorSourceMode->VideoSignalInfo.TotalSize;
    pMonitorSourceMode->VideoSignalInfo.VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;

    // We set the preference to PREFERRED since this is the only supported mode
    pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
    pMonitorSourceMode->Preference = D3DKMDT_MP_PREFERRED;
    pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet, pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            BDD_LOG_ERROR3("pfnAddMode failed with Status = 0x%I64x, hMonitorSourceModeSet = 0x%I64x, pMonitorSourceMode = 0x%I64x",
                            Status, pRecommendMonitorModes->hMonitorSourceModeSet, pMonitorSourceMode);
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        // If adding the mode failed, release the mode, if this doesn't work there is nothing that can be done, some memory will get leaked
        NTSTATUS TempStatus = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet, pMonitorSourceMode);
        UNREFERENCED_PARAMETER(TempStatus);
        if (!NT_SUCCESS(TempStatus) && NT_SUCCESS(Status)) Status = TempStatus;
        return Status;
    }
    else
    {
        // If AddMode succeeded with something other than STATUS_SUCCESS treat it as such anyway when propagating up
        return STATUS_SUCCESS;
    }
}

