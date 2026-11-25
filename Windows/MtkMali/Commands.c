/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Commands.h"
static uint32_t U32(const uint8_t *P) {
    return (uint32_t)P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);
}
static uint64_t U64(const uint8_t *P) { return U32(P)|((uint64_t)U32(P+4)<<32); }
static uint64_t Pair(const uint32_t *P) {return P[0]|((uint64_t)P[1]<<32);}
static void Put32(uint8_t *P,uint32_t V) {for(unsigned I=0;I<4;I++)P[I]=(uint8_t)(V>>(I*8));}
static void Put64(uint8_t *P,uint64_t V) {Put32(P,(uint32_t)V);Put32(P+4,(uint32_t)(V>>32));}
static int Range(uint64_t A,uint64_t N) {return A && A<MALI_JM_VA_LIMIT && N && N<=MALI_JM_VA_LIMIT-A;}
static int Overlap(uint64_t A,uint64_t N,uint64_t B,uint64_t M) {return A<B+M && B<A+N;}
static int CpuRange(const void *P,size_t N) {return P && N && (uintptr_t)P<=UINTPTR_MAX-N;}
static int CpuOverlap(const void *A,size_t N,const void *B,size_t M) {
    return Overlap((uintptr_t)A,N,(uintptr_t)B,M);
}
MALI_RESULT MaliCommandsBuildShader(const MALI_CAPS *C,const MALI_COMMAND_SHADER *S,
    void *Output,uint32_t Bytes) {
    if(!C || !S || S->Bytes!=sizeof(*S) || Bytes!=MALI_COMMAND_SHADER_BYTES ||
       !CpuRange(Output,Bytes) || CpuOverlap(Output,Bytes,C,sizeof(*C)) ||
       CpuOverlap(Output,Bytes,S,sizeof(*S)) || (S->Stage!=2 && S->Stage!=3) ||
       S->WorkRegisters>64 || S->WarpLimit>3 || S->FlushToZero>2 || S->HelperThreads>1 ||
       (S->Stage==2?S->WarpLimit!=0:S->HelperThreads!=0) ||
       (S->Preload&UINT64_C(0x0000ffffffffffff)) ||
       (S->CodeVa&7u) || !Range(S->CodeVa,8))return MaliInvalid;
    if((C->GpuId>>16)!=0x9093u)return MaliUnsupported;
    uint8_t *P=Output;
    for(uint32_t I=0;I<Bytes;I++)P[I]=0;
    Put32(P,8u|(S->Stage<<4)|(S->WarpLimit<<8)|(S->FlushToZero<<17)|
        (S->HelperThreads<<28)|((S->WorkRegisters<=32?2u:0u)<<30));
    Put32(P+4,(uint32_t)(S->Preload>>48));Put64(P+8,S->CodeVa);
    return MaliOk;
}
static uint32_t Width(const uint32_t *W) {
    uint32_t Type=W[10];uint64_t Value=Pair(&W[12]);
    if(Type==2)return Value?0:8;
    if(Type<4 || Type>7)return 0;
    unsigned Bits=8u<<(Type-4);
    if(Bits<64 && Value>>Bits)return 0;
    return Bits/8;
}
typedef struct {
    const MALI_COMMAND_RESOURCE *Resources;
    uint32_t ResourceCount;
} COMMAND_RANGES;
static int ValidRanges(const COMMAND_RANGES *W,uint64_t Va,uint32_t N,uint64_t SVa,uint32_t SN) {
    uint32_t Pages=0;
    for(uint32_t I=0;I<W->ResourceCount;I++) {
        const MALI_COMMAND_RESOURCE *R=&W->Resources[I];
        if(!Range(R->Va,R->Bytes) || (R->Va&4095u) || (R->Bytes&4095u) ||
           !(R->Access&MaliPteRead) || (R->Access&~(MaliPteRead|MaliPteWrite|MaliPteExecute)) ||
           ((R->Access&(MaliPteWrite|MaliPteExecute))==(MaliPteWrite|MaliPteExecute)) ||
           R->Bytes/4096u>MALI_COMMAND_MAX_RESOURCE_PAGES-Pages ||
           Overlap(R->Va,R->Bytes,Va,N) || Overlap(R->Va,R->Bytes,SVa,SN))return 0;
        Pages+=R->Bytes/4096u;
        for(uint32_t J=0;J<I;J++)
            if(Overlap(R->Va,R->Bytes,W->Resources[J].Va,W->Resources[J].Bytes))return 0;
    }
    return 1;
}
static int Contains(const COMMAND_RANGES *W,uint64_t Va,uint32_t Bytes,uint32_t Access) {
    if(!Range(Va,Bytes))return 0;
    for(uint32_t I=0;I<W->ResourceCount;I++) {
        const MALI_COMMAND_RESOURCE *R=&W->Resources[I];
        if(Va>=R->Va && Va-R->Va<R->Bytes && Bytes<=R->Bytes-(Va-R->Va) &&
           (R->Access&Access)==Access)return 1;
    }
    return 0;
}
static int ZeroWords(const uint32_t *P,uint32_t N) {
    for(uint32_t I=0;I<N;I++)if(P[I])return 0;
    return 1;
}
static int Environment(const COMMAND_RANGES *W,const uint32_t *E,int AttributeOffset,int Optional) {
    if(Optional && ZeroWords(E,16))return 1;
    if((!AttributeOffset && E[0]) || (E[1]&~255u) || !ZeroWords(E+2,6))return 0;
    uint64_t Res=Pair(E+8),Shader=Pair(E+10),Tls=Pair(E+12),Fau=Pair(E+14);
    uint32_t Count=(uint32_t)(Res&63u);
    if(Res && (!Count || (Count&3u) || !Contains(W,Res&~UINT64_C(63),Count*16u,MaliPteRead)))return 0;
    if((Shader&31u) || !Contains(W,Shader,32,MaliPteRead) ||
       (Tls&63u) || !Contains(W,Tls,32,MaliPteRead))return 0;
    if(E[1]?((Fau&7u) || !Contains(W,Fau,E[1]*8u,MaliPteRead)):Fau!=0)return 0;
    for(uint32_t I=0;I<W->ResourceCount;I++)if(W->Resources[I].Access&MaliPteExecute)return 1;
    return 0;
}
static int Compute(const COMMAND_RANGES *W,const uint32_t *H) {
    uint32_t X=(H[8]&1023u)+1u,Y=((H[8]>>10)&1023u)+1u,Z=((H[8]>>20)&1023u)+1u;
    if((H[8]&(1u<<30)) || (uint64_t)X*Y*Z>1024 || (H[9]&0xffff0000u) ||
       !(H[9]&16383u) || ((H[9]>>14)&3u)==3u || !H[10] || !H[11] || !H[12] ||
       H[10]>UINT32_MAX-H[13] || H[11]>UINT32_MAX-H[14] || H[12]>UINT32_MAX-H[15])return 0;
    return Environment(W,H+16,0,0);
}
/* Indirect descriptors and shader binaries remain process-owned, untrusted
   GPU data. Their containment is the private MMU, not these format checks. */
