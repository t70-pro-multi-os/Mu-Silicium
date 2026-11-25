/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Uses vkd3d-shader (LGPL-2.1-or-later) and Mesa (MIT); neither is copied here. */
#include "ShaderCompiler.h"
#include "FramebufferPreload.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkd3d_shader.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/glsl_types.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/compiler/pan_nir.h"
#include "panfrost/model/pan_model.h"

static int fail(int code, char *message, size_t size, const char *format, ...)
{
   if (message && size) {
      va_list args;
      va_start(args,format);
      vsnprintf(message,size,format,args);
      va_end(args);
   }
   return code;
}
static uint32_t le32(const void *ptr)
{
   const uint8_t *p=ptr;
   return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int validate_dxbc(const struct t70_shader_request *r)
{
   const uint8_t *p=r->dxbc;
   size_t n=r->dxbc_size;
   if (!p || n<36 || n>T70_SHADER_MAX_INPUT || (n&3) || memcmp(p,"DXBC",4) ||
       le32(p+20)!=1 || le32(p+24)!=n)
      return T70_SHADER_INVALID;
   uint32_t count=le32(p+28), found=0;
   if (!count || count>256 || count>(n-32)/4)
      return T70_SHADER_INVALID;
   for (uint32_t i=0;i<count;i++) {
      uint32_t offset=le32(p+32+4*i);
      if ((offset&3) || offset<32+4*count || offset>n-8)
         return T70_SHADER_INVALID;
      uint32_t size=le32(p+offset+4);
      if (size>n-offset-8)
         return T70_SHADER_INVALID;
      if (!memcmp(p+offset,"SHDR",4) || !memcmp(p+offset,"SHEX",4)) {
         if (++found!=1 || size<8 || (size&3) || le32(p+offset+12)!=size/4)
            return T70_SHADER_INVALID;
         uint32_t version=le32(p+offset+8);
         if ((version>>16)!=r->stage || ((version>>4)&15)<4 ||
             ((version>>4)&15)>5 || (version&15)>1 ||
             (((version>>4)&15)==5 && (version&15)))
            return T70_SHADER_UNSUPPORTED;
      }
   }
   return found==1 ? T70_SHADER_OK:T70_SHADER_UNSUPPORTED;
}
struct link_lane {
   uint8_t valid, location, component;
};
struct varying_link {
   struct link_lane lanes[32][4];
};
static int validate_signature(const struct vkd3d_shader_signature *signature)
{
   if (signature->element_count>256) return T70_SHADER_UNSUPPORTED;
   uint32_t occupied[32]={0};
   for (unsigned i=0;i<signature->element_count;i++) {
      const struct vkd3d_shader_signature_element *e=&signature->elements[i];
      if (e->sysval_semantic!=VKD3D_SHADER_SV_NONE) continue;
      if (e->stream_index || e->register_index>=32 || !e->mask || (e->mask&~15u) ||
          (e->used_mask&~e->mask) || !e->semantic_name || !*e->semantic_name ||
          (occupied[e->register_index]&e->mask))
         return T70_SHADER_INVALID;
      if (e->component_type!=VKD3D_SHADER_COMPONENT_FLOAT &&
          e->component_type!=VKD3D_SHADER_COMPONENT_INT &&
          e->component_type!=VKD3D_SHADER_COMPONENT_UINT)
         return T70_SHADER_UNSUPPORTED;
      occupied[e->register_index]|=e->mask;
      const struct vkd3d_shader_signature_element *first=
         vkd3d_shader_find_signature_element(signature,e->semantic_name,e->semantic_index,0);
      if (first!=e) return T70_SHADER_INVALID;
   }
   return T70_SHADER_OK;
}
static int build_link(const struct t70_shader_request *vertex,
                      const struct t70_shader_request *pixel, struct varying_link *link,
                      bool registers, char *message, size_t message_size)
{
   const struct t70_shader_request *requests[2]={vertex,pixel};
   struct vkd3d_shader_scan_signature_info signatures[2]={0};
   int status=T70_SHADER_OK;
   for (unsigned i=0;i<2;i++) {
      signatures[i].type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
      struct vkd3d_shader_compile_info ci={
         .type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO,.next=&signatures[i],
         .source={requests[i]->dxbc,requests[i]->dxbc_size},
         .source_type=VKD3D_SHADER_SOURCE_DXBC_TPF,
         .target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY,.log_level=VKD3D_SHADER_LOG_ERROR,
      };
      char *messages=NULL;
      int result=vkd3d_shader_scan(&ci,&messages);
      if (result<0)
         status=fail(T70_SHADER_TRANSLATION,message,message_size,"%s",
                     messages?messages:"shader signature scan failed");
      vkd3d_shader_free_messages(messages);
      if (status) goto cleanup;
   }
   const struct vkd3d_shader_signature *vs=&signatures[0].output,*ps=&signatures[1].input;
   status=validate_signature(vs);
   if (!status) status=validate_signature(ps);
   if (status) {
      fail(status,message,message_size,"invalid or unsupported inter-stage signature");
      goto cleanup;
   }
   if (registers) {
      /* DDI bytecode is prelinked. Semantic packing would move live values
       * when VS and PS declarations cover different subsets of a register. */
      for (unsigned i=0;i<ps->element_count;i++) {
         const struct vkd3d_shader_signature_element *input=&ps->elements[i];
         if (input->sysval_semantic!=VKD3D_SHADER_SV_NONE) continue;
         unsigned supplied=0;
         for (unsigned j=0;j<vs->element_count;j++) {
            const struct vkd3d_shader_signature_element *output=&vs->elements[j];
            if (output->sysval_semantic==VKD3D_SHADER_SV_NONE &&
                output->register_index==input->register_index)
               supplied|=output->mask;
         }
         if (input->mask&~supplied) {
            status=fail(T70_SHADER_UNSUPPORTED,message,message_size,
                        "pixel register %u mask %#x has no vertex output",
                        input->register_index,input->mask&~supplied);
            goto cleanup;
         }
         for (unsigned c=0;c<4;c++)
            if (input->mask&(1u<<c))
               link->lanes[input->register_index][c]=(struct link_lane){1,input->register_index,c};
      }
      goto cleanup;
   }
   struct vkd3d_shader_varying_map map[256];
   unsigned map_count=0, index=0;
   vkd3d_shader_build_varying_map(vs,ps,&map_count,map);
   for (unsigned i=0;i<ps->element_count;i++) {
      const struct vkd3d_shader_signature_element *input=&ps->elements[i];
      if (input->sysval_semantic!=VKD3D_SHADER_SV_NONE) continue;
      if (index>=map_count || map[index].output_signature_index>=vs->element_count) {
         status=fail(T70_SHADER_UNSUPPORTED,message,message_size,
                     "pixel input %s%u has no vertex output",input->semantic_name,input->semantic_index);
         goto cleanup;
      }
      const struct vkd3d_shader_signature_element *output=
         &vs->elements[map[index++].output_signature_index];
      if (output->sysval_semantic!=VKD3D_SHADER_SV_NONE ||
          input->component_type!=output->component_type ||
          __builtin_popcount(input->mask)>__builtin_popcount(output->mask)) {
         status=fail(T70_SHADER_UNSUPPORTED,message,message_size,
                     "incompatible type or width for %s%u",input->semantic_name,input->semantic_index);
         goto cleanup;
      }
      /* Match logical components, not their packed register lanes. The
       * vkd3d varying map alone does not move xy to zw or vice versa. */
      unsigned source=output->mask;
      for (unsigned c=0;c<4;c++) {
         if (!(input->mask&(1u<<c))) continue;
         unsigned lane=__builtin_ctz(source); source&=source-1;
         struct link_lane *entry=&link->lanes[output->register_index][lane];
         if (entry->valid) {
            status=fail(T70_SHADER_INVALID,message,message_size,"overlapping semantic linkage");
            goto cleanup;
         }
         *entry=(struct link_lane){1,input->register_index,c};
      }
   }
cleanup:
   for (unsigned i=0;i<2;i++) vkd3d_shader_free_scan_signature_info(&signatures[i]);
   return status;
}
static bool generic_output(const nir_intrinsic_instr *intr, const void *data)
{
   (void)data;
   return nir_intrinsic_io_semantics(intr).location>=VARYING_SLOT_VAR0;
}
struct remap_context {
   const struct varying_link *link;
   bool invalid;
};
static bool remap_vs_output(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)b;
   if (intr->intrinsic!=nir_intrinsic_store_output) return false;
   struct remap_context *ctx=data;
   nir_io_semantics sem=nir_intrinsic_io_semantics(intr);
   if (sem.location>=VARYING_SLOT_VAR0) {
      nir_src *offset=nir_get_io_offset_src(intr);
      unsigned component=nir_intrinsic_component(intr);
      if (sem.location>=VARYING_SLOT_VAR0+32 || component>=4 || intr->num_components!=1 ||
          !nir_src_is_const(*offset) || nir_src_as_uint(*offset)) {
         ctx->invalid=true;
         return false;
      }
      const struct link_lane *lane=&ctx->link->lanes[sem.location-VARYING_SLOT_VAR0][component];
      if (!lane->valid) {
         nir_instr_remove(&intr->instr);
         return true;
      }
      sem.location=VARYING_SLOT_VAR0+lane->location;
      nir_intrinsic_set_io_semantics(intr,sem);
      nir_intrinsic_set_component(intr,lane->component);
   }
   /* Several original registers can now feed one packet slot, so its base
    * must be independent of the original variable's driver_location. */
   nir_intrinsic_set_base(intr,sem.location);
   return true;
}
static unsigned io_type_size(const struct glsl_type *type, bool bindless)
{
   (void)bindless;
   return glsl_count_attribute_slots(type,false);
}
/* Valhall table 0: 14 CB descriptors, indexed by the D3D cb register.
 * Native LD_BUFFER descriptor bounds give D3D out-of-range reads zero. */
