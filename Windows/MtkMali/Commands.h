/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_COMMANDS_H
#define T70_MALI_COMMANDS_H
#include "Execution.h"
#define MALI_COMMAND_MAX_DMA (64u*1024u)
#define MALI_COMMAND_MAX_JOBS 64u
#define MALI_COMMAND_RESOURCE_VERSION 2u
#define MALI_COMMAND_MAX_RESOURCES 128u
#define MALI_COMMAND_MAX_RESOURCE_PAGES 131072u
#define MALI_COMMAND_PRIVATE_BYTES(S,R) (16u+16u*(S)+16u*(R))
#define MALI_COMMAND_MAX_PRIVATE MALI_COMMAND_PRIVATE_BYTES(MALI_EXEC_MAX_STEPS,MALI_COMMAND_MAX_RESOURCES)
#define MALI_COMMAND_STRIDE 128u
#define MALI_COMMAND_MAX_JOB_BYTES 384u
#define MALI_COMMAND_SHADOW_BYTES (MALI_COMMAND_MAX_JOBS*MALI_COMMAND_MAX_JOB_BYTES)
typedef MALI_RESULT (*MALI_COMMAND_PIN_WRITE)(void *, uint64_t, uint32_t);
typedef MALI_RESULT (*MALI_COMMAND_PIN_RESOURCE)(void *, uint64_t, uint32_t, uint32_t);
/* v2: 16-byte version/bytes/stage-count/resource-count prefix, stage entries,
   then these 16-byte GPUVA ranges. Access uses MaliPteRead/Write/Execute only.
   Ranges are page-aligned, disjoint, readable and W^X, up to 512 MiB total. */
typedef struct {uint64_t Va;uint32_t Bytes,Access;} MALI_COMMAND_RESOURCE;
typedef struct {
    uint64_t Source;
    uint32_t Bytes,Offset;
    uint32_t Words[MALI_COMMAND_MAX_JOB_BYTES/4u];
} MALI_COMMAND_JOB;
typedef struct {
    MALI_COMMAND_JOB Jobs[MALI_COMMAND_MAX_JOBS];
    uint32_t First[MALI_EXEC_MAX_STEPS], Count[MALI_EXEC_MAX_STEPS];
    MALI_EXEC_PACKET Packet;
    uint32_t JobCount, WriteCount, ResourceCount, ComputeCount, ShadowUsed;
    MALI_COMMAND_RESOURCE Resources[MALI_COMMAND_MAX_RESOURCES];
} MALI_COMMAND_WORKSPACE;
/* Input and Private are stable KMD-owned snapshots, NOT user mappings. The
   caller serializes Workspace and holds all root/table/mapping lifetimes.
   PinWrite must prove process ownership, writable resident data, and absence
   of PHYSICAL aliases to any command, shadow, page table or privileged BO.
   It retains each backing allocation until the submission's actual release.
   All callbacks are required; the parser cannot establish VidMm residency.

   Version 1 retains immediate 8/16/32/64 writes, system timestamps and cache
   maintenance. Version 2 also accepts arch-9 compute and fragment jobs with
   explicit buffers. Fragment accepts the eight-target MFBD layout, optional
   ZS/CRC extension, inclusive tile bounds and five render orders. Tile-enable
   maps and scissor arrays remain rejected. Version 2 also accepts arch-9
   malloc-vertex, tiler and fullscreen records (384/256/128 bytes), including
   indexed draws and optional position/varying/fragment shader environments.
   Records retain their full payload and use variable 128-byte-aligned offsets.
   ZERO and
   cycle-counter writes are rejected (unproven width/enable prerequisites).
   At most 64 jobs across 16 stages; chains are rebuilt at 128-byte alignment,
   with unique indices and strict dependencies. No input next/index survives.
   Shadow must be private nonpaged and inaccessible to CPU/UMD writes.
   Compute/vertex/fragment shaders, render targets and indirect descriptors remain untrusted:
   only the job payload and referenced descriptor/buffer extents are checked. Their containment
   is the private GPU MMU root, NOT instruction or descriptor validation. The
   root must expose only retained process-owned buffers and job/status pages,
   never page tables, privileged BOs or MMIO. Status pages are GPU-writable;
   arbitrary shaders can damage their own chain and must be handled as faults.
   PinResource verifies exact OS GPUVA/PFN identity and requested permissions,
   retains full-range MDLs and rejects privileged and W^X physical aliases.
   It is untouched on failure. Output and Workspace.ShadowUsed are valid only
   on MaliOk; ShadowUsed is the exact padded command extent to seal/submit. */
