/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShaderCompiler.h"
#include <type_traits>
static_assert(std::is_standard_layout<t70_shader_request>::value,"C-compatible request");
static_assert(std::is_standard_layout<t70_shader>::value,"C-compatible output");
static_assert(std::is_standard_layout<t70_shader_pipeline>::value,"C-compatible pipeline");
static_assert(offsetof(t70_shader_pipeline,pixel)==sizeof(t70_shader),"pipeline stage offset");
static_assert(sizeof(t70_shader_pipeline)==2*sizeof(t70_shader)+8,"pipeline metadata ABI");
static_assert(sizeof(t70_shader_varying)==24,"fixed width varying metadata");
static_assert(std::is_standard_layout<t70_shader_token_request>::value,"C-compatible DDI request");
static_assert(std::is_standard_layout<t70_shader_fb_preload_request>::value,"C-compatible framebuffer request");
static_assert(sizeof(t70_shader_fb_preload_sysvals)==T70_SHADER_FB_PRELOAD_SYSVAL_BYTES,"inclusive render-area ABI");
static_assert(offsetof(t70_shader_fb_preload_sysvals,max_x)==4 &&
              offsetof(t70_shader_fb_preload_sysvals,max_y)==6,"render-area component order");
static_assert(offsetof(t70_shader_fb_preload_request,array_layers)==16,"framebuffer layer-count offset");
static_assert(sizeof(t70_shader_fb_preload_request)==24,"framebuffer request ABI");
static_assert(sizeof(t70_shader_signature_entry)==12,"portable DDI signature");
static_assert(offsetof(t70_shader_token_request,tokens)==16,"DDI token offset");
static_assert(sizeof(t70_shader_token_request)==72,"ARM64 DDI request ABI");
extern "C" unsigned t70_shader_cpp_abi(void);
unsigned t70_shader_cpp_abi(void) { return t70_shader_abi_version(); }
