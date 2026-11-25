// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define STATIC static
#define CONST const
#define VOID void
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef uint16_t CHAR16, EFI_STRING_ID;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef void *EFI_HII_HANDLE;
#define ZeroMem(p,n) memset(p,0,n)
static unsigned Bad,Torn,SequenceReads,Sets,Creates,Allocated,FailSet,FailCreate;
static UINT32 Values[5]={3,1,2,7,0};
static CHAR16 Strings[7][20];
static UINT64 MmioRead64(UINTN a) {
 if(a==0x43F7F000)return Bad==1?0:0x0000000148303754ULL;
 assert(a==0x43F7F800);return Bad==2?0:0x000000074C443754ULL;
}
static UINT32 MmioRead32(UINTN a) {
 if(a==0x43F7F808)return Bad==3?0:0x43F7F060;
 if(a==0x43F7F80C)return Bad==4?1:0;
 if(a==0x43F7F814)return 4+(Torn && SequenceReads++!=0);
 UINTN offsets[]={0x818,0x81c,0x82c,0x840,0x844};
 for(unsigned i=0;i<5;i++)if(a==0x43F7F000+offsets[i])return Values[i];
 assert(0);return 0;
}
static void MemoryFence(void) {}
static EFI_STRING_ID HiiSetString(EFI_HII_HANDLE h,EFI_STRING_ID id,CHAR16 *s,void *lang) {
 assert(h && !lang);Sets++;if(Sets==FailSet)return 0;
 if(!id)id=++Allocated;assert(id<=6);
 UINTN i=0;do{assert(i<20);Strings[id][i]=s[i];}while(s[i++]);return id;
}
static void *HiiCreateTextOpCode(void *h,EFI_STRING_ID prompt,EFI_STRING_ID help,EFI_STRING_ID text) {
 assert(h && prompt && !help && !text);Creates++;
 return Creates==FailCreate?NULL:h;
}
#include "../../../../Mu_Basecore/MdeModulePkg/Application/UiApp/T70BootDiagnostic.h"
static void Reset(void){Bad=Torn=SequenceReads=Sets=Creates=Allocated=FailSet=FailCreate=0;ResetT70DiagnosticStrings();memset(Strings,0,sizeof Strings);}
static void CheckText(unsigned id,const CHAR16 *expected) {
 UINTN i=0;do{assert(Strings[id][i]==expected[i]);}while(expected[i++]);
}
int main(void) {
 Reset();AppendT70BootDiagnostic(NULL,(void*)2);AppendT70BootDiagnostic((void*)1,NULL);assert(!Sets&&!Creates);
 for(Bad=1;Bad<=4;Bad++){AppendT70BootDiagnostic((void*)1,(void*)2);assert(!Sets&&!Creates);}Bad=0;
 Torn=1;AppendT70BootDiagnostic((void*)1,(void*)2);assert(!Sets&&!Creates);Torn=0;
 AppendT70BootDiagnostic((void*)1,(void*)2);assert(Sets==6&&Creates==6&&Allocated==6);
 CheckText(1,L"T70 D1 (snapshot)");CheckText(2,L"I=0003");CheckText(3,L"O=0001");
 CheckText(4,L"F=0002");CheckText(5,L"P=00000007");CheckText(6,L"E=00000000");
 for(unsigned repeat=0;repeat<10;repeat++){AppendT70BootDiagnostic((void*)1,(void*)2);assert(Allocated==6);}
 Values[0]=0x10000;Values[3]=4;Values[4]=0x10001;
 AppendT70BootDiagnostic((void*)1,(void*)2);CheckText(2,L"I=FFFF");CheckText(5,L"P=00000004");CheckText(6,L"E=00010001");
 for(unsigned failure=1;failure<=6;failure++){
  Reset();FailSet=failure;AppendT70BootDiagnostic((void*)1,(void*)2);assert(!Creates);
  FailSet=0;AppendT70BootDiagnostic((void*)1,(void*)2);assert(Creates==6&&Allocated==6);
  Reset();FailCreate=failure;AppendT70BootDiagnostic((void*)1,(void*)2);assert(Creates==failure&&Allocated==6);
 }
 Reset();for(unsigned i=0;i<6;i++)assert(mT70DiagnosticTokens[i]==0);
 puts("T70 HII diagnostic: validated/torn snapshot, six read-only texts, token reuse/reset, string/opcode failure paths passed");return 0;
}
