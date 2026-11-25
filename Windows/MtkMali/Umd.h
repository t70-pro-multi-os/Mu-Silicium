/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_UMD_H
#define T70_MALI_UMD_H
#ifndef D3D_UMD_INTERFACE_VERSION
#define D3D_UMD_INTERFACE_VERSION 0x10000
#endif
#include <windows.h>
#include <d3d10_1.h> /* SDK constant macros prevent WDK external data definitions in C. */
#include <d3d10umddi.h>
#include "Commands.h"
#include "Shader/ShaderCompiler.h"
#ifdef __cplusplus
extern "C" {
#endif
/* An unregistered D3D10 resource/transfer/RTV/clear and VS/PS component, not OpenAdapter or a
   complete device DDI. The parent must supply the qualified adapter's actual
   LUID and caps; runtime HANDLEs are never cast to KMT handles. It owns a
   separate KMT device/context, so swap chains, runtime resource sharing,
   queries and interop are not implemented or advertised by this component.
   All OS backing is CreateResource|CreateShared|NtSecuritySharing. No global
   share handles or application-visible NT handles are produced.
   Storage is zeroed, caller-owned and at least DeviceBytes. After ANY Init
   attempt, Close must succeed before storage is freed/reused. Close polls;
   ERROR_BUSY retains all objects after pending GPU/paging or failed OS cleanup.
   The parent must stop other DDI calls before the final Close. Resource/view
   destruction itself is serialized and safe while GPU work is outstanding. */
SIZE_T MtkMaliUmdDeviceBytes(void);
HRESULT MtkMaliUmdInitialize(void *Storage,LUID Adapter,const MALI_CAPS *Caps,
    UINT Slot,D3D10DDI_HRTCORELAYER Core,PFND3D10DDI_SETERROR_CB SetError);
HRESULT MtkMaliUmdClose(D3D10DDI_HDEVICE Device);
HRESULT MtkMaliUmdFailureReason(D3D10DDI_HDEVICE Device);
/* Installs resource/transfer/RTV/clear/Flush entries and a narrow
   triangle-list Draw plus IA/RT/raster/blend/depth/viewport state callbacks.
   Draw uses a single-sample linear 32-bit UNORM color target and the built-in
   vertex-ID shader shape. Its internal pre-frame shader loads the existing
   target before drawing. This does not register a device or advertise a
   complete DDI. */
void MtkMaliUmdResourceFunctions(D3D10DDI_DEVICEFUNCS *Functions);
/* Installs shader lifetime/selection callbacks used by the narrow Draw path.
   Call alongside MtkMaliUmdResourceFunctions; this does not register an
   adapter, device, swap chain, or complete DDI. */
void MtkMaliUmdShaderFunctions(D3D10DDI_DEVICEFUNCS *Functions);
/* Compiles a snapshot of the currently selected VS+PS. The caller owns out,
   supplies Types from its actual input layout/attachments, frees out with
   t70_shader_free_pipeline(), and uploads it before submitting
   GPU jobs. This function performs CPU compilation only and returns errors;
   a parent Draw callback must report its own errors to the runtime. */
HRESULT MtkMaliUmdCompileBoundShaders(D3D10DDI_HDEVICE Device,
    const struct t70_shader_io_types *Types,struct t70_shader_pipeline *Out);
/* A unique, device-owned upload. Compiled stays valid until ReleaseProgram;
   shader destruction/rebinding does not alter it. Shader[0..3] are actual arch-9
   program descriptor addresses: point position, non-point position, optional
   varying (zero if unused), fragment. Descriptors is a read-only/NX mapping.
   Code ranges are page-sized
   read/execute GPU mappings, never GPU-writable. Upload queues paging waits
   on this component's context but submits no GPU job. The parent must use
   that context and serialize submissions against ReleaseProgram. It can
   reuse one upload across draws and keeps ownership until its last use.
   ReleaseProgram consumes/zeros the handle, then waits for device idle and
   disposes the code; busy/error cleanup is retained for Flush/Close retry.
   Live uploads block Close. CPU metadata/code must not be modified. */
enum { MaliUmdPositionPoints=0, MaliUmdPositionTriangles=1, MaliUmdVarying=2, MaliUmdFragment=3 };
typedef struct {
    uint64_t Id;
    const void *Owner;
    const struct t70_shader_pipeline *Compiled;
    MALI_COMMAND_RESOURCE Code[2]; /* VS, PS */
    MALI_COMMAND_RESOURCE Descriptors;
    uint64_t Shader[4];
} MALI_UMD_PROGRAM;
HRESULT MtkMaliUmdUploadBoundShaders(D3D10DDI_HDEVICE Device,
    const struct t70_shader_io_types *Types,MALI_UMD_PROGRAM *Out);
HRESULT MtkMaliUmdReleaseProgram(D3D10DDI_HDEVICE Device,MALI_UMD_PROGRAM *Program);
/* Snapshot a live owned allocation's full page range. Access is Read or
   Read|Write. A discard rename invalidates that snapshot; SubmitRaster rejects
   old addresses against the current bindings. No KMT/NT share handle escapes. */
HRESULT MtkMaliUmdResourceRange(D3D10DDI_HDEVICE Device,D3D10DDI_HRESOURCE Resource,
    UINT Access,MALI_COMMAND_RESOURCE *Out);
typedef struct {
    D3D10DDI_HRESOURCE Resource;
    UINT Access;
} MALI_UMD_RASTER_RESOURCE;
/* Submit actual malloc-vertex/fragment jobs on this device's context. Bindings
   include ALL indirect input/output/descriptor/heap buffers. Raster addresses
   come from current ResourceRange snapshots. This layer supplies the uploaded
   position/varying/fragment programs and compiler packet stride, and derives
   the secondary shader from metadata; callers do not supply those addresses.
   Position's resource/TLS/FAU environment is shared by its varying program.
   The narrow Draw callback produces framebuffer, tiler, TLS, and FAU
   descriptors for its one-RT triangle-list case; general D3D state, Present,
   adapter/device registration, and a complete DDI are not implemented.
   All bindings and program objects survive source destruction/renaming and
   cleanup failures until actual event completion. Failed post-submit event
   queuing removes the device while retaining every in-flight object.
   This is the submission layer used by the narrow D3D Draw callback. */
HRESULT MtkMaliUmdSubmitRaster(D3D10DDI_HDEVICE Device,const MALI_UMD_PROGRAM *Program,
    const MALI_COMMAND_RASTER *Raster,const MALI_UMD_RASTER_RESOURCE *Resources,
    UINT ResourceCount);


#ifdef __cplusplus
}
#endif
#endif