static bool lower_cbv(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)data;
   nir_def *value=NULL;
   b->cursor=nir_before_instr(&intr->instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
      value=nir_vec2(b,nir_imm_int(b,pan_res_handle(0,nir_intrinsic_binding(intr))),
                     nir_imm_int(b,0));
      break;
   case nir_intrinsic_load_vulkan_descriptor:
      value=intr->src[0].ssa;
      break;
   default:
      return false;
   }
   nir_def_replace(&intr->def,value);
   return true;
}
/* Table 1 contains t0..t127 textures; table 2 contains s0..s15 and
 * nearest-clamp dummy sampler 16, needed by descriptor queries and Load. */
static bool lower_texture(nir_builder *b, nir_tex_instr *tex, void *data)
{
   bool *invalid=data;
   b->cursor=nir_before_instr(&tex->instr);
   const nir_tex_src_type deref_types[]={nir_tex_src_texture_deref,nir_tex_src_sampler_deref};
   const nir_tex_src_type handle_types[]={nir_tex_src_texture_handle,nir_tex_src_sampler_handle};
   for (unsigned t=0;t<2;t++) {
      int i=nir_tex_instr_src_index(tex,deref_types[t]);
      nir_def *handle;
      if (i<0) {
         if (!t) { *invalid=true; return false; }
         handle=nir_imm_int(b,pan_res_handle(2,16));
      } else {
         nir_deref_instr *deref=nir_src_as_deref(tex->src[i].src);
         nir_variable *var=nir_deref_instr_get_variable(deref);
         if (!var || var->data.descriptor_set!=t+1) { *invalid=true; return false; }
         for (nir_deref_instr *cur=deref;cur->deref_type!=nir_deref_type_var;
              cur=nir_deref_instr_parent(cur)) {
            if (cur->deref_type!=nir_deref_type_array || !nir_src_is_const(cur->arr.index) ||
                nir_src_as_uint(cur->arr.index)!=0) {
               *invalid=true; return false;
            }
         }
         handle=nir_imm_int(b,pan_res_handle(t+1,var->data.binding));
         nir_tex_instr_remove_src(tex,i);
      }
      nir_tex_instr_add_src(tex,handle_types[t],handle);
   }
   tex->texture_index=0; tex->sampler_index=0;
   return true;
}
/* Driver FAU ABI: viewport scale xyz at 0, offset xyz at 4, first_vertex
 * at 8, base_instance at 9 and noperspective mask at 10 (32-bit words).
 * The caller must populate these at draw time; no viewport is baked in. */
