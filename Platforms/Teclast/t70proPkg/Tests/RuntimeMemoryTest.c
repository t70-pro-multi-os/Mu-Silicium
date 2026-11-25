// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Library/FdtLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/ArmLib.h>
static VOID EFIAPI TestArmCallSmc (ARM_SMC_ARGS *Args);
static BOOLEAN TestMmuEnabled;
BOOLEAN EFIAPI ArmMmuEnabled (VOID) { return TestMmuEnabled; }
#define ArmCallSmc TestArmCallSmc
#include "../Library/MemoryMapLib/MemoryMapLib.c"
#include "Display/LkDisplayFixture.h"
static LK_DISPLAY_FIXTURE *LkIo;
static UINT8 CcciHandoffImage[T70_CCCI_MEMORY_HANDOFF_BYTES];
static UINTN CcciHandoffWriteOffsets[16], CcciHandoffWriteWidths[16];
static UINT64 CcciHandoffWriteValues[16];
static UINTN CcciHandoffWriteCount;
static UINT64 CcciMapState;
static UINTN CcciCacheWritebacks;
static UINTN CcciSmcCalls;
static UINTN CcciSmcReturn;
static UINTN CcciSmcBootStatus0 = 0x5443000CU;
static UINTN CcciSmcBootStatus1;
static VOID TrackCcciHandoffWrite(UINTN Address, UINT64 Value, UINTN Bytes) {
  UINT64 Base=T70_CCCI_MEMORY_HANDOFF_ADDRESS;
  if ((UINT64)Address<Base || (UINT64)Address-Base>=sizeof(CcciHandoffImage) ||
      Bytes>sizeof(CcciHandoffImage)-((UINT64)Address-Base)) return;
  UINTN Offset=(UINTN)((UINT64)Address-Base);
  CHECK(CcciHandoffWriteCount<ARRAY_SIZE(CcciHandoffWriteOffsets));
  if (CcciHandoffWriteCount<ARRAY_SIZE(CcciHandoffWriteOffsets)) {
    CcciHandoffWriteOffsets[CcciHandoffWriteCount]=Offset;
    CcciHandoffWriteWidths[CcciHandoffWriteCount]=Bytes;
    CcciHandoffWriteValues[CcciHandoffWriteCount]=Value;
    CcciHandoffWriteCount++;
  }
  CopyMem(CcciHandoffImage+Offset,&Value,Bytes);
}
static VOID EFIAPI TestArmCallSmc (ARM_SMC_ARGS *Args) {
  CHECK(Args!=NULL && Args->Arg0==T70_CCCI_SMC_BOOT_STATUS_FID &&
      Args->Arg1==T70_CCCI_SMC_POWER_CONFIG &&
      Args->Arg2==T70_CCCI_SMC_MD_BOOT_STATUS);
  if(Args==NULL) return;
  CcciSmcCalls++;
  Args->Arg0=CcciSmcReturn;
  Args->Arg1=CcciSmcBootStatus0;
  Args->Arg2=CcciSmcBootStatus1;
}
UINT32 EFIAPI MmioRead32(UINTN Address) {
  UINT32 Value=0; CHECK(LkIo!=NULL); CHECK(LkFixtureRead(LkIo,Address,&Value)==EFI_SUCCESS); return Value;
}
VOID *EFIAPI WriteBackDataCacheRange(VOID *Address, UINTN Length) {
  CHECK(Address != NULL && Length != 0U);
  CcciCacheWritebacks++;
  return Address;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  TrackCcciHandoffWrite(Address,Value,sizeof(Value));
  if ((UINT64)Address>=T70_CCCI_MEMORY_HANDOFF_ADDRESS &&
      (UINT64)Address<T70_CCCI_MEMORY_HANDOFF_ADDRESS+T70_CCCI_MEMORY_HANDOFF_BYTES) return Value;
  CHECK(LkIo!=NULL); CHECK(LkFixtureWrite(LkIo,Address,Value)==EFI_SUCCESS); return Value;
}
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
UINT64 EFIAPI MmioRead64(UINTN Address) {
  UINT64 Value=0;
  if ((UINT64)Address==T70_CCCI_MEMORY_MAP_STATE_ADDRESS) return CcciMapState;
  if ((UINT64)Address>=T70_CCCI_MEMORY_HANDOFF_ADDRESS &&
      (UINT64)Address-T70_CCCI_MEMORY_HANDOFF_ADDRESS<=
          sizeof(CcciHandoffImage)-sizeof(Value)) {
    CopyMem(&Value,CcciHandoffImage+
        ((UINT64)Address-T70_CCCI_MEMORY_HANDOFF_ADDRESS),sizeof(Value));
  }
  return Value;
}
UINT64 EFIAPI MmioWrite64(UINTN Address,UINT64 Value) {
  if ((UINT64)Address==T70_CCCI_MEMORY_MAP_STATE_ADDRESS) {
    CcciMapState=Value;
    return Value;
  }
  TrackCcciHandoffWrite(Address,Value,sizeof(Value));
  return Value;
}
static void NoOverlap(EFI_MEMORY_REGION_DESCRIPTOR *r, UINT8 count, UINT64 a, UINT64 b) {
  for (unsigned n=ARRAY_SIZE(mBootstrap);n<count;n++) {
    if (r[n].MemoryType != EfiConventionalMemory) continue;
    CHECK((r[n].Address & 4095)==0 && (r[n].Length & 4095)==0 && r[n].Length>0);
    CHECK(r[n].Address+r[n].Length<=a || r[n].Address>=b);
  }
}

static UINT8 CcciTags[0x1000];

static VOID PutLe32(UINT8 *P, UINT32 Value) {
  for (UINTN I=0;I<4;I++) P[I]=(UINT8)(Value>>(I*8));
}

static VOID PutLe64(UINT8 *P, UINT64 Value) {
  for (UINTN I=0;I<8;I++) P[I]=(UINT8)(Value>>(I*8));
}

static VOID WriteCcciTags(UINT8 *Tags, UINTN NameBytes) {
  UINTN HeaderBytes=NameBytes+12U;
  UINT32 TableOffset=(UINT32)(HeaderBytes*2U);
  ZeroMem(Tags,0x1000);
  CopyMem(Tags,"hdr_count",sizeof("hdr_count"));
  PutLe32(Tags+NameBytes,0x100U);
  PutLe32(Tags+NameBytes+4U,4U);
  PutLe32(Tags+NameBytes+8U,(UINT32)HeaderBytes);
  CopyMem(Tags+HeaderBytes,"hdr_tbl_inf",sizeof("hdr_tbl_inf"));
  PutLe32(Tags+HeaderBytes+NameBytes,TableOffset+4U);
  PutLe32(Tags+HeaderBytes+NameBytes+4U,4U*24U);
  PutLe32(Tags+HeaderBytes+NameBytes+8U,0U);
  PutLe32(Tags+0x100U,4U);
  PutLe64(Tags+TableOffset+4U,0x82000000ULL);
  PutLe32(Tags+TableOffset+12U,0x04000000U);
  Tags[TableOffset+16U]=0U;
  Tags[TableOffset+17U]=0U;
  for (UINTN I=1U;I<4U;I++) {
    UINTN Entry=TableOffset+4U+I*24U;
    Tags[Entry+12U]=(UINT8)I;
    Tags[Entry+13U]=1U;
  }
}