MALI_RESULT MaliCommandsPrepare(const MALI_CAPS *Caps,
    const void *Input, uint64_t InputVa, uint32_t InputBytes,
    const void *Private, uint32_t PrivateBytes,
    void *Shadow, uint64_t ShadowVa, uint32_t ShadowBytes,
    MALI_COMMAND_WORKSPACE *Workspace, MALI_COMMAND_PIN_WRITE PinWrite, void *Context);
MALI_RESULT MaliCommandsPrepareResources(const MALI_CAPS *Caps,
    const void *Input, uint64_t InputVa, uint32_t InputBytes,
    const void *Private, uint32_t PrivateBytes,
    void *Shadow, uint64_t ShadowVa, uint32_t ShadowBytes,
    MALI_COMMAND_WORKSPACE *Workspace, MALI_COMMAND_PIN_WRITE PinWrite,
    MALI_COMMAND_PIN_RESOURCE PinResource, void *Context);
/* Arch-9 shader program descriptor. This is a CPU encoder, not a GPU job.
   Stage is 2 (fragment) or 3 (vertex). Preload is the compiler's R48..R63 mask;
   WorkRegisters is at most 64. WarpLimit is 0..3 for vertex, zero for fragment.
   FlushToZero is 0 (preserve), 1 (DX11) or 2 (all). HelperThreads is fragment-only.
   Caller proves CodeVa points into its retained read/execute allocation and
   owns residency, cache visibility and all indirect job/descriptors. Input,
   caps and the exact 32-byte output are disjoint; failure leaves output intact. */
#define MALI_COMMAND_SHADER_BYTES 32u
typedef struct {
    uint32_t Bytes,Stage,WorkRegisters,WarpLimit,FlushToZero,HelperThreads;
    uint64_t CodeVa,Preload;
} MALI_COMMAND_SHADER;
MALI_RESULT MaliCommandsBuildShader(const MALI_CAPS *Caps,const MALI_COMMAND_SHADER *Shader,
    void *Output,uint32_t OutputBytes);
/* Arch-9 malloc-vertex + fragment submission, in two ordered GPU stages.
   Primitive/Flags/Scissor/Tiles use the actual arch-9 packed scalar fields.
   All addresses, including indirect descriptors, belong to the retained
   resource list. Shader descriptors and their executable code are separate
   ranges. This encodes jobs, not framebuffer/resource/preload descriptors.
   Caller supplies those from its draw state and keeps every indirect buffer
   resident and immutable until completion. The KMD validates and confines the
   same list. Output/input ranges are disjoint; failure changes no output. */
#define MALI_COMMAND_RASTER_JOB_BYTES 512u
#define MALI_COMMAND_RASTER_PRIVATE_BYTES(R) MALI_COMMAND_PRIVATE_BYTES(2u,(R))
typedef struct {
    uint32_t AttributeOffset,FauCount;
    uint64_t Resources,Shader,ThreadStorage,Fau;
} MALI_COMMAND_ENVIRONMENT;
typedef struct {
    uint32_t Bytes,VertexSlot,FragmentSlot,Primitive;
    uint32_t BaseVertex,BaseInstance,IndexCount,InstanceCount,VaryingStride;
    uint32_t Scissor[2],PrimitiveSize,Flags[2],MinZ,MaxZ,BlendCount,Tiles[2];
    uint64_t Tiler,Indices,DepthStencil,Blend,Occlusion,Framebuffer;
    MALI_COMMAND_ENVIRONMENT Position,Varying,Fragment;
} MALI_COMMAND_RASTER;
MALI_RESULT MaliCommandsBuildRaster(const MALI_CAPS *Caps,const MALI_COMMAND_RASTER *Raster,
    const MALI_COMMAND_RESOURCE *Resources,uint32_t ResourceCount,
    void *Command,uint64_t CommandVa,uint32_t CommandBytes,
    void *Private,uint32_t PrivateBytes);