static bool lower_sysval(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)data;
   unsigned word;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_viewport_scale: word=0; break;
   case nir_intrinsic_load_viewport_offset: word=4; break;
   case nir_intrinsic_load_first_vertex: word=8; break;
   case nir_intrinsic_load_base_instance: word=9; break;
   case nir_intrinsic_load_noperspective_varyings_pan: word=10; break;
   default: return false;
   }
   b->cursor=nir_before_instr(&intr->instr);
   nir_def *value=nir_load_push_constant(b,intr->def.num_components,intr->def.bit_size,
                                         nir_imm_int(b,word*4));
   nir_def_replace(&intr->def,value);
   return true;
}
static void spirv_message(void *data, enum nir_spirv_debug_level level,
                          size_t offset, const char *text)
{
   (void)level; (void)offset; (void)data; (void)text;
}
uint32_t t70_shader_abi_version(void) { return T70_SHADER_ABI; }
void t70_shader_free(struct t70_shader *s)
{
   if (!s) return;
   free(s->binary);
   free(s->spirv);
   memset(s,0,sizeof(*s));
}
static int compile_shader(const struct t70_shader_request *r, struct t70_shader *out,
                          const struct varying_link *link,
                          const struct pan_varying_layout *shared_layout,
                          struct pan_varying_layout *compiled_layout, uint32_t *noperspective,
                          char *message, size_t message_size)
{
   if (message && message_size) message[0]=0;
   if (!r || !out || r->abi!=T70_SHADER_ABI || r->reserved ||
       r->stage>T70_SHADER_VERTEX || out->binary || out->spirv)
      return fail(T70_SHADER_INVALID,message,message_size,"invalid shader request or nonempty output");
   if (r->gpu_id!=T70_SHADER_GPU_ID)
      return fail(T70_SHADER_UNSUPPORTED,message,message_size,"GPU is not the qualified G57 arch-9 model");
   int status=validate_dxbc(r);
   if (status) return fail(status,message,message_size,"invalid DXBC container or unsupported shader stage/model");
   struct vkd3d_shader_compile_info ci={
      .type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO,
      .source={r->dxbc,r->dxbc_size},
      .source_type=VKD3D_SHADER_SOURCE_DXBC_TPF,
      .target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY,
      .log_level=VKD3D_SHADER_LOG_ERROR,
   };
   struct vkd3d_shader_scan_descriptor_info scan={
      .type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO,
   };
   ci.next=&scan;
   char *messages=NULL;
   status=vkd3d_shader_scan(&ci,&messages);
   if (status<0) {
      fail(T70_SHADER_TRANSLATION,message,message_size,"%s",messages?messages:"DXBC scan failed");
      vkd3d_shader_free_messages(messages);
      vkd3d_shader_free_scan_descriptor_info(&scan);
      return T70_SHADER_TRANSLATION;
   }
   vkd3d_shader_free_messages(messages); messages=NULL;
   struct vkd3d_shader_resource_binding bindings[158]={0};
   uint32_t cb_mask=0, sampler_mask=0, texture_mask[4]={0};
   for (unsigned i=0;i<scan.descriptor_count;i++) {
      struct vkd3d_shader_descriptor_info *desc=&scan.descriptors[i];
      unsigned table=0,limit=0,flags=0;
      uint32_t *mask=NULL;
      switch (desc->type) {
      case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:
         table=0; limit=14; flags=VKD3D_SHADER_BINDING_FLAG_BUFFER; mask=&cb_mask;
         break;
      case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
         table=1; limit=128;
         flags=desc->resource_type==VKD3D_SHADER_RESOURCE_BUFFER?
            VKD3D_SHADER_BINDING_FLAG_BUFFER:VKD3D_SHADER_BINDING_FLAG_IMAGE;
         if (desc->register_index<128) mask=&texture_mask[desc->register_index/32];
         break;
      case VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER:
         table=2; limit=16; mask=&sampler_mask;
         break;
      default: break;
      }
      if (i>=158 || !mask || desc->register_space || desc->register_index>=limit ||
          desc->count!=1 || (*mask&(1u<<(desc->register_index%32)))) {
         vkd3d_shader_free_scan_descriptor_info(&scan);
         return fail(T70_SHADER_UNSUPPORTED,message,message_size,"unsupported descriptor type, array, space or register");
      }
      *mask|=1u<<(desc->register_index%32);
      bindings[i]=(struct vkd3d_shader_resource_binding){
         .type=desc->type,.register_index=desc->register_index,
         .shader_visibility=VKD3D_SHADER_VISIBILITY_ALL,
         .flags=flags,.binding={table,desc->register_index,1},
      };
   }
   struct vkd3d_shader_interface_info interface={
      .type=VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO,
      .bindings=bindings,.binding_count=scan.descriptor_count,
   };
   vkd3d_shader_free_scan_descriptor_info(&scan);
   ci.next=&interface;
   struct vkd3d_shader_code spirv={0};
   status=vkd3d_shader_compile(&ci,&spirv,&messages);
   if (status<0) {
      fail(T70_SHADER_TRANSLATION,message,message_size,"%s",messages?messages:"DXBC to SPIR-V failed");
      vkd3d_shader_free_messages(messages);
      vkd3d_shader_free_shader_code(&spirv);
      return T70_SHADER_TRANSLATION;
   }
   vkd3d_shader_free_messages(messages);
   glsl_type_singleton_init_or_ref();
   mesa_shader_stage stage=r->stage==T70_SHADER_PIXEL?MESA_SHADER_FRAGMENT:MESA_SHADER_VERTEX;
   struct spirv_to_nir_options opts={
      .environment=NIR_SPIRV_VULKAN,
      .ubo_addr_format=nir_address_format_32bit_index_offset,
      .ssbo_addr_format=nir_address_format_32bit_index_offset,
      .shared_addr_format=nir_address_format_32bit_offset,
      .min_ubo_alignment=16, .min_ssbo_alignment=16,
      .skip_os_break_in_debug_build=true,
      .debug={.func=spirv_message},
   };
   nir_shader *nir=spirv_to_nir(spirv.code,spirv.size/4,NULL,stage,"main",&opts,
                               pan_get_nir_shader_compiler_options(9,stage,false));
   if (!nir) {
      status=fail(T70_SHADER_TRANSLATION,message,message_size,"SPIR-V to NIR failed");
      goto cleanup;
   }
   NIR_PASS(_,nir,nir_lower_variable_initializers,nir_var_function_temp);
   NIR_PASS(_,nir,nir_lower_returns);
   NIR_PASS(_,nir,nir_inline_functions);
   NIR_PASS(_,nir,nir_opt_copy_prop);
   NIR_PASS(_,nir,nir_opt_constant_folding);
   NIR_PASS(_,nir,nir_opt_deref);
   nir_remove_non_cmat_call_entrypoints(nir);
   NIR_PASS(_,nir,nir_lower_variable_initializers,~0);
   NIR_PASS(_,nir,nir_split_var_copies);
   NIR_PASS(_,nir,nir_split_per_member_structs);
   NIR_PASS(_,nir,nir_lower_io_vars_to_temporaries,nir_shader_get_entrypoint(nir),nir_var_shader_out);
   NIR_PASS(_,nir,nir_lower_global_vars_to_local);
   NIR_PASS(_,nir,nir_split_var_copies);
   NIR_PASS(_,nir,nir_lower_system_values);
   if (stage==MESA_SHADER_FRAGMENT)
      NIR_PASS(_,nir,nir_lower_wpos_center);
   pan_preprocess_nir(nir,9);
   bool invalid_texture=false;
   NIR_PASS(_,nir,nir_shader_tex_pass,lower_texture,nir_metadata_control_flow,&invalid_texture);
   if (invalid_texture) {
      status=fail(T70_SHADER_UNSUPPORTED,message,message_size,"unsupported texture descriptor dereference");
      goto cleanup;
   }
   NIR_PASS(_,nir,nir_shader_intrinsics_pass,lower_cbv,nir_metadata_control_flow,NULL);
   NIR_PASS(_,nir,nir_lower_explicit_io,nir_var_mem_ubo,nir_address_format_32bit_index_offset);
   if (stage==MESA_SHADER_VERTEX) {
      nir_foreach_shader_in_variable(var,nir) {
         unsigned index=var->data.location-VERT_ATTRIB_GENERIC0;
         if (index>=32) {
            status=fail(T70_SHADER_UNSUPPORTED,message,message_size,"vertex input register exceeds D3D limit");
            goto cleanup;
         }
         var->data.driver_location=pan_res_handle(3,index);
      }
   } else {
      nir_assign_io_var_locations(nir,nir_var_shader_in);
   }
   nir_assign_io_var_locations(nir,nir_var_shader_out);
   NIR_PASS(_,nir,nir_lower_var_copies);
   NIR_PASS(_,nir,nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_shader_in|nir_var_shader_out,UINT32_MAX);
   NIR_PASS(_,nir,nir_lower_io,nir_var_shader_in|nir_var_shader_out,
            io_type_size,nir_lower_io_use_interpolated_input_intrinsics);
   NIR_PASS(_,nir,nir_opt_constant_folding);
   if (link) {
      NIR_PASS(_,nir,nir_lower_io_to_scalar,nir_var_shader_out,generic_output,NULL);
      struct remap_context ctx={.link=link};
      NIR_PASS(_,nir,nir_shader_intrinsics_pass,remap_vs_output,nir_metadata_control_flow,&ctx);
      if (ctx.invalid) {
         status=fail(T70_SHADER_UNSUPPORTED,message,message_size,"unsupported vertex output addressing");
         goto cleanup;
      }
      NIR_PASS(_,nir,nir_opt_constant_folding);
      NIR_PASS(_,nir,nir_opt_dce);
#ifdef T70_SHADER_TESTS
      /* Observe real lowered values only in the host test build. */
      extern void t70_shader_test_linked_constant(unsigned, unsigned, uint32_t);
      nir_foreach_function_impl(impl,nir) {
         nir_foreach_block(block,impl) {
            nir_foreach_instr(instr,block) {
               if (instr->type!=nir_instr_type_intrinsic) continue;
               nir_intrinsic_instr *intr=nir_instr_as_intrinsic(instr);
               if (intr->intrinsic!=nir_intrinsic_store_output) continue;
               nir_io_semantics sem=nir_intrinsic_io_semantics(intr);
               if (sem.location>=VARYING_SLOT_VAR0 && intr->num_components==1 &&
                   nir_src_bit_size(intr->src[0])==32 && nir_src_is_const(intr->src[0]))
                  t70_shader_test_linked_constant(sem.location-VARYING_SLOT_VAR0,
                       nir_intrinsic_component(intr),nir_src_as_uint(intr->src[0]));
            }
         }
      }
#endif
   }
#ifdef T70_SHADER_TESTS
   if (stage==MESA_SHADER_FRAGMENT) {
      extern void t70_shader_test_linked_input(unsigned, unsigned, unsigned);
      nir_foreach_function_impl(impl,nir) {
         nir_foreach_block(block,impl) {
            nir_foreach_instr(instr,block) {
               if (instr->type!=nir_instr_type_intrinsic) continue;
               nir_intrinsic_instr *intr=nir_instr_as_intrinsic(instr);
               if (intr->intrinsic!=nir_intrinsic_load_input &&
                   intr->intrinsic!=nir_intrinsic_load_interpolated_input) continue;
               nir_src *offset=nir_get_io_offset_src(intr);
               if (nir_src_is_const(*offset))
                  t70_shader_test_linked_input(nir_intrinsic_io_semantics(intr).location+nir_src_as_uint(*offset),
                       nir_intrinsic_component(intr),intr->num_components);
            }
         }
      }
   }
#endif
   nir_shader_gather_info(nir,nir_shader_get_entrypoint(nir));
   if (nir->info.bit_sizes_float&64) {
      status=fail(T70_SHADER_UNSUPPORTED,message,message_size,"FP64 shader arithmetic requires software lowering");
      goto cleanup;
   }
   uint32_t attribute_mask=stage==MESA_SHADER_VERTEX?
      (uint32_t)(nir->info.inputs_read>>VERT_ATTRIB_GENERIC0):0;
   struct pan_varying_layout own_layout={0};
   if (!shared_layout) {
      pan_varying_collect_formats(&own_layout,nir,9);
      pan_build_varying_layout_compact(&own_layout,nir,9);
   }
   const struct pan_varying_layout *layout=shared_layout?shared_layout:&own_layout;
   struct pan_compile_inputs input={
      .arch=9, .model=pan_get_model(r->gpu_id,0,2,0),
      .varying_layout=layout,
      .fau={.reserved=stage==MESA_SHADER_VERTEX?12:0,.promote_immediates=true},
   };
   struct pan_shader_info info={0};
   pan_postprocess_nir(nir,&input,&info);
   NIR_PASS(_,nir,nir_shader_intrinsics_pass,lower_sysval,nir_metadata_control_flow,NULL);
   nir_validate_shader(nir,"t70 DXBC lowering");
   struct util_dynarray binary=UTIL_DYNARRAY_INIT;
   pan_shader_compile(nir,&input,&binary,&info);
   if (!binary.size || (binary.size&7)) {
      status=fail(T70_SHADER_TRANSLATION,message,message_size,"empty or unaligned Mali code");
      util_dynarray_fini(&binary);
      goto cleanup;
   }
   out->binary=malloc(binary.size);
   out->spirv=malloc(spirv.size);
   if (!out->binary || !out->spirv) {
      t70_shader_free(out);
      status=fail(T70_SHADER_MEMORY,message,message_size,"shader allocation failed");
      util_dynarray_fini(&binary);
      goto cleanup;
   }
   memcpy(out->binary,binary.data,binary.size);
   memcpy(out->spirv,spirv.code,spirv.size);
   out->binary_size=binary.size; out->spirv_size=spirv.size;
   out->stage=r->stage; out->arch=9; out->work_registers=info.work_reg_count;
   out->tls_size=info.tls_size; out->wls_size=info.wls_size;
   out->preload=info.preload; out->outputs_written=info.outputs_written;
   out->fau_count=info.fau.count; out->fau_reserved=info.fau.reserved;
   memcpy(out->fau_words,info.fau.words,sizeof(out->fau_words));
   memcpy(out->fau_const_mask,info.fau.is_const,sizeof(out->fau_const_mask));
   out->cb_mask=cb_mask; out->sampler_mask=sampler_mask;
   memcpy(out->texture_mask,texture_mask,sizeof(texture_mask));
   out->attributes_read=attribute_mask;
   out->attribute_count=attribute_mask?32-__builtin_clz(attribute_mask):0;
   out->varying_stride=layout->generic_size_B;
   out->flags=(info.ftz_fp32?T70_SHADER_FLAG_FTZ32:0)|
      (info.ftz_fp16?T70_SHADER_FLAG_FTZ16:0)|
      (info.contains_barrier?T70_SHADER_FLAG_BARRIER:0)|
      (info.bifrost.uses_flat_shading?T70_SHADER_FLAG_FLAT:0)|
      (info.bifrost.uses_ld_var?T70_SHADER_FLAG_VARYING_DESCRIPTORS:0);
   if (stage==MESA_SHADER_VERTEX) {
      out->flags|=(info.vs.idvs?T70_SHADER_FLAG_IDVS:0)|
         (info.vs.secondary_enable?T70_SHADER_FLAG_SECONDARY:0)|
         (info.vs.needs_extended_fifo?T70_SHADER_FLAG_EXTENDED_FIFO:0);
      out->secondary_offset=info.vs.secondary_enable?info.vs.secondary_offset:0;
      out->secondary_registers=info.vs.secondary_enable?info.vs.secondary_work_reg_count:0;
      out->secondary_preload=info.vs.secondary_enable?info.vs.secondary_preload:0;
      out->no_point_size_offset=info.vs.no_psiz_offset;
   } else {
      out->flags|=(info.fs.can_discard?T70_SHADER_FLAG_DISCARD:0)|
         (info.fs.writes_depth?T70_SHADER_FLAG_DEPTH:0)|
         (info.fs.writes_coverage?T70_SHADER_FLAG_COVERAGE:0);
   }
   out->varying_count=info.varyings.formats.count;
   assert(out->varying_count<=T70_SHADER_VARYINGS);
   for (unsigned i=0;i<out->varying_count;i++) {
      const struct pan_varying_slot *slot=&info.varyings.formats.slots[i];
      const struct pan_varying_slot *memory=pan_varying_layout_find_slot(layout,slot->location);
      if (!slot->alu_type || slot->section!=PAN_VARYING_SECTION_GENERIC) memory=NULL;
      out->varyings[i]=(struct t70_shader_varying){
         .location=slot->location,.components=slot->ncomps,
         .kind=nir_alu_type_get_base_type(slot->alu_type),
         .bits=nir_alu_type_get_type_size(memory?memory->alu_type:slot->alu_type),
         .section=slot->section,.offset=memory?memory->offset:slot->offset,
      };
   }
   for (unsigned i=0;i<8;i++) {
      out->blend_type[i]=info.bifrost.blend[i].type;
      out->blend_return_offset[i]=info.bifrost.blend[i].return_offset;
   }
   if (compiled_layout) *compiled_layout=*layout;
   if (noperspective) *noperspective=info.varyings.noperspective;
   util_dynarray_fini(&binary);
   status=T70_SHADER_OK;
cleanup:
   ralloc_free(nir);
   glsl_type_singleton_decref();
   vkd3d_shader_free_shader_code(&spirv);
   return status;
}