static UINTN CcciTagReaderCalls;
static EFI_STATUS EFIAPI ReadCcciTestTags(UINT64 Base, UINTN Bytes, VOID *Buffer) {
  (VOID)Base;
  if (Buffer==NULL || Bytes>sizeof(CcciTags)) return EFI_BAD_BUFFER_SIZE;
  CcciTagReaderCalls++;
  CopyMem(Buffer,CcciTags,Bytes);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI RejectCcciTestTagRead(UINT64 Base, UINTN Bytes, VOID *Buffer) {
  (VOID)Base; (VOID)Bytes; (VOID)Buffer;
  CcciTagReaderCalls++;
  return EFI_DEVICE_ERROR;
}

static void TestCcciTagParser(void) {
  UINT8 PropertyV1[16]={0};
  UINT8 PropertyV2[24]={0};
  CCCI_MD_MEMORY Memory;
  CHECK(T70CcciExpectedMdBootStatus(6295U,1U)==0x5443000CU);
  CHECK(T70CcciExpectedMdBootStatus(6295U,0U)==0x54430007U);
  CHECK(T70CcciExpectedMdBootStatus(6298U,1U)==0x5443000FU);
  CHECK(T70CcciExpectedMdBootStatus(6296U,0U)==0U);
  UINT32 MdGeneration=0U, MdSubVersion=0U;
  CHECK(ReadCcciBootStatusExpected(NULL,&MdGeneration,&MdSubVersion)==0U);
  CHECK(MdGeneration==0U && MdSubVersion==0U);
  TestMmuEnabled=TRUE;
  CHECK(ReadCcciTagBlockPhysical(0x82000000U,64U,CcciTags)==EFI_ACCESS_DENIED);
  TestMmuEnabled=FALSE;
  PutLe64(PropertyV1,0x100400100ULL);
  PutLe32(PropertyV1+8,0x1000U);
  PutLe32(PropertyV1+12,2U);
  WriteCcciTags(CcciTags,16U);
  CHECK(ParseCcciMdMemory(PropertyV1,sizeof(PropertyV1),FALSE,
      CcciTags,sizeof(CcciTags),&Memory)==EFI_SUCCESS);
  CHECK(Memory.Base==0x82000000ULL && Memory.Size==0x04000000U && Memory.ImageLoadReportedOk);

  PutLe64(PropertyV2,0x100400100ULL);
  PutLe32(PropertyV2+8,0x1000U);
  PutLe32(PropertyV2+12,0U);
  PutLe32(PropertyV2+16,2U);
  PutLe32(PropertyV2+20,2U);
  WriteCcciTags(CcciTags,64U);
  CHECK(ParseCcciMdMemory(PropertyV2,sizeof(PropertyV2),TRUE,
      CcciTags,sizeof(CcciTags),&Memory)==EFI_SUCCESS);
  CHECK(Memory.Base==0x82000000ULL && Memory.Size==0x04000000U);

  PutLe32(CcciTags+64U+8U,0U);
  CHECK(ParseCcciMdMemory(PropertyV2,sizeof(PropertyV2),TRUE,
      CcciTags,sizeof(CcciTags),&Memory)==EFI_COMPROMISED_DATA);
  PutLe32(CcciTags+64U+8U,76U);
  CcciTags[76U+64U+12U+4U+13U]=1U;
  CHECK(ParseCcciMdMemory(PropertyV2,sizeof(PropertyV2),TRUE,
      CcciTags,sizeof(CcciTags),&Memory)==EFI_SUCCESS && !Memory.ImageLoadReportedOk);
}

static void Cell(void *fdt, INT32 node, const char *name, UINT32 value) {
  UINT8 bytes[4]={(UINT8)(value>>24),(UINT8)(value>>16),(UINT8)(value>>8),(UINT8)value};
  CHECK(FdtSetProp(fdt,node,name,bytes,4)==0);
}
static INT32 VideoTree(void *fdt, const void *original, UINTN size, UINTN capacity,
                       UINT64 base, UINT32 bytes) {
  CHECK(FdtOpenInto(original,fdt,(INT32)capacity)==0);
  INT32 chosen=FdtAddSubnode(fdt,0,"chosen"); CHECK(chosen>=0);
  Cell(fdt,chosen,"atag,videolfb-fb_base_l",(UINT32)base);
  Cell(fdt,chosen,"atag,videolfb-fb_base_h",(UINT32)(base>>32));
  Cell(fdt,chosen,"atag,videolfb-vramSize",bytes);
  (VOID)size;
  return chosen;
}
static void CheckVideoMemory(const void *original, UINTN size) {
  UINTN capacity=size+8192; UINT8 *tree=malloc(capacity); CHECK(tree);
  EFI_MEMORY_REGION_DESCRIPTOR r[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINT8 count, initial=ARRAY_SIZE(mBootstrap); INT32 chosen, fb, reserved;
  /* No standard GOP description is needed to stop the allocator using LK VRAM. */
  VideoTree(tree,original,size,capacity,0x80001003,0x1234);
  count=initial;
  CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_SUCCESS);
  NoOverlap(r,count,0x80001000,0x80003000);
  for (unsigned i=initial;i<count;i++) if (r[i].MemoryType==EfiReservedMemoryType)
    CHECK(r[i].HobOption==HobOnlyNoCacheSetting);
  VideoTree(tree,original,size,capacity,0x180001003ULL,0x1234);
  count=initial;
  CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_SUCCESS);
  NoOverlap(r,count,0x180001000ULL,0x180003000ULL);
  /* Partial/zero/out-of-DRAM/overflowing tags never expose additional memory. */
  for (unsigned bad=0;bad<8;bad++) {
    chosen=VideoTree(tree,original,size,capacity,0x80000000,0x2000);
    if (bad==0) CHECK(FdtSetProp(tree,chosen,"atag,videolfb-fb_base_h",NULL,0)==0);
    if (bad==1) Cell(tree,chosen,"atag,videolfb-vramSize",0);
    if (bad==2) Cell(tree,chosen,"atag,videolfb-fb_base_l",0);
    if (bad==3) Cell(tree,chosen,"atag,videolfb-fb_base_h",16);
    if (bad==4) Cell(tree,chosen,"atag,videolfb-fb_base_l",0x44000000);
    if (bad==5) Cell(tree,chosen,"atag,videolfb-fb_base_h",3);
    if (bad==7) Cell(tree,chosen,"atag,videolfb-fb_base_l",DTB_END);
    if (bad==6) {
      Cell(tree,chosen,"atag,videolfb-fb_base_l",0xfffff000);
      Cell(tree,chosen,"atag,videolfb-fb_base_h",15);
    }
    count=initial;
    CHECK(EFI_ERROR(BuildRuntimeMemoryMap(tree,capacity,r,&count)) && count==initial);
  }
  /* The actual shared GOP parser feeds a reserved, CPU-mapped UC region. */
  chosen=VideoTree(tree,original,size,capacity,0x80000000,0x8000);
  Cell(tree,chosen,"#address-cells",2); Cell(tree,chosen,"#size-cells",2);
  reserved=FdtPathOffset(tree,"/reserved-memory"); CHECK(reserved>=0);
  fb=FdtAddSubnode(tree,reserved,"framebuffer@80000000"); CHECK(fb>=0);
  UINT8 reg[16]={0,0,0,0,0x80,0,0,0,0,0,0,0,0,0,0x50,0};
  CHECK(FdtSetProp(tree,fb,"reg",reg,sizeof(reg))==0);
  Cell(tree,fb,"phandle",7);
  chosen=FdtPathOffset(tree,"/chosen"); CHECK(chosen>=0);
  fb=FdtAddSubnode(tree,chosen,"framebuffer@80000000"); CHECK(fb>=0);
  CHECK(FdtSetPropString(tree,fb,"compatible","simple-framebuffer")==0);
  CHECK(FdtSetPropString(tree,fb,"format","x8r8g8b8")==0);
  CHECK(FdtSetProp(tree,fb,"reg",reg,sizeof(reg))==0);
  Cell(tree,fb,"width",2); Cell(tree,fb,"height",3); Cell(tree,fb,"stride",16);
  Cell(tree,fb,"memory-region",7);
  /* A legal size-only pool before the fixed framebuffer must not hide it. */
  reserved=FdtPathOffset(tree,"/reserved-memory"); CHECK(reserved>=0);
  INT32 pool=FdtAddSubnode(tree,reserved,"os-dynamic-pool"); CHECK(pool>=0);
  UINT8 poolsize[8]={0,0,0,0,1,0,0,0};
  CHECK(FdtSetProp(tree,pool,"size",poolsize,sizeof(poolsize))==0);
  CHECK(FdtSetPropEmpty(tree,pool,"no-map")==0);
  /* Fixed pools can contain several disjoint reg tuples before scanout. */
  reserved=FdtPathOffset(tree,"/reserved-memory"); CHECK(reserved>=0);
  INT32 multi=FdtAddSubnode(tree,reserved,"two-fixed-ranges"); CHECK(multi>=0);
  UINT8 fixed[32]={0,0,0,0,0x90,0,0,0,0,0,0,0,0,0,0x20,0,
                   0,0,0,0,0x91,0,0,0,0,0,0,0,0,0,0x30,0};
  CHECK(FdtSetProp(tree,multi,"reg",fixed,sizeof(fixed))==0);
  CHECK(FdtSetPropEmpty(tree,multi,"no-map")==0);
  count=initial;
  CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_SUCCESS);
  NoOverlap(r,count,0x80000000,0x80008000);
  unsigned mapped=0;
  for (unsigned i=initial;i<count;i++) if (r[i].MemoryType==EfiReservedMemoryType &&
      r[i].HobOption==AddMem) {
    CHECK(r[i].Address==0x80000000 && r[i].Length==0x5000);
    CHECK(r[i].HobOption==AddMem && r[i].ResourceType==SYS_MEM);
    CHECK(r[i].ArmAttributes==UNCACHED_UNBUFFERED);
    CHECK((r[i].ResourceAttribute & EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE)!=0);
    mapped++;
  }
  CHECK(mapped==1);
  MTK_DISPLAY_FRAMEBUFFER layout;
  CHECK(MtkDisplayParseHandoff(tree,capacity,&layout)==EFI_SUCCESS);
  EFI_MEMORY_DESCRIPTOR efi[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINTN descriptors=0;
  for (unsigned i=initial;i<count;i++) {
    ZeroMem(&efi[descriptors],sizeof(efi[descriptors]));
    efi[descriptors].PhysicalStart=r[i].Address;
    efi[descriptors].NumberOfPages=r[i].Length/EFI_PAGE_SIZE;
    efi[descriptors].Type=r[i].MemoryType;
    descriptors++;
  }
  CHECK(MtkDisplayValidateMemoryMap(&layout,efi,descriptors*sizeof(*efi),sizeof(*efi))==EFI_SUCCESS);

  /* A conflicting scanout range, wrong format or missing reservation fails. */
  chosen=FdtPathOffset(tree,"/chosen");
  Cell(tree,chosen,"atag,videolfb-vramSize",0x1000);
  count=initial;
  CHECK(EFI_ERROR(BuildRuntimeMemoryMap(tree,capacity,r,&count)) && count==initial);
  Cell(tree,chosen,"atag,videolfb-vramSize",0x8000);
  fb=FdtPathOffset(tree,"/chosen/framebuffer@80000000");
  CHECK(FdtSetPropString(tree,fb,"format","r5g6b5")==0);
  count=initial;
  CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_UNSUPPORTED && count==initial);
  free(tree);
}


