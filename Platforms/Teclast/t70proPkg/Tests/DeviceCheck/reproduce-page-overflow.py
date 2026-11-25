#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""OPT-IN renderer acceptance after visual-row wiring. Reproduce current Page using a host console model, not a fix acceptance test.
Run: python3 Platforms/Teclast/t70proPkg/Tests/DeviceCheck/reproduce-page-overflow.py
Requires cc. Extracts current production Page verbatim; only Print/console are
mocked. ASCII payloads model one cell per CHAR16 and final-column autowrap.
"""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[2] / "Applications/DeviceCheck/T70DeviceCheck.c"
text = source.read_text()
start = text.index("STATIC UINTN mPageColumns")
end = text.index("\nEFI_STATUS EFIAPI UefiMain", start)
page = text[start:end]
pre = r"""
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <assert.h>
#include <stdint.h>
#include "VisualRows.h"
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
typedef unsigned long UINTN;
typedef uint16_t CHAR16;
#define STATIC static
#define VOID void
#define MIN(a,b) ((a)<(b)?(a):(b))
static CHAR16 mDeviceCheckLog[8192];
static UINTN mLines[64],mLineCount,mDeviceCheckLogChars;
static int mTruncated;
static unsigned column,row,scrolls,width=80,height=25;
static void emit(const wchar_t *s){
 for(;*s;s++){
  if(*s==13)column=0;
  else if(*s==10)row++;
  else if(++column==width){column=0;row++;}
  if(row>=height){row=height-1;scrolls++;}
 }
}
static void Print(const wchar_t *fmt,...){
 va_list a;va_start(a,fmt);
 if(fmt[0]==37){emit(va_arg(a,wchar_t*));if(fmt[2])emit(L"\r\n");}
 else emit(fmt);
 va_end(a);
}
struct Console {void (*ClearScreen)(struct Console*);};
static void clear(struct Console*c){(void)c;row=column=scrolls=0;}
static struct Console con={clear};
static struct {struct Console *ConOut;} system_table={&con},*gST=&system_table;
"""
post = r"""
static void fixture(unsigned length){
 mLineCount=19;mDeviceCheckLogChars=0;
 for(unsigned i=0;i<19;i++){
  mLines[i]=mDeviceCheckLogChars;
  for(unsigned j=0;j<length;j++)mDeviceCheckLog[mDeviceCheckLogChars++]=65;
  mDeviceCheckLog[mDeviceCheckLogChars++]=13;
  mDeviceCheckLog[mDeviceCheckLogChars++]=10;
 }
 mDeviceCheckLog[mDeviceCheckLogChars]=0;
 Page(0,19);
}
int main(void){
 fixture(79);assert(scrolls==0);
 unsigned failed=0; fixture(80);failed|=(scrolls!=0);printf("CURRENT PAGE 80 columns: exact-width lines scroll %u rows\n",scrolls);
 fixture(160);failed|=(scrolls!=0);printf("CURRENT PAGE 80 columns: 160-character lines scroll %u rows\n",scrolls);
 if(failed) puts("FAIL: intended pagination contract is zero scrolling; content tails must remain navigable");
 return failed ? 1 : 0;
}
"""
with tempfile.TemporaryDirectory(prefix="t70-page-repro-") as tmp:
    c=Path(tmp)/"page.c";exe=Path(tmp)/"page"
    c.write_text(pre+page+post)
    subprocess.run(["cc","-fshort-wchar","-I"+str(source.parent),str(source.parent/"VisualRows.c"),"-Wall","-Wextra","-Werror",str(c),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