static int Draw(const COMMAND_RANGES *W,const uint32_t *D,int Malloc) {
    uint32_t Occlusion=(D[0]>>14)&3u,BlendCount=D[12]&15u;
    uint64_t Vertex=Pair(D+2),Zs=Pair(D+10),Blend=Pair(D+12)&~UINT64_C(15),Query=Pair(D+14);
    if((D[0]&~0x00ffefffu) || (D[1]&0xff000000u) || Occlusion==2 || D[5] || D[8] || D[9])return 0;
    if(Malloc) {
        /* The GPU writes its allocated vertex-array address/strides here. */
        if(Vertex!=1 || D[4])return 0;
    } else if((Vertex&62u) || !Contains(W,Vertex&~UINT64_C(63),16,MaliPteRead) || !D[4])return 0;
    if(Zs && ((Zs&31u) || !Contains(W,Zs,32,MaliPteRead)))return 0;
    if(BlendCount>8 || (BlendCount?!Contains(W,Blend,BlendCount*16u,MaliPteRead):Blend!=0))return 0;
    if(Occlusion?((Query&7u) || !Contains(W,Query,8,MaliPteRead|MaliPteWrite)):Query!=0)return 0;
    return Environment(W,D+16,0,1);
}
static MALI_RESULT Raster(const COMMAND_RANGES *W,const uint32_t *H,uint32_t Type) {
    uint32_t Mode=H[8]&255u,Index=(H[8]>>8)&7u,Point=(H[8]>>11)&3u;
    uint32_t MinX=H[26]&65535u,MinY=H[26]>>16,MaxX=H[27]&65535u,MaxY=H[27]>>16;
    uint64_t Tiler=Pair(H+14);
    if(!Mode || Mode==7 || Mode>14 || Index>3 || Point==1 || (H[8]&0xffc00000u) ||
       !H[11] || (Tiler&63u) || !Contains(W,Tiler,192,MaliPteRead|MaliPteWrite) ||
       !ZeroWords(H+16,10) || MinX>MaxX || MinY>MaxY)return MaliInvalid;
    if(H[8]&(1u<<21))return MaliUnsupported;
    if(Type==12) {
        uint64_t Dcd=Pair(H+12);
        if(Index || Point || (H[8]&((1u<<18)|(1u<<19))) || !ZeroWords(H+28,4) ||
           (Dcd&63u) || !Contains(W,Dcd,128,MaliPteRead))return MaliInvalid;
        return MaliOk;
    }
    uint64_t Indices=Pair(H+30);
    if(Index) {
        uint32_t Size=1u<<(Index-1u);
        if(H[11]>UINT32_MAX/Size || (Indices&(Size-1u)) ||
           !Contains(W,Indices,H[11]*Size,MaliPteRead))return MaliInvalid;
    } else if(Indices || (H[8]&(1u<<19)))return MaliInvalid;
    if(!H[12] || !Draw(W,H+32,Type==11))return MaliInvalid;
    if(Type==7) {
        if(!H[13])return MaliInvalid;
        if(Point) {
            uint32_t Size=Point==2?2u:4u;uint64_t Sizes=Pair(H+28);
            if(H[13]>UINT32_MAX/Size || (Sizes&(Size-1u)) ||
               !Contains(W,Sizes,H[13]*Size,MaliPteRead))return MaliInvalid;
        } else if(H[29])return MaliInvalid;
        return MaliOk;
    }
    uint32_t Packet=H[13]&65535u,Attribute=H[13]>>16;
    if(Attribute>65519u || Packet!=Attribute+16u || (!(H[8]&(1u<<18)) && Attribute) || H[29] ||
       !Environment(W,H+64,1,0))return MaliInvalid;
    if(H[8]&(1u<<18)) {
        if(!Environment(W,H+80,1,0))return MaliInvalid;
    } else if(!ZeroWords(H+80,16))return MaliInvalid;
    return MaliOk;
}
/* Arch-9 MFBD header/tag bounds. Indirect render targets and frame shaders
   remain untrusted and are confined by the submission's private GPU MMU. */