static void SizeCells(void *tree, INT32 node, const char *name, UINT64 value) {
  UINT8 bytes[8];
  for (unsigned i=0;i<8;i++) bytes[i]=(UINT8)(value>>(56-i*8));
  CHECK(FdtSetProp(tree,node,name,bytes,sizeof(bytes))==0);
}
static void CheckDynamicMemory(const void *original, UINTN size) {
  UINTN capacity=size+8192; UINT8 *tree=malloc(capacity),*saved=malloc(capacity); CHECK(tree && saved);
  EFI_MEMORY_REGION_DESCRIPTOR r[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT],base[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINT8 initial=ARRAY_SIZE(mBootstrap),count=initial,basecount=initial;
  CHECK(BuildRuntimeMemoryMap(original,size,base,&basecount)==EFI_SUCCESS);
  CHECK(FdtOpenInto(original,tree,(INT32)capacity)==0);
  const char *names[]={"tkcore","ssmr-reserved-cma_memory","ccci-dpmaif-cache-memory","ccci-dpmaif-nocache-memory"};
  const UINT64 sizes[]={0x1000000,0x10000000,0x50000,0x20000};
  const UINT64 alignment[]={0x1000,0x1000000,0x10000,0x10000};
  for (unsigned i=0;i<4;i++) {
    INT32 root=FdtPathOffset(tree,"/reserved-memory"); CHECK(root>=0);
    INT32 node=FdtAddSubnode(tree,root,names[i]); CHECK(node>=0);
    SizeCells(tree,node,"size",sizes[i]); SizeCells(tree,node,"alignment",alignment[i]);
    if (i!=1) {
      UINT8 range[16]={0,0,0,0,0x40,0,0,0,0,0,0,0,0x80,0,0,0};
      if (i>1) range[12]=0xf0;
      CHECK(FdtSetProp(tree,node,"alloc-ranges",range,sizeof(range))==0);
    }
    if (i!=2) CHECK(FdtSetPropEmpty(tree,node,i==1?"reusable":"no-map")==0);
  }
  /* Size-only pools are OS requests, not already occupied physical memory. */
  memcpy(saved,tree,capacity);
  CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_SUCCESS && count==basecount);
  CHECK(memcmp(base+initial,r+initial,(count-initial)*sizeof(*r))==0);
  CHECK(memcmp(saved,tree,capacity)==0);
  /* GOP and SEC must also accept the same four real OS pool requests together. */
  CheckVideoMemory(tree,capacity);

  /* Each malformed request must fail atomically without exposing extra RAM. */
  for (unsigned bad=0;bad<15;bad++) {
    memcpy(tree,saved,capacity);
    INT32 node=FdtPathOffset(tree,"/reserved-memory/tkcore"); CHECK(node>=0);
    if (bad==0) CHECK(FdtSetPropEmpty(tree,node,"size")==0);
    if (bad==1) SizeCells(tree,node,"size",0);
    if (bad==2) SizeCells(tree,node,"size",PA_LIMIT+1);
    if (bad==3) Cell(tree,node,"alignment",0x1000);
    if (bad==4) SizeCells(tree,node,"alignment",0);
    if (bad==5) SizeCells(tree,node,"alignment",0x3000);
    if (bad==6) SizeCells(tree,node,"alignment",PA_LIMIT*2);
    if (bad==7) CHECK(FdtSetPropEmpty(tree,node,"alloc-ranges")==0);
    if (bad==8) Cell(tree,node,"alloc-ranges",1);
    if (bad==9 || bad==10 || bad==11) {
      UINT8 range[16]={0,0,0,0x0f,0xff,0xff,0xf0,0,0,0,0,0,0,0,0x20,0};
      if (bad==10) memset(range+8,0,8);
      if (bad==11) range[3]=16;
      CHECK(FdtSetProp(tree,node,"alloc-ranges",range,sizeof(range))==0);
    }
    if (bad==12) Cell(tree,node,"no-map",0);
    if (bad==13) CHECK(FdtSetPropEmpty(tree,node,"reusable")==0);
    if (bad==14) CHECK(FdtSetPropEmpty(tree,node,"reg")==0);
    count=initial;
    CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_COMPROMISED_DATA && count==initial);
  }
  memcpy(tree,saved,capacity);
  INT32 node=FdtPathOffset(tree,"/reserved-memory/tkcore"); CHECK(node>=0);
  /* A fixed reg takes precedence over stale size/allocation constraints. */
  UINT8 reg[16]={0,0,0,0,0x90,0,0,0,0,0,0,0,0,0,0x20,0};
  CHECK(FdtSetProp(tree,node,"reg",reg,sizeof(reg))==0);
  CHECK(FdtSetPropEmpty(tree,node,"size")==0);
  CHECK(FdtSetPropEmpty(tree,node,"alignment")==0);
  CHECK(FdtSetPropEmpty(tree,node,"alloc-ranges")==0);
  count=initial; CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_SUCCESS);
  NoOverlap(r,count,0x90000000,0x90002000);
  unsigned found=0;
  for (unsigned i=initial;i<count;i++) if (r[i].Address==0x90000000) {
    CHECK(r[i].Length==0x2000 && r[i].MemoryType==EfiReservedMemoryType);
    CHECK(r[i].ResourceType==SYS_MEM && r[i].HobOption==HobOnlyNoCacheSetting); found++;
  }
  CHECK(found==1);
  free(saved); free(tree);
}

