// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the production installer against fault-injected boot-service/FV mocks.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define IN
#define OUT
#define EFIAPI
#define VOID void
#define ASSERT assert
#define ASSERT_EFI_ERROR(s) assert(!EFI_ERROR(s))
#define OFFSET_OF(t,f) offsetof(t,f)
#define DEBUG(x) ((void)0)
#define DEBUG_ERROR 1
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 1
#define EFI_DEVICE_ERROR 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_ABORTED 4
#define EFI_COMPROMISED_DATA 5
#define EFI_REQUEST_UNLOAD_IMAGE 6
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
#define EFI_SECTION_RAW 0x19
#define ByProtocol 0
#define EfiBootServicesData 4
#define PcdGetPtr(x) (&StorageGuid)
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef ptrdiff_t INTN;
typedef int EFI_STATUS;
typedef void *EFI_HANDLE;
typedef int EFI_SYSTEM_TABLE;
typedef uint8_t EFI_FV_FILETYPE;
typedef uint32_t EFI_FV_FILE_ATTRIBUTES;
typedef struct { uint8_t Data[16]; } EFI_GUID;
typedef struct { UINT32 Signature, Length; UINT8 Revision,Checksum; UINT8 Rest[26]; } EFI_ACPI_DESCRIPTION_HEADER;
typedef EFI_ACPI_DESCRIPTION_HEADER EFI_ACPI_COMMON_HEADER;
typedef struct ACPI EFI_ACPI_TABLE_PROTOCOL;
struct ACPI {
 EFI_STATUS (*InstallAcpiTable)(EFI_ACPI_TABLE_PROTOCOL *,VOID *,UINTN,UINTN *);
 EFI_STATUS (*UninstallAcpiTable)(EFI_ACPI_TABLE_PROTOCOL *,UINTN);
};
typedef struct FV EFI_FIRMWARE_VOLUME2_PROTOCOL;
struct FV {
 EFI_STATUS (*ReadFile)(EFI_FIRMWARE_VOLUME2_PROTOCOL *,EFI_GUID *,VOID **,UINTN *,EFI_FV_FILETYPE *,EFI_FV_FILE_ATTRIBUTES *,UINT32 *);
 EFI_STATUS (*ReadSection)(EFI_FIRMWARE_VOLUME2_PROTOCOL *,EFI_GUID *,UINT8,UINTN,VOID **,UINTN *,UINT32 *);
};
struct BS {
 EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID *,VOID *,UINTN *,EFI_HANDLE **);
 EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);
 EFI_STATUS (*FreePool)(VOID *);
 EFI_STATUS (*LocateProtocol)(EFI_GUID *,VOID *,VOID **);
 EFI_STATUS (*AllocatePool)(int,UINTN,VOID **);
};
static EFI_GUID StorageGuid,gEfiAcpiTableProtocolGuid,gEfiFirmwareVolume2ProtocolGuid;
static struct BS *gBS;
static UINT8 CalculateCheckSum8(UINT8 *p,UINTN n) { UINT8 s=0; while(n--) s+=*p++; return (UINT8)-s; }
#include ACPI_PLATFORM_SOURCE
static unsigned PoisonReadFailure,PoisonAllocFailure,InvalidFrees;
static unsigned FvHandleCount,HandleCalls,HandleFailures;
static unsigned Count,ReadFail,InstallFail,PublishFail,Malformed,AllocFail,UninstallFail;
static unsigned Installs,Live,Allocs,Frees,AllocCalls,Uninstalls,Order[16];
static unsigned RawChecksum,LongTable,ExactSize;
static unsigned LiveMask,UninstallFailMask;
static EFI_STATUS InstallFailureStatus,ReadFailureStatus;
static unsigned ReadCalls,ReadIndices[16];
static EFI_STATUS Pool(int type,UINTN n,VOID **p) {
 (void)type; ++AllocCalls; if(AllocFail==AllocCalls){*p=PoisonAllocFailure?(VOID *)(uintptr_t)0xdead:NULL;return EFI_OUT_OF_RESOURCES;}
 *p=calloc(1,n); if(!*p)return EFI_OUT_OF_RESOURCES; ++Allocs;return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p){if(p==(VOID *)(uintptr_t)0xdead){++InvalidFrees;return EFI_DEVICE_ERROR;}if(p){++Frees;free(p);}return EFI_SUCCESS;}