struct framebuffer_preload_lowering {
   uint32_t array_layers;
   unsigned texture_count, render_area_count, layer_count;
   bool invalid;
};
static bool lower_framebuffer_preload_instr(nir_builder *b, nir_instr *instr,
                                            void *data)
{
   struct framebuffer_preload_lowering *ctx=data;
   b->cursor=nir_before_instr(instr);
   if (instr->type==nir_instr_type_tex) {
      nir_tex_instr *tex=nir_instr_as_tex(instr);
      /* Mesa labels framebuffer reads by fragment-result slot, not sampler binding. */
      if (tex->texture_index!=FRAG_RESULT_DATA0 || tex->sampler_index ||
          tex->is_array!=(ctx->array_layers>1) ||
          nir_tex_instr_src_index(tex,nir_tex_src_texture_handle)>=0 ||
          nir_tex_instr_src_index(tex,nir_tex_src_sampler_handle)>=0) {
         ctx->invalid=true;
         return false;
      }
      tex->texture_index=0;
      tex->sampler_index=0;
      nir_tex_instr_add_src(tex,nir_tex_src_sampler_handle,
                            pan_nir_res_handle(b,T70_SHADER_FB_PRELOAD_SAMPLER_TABLE,
                                               T70_SHADER_FB_PRELOAD_SAMPLER_INDEX,NULL));
      nir_tex_instr_add_src(tex,nir_tex_src_texture_handle,
                            pan_nir_res_handle(b,T70_SHADER_FB_PRELOAD_TEXTURE_TABLE,
                                               T70_SHADER_FB_PRELOAD_TEXTURE_INDEX,NULL));
      ctx->texture_count++;
      return true;
   }
   if (instr->type!=nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr=nir_instr_as_intrinsic(instr);
   nir_def *value;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_fb_render_area_pan:
      if (intr->def.num_components!=4 || intr->def.bit_size!=16) {
         ctx->invalid=true;
         return false;
      }
      value=nir_load_push_constant(b,4,16,nir_imm_int(b,0));
      ctx->render_area_count++;
      break;
   case nir_intrinsic_load_layer_id:
      value=nir_extract_u8_imm(b,nir_u2u32(b,nir_load_frame_arg_pan(b)),0);
      ctx->layer_count++;
      break;
   default:
      return false;
   }
   nir_def_replace(&intr->def,value);
   return true;
}

