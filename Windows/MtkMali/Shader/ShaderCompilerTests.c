/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShaderCompiler.h"
#include "../Commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "vkd3d_shader.h"
#include "compiler/spirv/spirv.h"
#include "compiler/shader_enums.h"
#include "panfrost/compiler/bifrost/valhall/disassemble.h"
static unsigned checks,cases,pipeline_cases,register_moves,component_moves,token_cases,token_pipelines;
static void token_case(const struct vkd3d_shader_code *,unsigned,const struct t70_shader *);
extern unsigned t70_shader_cpp_abi(void);
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static struct vkd3d_shader_code hlsl(const char *source, const char *profile)
{
   struct vkd3d_shader_hlsl_source_info hi={
      .type=VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO,.entry_point="main",.profile=profile,
   };
   struct vkd3d_shader_compile_info ci={
      .type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO,.next=&hi,
      .source={source,strlen(source)},.source_type=VKD3D_SHADER_SOURCE_HLSL,
      .target_type=VKD3D_SHADER_TARGET_DXBC_TPF,.log_level=VKD3D_SHADER_LOG_ERROR,
   };
   struct vkd3d_shader_code code={0};
   char *message=NULL;
   int status=vkd3d_shader_compile(&ci,&code,&message);
   if (status<0) fprintf(stderr,"HLSL: %s\n",message?message:"unknown");
   vkd3d_shader_free_messages(message);
   CHECK(status>=0 && code.size>=36);
   return code;
}
static void descriptor_case(const struct t70_shader *s)
{
   MALI_CAPS caps={.GpuId=T70_SHADER_GPU_ID};
   for (unsigned variant=0;variant<(s->stage==T70_SHADER_VERTEX?3u:1u);variant++) {
      if (variant==2 && !(s->flags&T70_SHADER_FLAG_SECONDARY)) continue;
      uint32_t offset=variant==2?s->secondary_offset:variant==1?s->no_point_size_offset:0;
      uint32_t registers=variant==2?s->secondary_registers:s->work_registers;
      uint64_t preload=variant==2?s->secondary_preload:s->preload;
      CHECK(!(offset&7) && offset<=s->binary_size-8 && registers && registers<=64);
      MALI_COMMAND_SHADER d={sizeof(d),s->stage==T70_SHADER_VERTEX?3u:2u,registers,
         s->stage==T70_SHADER_VERTEX && variant!=2?1u:0u,
         s->flags&T70_SHADER_FLAG_FTZ32?(s->flags&T70_SHADER_FLAG_FTZ16?2u:1u):0u,
         s->stage==T70_SHADER_PIXEL && (s->flags&T70_SHADER_FLAG_BARRIER)?1u:0u,
         UINT64_C(0xb0000000)+offset,preload};
      uint32_t words[8];memset(words,0xa7,sizeof(words));
      CHECK(MaliCommandsBuildShader(&caps,&d,words,sizeof(words))==MaliOk);
      CHECK((words[0]&15)==8 && ((words[0]>>4)&15)==d.Stage);
      CHECK(((words[0]>>17)&3)==d.FlushToZero && ((words[0]>>30)&3)==(registers<=32?2u:0u));
      CHECK(words[1]==preload>>48 && words[2]==0xb0000000u+offset && words[3]==0);
      CHECK(!words[4] && !words[5] && !words[6] && !words[7]);
   }
}
static void compile_case(const char *name,const char *source,const char *profile,
                          unsigned stage,unsigned cb_mask)
{
   struct vkd3d_shader_code code=hlsl(source,profile);
   struct t70_shader_request r={T70_SHADER_ABI,T70_SHADER_GPU_ID,stage,0,code.code,code.size};
   struct t70_shader s={0}; char message[1024];
   int status=t70_shader_compile(&r,&s,message,sizeof(message));
   if (status) fprintf(stderr,"%s: %d %s\n",name,status,message);
   CHECK(status==0);
   CHECK(s.binary && s.binary_size && !(s.binary_size&7));
   CHECK(s.cb_mask==cb_mask);
   unsigned ftz32=0;
   const uint32_t *spv=s.spirv;
   for (size_t word=5;word<s.spirv_size/4;word+=spv[word]>>16) {
      CHECK((spv[word]>>16)>0 && (spv[word]>>16)<=s.spirv_size/4-word);
      if ((spv[word]&65535)==SpvOpExecutionMode && (spv[word]>>16)==4 &&
          spv[word+2]==SpvExecutionModeDenormFlushToZero && spv[word+3]==32) ftz32=1;
   }
   CHECK(!!(s.flags&T70_SHADER_FLAG_FTZ32)==ftz32);
   CHECK(!(s.flags&T70_SHADER_FLAG_FTZ16)); /* SM4/5 cases use binary32 only. */
   CHECK(*(const uint32_t *)s.spirv==0x07230203u);
   CHECK(s.varying_count<=T70_SHADER_VARYINGS);
   CHECK(s.secondary_offset<s.binary_size || !s.secondary_offset);
   CHECK(s.no_point_size_offset<s.binary_size || !s.no_point_size_offset);
   if (stage==T70_SHADER_VERTEX && strstr(name,"attribute"))
      CHECK(s.attributes_read==1 && s.attribute_count==1);
   if (strstr(name,"discard")) CHECK(s.flags&T70_SHADER_FLAG_DISCARD);
   if (strstr(name,"Sample")) CHECK(s.sampler_mask && (s.texture_mask[0]|s.texture_mask[3]));
   if (strstr(name," Load")) CHECK(s.texture_mask[3]==0x80000000u && !s.sampler_mask);
   struct t70_shader again={0};
   CHECK(t70_shader_compile(&r,&again,message,sizeof(message))==0);
   CHECK(s.binary_size==again.binary_size && !memcmp(s.binary,again.binary,s.binary_size));
   CHECK(s.spirv_size==again.spirv_size && !memcmp(s.spirv,again.spirv,s.spirv_size));
   CHECK(s.flags==again.flags && s.fau_count==again.fau_count &&
         !memcmp(s.fau_words,again.fau_words,sizeof(s.fau_words)));
   token_case(&code,stage,&s);
   t70_shader_free(&again); ++cases;
   CHECK(s.stage==stage && s.arch==9 && s.work_registers>0 && s.work_registers<=64);
   CHECK(s.fau_count<=128 && s.fau_reserved==(stage==T70_SHADER_VERTEX?12:0));
   descriptor_case(&s);
   printf("%s: %zu DXBC -> %zu SPIR-V -> %zu Valhall bytes, %u registers\n",
          name,code.size,s.spirv_size,s.binary_size,s.work_registers);
   t70_shader_free(&s);
   vkd3d_shader_free_shader_code(&code);
}
struct thread_case {
   struct t70_shader_request request;
   const struct t70_shader *expected;
   int result;
};
static void *compile_thread(void *context)
{
   struct thread_case *c=context;
   for (unsigned i=0;i<8;i++) {
      struct t70_shader out={0};
      if (t70_shader_compile(&c->request,&out,NULL,0) || out.binary_size!=c->expected->binary_size ||
          memcmp(out.binary,c->expected->binary,out.binary_size) ||
          out.fau_count!=c->expected->fau_count ||
          memcmp(out.fau_words,c->expected->fau_words,sizeof(out.fau_words))) c->result=1;
      t70_shader_free(&out);
      if (c->result)break;
   }
   return NULL;
}
static void threaded_compile(void)
{
   struct vkd3d_shader_code code=hlsl(
      "Texture2D<float4> image:register(t5); SamplerState samp:register(s2);"
      "cbuffer C:register(b1) { float4 colour; };"
      "float4 main(float2 p:TEXCOORD):SV_Target { return image.Sample(samp,p)*colour; }","ps_4_0");
   struct t70_shader_request request={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,code.code,code.size};
   struct t70_shader expected={0};
   CHECK(t70_shader_compile(&request,&expected,NULL,0)==0);
   struct thread_case cases[4];pthread_t threads[4];
   for (unsigned i=0;i<4;i++) {
      cases[i]=(struct thread_case){request,&expected,0};
      CHECK(pthread_create(&threads[i],NULL,compile_thread,&cases[i])==0);
   }
   for (unsigned i=0;i<4;i++) {
      CHECK(pthread_join(threads[i],NULL)==0);
      CHECK(!cases[i].result);
   }
   CHECK(expected.cb_mask==2 && expected.texture_mask[0]==32 && expected.sampler_mask==4);
   t70_shader_free(&expected);vkd3d_shader_free_shader_code(&code);
}
static void write32(unsigned char *p,unsigned x)
{ p[0]=(unsigned char)x;p[1]=(unsigned char)(x>>8);p[2]=(unsigned char)(x>>16);p[3]=(unsigned char)(x>>24); }
static unsigned read32(const unsigned char *p)
{ return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
static void invalid_inputs(void)
{
   struct vkd3d_shader_code c=hlsl("float4 main():SV_Target { return 1; }","ps_4_0");
   struct t70_shader_request good={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,c.code,c.size};
   struct t70_shader_request r=good;
   struct t70_shader out={0};char msg[8];
   CHECK(t70_shader_compile(NULL,&out,msg,sizeof(msg))==T70_SHADER_INVALID && msg[7]==0);
   CHECK(t70_shader_compile(&r,NULL,NULL,0)==T70_SHADER_INVALID);
   r.abi++;CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);r=good;
   r.reserved=1;CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);r=good;
   r.gpu_id=0xdeadbeef;CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_UNSUPPORTED);r=good;
   r.stage=2;CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);r=good;
   r.stage=T70_SHADER_VERTEX;CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_UNSUPPORTED);r=good;
   r.dxbc=NULL;CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);r=good;
   r.dxbc_size=T70_SHADER_MAX_INPUT+4u;
   CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);r=good;
   for (size_t n=0;n<36;n++) {
      r.dxbc_size=n;
      CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);
   }
   r=good;
   unsigned char *bytes=malloc(c.size+1);CHECK(bytes!=NULL);
   memcpy(bytes+1,c.code,c.size);r.dxbc=bytes+1;
   CHECK(t70_shader_compile(&r,&out,NULL,0)==0);
   CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);
   t70_shader_free(&out);
   memcpy(bytes,c.code,c.size);r.dxbc=bytes;
   const unsigned fields[]={0,20,24,28,32};
   const unsigned values[]={0,2,0,0xffffffffu,0xfffffffcu};
   for (unsigned i=0;i<5;i++) {
      memcpy(bytes,c.code,c.size);write32(bytes+fields[i],values[i]);
      CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);
   }
   memcpy(bytes,c.code,c.size);
   unsigned chunk=read32(bytes+32);write32(bytes+chunk+4,0xfffffffcu);
   CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_INVALID);
   memcpy(bytes,c.code,c.size);bytes[4]^=1;
   CHECK(t70_shader_compile(&r,&out,NULL,0)==T70_SHADER_TRANSLATION);
   CHECK(!out.binary && !out.spirv && !out.binary_size && !out.spirv_size);
   t70_shader_free(&out);t70_shader_free(NULL);
   free(bytes);vkd3d_shader_free_shader_code(&c);
   struct vkd3d_shader_code unsupported=hlsl(
      "RWTexture2D<float4> image:register(u1);"
      "float4 main(float4 p:SV_Position):SV_Target { image[int2(p.xy)]=1; return 1; }",
      "ps_5_0");
   r=good;r.dxbc=unsupported.code;r.dxbc_size=unsupported.size;
   CHECK(t70_shader_compile(&r,&out,msg,1)==T70_SHADER_UNSUPPORTED && !msg[0]);
   vkd3d_shader_free_shader_code(&unsupported);
}
struct constant_oracle {
   uint32_t expected[32][4], masks[32], seen[32];
};
static _Thread_local struct constant_oracle *active_oracle;
void t70_shader_test_linked_constant(unsigned location,unsigned component,uint32_t value);
void t70_shader_test_linked_constant(unsigned location,unsigned component,uint32_t value)
{
   if (!active_oracle) return;
   CHECK(location<32 && component<4);
   CHECK(active_oracle->masks[location]&(1u<<component));
   CHECK(active_oracle->expected[location][component]==value);
   active_oracle->seen[location]|=1u<<component;
}
static _Thread_local unsigned clip_slot,clip_inputs;
void t70_shader_test_linked_input(unsigned location,unsigned component,unsigned count);
void t70_shader_test_linked_input(unsigned location,unsigned component,unsigned count)
{
   if (!clip_slot) return;
   CHECK(location>=clip_slot && location<=clip_slot+1 && component<4 && count<=4-component);
   clip_inputs|=((1u<<count)-1)<<(4*(location-clip_slot)+component);
}
static struct vkd3d_shader_scan_signature_info signatures(const struct vkd3d_shader_code *code)
{
   struct vkd3d_shader_scan_signature_info scan={.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO};
   struct vkd3d_shader_compile_info ci={
      .type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO,.next=&scan,.source=*code,
      .source_type=VKD3D_SHADER_SOURCE_DXBC_TPF,.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY,
      .log_level=VKD3D_SHADER_LOG_ERROR,
   };
   CHECK(vkd3d_shader_scan(&ci,NULL)>=0);
   return scan;
}
static const struct t70_shader_varying *varying(const struct t70_shader *s, unsigned location)
{
   for (unsigned i=0;i<s->varying_count;i++)
      if (s->varyings[i].bits && s->varyings[i].location==location) return &s->varyings[i];
   return NULL;
}
static void same_machine_shader(const struct t70_shader *a,const struct t70_shader *b)
{
   if (a->binary_size!=b->binary_size || memcmp(a->binary,b->binary,a->binary_size)) {
      fprintf(stderr,"machine code differs for stage %u (%zu/%zu bytes)\n",a->stage,a->binary_size,b->binary_size);
      disassemble_valhall(stderr,a->binary,a->binary_size,true);
      disassemble_valhall(stderr,b->binary,b->binary_size,true);
   }
   CHECK(a->binary_size==b->binary_size && !memcmp(a->binary,b->binary,a->binary_size));
   CHECK(a->flags==b->flags && a->fau_count==b->fau_count);
   CHECK(!memcmp(a->fau_words,b->fau_words,sizeof(a->fau_words)));
   CHECK(!memcmp(a->fau_const_mask,b->fau_const_mask,sizeof(a->fau_const_mask)));
}
static void sample_interpolation(struct vkd3d_shader_code *code)
{
   /* The pinned HLSL producer cannot spell 'sample'. Build a real SM4.1
    * declaration with the documented D3D token bits and seal its checksum. */
   struct vkd3d_shader_dxbc_desc desc={0};
   CHECK(vkd3d_shader_parse_dxbc(code,0,&desc,NULL)>=0);
   void *tokens=NULL;unsigned changes=0;
   for (size_t i=0;i<desc.section_count;i++) {
      struct vkd3d_shader_dxbc_section_desc *section=&desc.sections[i];
      if (section->tag!=0x52444853 && section->tag!=0x58454853) continue;
      CHECK(!tokens && section->data.size>=8 && !(section->data.size&3));
      unsigned char *bytes=malloc(section->data.size);CHECK(bytes!=NULL);
      memcpy(bytes,section->data.code,section->data.size);tokens=bytes;section->data.code=bytes;
      write32(bytes,read32(bytes)|1u);
      size_t n=section->data.size/4;
      for (size_t offset=2;offset<n;) {
         uint32_t word=read32(bytes+4*offset);unsigned count=(word>>24)&31;
         CHECK(count && count<=n-offset);
         if ((word&0xff)==0x62) {
            CHECK(((word>>11)&15)==4);
            write32(bytes+4*offset,(word&~(15u<<11))|(7u<<11));changes++;
         }
         offset+=count;
      }
   }
   CHECK(tokens && changes);
   struct vkd3d_shader_code result={0};
   CHECK(vkd3d_shader_serialize_dxbc(desc.section_count,desc.sections,&result,NULL)>=0);
   vkd3d_shader_free_dxbc(&desc);free(tokens);vkd3d_shader_free_shader_code(code);*code=result;
}
static bool spirv_decoration(const struct t70_shader *shader, unsigned decoration)
{
   const uint32_t *words=shader->spirv;size_t n=shader->spirv_size/4;
   for (size_t offset=5;offset<n;) {
      unsigned count=words[offset]>>16,opcode=words[offset]&0xffff;
      CHECK(count && count<=n-offset);
      if (opcode==SpvOpDecorate && count>=3 && words[offset+2]==decoration) return true;
      offset+=count;
   }
   return false;
}
static void pipeline_case(const char *name,const char *vertex,const char *pixel,
                          const char *reference,const char *noperspective)
{
   struct vkd3d_shader_code vs=hlsl(vertex,"vs_4_0"),ps=hlsl(pixel,"ps_4_0");
   if (strstr(name,"sample")) sample_interpolation(&ps);
   struct t70_shader_request vr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_VERTEX,0,vs.code,vs.size};
   struct t70_shader_request pr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,ps.code,ps.size};
   struct vkd3d_shader_scan_signature_info vsi=signatures(&vs),psi=signatures(&ps);
   unsigned expected_noperspective=0;
   for (unsigned i=0;i<psi.input.element_count;i++) {
      const struct vkd3d_shader_signature_element *input=&psi.input.elements[i];
      if (input->sysval_semantic!=VKD3D_SHADER_SV_NONE) continue;
      const struct vkd3d_shader_signature_element *output=
         vkd3d_shader_find_signature_element(&vsi.output,input->semantic_name,input->semantic_index,0);
      CHECK(output!=NULL);
      register_moves+=input->register_index!=output->register_index;
      component_moves+=input->mask!=output->mask;
      if (noperspective && !strcmp(input->semantic_name,noperspective))
         expected_noperspective|=1u<<input->register_index;
   }
   struct t70_shader_pipeline out={0},again={0};char message[1024];
   int status=t70_shader_compile_pipeline(&vr,&pr,&out,message,sizeof(message));
   if (status) fprintf(stderr,"pipeline %s: %d %s\n",name,status,message);
   CHECK(status==T70_SHADER_OK);
   CHECK(out.vertex.binary_size && out.pixel.binary_size);
   CHECK(out.varying_stride==out.vertex.varying_stride && out.varying_stride==out.pixel.varying_stride);
   CHECK(out.noperspective_mask==expected_noperspective);
   if (noperspective) {
      CHECK(expected_noperspective!=0);
      CHECK(spirv_decoration(&out.pixel,SpvDecorationNoPerspective));
   }
   if (strstr(name,"centroid")) CHECK(spirv_decoration(&out.pixel,SpvDecorationCentroid));
   if (strstr(name,"sample")) CHECK(spirv_decoration(&out.pixel,SpvDecorationSample));
   unsigned linked=0;
   for (unsigned i=0;i<out.pixel.varying_count;i++) {
      const struct t70_shader_varying *in=&out.pixel.varyings[i];
      if (!in->bits || in->section!=3) continue;
      const struct t70_shader_varying *memory=varying(&out.vertex,in->location);
      CHECK(memory!=NULL);
      CHECK(in->offset==memory->offset && in->bits==memory->bits);
      CHECK(in->components<=memory->components);
      CHECK(in->offset>=0 && in->offset+memory->components*(memory->bits/8)<=out.varying_stride);
      linked++;
   }
   if (psi.input.element_count) CHECK(linked || !out.varying_stride);
   CHECK(t70_shader_compile_pipeline(&vr,&pr,&again,message,sizeof(message))==T70_SHADER_OK);
   same_machine_shader(&out.vertex,&again.vertex);same_machine_shader(&out.pixel,&again.pixel);
   CHECK(t70_shader_compile_pipeline(&vr,&pr,&again,NULL,0)==T70_SHADER_INVALID);
   if (reference) {
      struct vkd3d_shader_code ref=hlsl(reference,"vs_4_0");
      struct t70_shader_request rr=vr;rr.dxbc=ref.code;rr.dxbc_size=ref.size;
      struct t70_shader_pipeline expected={0};
      CHECK(t70_shader_compile_pipeline(&rr,&pr,&expected,message,sizeof(message))==T70_SHADER_OK);
      /* Different VS schedules are valid. The identical PS must read the
       * same packet; constant lane values are checked independently below. */
      CHECK(out.vertex.varying_count==expected.vertex.varying_count);
      CHECK(!memcmp(out.vertex.varyings,expected.vertex.varyings,sizeof(out.vertex.varyings)));
      same_machine_shader(&out.pixel,&expected.pixel);
      CHECK(out.varying_stride==expected.varying_stride);
      t70_shader_free_pipeline(&expected);vkd3d_shader_free_shader_code(&ref);
   }
   if (strstr(name,"unused")) {
      struct t70_shader independent={0};
      CHECK(t70_shader_compile(&vr,&independent,message,sizeof(message))==T70_SHADER_OK);
      CHECK(independent.varying_stride>out.varying_stride);
      t70_shader_free(&independent);
   }
   printf("pipeline %s: VS %zu / PS %zu Valhall bytes; packet %u bytes; noperspective %#x\n",
          name,out.vertex.binary_size,out.pixel.binary_size,out.varying_stride,out.noperspective_mask);
   t70_shader_free_pipeline(&out);t70_shader_free_pipeline(&again);t70_shader_free_pipeline(&out);
   CHECK(!out.vertex.binary && !out.pixel.spirv && !out.varying_stride && !out.noperspective_mask);
   vkd3d_shader_free_scan_signature_info(&vsi);vkd3d_shader_free_scan_signature_info(&psi);
   vkd3d_shader_free_shader_code(&vs);vkd3d_shader_free_shader_code(&ps);
   pipeline_cases++;
}
static void constant_link_values(void)
{
   struct vkd3d_shader_code vs=hlsl(
      "struct O { float4 p:SV_Position; float2 a:VALUE0; float b:VALUE1; float c:VALUE2; float4 unused:EXTRA; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.a=float2(0.125,0.25); o.b=0.5; o.c=1; o.unused=9; return o; }","vs_4_0");
   struct vkd3d_shader_code ps=hlsl(
      "struct I { float c:VALUE2; float2 a:VALUE0; float b:VALUE1; };"
      "float4 main(I i):SV_Target { return float4(i.a,i.b,i.c); }","ps_4_0");
   struct vkd3d_shader_scan_signature_info sig=signatures(&ps);
   const float values[3][2]={{0.125f,0.25f},{0.5f,0},{1,0}};
   struct constant_oracle oracle={0};
   for (unsigned i=0;i<sig.input.element_count;i++) {
      const struct vkd3d_shader_signature_element *input=&sig.input.elements[i];
      CHECK(input->sysval_semantic==VKD3D_SHADER_SV_NONE && input->semantic_index<3);
      unsigned index=0;
      for (unsigned c=0;c<4;c++) {
         if (!(input->mask&(1u<<c))) continue;
         CHECK(index<2);
         memcpy(&oracle.expected[input->register_index][c],&values[input->semantic_index][index++],4);
         oracle.masks[input->register_index]|=1u<<c;
      }
   }
   struct t70_shader_request vr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_VERTEX,0,vs.code,vs.size};
   struct t70_shader_request pr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,ps.code,ps.size};
   struct t70_shader_pipeline out={0};char msg[1024];
   active_oracle=&oracle;
   int status=t70_shader_compile_pipeline(&vr,&pr,&out,msg,sizeof(msg));
   active_oracle=NULL;
   if (status) fprintf(stderr,"constant linkage: %s\n",msg);
   CHECK(status==T70_SHADER_OK);
   CHECK(!memcmp(oracle.masks,oracle.seen,sizeof(oracle.masks)));
   t70_shader_free_pipeline(&out);
   vkd3d_shader_free_scan_signature_info(&sig);
   vkd3d_shader_free_shader_code(&vs);vkd3d_shader_free_shader_code(&ps);
   pipeline_cases++;
}
struct pipeline_thread_case {
   struct t70_shader_request vertex,pixel;
   const struct t70_shader_pipeline *expected;
   int result;
};
static bool same_binary(const struct t70_shader *a,const struct t70_shader *b)
{
   return a->binary_size==b->binary_size && !memcmp(a->binary,b->binary,a->binary_size) &&
      a->fau_count==b->fau_count && !memcmp(a->fau_words,b->fau_words,sizeof(a->fau_words));
}
static void *pipeline_thread(void *context)
{
   struct pipeline_thread_case *c=context;
   for (unsigned i=0;i<8;i++) {
      struct t70_shader_pipeline out={0};
      if (t70_shader_compile_pipeline(&c->vertex,&c->pixel,&out,NULL,0) ||
          !same_binary(&out.vertex,&c->expected->vertex) || !same_binary(&out.pixel,&c->expected->pixel) ||
          out.varying_stride!=c->expected->varying_stride ||
          out.noperspective_mask!=c->expected->noperspective_mask) c->result=1;
      t70_shader_free_pipeline(&out);
      if (c->result) break;
   }
   return NULL;
}
static void threaded_pipeline(void)
{
   struct vkd3d_shader_code vs=hlsl(
      "cbuffer C:register(b4) { float4 offset; };"
      "struct O { float4 p:SV_Position; float4 c:COLOR; float2 uv:TEXCOORD; };"
      "O main(float4 p:POSITION) { O o; o.p=p+offset; o.c=p.yxwz; o.uv=p.zw; return o; }","vs_4_0");
   struct vkd3d_shader_code ps=hlsl(
      "Texture2D<float4> image:register(t5); SamplerState samp:register(s2);"
      "struct I { noperspective float2 uv:TEXCOORD; float4 c:COLOR; };"
      "float4 main(I i):SV_Target { return image.Sample(samp,i.uv)*i.c; }","ps_4_0");
   struct t70_shader_request vr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_VERTEX,0,vs.code,vs.size};
   struct t70_shader_request pr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,ps.code,ps.size};
   struct t70_shader_pipeline expected={0};char msg[1024];
   CHECK(t70_shader_compile_pipeline(&vr,&pr,&expected,msg,sizeof(msg))==T70_SHADER_OK);
   CHECK(expected.vertex.cb_mask==16 && expected.pixel.texture_mask[0]==32 && expected.pixel.sampler_mask==4);
   CHECK(expected.noperspective_mask==1);
   struct pipeline_thread_case cases[4];pthread_t threads[4];
   for (unsigned i=0;i<4;i++) {
      cases[i]=(struct pipeline_thread_case){vr,pr,&expected,0};
      CHECK(pthread_create(&threads[i],NULL,pipeline_thread,&cases[i])==0);
   }
   for (unsigned i=0;i<4;i++) {
      CHECK(pthread_join(threads[i],NULL)==0);CHECK(!cases[i].result);
   }
   t70_shader_free_pipeline(&expected);
   vkd3d_shader_free_shader_code(&vs);vkd3d_shader_free_shader_code(&ps);
}
static void invalid_pipeline(void)
{
   struct vkd3d_shader_code vs=hlsl(
      "struct O { float4 p:SV_Position; float4 c:COLOR; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.c=p.yxwz; return o; }","vs_4_0");
   struct vkd3d_shader_code ps=hlsl("float4 main(float4 c:COLOR):SV_Target { return c; }","ps_4_0");
   struct t70_shader_request vr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_VERTEX,0,vs.code,vs.size};
   struct t70_shader_request pr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,ps.code,ps.size};
   struct t70_shader_pipeline out={0};char message[1];
   out.varying_stride=0xface;
   struct t70_shader_pipeline original=out;
   CHECK(t70_shader_compile_pipeline(NULL,&pr,&out,message,1)==T70_SHADER_INVALID && !message[0]);
   CHECK(t70_shader_compile_pipeline(&vr,NULL,&out,NULL,0)==T70_SHADER_INVALID);
   CHECK(t70_shader_compile_pipeline(&vr,&pr,NULL,NULL,0)==T70_SHADER_INVALID);
   struct t70_shader_request bad=vr;bad.stage=T70_SHADER_PIXEL;
   CHECK(t70_shader_compile_pipeline(&bad,&pr,&out,NULL,0)==T70_SHADER_INVALID);
   bad=pr;bad.gpu_id=0;
   CHECK(t70_shader_compile_pipeline(&vr,&bad,&out,NULL,0)==T70_SHADER_UNSUPPORTED);
   bad=pr;bad.dxbc_size=32;
   CHECK(t70_shader_compile_pipeline(&vr,&bad,&out,NULL,0)==T70_SHADER_INVALID);
   const char *invalid[]={
      "float4 main(float4 c:MISSING):SV_Target { return c; }",
      "float4 main(nointerpolation uint4 c:COLOR):SV_Target { return c; }",
      "RWTexture2D<float4> image:register(u1); float4 main(float4 c:COLOR):SV_Target { image[int2(c.xy)]=c; return c; }",
   };
   for (unsigned i=0;i<3;i++) {
      struct vkd3d_shader_code code=hlsl(invalid[i],"ps_5_0");
      bad=pr;bad.dxbc=code.code;bad.dxbc_size=code.size;
      CHECK(t70_shader_compile_pipeline(&vr,&bad,&out,message,1)==T70_SHADER_UNSUPPORTED && !message[0]);
      CHECK(!memcmp(&out,&original,sizeof(out)));
      vkd3d_shader_free_shader_code(&code);
   }
   struct vkd3d_shader_code narrow=hlsl(
      "struct O { float4 p:SV_Position; float2 c:COLOR; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.c=p.xy; return o; }","vs_4_0");
   bad=vr;bad.dxbc=narrow.code;bad.dxbc_size=narrow.size;
   CHECK(t70_shader_compile_pipeline(&bad,&pr,&out,message,1)==T70_SHADER_UNSUPPORTED && !message[0]);
   vkd3d_shader_free_shader_code(&narrow);
   CHECK(!memcmp(&out,&original,sizeof(out)));
   t70_shader_free_pipeline(&out);t70_shader_free_pipeline(NULL);
   vkd3d_shader_free_shader_code(&vs);vkd3d_shader_free_shader_code(&ps);
}
struct token_input {
   struct t70_shader_token_request request;
   struct t70_shader_signature_entry inputs[T70_SHADER_MAX_SIGNATURE],outputs[T70_SHADER_MAX_SIGNATURE];
};
static void token_input(struct token_input *out,const struct vkd3d_shader_code *code,unsigned stage)
{
   memset(out,0,sizeof(*out));
   out->request=(struct t70_shader_token_request){.abi=T70_SHADER_ABI,.gpu_id=T70_SHADER_GPU_ID,.stage=stage,
      .inputs=out->inputs,.outputs=out->outputs};
   struct vkd3d_shader_dxbc_desc desc={0};
   CHECK(vkd3d_shader_parse_dxbc(code,0,&desc,NULL)>=0);
   for (size_t i=0;i<desc.section_count;i++) {
      if (desc.sections[i].tag!=0x52444853 && desc.sections[i].tag!=0x58454853) continue;
      CHECK(!out->request.tokens);
      out->request.tokens=desc.sections[i].data.code;out->request.tokens_size=desc.sections[i].data.size;
   }
   CHECK(out->request.tokens);
   vkd3d_shader_free_dxbc(&desc);
   struct vkd3d_shader_scan_signature_info scan=signatures(code);
   const struct vkd3d_shader_signature *sigs[]={&scan.input,&scan.output};
   for (unsigned j=0;j<2;j++) {
      struct t70_shader_signature_entry *entries=j?out->outputs:out->inputs;
      uint32_t *count=j?&out->request.output_count:&out->request.input_count;
      for (unsigned i=0;i<sigs[j]->element_count;i++) {
         const struct vkd3d_shader_signature_element *e=&sigs[j]->elements[i];
         if (e->register_index==UINT32_MAX) continue; /* scalar special-register outputs */
         CHECK(*count<T70_SHADER_MAX_SIGNATURE && e->register_index<31);
         if (stage==T70_SHADER_VERTEX && j==0 && e->sysval_semantic==VKD3D_SHADER_SV_NONE) {
            if (e->component_type==VKD3D_SHADER_COMPONENT_UINT)out->request.types.vertex_uint|=1u<<e->register_index;
            if (e->component_type==VKD3D_SHADER_COMPONENT_INT)out->request.types.vertex_sint|=1u<<e->register_index;
         }
         if (stage==T70_SHADER_PIXEL && j==1 && e->sysval_semantic==VKD3D_SHADER_SV_TARGET) {
            if (e->component_type==VKD3D_SHADER_COMPONENT_UINT)out->request.types.target_uint|=1u<<e->register_index;
            if (e->component_type==VKD3D_SHADER_COMPONENT_INT)out->request.types.target_sint|=1u<<e->register_index;
         }
         entries[(*count)++]=(struct t70_shader_signature_entry){
            e->sysval_semantic<=10?e->sysval_semantic:0,e->register_index,e->mask};
      }
      /* Runtime full signatures can include declarations not used by this shader. */
      CHECK(*count<T70_SHADER_MAX_SIGNATURE);
      entries[(*count)++]=(struct t70_shader_signature_entry){0,31,1};
   }
   vkd3d_shader_free_scan_signature_info(&scan);
}
static void token_case(const struct vkd3d_shader_code *code,unsigned stage,const struct t70_shader *expected)
{
   struct token_input in;token_input(&in,code,stage);
   struct t70_shader out={0};char message[1024];
   int status=t70_shader_compile_tokens(&in.request,&out,message,sizeof(message));
   if (status) fprintf(stderr,"DDI tokens: %d %s\n",status,message);
   CHECK(status==T70_SHADER_OK);same_machine_shader(expected,&out);
   CHECK(out.stage==stage && out.cb_mask==expected->cb_mask && out.attributes_read==expected->attributes_read);
   CHECK(!memcmp(out.blend_type,expected->blend_type,sizeof(out.blend_type)));
   CHECK(out.sampler_mask==expected->sampler_mask && !memcmp(out.texture_mask,expected->texture_mask,sizeof(out.texture_mask)));
   CHECK(t70_shader_compile_tokens(&in.request,&out,NULL,0)==T70_SHADER_INVALID);
   t70_shader_free(&out);
   in.request.input_count=in.request.output_count=0;in.request.inputs=in.request.outputs=NULL;
   CHECK(t70_shader_compile_tokens(&in.request,&out,message,sizeof(message))==T70_SHADER_OK);
   same_machine_shader(expected,&out);t70_shader_free(&out);token_cases++;
}
static void token_pipeline_case(const char *vertex,const char *pixel,const char *profile,bool sample)
{
   struct vkd3d_shader_code vs=hlsl(vertex,"vs_4_0"),ps=hlsl(pixel,profile);
   if (sample) sample_interpolation(&ps);
   struct token_input vi,pi;token_input(&vi,&vs,T70_SHADER_VERTEX);token_input(&pi,&ps,T70_SHADER_PIXEL);
   struct t70_shader_request vr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_VERTEX,0,vs.code,vs.size};
   struct t70_shader_request pr={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,ps.code,ps.size};
   struct t70_shader_pipeline expected={0},out={0};char message[1024];
   CHECK(t70_shader_compile_pipeline(&vr,&pr,&expected,message,sizeof(message))==T70_SHADER_OK);
   int status=t70_shader_compile_token_pipeline(&vi.request,&pi.request,&out,message,sizeof(message));
   if (status) fprintf(stderr,"DDI pipeline: %d %s\n",status,message);
   CHECK(status==T70_SHADER_OK);
   same_machine_shader(&expected.vertex,&out.vertex);same_machine_shader(&expected.pixel,&out.pixel);
   CHECK(out.varying_stride==expected.varying_stride && out.noperspective_mask==expected.noperspective_mask);
   CHECK(t70_shader_compile_token_pipeline(&vi.request,&pi.request,&out,NULL,0)==T70_SHADER_INVALID);
   t70_shader_free_pipeline(&out);t70_shader_free_pipeline(&expected);
   vkd3d_shader_free_shader_code(&vs);vkd3d_shader_free_shader_code(&ps);token_pipelines++;
}
static void token_coordinates(void)
{
   /* Runtime coordinates: PS consumes only o0.zw. A semantic-style linker
    * would wrongly forward o0.xy instead; observe the real lowered values. */
   const uint32_t vertex[]={0x10040,26,
      0x0300005f,0x001010f2,0,
      0x03000065,0x001020f2,0,
      0x04000067,0x001020f2,1,1,
      0x08000036,0x001020f2,0,0x00004002,0x3e000000,0x3e800000,0x3f000000,0x3f800000,
      0x05000036,0x001020f2,1,0x00101e46,0,
      0x0100003e};
   const uint32_t pixel[]={0x40,18,
      0x00000035,4,0x03000065,0x001020f2, /* comment data is not a declaration */
      0x03001062,0x001010c2,0,
      0x03000065,0x00102032,0,
      0x05000036,0x00102032,0,0x00101ee6,0,
      0x0100003e};
   CHECK(sizeof(vertex)/4==vertex[1] && sizeof(pixel)/4==pixel[1]);
   struct t70_shader_token_request vr={.abi=T70_SHADER_ABI,.gpu_id=T70_SHADER_GPU_ID,.stage=T70_SHADER_VERTEX,
      .tokens=vertex,.tokens_size=sizeof(vertex)},pr=vr;
   pr.stage=T70_SHADER_PIXEL;pr.tokens=pixel;pr.tokens_size=sizeof(pixel);
   struct constant_oracle oracle={0};oracle.masks[0]=12;
   oracle.expected[0][2]=0x3f000000;oracle.expected[0][3]=0x3f800000;
   struct t70_shader_pipeline out={0};char message[1024];active_oracle=&oracle;
   int status=t70_shader_compile_token_pipeline(&vr,&pr,&out,message,sizeof(message));active_oracle=NULL;
   if (status) fprintf(stderr,"DDI coordinate pipeline: %d %s\n",status,message);
   CHECK(status==T70_SHADER_OK && oracle.seen[0]==12);
   CHECK(out.varying_stride && out.vertex.binary_size && out.pixel.binary_size);
   t70_shader_free_pipeline(&out);
   uint32_t missing[sizeof(vertex)/4];memcpy(missing,vertex,sizeof(vertex));
   missing[7]=missing[14]=2; /* valid VS writes o2, but PS requires o0 */
   vr.tokens=missing;
   out.varying_stride=0xbeef;struct t70_shader_pipeline unchanged=out;
   CHECK(t70_shader_compile_token_pipeline(&vr,&pr,&out,NULL,0)==T70_SHADER_UNSUPPORTED);
   CHECK(!memcmp(&out,&unchanged,sizeof(out)));token_pipelines++;
}
static unsigned builtin_indices(const struct t70_shader *shader,unsigned builtin)
{
   const uint32_t *words=shader->spirv;size_t n=shader->spirv_size/4;
   unsigned variable=0,indices=0,bound=words[3];
   uint32_t *constants=calloc(bound,sizeof(*constants));CHECK(constants);
   for (size_t offset=5;offset<n;) {
      unsigned count=words[offset]>>16,opcode=words[offset]&0xffff;
      CHECK(count && count<=n-offset);
      if (opcode==SpvOpDecorate && count==4 && words[offset+2]==SpvDecorationBuiltIn && words[offset+3]==builtin)
         variable=words[offset+1];
      if (opcode==SpvOpConstant && count==4) {
         CHECK(words[offset+2]<bound);constants[words[offset+2]]=words[offset+3];
      }
      offset+=count;
   }
   CHECK(variable);
   for (size_t offset=5;offset<n;) {
      unsigned count=words[offset]>>16,opcode=words[offset]&0xffff;
      if (opcode==SpvOpAccessChain && count>=5 && words[offset+3]==variable) {
         CHECK(words[offset+count-1]<bound);
         unsigned index=constants[words[offset+count-1]];CHECK(index<8);indices|=1u<<index;
      }
      offset+=count;
   }
   free(constants);return indices;
}
static void token_clip_subset(unsigned system,unsigned builtin)
{
   uint32_t vertex[]={0x10040,39,
      0x04000067,0x001020f2,0,system,
      0x04000067,0x001020f2,1,system,
      0x04000067,0x001020f2,2,1,
      0x08000036,0x001020f2,0,0x00004002,0x3f800000,0x40000000,0x40400000,0x40800000,
      0x08000036,0x001020f2,1,0x00004002,0x40a00000,0x40c00000,0x40e00000,0x41000000,
      0x08000036,0x001020f2,2,0x00004002,0,0,0,0x3f800000,
      0x0100003e};
   uint32_t pixel[]={0x40,15,
      0x04001064,0x001010f2,1,system,
      0x03000065,0x001020f2,0,
      0x05000036,0x001020f2,0,0x00101e46,1,
      0x0100003e};
   CHECK(sizeof(vertex)/4==vertex[1] && sizeof(pixel)/4==pixel[1]);
   struct t70_shader_token_request vr={.abi=T70_SHADER_ABI,.gpu_id=T70_SHADER_GPU_ID,.stage=T70_SHADER_VERTEX,
      .tokens=vertex,.tokens_size=sizeof(vertex)},pr=vr;
   pr.stage=T70_SHADER_PIXEL;pr.tokens=pixel;pr.tokens_size=sizeof(pixel);
   struct t70_shader_pipeline out={0};char message[1024];
   clip_slot=system==2?VARYING_SLOT_CLIP_DIST0:VARYING_SLOT_CULL_DIST0;clip_inputs=0;
   int status=t70_shader_compile_token_pipeline(&vr,&pr,&out,message,sizeof(message));
   clip_slot=0;
   if (status) fprintf(stderr,"DDI clip/cull subset %u: %d %s\n",system,status,message);
   CHECK(status==T70_SHADER_OK);
   CHECK(builtin_indices(&out.vertex,builtin)==255);
   /* SPIR-V copies the full array to temporaries; the real NIR lowering
    * must eliminate unused lanes and read only the VS's second register. */
   CHECK(builtin_indices(&out.pixel,builtin)==255);
   CHECK(clip_inputs==240);
   t70_shader_free_pipeline(&out);
   pixel[4]=3; /* register has no matching VS clip/cull output */
   CHECK(t70_shader_compile_token_pipeline(&vr,&pr,&out,NULL,0)==T70_SHADER_UNSUPPORTED);
   CHECK(!out.vertex.binary && !out.pixel.binary);token_pipelines++;
}
static void invalid_tokens(void)
{
   struct vkd3d_shader_code code=hlsl("float4 main(float4 c:COLOR):SV_Target { return c; }","ps_4_0");
   struct token_input input;token_input(&input,&code,T70_SHADER_PIXEL);
   struct t70_shader_token_request good=input.request,bad=good;
   struct t70_shader out={.work_registers=0xface},unchanged=out;
   char msg[1];
   CHECK(t70_shader_compile_tokens(NULL,&out,msg,1)==T70_SHADER_INVALID && !msg[0]);
   CHECK(t70_shader_compile_tokens(&good,NULL,NULL,0)==T70_SHADER_INVALID);
   for (size_t n=0;n<good.tokens_size;n++) {
      bad=good;bad.tokens_size=n;
      CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
      CHECK(!memcmp(&out,&unchanged,sizeof(out)));
   }
   bad=good;bad.tokens_size=T70_SHADER_MAX_INPUT+4u;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   bad=good;bad.abi++;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   bad=good;bad.reserved=1;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   bad=good;bad.gpu_id=0;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_UNSUPPORTED);
   bad=good;bad.stage=T70_SHADER_VERTEX;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_UNSUPPORTED);
   bad=good;bad.types.vertex_uint=bad.types.vertex_sint=1;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   bad=good;bad.types.target_uint=bad.types.target_sint=1;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   bad=good;bad.types.target_uint=256;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   bad=good;bad.inputs=NULL;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   bad=good;bad.input_count=T70_SHADER_MAX_SIGNATURE+1;
   CHECK(t70_shader_compile_tokens(&bad,&out,NULL,0)==T70_SHADER_INVALID);
   struct t70_shader_signature_entry saved=input.inputs[0];
   input.inputs[0].mask=0;
   CHECK(t70_shader_compile_tokens(&good,&out,NULL,0)==T70_SHADER_INVALID);
   input.inputs[0]=saved;input.inputs[0].system_value=11;
   CHECK(t70_shader_compile_tokens(&good,&out,NULL,0)==T70_SHADER_INVALID);
   input.inputs[0]=saved;input.inputs[0].register_index=32;
   CHECK(t70_shader_compile_tokens(&good,&out,NULL,0)==T70_SHADER_INVALID);
   input.inputs[0]=saved;input.inputs[0].mask=1;
   CHECK(t70_shader_compile_tokens(&good,&out,NULL,0)==T70_SHADER_INVALID);
   input.inputs[0]=saved;input.inputs[1]=saved;
   CHECK(t70_shader_compile_tokens(&good,&out,NULL,0)==T70_SHADER_INVALID);
   input.inputs[1]=(struct t70_shader_signature_entry){0,31,1};
   unsigned char *copy=malloc(good.tokens_size+1);CHECK(copy);
   memcpy(copy+1,good.tokens,good.tokens_size);bad=good;bad.tokens=copy+1;
   struct t70_shader unaligned={0};
   CHECK(t70_shader_compile_tokens(&bad,&unaligned,NULL,0)==T70_SHADER_OK);t70_shader_free(&unaligned);
   const unsigned offsets[]={0,4,8,8,8,12,16};
   const unsigned words[]={0x42,0xffffffffu,0x00000062,0x7f000062,0x01000035,0x00000002,32};
   for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
      memcpy(copy,good.tokens,good.tokens_size);bad.tokens=copy;
      write32(copy+offsets[i],words[i]);
      CHECK(t70_shader_compile_tokens(&bad,&out,msg,1)!=T70_SHADER_OK && !msg[0]);
      CHECK(!memcmp(&out,&unchanged,sizeof(out)));
   }
   free(copy);vkd3d_shader_free_shader_code(&code);
}
struct token_thread {
   struct t70_shader_token_request vertex,pixel;
   const struct t70_shader_pipeline *expected;
   int failed;
};
static void *token_thread(void *arg)
{
   struct token_thread *t=arg;
   for (unsigned i=0;i<8;i++) {
      struct t70_shader_pipeline out={0};
      if (t70_shader_compile_token_pipeline(&t->vertex,&t->pixel,&out,NULL,0) ||
          out.vertex.binary_size!=t->expected->vertex.binary_size ||
          out.pixel.binary_size!=t->expected->pixel.binary_size ||
          memcmp(out.vertex.binary,t->expected->vertex.binary,out.vertex.binary_size) ||
          memcmp(out.pixel.binary,t->expected->pixel.binary,out.pixel.binary_size) ||
          out.varying_stride!=t->expected->varying_stride ||
          out.noperspective_mask!=t->expected->noperspective_mask)t->failed=1;
      t70_shader_free_pipeline(&out);
   }
   return NULL;
}
static void token_threaded(void)
{
   struct vkd3d_shader_code vs=hlsl("struct O { float4 c:COLOR; float4 p:SV_Position; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.c=p.yxwz; return o; }","vs_4_0");
   struct vkd3d_shader_code ps=hlsl("float4 main(noperspective float4 c:COLOR):SV_Target { return c; }","ps_4_0");
   struct token_input vi,pi;token_input(&vi,&vs,T70_SHADER_VERTEX);token_input(&pi,&ps,T70_SHADER_PIXEL);
   struct t70_shader_pipeline expected={0};
   CHECK(t70_shader_compile_token_pipeline(&vi.request,&pi.request,&expected,NULL,0)==T70_SHADER_OK);
   pthread_t threads[4];struct token_thread cases[4];
   for (unsigned i=0;i<4;i++) {
      cases[i]=(struct token_thread){vi.request,pi.request,&expected,0};
      CHECK(pthread_create(&threads[i],NULL,token_thread,&cases[i])==0);
   }
   for (unsigned i=0;i<4;i++) {CHECK(pthread_join(threads[i],NULL)==0);CHECK(!cases[i].failed);}
   t70_shader_free_pipeline(&expected);
   vkd3d_shader_free_shader_code(&vs);vkd3d_shader_free_shader_code(&ps);
   printf("DDI threaded pipelines: 32\n");
}
static void token_pipelines_test(void)
{
   const char *vf="struct O { float4 c:COLOR; float4 p:SV_Position; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.c=p.yxwz; return o; }";
   token_pipeline_case(vf,"float4 main(float4 c:COLOR):SV_Target { return c; }","ps_4_0",false);
   token_pipeline_case(vf,"float4 main(nointerpolation float4 c:COLOR):SV_Target { return c; }","ps_4_0",false);
   token_pipeline_case(
      "struct O { uint4 c:COLOR; float4 p:SV_Position; }; O main(float4 p:POSITION) { O o; o.p=p; o.c=uint4(p); return o; }",
      "float4 main(nointerpolation uint4 c:COLOR):SV_Target { return float4(c); }","ps_4_0",false);
   token_pipeline_case(
      "struct O { int4 c:COLOR; float4 p:SV_Position; }; O main(float4 p:POSITION) { O o; o.p=p; o.c=int4(p); return o; }",
      "float4 main(nointerpolation int4 c:COLOR):SV_Target { return float4(c); }","ps_4_0",false);
   token_pipeline_case(vf,"float4 main(centroid noperspective float4 c:COLOR):SV_Target { return c; }","ps_4_0",false);
   token_pipeline_case(vf,"float4 main(noperspective float4 c:COLOR):SV_Target { return c; }","ps_4_0",true);
   token_pipeline_case("float4 main(float4 p:POSITION):SV_Position { return p; }",
      "float4 main(float4 p:SV_Position,bool front:SV_IsFrontFace):SV_Target { return front?p:0; }","ps_4_0",false);
   token_coordinates();token_clip_subset(2,SpvBuiltInClipDistance);token_clip_subset(3,SpvBuiltInCullDistance);
   invalid_tokens();token_threaded();
}
static void preload_invalid_case(const struct t70_shader_fb_preload_request *request,
                                 int expected)
{
   struct t70_shader out={.work_registers=0x5a17c0de};
   struct t70_shader before=out;
   char message[1]={0x7f};
   CHECK(t70_shader_compile_framebuffer_preload(request,&out,message,sizeof(message))==expected);
   CHECK(memcmp(&out,&before,sizeof(out))==0);
   CHECK(message[0]==0);
}
struct preload_thread_case {
   struct t70_shader_fb_preload_request request;
   const struct t70_shader *expected;
   int status;
};
static void *preload_thread(void *arg)
{
   struct preload_thread_case *test=arg;
   struct t70_shader out={0};
   test->status=t70_shader_compile_framebuffer_preload(&test->request,&out,NULL,0);
   if (test->status==T70_SHADER_OK &&
       (out.binary_size!=test->expected->binary_size ||
        memcmp(out.binary,test->expected->binary,out.binary_size)))
      test->status=T70_SHADER_TRANSLATION;
   t70_shader_free(&out);
   return NULL;
}
static void framebuffer_preload_tests(void)
{
   const uint32_t formats[]={T70_SHADER_FB_RGBA8_UNORM,
                             T70_SHADER_FB_BGRA8_UNORM,
                             T70_SHADER_FB_BGRX8_UNORM};
   struct t70_shader_fb_preload_request baseline={
      T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_FB_RGBA8_UNORM,1,1,0};
   struct t70_shader expected={0};
   char preload_message[256]={0};
   int preload_status=t70_shader_compile_framebuffer_preload(
      &baseline,&expected,preload_message,sizeof(preload_message));
   if (preload_status!=T70_SHADER_OK)
      fprintf(stderr,"framebuffer preload compilation failed (%d): %s\n",preload_status,preload_message);
   CHECK(preload_status==T70_SHADER_OK);
   CHECK(expected.binary && expected.binary_size && !(expected.binary_size&7));
   CHECK(expected.spirv==NULL && expected.spirv_size==0);
   CHECK(expected.stage==T70_SHADER_PIXEL && expected.arch==9);
   CHECK(expected.fau_reserved==T70_SHADER_FB_PRELOAD_FAU_WORDS);
   CHECK(expected.fau_count>=expected.fau_reserved && expected.fau_count<=128);
   /* The actual Mesa output must satisfy the UMD's fixed table-0 preload ABI. */
   CHECK(expected.work_registers && expected.work_registers<=64);
   CHECK(!expected.tls_size && !expected.wls_size && !expected.attributes_read &&
         !expected.attribute_count && !expected.varying_count &&
         !expected.varying_stride && !expected.cb_mask);
   CHECK(!(expected.flags&~(T70_SHADER_FLAG_FTZ32|T70_SHADER_FLAG_FTZ16)));
   CHECK(!expected.sampler_mask && !expected.texture_mask[0] &&
         !expected.texture_mask[1] && !expected.texture_mask[2] && !expected.texture_mask[3]);
   for (unsigned i=T70_SHADER_FB_PRELOAD_FAU_WORDS;i<expected.fau_count;i++)
      CHECK((expected.fau_const_mask[i/32]>>(i%32))&1u);
   for (unsigned i=0;i<sizeof(formats)/sizeof(formats[0]);i++) {
      struct t70_shader_fb_preload_request request=baseline;
      struct t70_shader out={0};
      request.format=formats[i];
      CHECK(t70_shader_compile_framebuffer_preload(&request,&out,NULL,0)==T70_SHADER_OK);
      CHECK(out.binary && out.binary_size && !(out.binary_size&7));
      CHECK(out.spirv==NULL && out.spirv_size==0);
      CHECK(out.stage==T70_SHADER_PIXEL && out.arch==9);
      CHECK(out.fau_reserved==T70_SHADER_FB_PRELOAD_FAU_WORDS);
      if (i==0)
         CHECK(out.binary_size==expected.binary_size &&
               memcmp(out.binary,expected.binary,out.binary_size)==0);
      t70_shader_free(&out);
   }

   struct t70_shader_fb_preload_request array_request=baseline;
   struct t70_shader array_shader={0};
   array_request.array_layers=2;
   CHECK(t70_shader_compile_framebuffer_preload(&array_request,&array_shader,NULL,0)==T70_SHADER_OK);
   CHECK(array_shader.binary && array_shader.binary_size && array_shader.fau_reserved==2);
   t70_shader_free(&array_shader);

   struct t70_shader_fb_preload_request bad=baseline;
   bad.abi++;preload_invalid_case(&bad,T70_SHADER_INVALID);bad=baseline;
   bad.reserved=1;preload_invalid_case(&bad,T70_SHADER_INVALID);bad=baseline;
   bad.array_layers=0;preload_invalid_case(&bad,T70_SHADER_INVALID);bad=baseline;
   bad.sample_count=2;preload_invalid_case(&bad,T70_SHADER_UNSUPPORTED);bad=baseline;
   bad.array_layers=T70_SHADER_FB_PRELOAD_MAX_LAYERS+1;
   preload_invalid_case(&bad,T70_SHADER_UNSUPPORTED);bad=baseline;
   bad.format=0xffffffffu;preload_invalid_case(&bad,T70_SHADER_UNSUPPORTED);bad=baseline;
   bad.gpu_id=0xdeadbeefu;preload_invalid_case(&bad,T70_SHADER_UNSUPPORTED);
   static uint8_t marker;
   struct t70_shader nonempty={.binary=&marker,.binary_size=1};
   struct t70_shader nonempty_before=nonempty;
   CHECK(t70_shader_compile_framebuffer_preload(&baseline,&nonempty,NULL,0)==T70_SHADER_INVALID);
   CHECK(memcmp(&nonempty,&nonempty_before,sizeof(nonempty))==0);

   struct preload_thread_case cases[2];
   pthread_t threads[2];
   for (unsigned i=0;i<2;i++) {
      cases[i]=(struct preload_thread_case){baseline,&expected,T70_SHADER_INVALID};
      CHECK(pthread_create(&threads[i],NULL,preload_thread,&cases[i])==0);
   }
   for (unsigned i=0;i<2;i++) {
      CHECK(pthread_join(threads[i],NULL)==0);
      CHECK(cases[i].status==T70_SHADER_OK);
   }
   t70_shader_free(&expected);
   printf("framebuffer preload variants: 4; concurrent compiles: 2\n");
}

