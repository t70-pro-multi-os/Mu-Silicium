/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Commands.h"
#include <string.h>
#ifdef MALI_COMMAND_TEST_NATIVE
#include <stdio.h>
#endif
static unsigned FirstFailure;
unsigned MtkMaliCommandsAssertionCount;
#define CHECK(X) do { ++MtkMaliCommandsAssertionCount; if(!(X) && !FirstFailure)FirstFailure=__LINE__; } while(0)
static uint8_t Input[MALI_COMMAND_MAX_DMA],Shadow[MALI_COMMAND_SHADOW_BYTES];
static MALI_COMMAND_WORKSPACE Work;
static MALI_EXEC_PACKET Packet;
static MALI_CAPS Caps;
static unsigned Pins,DenyAt,Mutate;
static void P32(uint8_t *P,uint32_t V) {for(unsigned I=0;I<4;I++)P[I]=(uint8_t)(V>>(I*8));}
static void P64(uint8_t *P,uint64_t V) {P32(P,(uint32_t)V);P32(P+4,(uint32_t)(V>>32));}
static uint32_t R32(const uint8_t *P) {return P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);}
static uint64_t R64(const uint8_t *P) {return R32(P)|((uint64_t)R32(P+4)<<32);}
static void Job(unsigned Offset,unsigned Index,uint64_t Next,unsigned Type,uint64_t Target,uint64_t Value) {
    uint8_t *P=Input+Offset;memset(P,0,128);P32(P+16,(Index<<16)|(2u<<1));P64(P+24,Next);
    P64(P+32,Target);P32(P+40,Type);P64(P+48,Value);
}
static void Init(void) {
    memset(Input,0,4096);memset(Shadow,0x5a,sizeof(Shadow));
    memset(&Packet,0,sizeof(Packet));memset(&Caps,0,sizeof(Caps));
    Packet.Version=1;Packet.Bytes=MALI_EXEC_PACKET_BYTES(1);Packet.StepCount=1;
    Packet.Steps[0].Chain=0x80000000;Packet.Steps[0].Slot=1;
    Caps.GpuId=0x90930002;Caps.JobSlots=7;Caps.JobFeatures[1]=(1u<<2)|(1u<<3);
    Pins=DenyAt=Mutate=0;Job(0,77,0,7,0xa0000000,UINT64_C(0x123456789abcdef0));
}
static MALI_RESULT Pin(void *C,uint64_t A,uint32_t N) {
    CHECK(C==&Work && N && N<=8);
    if(A<0xa0000000 || A+N>0xa0001000)return MaliInvalid;
    ++Pins;
    if(Mutate) {memset(Input,0xff,sizeof(Input));memset(&Packet,0xff,sizeof(Packet));}
    return Pins==DenyAt?MaliFault:MaliOk;
}
static MALI_RESULT Run(void) {
    return MaliCommandsPrepare(&Caps,Input,0x80000000,4096,&Packet,Packet.Bytes,
        Shadow,0x90000000,sizeof(Shadow),&Work,Pin,&Work);
}
static int Untouched(void) {
    for(unsigned I=0;I<sizeof(Shadow);I+=8)if(R64(Shadow+I)!=UINT64_C(0x5a5a5a5a5a5a5a5a))return 0;
    return 1;
}
static void Mixed(void) {
    Init();Job(0,70,0x80000080,7,0xa0000000,UINT64_C(0x123456789abcdef0));
    Job(128,2,0x80000100,4,0xa0000008,0xff);P32(Input+128+20,70);
    Job(256,600,0x80000180,2,0xa0000010,0);P32(Input+256+20,2);
    memset(Input+384,0,128);P32(Input+384+16,(51u<<16)|(3u<<1));P32(Input+384+20,600);
    P32(Input+384+32,2);P32(Input+384+36,2); // canonicalized to clean+invalidate
    CHECK(Run()==MaliOk && Pins==3 && Work.JobCount==4 && Work.WriteCount==3);
    CHECK(Work.Packet.Steps[0].Chain==0x90000000 && Work.Packet.Steps[0].Slot==1);
    for(unsigned I=0;I<4;I++) {
        uint8_t *S=Shadow+128*I;CHECK(!R64(S) && !R64(S+8));
        CHECK((R32(S+16)>>16)==I+1 && R32(S+20)==I);
        CHECK(R64(S+24)==(I==3?0:0x90000000+128*(I+1)));
        CHECK((R32(S+16)&0x900)==0x900);
        for(unsigned J=56;J<128;J++)CHECK(!S[J]);
    }
    CHECK(R64(Shadow+48)==UINT64_C(0x123456789abcdef0));
    CHECK(R32(Shadow+384+32)==0x03030007 && R32(Shadow+384+36)==3);
    Init();Mutate=1;CHECK(Run()==MaliOk && Pins==1);
    CHECK(R64(Shadow+48)==UINT64_C(0x123456789abcdef0) && Work.Packet.Steps[0].Slot==1);
}
static void Rejects(void) {
    for(unsigned Case=0;Case<34;Case++) {
        Init();
        switch(Case) {
            case 0:Packet.Version=2;break;
            case 1:Packet.Flags=1;break;
            case 2:Packet.StepCount=0;break;
            case 3:Packet.Bytes++;break;
            case 4:Packet.Steps[0].Reserved=1;break;
            case 5:Packet.Steps[0].Chain=0;break;
            case 6:Packet.Steps[0].Chain+=64;break;
            case 7:Packet.Steps[0].Chain+=4096;break;
            case 8:Packet.Steps[0].Slot=3;break;
            case 9:Caps.JobSlots=0;break;
            case 10:Caps.JobFeatures[1]=0;break;
            case 11:Caps.GpuId=0x70930000;break;
            case 12:P32(Input,1);break;
            case 13:P32(Input+4,1);break;
            case 14:P64(Input+8,0xdead0000);break;
            case 15:P32(Input+16,4);break;
            case 16:P32(Input+16,(1u<<16)|4|1);break;
            case 17:P32(Input+16,(1u<<16)|4|(1u<<14));break;
            case 18:P32(Input+20,1);break;
            case 19:P64(Input+24,0x80000000);break;
            case 20:P64(Input+24,0x80001000);break;
            case 21:P64(Input+24,0x80000040);break;
            case 22:P64(Input+32,0x80000000);break;
            case 23:P64(Input+32,0x90000000);break;
            case 24:P64(Input+32,MALI_JM_VA_LIMIT-4);break;
            case 25:P64(Input+32,0xa0000001);break;
            case 26:P32(Input+40,3);break;
            case 27:P32(Input+40,1);break;
            case 28:P32(Input+40,4);P64(Input+48,256);break;
            case 29:P32(Input+40,2);P64(Input+48,1);break;
            case 30:P32(Input+44,1);break;
            case 31:P32(Input+56,1);break;
            case 32:P32(Input+16,(1u<<16)|8u);break;
            case 33:P32(Input+16,(1u<<16)|6u);P32(Input+40,1);break;
        }
        CHECK(Run()!=MaliOk && !Pins && Untouched());
    }
    Init();DenyAt=1;CHECK(Run()==MaliFault && Pins==1 && Untouched());
    Init();P64(Input+24,0x80000080);Job(128,78,0,7,0xa0000008,1);DenyAt=2;
    CHECK(Run()==MaliFault && Pins==2 && Untouched());
    Init();P64(Input+24,0x80000080);Job(128,77,0,7,0xa0000000,1);
    CHECK(Run()==MaliInvalid && !Pins && Untouched());
    Init();Packet.StepCount=2;Packet.Bytes=MALI_EXEC_PACKET_BYTES(2);Packet.Steps[1]=Packet.Steps[0];
    CHECK(Run()==MaliInvalid && !Pins && Untouched());
}
static void Bounds(void) {
    Init();
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,4096,&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,0,&Work)==MaliInvalid && Untouched());
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,4096,&Packet,Packet.Bytes,Input,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliInvalid);
    CHECK(MaliCommandsPrepare(&Caps,Input,MALI_JM_VA_LIMIT-128,4096,&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliInvalid);
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,4096,&Packet,Packet.Bytes,Shadow,MALI_JM_VA_LIMIT-128,
        sizeof(Shadow),&Work,Pin,&Work)==MaliInvalid);
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,63,&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliInvalid);
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,sizeof(Input)+1,&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliInvalid);
    Init();Job(sizeof(Input)-128,1,0,7,0xa0000000,1);Packet.Steps[0].Chain=0x80000000+sizeof(Input)-128;
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,sizeof(Input),&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliOk && Pins==1);
    Init();CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,64,&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliOk && Pins==1);
    for(unsigned Type=4;Type<=7;Type++) {
        Init();uint64_t V=Type==7?UINT64_MAX:((UINT64_C(1)<<(8u<<(Type-4)))-1);
        Job(0,1,0,Type,0xa0000000,V);CHECK(Run()==MaliOk && Pins==1);
        CHECK(R64(Shadow+48)==V);
    }
    Init();for(unsigned I=0;I<65;I++)Job(I*128,I+1,I==64?0:0x80000000+128*(I+1),7,0xa0000000,I);
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,65*128,&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliInvalid && !Pins && Untouched());
    P64(Input+63*128+24,0);
    CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,64*128,&Packet,Packet.Bytes,Shadow,0x90000000,
        sizeof(Shadow),&Work,Pin,&Work)==MaliOk && Work.JobCount==64 && Pins==64);
    Init();Packet.StepCount=16;Packet.Bytes=MALI_EXEC_PACKET_BYTES(16);
    for(unsigned I=0;I<16;I++) {
        Job(I*128,1,0,7,0xa0000000,I);Packet.Steps[I].Chain=0x80000000+I*128;Packet.Steps[I].Slot=1;
    }
    CHECK(Run()==MaliOk && Work.JobCount==16 && Pins==16);
    for(unsigned I=0;I<16;I++)CHECK(!R64(Shadow+I*128+24) && Work.Packet.Steps[I].Chain==0x90000000+I*128);
}
static void BitMutations(void) {
    for(unsigned I=0;I<512;I++) {
        Init();Input[I/8]^=(uint8_t)(1u<<(I%8));
        MALI_RESULT R=Run();CHECK(R==MaliOk || R==MaliInvalid || R==MaliUnsupported);
        if(R)CHECK(Untouched() && !Pins);
        else CHECK(Work.JobCount==1 && Work.Packet.Steps[0].Chain==0x90000000 && Pins==1);
    }
}
static uint8_t ResourcePrivate[MALI_COMMAND_MAX_PRIVATE];
static unsigned ResourcePins,ResourceDenyAt;
static uint64_t ResourceAddresses[MALI_COMMAND_MAX_RESOURCES];
static uint32_t ResourceLengths[MALI_COMMAND_MAX_RESOURCES],ResourceAccesses[MALI_COMMAND_MAX_RESOURCES];
static void Resource(unsigned I,uint64_t Va,uint32_t Bytes,uint32_t Access) {
    uint8_t *P=ResourcePrivate+32+16*I;P64(P,Va);P32(P+8,Bytes);P32(P+12,Access);
}
static void ComputeInit(void) {
    Init();memset(Input,0,128);memset(ResourcePrivate,0,sizeof(ResourcePrivate));
    ResourcePins=ResourceDenyAt=0;Caps.JobFeatures[1]|=1u<<4;
    P32(Input+16,(11u<<16)|(4u<<1));P32(Input+32,7u|(3u<<10)|(1u<<20));
    P32(Input+36,1u|(2u<<14));P32(Input+40,2);P32(Input+44,3);P32(Input+48,4);
    P32(Input+52,5);P32(Input+56,6);P32(Input+60,7);P32(Input+68,2);
    P64(Input+96,0xa0000084);P64(Input+104,0xa0000000);P64(Input+112,0xa0000040);P64(Input+120,0xa00000c0);
    P32(ResourcePrivate,2);P32(ResourcePrivate+4,MALI_COMMAND_PRIVATE_BYTES(1,3));
    P32(ResourcePrivate+8,1);P32(ResourcePrivate+12,3);P64(ResourcePrivate+16,0x80000000);P32(ResourcePrivate+24,1);
    Resource(0,0xa0000000,4096,MaliPteRead);Resource(1,0xb0000000,4096,MaliPteRead|MaliPteExecute);
    Resource(2,0xc0000000,8192,MaliPteRead|MaliPteWrite);
}
static MALI_RESULT PinBuffer(void *C,uint64_t Va,uint32_t Bytes,uint32_t Access) {
    CHECK(C==&Work && !(Va&4095u) && Bytes && !(Bytes&4095u) && ResourcePins<MALI_COMMAND_MAX_RESOURCES);
    ResourceAddresses[ResourcePins]=Va;ResourceLengths[ResourcePins]=Bytes;ResourceAccesses[ResourcePins]=Access;
    ++ResourcePins;
    if(Mutate){memset(Input,0xff,sizeof(Input));memset(ResourcePrivate,0xff,sizeof(ResourcePrivate));}
    return ResourcePins==ResourceDenyAt?MaliFault:MaliOk;
}
static MALI_RESULT PinComputeWrite(void *C,uint64_t Va,uint32_t Bytes) {
    CHECK(C==&Work && Va>=0xc0000000 && Va+Bytes<=0xc0002000 && Bytes<=8);++Pins;return MaliOk;
}
static MALI_RESULT RunComputeBytes(uint32_t N,uint32_t PN) {
    return MaliCommandsPrepareResources(&Caps,Input,0x80000000,N,ResourcePrivate,PN,
        Shadow,0x90000000,sizeof(Shadow),&Work,PinComputeWrite,PinBuffer,&Work);
}
static MALI_RESULT RunCompute(void) {return RunComputeBytes(4096,R32(ResourcePrivate+4));}
static void ComputeResources(void) {
    ComputeInit();uint8_t Payload[96];memcpy(Payload,Input+32,sizeof(Payload));
    CHECK(RunCompute()==MaliOk && Work.ComputeCount==1 && Work.ResourceCount==3 && ResourcePins==3 && !Pins);
    for(unsigned I=0;I<sizeof(Payload);I++)CHECK(Payload[I]==Shadow[32+I]);
    CHECK((R32(Shadow+16)&255u)==8u);
    CHECK(ResourceAddresses[0]==0xa0000000 && ResourceAccesses[0]==MaliPteRead);
    CHECK(ResourceAddresses[1]==0xb0000000 && ResourceAccesses[1]==(MaliPteRead|MaliPteExecute));
    CHECK(ResourceAddresses[2]==0xc0000000 && ResourceLengths[2]==8192 && ResourceAccesses[2]==(MaliPteRead|MaliPteWrite));
    ComputeInit();Mutate=1;CHECK(RunCompute()==MaliOk && ResourcePins==3 && Work.ComputeCount==1);
    for(unsigned I=0;I<sizeof(Payload);I++)CHECK(Payload[I]==Shadow[32+I]);
    CHECK(ResourceAddresses[2]==0xc0000000 && Work.Packet.Steps[0].Slot==1);
    ComputeInit();P64(Input+24,0x80000080);Job(128,12,0,7,0xc0000000,42);P32(Input+148,11);
    CHECK(RunCompute()==MaliOk && Work.ComputeCount==1 && Work.WriteCount==1 && Pins==1 && ResourcePins==3);
    CHECK(R64(Shadow+128+32)==0xc0000000 && R32(Shadow+128+20)==1 && R64(Shadow+24)==0x90000080);
    for(unsigned At=1;At<=3;At++) {
        ComputeInit();ResourceDenyAt=At;CHECK(RunCompute()==MaliFault && ResourcePins==At && !Pins && Untouched());
    }
    for(unsigned Case=0;Case<42;Case++) {
        ComputeInit();
        switch(Case) {
        case 0:P32(ResourcePrivate,1);break;
        case 1:P32(ResourcePrivate+12,129);break;
        case 2:P32(ResourcePrivate+4,R32(ResourcePrivate+4)+1);break;
        case 3:Resource(0,0,4096,MaliPteRead);break;
        case 4:Resource(0,0xa0000001,4096,MaliPteRead);break;
        case 5:Resource(0,0xa0000000,4095,MaliPteRead);break;
        case 6:Resource(0,0xa0000000,0,MaliPteRead);break;
        case 7:Resource(0,0xa0000000,4096,MaliPteWrite);break;
        case 8:Resource(0,0xa0000000,4096,MaliPteRead|MaliPteValid);break;
        case 9:Resource(1,0xb0000000,4096,MaliPteRead|MaliPteWrite|MaliPteExecute);break;
        case 10:Resource(2,0xa0000000,4096,MaliPteRead|MaliPteWrite);break;
        case 11:Resource(2,0x80000000,4096,MaliPteRead|MaliPteWrite);break;
        case 12:Resource(2,0x90000000,4096,MaliPteRead|MaliPteWrite);break;
        case 13:Resource(2,MALI_JM_VA_LIMIT-4096,8192,MaliPteRead|MaliPteWrite);break;
        case 14:Resource(2,0xc0000000,536870912u,MaliPteRead|MaliPteWrite);break;
        case 15:Resource(1,0xb0000000,4096,MaliPteRead);break;
        case 16:P32(Input+32,R32(Input+32)|(1u<<30));break;
        case 17:P32(Input+32,1023u|(1023u<<10));break;
        case 18:P32(Input+36,0);break;
        case 19:P32(Input+36,1u|(3u<<14));break;
        case 20:P32(Input+36,0x10001);break;
        case 21:P32(Input+40,0);break;
        case 22:P32(Input+44,0);break;
        case 23:P32(Input+48,0);break;
        case 24:P32(Input+52,UINT32_MAX);break;
        case 25:P32(Input+56,UINT32_MAX);break;
        case 26:P32(Input+60,UINT32_MAX);break;
        case 27:P32(Input+64,1);break;
        case 28:P32(Input+68,256);break;
        case 29:P32(Input+72,1);break;
        case 30:P64(Input+96,0xa0000081);break;
        case 31:P64(Input+96,0xa0001004);break;
        case 32:P64(Input+104,0xa0000001);break;
        case 33:P64(Input+104,0xd0000000);break;
        case 34:P64(Input+112,0xa0000020);break;
        case 35:P64(Input+112,0xd0000000);break;
        case 36:P64(Input+120,0xa0000ff8);break;
        case 37:P64(Input+120,0xa0000001);break;
        case 38:P32(Input+68,0);break;
        case 39:Caps.JobFeatures[1]&=~(1u<<4);break;
        case 40:P64(Input+24,0x80000080);Job(128,12,0,7,0xa0000000,42);break;
        case 41:P64(Input+24,0x80000080);Job(128,12,0,7,0xc0002000,42);break;
        }
        CHECK(RunCompute()!=MaliOk && !ResourcePins && !Pins && Untouched());
    }
    ComputeInit();CHECK(RunComputeBytes(64,R32(ResourcePrivate+4))==MaliInvalid && !ResourcePins && Untouched());
    ComputeInit();CHECK(MaliCommandsPrepare(&Caps,Input,0x80000000,4096,ResourcePrivate,R32(ResourcePrivate+4),
        Shadow,0x90000000,sizeof(Shadow),&Work,Pin,&Work)==MaliInvalid && !ResourcePins && Untouched());
    ComputeInit();P32(Input+68,0);P64(Input+120,0);P64(Input+96,0);CHECK(RunCompute()==MaliOk);
    ComputeInit();P32(Input+32,1023);CHECK(RunCompute()==MaliOk);
    ComputeInit();
    for(unsigned I=1;I<64;I++){memcpy(Input+128*I,Input,128);P32(Input+128*I+16,((11u+I)<<16)|8u);}
    for(unsigned I=0;I<64;I++)P64(Input+128*I+24,I==63?0:0x80000000+128*(I+1));
    CHECK(RunComputeBytes(8192,R32(ResourcePrivate+4))==MaliOk && Work.JobCount==64 && Work.ComputeCount==64 && ResourcePins==3);
    ComputeInit();P32(ResourcePrivate+12,128);P32(ResourcePrivate+4,MALI_COMMAND_PRIVATE_BYTES(1,128));
    for(unsigned I=3;I<128;I++)Resource(I,0xd0000000+I*4096u,4096,MaliPteRead);
    CHECK(RunCompute()==MaliOk && ResourcePins==128 && Work.ResourceCount==128);
    for(unsigned Bit=0;Bit<1024;Bit++) {
        ComputeInit();Input[Bit/8]^=(uint8_t)(1u<<(Bit%8));MALI_RESULT R=RunCompute();
        CHECK(R==MaliOk || R==MaliInvalid || R==MaliUnsupported);
        if(R)CHECK(!ResourcePins && !Pins && Untouched());
        else CHECK(ResourcePins==3 && Work.ComputeCount==1 && Work.JobCount==1);
    }
}
static void FragmentInit(void) {
    ComputeInit();memset(Input,0,128);Caps.JobFeatures[1]|=1u<<9;
    P32(Input+16,(71u<<16)|(9u<<1));P32(Input+32,2u|(3u<<16));
    P32(Input+36,1023u|(2047u<<16));P64(Input+40,0xa0000001);
    /* Fast clears need no executable resource. Frame shaders are optional. */
    Resource(1,0xb0000000,4096,MaliPteRead);
}
static void FragmentResources(void) {
    const unsigned Orders[]={0,1,2,5,6};
    for(unsigned O=0;O<5;O++)for(unsigned T=1;T<=8;T++)for(unsigned Z=0;Z<2;Z++) {
        FragmentInit();P32(Input+36,1023u|(2047u<<16)|(Orders[O]<<28));
        P64(Input+40,0xa0000001u+((T-1u)<<2)+(Z<<1));
        uint8_t Payload[32];memcpy(Payload,Input+32,32);
        CHECK(RunCompute()==MaliOk && ResourcePins==3 && !Pins && Work.JobCount==1 && !Work.ComputeCount);
        CHECK((R32(Shadow+16)&255u)==18u && !R32(Shadow+20) && !R64(Shadow+24));
        for(unsigned I=0;I<32;I++)CHECK(Shadow[32+I]==Payload[I]);
        for(unsigned I=64;I<128;I++)CHECK(!Shadow[I]);
    }
    FragmentInit();Mutate=1;CHECK(RunCompute()==MaliOk && ResourcePins==3 && R64(Shadow+40)==0xa0000001);
    for(unsigned At=1;At<=3;At++) {
        FragmentInit();ResourceDenyAt=At;CHECK(RunCompute()==MaliFault && ResourcePins==At && !Pins && Untouched());
    }
    for(unsigned Case=0;Case<28;Case++) {
        FragmentInit();
        switch(Case) {
        case 0:P32(ResourcePrivate,1);P32(ResourcePrivate+12,0);P32(ResourcePrivate+4,32);break;
        case 1:Caps.JobFeatures[1]&=~(1u<<9);break;
        case 2:Caps.JobFeatures[1]=1u<<4;break;
        case 3:P32(Input+32,1024u|(3u<<16));break;
        case 4:P32(Input+32,2u|(2048u<<16));break;
        case 5:P32(Input+32,R32(Input+32)|(1u<<12));break;
        case 6:P32(Input+32,R32(Input+32)|(1u<<28));break;
        case 7:P32(Input+36,R32(Input+36)|(1u<<12));break;
        case 8:P32(Input+36,R32(Input+36)|(3u<<28));break;
        case 9:P32(Input+36,R32(Input+36)|(4u<<28));break;
        case 10:P32(Input+36,R32(Input+36)|(7u<<28));break;
        case 11:P64(Input+40,0xa0000000);break;
        case 12:P64(Input+40,0xa0000021);break;
        case 13:P64(Input+40,1);break;
        case 14:P64(Input+40,MALI_JM_VA_LIMIT|1u);break;
        case 15:P64(Input+40,0xa0000f81);break;
        case 16:P64(Input+40,0xa0000f43);break;
        case 17:P64(Input+40,0x90000001);break;
        case 18:P64(Input+40,0x80000001);break;
        case 19:P32(Input+48,1);break;
        case 20:P32(Input+52,1);break;
        case 21:P32(Input+56,1);break;
        case 22:P32(Input+60,1);break;
        case 23:P32(Input+36,R32(Input+36)|(1u<<31));P64(Input+48,0xb0000000);P32(Input+56,64);break;
        case 24:P64(Input+24,0x80000080);P32(Input+128+16,(72u<<16)|18u);break;
        case 25:P64(Input+40,UINT64_MAX);break;
        case 26:Caps.JobSlots&=~2u;break;
        case 27:P32(ResourcePrivate+12,0);P32(ResourcePrivate+4,32);break;
        }
        CHECK(RunCompute()!=MaliOk && !ResourcePins && !Pins && Untouched());
    }
    FragmentInit();P64(Input+40,0xa0000f41);CHECK(RunCompute()==MaliOk);
    FragmentInit();P32(Input+32,0);P32(Input+36,4095u|(4095u<<16));
    CHECK(RunComputeBytes(64,R32(ResourcePrivate+4))==MaliOk);
    FragmentInit();P64(Input+24,0x80000080);Job(128,72,0,7,0xc0000000,42);P32(Input+148,71);
    CHECK(RunCompute()==MaliOk && Work.JobCount==2 && Work.WriteCount==1 && Pins==1 && ResourcePins==3);
    CHECK(R64(Shadow+24)==0x90000080 && R32(Shadow+148)==1);
    FragmentInit();
    for(unsigned I=1;I<64;I++){memcpy(Input+128*I,Input,128);P32(Input+128*I+16,((71u+I)<<16)|18u);}
    for(unsigned I=0;I<64;I++)P64(Input+128*I+24,I==63?0:0x80000000+128*(I+1));
    CHECK(RunComputeBytes(8192,R32(ResourcePrivate+4))==MaliOk && Work.JobCount==64 && ResourcePins==3);
    for(unsigned Bit=0;Bit<512;Bit++) {
        FragmentInit();Input[Bit/8]^=(uint8_t)(1u<<(Bit%8));MALI_RESULT R=RunCompute();
        CHECK(R==MaliOk || R==MaliInvalid || R==MaliUnsupported);
        if(R)CHECK(!ResourcePins && !Pins && Untouched());
        else CHECK(ResourcePins==3 && !Pins && Work.JobCount==1);
    }
}
static uint8_t ClearDescriptors[MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES];
static MALI_COMMAND_CLEAR ClearDesc;
static void ClearInit(void) {
    ComputeInit();Caps.Tilers=1;Caps.JobFeatures[1]|=1u<<9;
    memset(&ClearDesc,0,sizeof(ClearDesc));ClearDesc.Bytes=sizeof(ClearDesc);
    ClearDesc.Width=17;ClearDesc.Height=19;ClearDesc.Pitch=128;ClearDesc.Format=MaliClearBgra8;ClearDesc.Slot=1;
    ClearDesc.Color[0]=0x3f800000;ClearDesc.Color[1]=0x3f000000;ClearDesc.Color[2]=0;ClearDesc.Color[3]=0x3e800000;
    ClearDesc.TargetVa=0xc0000000;ClearDesc.TargetBytes=4096;ClearDesc.HeapVa=0xd0000000;
    memset(Input,0x5a,128);memset(ClearDescriptors,0x5a,sizeof(ClearDescriptors));
    memset(ResourcePrivate,0x5a,MALI_COMMAND_CLEAR_PRIVATE_BYTES);
}
static MALI_RESULT BuildClear(void) {
    return MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,128,
        ClearDescriptors,0xa0000000,sizeof(ClearDescriptors),ResourcePrivate,MALI_COMMAND_CLEAR_PRIVATE_BYTES);
}
static int ClearUntouched(void) {
    for(unsigned I=0;I<128;I++)if(Input[I]!=0x5a)return 0;
    for(unsigned I=0;I<sizeof(ClearDescriptors);I++)if(ClearDescriptors[I]!=0x5a)return 0;
    for(unsigned I=0;I<MALI_COMMAND_CLEAR_PRIVATE_BYTES;I++)if(ResourcePrivate[I]!=0x5a)return 0;
    return 1;
}
static void ClearCommands(void) {
    const unsigned Formats[]={MaliClearBgra8,MaliClearBgrx8,MaliClearRgba8,MaliClearRgbx8};
    const unsigned Sizes[][2]={{1,1},{17,19},{2048,3},{2049,1},{16384,17},{16384,2048}};
    CHECK(sizeof(ClearDesc)==64);
    for(unsigned F=0;F<4;F++)for(unsigned I=0;I<sizeof(Sizes)/sizeof(Sizes[0]);I++) {
        ClearInit();ClearDesc.Format=Formats[F];ClearDesc.Width=Sizes[I][0];ClearDesc.Height=Sizes[I][1];
        ClearDesc.Pitch=(ClearDesc.Width*4u+63u)&~63u;
        ClearDesc.TargetBytes=(ClearDesc.Pitch*ClearDesc.Height+4095u)&~4095u;
        MALI_COMMAND_CLEAR Before=ClearDesc;CHECK(BuildClear()==MaliOk);
        CHECK(!memcmp(&Before,&ClearDesc,sizeof(Before)));
        CHECK(R32(Input+36)==(((ClearDesc.Width-1u)>>4)|(((ClearDesc.Height-1u)>>4)<<16)));
        CHECK(R64(Input+40)==0xa0000001 && !R64(Input+24));
        CHECK(R32(ClearDescriptors+32)==((ClearDesc.Width-1u)|((ClearDesc.Height-1u)<<16)));
        CHECK(R64(ClearDescriptors+16)==0xa0000200 && R64(ClearDescriptors+56)==0xa00000c0);
        CHECK(R32(ClearDescriptors+44)==0x01005080 && R32(ClearDescriptors+48)==0x10000);
        uint32_t Color=(F&1u)?0xff0080ffu:0x400080ffu;
        for(unsigned K=0;K<4;K++)CHECK(R32(ClearDescriptors+176+4*K)==Color);
        CHECK(R64(ClearDescriptors+160)==ClearDesc.TargetVa && R32(ClearDescriptors+168)==ClearDesc.Pitch);
        CHECK(R32(ClearDescriptors+172)==ClearDesc.Pitch*ClearDesc.Height);
        CHECK(R32(ClearDescriptors+200)==((I>=4?0x400u:0x80u)|(1u<<18)));
        CHECK(R32(ClearDescriptors+384)==0x29 && R64(ClearDescriptors+408)==ClearDesc.HeapVa+MALI_COMMAND_CLEAR_HEAP_BYTES);
        CHECK(R32(ClearDescriptors+512)==0x00800080 && R32(ClearDescriptors+640)==0x00800080);
        CHECK(RunComputeBytes(128,MALI_COMMAND_CLEAR_PRIVATE_BYTES)==MaliOk && ResourcePins==3 && !Pins);
        CHECK(ResourceAddresses[1]==ClearDesc.TargetVa && ResourceLengths[1]==ClearDesc.TargetBytes);
        CHECK(ResourceAddresses[2]==ClearDesc.HeapVa && ResourceLengths[2]==MALI_COMMAND_CLEAR_HEAP_BYTES);
        for(unsigned K=0;K<3;K++)CHECK(ResourceAccesses[K]==(MaliPteRead|MaliPteWrite));
        CHECK(R64(Shadow+40)==0xa0000001);
    }
    const uint32_t Bits[]={0,0x80000000,1,0x3b000000,0x3b008080,0x3b008082,0x3effffff,
        0x3f000000,0x3f000001,0x3f800000,0x40000000,0xbf800000,0x7f800000,0xff800000,0x7fc00001,0x7f800001};
    const unsigned Expected[]={0,0,0,0,0,1,127,128,128,255,255,0,255,0,0,0};
    for(unsigned I=0;I<sizeof(Bits)/sizeof(Bits[0]);I++) {
        ClearInit();for(unsigned C=0;C<4;C++)ClearDesc.Color[C]=Bits[I];
        CHECK(BuildClear()==MaliOk);CHECK(R32(ClearDescriptors+176)==Expected[I]*0x01010101u);
    }
    for(unsigned Case=0;Case<28;Case++) {
        ClearInit();
        switch(Case) {
        case 0:ClearDesc.Bytes--;break;case 1:ClearDesc.Reserved=1;break;
        case 2:ClearDesc.Width=0;break;case 3:ClearDesc.Width=16385;break;
        case 4:ClearDesc.Height=0;break;case 5:ClearDesc.Height=16385;break;
        case 6:ClearDesc.Pitch=64;break;case 7:ClearDesc.Pitch=129;break;
        case 8:ClearDesc.TargetBytes=2432;break;case 9:ClearDesc.TargetBytes=8192;break;
        case 10:ClearDesc.TargetVa=0;break;case 11:ClearDesc.TargetVa++;break;
        case 12:ClearDesc.HeapVa++;break;case 13:ClearDesc.HeapVa=0;break;
        case 14:ClearDesc.TargetVa=0x80000000;break;case 15:ClearDesc.TargetVa=0xa0000000;break;
        case 16:ClearDesc.HeapVa=ClearDesc.TargetVa;break;case 17:ClearDesc.HeapVa=0x80000000;break;
        case 18:ClearDesc.HeapVa=0xa0000000;break;case 19:ClearDesc.TargetVa=MALI_JM_VA_LIMIT;break;
        case 20:ClearDesc.HeapVa=MALI_JM_VA_LIMIT-4096;break;case 21:ClearDesc.Format=28;break;
        case 22:ClearDesc.Slot=3;break;case 23:Caps.JobFeatures[1]&=~(1u<<9);break;
        case 24:Caps.JobSlots=0;break;case 25:Caps.GpuId=0x70930000;break;case 26:Caps.Tilers=0;break;
        case 27:ClearDesc.Width=16384;ClearDesc.Height=8192;ClearDesc.Pitch=65536;ClearDesc.TargetBytes=536870912;break;
        }
        CHECK(BuildClear()!=MaliOk && ClearUntouched());
    }
    ClearInit();
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000001,128,ClearDescriptors,0xa0000000,4096,
        ResourcePrivate,80)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,127,ClearDescriptors,0xa0000000,4096,
        ResourcePrivate,80)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,128,ClearDescriptors,0xa0000000,4095,
        ResourcePrivate,80)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,128,ClearDescriptors,0xa0000000,4096,
        ResourcePrivate,79)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,128,ClearDescriptors,0xa0000001,4096,
        ResourcePrivate,80)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,128,Input,0xa0000000,4096,
        ResourcePrivate,80)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,128,ClearDescriptors,0xa0000000,4096,
        Input,80)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,(void*)&ClearDesc,0x80000000,128,ClearDescriptors,0xa0000000,4096,
        ResourcePrivate,80)==MaliInvalid && ClearUntouched());
    CHECK(MaliCommandsBuildClear(&Caps,&ClearDesc,Input,0x80000000,128,(void*)&Caps,0xa0000000,4096,
        ResourcePrivate,80)==MaliInvalid && ClearUntouched());
}
static uint8_t PreloadDescriptors[MALI_COMMAND_PRELOAD_DESCRIPTOR_BYTES];
static MALI_COMMAND_PRELOAD PreloadDesc;
static void PreloadInit(void) {
    ClearInit();memset(&PreloadDesc,0,sizeof(PreloadDesc));
    PreloadDesc.Bytes=sizeof(PreloadDesc);PreloadDesc.Width=17;PreloadDesc.Height=19;
    PreloadDesc.Pitch=128;PreloadDesc.Format=MaliClearBgra8;PreloadDesc.TargetBytes=4096;
    PreloadDesc.FauCount=4;PreloadDesc.ThreadStorageBytes=64;
    PreloadDesc.TargetVa=0xc0000000;PreloadDesc.HeapVa=0xd0000000;
    PreloadDesc.ShaderProgramVa=0xe0000000;PreloadDesc.FauVa=0xf0000000;
    PreloadDesc.ThreadStorageVa=0x100000000;
    memset(PreloadDescriptors,0x5a,sizeof(PreloadDescriptors));
}
static MALI_RESULT BuildPreload(void) {
    return MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,0xa0000000,
        sizeof(PreloadDescriptors));
}
static int PreloadUntouched(void) {
    for(unsigned I=0;I<sizeof(PreloadDescriptors);I++)if(PreloadDescriptors[I]!=0x5a)return 0;
    return 1;
}
static void PreloadDescriptorsTest(void) {
    const uint32_t Formats[]={MaliClearBgra8,MaliClearBgrx8,MaliClearRgba8};
    const uint32_t Orders[]={4u,20u,0u};
    const uint32_t TargetSwizzles[]={0x60au,0x60au,0x688u};
    CHECK(sizeof(PreloadDesc)==80u);
    for(unsigned I=0;I<3;I++) {
        PreloadInit();PreloadDesc.Format=Formats[I];
        MALI_COMMAND_PRELOAD Before=PreloadDesc;
        CHECK(BuildPreload()==MaliOk && !memcmp(&Before,&PreloadDesc,sizeof(Before)));
        CHECK(R32(PreloadDescriptors)==1u);
        CHECK(R64(PreloadDescriptors+16)==0xa0000200u);
        CHECK(R64(PreloadDescriptors+24)==0xa0000000u+MALI_COMMAND_PRELOAD_DCD0_OFFSET);
        CHECK(R32(PreloadDescriptors+32)==((16u)|((18u)<<16)));
        CHECK(R32(PreloadDescriptors+44)==0x01005080u && R32(PreloadDescriptors+48)==0x10000u);
        CHECK(R32(PreloadDescriptors+132)==(1u|(19u<<3)|(2u<<8)|(1u<<15)|
            (TargetSwizzles[I]<<16)|(1u<<31)));
        CHECK(R64(PreloadDescriptors+160)==PreloadDesc.TargetVa &&
            R32(PreloadDescriptors+168)==PreloadDesc.Pitch && R32(PreloadDescriptors+172)==2432u);
        for(unsigned J=MALI_COMMAND_PRELOAD_DCD1_OFFSET;J<MALI_COMMAND_PRELOAD_SAMPLER_OFFSET;J++)
            CHECK(PreloadDescriptors[J]==0);
        uint8_t *Dcd=PreloadDescriptors+MALI_COMMAND_PRELOAD_DCD0_OFFSET;
        CHECK(R32(Dcd)==0x203u && R32(Dcd+4)==0x0001ffffu && R32(Dcd+28)==0x3f800000u);
        CHECK(R64(Dcd+40)==0xa0000000u+MALI_COMMAND_PRELOAD_DEPTH_STENCIL_OFFSET);
        CHECK(R64(Dcd+48)==((0xa0000000u+MALI_COMMAND_PRELOAD_BLEND_OFFSET)|1u));
        CHECK(R32(Dcd+68)==PreloadDesc.FauCount);
        CHECK(R64(Dcd+96)==((0xa0000000u+MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET)|4u));
        CHECK(R64(Dcd+104)==PreloadDesc.ShaderProgramVa && R64(Dcd+112)==PreloadDesc.ThreadStorageVa);
        CHECK(R64(Dcd+120)==PreloadDesc.FauVa);
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_SAMPLER_OFFSET)==0x18299901u);
        uint8_t *Texture=PreloadDescriptors+MALI_COMMAND_PRELOAD_TEXTURE_OFFSET;
        CHECK(R32(Texture)==(2u|(2u<<4)|(187u<<22)|(Orders[I]<<10)));
        CHECK(R32(Texture+4)==(16u|(18u<<16)) && R32(Texture+8)==0x688u);
        CHECK(R64(Texture+16)==0xa0000000u+MALI_COMMAND_PRELOAD_PLANE_OFFSET);
        uint8_t *Plane=PreloadDescriptors+MALI_COMMAND_PRELOAD_PLANE_OFFSET;
        CHECK(R32(Plane)==0x0200021au && R32(Plane+4)==2432u);
        CHECK(R64(Plane+8)==PreloadDesc.TargetVa && R32(Plane+16)==PreloadDesc.Pitch);
        uint8_t *Resource=PreloadDescriptors+MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET;
        CHECK(R64(Resource)==((UINT64_C(0xa0000000)+MALI_COMMAND_PRELOAD_SAMPLER_OFFSET)|
            (UINT64_C(1)<<56)));
        CHECK(R32(Resource+4)==(1u<<24) && R64(Resource+8)==64u);
        for(unsigned J=16;J<64;J++)CHECK(Resource[J]==0);
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_BLEND_OFFSET)==0x00000a00u);
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_BLEND_OFFSET+4)==0xf0122122u);
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_BLEND_OFFSET+8)==0x19u);
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_BLEND_OFFSET+12)==(237u<<12));
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_DEPTH_STENCIL_OFFSET)==0x24f24f7u);
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_DEPTH_STENCIL_OFFSET+4)==UINT32_MAX);
        CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_DEPTH_STENCIL_OFFSET+16)==0xe4800000u);
        CHECK(R32(PreloadDescriptors+512)==0x00800080u && R32(PreloadDescriptors+640)==0x00800080u);
    }
    const uint32_t Sizes[][2]={{1u,1u},{17u,19u},{2048u,3u},{16384u,2048u}};
    for(unsigned I=0;I<sizeof(Sizes)/sizeof(Sizes[0]);I++) {
        PreloadInit();PreloadDesc.Width=Sizes[I][0];PreloadDesc.Height=Sizes[I][1];
        PreloadDesc.Pitch=(PreloadDesc.Width*4u+63u)&~63u;
        PreloadDesc.TargetBytes=(PreloadDesc.Pitch*PreloadDesc.Height+4095u)&~4095u;
        CHECK(BuildPreload()==MaliOk);
        CHECK(R32(PreloadDescriptors+32)==((PreloadDesc.Width-1u)|((PreloadDesc.Height-1u)<<16)));
        CHECK(R32(PreloadDescriptors+172)==PreloadDesc.Pitch*PreloadDesc.Height);
        uint32_t Largest=PreloadDesc.Width>PreloadDesc.Height?PreloadDesc.Width:PreloadDesc.Height;
        uint32_t Levels=0;for(uint32_t N=(Largest+15u)/16u;N;N>>=1)++Levels;
        CHECK(R32(PreloadDescriptors+200)==((0x80u<<(Levels>8u?Levels-8u:0u))|(1u<<18)));
    }
    PreloadInit();PreloadDesc.ThreadStorageVa=UINT64_C(0x110000000);
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,0x100000000u,
        sizeof(PreloadDescriptors))==MaliOk);
    CHECK(R64(PreloadDescriptors+MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET)==
        ((UINT64_C(0x100000000)+MALI_COMMAND_PRELOAD_SAMPLER_OFFSET)|(UINT64_C(1)<<56)));
    CHECK(R32(PreloadDescriptors+MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET+4)==0x01000001u);
    for(unsigned Case=0;Case<36;Case++) {
        PreloadInit();
        switch(Case) {
        case 0:PreloadDesc.Bytes--;break;
        case 1:PreloadDesc.Reserved[0]=1;break;
        case 2:PreloadDesc.Reserved[1]=1;break;
        case 3:PreloadDesc.Width=0;break;
        case 4:PreloadDesc.Width=16385;break;
        case 5:PreloadDesc.Height=0;break;
        case 6:PreloadDesc.Pitch=64;break;
        case 7:PreloadDesc.Pitch=129;break;
        case 8:PreloadDesc.TargetBytes=8192;break;
        case 9:PreloadDesc.Format=MaliClearRgbx8;break;
        case 10:PreloadDesc.TargetVa=0;break;
        case 11:PreloadDesc.TargetVa++;break;
        case 12:PreloadDesc.HeapVa++;break;
        case 13:PreloadDesc.ShaderProgramVa++;break;
        case 14:PreloadDesc.FauVa++;break;
        case 15:PreloadDesc.ThreadStorageVa++;break;
        case 16:PreloadDesc.FauCount=0;break;
        case 17:PreloadDesc.FauCount=256;break;
        case 18:PreloadDesc.ThreadStorageBytes=63;break;
        case 19:PreloadDesc.ThreadStorageBytes=65;break;
        case 20:PreloadDesc.TargetVa=0xa0000000;break;
        case 21:PreloadDesc.HeapVa=PreloadDesc.TargetVa;break;
        case 22:PreloadDesc.ShaderProgramVa=0xa0000000+MALI_COMMAND_PRELOAD_DCD_BYTES;break;
        case 23:PreloadDesc.FauVa=PreloadDesc.TargetVa;break;
        case 24:PreloadDesc.ThreadStorageVa=PreloadDesc.HeapVa;break;
        case 25:PreloadDesc.TargetVa=MALI_JM_VA_LIMIT-2048;break;
        case 26:PreloadDesc.HeapVa=MALI_JM_VA_LIMIT;break;
        case 27:PreloadDesc.ShaderProgramVa=MALI_JM_VA_LIMIT;break;
        case 28:PreloadDesc.FauVa=MALI_JM_VA_LIMIT;break;
        case 29:PreloadDesc.ThreadStorageVa=MALI_JM_VA_LIMIT;break;
        case 30:Caps.Tilers=0;break;
        case 31:PreloadDesc.HeapVa=MALI_JM_VA_LIMIT-4096;break;
        case 32:PreloadDesc.FauVa=MALI_JM_VA_LIMIT-8;break;
        case 33:PreloadDesc.ThreadStorageVa=MALI_JM_VA_LIMIT-64;PreloadDesc.ThreadStorageBytes=128;break;
        case 34:PreloadDesc.Width=16384;PreloadDesc.Height=8192;PreloadDesc.Pitch=65536;PreloadDesc.TargetBytes=536870912;break;
        case 35:PreloadDesc.Height=1024;PreloadDesc.TargetBytes=131072;PreloadDesc.TargetVa=MALI_JM_VA_LIMIT-4096;break;
        }
        MALI_COMMAND_PRELOAD Before=PreloadDesc;
        CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,0xa0000000,
            sizeof(PreloadDescriptors))!=MaliOk && PreloadUntouched() &&
            !memcmp(&Before,&PreloadDesc,sizeof(Before)));
    }
    PreloadInit();
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,0xa0000001,
        sizeof(PreloadDescriptors))==MaliInvalid && PreloadUntouched());
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,0xa0000000,
        sizeof(PreloadDescriptors)-1u)==MaliInvalid && PreloadUntouched());
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,(void *)&Caps,0xa0000000,
        sizeof(PreloadDescriptors))==MaliInvalid && PreloadUntouched());
    MALI_COMMAND_PRELOAD Before=PreloadDesc;
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,(void *)&PreloadDesc,0xa0000000,
        sizeof(PreloadDescriptors))==MaliInvalid && PreloadUntouched() &&
        !memcmp(&Before,&PreloadDesc,sizeof(Before)));
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,MALI_JM_VA_LIMIT,
        sizeof(PreloadDescriptors))==MaliInvalid && PreloadUntouched());
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,0xa0000000,
        sizeof(PreloadDescriptors))==MaliOk);
    PreloadInit();Caps.GpuId=0x70930000;
    CHECK(MaliCommandsBuildPreload(&Caps,&PreloadDesc,PreloadDescriptors,0xa0000000,
        sizeof(PreloadDescriptors))==MaliUnsupported && PreloadUntouched());
}
static void RasterRecord(unsigned Offset,unsigned Type,unsigned Index,uint64_t Next,int Secondary) {
    uint8_t *P=Input+Offset;unsigned Bytes=Type==11?384:Type==7?256:128;
    memset(P,0,Bytes);P32(P+16,(Index<<16)|(Type<<1));P64(P+24,Next);
    P32(P+32,8u|(Secondary?(1u<<18):0));P32(P+44,3);
    P64(P+56,0xc0000000);P32(P+108,127u|(127u<<16));
    if(Type==12){P64(P+48,0xa0000100);return;}
    P32(P+48,1);P32(P+52,Type==11?(Secondary?(32u|(16u<<16)):16u):3u);
    P32(P+112,0x3f800000);P32(P+132,0x1ffff);P32(P+156,0x3f800000);
    P64(P+136,Type==11?1u:0xa0000201u);P32(P+144,Type==11?0u:16u);
    P64(P+168,0xa0000300);P64(P+176,0xa0000341);
    for(unsigned E=192;E<Bytes;E+=64) {
        if(E==320 && !Secondary)continue;
        P32(P+E+4,2);P64(P+E+32,0xa0000084);P64(P+E+40,0xa0000000);
        P64(P+E+48,0xa0000040);P64(P+E+56,0xa00000c0);
    }
}
static void RasterInit(unsigned Type,int Secondary) {
    ComputeInit();Caps.JobFeatures[1]|=(1u<<7)|(1u<<11)|(1u<<12)|(1u<<9);
    RasterRecord(0,Type,71,0,Secondary);
}
static void EqualRaster(const uint8_t *A,const uint8_t *B,unsigned N) {
    for(unsigned I=0;I<N;I++)CHECK(A[I]==B[I]);
}
static void RasterCommands(void) {
    for(unsigned Type=7;Type<=12;Type++) {
        if(Type!=7 && Type!=11 && Type!=12)continue;
        RasterInit(Type,0);unsigned Bytes=Type==11?384:Type==7?256:128;
        uint8_t Payload[352];memcpy(Payload,Input+32,Bytes-32);
        CHECK(RunComputeBytes(Bytes,R32(ResourcePrivate+4))==MaliOk && ResourcePins==3 && !Pins);
        CHECK(Work.ShadowUsed==Bytes && Work.Jobs[0].Bytes==Bytes && !Work.Jobs[0].Offset);
        EqualRaster(Payload,Shadow+32,Bytes-32);CHECK(!R64(Shadow+24));
        for(unsigned I=Bytes;I<Bytes+128;I++)CHECK(!Shadow[I]);
        RasterInit(Type,0);Mutate=1;
        CHECK(RunComputeBytes(Bytes,R32(ResourcePrivate+4))==MaliOk && ResourcePins==3);
        EqualRaster(Payload,Shadow+32,Bytes-32);
        const unsigned Boundaries[]={64,127,128,255,256,383};
        for(unsigned I=0;I<sizeof(Boundaries)/sizeof(Boundaries[0]);I++)if(Boundaries[I]<Bytes) {
            RasterInit(Type,0);CHECK(RunComputeBytes(Boundaries[I],R32(ResourcePrivate+4))==MaliInvalid && !ResourcePins && Untouched());
        }
    }
    RasterInit(11,1);P32(Input+256,0xffffffff);P32(Input+320,37);
    CHECK(RunCompute()==MaliOk && Work.ShadowUsed==384 && R32(Shadow+320)==37);
    RasterInit(11,0);memset(Input+192,0,64);CHECK(RunCompute()==MaliOk); // Depth-only, no FS.
    RasterInit(11,0);P32(Input+32,1u|(2u<<11));CHECK(RunCompute()==MaliOk); // GPU point size output.
    for(unsigned Index=1;Index<=3;Index++) {
        RasterInit(11,0);P32(Input+32,8u|(Index<<8)|(1u<<19));P64(Input+120,0xa0000800);
        CHECK(RunCompute()==MaliOk && R64(Shadow+120)==0xa0000800);
    }
    RasterInit(7,0);P32(Input+32,1u|(3u<<11));P64(Input+112,0xa0000800);
    CHECK(RunCompute()==MaliOk);
    for(unsigned Case=0;Case<72;Case++) {
        RasterInit(11,0);
        switch(Case) {
        case 0:P32(Input+32,0);break;case 1:P32(Input+32,7);break;
        case 2:P32(Input+32,15);break;case 3:P32(Input+32,8u|(4u<<8));break;
        case 4:P32(Input+32,8u|(1u<<11));break;case 5:P32(Input+32,8u|(1u<<22));break;
        case 6:P32(Input+32,8u|(1u<<21));break;case 7:P32(Input+44,0);break;
        case 8:P32(Input+48,0);break;case 9:P32(Input+52,15);break;
        case 10:P32(Input+52,16u|(65535u<<16));break;case 11:P32(Input+52,32u|(16u<<16));break;
        case 12:P64(Input+56,0);break;case 13:P64(Input+56,0xc0000001);break;
        case 14:P64(Input+56,0xc0001f80);break;case 15:P64(Input+56,0xa0000000);break;
        case 16:P32(Input+64,1);break;case 17:P32(Input+100,1);break;
        case 18:P32(Input+104,128);break;case 19:P32(Input+104,128u<<16);break;
        case 20:P32(Input+116,1);break;case 21:P64(Input+120,0xa0000800);break;
        case 22:P32(Input+32,8u|(1u<<19));break;
        case 23:P32(Input+32,8u|(3u<<8));P32(Input+44,0x40000000);P64(Input+120,0xa0000800);break;
        case 24:P32(Input+32,8u|(2u<<8));P64(Input+120,0xa0000801);break;
        case 25:P32(Input+32,8u|(3u<<8));P64(Input+120,0xa0000ffc);break;
        case 26:P32(Input+128,1u<<12);break;case 27:P32(Input+128,1u<<24);break;
        case 28:P32(Input+128,2u<<14);break;case 29:P32(Input+132,1u<<24);break;
        case 30:P64(Input+136,0);break;case 31:P64(Input+136,0xa0000201);break;
        case 32:P32(Input+144,16);break;case 33:P32(Input+148,1);break;
        case 34:P32(Input+160,1);break;case 35:P32(Input+164,1);break;
        case 36:P64(Input+168,0xa0000301);break;case 37:P64(Input+168,0xa0001000);break;
        case 38:P64(Input+176,0xa0000349);break;case 39:P64(Input+176,0xa0000340);break;
        case 40:P64(Input+176,0xa0000ff2);break;case 41:P64(Input+184,0xc0000100);break;
        case 42:P32(Input+128,1u<<14);P64(Input+184,0xc0000101);break;
        case 43:P32(Input+128,3u<<14);P64(Input+184,0xa0000100);break;
        case 44:P32(Input+192,1);break;case 45:P32(Input+196,256);break;
        case 46:P32(Input+200,1);break;case 47:P64(Input+232,0xa0000001);break;
        case 48:P64(Input+240,0xa0000041);break;case 49:P64(Input+248,0xa0000ff8);break;
        case 50:P32(Input+260,256);break;case 51:P32(Input+280,1);break;
        case 52:P64(Input+288,0xa0000081);break;case 53:P64(Input+296,0);break;
        case 54:P64(Input+304,0);break;case 55:P64(Input+312,0xa00000c1);break;
        case 56:P32(Input+320,1);break;
        case 57:P32(Input+32,8u|(1u<<18));break;
        case 58:Resource(1,0xb0000000,4096,MaliPteRead);break;
        case 59:Caps.JobFeatures[1]&=~(1u<<11);break;
        case 60:P32(ResourcePrivate,1);P32(ResourcePrivate+12,0);P32(ResourcePrivate+4,32);break;
        case 61:P64(Input+24,0x80000080);break;
        case 62:RasterInit(7,0);P32(Input+52,0);break;
        case 63:RasterInit(7,0);P64(Input+136,0);break;
        case 64:RasterInit(7,0);P32(Input+32,1u|(2u<<11));P64(Input+112,0xa0000ffe);break;
        case 65:RasterInit(12,0);P64(Input+48,0xa0000101);break;
        case 66:RasterInit(12,0);P64(Input+48,0xa0000fc0);break;
        case 67:RasterInit(12,0);P32(Input+32,8u|(1u<<18));break;
        case 68:RasterInit(12,0);P32(Input+112,1);break;
        case 69:RasterInit(12,0);P32(Input+32,8u|(1u<<8));break;
        case 70:RasterInit(12,0);P32(Input+32,1u|(2u<<11));break;
        case 71:P32(Input+16,(71u<<16)|(10u<<1));break;
        }
        CHECK(RunCompute()!=MaliOk && !Pins && !ResourcePins && Untouched());
    }
    RasterInit(11,0);P32(Input+128,3u<<14);P64(Input+184,0xc0000100);
    CHECK(RunCompute()==MaliOk);
    RasterInit(11,1);ResourceDenyAt=3;CHECK(RunCompute()==MaliFault && ResourcePins==3 && Untouched());
    // Mix unequal payloads, then start another stage after a 64-byte fragment.
    RasterInit(11,1);P64(Input+24,0x80000180);
    RasterRecord(384,7,72,0x80000280,0);P32(Input+384+20,71);
    RasterRecord(640,12,73,0x80000300,0);P32(Input+640+20,72);
    memset(Input+768,0,128);P32(Input+784,(74u<<16)|(9u<<1));P32(Input+788,73);P64(Input+808,0xa0000001);
    P32(ResourcePrivate+4,MALI_COMMAND_PRIVATE_BYTES(2,3));P32(ResourcePrivate+8,2);
    memmove(ResourcePrivate+48,ResourcePrivate+32,48);P64(ResourcePrivate+32,0x80000380);P32(ResourcePrivate+40,1);P32(ResourcePrivate+44,0);
    RasterRecord(896,11,1,0,0);
    CHECK(RunCompute()==MaliOk && Work.JobCount==5 && Work.ShadowUsed==1280 && ResourcePins==3);
    CHECK(R64(Shadow+24)==0x90000180 && R64(Shadow+384+24)==0x90000280 &&
        R64(Shadow+640+24)==0x90000300 && !R64(Shadow+768+24));
    CHECK(Work.Packet.Steps[1].Chain==0x90000380 && R32(Shadow+896+20)==0 && (R32(Shadow+896+16)>>16)==1);
    RasterInit(11,0);
    for(unsigned I=0;I<64;I++)RasterRecord(384*I,11,I+1,I==63?0:0x80000000+384*(I+1),0);
    CHECK(RunComputeBytes(64*384,R32(ResourcePrivate+4))==MaliOk && Work.JobCount==64 && Work.ShadowUsed==sizeof(Shadow));
    EqualRaster(Input+63*384+32,Shadow+63*384+32,352);CHECK(!R64(Shadow+63*384+24));
    RasterInit(11,0);RasterRecord(384,11,2,0x80000000,0);P64(Input+24,0x80000180);
    CHECK(RunCompute()==MaliInvalid && !ResourcePins && Untouched());
}
static void RasterBuilder(void) {
    MALI_COMMAND_RESOURCE Resources[3]={{0xa0000000,4096,MaliPteRead},
        {0xb0000000,4096,MaliPteRead|MaliPteExecute},{0xc0000000,8192,MaliPteRead|MaliPteWrite}};
    MALI_COMMAND_ENVIRONMENT Env={0,2,0xa0000084,0xa0000000,0xa0000040,0xa00000c0};
    MALI_COMMAND_RASTER Base={0};Base.Bytes=sizeof(Base);Base.VertexSlot=1;Base.FragmentSlot=0;
    Base.Primitive=8;Base.IndexCount=3;Base.InstanceCount=1;Base.PrimitiveSize=0x3f800000;
    Base.Scissor[1]=127u|(127u<<16);Base.Flags[1]=0x1ffff;Base.MaxZ=0x3f800000;
    Base.BlendCount=1;Base.Tiler=0xc0000000;Base.DepthStencil=0xa0000300;
    Base.Blend=0xa0000340;Base.Framebuffer=0xa0000001;Base.Position=Base.Fragment=Env;
    for(unsigned Secondary=0;Secondary<2;Secondary++)for(unsigned Index=0;Index<4;Index++) {
        RasterInit(11,Secondary);Caps.Tilers=1;Caps.JobFeatures[0]|=1u<<9;
        MALI_COMMAND_RASTER D=Base;D.Primitive|=Index<<8;D.Indices=Index?0xa0000800:0;
        if(Secondary){D.Primitive|=1u<<18;D.VaryingStride=16;D.Varying=Env;}
        uint8_t Expected[512]={0};memcpy(Expected,Input,384);
        P32(Expected+16,(1u<<16)|(11u<<1));P32(Expected+32,D.Primitive);P64(Expected+120,D.Indices);
        P32(Expected+384+16,(1u<<16)|(9u<<1));P64(Expected+384+40,D.Framebuffer);
        CHECK(sizeof(D)==248 && sizeof(Env)==40);
        CHECK(MaliCommandsBuildRaster(&Caps,&D,Resources,3,Input,0x80000000,512,ResourcePrivate,96)==MaliOk);
        EqualRaster(Input,Expected,512);
        CHECK(R32(ResourcePrivate)==2 && R32(ResourcePrivate+4)==96 && R32(ResourcePrivate+8)==2);
        CHECK(R64(ResourcePrivate+16)==0x80000000 && R32(ResourcePrivate+24)==1);
        CHECK(R64(ResourcePrivate+32)==0x80000180 && R32(ResourcePrivate+40)==0);
        CHECK(RunComputeBytes(512,96)==MaliOk && ResourcePins==3 && Work.JobCount==2 && Work.ShadowUsed==512);
        CHECK(Work.Packet.Steps[0].Chain==0x90000000 && Work.Packet.Steps[1].Chain==0x90000180);
        EqualRaster(Input+32,Shadow+32,352);EqualRaster(Input+416,Shadow+416,32);
        CHECK(!R64(Shadow+24) && !R64(Shadow+408));
    }
    for(unsigned Case=0;Case<37;Case++) {
        RasterInit(11,0);Caps.Tilers=1;Caps.JobFeatures[0]|=1u<<9;
        MALI_COMMAND_RASTER D=Base;MALI_COMMAND_RESOURCE R[3];memcpy(R,Resources,sizeof(R));
        unsigned Count=3,CN=512,PN=96;uint64_t Va=0x80000000;
        switch(Case) {
        case 0:D.Bytes--;break;case 1:D.VertexSlot=3;break;case 2:D.FragmentSlot=3;break;
        case 3:Caps.GpuId=0;break;case 4:Caps.Tilers=0;break;case 5:Caps.JobSlots=1;break;
        case 6:Caps.JobFeatures[1]=1u<<9;break;case 7:Caps.JobFeatures[0]=1u<<11;break;
        case 8:Va++;break;case 9:Va=MALI_JM_VA_LIMIT-128;break;case 10:CN--;break;
        case 11:PN--;break;case 12:Count=0;break;case 13:Count=129;break;
        case 14:D.VaryingStride=65520;break;case 15:D.BlendCount=9;break;case 16:D.Blend++;break;
        case 17:D.Position.FauCount=256;break;case 18:D.Position.ThreadStorage++;break;
        case 19:D.Fragment.AttributeOffset=1;break;case 20:D.Varying=Env;break;
        case 21:D.Primitive|=1u<<21;break;case 22:D.IndexCount=0;break;case 23:D.InstanceCount=0;break;
        case 24:D.Framebuffer=0;break;case 25:D.Tiles[1]=1u<<31;break;case 26:D.Tiler=0xa0000000;break;
        case 27:R[0].Access|=MaliPteWrite|MaliPteExecute;break;case 28:R[1].Va=R[0].Va;break;
        case 29:R[0].Va=Va;break;case 30:R[0].Bytes=4095;break;case 31:R[0].Access=0;break;
        case 32:R[2].Bytes=0x20001000;break;case 33:D.Primitive=7;break;
        case 34:D.Scissor[0]=128;break;case 35:D.Indices=0xa0000800;break;case 36:D.Flags[0]=2u<<14;break;
        }
        memset(Input,0x5a,512);memset(ResourcePrivate,0xa5,96);
        CHECK(MaliCommandsBuildRaster(&Caps,&D,R,Count,Input,Va,CN,ResourcePrivate,PN)!=MaliOk);
        for(unsigned I=0;I<512;I++)CHECK(Input[I]==0x5a);
        for(unsigned I=0;I<96;I++)CHECK(ResourcePrivate[I]==0xa5);
    }
    RasterInit(11,0);Caps.Tilers=1;Caps.JobFeatures[0]|=1u<<9;
    CHECK(MaliCommandsBuildRaster(0,&Base,Resources,3,Input,0x80000000,512,ResourcePrivate,96)==MaliInvalid);
    CHECK(MaliCommandsBuildRaster(&Caps,0,Resources,3,Input,0x80000000,512,ResourcePrivate,96)==MaliInvalid);
    CHECK(MaliCommandsBuildRaster(&Caps,&Base,0,3,Input,0x80000000,512,ResourcePrivate,96)==MaliInvalid);
    CHECK(MaliCommandsBuildRaster(&Caps,&Base,Resources,3,Input,0x80000000,512,Input,96)==MaliInvalid);
    CHECK(MaliCommandsBuildRaster(&Caps,&Base,Resources,3,&Base,0x80000000,512,ResourcePrivate,96)==MaliInvalid);
    CHECK(MaliCommandsBuildRaster(&Caps,&Base,Resources,3,Input,0x80000000,512,Resources,96)==MaliInvalid);
    CHECK(MaliCommandsBuildRaster(&Caps,&Base,Resources,3,&Caps,0x80000000,512,ResourcePrivate,96)==MaliInvalid);
}
static void ShaderPrograms(void) {
    Init();MALI_COMMAND_SHADER S={sizeof(S),3,32,1,1,0,0xb0000040,UINT64_C(0xa501000000000000)};
    CHECK(sizeof(S)==40 && MaliCommandsBuildShader(&Caps,&S,Input,32)==MaliOk);
    CHECK(R32(Input)==0x80020138u && R32(Input+4)==0xa501 && R64(Input+8)==S.CodeVa);
    for(unsigned I=16;I<32;I++)CHECK(!Input[I]);
    S.Stage=2;S.WarpLimit=0;S.HelperThreads=1;S.WorkRegisters=33;S.FlushToZero=2;
    CHECK(MaliCommandsBuildShader(&Caps,&S,Input,32)==MaliOk && R32(Input)==0x10040028u);
    for(unsigned Case=0;Case<16;Case++) {
        Init();memset(Input,0x5a,32);
        S=(MALI_COMMAND_SHADER){sizeof(S),3,32,1,1,0,0xb0000040,UINT64_C(0xa501000000000000)};
        switch(Case) {
        case 0:S.Bytes--;break;case 1:S.Stage=0;break;case 2:S.Stage=4;break;
        case 3:S.WorkRegisters=65;break;case 4:S.WarpLimit=4;break;
        case 5:S.FlushToZero=3;break;case 6:S.HelperThreads=2;break;case 7:S.HelperThreads=1;break;
        case 8:S.Stage=2;break;case 9:S.Preload|=1;break;case 10:S.Preload|=UINT64_C(1)<<47;break;
        case 11:S.CodeVa=0;break;case 12:S.CodeVa++;break;case 13:S.CodeVa=MALI_JM_VA_LIMIT;break;
        case 14:S.CodeVa=MALI_JM_VA_LIMIT-4;break;case 15:Caps.GpuId=0x70930000;break;
        }
        CHECK(MaliCommandsBuildShader(&Caps,&S,Input,32)!=MaliOk);
        for(unsigned I=0;I<32;I++)CHECK(Input[I]==0x5a);
    }
    Init();S=(MALI_COMMAND_SHADER){sizeof(S),3,64,0,0,0,MALI_JM_VA_LIMIT-8,UINT64_C(1)<<63};
    CHECK(MaliCommandsBuildShader(&Caps,&S,Input,32)==MaliOk);
    memset(Input,0x5a,32);
    CHECK(MaliCommandsBuildShader(0,&S,Input,32)==MaliInvalid);
    CHECK(MaliCommandsBuildShader(&Caps,0,Input,32)==MaliInvalid);
    CHECK(MaliCommandsBuildShader(&Caps,&S,0,32)==MaliInvalid);
    CHECK(MaliCommandsBuildShader(&Caps,&S,Input,31)==MaliInvalid);
    CHECK(MaliCommandsBuildShader(&Caps,&S,Input,33)==MaliInvalid);
    CHECK(MaliCommandsBuildShader(&Caps,&S,&S,32)==MaliInvalid);
    CHECK(MaliCommandsBuildShader(&Caps,&S,&Caps,32)==MaliInvalid);
    for(unsigned I=0;I<32;I++)CHECK(Input[I]==0x5a);
}
unsigned TestMain(void) {ShaderPrograms();Mixed();Rejects();Bounds();BitMutations();ComputeResources();FragmentResources();RasterCommands();RasterBuilder();ClearCommands();PreloadDescriptorsTest();return FirstFailure;}
#ifdef MALI_COMMAND_TEST_NATIVE
int main(void) {
    unsigned R=TestMain();
    printf("Mali command validator: %u assertions, first failure line %u (ASan/UBSan)\n",MtkMaliCommandsAssertionCount,R);
    return R?1:0;
}
#endif