static void CheckFixedReservations(const void *original, UINTN size) {
  UINTN capacity=size+4096; UINT8 *tree=malloc(capacity); CHECK(tree);
  CHECK(FdtOpenInto(original,tree,(INT32)capacity)==0);
  INT32 root=FdtPathOffset(tree,"/reserved-memory"); CHECK(root>=0);
  INT32 node=FdtAddSubnode(tree,root,"nomap-inside-cma"); CHECK(node>=0);
  UINT8 reg[16]={0,0,0,0,0xa1,0,0,1,0,0,0,0,0,0,0x10,0};
  CHECK(FdtSetProp(tree,node,"reg",reg,sizeof(reg))==0);
  CHECK(FdtSetPropEmpty(tree,node,"no-map")==0);
  EFI_MEMORY_REGION_DESCRIPTOR r[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINT8 count=ARRAY_SIZE(mBootstrap),initial=count;
  CHECK(BuildRuntimeMemoryMap(tree,capacity,r,&count)==EFI_SUCCESS);
  unsigned cma=0,aee=0,ccci=0,md1=0;
  UINT64 total=0;
  for (unsigned i=initial;i<count;i++) {
    CHECK(r[i].ResourceType==SYS_MEM); total+=r[i].Length;
    for (unsigned j=initial;j<i;j++) CHECK(r[j].Address+r[j].Length<=r[i].Address ||
        r[i].Address+r[i].Length<=r[j].Address);
    if (r[i].Address==DTB_END) {
      CHECK(r[i].Length==0x10000 && r[i].MemoryType==EfiReservedMemoryType);
      CHECK(r[i].HobOption==HobOnlyNoCacheSetting); aee++;
    }
    if (r[i].Address==0x100400000ULL) {
      CHECK(r[i].Length==0x11000 && r[i].MemoryType==EfiReservedMemoryType);
      CHECK(r[i].HobOption==HobOnlyNoCacheSetting); ccci++;
    }
    if (r[i].Address==0x82000000ULL) {
      CHECK(r[i].Length==0x04000000U &&
            r[i].MemoryType==EfiReservedMemoryType);
      CHECK(r[i].HobOption==HobOnlyNoCacheSetting); md1++;
    }
    if (r[i].Address>=0xa0000000 && r[i].Address<0xb0000000) {
      cma++;
      if (r[i].Address==0xa1000000) {
        CHECK(r[i].Length==0x2000 && r[i].MemoryType==EfiReservedMemoryType);
        CHECK(r[i].HobOption==HobOnlyNoCacheSetting);
      } else CHECK(r[i].MemoryType==EfiBootServicesData && r[i].HobOption==AddMem);
    }
  }
  CHECK(cma==3 && aee==1 && ccci==1 && md1==1);
  CHECK(total==0x180000000ULL-(DTB_END-0x40000000));
  free(tree);
}


UINT32 EFIAPI SwapBytes32(UINT32 x) { return __builtin_bswap32(x); }
static void Reg(void *tree, INT32 node, UINT64 base, UINT64 bytes) {
  UINT32 v[4]={SwapBytes32((UINT32)(base>>32)),SwapBytes32((UINT32)base),
               SwapBytes32((UINT32)(bytes>>32)),SwapBytes32((UINT32)bytes)};
  CHECK(FdtSetProp(tree,node,"reg",v,sizeof(v))==0);
}
static void *Fixture(UINTN *size) {
  UINT8 *tree=malloc(4096); CHECK(tree && FdtCreateEmptyTree(tree,4096)==0);
  Cell(tree,0,"#address-cells",2); Cell(tree,0,"#size-cells",2);
  INT32 n=FdtAddSubnode(tree,0,"memory@40000000"); CHECK(n>=0);
  CHECK(FdtSetPropString(tree,n,"device_type","memory")==0);
  UINT32 banks[8]={0,SwapBytes32(0x40000000),0,SwapBytes32(0x80000000),
                    SwapBytes32(1),0,SwapBytes32(1),0};
  CHECK(FdtSetProp(tree,n,"reg",banks,sizeof(banks))==0);
  n=FdtAddSubnode(tree,0,"reserved-memory"); CHECK(n>=0);
  Cell(tree,n,"#address-cells",2); Cell(tree,n,"#size-cells",2);
  CHECK(FdtSetPropEmpty(tree,n,"ranges")==0);
  const UINT64 ranges[3][2]={{0x50000001,0x1001},{0x60000000,0x10000000},{0xa0000000,0x10000000}};
  const char *names[3]={"unaligned@50000001","modem@60000000","cma@a0000000"};
  for (unsigned i=0;i<3;i++) {
    INT32 root=FdtPathOffset(tree,"/reserved-memory");
    n=FdtAddSubnode(tree,root,names[i]); CHECK(n>=0); Reg(tree,n,ranges[i][0],ranges[i][1]);
    CHECK(FdtSetPropEmpty(tree,n,i==2?"reusable":"no-map")==0);
  }
  n=FdtAddSubnode(tree,0,"modem"); CHECK(n>=0);
  UINT8 info[16]={0,1,0x40,0,1,0,0,0,0,0x10,0,0,2,0,0,0};
  CHECK(FdtSetProp(tree,n,"ccci,modem_info",info,sizeof(info))==0);
  CHECK(FdtPack(tree)==0); *size=FdtTotalSize(tree); return tree;
}
static void CheckNamedFramebufferContainer(void) {
  UINT8 Tree[16384];EFI_MEMORY_REGION_DESCRIPTOR R[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  LkIo=malloc(sizeof(*LkIo));CHECK(LkIo);
  for (UINTN Kind=0;Kind<8;Kind++) {
    LkFixtureTree(Tree,sizeof(Tree));LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE);
    INT32 Root=FdtPathOffset(Tree,"/reserved-memory");
    INT32 Pool=FdtAddSubnode(Tree,Root,Kind==2?"mblock-7-other":"mblock-7-framebuffer");CHECK(Pool>=0);
    CHECK(FdtSetPropString(Tree,Pool,"compatible",Kind==1?"mediatek,other":"mediatek,framebuffer")==0);
    LkFixtureReg(Tree,Pool,Kind==3?0x90000001:Kind==6?0x8FFFF000:0x90000000,
                 Kind==5?0x10000:0x22000);
    CHECK(FdtSetPropEmpty(Tree,Pool,"no-map")==0);
    if(Kind==4) {
      UINT8 Regs[32];INT32 Length;CONST VOID *RegValue=FdtGetProp(Tree,Pool,"reg",&Length);
      CHECK(Length==16);memcpy(Regs,RegValue,16);memcpy(Regs+16,RegValue,16);
      CHECK(FdtSetProp(Tree,Pool,"reg",Regs,32)==0);
    }
    if(Kind==7) {
      INT32 Other=FdtAddSubnode(Tree,FdtPathOffset(Tree,"/reserved-memory"),"mblock-8-framebuffer");CHECK(Other>=0);
      CHECK(FdtSetPropString(Tree,Other,"compatible","mediatek,framebuffer")==0);
      LkFixtureReg(Tree,Other,0x91000000,0x10000);CHECK(FdtSetPropEmpty(Tree,Other,"no-map")==0);
    }
    UINT8 Count=ARRAY_SIZE(mBootstrap);EFI_STATUS Status=BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count);
    if(Kind==0) {
      CHECK(Status==EFI_SUCCESS && LkIo->Writes==1);NoOverlap(R,Count,0x90000000,0x90022000);
      UINTN Mapped=0;
      for(UINTN I=ARRAY_SIZE(mBootstrap);I<Count;I++)
        if(R[I].Address>=0x90000000 && R[I].Address<0x90022000) {
          CHECK(R[I].MemoryType==EfiReservedMemoryType);
          if(R[I].HobOption==AddMem){CHECK(R[I].Address==0x90000000 && R[I].Length==0x3000);Mapped++;}
        }
      CHECK(Mapped==1);
    } else {
      CHECK(Status==EFI_ACCESS_DENIED && Count==ARRAY_SIZE(mBootstrap));
      CHECK(LkIo->Writes==0 && LkIo->Reads==0 && mOverlapValid);
    }
  }
  free(LkIo);LkIo=NULL;
}
extern int fdt_add_mem_rsv(VOID *Fdt, UINT64 Address, UINT64 Size);
static void CheckDuplicateLkReservation(void) {
  UINT8 Tree[16384];EFI_MEMORY_REGION_DESCRIPTOR R[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  LkIo=malloc(sizeof(*LkIo));CHECK(LkIo);
  for (UINTN Kind=0;Kind<6;Kind++) {
    LkFixtureTree(Tree,sizeof(Tree));LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE);
    UINT64 Start=0x90000000,Size=0x20000;
    if(Kind==1){Start-=4096;Size+=4096;}
    if(Kind==2){Start+=4096;Size-=4096;}
    if(Kind==3){Start-=4096;Size=8192;}
    if(Kind==5){Start++;Size--;} // Same pages, different original bytes.
    CHECK(fdt_add_mem_rsv(Tree,Start,Size)==0);
    if(Kind==4) {
      INT32 Modem=FdtPathOffset(Tree,"/modem");
      UINT8 Info[16]={0,0,0,0x90,0,0,0,0,0,0x10,0,0,2,0,0,0};
      CHECK(FdtSetProp(Tree,Modem,"ccci,modem_info",Info,16)==0);
    }
    UINT8 Count=ARRAY_SIZE(mBootstrap);
    EFI_STATUS Status=BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count);
    if(Kind==0) {
      CHECK(Status==EFI_SUCCESS && LkIo->Writes==1 && !mOverlapValid);
      NoOverlap(R,Count,0x90000000,0x90020000);
    } else {
      CHECK(Status==EFI_ACCESS_DENIED && Count==ARRAY_SIZE(mBootstrap));
      CHECK(LkIo->Writes==0 && LkIo->Reads==0 && mOverlapValid);
      CHECK(mOverlapDetail[0]==0x90000000 && mOverlapDetail[1]==0x90020000);
      INT32 Owner=(INT32)(mOverlapDetail[4]>>32);
      CHECK(Kind==4 ? (Owner==RANGE_OWNER_CCCI_METADATA || Owner==RANGE_OWNER_MEMRESERVE) : Owner==RANGE_OWNER_MEMRESERVE);
    }
  }
  free(LkIo);LkIo=NULL;
}
static void TestCcciMemoryHandoffPublication(void) {
  T70_CCCI_MEMORY_HANDOFF Handoff;
  mCcciMemoryValid=TRUE;
  mCcciMemory.Base=0x82000000ULL;
  mCcciMemory.Size=0x04000000U;
  mCcciMemory.ImageLoadReportedOk=TRUE;
  mCcciMemory.MdGeneration=6295U;
  mCcciMemory.MdSubVersion=1U;
  mCcciMemory.ExpectedBootStatus=0x5443000CU;
  mCcciMemory.BootStatus0=mCcciMemory.BootStatus1=0U;
  mCcciMemory.BootStatusMatched=FALSE;
  CcciSmcReturn=T70_CCCI_SMC_SUCCESS;
  CcciSmcBootStatus0=0x5443000CU;
  CcciSmcBootStatus1=0x12345678U;
  CcciSmcCalls=0U;
  ZeroMem(CcciHandoffImage,sizeof(CcciHandoffImage));
  CcciHandoffWriteCount=0;
  PublishCcciMemoryHandoff(TRUE);
  CopyMem(&Handoff,CcciHandoffImage,sizeof(Handoff));
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  CHECK(Handoff.Version==T70_CCCI_MEMORY_HANDOFF_VERSION &&
        Handoff.ModemBase==0x82000000ULL &&
        Handoff.ModemBytes==0x04000000U &&
        Handoff.MdGeneration==6295U && Handoff.MdSubVersion==1U &&
        Handoff.BootStatus0==0x5443000CU &&
        Handoff.BootStatus1==0x12345678U &&
        Handoff.Flags==(T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK |
                        T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED));
  CHECK(CcciSmcCalls==1U && CcciHandoffWriteCount==13U);
  CHECK(CcciCacheWritebacks>=3U);
  CcciMapState=T70_CCCI_MEMORY_MAP_STATE_VALID;
  CHECK(LoadExistingCcciHandoff());
  CHECK(mCcciHandoffReused && !mCcciTagReadAllowed &&
        mCcciMemory.Base==0x82000000ULL &&
        mCcciMemory.BootStatusMatched &&
        mCcciMemory.BootStatus0==0x5443000CU);
  CcciMapState=T70_CCCI_MEMORY_MAP_STATE_FAILED;
  mCcciHandoffReused=FALSE;
  CHECK(!LoadExistingCcciHandoff() && !mCcciTagReadAllowed);
  CcciMapState=0U;
  mCcciHandoffReused=FALSE;
  mCcciTagReadAllowed=TRUE;
  CHECK(CcciHandoffWriteOffsets[0]==0U && CcciHandoffWriteWidths[0]==8U &&
        CcciHandoffWriteValues[0]==0U);
  CHECK(CcciHandoffWriteOffsets[CcciHandoffWriteCount-1U]==0U &&
        CcciHandoffWriteWidths[CcciHandoffWriteCount-1U]==8U &&
        CcciHandoffWriteValues[CcciHandoffWriteCount-1U]==
            T70_CCCI_MEMORY_HANDOFF_SIGNATURE);

  ZeroMem(CcciHandoffImage,sizeof(CcciHandoffImage));
  CcciHandoffWriteCount=0;
  CcciSmcBootStatus0=0U; // LK image load is clean, but the modem is not boot-ready.
  PublishCcciMemoryHandoff(TRUE);
  CopyMem(&Handoff,CcciHandoffImage,sizeof(Handoff));
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  CHECK(Handoff.Flags==T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK &&
        Handoff.BootStatus0==0U && Handoff.BootStatus1==0U &&
        CcciSmcCalls==2U);

  ZeroMem(CcciHandoffImage,sizeof(CcciHandoffImage));
  CcciHandoffWriteCount=0;
  CcciSmcReturn=(UINTN)-1;
  CcciSmcBootStatus0=0x5443000CU;
  PublishCcciMemoryHandoff(TRUE);
  CopyMem(&Handoff,CcciHandoffImage,sizeof(Handoff));
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  CHECK(Handoff.Flags==T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK &&
        Handoff.BootStatus0==0U && CcciSmcCalls==3U);

  CcciSmcCalls=0U;
  ZeroMem(CcciHandoffImage,sizeof(CcciHandoffImage));
  CcciHandoffWriteCount=0;
  PublishCcciMemoryHandoff(FALSE);
  CopyMem(&Handoff,CcciHandoffImage,sizeof(Handoff));
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff) &&
        CcciHandoffWriteCount==1U && CcciSmcCalls==0U);
  CcciSmcReturn=T70_CCCI_SMC_SUCCESS;
  CcciSmcBootStatus0=0x5443000CU;
  CcciSmcBootStatus1=0U;
}
static void CheckLkMemory(void) {
  UINT8 Tree[16384]; EFI_MEMORY_REGION_DESCRIPTOR R[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINT8 Count=ARRAY_SIZE(mBootstrap); LkIo=malloc(sizeof(*LkIo)); CHECK(LkIo);
  LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE);
  INT32 Soc=FdtAddSubnode(Tree,0,"soc"); CHECK(Soc>=0);
  INT32 Md=FdtAddSubnode(Tree,Soc,"mddriver"); CHECK(Md>=0);
  CHECK(FdtSetPropString(Tree,Md,"compatible","mediatek,mddriver")==0);
  LkFixtureCell(Tree,Md,"mediatek,md-generation",6295U);
  LkFixtureCell(Tree,Md,"mediatek,md-sub-version",1U);
  CHECK(BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count)==EFI_SUCCESS);
  CHECK(mCcciMemoryValid && mCcciMemory.Base==0x82000000ULL &&
        mCcciMemory.Size==0x04000000U && mCcciMemory.ImageLoadReportedOk &&
        mCcciMemory.MdGeneration==6295U && mCcciMemory.MdSubVersion==1U &&
        mCcciMemory.ExpectedBootStatus==0x5443000CU);
  TestCcciMemoryHandoffPublication();
  CHECK(LkIo->Writes==1); NoOverlap(R,Count,0x90000000,0x90020000);
  UINTN Mapped=0;
  for (UINTN I=ARRAY_SIZE(mBootstrap);I<Count;I++) if (R[I].HobOption==AddMem && R[I].MemoryType==EfiReservedMemoryType) {
    CHECK(R[I].Address==0x90000000 && R[I].Length==0x3000 && R[I].ArmAttributes==UNCACHED_UNBUFFERED); ++Mapped;
  }
  CHECK(Mapped==1);

  /* A later DXE-linked copy must reuse the early handoff and never reread
     the HobOnlyNoCacheSetting LK tag block after MMU enable. */
  CcciSmcReturn=T70_CCCI_SMC_SUCCESS;
  CcciSmcBootStatus0=0x5443000CU;
  CcciSmcBootStatus1=0U;
  CcciMapState=0U;
  mCcciHandoffReused=FALSE;
  mCcciTagReadAllowed=TRUE;
  PublishCcciMemoryHandoff(TRUE);
  MarkCcciMemoryMapState(T70_CCCI_MEMORY_MAP_STATE_VALID);
  CCCI_TAG_READER SavedReader=mCcciTagReader;
  mCcciTagReader=RejectCcciTestTagRead;
  CcciTagReaderCalls=0U;
  mCcciHandoffReused=FALSE;
  CHECK(LoadExistingCcciHandoff());
  TestMmuEnabled=TRUE;
  LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE);
  Count=ARRAY_SIZE(mBootstrap);
  CHECK(BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count)==EFI_SUCCESS);
  CHECK(CcciTagReaderCalls==0U && mCcciHandoffReused);
  TestMmuEnabled=FALSE;
  mCcciTagReader=SavedReader;
  CcciMapState=0U;
  mCcciHandoffReused=FALSE;
  mCcciTagReadAllowed=TRUE;

  LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE); LkIo->Spm[0x16C/4]=0; Count=ARRAY_SIZE(mBootstrap);
  CHECK(BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count)==EFI_SUCCESS);
  CHECK(LkIo->Writes==0 && LkIo->DisplayReads==0); NoOverlap(R,Count,0x90000000,0x90020000);
  for (UINTN I=ARRAY_SIZE(mBootstrap);I<Count;I++) if (R[I].MemoryType==EfiReservedMemoryType)
    CHECK(R[I].HobOption==HobOnlyNoCacheSetting);
  free(LkIo); LkIo=NULL;
}
static void AddStandardLkFramebuffer(VOID *Tree) {
  INT32 Root=FdtPathOffset(Tree,"/reserved-memory"); CHECK(Root>=0);
  INT32 Pool=FdtAddSubnode(Tree,Root,"framebuffer@90000000"); CHECK(Pool>=0);
  LkFixtureReg(Tree,Pool,0x90000000,0x3000); LkFixtureCell(Tree,Pool,"phandle",7);
  CHECK(FdtSetPropEmpty(Tree,Pool,"no-map")==0);
  INT32 Chosen=FdtPathOffset(Tree,"/chosen"); CHECK(Chosen>=0);
  LkFixtureCell(Tree,Chosen,"#address-cells",2); LkFixtureCell(Tree,Chosen,"#size-cells",2);
  INT32 Fb=FdtAddSubnode(Tree,Chosen,"framebuffer@90000000"); CHECK(Fb>=0);
  CHECK(FdtSetPropString(Tree,Fb,"compatible","simple-framebuffer")==0);
  CHECK(FdtSetPropString(Tree,Fb,"format","x8r8g8b8")==0);
  LkFixtureReg(Tree,Fb,0x90000000,0x3000);
  LkFixtureCell(Tree,Fb,"width",64); LkFixtureCell(Tree,Fb,"height",32);
  LkFixtureCell(Tree,Fb,"stride",260); LkFixtureCell(Tree,Fb,"memory-region",7);
}
extern INT32 fdt_add_mem_rsv(VOID *Tree,UINT64 Base,UINT64 Size);
static void CheckFrameBufferCollisions(void) {
  UINT8 Tree[16384]; EFI_MEMORY_REGION_DESCRIPTOR R[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  LkIo=malloc(sizeof(*LkIo)); CHECK(LkIo);
  for (UINTN Standard=0;Standard<2;Standard++) for (UINTN Source=0;Source<5;Source++) {
    LkFixtureTree(Tree,sizeof(Tree)); LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE);
    if (Standard) AddStandardLkFramebuffer(Tree);
    if (Source==0) {
      INT32 N=FdtPathOffset(Tree,"/modem"); CHECK(N>=0);
      UINT8 Info[16]={0,0x10,0,0x90,0,0,0,0,0,0x10,0,0,2,0,0,0};
      CHECK(FdtSetProp(Tree,N,"ccci,modem_info",Info,sizeof(Info))==0);
    } else if (Source!=2) {
      INT32 Root=FdtPathOffset(Tree,"/reserved-memory"); CHECK(Root>=0);
      INT32 N=FdtAddSubnode(Tree,Root,"private@90001000"); CHECK(N>=0);
      LkFixtureReg(Tree,N,0x90001001,0x1000);
      if(Source==1) CHECK(FdtSetPropEmpty(Tree,N,"no-map")==0);
      if(Source==4) CHECK(FdtSetPropEmpty(Tree,N,"reusable")==0);
    } else CHECK(fdt_add_mem_rsv(Tree,0x90001001,0x1000)==0);
    UINT8 Count=ARRAY_SIZE(mBootstrap);
    EFI_STATUS Status=BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count);
    if(Status!=EFI_ACCESS_DENIED) fprintf(stderr,"Synthetic framebuffer/private collision source=%u standard=%u status=%llx writes=%u\n",(unsigned)Source,(unsigned)Standard,(unsigned long long)Status,(unsigned)LkIo->Writes);
    CHECK(Status==EFI_ACCESS_DENIED && Count==ARRAY_SIZE(mBootstrap));
    CHECK(LkIo->Reads==0 && LkIo->Writes==0);
  }
  // A no-map pool explicitly referenced by this framebuffer remains supported.
  LkFixtureTree(Tree,sizeof(Tree)); AddStandardLkFramebuffer(Tree);
  LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE);
  UINT8 Count=ARRAY_SIZE(mBootstrap);
  CHECK(BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count)==EFI_SUCCESS);
  CHECK(LkIo->Reads==0 && LkIo->Writes==0);
  INT32 Fb=FdtPathOffset(Tree,"/chosen/framebuffer@90000000"); CHECK(Fb>=0);
  CHECK(FdtDelProp(Tree,Fb,"memory-region")==0);
  Count=ARRAY_SIZE(mBootstrap);
  CHECK(BuildRuntimeMemoryMap(Tree,sizeof(Tree),R,&Count)==EFI_ACCESS_DENIED && Count==ARRAY_SIZE(mBootstrap));
  CHECK(LkIo->Reads==0 && LkIo->Writes==0);
  free(LkIo); LkIo=NULL;
}
static void CheckSmallRamTuples(void) {
  UINT8 Tree[16384]; LkFixtureTree(Tree,sizeof(Tree));
  INT32 N=FdtPathOffset(Tree,"/memory@80000000"); CHECK(N>=0);
  RAM_RANGE R[4], Before[4];
  for(UINTN Existing=0;Existing<2;Existing++) for(UINTN Kind=0;Kind<3;Kind++) {
    memset(R,0xA5,sizeof(R)); memcpy(Before,R,sizeof(R)); UINTN Count=Existing;
    LkFixtureReg(Tree,N,0x90000000+(Kind==2?1:0),Kind==0?1:0xFFF);
    CHECK(ReadReg(Tree,N,2,2,R,&Count,EfiConventionalMemory)==EFI_SUCCESS);
    CHECK(Count==Existing && memcmp(R,Before,sizeof(R))==0);
  }
  memset(R,0xA5,sizeof(R)); UINTN Count=0;
  LkFixtureReg(Tree,N,0x90000001,0x2000);
  CHECK(ReadReg(Tree,N,2,2,R,&Count,EfiConventionalMemory)==EFI_SUCCESS && Count==1);
  CHECK(R[0].Start==0x90001000 && R[0].End==0x90002000 && R[0].Owner==N);
  Count=0; LkFixtureReg(Tree,N,0x90000001,1);
  CHECK(ReadReg(Tree,N,2,2,R,&Count,EfiReservedMemoryType)==EFI_SUCCESS && Count==1);
  CHECK(R[0].Start==0x90000000 && R[0].End==0x90001000 && R[0].Owner==N);
}
int main(void) {
  WriteCcciTags(CcciTags,16U);
  mCcciTagReader=ReadCcciTestTags;
  TestCcciTagParser();
  WriteCcciTags(CcciTags,16U);
  CHECK(sizeof(EFI_MEMORY_REGION_DESCRIPTOR)==72);
  CHECK(OFFSET_OF(EFI_MEMORY_REGION_DESCRIPTOR,ResourceAttribute)==56);
  CHECK(OFFSET_OF(EFI_MEMORY_REGION_DESCRIPTOR,MemoryType)==60);
  CHECK(OFFSET_OF(EFI_MEMORY_REGION_DESCRIPTOR,ArmAttributes)==64);
  UINTN size; UINT8 *dtb=Fixture(&size), *original=malloc(size); CHECK(original);
  memcpy(original,dtb,size);
  UINTN KeypadMappings=0;
  /* Stock DT resources are independently checked by verify-acpi.py. */
  for (UINTN I=0;I<ARRAY_SIZE(mBootstrap);I++) {
    CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&mBootstrap[I];
    if (R->Address==0x10010000) {
      KeypadMappings++;
      CHECK(R->Length==0x1000 && R->HobOption==AddDev &&
            R->ResourceType==MMAP_IO && R->ResourceAttribute==UNCACHEABLE &&
            R->MemoryType==MmIO && R->ArmAttributes==NS_DEVICE);
    }
  }
  CHECK(KeypadMappings==1);
  EFI_MEMORY_REGION_DESCRIPTOR regions[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINT8 count=ARRAY_SIZE(mBootstrap), initial=count;
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_SUCCESS && count==initial+13);
  CHECK(memcmp(dtb,original,size)==0);
  NoOverlap(regions,count,0,DTB_END);
  NoOverlap(regions,count,0x50000000,0x50002000);
  NoOverlap(regions,count,0x60000000,0x70000000);
  NoOverlap(regions,count,0xa0000000,0xb0000000);
  NoOverlap(regions,count,0x100400000ULL,0x100411000ULL);
  NoOverlap(regions,count,0x82000000ULL,0x86000000ULL);
  UINTN MdReserved=0U;
  for (UINTN I=initial;I<count;I++) {
    if (regions[I].Address==0x82000000ULL) {
      CHECK(regions[I].Length==0x04000000ULL &&
            regions[I].MemoryType==EfiReservedMemoryType);
      MdReserved++;
    }
  }
  CHECK(MdReserved==1U);
  UINT64 bytes=0;
  for (unsigned n=initial;n<count;n++) if (regions[n].MemoryType==EfiConventionalMemory)
    bytes+=regions[n].Length;
  CHECK(bytes==0x180000000ULL-(DTB_END+0x10000-0x40000000)-0x2000-0x10000000-0x10000000-0x11000-0x04000000);
  count=initial; dtb[0]=0;
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_COMPROMISED_DATA && count==initial);
  memcpy(dtb,original,size); INT32 len;
  UINT8 *cells=(UINT8 *)FdtGetProp(dtb,0,"#address-cells",&len); CHECK(cells && len==4); cells[3]=3;
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_UNSUPPORTED && count==initial);
  memcpy(dtb,original,size);
  INT32 node=FdtPathOffset(dtb,"/modem"); CHECK(node>=0);
  UINT8 *info=(UINT8 *)FdtGetProp(dtb,node,"ccci,modem_info",&len); CHECK(info && len==16);
  memset(info,0,8);
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_NOT_READY && count==initial);
  memcpy(dtb,original,size);
  info=(UINT8 *)FdtGetProp(dtb,node,"ccci,modem_info",&len); info[10]=2;
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_NOT_READY && count==initial);
  memcpy(dtb,original,size); node=FdtPathOffset(dtb,"/memory@40000000"); CHECK(node>=0);
  UINT8 *reg=(UINT8 *)FdtGetProp(dtb,node,"reg",&len); CHECK(reg && len==32);
  memset(reg+8,0xff,8);
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_COMPROMISED_DATA && count==initial);
  memcpy(dtb,original,size); reg=(UINT8 *)FdtGetProp(dtb,node,"reg",&len);
  memcpy(reg+16,reg,8);
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_COMPROMISED_DATA && count==initial);
  memcpy(dtb,original,size); count=127;
  CHECK(BuildRuntimeMemoryMap(dtb,size,regions,&count)==EFI_OUT_OF_RESOURCES && count==127);
  CHECK(BuildRuntimeMemoryMap(NULL,size,regions,&count)==EFI_INVALID_PARAMETER);
  CheckVideoMemory(original,(UINTN)size);
  CheckDynamicMemory(original,(UINTN)size);
  CheckFixedReservations(original,(UINTN)size);
  CheckLkMemory();
  CheckDuplicateLkReservation();
  CheckNamedFramebufferContainer();
  CheckFrameBufferCollisions();
  CheckSmallRamTuples();
  free(original); free(dtb);
  printf("RUNTIME RAM: %u checks passed (OS dynamic pools, fixed UEFI reservations, no-map/CMA overlaps, preloader/modem data and GOP)\n",Checks);
}