int t70_shader_compile_framebuffer_preload(
   const struct t70_shader_fb_preload_request *request,
   struct t70_shader *out, char *message, size_t message_size)
{
   if (message && message_size) message[0]=0;
   if (!request || !out || request->abi!=T70_SHADER_ABI || request->reserved ||
       request->array_layers==0 || out->binary || out->spirv)
      return fail(T70_SHADER_INVALID,message,message_size,
                  "invalid framebuffer preload request or nonempty output");
   if (request->gpu_id!=T70_SHADER_GPU_ID || request->sample_count!=1 ||
       request->array_layers>T70_SHADER_FB_PRELOAD_MAX_LAYERS ||
       (request->format!=T70_SHADER_FB_RGBA8_UNORM &&
        request->format!=T70_SHADER_FB_BGRA8_UNORM &&
        request->format!=T70_SHADER_FB_BGRX8_UNORM))
      return fail(T70_SHADER_UNSUPPORTED,message,message_size,
                  "unsupported framebuffer preload target");

   struct t70_shader generated={0};
   int status=T70_SHADER_OK;
   glsl_type_singleton_init_or_ref();
   nir_shader *nir=t70_shader_build_framebuffer_preload_nir(
      request->format,request->array_layers);
   if (!nir) {
      status=fail(T70_SHADER_TRANSLATION,message,message_size,
                  "Mesa framebuffer preload generator failed");
      goto cleanup;
   }

