/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_INTERRUPT_RESOURCES_H
#define T70_MALI_INTERRUPT_RESOURCES_H
#include "Ddi.h"
/* Shared by the PnP resource binder and the DDI interrupt domain. */
static NTSTATUS MaliValidateInterruptResources(const CM_PARTIAL_RESOURCE_DESCRIPTOR *Resources,
    UINT Count,ULONG *LevelMask,KIRQL *SyncIrql) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Resources || Count!=3 || !LevelMask || !SyncIrql ||
       ((uintptr_t)Resources&3u) || (uintptr_t)Resources>UINTPTR_MAX-3u*sizeof(*Resources))
        return STATUS_INVALID_PARAMETER;
    ULONG Levels=0;KIRQL Maximum=0;
    for(UINT I=0;I<Count;I++) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *R=&Resources[I];ULONG Level=R->u.Interrupt.Level;
        if(R->Type!=CmResourceTypeInterrupt ||
           (R->Flags&~(CM_RESOURCE_INTERRUPT_POLICY_INCLUDED|CM_RESOURCE_INTERRUPT_WAKE_HINT)) ||
           R->ShareDisposition<CmResourceShareDeviceExclusive || R->ShareDisposition>CmResourceShareShared ||
           Level<=DISPATCH_LEVEL || Level>=PROFILE_LEVEL ||
#if defined(NT_PROCESSOR_GROUPS)
           R->u.Interrupt.Group ||
#endif
           !R->u.Interrupt.Vector || !R->u.Interrupt.Affinity)return STATUS_DEVICE_CONFIGURATION_ERROR;
        for(UINT J=0;J<I;J++)if(Resources[J].u.Interrupt.Vector==R->u.Interrupt.Vector)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        Levels|=1u<<Level;if(Level>Maximum)Maximum=(KIRQL)Level;
    }
    *LevelMask=Levels;*SyncIrql=Maximum;return STATUS_SUCCESS;
}
#endif