static MALI_RESULT Fragment(const COMMAND_RANGES *W,const uint32_t *H) {
    uint32_t MinX=H[8]&4095u,MinY=(H[8]>>16)&4095u;
    uint32_t MaxX=H[9]&4095u,MaxY=(H[9]>>16)&4095u,Order=(H[9]>>28)&7u;
    uint64_t Fbd=Pair(H+10);uint32_t Targets=(uint32_t)((Fbd>>2)&15u)+1u;
    if((H[8]&0xf000f000u) || (H[9]&0xf000u) || MinX>MaxX || MinY>MaxY ||
       (Order!=0 && Order!=1 && Order!=2 && Order!=5 && Order!=6) || H[15] ||
       !(Fbd&1u) || Targets>8 ||
       !Contains(W,Fbd&~UINT64_C(63),128u+64u*Targets+((Fbd&2u)?64u:0u),MaliPteRead))return MaliInvalid;
    /* The local arch-9 emitter uses frame shaders for partial updates. The
       older tile-enable map layout has no arch-9 execution proof here. */
    if(H[9]&(1u<<31))return MaliUnsupported;
    if(H[12] || H[13] || H[14])return MaliInvalid;
    return MaliOk;
}
MALI_RESULT MaliCommandsPrepareResources(const MALI_CAPS *C,
    const void *Input,uint64_t Va,uint32_t N,const void *Private,uint32_t PN,
    void *Shadow,uint64_t SVa,uint32_t SN,MALI_COMMAND_WORKSPACE *W,
    MALI_COMMAND_PIN_WRITE PinWrite,MALI_COMMAND_PIN_RESOURCE PinResource,void *Context) {
    if(!C || !W || !PinWrite || N<64 || N>MALI_COMMAND_MAX_DMA || PN<16 ||
       PN>MALI_COMMAND_MAX_PRIVATE || SN!=MALI_COMMAND_SHADOW_BYTES || !Range(Va,N) || !Range(SVa,SN) ||
       (Va&127u) || (SVa&127u) || Overlap(Va,N,SVa,SN) ||
       !CpuRange(Input,N) || !CpuRange(Private,PN) || !CpuRange(Shadow,SN) ||
       !CpuRange(W,sizeof(*W)) || ((uintptr_t)W&7u) ||
       CpuOverlap(Shadow,SN,Input,N) || CpuOverlap(Shadow,SN,Private,PN) ||
       CpuOverlap(W,sizeof(*W),Input,N) || CpuOverlap(W,sizeof(*W),Private,PN) ||
       CpuOverlap(W,sizeof(*W),Shadow,SN))return MaliInvalid;
    if((C->GpuId>>16)!=0x9093u)return MaliUnsupported;
    const uint8_t *P=Private,*Data=Input;
    uint32_t Steps=U32(P+8);
    uint32_t Version=U32(P),Resources=U32(P+12);
    if(U32(P+4)!=PN || !Steps || Steps>MALI_EXEC_MAX_STEPS ||
       (Version!=MALI_EXEC_PACKET_VERSION && Version!=MALI_COMMAND_RESOURCE_VERSION) ||
       Resources>MALI_COMMAND_MAX_RESOURCES || (Version==1 && Resources) ||
       PN!=MALI_COMMAND_PRIVATE_BYTES(Steps,Resources) ||
       (Version==MALI_COMMAND_RESOURCE_VERSION && !PinResource))return MaliInvalid;
    W->JobCount=0;W->WriteCount=0;W->ComputeCount=0;W->ResourceCount=Resources;W->ShadowUsed=0;
    for(uint32_t I=0;I<Resources;I++) {
        const uint8_t *Q=P+16+16*Steps+16*I;MALI_COMMAND_RESOURCE *R=&W->Resources[I];
        R->Va=U64(Q);R->Bytes=U32(Q+8);R->Access=U32(Q+12);
    }
    const COMMAND_RANGES View={W->Resources,Resources};
    if(!ValidRanges(&View,Va,N,SVa,SN))return MaliInvalid;
    for(uint32_t S=0;S<Steps;S++) {
        uint64_t Next=U64(P+16+16*S);uint32_t Slot=U32(P+24+16*S);
        if(Slot>=MALI_JM_MAX_SLOTS || !(C->JobSlots&(1u<<Slot)) || U32(P+28+16*S))return MaliInvalid;
        W->First[S]=W->JobCount;W->Count[S]=0;
        if(!Next)return MaliInvalid;
        while(Next) {
            if((Next&127u) || Next<Va || Next-Va>N-64u || W->JobCount==MALI_COMMAND_MAX_JOBS)return MaliInvalid;
            MALI_COMMAND_JOB *Job=&W->Jobs[W->JobCount];Job->Source=Next;
            uint32_t *H=Job->Words;
            for(unsigned J=0;J<8;J++)H[J]=U32(Data+(size_t)(Next-Va)+J*4u);
            uint32_t Type=(H[4]>>1)&127u,Index=H[4]>>16,Bytes;
            if(Type==2 || Type==3 || Type==9)Bytes=64;
            else if(Type==4 || Type==12)Bytes=128;
            else if(Type==7)Bytes=256;
            else if(Type==11)Bytes=384;
            else return MaliUnsupported;
            if(N<Bytes || Next-Va>N-Bytes)return MaliInvalid;
            for(uint32_t J=0;J<W->JobCount;J++)
                if(Overlap(Next,Bytes,W->Jobs[J].Source,W->Jobs[J].Bytes))return MaliInvalid;
            Job->Bytes=Bytes;Job->Offset=W->ShadowUsed;
            for(unsigned J=8;J<Bytes/4u;J++)H[J]=U32(Data+(size_t)(Next-Va)+J*4u);
            for(unsigned J=Bytes/4u;J<MALI_COMMAND_MAX_JOB_BYTES/4u;J++)H[J]=0;
            if(H[0] || H[1] || H[2] || H[3] || (H[4]&0xf601u) || !Index)return MaliInvalid;
            for(uint32_t J=W->First[S];J<W->JobCount;J++)if((W->Jobs[J].Words[4]>>16)==Index)return MaliInvalid;
            for(unsigned D=0;D<2;D++) {
                uint32_t Dep=(H[5]>>(D*16u))&65535u;int Found=!Dep;
                for(uint32_t J=W->First[S];J<W->JobCount;J++)if((W->Jobs[J].Words[4]>>16)==Dep)Found=1;
                if(!Found)return MaliInvalid;
            }
            if((Type==2 || Type==3) && (H[14] || H[15]))return MaliInvalid;
            if(!(C->JobFeatures[Slot]&(1u<<Type)))return MaliUnsupported;
            if(Type==2) {
                uint32_t Bytes=Width(H);uint64_t Address=Pair(&H[8]);
                if(!Bytes || H[11] || !Range(Address,Bytes) || (Address&(Bytes-1u)) ||
                   Overlap(Address,Bytes,Va,N) || Overlap(Address,Bytes,SVa,SN))return MaliInvalid;
                if(Version==MALI_COMMAND_RESOURCE_VERSION &&
                   !Contains(&View,Address,Bytes,MaliPteRead|MaliPteWrite))return MaliInvalid;
                ++W->WriteCount;
            } else if(Type==4) {
                if(Version!=MALI_COMMAND_RESOURCE_VERSION)return MaliUnsupported;
                if(!Compute(&View,H))return MaliInvalid;
                ++W->ComputeCount;
            } else if(Type==9) {
                if(Version!=MALI_COMMAND_RESOURCE_VERSION)return MaliUnsupported;
                MALI_RESULT R=Fragment(&View,H);if(R)return R;
            } else if(Type==7 || Type==11 || Type==12) {
                if(Version!=MALI_COMMAND_RESOURCE_VERSION)return MaliUnsupported;
                MALI_RESULT R=Raster(&View,H,Type);if(R)return R;
            } else {
                if((H[8]&~0x03030007u) || (H[9]&~3u) || H[10] || H[11] || H[12] || H[13])return MaliInvalid;
            }
            W->ShadowUsed+=Bytes<MALI_COMMAND_STRIDE?MALI_COMMAND_STRIDE:Bytes;
            Next=Pair(&H[6]);++W->JobCount;++W->Count[S];
        }
        W->Packet.Steps[S].Chain=SVa+W->Jobs[W->First[S]].Offset;
        W->Packet.Steps[S].Slot=Slot;W->Packet.Steps[S].Reserved=0;
    }
    /* Freeze the entire graph and resource list before invoking residency. */
    for(uint32_t I=0;I<Resources;I++) {
        MALI_COMMAND_RESOURCE *R=&W->Resources[I];
        MALI_RESULT V=PinResource(Context,R->Va,R->Bytes,R->Access);if(V)return V;
    }
    for(uint32_t J=0;J<W->JobCount;J++) {
        uint32_t *H=W->Jobs[J].Words;
        if(((H[4]>>1)&127u)==2) {
            MALI_RESULT R=PinWrite(Context,Pair(&H[8]),Width(H));if(R)return R;
        }
    }
    uint8_t *Out=Shadow;for(uint32_t I=0;I<SN;I++)Out[I]=0;
    for(uint32_t S=0;S<Steps;S++)for(uint32_t J=0;J<W->Count[S];J++) {
        uint32_t I=W->First[S]+J,*H=W->Jobs[I].Words,Type=(H[4]>>1)&127u;
        uint8_t *Record=Out+W->Jobs[I].Offset;
        /* Sequential dependencies prevent untrusted scheduling flags/indices
           from bypassing earlier writes or cache operations. */
        Put32(Record+16,(Type<<1)|(1u<<8)|(1u<<11)|((J+1u)<<16));
        Put32(Record+20,J);
        Put64(Record+24,J+1<W->Count[S]?SVa+W->Jobs[I+1u].Offset:0);
        if(Type==2)for(unsigned K=8;K<14;K++)Put32(Record+K*4u,H[K]);
        else if(Type!=3)for(unsigned K=8;K<W->Jobs[I].Bytes/4u;K++)Put32(Record+K*4u,H[K]);
        else {Put32(Record+32,0x03030007u);Put32(Record+36,3);}
    }
    W->Packet.Version=MALI_EXEC_PACKET_VERSION;W->Packet.StepCount=Steps;
    W->Packet.Bytes=MALI_EXEC_PACKET_BYTES(Steps);W->Packet.Flags=0;
    for(uint32_t S=Steps;S<MALI_EXEC_MAX_STEPS;S++) {
        W->Packet.Steps[S].Chain=0;W->Packet.Steps[S].Slot=0;W->Packet.Steps[S].Reserved=0;
    }
    return MaliOk;
}

