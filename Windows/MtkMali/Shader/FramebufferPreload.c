/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FramebufferPreload.h"
#include "ShaderCompiler.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/lib/pan_fb.h"

struct nir_shader *t70_shader_build_framebuffer_preload_nir(uint32_t format,
                                                            uint32_t array_layers)
{
   /* The accepted UNORM formats all use F32 NIR; the bound view carries the
    * concrete channel ordering and X/alpha interpretation. */
   switch (format) {
   case T70_SHADER_FB_RGBA8_UNORM:
   case T70_SHADER_FB_BGRA8_UNORM:
   case T70_SHADER_FB_BGRX8_UNORM:
      break;
   default:
      return NULL;
   }

   if (array_layers == 0 || array_layers > T70_SHADER_FB_PRELOAD_MAX_LAYERS)
      return NULL;

   const struct pan_fb_shader_key key = {
      .rts = {{
         .in_bounds_op = PAN_FB_SHADER_LOAD_IMAGE,
         .border_op = PAN_FB_SHADER_LOAD_IMAGE,
         .in_bounds_msaa = PAN_FB_MSAA_COPY_SINGLE,
         .border_msaa = PAN_FB_MSAA_COPY_SINGLE,
         .image_dim = MALI_TEXTURE_DIMENSION_2D,
         .image_is_array = array_layers > 1,
         .data_type = PAN_FB_SHADER_DATA_TYPE_F32,
      }},
      .fb_sample_count = 1,
   };
   return GENX(pan_get_fb_shader)(
      &key, pan_get_nir_shader_compiler_options(9, MESA_SHADER_FRAGMENT, false));
}