int main(void)
{
   setvbuf(stdout,NULL,_IONBF,0);
   CHECK(t70_shader_abi_version()==T70_SHADER_ABI && t70_shader_cpp_abi()==T70_SHADER_ABI);
   framebuffer_preload_tests();
   invalid_inputs();
   token_pipelines_test();
   struct vkd3d_shader_code code=hlsl("float4 main() : SV_Target { return float4(0.2, 0.4, 0.6, 1.0); }","ps_4_0");
   struct t70_shader_request r={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_PIXEL,0,code.code,code.size};
   struct t70_shader s={0}; char message[1024];
   int status=t70_shader_compile(&r,&s,message,sizeof(message));
   if (status) fprintf(stderr,"compile: %d %s\n",status,message);
   CHECK(status==0);
   CHECK(s.binary && s.binary_size && !(s.binary_size&7));
   CHECK(s.arch==9 && s.stage==T70_SHADER_PIXEL && s.spirv_size>20);
   CHECK(s.fau_count<=128 && s.fau_reserved<=s.fau_count);
   printf("pixel: %zu DXBC -> %zu SPIR-V -> %zu Valhall bytes, %u registers\n",
          code.size,s.spirv_size,s.binary_size,s.work_registers);
   token_case(&code,T70_SHADER_PIXEL,&s);
   disassemble_valhall(stdout,s.binary,s.binary_size,true);
   t70_shader_free(&s);
   vkd3d_shader_free_shader_code(&code);
   compile_case("vertex attribute","float4 main(float4 p:POSITION):SV_Position { return p; }",
                "vs_4_0",T70_SHADER_VERTEX,0);
   compile_case("vertex matrix",
      "cbuffer Transform : register(b3) { float4x4 m; };"
      "float4 main(float4 p:POSITION):SV_Position { return mul(m,p); }",
      "vs_4_0",T70_SHADER_VERTEX,1u<<3);
   compile_case("pixel constants",
      "cbuffer Colour : register(b13) { float4 colour; };"
      "float4 main():SV_Target { return colour; }",
      "ps_4_0",T70_SHADER_PIXEL,1u<<13);
   compile_case("pixel varying",
      "float4 main(float4 p:COLOR):SV_Target { return saturate(p*p+float4(0.1,0.2,0.3,0)); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("pixel discard derivative",
      "float4 main(float2 p:TEXCOORD):SV_Target { clip(p.x-0.25); return float4(ddx(p),ddy(p)); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("pixel Sample",
      "Texture2D<float4> image:register(t7); SamplerState samp:register(s3);"
      "float4 main(float2 p:TEXCOORD):SV_Target { return image.Sample(samp,p); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("pixel Load",
      "Texture2D<float4> image:register(t127);"
      "float4 main(float4 p:SV_Position):SV_Target { return image.Load(int3(p.xy,0)); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("pixel SampleLevel",
      "Texture2D<float4> image:register(t0); SamplerState samp:register(s15);"
      "float4 main(float2 p:TEXCOORD):SV_Target { return image.SampleLevel(samp,p,1); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("pixel loop",
      "cbuffer Loop:register(b2) { int count; float value; };"
      "float4 main(float2 p:TEXCOORD):SV_Target { float v=0; for(int i=0;i<count;i++) { if(v>p.x) break; v+=value; } return v; }",
      "ps_4_0",T70_SHADER_PIXEL,1u<<2);
   compile_case("pixel dimensions",
      "Texture2D<float4> image:register(t8);"
      "float4 main():SV_Target { uint w,h,m; image.GetDimensions(1,w,h,m); return float4(w,h,m,1); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("pixel cube",
      "TextureCube<float4> image:register(t4); SamplerState samp:register(s0);"
      "float4 main(float3 p:TEXCOORD):SV_Target { return image.Sample(samp,p); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("vertex id",
      "float4 main(uint id:SV_VertexID,uint instance:SV_InstanceID):SV_Position { return float4(id,instance,0,1); }",
      "vs_4_0",T70_SHADER_VERTEX,0);
   compile_case("pixel depth",
      "struct Output { float4 color:SV_Target; float depth:SV_Depth; };"
      "Output main(float2 p:TEXCOORD) { Output o; o.color=1; o.depth=p.x; return o; }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("unsigned vertex attribute","float4 main(uint4 p:POSITION):SV_Position { return float4(p); }",
      "vs_4_0",T70_SHADER_VERTEX,0);
   compile_case("signed vertex attribute","float4 main(int4 p:POSITION):SV_Position { return float4(p); }",
      "vs_4_0",T70_SHADER_VERTEX,0);
   compile_case("clip distance","struct O { float4 p:SV_Position; float2 d:SV_ClipDistance; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.d=p.xy; return o; }","vs_4_0",T70_SHADER_VERTEX,0);
   compile_case("unsigned target","uint4 main():SV_Target { return uint4(1,2,3,4); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("signed target","int4 main():SV_Target { return int4(-1,-2,3,4); }",
      "ps_4_0",T70_SHADER_PIXEL,0);
   compile_case("coverage output","uint main():SV_Coverage { return 1; }","ps_4_1",T70_SHADER_PIXEL,0);
   pipeline_case("unused reordered outputs",
      "struct O { float4 p:SV_Position; float3 unused:EXTRA; float4 c:COLOR; float2 uv:TEXCOORD; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.unused=p.xyz*3; o.c=p.yxwz; o.uv=p.wz; return o; }",
      "struct I { float2 uv:TEXCOORD; float4 c:COLOR; };"
      "float4 main(I i):SV_Target { return i.c*float4(i.uv,2,1); }",
      "struct O { float4 p:SV_Position; float2 uv:TEXCOORD; float4 c:COLOR; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.uv=p.wz; o.c=p.yxwz; return o; }",NULL);
   pipeline_case("packed component permutation",
      "struct O { float4 p:SV_Position; float a:VALUE0; float2 b:VALUE1; float c:VALUE2; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.a=p.x; o.b=p.yz; o.c=p.w; return o; }",
      "struct I { float c:VALUE2; float2 b:VALUE1; float a:VALUE0; };"
      "float4 main(I i):SV_Target { return float4(i.a,i.b,i.c); }",NULL,NULL);
   pipeline_case("flat integer",
      "struct O { float4 p:SV_Position; nointerpolation uint2 i:VALUE; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.i=uint2(p.xy); return o; }",
      "float4 main(nointerpolation uint2 i:VALUE):SV_Target { return float4(i,0,1); }",NULL,NULL);
   pipeline_case("noperspective centroid",
      "struct O { float4 p:SV_Position; float2 uv:TEXCOORD; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.uv=p.yx; return o; }",
      "float4 main(centroid noperspective float2 uv:TEXCOORD):SV_Target { return float4(uv,0,1); }",
      NULL,"TEXCOORD");
   pipeline_case("noperspective pixel centre",
      "struct O { float4 p:SV_Position; float2 uv:TEXCOORD; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.uv=p.yx; return o; }",
      "float4 main(noperspective float2 uv:TEXCOORD):SV_Target { return float4(uv,0,1); }",
      NULL,"TEXCOORD");
   pipeline_case("noperspective sample",
      "struct O { float4 p:SV_Position; float2 uv:TEXCOORD; };"
      "O main(float4 p:POSITION) { O o; o.p=p; o.uv=p.yx; return o; }",
      "float4 main(noperspective float2 uv:TEXCOORD):SV_Target { return float4(uv,0,1); }",
      NULL,"TEXCOORD");
   pipeline_case("system values only",
      "float4 main(float4 p:POSITION):SV_Position { return p; }",
      "float4 main(float4 p:SV_Position,bool front:SV_IsFrontFace):SV_Target { return front?p:0; }",
      NULL,NULL);
   constant_link_values();
   invalid_pipeline();
   CHECK(register_moves>0);
   threaded_compile();threaded_pipeline();
   printf("DDI token shaders: %u; token pipelines: %u\n",token_cases,token_pipelines);
   printf("threaded pipelines: 32\n");
   printf("linked pipelines: %u; register moves: %u; component moves: %u\n",pipeline_cases,register_moves,component_moves);
   printf("shader cases: %u; threaded compilations: 32; checks: %u\n",cases+1,checks);
   return 0;
}