MALI_RESULT MaliCommandsPrepare(const MALI_CAPS *C,
    const void *Input,uint64_t Va,uint32_t N,const void *Private,uint32_t PN,
    void *Shadow,uint64_t SVa,uint32_t SN,MALI_COMMAND_WORKSPACE *W,
    MALI_COMMAND_PIN_WRITE PinWrite,void *Context) {
    return MaliCommandsPrepareResources(C,Input,Va,N,Private,PN,Shadow,SVa,SN,W,PinWrite,0,Context);
}


/* Round an unsigned significand to nearest, ties to even. Shift is 1..31. */
static uint32_t RoundEven(uint32_t Value,uint32_t Shift) {
    uint32_t Whole=Value>>Shift,Mask=(1u<<Shift)-1u,Rem=Value&Mask,Half=1u<<(Shift-1u);
    return Whole+(Rem>Half || (Rem==Half && (Whole&1u)));
}
static uint32_t ClearUnorm8(uint32_t Bits) {
    /* UNORM clamps negative values and NaNs to zero, positive infinity to one.
       Values below 2^-9 cannot round to one after multiplication by 255. */
    if((Bits&0x80000000u) || (Bits&0x7fffffffu)>0x7f800000u)return 0;
    if(Bits>=0x3f800000u)return 255;
    uint32_t Exp=Bits>>23;
    if(Exp<118u)return 0;
    uint32_t Product=((Bits&0x7fffffu)|0x800000u)*255u,Shift=0,Scan=Product>>24;
    for(;Scan;Scan>>=1)++Shift;
    /* Mesa multiplies in binary32 before integer conversion. Rounding just
       the exact rational product differs next to some half-way inputs. */
    if(Shift)Product=RoundEven(Product,Shift);
    return RoundEven(Product,150u-Exp-Shift);
}
MALI_RESULT MaliCommandsBuildClear(const MALI_CAPS *C,const MALI_COMMAND_CLEAR *D,
    void *Command,uint64_t CVa,uint32_t CN,void *Descriptors,uint64_t DVa,uint32_t DN,
    void *Private,uint32_t PN) {
    if(!CpuRange(C,sizeof(*C)) || !CpuRange(D,sizeof(*D)) ||
       ((uintptr_t)C&7u) || ((uintptr_t)D&7u) ||
       CN!=MALI_COMMAND_CLEAR_JOB_BYTES || DN!=MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES ||
       PN!=MALI_COMMAND_CLEAR_PRIVATE_BYTES || !CpuRange(Command,CN) ||
       !CpuRange(Descriptors,DN) || !CpuRange(Private,PN) ||
       CpuOverlap(Command,CN,Descriptors,DN) || CpuOverlap(Command,CN,Private,PN) ||
       CpuOverlap(Descriptors,DN,Private,PN) ||
       CpuOverlap(C,sizeof(*C),Command,CN) || CpuOverlap(C,sizeof(*C),Descriptors,DN) ||
       CpuOverlap(C,sizeof(*C),Private,PN) ||
       CpuOverlap(D,sizeof(*D),Command,CN) || CpuOverlap(D,sizeof(*D),Descriptors,DN) ||
       CpuOverlap(D,sizeof(*D),Private,PN))return MaliInvalid;
    if(D->Bytes!=sizeof(*D) || D->Reserved || !D->Width || D->Width>16384u ||
       !D->Height || D->Height>16384u || D->Pitch<D->Width*4u || (D->Pitch&63u) ||
       D->Slot>=MALI_JM_MAX_SLOTS)return MaliInvalid;
    if((C->GpuId>>16)!=0x9093u || !C->Tilers ||
       !(C->JobSlots&(1u<<D->Slot)) || !(C->JobFeatures[D->Slot]&(1u<<9)))return MaliUnsupported;
    if(D->Format!=MaliClearBgra8 && D->Format!=MaliClearBgrx8 &&
       D->Format!=MaliClearRgba8 && D->Format!=MaliClearRgbx8)return MaliUnsupported;
    uint64_t Used=(uint64_t)D->Pitch*D->Height,Required=(Used+4095u)&~UINT64_C(4095);
    const uint32_t Heap=MALI_COMMAND_CLEAR_HEAP_BYTES;
    if(Required!=D->TargetBytes || Required>UINT64_C(536870912)-Heap-DN ||
       (CVa&127u) || (DVa&4095u) || (D->TargetVa&4095u) || (D->HeapVa&4095u) ||
       !Range(CVa,CN) || !Range(DVa,DN) || !Range(D->TargetVa,D->TargetBytes) || !Range(D->HeapVa,Heap) ||
       Overlap(CVa,CN,DVa,DN) || Overlap(CVa,CN,D->TargetVa,D->TargetBytes) ||
       Overlap(CVa,CN,D->HeapVa,Heap) || Overlap(DVa,DN,D->TargetVa,D->TargetBytes) ||
       Overlap(DVa,DN,D->HeapVa,Heap) || Overlap(D->TargetVa,D->TargetBytes,D->HeapVa,Heap))return MaliInvalid;
    uint32_t Color=0;
    for(unsigned I=0;I<4;I++) {
        uint32_t Channel=(I==3 && (D->Format==MaliClearBgrx8 || D->Format==MaliClearRgbx8))?
            255u:ClearUnorm8(D->Color[I]);
        Color|=Channel<<(I*8u);
    }
    /* One enabled hierarchy covering the whole FB: Mesa's conservative
       max_levels=1 selection (0x80, shifted when the grid needs more bits). */
    uint32_t Largest=D->Width>D->Height?D->Width:D->Height,Levels=0;
    for(uint32_t N=(Largest+15u)/16u;N;N>>=1)++Levels;
    uint32_t Hierarchy=0x80u<<(Levels>8u?Levels-8u:0u);
    uint32_t Swizzle=D->Format==MaliClearBgra8 || D->Format==MaliClearBgrx8?0x60au:0x688u;
    uint8_t *J=Command,*F=Descriptors,*P=Private;
    for(unsigned I=0;I<CN;I++)J[I]=0;
    for(unsigned I=0;I<DN;I++)F[I]=0;
    for(unsigned I=0;I<PN;I++)P[I]=0;
    Put32(J+16,(1u<<16)|(9u<<1));
    Put32(J+36,((D->Width-1u)>>4)|(((D->Height-1u)>>4)<<16));Put64(J+40,DVa|1u);
    /* MFBD, one RT, no ZS/CRC/frame shaders, a 16x16 RGBA8 tile (1024 B). */
    Put64(F+16,DVa+512u);Put32(F+32,(D->Width-1u)|((D->Height-1u)<<16));
    Put32(F+40,(D->Width-1u)|((D->Height-1u)<<16));
    Put32(F+44,(2u<<6)|(8u<<9)|(1u<<14)|(1u<<24));
    Put32(F+48,1u<<16);Put64(F+56,DVa+192u);
    /* Single-sample linear writeback; clean tiles must also be stored.
       Tile-buffer clear is always RGBA; writeback swizzle controls RAM order. */
    Put32(F+128,1u<<26);
    Put32(F+132,1u|(19u<<3)|(2u<<8)|(1u<<15)|(Swizzle<<16)|(1u<<31));
    Put64(F+160,D->TargetVa);Put32(F+168,D->Pitch);Put32(F+172,(uint32_t)Used);
    for(unsigned I=0;I<4;I++)Put32(F+176+I*4u,Color);
    /* Fresh tiler context (192 B), heap descriptor (32 B), single-sample
       positions (256 B, origin at index 32). No polygon/tiler job is emitted. */
    Put32(F+200,Hierarchy|(1u<<18));Put32(F+204,(D->Width-1u)|((D->Height-1u)<<16));
    Put64(F+216,DVa+384u);
    Put32(F+384,9u|(2u<<4));Put32(F+388,Heap);
    Put64(F+392,D->HeapVa);Put64(F+400,D->HeapVa);Put64(F+408,D->HeapVa+Heap);
    Put32(F+512,0x00800080u);Put32(F+640,0x00800080u);
    Put32(P,2);Put32(P+4,PN);Put32(P+8,1);Put32(P+12,3);
    Put64(P+16,CVa);Put32(P+24,D->Slot);
    const uint64_t Addresses[]={DVa,D->TargetVa,D->HeapVa};
    const uint32_t Lengths[]={DN,D->TargetBytes,Heap};
    for(unsigned I=0;I<3;I++) {
        Put64(P+32+16u*I,Addresses[I]);Put32(P+40+16u*I,Lengths[I]);
        Put32(P+44+16u*I,MaliPteRead|MaliPteWrite);
    }
    return MaliOk;
}