/* Full-surface, single-sample linear 32-bit UNORM clear. Format is the
   D3DDDIFORMAT value used by MALI_ALLOCATION_DESC, NOT DXGI_FORMAT.
   Color contains IEEE binary32 RGBA bit patterns; conversion uses integer
   arithmetic, including binary32 multiply and nearest-even quantization.
   The three outputs and both inputs must be stable, disjoint CPU ranges.
   Command/descriptor/private sizes must match the constants below. TargetBytes
   must equal page-rounded Pitch*Height; its allocation must explicitly include
   that padding. All GPU ranges must be disjoint; descriptor/target/heap are
   process-owned, readable/writable, non-executable buffers. Heap is at least
   the fixed 256 KiB chunk below. Every generated descriptor byte is initialized.
   Caller owns allocation, GPUVA residency, CPU cache flush and synchronization;
   no buffer can be reused until actual completion/reset releases its lease.
   This builds real arch-9 fixed-function clear descriptors and v2 submission
   data, not a registered D3D callback or a hardware pixel-output guarantee.
   Output bytes are unchanged on failure. */
#define MALI_COMMAND_CLEAR_JOB_BYTES 128u
#define MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES 4096u
#define MALI_COMMAND_CLEAR_PRIVATE_BYTES MALI_COMMAND_PRIVATE_BYTES(1u,3u)
#define MALI_COMMAND_CLEAR_HEAP_BYTES (256u*1024u)
enum { MaliClearBgra8=21, MaliClearBgrx8=22, MaliClearRgba8=32, MaliClearRgbx8=33 };
typedef struct {
    uint32_t Bytes,Width,Height,Pitch,Format,Slot,Color[4];
    uint64_t TargetVa;
    uint32_t TargetBytes,Reserved;
    uint64_t HeapVa;
} MALI_COMMAND_CLEAR;
MALI_RESULT MaliCommandsBuildClear(const MALI_CAPS *Caps,const MALI_COMMAND_CLEAR *Clear,
    void *Command,uint64_t CommandVa,uint32_t CommandBytes,
    void *Descriptors,uint64_t DescriptorVa,uint32_t DescriptorBytes,
    void *Private,uint32_t PrivateBytes);
/* Arch-9 single-sample linear RGBA8/BGRA8/BGRX8 framebuffer preload. This
   emits an MFBD and its pre-frame LOAD_IMAGE shader descriptors only; the
   caller supplies the normal render job and retains every referenced range.
   Format uses the D3DDDIFORMAT values above. ShaderProgramVa addresses a
   32-byte Mali shader-program descriptor, not machine code. FauCount is in
   64-bit FAU words. ThreadStorageBytes describes
   the caller-owned TLS allocation for range/overlap checks. The descriptor
   block reuses Clear's 4 KiB layout: pre/post DCD slots at 768/896/1024,
   sampler at 1152, texture at 1184, plane at 1216, 4-entry resource table at
   1280, blend at 1344, and depth/stencil at 1376. DCD0 loads every pixel;
   DCD1 and DCD2 remain zero. All output bytes are unchanged on failure.
   This descriptor encoding alone does not prove end-to-end pixel preservation. */
#define MALI_COMMAND_PRELOAD_DESCRIPTOR_BYTES MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES
#define MALI_COMMAND_PRELOAD_DCD_BYTES 128u
#define MALI_COMMAND_PRELOAD_DCD0_OFFSET 768u
#define MALI_COMMAND_PRELOAD_DCD1_OFFSET 896u
#define MALI_COMMAND_PRELOAD_DCD2_OFFSET 1024u
#define MALI_COMMAND_PRELOAD_SAMPLER_OFFSET 1152u
#define MALI_COMMAND_PRELOAD_TEXTURE_OFFSET 1184u
#define MALI_COMMAND_PRELOAD_PLANE_OFFSET 1216u
#define MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET 1280u
#define MALI_COMMAND_PRELOAD_BLEND_OFFSET 1344u
#define MALI_COMMAND_PRELOAD_DEPTH_STENCIL_OFFSET 1376u
typedef struct {
    uint32_t Bytes,Width,Height,Pitch,Format,TargetBytes,FauCount,ThreadStorageBytes;
    uint32_t Reserved[2];
    uint64_t TargetVa,HeapVa,ShaderProgramVa,FauVa,ThreadStorageVa;
} MALI_COMMAND_PRELOAD;
MALI_RESULT MaliCommandsBuildPreload(const MALI_CAPS *Caps,const MALI_COMMAND_PRELOAD *Preload,
    void *Descriptors,uint64_t DescriptorVa,uint32_t DescriptorBytes);
#endif