   struct framebuffer_preload_lowering lowering={.array_layers=request->array_layers};
   nir_shader_instructions_pass(nir,lower_framebuffer_preload_instr,
                                nir_metadata_control_flow,&lowering);
   if (lowering.invalid || lowering.texture_count!=1 ||
       lowering.render_area_count!=1 || lowering.layer_count!=1) {
      status=fail(T70_SHADER_UNSUPPORTED,message,message_size,
                  "unexpected Mesa framebuffer preload NIR shape (invalid=%u, tex=%u, area=%u, layer=%u)",
                  (unsigned)lowering.invalid,lowering.texture_count,
                  lowering.render_area_count,lowering.layer_count);
      goto cleanup;
   }
   nir_shader_gather_info(nir,nir_shader_get_entrypoint(nir));
   pan_preprocess_nir(nir,9);
   struct pan_compile_inputs input={
      .arch=9,
      .model=pan_get_model(request->gpu_id,0,2,0),
      .is_blit=true,
      .fau={.reserved=T70_SHADER_FB_PRELOAD_FAU_WORDS},
   };
   struct pan_shader_info info={0};
   pan_postprocess_nir(nir,&input,&info);
   nir_validate_shader(nir,"t70 framebuffer preload lowering");
   struct util_dynarray binary=UTIL_DYNARRAY_INIT;
   pan_shader_compile(nir,&input,&binary,&info);
   /* The machine-code backend publishes FAU metadata, not NIR postprocessing. */
   if (info.fau.count>sizeof(generated.fau_words)/sizeof(generated.fau_words[0]) ||
       info.fau.reserved!=T70_SHADER_FB_PRELOAD_FAU_WORDS ||
       info.varyings.formats.count) {
      status=fail(T70_SHADER_UNSUPPORTED,message,message_size,
                  "unexpected framebuffer preload compiler metadata (fau=%u/%u, varyings=%u)",
                  info.fau.count,info.fau.reserved,info.varyings.formats.count);
      util_dynarray_fini(&binary);
      goto cleanup;
   }
   if (!binary.size || (binary.size&7)) {
      status=fail(T70_SHADER_TRANSLATION,message,message_size,
                  "empty or unaligned framebuffer preload code");
      util_dynarray_fini(&binary);
      goto cleanup;
   }
   generated.binary=malloc(binary.size);
   if (!generated.binary) {
      status=fail(T70_SHADER_MEMORY,message,message_size,
                  "framebuffer preload allocation failed");
      util_dynarray_fini(&binary);
      goto cleanup;
   }
   memcpy(generated.binary,binary.data,binary.size);
   generated.binary_size=binary.size;
   generated.stage=T70_SHADER_PIXEL;
   generated.arch=9;
   generated.work_registers=info.work_reg_count;
   generated.tls_size=info.tls_size;
   generated.wls_size=info.wls_size;
   generated.preload=info.preload;
   generated.outputs_written=info.outputs_written;
   generated.fau_count=info.fau.count;
   generated.fau_reserved=info.fau.reserved;
   memcpy(generated.fau_words,info.fau.words,sizeof(generated.fau_words));
   memcpy(generated.fau_const_mask,info.fau.is_const,sizeof(generated.fau_const_mask));
   generated.flags=(info.ftz_fp32?T70_SHADER_FLAG_FTZ32:0)|
      (info.ftz_fp16?T70_SHADER_FLAG_FTZ16:0)|
      (info.contains_barrier?T70_SHADER_FLAG_BARRIER:0)|
      (info.bifrost.uses_flat_shading?T70_SHADER_FLAG_FLAT:0)|
      (info.bifrost.uses_ld_var?T70_SHADER_FLAG_VARYING_DESCRIPTORS:0)|
      (info.fs.can_discard?T70_SHADER_FLAG_DISCARD:0)|
      (info.fs.writes_depth?T70_SHADER_FLAG_DEPTH:0)|
      (info.fs.writes_coverage?T70_SHADER_FLAG_COVERAGE:0);
   util_dynarray_fini(&binary);
   *out=generated;

cleanup:
   ralloc_free(nir);
   glsl_type_singleton_decref();
   if (status)
      t70_shader_free(&generated);
   return status;
}

int t70_shader_compile(const struct t70_shader_request *r, struct t70_shader *out,
                       char *message, size_t message_size)
{
   return compile_shader(r,out,NULL,NULL,NULL,NULL,message,message_size);
}
void t70_shader_free_pipeline(struct t70_shader_pipeline *pipeline)
{
   if (!pipeline) return;
   t70_shader_free(&pipeline->vertex);
   t70_shader_free(&pipeline->pixel);
   memset(pipeline,0,sizeof(*pipeline));
}
static int compile_pipeline(const struct t70_shader_request *vertex,
                               const struct t70_shader_request *pixel,
                               struct t70_shader_pipeline *out, bool registers,
                               char *message, size_t message_size)
{
   if (message && message_size) message[0]=0;
   if (!vertex || !pixel || !out || out->vertex.binary || out->vertex.spirv ||
       out->pixel.binary || out->pixel.spirv || vertex->abi!=T70_SHADER_ABI ||
       pixel->abi!=T70_SHADER_ABI || vertex->reserved || pixel->reserved ||
       vertex->stage!=T70_SHADER_VERTEX || pixel->stage!=T70_SHADER_PIXEL)
      return fail(T70_SHADER_INVALID,message,message_size,"invalid pipeline request or nonempty output");
   if (vertex->gpu_id!=T70_SHADER_GPU_ID || pixel->gpu_id!=T70_SHADER_GPU_ID)
      return fail(T70_SHADER_UNSUPPORTED,message,message_size,"unsupported pipeline GPU");
   int status=validate_dxbc(vertex);
   if (!status) status=validate_dxbc(pixel);
   if (status) return fail(status,message,message_size,"invalid pipeline DXBC or shader model");
   struct varying_link link={0};
   status=build_link(vertex,pixel,&link,registers,message,message_size);
   if (status) return status;
   struct t70_shader_pipeline compiled={0};
   struct pan_varying_layout layout;
   status=compile_shader(vertex,&compiled.vertex,&link,NULL,&layout,NULL,message,message_size);
   if (!status)
      status=compile_shader(pixel,&compiled.pixel,NULL,&layout,NULL,
                            &compiled.noperspective_mask,message,message_size);
   if (status) {
      t70_shader_free_pipeline(&compiled);
      return status;
   }
   compiled.varying_stride=layout.generic_size_B;
   *out=compiled;
   return T70_SHADER_OK;
}

int t70_shader_compile_pipeline(const struct t70_shader_request *vertex,
                               const struct t70_shader_request *pixel,
                               struct t70_shader_pipeline *out,
                               char *message, size_t message_size)
{
   return compile_pipeline(vertex,pixel,out,false,message,message_size);
}

/* Only declaration operands are decoded here. vkd3d validates the complete
 * instruction program after the missing container/signatures are restored. */