MALI_RESULT MaliCommandsBuildPreload(const MALI_CAPS *C,const MALI_COMMAND_PRELOAD *D,
    void *Descriptors,uint64_t DVa,uint32_t DN) {
    if(!CpuRange(C,sizeof(*C)) || !CpuRange(D,sizeof(*D)) ||
       ((uintptr_t)C&7u) || ((uintptr_t)D&7u) ||
       DN!=MALI_COMMAND_PRELOAD_DESCRIPTOR_BYTES || !CpuRange(Descriptors,DN) ||
       CpuOverlap(C,sizeof(*C),Descriptors,DN) || CpuOverlap(D,sizeof(*D),Descriptors,DN))return MaliInvalid;
    if((C->GpuId>>16)!=0x9093u || !C->Tilers)return MaliUnsupported;
    if(D->Bytes!=sizeof(*D) || D->Reserved[0] || D->Reserved[1] ||
       !D->Width || D->Width>16384u || !D->Height || D->Height>16384u ||
       D->Pitch<D->Width*4u || (D->Pitch&63u) ||
       !D->FauCount || D->FauCount>255u || D->ThreadStorageBytes<64u ||
       (D->ThreadStorageBytes&63u))return MaliInvalid;
    if(D->Format!=MaliClearBgra8 && D->Format!=MaliClearBgrx8 &&
       D->Format!=MaliClearRgba8)return MaliUnsupported;
    uint64_t Used=(uint64_t)D->Pitch*D->Height;
    uint64_t Required=(Used+4095u)&~UINT64_C(4095);
    uint64_t FauBytes=(uint64_t)D->FauCount*8u;
    if(Used>UINT32_MAX || Required!=D->TargetBytes ||
       Required>UINT64_C(536870912)-MALI_COMMAND_CLEAR_HEAP_BYTES-DN ||
       (DVa&4095u) || (D->TargetVa&4095u) || (D->HeapVa&4095u) ||
       (D->ShaderProgramVa&31u) || (D->FauVa&7u) || (D->ThreadStorageVa&63u) ||
       !Range(DVa,DN) || !Range(D->TargetVa,D->TargetBytes) ||
       !Range(D->HeapVa,MALI_COMMAND_CLEAR_HEAP_BYTES) || !Range(D->ShaderProgramVa,32u) ||
       !Range(D->FauVa,FauBytes) || !Range(D->ThreadStorageVa,D->ThreadStorageBytes))return MaliInvalid;
    const uint64_t Va[]={DVa,D->TargetVa,D->HeapVa,D->ShaderProgramVa,D->FauVa,D->ThreadStorageVa};
    const uint64_t Size[]={DN,D->TargetBytes,MALI_COMMAND_CLEAR_HEAP_BYTES,32u,FauBytes,D->ThreadStorageBytes};
    for(unsigned I=0;I<sizeof(Va)/sizeof(Va[0]);I++)
        for(unsigned J=0;J<I;J++)if(Overlap(Va[I],Size[I],Va[J],Size[J]))return MaliInvalid;
    uint32_t Used32=(uint32_t)Used;
    uint32_t Swizzle=(D->Format==MaliClearBgra8 || D->Format==MaliClearBgrx8)?0x60au:0x688u;
    uint32_t Order=D->Format==MaliClearBgra8?4u:D->Format==MaliClearBgrx8?20u:0u;
    uint32_t Largest=D->Width>D->Height?D->Width:D->Height,Levels=0;
    for(uint32_t N=(Largest+15u)/16u;N;N>>=1)++Levels;
    uint32_t Hierarchy=0x80u<<(Levels>8u?Levels-8u:0u);
    uint8_t *F=Descriptors;
    for(uint32_t I=0;I<DN;I++)F[I]=0;
    /* Same one-target MFBD, tiler, heap and sample layout as BuildClear. */
    Put32(F,1u); /* pre_frame_0 = ALWAYS */
    Put64(F+16,DVa+512u);Put64(F+24,DVa+MALI_COMMAND_PRELOAD_DCD0_OFFSET);
    Put32(F+32,(D->Width-1u)|((D->Height-1u)<<16));
    Put32(F+40,(D->Width-1u)|((D->Height-1u)<<16));
    Put32(F+44,(2u<<6)|(8u<<9)|(1u<<14)|(1u<<24));
    Put32(F+48,1u<<16);Put64(F+56,DVa+192u);
    Put32(F+128,1u<<26);
    Put32(F+132,1u|(19u<<3)|(2u<<8)|(1u<<15)|(Swizzle<<16)|(1u<<31));
    Put64(F+160,D->TargetVa);Put32(F+168,D->Pitch);Put32(F+172,Used32);
    Put32(F+200,Hierarchy|(1u<<18));Put32(F+204,(D->Width-1u)|((D->Height-1u)<<16));
    Put64(F+216,DVa+384u);
    Put32(F+384,9u|(2u<<4));Put32(F+388,MALI_COMMAND_CLEAR_HEAP_BYTES);
    Put64(F+392,D->HeapVa);Put64(F+400,D->HeapVa);
    Put64(F+408,D->HeapVa+MALI_COMMAND_CLEAR_HEAP_BYTES);
    Put32(F+512,0x00800080u);Put32(F+640,0x00800080u);

    uint8_t *Dcd=F+MALI_COMMAND_PRELOAD_DCD0_OFFSET;
    Put32(Dcd,0x203u); /* color preload: clean write and forward-pixel kill */
    Put32(Dcd+4,0x0001ffffu); /* sample mask and render-target 0 */
    Put32(Dcd+28,0x3f800000u); /* maximum Z */
    Put64(Dcd+40,DVa+MALI_COMMAND_PRELOAD_DEPTH_STENCIL_OFFSET);
    Put64(Dcd+48,(DVa+MALI_COMMAND_PRELOAD_BLEND_OFFSET)|1u);
    Put32(Dcd+68,D->FauCount);
    Put64(Dcd+96,(DVa+MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET)|4u);
    Put64(Dcd+104,D->ShaderProgramVa);
    Put64(Dcd+112,D->ThreadStorageVa);
    Put64(Dcd+120,D->FauVa);

    /* Mesa v9 uses sampler 0 and texture 1 in resource-table entry 0. */
    uint8_t *Sampler=F+MALI_COMMAND_PRELOAD_SAMPLER_OFFSET;
    Put32(Sampler,0x18299901u); /* nearest, unnormalized, clamp-to-edge */
    uint8_t *Texture=F+MALI_COMMAND_PRELOAD_TEXTURE_OFFSET;
    Put32(Texture,2u|(2u<<4)|(187u<<22)|(Order<<10)); /* 2D RGBA8_UNORM */
    Put32(Texture+4,(D->Width-1u)|((D->Height-1u)<<16));
    Put32(Texture+8,0x688u); /* identity channel swizzle */
    Put64(Texture+16,DVa+MALI_COMMAND_PRELOAD_PLANE_OFFSET);

    uint8_t *Plane=F+MALI_COMMAND_PRELOAD_PLANE_OFFSET;
    Put32(Plane,10u|(1u<<4)|(2u<<8)|(2u<<24)); /* GENERIC, LINEAR, RAW32 */
    Put32(Plane+4,Used32);Put64(Plane+8,D->TargetVa);Put32(Plane+16,D->Pitch);

    uint8_t *Resources=F+MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET;
    Put64(Resources,DVa+MALI_COMMAND_PRELOAD_SAMPLER_OFFSET);
    Put32(Resources+4,(uint32_t)((DVa+MALI_COMMAND_PRELOAD_SAMPLER_OFFSET)>>32)|(1u<<24));
    Put64(Resources+8,64u);

    uint8_t *Blend=F+MALI_COMMAND_PRELOAD_BLEND_OFFSET;
    Put32(Blend,0x00000a00u);Put32(Blend+4,0xf0122122u);
    Put32(Blend+8,0x19u);Put32(Blend+12,237u<<12); /* OPAQUE, RGBA8_TB */

    uint8_t *Depth=F+MALI_COMMAND_PRELOAD_DEPTH_STENCIL_OFFSET;
    Put32(Depth,0x24f24f7u);Put32(Depth+4,UINT32_MAX);
    Put32(Depth+16,0xe4800000u); /* ALWAYS; cull disabled; default [0,1] clamp */
    return MaliOk;
}

