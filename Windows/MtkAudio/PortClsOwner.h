// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_PORTCLS_OWNER_H
#define T70_PORTCLS_OWNER_H
#include <ntddk.h>
#include <wdf.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Driver-wide lifecycle, called only from the actual DriverEntry/unload.
 * Owner is zeroed nonpaged storage that lives for the entire image lifetime.
 * Actual AddDevice and Unload are supplied by the integrating driver; no dummy
 * device, discovery, resource assignment or endpoint is created here.
 * Owner must be disjoint from Driver, its extension and RegistryPath storage.
 * No concurrent calls; PASSIVE_LEVEL. No pre-existing WDF/PortCls binding.
 * Unload must stop admission and drain/delete ALL device resources first, then
 * call FinishUnload exactly once. This object does not implement device rundown.
 * Successful initialization is NOT a loaded endpoint or playback guarantee. */
typedef struct {
 PDRIVER_OBJECT Driver;
 WDFDRIVER Framework;
 PDRIVER_UNLOAD PortUnload;
} T70_PORTCLS_OWNER;
NTSTATUS T70PortClsInitialize(T70_PORTCLS_OWNER *Owner,PDRIVER_OBJECT Driver,
 PUNICODE_STRING RegistryPath,PDRIVER_ADD_DEVICE AddDevice,PDRIVER_UNLOAD Unload);
NTSTATUS T70PortClsFinishUnload(T70_PORTCLS_OWNER *Owner);
#ifdef __cplusplus
}
#endif
#endif
