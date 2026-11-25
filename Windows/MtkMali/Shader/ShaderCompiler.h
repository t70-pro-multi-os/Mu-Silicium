/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_SHADER_COMPILER_H
#define T70_SHADER_COMPILER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define T70_SHADER_ABI 1u
#define T70_SHADER_GPU_ID 0x90930000u
#define T70_SHADER_MAX_INPUT (1024u*1024u)
#define T70_SHADER_MAX_SIGNATURE 256u
enum t70_shader_stage { T70_SHADER_PIXEL=0, T70_SHADER_VERTEX=1 };
#define T70_SHADER_VARYINGS 70u
#define T70_SHADER_FLAG_IDVS 1u
#define T70_SHADER_FLAG_SECONDARY 2u
#define T70_SHADER_FLAG_EXTENDED_FIFO 4u
#define T70_SHADER_FLAG_DISCARD 8u
#define T70_SHADER_FLAG_DEPTH 16u
#define T70_SHADER_FLAG_COVERAGE 32u
#define T70_SHADER_FLAG_BARRIER 64u
#define T70_SHADER_FLAG_FLAT 128u
#define T70_SHADER_FLAG_VARYING_DESCRIPTORS 256u
#define T70_SHADER_FLAG_FTZ32 512u
#define T70_SHADER_FLAG_FTZ16 1024u
struct t70_shader_varying {
   uint32_t location, components, kind, bits, section;
   int32_t offset;
};
enum t70_shader_status {
   T70_SHADER_OK=0, T70_SHADER_INVALID=-1, T70_SHADER_UNSUPPORTED=-2,
   T70_SHADER_TRANSLATION=-3, T70_SHADER_MEMORY=-4
};
/* Pure CPU compilation. No device, file, KMT, or GPU submission is performed.
 * Caller supplies an empty output and frees it with t70_shader_free().
 * Complete DXBC containers (SM4/5 VS/PS) are accepted, not raw DDI tokens.
 * Code is hardware-specific Valhall arch-9 code, not executable CPU code. */
struct t70_shader_request {
   uint32_t abi, gpu_id, stage, reserved;
   const void *dxbc;
   size_t dxbc_size;
};
enum t70_shader_framebuffer_format {
   T70_SHADER_FB_RGBA8_UNORM=1,
   T70_SHADER_FB_BGRA8_UNORM=2,
   T70_SHADER_FB_BGRX8_UNORM=3,
};
#define T70_SHADER_FB_PRELOAD_MAX_LAYERS 256u
#define T70_SHADER_FB_PRELOAD_FAU_WORDS 2u
#define T70_SHADER_FB_PRELOAD_SYSVAL_BYTES 8u
#define T70_SHADER_FB_PRELOAD_TEXTURE_TABLE 0u
#define T70_SHADER_FB_PRELOAD_TEXTURE_INDEX 1u
#define T70_SHADER_FB_PRELOAD_SAMPLER_TABLE 0u
#define T70_SHADER_FB_PRELOAD_SAMPLER_INDEX 0u
/* Bounds are inclusive, matching pan_fb_bbox/render_area_px. */
struct t70_shader_fb_preload_sysvals {
   uint16_t min_x, min_y, max_x, max_y;
};
struct t70_shader_fb_preload_request {
   uint32_t abi, gpu_id, format, sample_count, array_layers, reserved;
};
/* D3D10/11 stage signatures contain SB system-value numbers (0..10),
 * register numbers and xyzw masks, without HLSL semantic names or types. */
struct t70_shader_signature_entry {
   uint32_t system_value, register_index, mask;
};
/* Specialization from the bound input layout and render-target formats.
 * A clear bit denotes float/normalized data. Signed/unsigned bits are exclusive. */
struct t70_shader_io_types {
   uint32_t vertex_uint, vertex_sint, target_uint, target_sint;
};
struct t70_shader_token_request {
   uint32_t abi, gpu_id, stage, reserved;
   const void *tokens;
   size_t tokens_size;
   const struct t70_shader_signature_entry *inputs, *outputs;
   uint32_t input_count, output_count;
   struct t70_shader_io_types types;
};
struct t70_shader {
   void *binary;
   size_t binary_size;
   void *spirv;
   size_t spirv_size;
   uint32_t stage, arch, work_registers, tls_size, wls_size;
   uint64_t preload, outputs_written;
   uint32_t fau_count, fau_reserved, fau_words[128], fau_const_mask[4];
   uint32_t attributes_read, attribute_count, varying_stride, cb_mask, sampler_mask, texture_mask[4];
   uint32_t flags, secondary_offset, secondary_registers, no_point_size_offset;
   uint64_t secondary_preload;
   uint32_t varying_count;
   struct t70_shader_varying varyings[T70_SHADER_VARYINGS];
   uint32_t blend_type[8], blend_return_offset[8];
};
/* Both stages share the vertex packet layout. Feed noperspective_mask into
 * vertex FAU word 10 at draw time. The standalone API cannot link two stages. */
struct t70_shader_pipeline {
   struct t70_shader vertex, pixel;
   uint32_t varying_stride, noperspective_mask;
};
#if defined(_WIN32) && defined(T70_SHADER_BUILD_DLL)
#define T70_SHADER_API __declspec(dllexport)
#elif defined(_WIN32) && defined(T70_SHADER_USE_DLL)
#define T70_SHADER_API __declspec(dllimport)
#else
#define T70_SHADER_API
#endif
T70_SHADER_API uint32_t t70_shader_abi_version(void);
T70_SHADER_API int t70_shader_compile(const struct t70_shader_request *request,
                       struct t70_shader *out, char *message, size_t message_size);
/* Generates Mesa's arch-9 LOAD_IMAGE fragment shader for one single-sample
 * RGBA8/BGRA8/BGRX8 target. array_layers=1 is 2D; 2..256 uses frame_arg[7:0].
 * FAU words 0..1 contain t70_shader_fb_preload_sysvals. The shader reads its
 * source from table 0 texture 1 and sampler 0. The existing D3D texture/sampler
 * masks do not describe these fixed internal slots. Output is freed with
 * t70_shader_free(); failures leave the caller's output unchanged. */
T70_SHADER_API int t70_shader_compile_framebuffer_preload(
                       const struct t70_shader_fb_preload_request *request,
                       struct t70_shader *out, char *message, size_t message_size);
T70_SHADER_API void t70_shader_free(struct t70_shader *shader);
/* DXBC semantic names/indices link VS outputs to PS inputs, including packed
 * component moves. Incompatible signatures fail without changing out. */
T70_SHADER_API int t70_shader_compile_pipeline(const struct t70_shader_request *vertex,
                        const struct t70_shader_request *pixel,
                        struct t70_shader_pipeline *out, char *message, size_t message_size);
T70_SHADER_API void t70_shader_free_pipeline(struct t70_shader_pipeline *pipeline);
/* Raw little-endian runtime tokens, including version and word count.
 * Optional full signatures may include unused declarations. Linking preserves
 * the register/component coordinates already assigned by the D3D runtime.
 * Types must match the bound input layout/attachments before GPU use.
 * Inputs remain caller-owned; outputs use the existing free functions. */
T70_SHADER_API int t70_shader_compile_tokens(const struct t70_shader_token_request *request,
                        struct t70_shader *out, char *message, size_t message_size);
T70_SHADER_API int t70_shader_compile_token_pipeline(const struct t70_shader_token_request *vertex,
                        const struct t70_shader_token_request *pixel,
                        struct t70_shader_pipeline *out, char *message, size_t message_size);

#ifdef __cplusplus
}
#endif
#endif
