// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synchronous software present: no simulated hardware interrupt or worker thread.
#include "bdd.hxx"
#pragma code_seg("PAGE")
BDD_HWBLT::BDD_HWBLT() : m_SourceId(0), m_DevExt(NULL) {}
BDD_HWBLT::~BDD_HWBLT() {}
NTSTATUS BDD_HWBLT::ExecutePresentDisplayOnly(BYTE *DstAddr,UINT DstBitPerPixel,
    BYTE *SrcAddr,UINT SrcBytesPerPixel,LONG SrcPitch,ULONG NumMoves,
    D3DKMT_MOVE_RECT *Moves,ULONG NumDirtyRects,RECT *DirtyRect,
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation) {
    PAGED_CODE();
    const CURRENT_BDD_MODE *m = m_DevExt->GetCurrentMode(m_SourceId);
    if (!m || DstBitPerPixel != 32 || SrcBytesPerPixel != 4 || SrcPitch <= 0 ||
        SrcPitch > 65536 || !SrcAddr || Rotation < D3DKMDT_VPPR_IDENTITY ||
        Rotation > D3DKMDT_VPPR_ROTATE270 || NumMoves > MTK_FB_MAX_RECTS ||
        NumDirtyRects > MTK_FB_MAX_RECTS-NumMoves ||
        (NumMoves && !Moves) || (NumDirtyRects && !DirtyRect)) return STATUS_INVALID_PARAMETER;
    static_assert(sizeof(MTK_FB_MOVE)==sizeof(D3DKMT_MOVE_RECT),"move layout");
    static_assert(sizeof(MTK_FB_RECT)==sizeof(RECT),"rect layout");
    unsigned rot = (unsigned)Rotation-(unsigned)D3DKMDT_VPPR_IDENTITY;
    UINT sw = (rot & 1) ? m->DispInfo.Height : m->DispInfo.Width;
    UINT sh = (rot & 1) ? m->DispInfo.Width : m->DispInfo.Height;
    SIZE_T sn = (SIZE_T)SrcPitch*sh, dn = (SIZE_T)m->DispInfo.Pitch*m->DispInfo.Height;
    SIZE_T bytes = (SIZE_T)NumMoves*sizeof(MTK_FB_MOVE)+(SIZE_T)NumDirtyRects*sizeof(MTK_FB_RECT);
    BYTE *snapshot = bytes ? (BYTE*)ExAllocatePool2(POOL_FLAG_PAGED,bytes,BDDTAG) : NULL;
    if (bytes && !snapshot) return STATUS_INSUFFICIENT_RESOURCES;
    MTK_FB_MOVE *moves = (MTK_FB_MOVE*)snapshot;
    MTK_FB_RECT *rects = bytes ? (MTK_FB_RECT*)(snapshot+NumMoves*sizeof(MTK_FB_MOVE)) : NULL;
    if (NumMoves) RtlCopyMemory(moves,Moves,NumMoves*sizeof(*moves));
    if (NumDirtyRects) RtlCopyMemory(rects,DirtyRect,NumDirtyRects*sizeof(*rects));
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    PMDL mdl = NULL;
    BOOLEAN locked = FALSE;
    if (MtkFbValidatePresent(m->DispInfo.Width,m->DispInfo.Height,m->DispInfo.Pitch,dn,
        sw,sh,SrcPitch,sn,rot,moves,NumMoves,rects,NumDirtyRects) &&
        (ULONG_PTR)SrcAddr <= MAXULONG_PTR-sn) {
        mdl = IoAllocateMdl(SrcAddr,(ULONG)sn,FALSE,FALSE,NULL);
        if (!mdl) status = STATUS_INSUFFICIENT_RESOURCES;
        else {
            __try {
                MmProbeAndLockPages(mdl,(ULONG_PTR)SrcAddr <= (ULONG_PTR)MmUserProbeAddress ? UserMode : KernelMode,IoReadAccess);
                locked = TRUE;
                PVOID source = MmGetSystemAddressForMdlSafe(mdl,NormalPagePriority | MdlMappingNoExecute);
                status = STATUS_INSUFFICIENT_RESOURCES;
                if (source) {
                    MTK_FB_RECT full = {0,0,(int32_t)sw,(int32_t)sh};
                    // Validate every submitted rectangle before replacing by a full repaint.
                    status = MtkFbPresent(DstAddr,m->DispInfo.Width,m->DispInfo.Height,m->DispInfo.Pitch,dn,
                        source,sw,sh,SrcPitch,sn,rot,
                        m->Flags.FullscreenPresent ? NULL : moves,m->Flags.FullscreenPresent ? 0 : NumMoves,
                        m->Flags.FullscreenPresent ? &full : rects,m->Flags.FullscreenPresent ? 1 : NumDirtyRects)
                        ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
                    KeMemoryBarrier();
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
        }
    }
    if (locked) MmUnlockPages(mdl);
    if (mdl) IoFreeMdl(mdl);
    if (snapshot) ExFreePool(snapshot);
    return status;
}