static EFI_STATUS ReadFile(EFI_FIRMWARE_VOLUME2_PROTOCOL *f,EFI_GUID *g,VOID **p,UINTN *s,EFI_FV_FILETYPE *t,EFI_FV_FILE_ATTRIBUTES *a,UINT32 *v){return EFI_SUCCESS;}
static EFI_STATUS Section(EFI_FIRMWARE_VOLUME2_PROTOCOL *f,EFI_GUID *g,UINT8 ty,UINTN i,VOID **out,UINTN *size,UINT32 *v){
 assert(ReadCalls<16);ReadIndices[ReadCalls++]=(unsigned)i;
 if(i==ReadFail){if(PoisonReadFailure)*out=(VOID *)(uintptr_t)0xdead;return ReadFailureStatus;}
 if(i>=Count)return EFI_NOT_FOUND;
 *size=sizeof(EFI_ACPI_DESCRIPTION_HEADER)+(ExactSize?0:8);
 if(Malformed==4 && i==1){*out=NULL;return EFI_SUCCESS;}
 // FV allocations do not consume installer allocation-failure injection.
 *out=calloc(1,*size);assert(*out);++Allocs;
 EFI_ACPI_DESCRIPTION_HEADER *h=*out;h->Length=sizeof(*h)+(LongTable?8:0);h->Signature=0x54445353;
 h->Checksum=(UINT8)RawChecksum;h->Rest[0]=0x5a;
 // Nonzero trailing bytes must only participate when inside Header.Length.
 if(!ExactSize)memset((UINT8*)*out+sizeof(*h),0xa7,8);
 if(Malformed==1 && i==1)*size=4;
 if(Malformed==2 && i==1)h->Length=4;
 if(Malformed==3 && i==1)h->Length=(UINT32)*size+8;
 if(Malformed==5 && i==1)*size=0;
 if(Malformed==6 && i==1)h->Length=0;
 if(Malformed==7 && i==1)h->Length=UINT32_MAX;
 if(Malformed==8 && i==1)h->Length=sizeof(*h)-1;
 if(Malformed==9 && i==1)*size=sizeof(*h)-1;
 return EFI_SUCCESS;
}
static EFI_STATUS Install(EFI_ACPI_TABLE_PROTOCOL *a,VOID *p,UINTN n,UINTN *key){
 ++Installs;if(Installs==InstallFail)return InstallFailureStatus;
 assert(n==sizeof(EFI_ACPI_DESCRIPTION_HEADER)+(LongTable?8:0));assert(CalculateCheckSum8(p,n)==0);
 EFI_ACPI_DESCRIPTION_HEADER *h=p;
 assert(h->Signature==0x54445353 && h->Length==n && h->Rest[0]==0x5a);
 if(LongTable)for(UINTN i=sizeof(*h);i<n;i++)assert(((UINT8*)p)[i]==0xa7);
 *key=Installs;Live++;LiveMask|=1U<<Installs;return Installs==PublishFail?InstallFailureStatus:EFI_SUCCESS;
}
static EFI_STATUS Uninstall(EFI_ACPI_TABLE_PROTOCOL *a,UINTN key){
 Order[Uninstalls++]=(unsigned)key;assert(LiveMask&(1U<<key));
 if(key==UninstallFail || (UninstallFailMask&(1U<<key)))return EFI_DEVICE_ERROR;
 assert(Live);Live--;LiveMask&=~(1U<<key);return EFI_SUCCESS;
}
static EFI_ACPI_TABLE_PROTOCOL Acpi={Install,Uninstall};
static EFI_FIRMWARE_VOLUME2_PROTOCOL Fv={ReadFile,Section};
static EFI_STATUS Locate(EFI_GUID *g,VOID *r,VOID **p){*p=&Acpi;return EFI_SUCCESS;}
static EFI_STATUS Handles(int t,EFI_GUID *g,VOID *s,UINTN *n,EFI_HANDLE **p){*n=FvHandleCount;*p=malloc(sizeof(**p)*(*n));assert(*p);++Allocs;for(UINTN i=0;i<*n;i++)(*p)[i]=&Fv;return EFI_SUCCESS;}
static EFI_STATUS Handle(EFI_HANDLE h,EFI_GUID *g,VOID **p){
 if(HandleCalls++<HandleFailures){*p=(VOID *)(uintptr_t)0xdead;return EFI_DEVICE_ERROR;}
 *p=&Fv;return EFI_SUCCESS;
}
static struct BS Services={Handles,Handle,Free,Locate,Pool};
static void Reset(void){ReadCalls=0;memset(ReadIndices,0,sizeof(ReadIndices));ReadFailureStatus=EFI_DEVICE_ERROR;InstallFailureStatus=EFI_DEVICE_ERROR;LiveMask=UninstallFailMask=0;RawChecksum=LongTable=ExactSize=0;FvHandleCount=1;HandleCalls=HandleFailures=0;PoisonReadFailure=PoisonAllocFailure=InvalidFrees=0;Count=3;ReadFail=99;InstallFail=PublishFail=Malformed=AllocFail=UninstallFail=0;Installs=Live=Allocs=Frees=AllocCalls=Uninstalls=0;memset(Order,0,sizeof(Order));gBS=&Services;}
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} }while(0)
int main(int argc,char **argv){
 Reset();ReadFail=1;EFI_STATUS s=AcpiPlatformEntryPoint(NULL,NULL);
 if(argc>1 && !strcmp(argv[1],"--baseline")){
  printf("baseline read error: returned=%d live_tables=%u (expected failure and zero)\n",s,Live);
  CHECK(s==EFI_REQUEST_UNLOAD_IMAGE && Live==1);
  Reset();InstallFail=2;s=AcpiPlatformEntryPoint(NULL,NULL);
  printf("baseline install error: returned=%d live_tables=%u (expected zero)\n",s,Live);
  CHECK(EFI_ERROR(s) && Live==1);return 0;
 }
 Reset();FvHandleCount=2;HandleFailures=1;
 EFI_FIRMWARE_VOLUME2_PROTOCOL *Found=NULL;
 CHECK(LocateFvInstanceWithTables(&Found)==EFI_SUCCESS && Found==&Fv);
 CHECK(HandleCalls==2 && Allocs==Frees);
 Reset();FvHandleCount=2;HandleFailures=2;Found=(VOID *)(uintptr_t)0xdead;
 CHECK(LocateFvInstanceWithTables(&Found)==EFI_DEVICE_ERROR && Found==NULL);
 CHECK(HandleCalls==2 && Allocs==Frees);
 Reset();FvHandleCount=2;HandleFailures=1;s=AcpiPlatformEntryPoint(NULL,NULL);
 CHECK(s==EFI_REQUEST_UNLOAD_IMAGE && HandleCalls==2 && Live==3 && Allocs==Frees);
 Reset();FvHandleCount=2;HandleFailures=2;s=AcpiPlatformEntryPoint(NULL,NULL);
 CHECK(s==EFI_ABORTED && Installs==0 && Allocs==Frees);
 Reset();ReadFail=1;s=AcpiPlatformEntryPoint(NULL,NULL);
 CHECK(s==EFI_DEVICE_ERROR && Live==0 && Allocs==Frees);
 Reset();ReadFail=1;PoisonReadFailure=1;s=AcpiPlatformEntryPoint(NULL,NULL);
 CHECK(s==EFI_DEVICE_ERROR && InvalidFrees==0 && Live==0 && Allocs==Frees);
 Reset();s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_REQUEST_UNLOAD_IMAGE && Live==3 && Uninstalls==0 && Allocs==Frees);
 for(unsigned i=0;i<3;i++){Reset();ReadFail=i;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_DEVICE_ERROR && Live==0 && Allocs==Frees);}
 for(unsigned i=1;i<=3;i++){Reset();InstallFail=i;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_DEVICE_ERROR && Live==0 && Allocs==Frees);}
 for(unsigned i=1;i<=3;i++){Reset();PublishFail=i;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_DEVICE_ERROR && Live==0 && Allocs==Frees);}
 Reset();ReadFail=2;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(Uninstalls==2 && Order[0]==2 && Order[1]==1);
 for(unsigned i=1;i<=9;i++){Reset();Malformed=i;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_COMPROMISED_DATA && Live==0 && Allocs==Frees);}
 for(unsigned i=1;i<=3;i++){Reset();AllocFail=i;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_OUT_OF_RESOURCES && Live==0 && Allocs==Frees);}
 for(unsigned checksum=0;checksum<256;checksum++){
  for(unsigned shape=0;shape<3;shape++){
   Reset();RawChecksum=checksum;LongTable=shape==1;ExactSize=shape==2;
   s=AcpiPlatformEntryPoint(NULL,NULL);
   CHECK(s==EFI_REQUEST_UNLOAD_IMAGE && Live==3 && Uninstalls==0 && Allocs==Frees);
  }
 }
 Reset();Count=0;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_NOT_FOUND && Live==0 && Allocs==Frees);
 Reset();ReadFail=2;UninstallFail=2;s=AcpiPlatformEntryPoint(NULL,NULL);CHECK(s==EFI_DEVICE_ERROR && Live==1 && Uninstalls==2 && Allocs==Frees);
 // Bookkeeping is one fixed-size node per table, allocated before install.
 // Failure must not discard prior keys or free an undefined failed output.
 unsigned AllocationCases=0;
 for(unsigned fail=1;fail<=6;fail++){
  for(unsigned poison=0;poison<2;poison++){
   for(unsigned cleanupFailure=0;cleanupFailure<2;cleanupFailure++){
    Reset();Count=6;AllocFail=fail;PoisonAllocFailure=poison;
    if(cleanupFailure && fail>1)UninstallFail=fail-1;
    s=AcpiPlatformEntryPoint(NULL,NULL);
    CHECK(s==(UninstallFail?EFI_DEVICE_ERROR:EFI_OUT_OF_RESOURCES));
    CHECK(AllocCalls==fail && ReadCalls==fail && Installs==fail-1);
    CHECK(Uninstalls==fail-1 && InvalidFrees==0 && Allocs==Frees);
    CHECK(Live==(UninstallFail?1U:0U));
    CHECK(LiveMask==(UninstallFail?(1U<<UninstallFail):0U));
    for(unsigned i=0;i<Uninstalls;i++)CHECK(Order[i]==fail-1-i);
    AllocationCases++;
   }
  }
 }
 printf("ACPI bookkeeping: %u fixed-node allocation/rollback scenarios passed\n",AllocationCases);
 // ReadSection owns outputs only on success. A failed terminal call may
 // poison its output just like a failed intermediate call.
 unsigned EnumerationCases=0;
 for(unsigned count=0;count<=5;count++){
  Reset();Count=count;s=AcpiPlatformEntryPoint(NULL,NULL);
  CHECK(s==(count?EFI_REQUEST_UNLOAD_IMAGE:EFI_NOT_FOUND));
  CHECK(ReadCalls==count+1 && Installs==count && Live==count && Uninstalls==0);
  CHECK(Allocs==Frees);
  for(unsigned i=0;i<ReadCalls;i++)CHECK(ReadIndices[i]==i);
  EnumerationCases++;
 }
 const EFI_STATUS ReadErrors[]={EFI_DEVICE_ERROR,EFI_OUT_OF_RESOURCES,EFI_ABORTED,EFI_COMPROMISED_DATA,EFI_NOT_FOUND};
 for(unsigned pos=0;pos<5;pos++){
  for(unsigned error=0;error<sizeof(ReadErrors)/sizeof(ReadErrors[0]);error++){
   for(unsigned poison=0;poison<2;poison++){
    Reset();Count=5;ReadFail=pos;ReadFailureStatus=ReadErrors[error];PoisonReadFailure=poison;
    s=AcpiPlatformEntryPoint(NULL,NULL);
    unsigned normalEnd=ReadFailureStatus==EFI_NOT_FOUND && pos!=0;
    CHECK(s==(normalEnd?EFI_REQUEST_UNLOAD_IMAGE:ReadFailureStatus));
    CHECK(ReadCalls==pos+1 && Installs==pos && InvalidFrees==0 && Allocs==Frees);
    CHECK(Live==(normalEnd?pos:0) && Uninstalls==(normalEnd?0:pos));
    for(unsigned i=0;i<ReadCalls;i++)CHECK(ReadIndices[i]==i);
    for(unsigned i=0;i<Uninstalls;i++)CHECK(Order[i]==pos-i);
    EnumerationCases++;
   }
  }
 }
 printf("ACPI enumeration: %u sequence/status/ownership scenarios passed\n",EnumerationCases);
 // Every rollback-failure subset, including multiple failures, must still
 // attempt all keys in reverse order and leave exactly the failed keys live.
 unsigned RollbackCases=0;
 for(unsigned published=0;published<2;published++){
  for(unsigned fail=2;fail<=5;fail++){
   unsigned installed=published?fail:fail-1;
   for(unsigned failures=0;failures<(1U<<installed);failures++){
    Reset();Count=5;InstallFailureStatus=EFI_ABORTED;
    if(published)PublishFail=fail;else InstallFail=fail;
    UninstallFailMask=failures<<1;
    s=AcpiPlatformEntryPoint(NULL,NULL);
    CHECK(s==(failures?EFI_DEVICE_ERROR:EFI_ABORTED));
    CHECK(Installs==fail && Uninstalls==installed && Allocs==Frees);
    CHECK(LiveMask==UninstallFailMask);
    unsigned expectedLive=0;
    for(unsigned i=0;i<installed;i++){
     CHECK(Order[i]==installed-i);
     expectedLive+=(failures>>i)&1U;
    }
    CHECK(Live==expectedLive);
    RollbackCases++;
   }
  }
 }
 printf("ACPI rollback: %u failure-subset scenarios passed (exact reverse order, retained keys, error precedence)\n",RollbackCases);
 puts("ACPI installer: FV lookup failures/fallback, success, read/install/allocation failures, 9 malformed/null cases, 768 checksum/size combinations, reverse rollback and rollback failure passed");return 0;
}