static void PackEnvironment(uint8_t *P,const MALI_COMMAND_ENVIRONMENT *E) {
    Put32(P,E->AttributeOffset);Put32(P+4,E->FauCount);
    Put64(P+32,E->Resources);Put64(P+40,E->Shader);
    Put64(P+48,E->ThreadStorage);Put64(P+56,E->Fau);
}
MALI_RESULT MaliCommandsBuildRaster(const MALI_CAPS *C,const MALI_COMMAND_RASTER *D,
    const MALI_COMMAND_RESOURCE *Resources,uint32_t Count,
    void *Command,uint64_t CVa,uint32_t CN,void *Private,uint32_t PN) {
    if(!Count || Count>MALI_COMMAND_MAX_RESOURCES ||
       !CpuRange(C,sizeof(*C)) || !CpuRange(D,sizeof(*D)) ||
       !CpuRange(Resources,(size_t)Count*sizeof(*Resources)) ||
       ((uintptr_t)C&7u) || ((uintptr_t)D&7u) || ((uintptr_t)Resources&7u) ||
       CN!=MALI_COMMAND_RASTER_JOB_BYTES || PN!=MALI_COMMAND_RASTER_PRIVATE_BYTES(Count) ||
       !CpuRange(Command,CN) || !CpuRange(Private,PN) ||
       CpuOverlap(Command,CN,Private,PN) ||
       CpuOverlap(C,sizeof(*C),Command,CN) || CpuOverlap(C,sizeof(*C),Private,PN) ||
       CpuOverlap(D,sizeof(*D),Command,CN) || CpuOverlap(D,sizeof(*D),Private,PN) ||
       CpuOverlap(Resources,(size_t)Count*sizeof(*Resources),Command,CN) ||
       CpuOverlap(Resources,(size_t)Count*sizeof(*Resources),Private,PN) ||
       D->Bytes!=sizeof(*D) || D->VertexSlot>=MALI_JM_MAX_SLOTS ||
       D->FragmentSlot>=MALI_JM_MAX_SLOTS || (CVa&127u) || !Range(CVa,CN) ||
       D->VaryingStride>65519u || D->BlendCount>8u || (D->Blend&15u))return MaliInvalid;
    if((C->GpuId>>16)!=0x9093u || !C->Tilers ||
       !(C->JobSlots&(1u<<D->VertexSlot)) || !(C->JobSlots&(1u<<D->FragmentSlot)) ||
       !(C->JobFeatures[D->VertexSlot]&(1u<<11)) ||
       !(C->JobFeatures[D->FragmentSlot]&(1u<<9)))return MaliUnsupported;
    const COMMAND_RANGES View={Resources,Count};
    if(!ValidRanges(&View,CVa,CN,0,0))return MaliInvalid;
    uint32_t Words[MALI_COMMAND_RASTER_JOB_BYTES/4u]={0};
    uint8_t *J=(uint8_t *)Words,*F=J+384;
    Put32(J+16,(1u<<16)|(11u<<1));Put32(J+32,D->Primitive);
    Put32(J+36,D->BaseVertex);Put32(J+40,D->BaseInstance);Put32(J+44,D->IndexCount);
    Put32(J+48,D->InstanceCount);Put32(J+52,(D->VaryingStride<<16)|(D->VaryingStride+16u));
    Put64(J+56,D->Tiler);Put32(J+104,D->Scissor[0]);Put32(J+108,D->Scissor[1]);
    Put32(J+112,D->PrimitiveSize);Put64(J+120,D->Indices);
    Put32(J+128,D->Flags[0]);Put32(J+132,D->Flags[1]);Put64(J+136,1);
    Put32(J+152,D->MinZ);Put32(J+156,D->MaxZ);Put64(J+168,D->DepthStencil);
    Put64(J+176,D->Blend|D->BlendCount);Put64(J+184,D->Occlusion);
    PackEnvironment(J+192,&D->Fragment);PackEnvironment(J+256,&D->Position);
    PackEnvironment(J+320,&D->Varying);
    Put32(F+16,(1u<<16)|(9u<<1));Put32(F+32,D->Tiles[0]);Put32(F+36,D->Tiles[1]);
    Put64(F+40,D->Framebuffer);
    MALI_RESULT R=Raster(&View,Words,11);if(R)return R;
    R=Fragment(&View,Words+96);if(R)return R;
    uint8_t *P=Private,*Out=Command;
    for(uint32_t I=0;I<CN;I++)Out[I]=J[I];
    for(uint32_t I=0;I<PN;I++)P[I]=0;
    Put32(P,2);Put32(P+4,PN);Put32(P+8,2);Put32(P+12,Count);
    Put64(P+16,CVa);Put32(P+24,D->VertexSlot);
    Put64(P+32,CVa+384);Put32(P+40,D->FragmentSlot);
    for(uint32_t I=0;I<Count;I++) {
        Put64(P+48+16u*I,Resources[I].Va);Put32(P+56+16u*I,Resources[I].Bytes);
        Put32(P+60+16u*I,Resources[I].Access);
    }
    return MaliOk;
}
