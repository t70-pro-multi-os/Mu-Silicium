/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* These words are copied into NIR intrinsics or 64-bit compiler hash keys. */
#include "panfrost/compiler/pan_nir.h"
#include "panfrost/compiler/bifrost/compiler.h"
#include "ShaderCompiler.h"
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
EXPORT volatile unsigned T70ShaderLayoutAssertions;
#define CHECK(x) do { ++T70ShaderLayoutAssertions; if (!(x)) return __LINE__; } while (0)
static_assert(sizeof(struct pan_bi_tex_flags)==4, "Bifrost flags word");
static_assert(sizeof(struct pan_va_tex_flags)==4, "Valhall flags word");
static_assert(sizeof(bi_index)==8, "compiler hash key");
static_assert(sizeof(struct t70_shader_request)==32, "64-bit public request ABI");
static_assert(offsetof(struct t70_shader_request,dxbc)==16, "DXBC pointer ABI");
static_assert(offsetof(struct t70_shader_request,dxbc_size)==24, "DXBC size ABI");
static_assert(offsetof(struct t70_shader_pipeline,pixel)==sizeof(struct t70_shader), "pipeline stage ABI");
static_assert(sizeof(struct t70_shader_pipeline)==2*sizeof(struct t70_shader)+8, "pipeline metadata ABI");
static_assert(sizeof(struct t70_shader_signature_entry)==12,"DDI signature ABI");
static_assert(sizeof(struct t70_shader_token_request)==72,"DDI request ABI");
static_assert(offsetof(struct t70_shader_token_request,tokens)==16,"DDI token ABI");
EXPORT int DriverEntry(void);
EXPORT int DriverEntry(void)
{
   CHECK(sizeof(struct pan_bi_tex_flags)==4);
   CHECK(sizeof(struct pan_va_tex_flags)==4);
   CHECK(sizeof(bi_index)==8);
   for (unsigned i=0;i<33;i++) {
      uint32_t word=i==32?UINT32_MAX:UINT32_C(1)<<i;
      struct pan_bi_tex_flags bi={0};
      bi.skip=(word>>0)&1; bi.explicit_lod=(word>>1)&1;
      bi._pad=(word>>2)&0x3fff; bi.sampler_idx=(word>>16)&255;
      bi.texture_idx=(word>>24)&255;
      uint32_t actual=0; memcpy(&actual,&bi,4); CHECK(actual==word);
      memset(&bi,0,sizeof(bi)); memcpy(&bi,&word,4);
      CHECK(bi.skip==((word>>0)&1) && bi.explicit_lod==((word>>1)&1));
      CHECK(bi.sampler_idx==((word>>16)&255) && bi.texture_idx==(word>>24));
      struct pan_va_tex_flags va={0};
      va.wide_indices=(word>>0)&1; va.array_enable=(word>>1)&1;
      va.texel_offset=(word>>2)&1; va.compare_enable=(word>>3)&1;
      va.lod_mode=(word>>4)&7; va.derivative_enable=(word>>7)&1;
      va.force_delta_enable=(word>>8)&1; va.lod_bias_disable=(word>>9)&1;
      va.lod_clamp_disable=(word>>10)&1; va.projection_enable=(word>>11)&1;
      va.var_sample_loc=(enum pan_bi_sample_loc)((word>>12)&3);
      va.var_buf_src_f16=(word>>14)&1; va._pad=(word>>15)&0x1ffff;
      memcpy(&actual,&va,4); CHECK(actual==word);
      memset(&va,0,sizeof(va)); memcpy(&va,&word,4);
      CHECK(va.lod_mode==((word>>4)&7) && va.var_sample_loc==((word>>12)&3));
      CHECK(va.var_buf_src_f16==((word>>14)&1) && va.projection_enable==((word>>11)&1));
      bi_index idx={0};
      idx.value=0x89abcdef; idx.abs=(word>>0)&1; idx.neg=(word>>1)&1;
      idx.discard=(word>>2)&1; idx.swizzle=(enum bi_swizzle)((word>>3)&31);
      idx.offset=(word>>8)&7; idx.type=(enum bi_index_type)((word>>11)&7);
      idx.kill_ssa=(word>>14)&1; idx.memory=(word>>15)&1; idx.padding=word>>16;
      uint64_t key=0; memcpy(&key,&idx,8);
      CHECK(key==((uint64_t)word<<32|UINT64_C(0x89abcdef)));
      memset(&idx,0,sizeof(idx)); memcpy(&idx,&key,8);
      CHECK(idx.value==0x89abcdef && idx.swizzle==((word>>3)&31));
      CHECK(idx.type==((word>>11)&7) && idx.memory==((word>>15)&1));
   }
   return 0;
}
#ifdef T70_LAYOUT_NATIVE
int main(void) {
   int result=DriverEntry();
   if (result) fprintf(stderr,"shader layout failed at line %d\n",result);
   else printf("shader layouts: %u checks passed\n",T70ShaderLayoutAssertions);
   return result?1:0;
}
#endif