struct token_element { uint32_t reg, mask, system, operand, used_mask; };
struct token_signature {
   unsigned count;
   struct token_element elements[T70_SHADER_MAX_SIGNATURE];
};
static void put32(void *ptr, uint32_t value)
{
   uint8_t *p=ptr;
   for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(value>>(8*i));
}
static int full_signature(const struct t70_shader_signature_entry *entries, unsigned count)
{
   if (count>T70_SHADER_MAX_SIGNATURE || (count && !entries)) return T70_SHADER_INVALID;
   uint32_t occupied[32]={0};
   for (unsigned i=0;i<count;i++) {
      const struct t70_shader_signature_entry *e=&entries[i];
      if (e->system_value>10 || !e->mask || (e->mask&~15u) || e->register_index>=32 ||
          (occupied[e->register_index]&e->mask)) return T70_SHADER_INVALID;
      occupied[e->register_index]|=e->mask;
   }
   return T70_SHADER_OK;
}
static int add_declaration(struct token_signature *sig, const uint8_t *p, size_t n,
                           unsigned stage, unsigned opcode, uint32_t instruction,
                           const struct t70_shader_signature_entry *full, unsigned full_count)
{
   bool output=opcode>=0x65;
   bool system=opcode==0x60 || opcode==0x61 || opcode==0x63 || opcode==0x64 ||
               opcode==0x66 || opcode==0x67;
   if (n<4 || (instruction&0x80000000u)) return T70_SHADER_INVALID;
   uint32_t operand=le32(p), extension=operand;
   size_t offset=4;
   while (extension&0x80000000u) {
      if (n-offset<4) return T70_SHADER_INVALID;
      extension=le32(p+offset); offset+=4;
   }
   unsigned type=(operand>>12)&255, dimensions=(operand>>20)&3;
   unsigned components=operand&3;
   unsigned mask=components==1?1:(operand>>4)&15;
   if ((components!=1 && components!=2) || !mask ||
       (components==2 && ((operand>>2)&3))) return T70_SHADER_INVALID;
   struct token_element e={.reg=UINT32_MAX,.mask=mask,.operand=type,.used_mask=mask};
   if (type==(output?2u:1u)) {
      if (dimensions!=1 || ((operand>>22)&7) || n-offset<4)
         return T70_SHADER_INVALID;
      e.reg=le32(p+offset);offset+=4;
      if (e.reg>=32) return T70_SHADER_UNSUPPORTED;
   } else {
      bool special=stage==T70_SHADER_PIXEL && !system && !dimensions && components==1 &&
         (output?(type==12 || type==15 || type==38 || type==39):(type==11 || type==35));
      if (!special) return T70_SHADER_UNSUPPORTED;
   }
   if (system) {
      if (n-offset<4) return T70_SHADER_INVALID;
      e.system=le32(p+offset);offset+=4;
      if (!e.system || e.system>10) return T70_SHADER_UNSUPPORTED;
   }
   if (offset!=n || sig->count>=T70_SHADER_MAX_SIGNATURE) return T70_SHADER_INVALID;
   if (full_count && e.reg!=UINT32_MAX) {
      unsigned covered=0;
      for (unsigned i=0;i<full_count;i++)
         if (full[i].register_index==e.reg && full[i].system_value==e.system)
            covered|=full[i].mask;
      if (e.mask&~covered) return T70_SHADER_INVALID;
   }
   for (unsigned i=0;i<sig->count;i++) {
      const struct token_element *old=&sig->elements[i];
      if (old->reg==e.reg && (e.reg!=UINT32_MAX || old->operand==e.operand) &&
          (old->mask&e.mask)) return T70_SHADER_INVALID;
   }
   sig->elements[sig->count++]=e;
   return T70_SHADER_OK;
}
static int link_clip_cull(struct token_signature *sig, const struct token_signature *upstream)
{
   unsigned used[2][32]={{0}}, systems=0;
   for (unsigned i=0;i<sig->count;i++) {
      const struct token_element *e=&sig->elements[i];
      if (e->system!=2 && e->system!=3) continue;
      unsigned provided=0;
      for (unsigned j=0;j<upstream->count;j++) {
         const struct token_element *v=&upstream->elements[j];
         if (v->system==e->system && v->reg==e->reg) provided|=v->mask;
      }
      if (e->mask&~provided) return T70_SHADER_UNSUPPORTED;
      used[e->system-2][e->reg]|=e->mask;systems|=1u<<e->system;
   }
   unsigned count=0;
   for (unsigned i=0;i<sig->count;i++)
      if (!(systems&(1u<<sig->elements[i].system))) sig->elements[count++]=sig->elements[i];
   sig->count=count;
   /* vkd3d packs clip/cull arrays from the full signature masks, including
    * unused lanes. Preserve the VS schema and mark only the PS lanes used. */
   for (unsigned i=0;i<upstream->count;i++) {
      struct token_element e=upstream->elements[i];
      if (!(systems&(1u<<e.system))) continue;
      for (unsigned j=0;j<sig->count;j++)
         if (sig->elements[j].reg==e.reg && (sig->elements[j].mask&e.mask))
            return T70_SHADER_UNSUPPORTED;
      if (sig->count==T70_SHADER_MAX_SIGNATURE) return T70_SHADER_INVALID;
      e.operand=1;e.used_mask=e.mask&used[e.system-2][e.reg];
      sig->elements[sig->count++]=e;
   }
   return T70_SHADER_OK;
}
static int signature_chunk(const struct token_signature *sig, unsigned stage, bool output,
                            const struct t70_shader_io_types *types, const struct token_signature *upstream,
                            struct vkd3d_shader_code *chunk)
{
   static const char *const names[]={"T70_REG","SV_Position","SV_ClipDistance","SV_CullDistance",
      "SV_RenderTargetArrayIndex","SV_ViewportArrayIndex","SV_VertexID","SV_PrimitiveID",
      "SV_InstanceID","SV_IsFrontFace","SV_SampleIndex"};
   /* Every emitted semantic name fits in 32 bytes. */
   size_t size=8+sig->count*(24+32);
   uint8_t *data=calloc(1,size);
   if (!data) return T70_SHADER_MEMORY;
   put32(data,sig->count);put32(data+4,8);
   size_t next=8+24*sig->count;
   for (unsigned i=0;i<sig->count;i++) {
      const struct token_element *e=&sig->elements[i];
      const char *name=names[e->system];
      unsigned index=e->reg*16+e->mask;
      unsigned sysvalue=e->system;
      unsigned component=e->system>=4?VKD3D_SHADER_COMPONENT_UINT:VKD3D_SHADER_COMPONENT_FLOAT;
      if (!e->system && e->reg<32 && stage==T70_SHADER_VERTEX && !output) {
         if (types->vertex_uint&(1u<<e->reg)) component=VKD3D_SHADER_COMPONENT_UINT;
         if (types->vertex_sint&(1u<<e->reg)) component=VKD3D_SHADER_COMPONENT_INT;
      }
      if (e->system) {
         index=0;
         if (e->system==2 || e->system==3) {
            /* Clip/cull arrays have no semantic index in DDI tokens. Base PS
             * indices on the complete VS output, not the PS's used subset. */
            const struct token_signature *space=upstream?upstream:sig;
            unsigned provided=0,earlier=0;
            for (unsigned j=0;j<space->count;j++) {
               const struct token_element *v=&space->elements[j];
               if (v->system!=e->system) continue;
               if (v->reg==e->reg) provided|=v->mask;
               if (v->reg<e->reg) earlier|=1u<<v->reg;
            }
            if (e->mask&~provided) {free(data);return T70_SHADER_UNSUPPORTED;}
            index=__builtin_popcount(earlier);
         }
      } else if (output && stage==T70_SHADER_PIXEL) {
         index=e->reg;
         switch (e->operand) {
         case 2:
            if (e->reg>=8) {free(data);return T70_SHADER_UNSUPPORTED;}
            name="SV_Target";
            if (types->target_uint&(1u<<e->reg)) component=VKD3D_SHADER_COMPONENT_UINT;
            if (types->target_sint&(1u<<e->reg)) component=VKD3D_SHADER_COMPONENT_INT;
            break;
         case 12:name="SV_Depth";index=0;break;
         case 15:name="SV_Coverage";index=0;component=VKD3D_SHADER_COMPONENT_UINT;break;
         case 38:name="SV_DepthGreaterEqual";index=0;break;
         case 39:name="SV_DepthLessEqual";index=0;break;
         default:free(data);return T70_SHADER_UNSUPPORTED;
         }
      } else if (e->operand==11) {
         name="SV_PrimitiveID";index=0;sysvalue=VKD3D_SHADER_SV_PRIMITIVE_ID;
         component=VKD3D_SHADER_COMPONENT_UINT;
      } else if (e->operand==35) {
         name="SV_Coverage";index=0;sysvalue=VKD3D_SHADER_SV_COVERAGE;
         component=VKD3D_SHADER_COMPONENT_UINT;
      }
      uint8_t *entry=data+8+24*i;
      put32(entry,(uint32_t)next);put32(entry+4,index);put32(entry+8,sysvalue);
      put32(entry+12,component);put32(entry+16,e->reg);
      put32(entry+20,e->mask|(output?0:e->used_mask<<8));
      size_t length=strlen(name)+1;
      memcpy(data+next,name,length);next+=length;
   }
   chunk->code=data;chunk->size=(next+3)&~(size_t)3;
   return T70_SHADER_OK;
}
static int tokens_dxbc(const struct t70_shader_token_request *r, struct vkd3d_shader_code *code,
                        const struct token_signature *upstream, struct token_signature *outputs,
                        char *message, size_t message_size)
{
   if (!r || r->abi!=T70_SHADER_ABI || r->reserved || r->stage>T70_SHADER_VERTEX ||
       !r->tokens || r->tokens_size<8 || r->tokens_size>T70_SHADER_MAX_INPUT ||
       (r->tokens_size&3) || (r->types.vertex_uint&r->types.vertex_sint) ||
       (r->types.target_uint&r->types.target_sint) || ((r->types.target_uint|r->types.target_sint)&~255u) ||
       full_signature(r->inputs,r->input_count) ||
       full_signature(r->outputs,r->output_count))
      return fail(T70_SHADER_INVALID,message,message_size,"invalid DDI token request/signatures");
   if (r->gpu_id!=T70_SHADER_GPU_ID)
      return fail(T70_SHADER_UNSUPPORTED,message,message_size,"unsupported DDI shader GPU");
   const uint8_t *p=r->tokens;
   uint32_t version=le32(p),major=(version>>4)&15,minor=version&15;
   if (le32(p+4)!=r->tokens_size/4)
      return fail(T70_SHADER_INVALID,message,message_size,"invalid DDI token word count");
   if ((version>>16)!=r->stage || major<4 || major>5 || minor>1 || (major==5 && minor))
      return fail(T70_SHADER_UNSUPPORTED,message,message_size,"unsupported DDI shader model/stage");
   struct token_signature *signatures=calloc(2,sizeof(*signatures));
   if (!signatures) return fail(T70_SHADER_MEMORY,message,message_size,"DDI signature allocation failed");
   int status=T70_SHADER_OK;
   struct vkd3d_shader_code chunks[2]={0};
   for (size_t pos=8;pos<r->tokens_size;) {
      uint32_t instruction=le32(p+pos),opcode=instruction&0x7ff;
      size_t words=(instruction>>24)&127;
      if (opcode==0x35) {
         if (r->tokens_size-pos<8) { status=T70_SHADER_INVALID;break; }
         words=le32(p+pos+4);
         if (words<2) { status=T70_SHADER_INVALID;break; }
      }
      if (!words || words>(r->tokens_size-pos)/4) { status=T70_SHADER_INVALID;break; }
      if (opcode>=0x5f && opcode<=0x67) {
         bool output=opcode>=0x65;
         status=add_declaration(&signatures[output],p+pos+4,words*4-4,r->stage,opcode,instruction,
                  output?r->outputs:r->inputs,output?r->output_count:r->input_count);
         if (status) break;
      }
      pos+=words*4;
   }
   if (!status && upstream) status=link_clip_cull(&signatures[0],upstream);
   if (!status) status=signature_chunk(&signatures[0],r->stage,false,&r->types,upstream,&chunks[0]);
   if (!status) status=signature_chunk(&signatures[1],r->stage,true,&r->types,NULL,&chunks[1]);
   if (!status) {
      const struct vkd3d_shader_dxbc_section_desc sections[]={
         {.tag=0x4e475349,.data=chunks[0]}, {.tag=0x4e47534f,.data=chunks[1]},
         {.tag=major==5?0x58454853:0x52444853,.data={r->tokens,r->tokens_size}},
      };
      int result=vkd3d_shader_serialize_dxbc(3,sections,code,NULL);
      if (result<0) status=result==VKD3D_ERROR_OUT_OF_MEMORY?T70_SHADER_MEMORY:T70_SHADER_TRANSLATION;
   }
   if (!status && outputs) *outputs=signatures[1];
   free((void *)chunks[0].code);free((void *)chunks[1].code);free(signatures);
   if (status) fail(status,message,message_size,"invalid/unsupported DDI declarations or container allocation");
   return status;
}
int t70_shader_compile_tokens(const struct t70_shader_token_request *r,
                              struct t70_shader *out, char *message, size_t message_size)
{
   if (message && message_size) message[0]=0;
   if (!out || out->binary || out->spirv)
      return fail(T70_SHADER_INVALID,message,message_size,"nonempty or missing DDI shader output");
   struct vkd3d_shader_code code={0};
   int status=tokens_dxbc(r,&code,NULL,NULL,message,message_size);
   if (!status) {
      const struct t70_shader_request request={r->abi,r->gpu_id,r->stage,0,code.code,code.size};
      status=t70_shader_compile(&request,out,message,message_size);
   }
   vkd3d_shader_free_shader_code(&code);
   return status;
}
int t70_shader_compile_token_pipeline(const struct t70_shader_token_request *vertex,
                                     const struct t70_shader_token_request *pixel,
                                     struct t70_shader_pipeline *out,
                                     char *message, size_t message_size)
{
   if (message && message_size) message[0]=0;
   if (!vertex || !pixel || vertex->stage!=T70_SHADER_VERTEX || pixel->stage!=T70_SHADER_PIXEL ||
       !out || out->vertex.binary || out->vertex.spirv || out->pixel.binary || out->pixel.spirv)
      return fail(T70_SHADER_INVALID,message,message_size,"invalid DDI pipeline request/output");
   struct vkd3d_shader_code codes[2]={0};
   struct token_signature *outputs=calloc(1,sizeof(*outputs));
   if (!outputs) return fail(T70_SHADER_MEMORY,message,message_size,"DDI linkage allocation failed");
   int status=tokens_dxbc(vertex,&codes[0],NULL,outputs,message,message_size);
   if (!status) status=tokens_dxbc(pixel,&codes[1],outputs,NULL,message,message_size);
   if (!status) {
      const struct t70_shader_request vs={vertex->abi,vertex->gpu_id,vertex->stage,0,codes[0].code,codes[0].size};
      const struct t70_shader_request ps={pixel->abi,pixel->gpu_id,pixel->stage,0,codes[1].code,codes[1].size};
      status=compile_pipeline(&vs,&ps,out,true,message,message_size);
   }
   free(outputs);vkd3d_shader_free_shader_code(&codes[0]);vkd3d_shader_free_shader_code(&codes[1]);
   return status;
}
