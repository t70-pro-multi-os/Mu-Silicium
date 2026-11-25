#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Focused host mocks of actual console mode, scaled draw, and cursor functions.
Not a firmware build or real HII/GOP rendering test.
"""
from pathlib import Path
import subprocess, tempfile
here=Path(__file__).resolve().parent
folder=here.parents[3]/'Mu_Basecore/MdeModulePkg/Universal/Console/GraphicsConsoleDxe'
s=(folder/'GraphicsConsole.c').read_text()
def function(name, source=s):
    start=source.index('\n'+name+' (')+1
    brace=source.index('{',start); depth=1; end=brace+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    return 'EFI_STATUS\n'+source[start:end]+'\n'
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t UINTN; typedef intptr_t INTN; typedef uint32_t UINT32;
typedef uint16_t UINT16,CHAR16; typedef CHAR16* EFI_STRING; typedef int32_t INT32; typedef int EFI_STATUS;
#define VOID void
#define STATIC static
#define CONST const
#define IN
#define OUT
#define EFIAPI
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED -1
#define EFI_OUT_OF_RESOURCES -2
#define EFI_INVALID_PARAMETER -3
#define EFI_ERROR(x) ((x)<0)
#define MAX_UINTN UINTPTR_MAX
#define EFI_GLYPH_WIDTH 8
#define EFI_GLYPH_HEIGHT 19
#define EFI_WIDE_ATTRIBUTE 128
#define EFI_HII_IGNORE_IF_NO_GLYPH 1
#define EFI_HII_IGNORE_LINE_BREAK 2
#define EFI_HII_DIRECT_TO_SCREEN 4
#define BIT0 1
#define ASSERT assert
#define DEBUG_CODE_BEGIN()
#define DEBUG_CODE_END()
#define DEBUG(x)
#define CopyMem memcpy
#define ZeroMem(p,n) memset(p,0,n)
typedef struct { uint8_t Blue,Green,Red,Reserved; } EFI_GRAPHICS_OUTPUT_BLT_PIXEL;
typedef union { EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel; uint32_t Raw; } EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION;
typedef struct { UINT16 Width,Height; union { EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bitmap; void *Screen; } Image; } EFI_IMAGE_OUTPUT;
typedef struct { EFI_GRAPHICS_OUTPUT_BLT_PIXEL ForegroundColor,BackgroundColor; } EFI_FONT_DISPLAY_INFO;
typedef struct { UINTN Columns,Rows; INTN DeltaX,DeltaY; UINT32 GopWidth,GopHeight,GopModeNumber; } GRAPHICS_CONSOLE_MODE_DATA;
typedef struct { INT32 Mode,CursorColumn,CursorRow,Attribute,CursorVisible,MaxMode; } EFI_SIMPLE_TEXT_OUTPUT_MODE;
typedef struct OUTPROTO EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct OUTPROTO { EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode; EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*,CHAR16*); EFI_STATUS (*ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*); EFI_STATUS (*QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*,UINTN,UINTN*,UINTN*); };
typedef struct { UINT32 Mode; } GOP_MODE;
typedef struct GOP GOP;
struct GOP { EFI_STATUS (*Blt)(GOP*,EFI_GRAPHICS_OUTPUT_BLT_PIXEL*,int,UINTN,UINTN,UINTN,UINTN,UINTN,UINTN,UINTN); GOP_MODE *Mode; EFI_STATUS (*SetMode)(GOP*,UINT32); };
typedef GOP EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct { GOP *GraphicsOutput; GRAPHICS_CONSOLE_MODE_DATA *ModeData; EFI_GRAPHICS_OUTPUT_BLT_PIXEL *LineBuffer; } GRAPHICS_CONSOLE_DEV;
static GRAPHICS_CONSOLE_DEV Dev;
#define GRAPHICS_CONSOLE_CON_OUT_DEV_FROM_THIS(x) (&Dev)
#define EfiBltVideoToBltBuffer 1
#define EfiBltBufferToVideo 2
#define EfiBltVideoToVideo 3
#define EfiBltVideoFill 4
typedef int BOOLEAN,EFI_TPL;
#define TRUE 1
#define FALSE 0
#define TPL_NOTIFY 16
#define CHAR_NULL 0
#define CHAR_BACKSPACE 8
#define CHAR_LINEFEED 10
#define CHAR_CARRIAGE_RETURN 13
#define WIDE_CHAR 0xfff1
#define NARROW_CHAR 0xfff0
#define EFI_WARN_UNKNOWN_GLYPH 1
static CHAR16 SpaceStr[]={NARROW_CHAR,32,0},mCrLfString[]={13,10,0};
static EFI_TPL raise_tpl(EFI_TPL t){return t;}
static void restore_tpl(EFI_TPL t){(void)t;}
static struct { EFI_TPL(*RaiseTPL)(EFI_TPL); void(*RestoreTPL)(EFI_TPL); } Boot={raise_tpl,restore_tpl},*gBS=&Boot;
EFI_STATUS DrawUnicodeWeightAtCursorN(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*,CHAR16*,UINTN);
static BOOLEAN mClearScreen;
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL mGraphicsEfiColors[16];
static int SetStatus,SetCalls,ClearStatus,ClearCalls;
static EFI_STATUS set_mode(GOP*g,UINT32 n){SetCalls++;if(SetStatus<0)return SetStatus;g->Mode->Mode=n;return 0;}
static EFI_STATUS clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*t){(void)t;ClearCalls++;return ClearStatus;}
static UINTN LastAllocation;
static int Live,AllocCalls,FailAlloc,BltCalls,HiiCalls,FailRead,FailWrite,FailFill,HiiStatus,FailBltCall;
static void *AllocatePool(UINTN n) { LastAllocation=n; AllocCalls++; if(AllocCalls==FailAlloc)return NULL; void*p=malloc(n); assert(p);Live++;return p; }
static void *AllocateZeroPool(UINTN n) { void*p=AllocatePool(n);if(p)memset(p,0,n);return p; }
static void *AllocateCopyPool(UINTN n,const void*q){void*p=AllocatePool(n);if(p)memcpy(p,q,n);return p;}
static void FreePool(void*p) {assert(p);Live--;free(p);}
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL Screen[2400*2400];
static EFI_STATUS blt(GOP*g,EFI_GRAPHICS_OUTPUT_BLT_PIXEL*p,int op,UINTN sx,UINTN sy,UINTN dx,UINTN dy,UINTN w,UINTN h,UINTN delta) {
 (void)g;BltCalls++; if(BltCalls==FailBltCall)return -14; assert(delta==0||delta==w*sizeof(*p));
 if(op==EfiBltVideoFill&&FailFill)return -13;
 if(op==EfiBltVideoToBltBuffer&&FailRead)return -7;
 if(op==EfiBltBufferToVideo&&FailWrite)return -8;
 assert((op==1?sx:dx)+w<=Dev.ModeData[0].GopWidth);
 assert((op==1?sy:dy)+h<=Dev.ModeData[0].GopHeight);
 assert((op==1?sx:dx)+w<=2400);assert((op==1?sy:dy)+h<=2400);
 for(UINTN y=0;y<h;y++)for(UINTN x=0;x<w;x++) {
  if(op==1)p[y*w+x]=Screen[(sy+y)*2400+sx+x];
  else if(op==2) Screen[(dy+y)*2400+dx+x]=p[y*w+x];
  else if(op==3) Screen[(dy+y)*2400+dx+x]=Screen[(sy+y)*2400+sx+x];
  else Screen[(dy+y)*2400+dx+x]=*p;
 }
 return 0;
}
static void GetTextColors(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*t,EFI_GRAPHICS_OUTPUT_BLT_PIXEL*f,EFI_GRAPHICS_OUTPUT_BLT_PIXEL*b) { (void)t;*f=(EFI_GRAPHICS_OUTPUT_BLT_PIXEL){255,255,255,0};*b=(EFI_GRAPHICS_OUTPUT_BLT_PIXEL){0,0,0,0}; }
typedef struct HII HII;
struct HII { EFI_STATUS (*StringToImage)(HII*,int,CHAR16*,EFI_FONT_DISPLAY_INFO*,EFI_IMAGE_OUTPUT**,UINTN,UINTN,void*,void*,void*); };
static EFI_STATUS render(HII*h,int flags,CHAR16*s,EFI_FONT_DISPLAY_INFO*f,EFI_IMAGE_OUTPUT**im,UINTN x,UINTN y,void*a,void*b,void*c) {
 (void)h;(void)f;(void)a;(void)b;(void)c;HiiCalls++;
 if(flags&EFI_HII_DIRECT_TO_SCREEN) { if(!(flags==7&&x<(*im)->Width&&y<(*im)->Height))fprintf(stderr,"direct flags=%d x=%lu y=%lu w=%u h=%u\n",flags,(unsigned long)x,(unsigned long)y,(*im)->Width,(*im)->Height);assert(flags==7&&x<(*im)->Width&&y<(*im)->Height); return HiiStatus; }
 assert(flags==3&&x==0&&y==0&&s[0]);
 if(EFI_ERROR(HiiStatus))return HiiStatus;
 for(UINTN j=0;j<(*im)->Height;j++)for(UINTN i=0;i<(*im)->Width;i++)(*im)->Image.Bitmap[j*(*im)->Width+i]=(EFI_GRAPHICS_OUTPUT_BLT_PIXEL){i%251,j%251,77,0};
 return HiiStatus;
}
static HII Hii={render};static HII*mHiiFont=&Hii;
static struct { uint8_t GlyphCol1[19]; } mCursorGlyph={{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255,255}};
static GRAPHICS_CONSOLE_MODE_DATA mGraphicsConsoleModeData[]={{100,31,0,0,0,0,0},{128,40,0,0,0,0,0},{160,42,0,0,0,0,0},{240,56,0,0,0,0,0},{0}};
'''
hii_source=(folder.parents[2]/'Library/CustomizedDisplayLib/CustomizedDisplayLibInternal.c').read_text()
hii_header=(folder.parents[2]/'Library/CustomizedDisplayLib/CustomizedDisplayLibInternal.h').read_text()
# Constants must match the linked menu library, not guessed dimensions.
for key,value in [('FRONT_PAGE_HEADER_HEIGHT',6),('FOOTER_HEIGHT',4),('STATUS_BAR_HEIGHT',1)]:
    import re
    assert re.search(r'#define\s+'+key+r'\s+'+str(value)+r'\b',hii_header)
hii_pre=r'''
#define FRONT_PAGE_HEADER_HEIGHT 6
#define FOOTER_HEIGHT 4
#define STATUS_BAR_HEIGHT 1
typedef struct {UINTN LeftColumn,RightColumn,TopRow,BottomRow;} EFI_SCREEN_DESCRIPTOR;
typedef struct LIST_ENTRY {struct LIST_ENTRY *Next;} LIST_ENTRY;
static int IsListEmpty(LIST_ENTRY*p){return p->Next==p;}
static LIST_ENTRY*GetFirstNode(LIST_ENTRY*p){return p->Next;}
static LIST_ENTRY*GetNextNode(LIST_ENTRY*h,LIST_ENTRY*p){(void)h;return p->Next;}
static int IsNull(LIST_ENTRY*h,LIST_ENTRY*p){return h==p;}
typedef struct {LIST_ENTRY HotKeyListHead;EFI_SCREEN_DESCRIPTOR*ScreenDimensions;} FORM_DISPLAY_ENGINE_FORM;
static EFI_SCREEN_DESCRIPTOR gScreenDimensions;
static UINTN gFooterHeight;
static struct {EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*ConOut;} System,*gST=&System;
'''
main=r'''
int main(void) {
 GOP_MODE gm={0};GOP g={blt,&gm,set_mode};EFI_SIMPLE_TEXT_OUTPUT_MODE mode={0,1,2,0,1,2};EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL out={&mode,GraphicsConsoleConOutOutputString,clear_screen,GraphicsConsoleConOutQueryMode};
 gST->ConOut=&out;
 UINTN resolutions[][2]={{2240,1400},{1400,2240},{1200,2000},{2000,1200},{1280,950},{1279,950},{1280,949},{640,480},{1920,1080},{2400,2400}};
 for(UINTN r=0;r<sizeof(resolutions)/sizeof(*resolutions);r++) {
  UINTN w=resolutions[r][0],h=resolutions[r][1],count=0;GRAPHICS_CONSOLE_MODE_DATA*data=NULL;
  assert(InitializeGraphicsConsoleTextMode(w,h,0,&count,&data)==0&&count>=2);
  UINTN scale=ConsoleScale(w,h);
  #ifdef T70PRO_RAM_DIAGNOSTICS
  assert(scale==((w>=1280&&h>=950)?2:1));
#else
  assert(scale==1);
#endif
  assert(data[0].Columns==80&&data[0].Rows==25);
  for(UINTN i=0;i<count;i++)if(data[i].Columns&&data[i].Rows) {
   assert(data[i].Columns*8*scale+2*data[i].DeltaX<=w);
   assert(data[i].Rows*19*scale+2*data[i].DeltaY<=h);
  }
  mode.MaxMode=(INT32)count;
  Dev=(GRAPHICS_CONSOLE_DEV){&g,data,NULL}; memset(Screen,0,sizeof(Screen));
  int calls=BltCalls;
  assert(FlushCursor(&out)==0&&BltCalls==calls+2);
  UINTN x=data[0].DeltaX+8*scale,y=data[0].DeltaY+2*19*scale;
  assert(Screen[(y+16*scale)*2400+x].Blue==255);
  assert(Screen[(y+16*scale-1)*2400+x].Blue==0);
  assert(FlushCursor(&out)==0&&Screen[(y+16*scale)*2400+x].Blue==0);
  FailRead=1;calls=BltCalls;assert(FlushCursor(&out)==-7&&BltCalls==calls+1);FailRead=0;
  FailWrite=1;assert(FlushCursor(&out)==-8);FailWrite=0;
  if(scale==2) {
   CHAR16 str[]={'A','B',0};
   for(int wide=0;wide<2;wide++) {
    mode.Attribute=wide?128:0;
    assert(DrawScaledConsoleString(&out,str,2)==0);
    UINTN width=16*(wide?2:1);
    for(UINTN yy=0;yy<38;yy++)for(UINTN xx=0;xx<width*2;xx++) {
     EFI_GRAPHICS_OUTPUT_BLT_PIXEL p=Screen[(y+yy)*2400+x+xx];
     assert(p.Blue==(xx/2)%251&&p.Green==yy/2&&p.Red==77);
    }
   }
   mode.Attribute=0;
   for(int fail=1;fail<=3;fail++) { AllocCalls=0;FailAlloc=fail;calls=BltCalls;assert(DrawScaledConsoleString(&out,str,2)==-2&&BltCalls==calls&&Live==1); }
   FailAlloc=0;HiiStatus=-9;calls=BltCalls;assert(DrawScaledConsoleString(&out,str,2)==-9&&BltCalls==calls&&Live==1);
   HiiStatus=1;assert(DrawScaledConsoleString(&out,str,2)==1);HiiStatus=0;
   FailWrite=1;assert(DrawScaledConsoleString(&out,str,2)==-8&&Live==1);FailWrite=0;
   assert(DrawScaledConsoleString(&out,str,0)==0);
   assert(DrawScaledConsoleString(&out,str,MAX_UINTN)==-1&&Live==1);
   Dev.GraphicsOutput=NULL;assert(DrawScaledConsoleString(&out,str,2)==-1);Dev.GraphicsOutput=&g;
  }
  mode.CursorVisible=0;calls=BltCalls;assert(FlushCursor(&out)==0&&BltCalls==calls);mode.CursorVisible=1;
  // Exercise the actual output function's newline/scroll path, not a clone.
  mode.CursorVisible=0;mode.CursorRow=24;mode.CursorColumn=0;
  UINTN top=(UINTN)data[0].DeltaY,left=(UINTN)data[0].DeltaX;
  for(UINTN yy=0;yy<25*19*scale;yy++)for(UINTN xx=0;xx<80*8*scale;xx++)
   Screen[(top+yy)*2400+left+xx]=(EFI_GRAPHICS_OUTPUT_BLT_PIXEL){(yy/(19*scale))+1,0,0,0};
  CHAR16 newline[]={10,0};calls=BltCalls;
  assert(GraphicsConsoleConOutOutputString(&out,newline)==0&&BltCalls==calls+2);
  assert(mode.CursorRow==24&&mode.CursorColumn==0);
  for(UINTN yy=0;yy<25*19*scale;yy++) {
   unsigned expected=yy<24*19*scale?yy/(19*scale)+2:0;
   assert(Screen[(top+yy)*2400+left].Blue==expected);
   assert(Screen[(top+yy)*2400+left+80*8*scale-1].Blue==expected);
  }
  // Actual OutputString -> actual draw dispatch at all last-column boundaries.
  CHAR16 narrow[]={'A',0},wide[]={WIDE_CHAR,'B',0},mixed[]={'A',WIDE_CHAR,'B',NARROW_CHAR,'C',0};
  for(UINTN row=0;row<25;row+=24)for(UINTN col=77;col<80;col++) {
   mode.CursorRow=row;mode.CursorColumn=col;mode.Attribute=0;
   assert(GraphicsConsoleConOutOutputString(&out,narrow)==0);
   assert(mode.CursorColumn==(INT32)((col+1)%80));
   assert(mode.CursorRow==(INT32)(row+(col==79&&row<24)));
   mode.CursorRow=row;mode.CursorColumn=col;
   assert(GraphicsConsoleConOutOutputString(&out,wide)==0);
   assert(mode.CursorColumn==(INT32)(col==77?79:col==78?0:2));
   assert(mode.CursorRow==(INT32)(row+(col>=78&&row<24)));
   assert(mode.Attribute==0);
   mode.CursorRow=row;mode.CursorColumn=col;
   assert(GraphicsConsoleConOutOutputString(&out,mixed)==0);
   assert(mode.CursorColumn>=0&&mode.CursorColumn<80&&mode.CursorRow>=0&&mode.CursorRow<25&&mode.Attribute==0);
  }
  if(scale==2) {
   CHAR16 two[]={'X','Y',0};mode.CursorColumn=79;mode.CursorRow=24;calls=BltCalls;
   assert(DrawScaledConsoleString(&out,two,2)==-1&&BltCalls==calls&&Live==1);
   mode.Attribute=EFI_WIDE_ATTRIBUTE;
   assert(DrawScaledConsoleString(&out,two,1)==-1&&BltCalls==calls&&Live==1);
   mode.Attribute=0;mode.CursorColumn=78;
   assert(DrawScaledConsoleString(&out,two,2)==0&&Live==1);
   FailWrite=1;mode.CursorColumn=79;
   assert(GraphicsConsoleConOutOutputString(&out,narrow)==EFI_WARN_UNKNOWN_GLYPH&&mode.CursorColumn==0&&mode.CursorRow==24);
   FailWrite=0;
  }
  // Full-width mode, not only mandatory 80 columns, at bottom-right pixel.
  CHAR16 full[301];for(UINTN i=0;i<300;i++)full[i]='Z';full[300]=0;
  UINTN largest=0;for(UINTN i=1;i<count;i++)if(data[i].Columns&&data[i].Rows&&data[i].Columns>=data[largest].Columns)largest=i;
  mode.Mode=(INT32)largest;mode.CursorColumn=0;mode.CursorRow=data[largest].Rows-1;
  assert(data[largest].Columns<=300);
  assert(DrawUnicodeWeightAtCursorN(&out,full,data[largest].Columns)==0&&Live==1);
  mode.Mode=0;
  mode.CursorRow=24;mode.CursorColumn=79;HiiStatus=-9;
  assert(GraphicsConsoleConOutOutputString(&out,narrow)==EFI_WARN_UNKNOWN_GLYPH);
  assert(mode.CursorRow==24&&mode.CursorColumn==0);HiiStatus=0;
  CHAR16 longline[162];for(UINTN i=0;i<161;i++)longline[i]='X';longline[161]=0;
  mode.CursorRow=23;mode.CursorColumn=79;assert(GraphicsConsoleConOutOutputString(&out,longline)==0);
  assert(mode.CursorRow==24&&mode.CursorColumn==0&&Live==1);
  mode.CursorRow=2;mode.CursorColumn=1;mode.CursorVisible=1;
  calls=BltCalls;assert(GraphicsConsoleConOutSetCursorPosition(&out,80,0)==-1&&BltCalls==calls);
  assert(GraphicsConsoleConOutSetCursorPosition(&out,0,25)==-1);
  assert(GraphicsConsoleConOutSetCursorPosition(&out,MAX_UINTN,MAX_UINTN)==-1);
  assert(mode.CursorRow==2&&mode.CursorColumn==1);
  FailRead=1;assert(GraphicsConsoleConOutSetCursorPosition(&out,79,24)==-7);
  assert(mode.CursorRow==2&&mode.CursorColumn==1);FailRead=0;
  assert(GraphicsConsoleConOutSetCursorPosition(&out,79,24)==0);
  assert(mode.CursorRow==24&&mode.CursorColumn==79);
  // Failure on the new position's cursor read is reported after logical move.
  FailBltCall=BltCalls+3;
  assert(GraphicsConsoleConOutSetCursorPosition(&out,1,2)==-14);
  assert(mode.CursorRow==2&&mode.CursorColumn==1);FailBltCall=0;
  FailWrite=1;
  assert(GraphicsConsoleConOutSetCursorPosition(&out,79,24)==-8);
  assert(mode.CursorRow==2&&mode.CursorColumn==1);FailWrite=0;
  mode.CursorVisible=1;mode.CursorRow=2;mode.CursorColumn=1;
  mode.CursorVisible=0;
  UINTN columns=99,rows=99;
  assert(GraphicsConsoleConOutQueryMode(&out,count,&columns,&rows)==-1&&columns==99&&rows==99);
  assert(GraphicsConsoleConOutQueryMode(&out,MAX_UINTN,&columns,&rows)==-1);
  FORM_DISPLAY_ENGINE_FORM form={0};form.HotKeyListHead.Next=&form.HotKeyListHead;
  for(UINTN m=0;m<count;m++) {
   EFI_STATUS status=GraphicsConsoleConOutQueryMode(&out,m,&columns,&rows);
   if(!data[m].Columns||!data[m].Rows) {
    assert(status==-1);EFI_GRAPHICS_OUTPUT_BLT_PIXEL*previous=Dev.LineBuffer;INT32 previousmode=mode.Mode;
    assert(GraphicsConsoleConOutSetMode(&out,m)==-1&&Dev.LineBuffer==previous&&mode.Mode==previousmode);continue;
   }
   assert(status==0&&columns==data[m].Columns&&rows==data[m].Rows&&columns>=80&&rows>=25);
   assert(GraphicsConsoleConOutSetMode(&out,m)==0&&mode.Mode==(INT32)m);
   assert(Dev.LineBuffer&&Live==2);
   assert(ScreenDimensionInfoValidate(&form)==0);
   assert(gScreenDimensions.RightColumn==columns&&gScreenDimensions.BottomRow==rows);
   LIST_ENTRY keys[9];
   for(UINTN keycount=0;keycount<=9;keycount+=3) {
    form.HotKeyListHead.Next=keycount?&keys[0]:&form.HotKeyListHead;
    for(UINTN k=0;k<keycount;k++)keys[k].Next=k+1<keycount?&keys[k+1]:&form.HotKeyListHead;
    assert(ScreenDimensionInfoValidate(&form)==0&&gFooterHeight==FOOTER_HEIGHT+keycount/3);
    assert(rows>STATUS_BAR_HEIGHT+FRONT_PAGE_HEADER_HEIGHT+gFooterHeight+3);
   }
   form.HotKeyListHead.Next=&form.HotKeyListHead;
   // The exact queried bounds satisfy the existing HII local-screen contract.
   EFI_SCREEN_DESCRIPTOR local={0,columns,0,rows};form.ScreenDimensions=&local;
   assert(ScreenDimensionInfoValidate(&form)==0&&rows>STATUS_BAR_HEIGHT+FRONT_PAGE_HEADER_HEIGHT+gFooterHeight+3);
   local.RightColumn=columns+1;assert(ScreenDimensionInfoValidate(&form)==-3);
   local.RightColumn=columns;local.BottomRow=rows+1;assert(ScreenDimensionInfoValidate(&form)==-3);
   form.ScreenDimensions=NULL;
   mode.CursorRow=(INT32)rows-1;mode.CursorColumn=0;calls=BltCalls;
   CHAR16 lf[]={10,0};assert(GraphicsConsoleConOutOutputString(&out,lf)==0&&BltCalls==calls+2);
   assert(mode.CursorRow==(INT32)rows-1&&mode.CursorColumn==0);
  }
  if(w==2240&&h==1400&&scale==2){assert(data[0].Columns==80&&data[0].Rows==25);assert(data[largest].Columns==140&&data[largest].Rows==36);}
  if(w==1400&&h==2240&&scale==2){assert(data[1].Columns==80&&data[1].Rows==50);assert(data[largest].Columns==87&&data[largest].Rows==58);}
  FreePool(Dev.LineBuffer);Dev.LineBuffer=NULL;
  mode.Mode=0;mode.CursorRow=2;mode.CursorColumn=1;mode.CursorVisible=1;
  FreePool(data);assert(Live==0);
 }
 // SetMode owns the old line until allocation and GOP transition succeed.
 GRAPHICS_CONSOLE_MODE_DATA modes[2]={{80,25,0,0,2240,1400,0},{80,25,0,0,640,480,1}};
 Dev=(GRAPHICS_CONSOLE_DEV){&g,modes,NULL};mode.MaxMode=2;mode.Mode=-1;mode.CursorVisible=0;gm.Mode=0;mClearScreen=0;
 AllocCalls=0;FailAlloc=1;
 assert(GraphicsConsoleConOutSetMode(&out,0)==-2&&Dev.LineBuffer==NULL&&mode.Mode==-1&&Live==0);
 FailAlloc=0;SetStatus=-11;
 assert(GraphicsConsoleConOutSetMode(&out,1)==-11&&Dev.LineBuffer==NULL&&mode.Mode==-1&&Live==0);
 SetStatus=0;mClearScreen=0;
 assert(GraphicsConsoleConOutSetMode(&out,0)==0&&Live==1&&mode.Mode==0);
 assert(LastAllocation==80*8*19*sizeof(*Dev.LineBuffer)*ConsoleScale(2240,1400)*ConsoleScale(2240,1400));
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL*old=Dev.LineBuffer;
 // A failed allocation must not free the old allocation or touch GOP state.
 AllocCalls=0;FailAlloc=1;int sets=SetCalls;
 assert(GraphicsConsoleConOutSetMode(&out,1)==-2);
 assert(Live==1&&Dev.LineBuffer==old&&mode.Mode==0&&gm.Mode==0&&SetCalls==sets);
 Dev.LineBuffer[0].Blue=123; // ASan catches stale old-buffer ownership.
 FailAlloc=0;SetStatus=-11;
 assert(GraphicsConsoleConOutSetMode(&out,1)==-11);
 assert(Live==1&&Dev.LineBuffer==old&&mode.Mode==0&&gm.Mode==0&&Dev.LineBuffer[0].Blue==123);
 // Cursor read failure must leave both ownership and GOP untouched.
 SetStatus=0;mode.CursorVisible=1;FailRead=1;sets=SetCalls;
 assert(GraphicsConsoleConOutSetMode(&out,1)==-7&&SetCalls==sets&&Dev.LineBuffer==old&&Live==1&&mode.Mode==0);
 FailRead=0;
 // Failed GOP transition erases then restores the old visible cursor.
 assert(FlushCursor(&out)==0);SetStatus=-11;
 unsigned before=Screen[16*ConsoleScale(2240,1400)*2400].Blue;
 assert(GraphicsConsoleConOutSetMode(&out,1)==-11&&Live==1&&Dev.LineBuffer==old);
 assert(Screen[16*ConsoleScale(2240,1400)*2400].Blue==before);
 SetStatus=0;mode.CursorVisible=0;
 assert(GraphicsConsoleConOutSetMode(&out,1)==0&&Live==1&&mode.Mode==1&&gm.Mode==1);
 assert(LastAllocation==80*8*19*sizeof(*Dev.LineBuffer));
 old=Dev.LineBuffer;ClearStatus=-12;assert(GraphicsConsoleConOutSetMode(&out,1)==-12&&Dev.LineBuffer==old&&Live==1);ClearStatus=0;
 assert(GraphicsConsoleConOutSetMode(&out,1)==0&&Dev.LineBuffer==old&&Live==1);
 assert(GraphicsConsoleConOutSetMode(&out,2)==-1&&Dev.LineBuffer==old&&Live==1);
 assert(GraphicsConsoleConOutSetMode(&out,0)==0&&Live==1&&mode.Mode==0&&gm.Mode==0);
 // Different text mode on the same GOP: video-clear failure also rolls back.
 old=Dev.LineBuffer;modes[1].GopModeNumber=0;FailFill=1;
 assert(GraphicsConsoleConOutSetMode(&out,1)==-13&&Dev.LineBuffer==old&&Live==1&&mode.Mode==0);
 FailFill=0;
 FreePool(Dev.LineBuffer);Dev.LineBuffer=NULL;assert(Live==0);
 puts("QueryMode/HII: all generated modes, minimum 80x25, stock orientations and bottom-row scroll passed");
 puts("SetMode: allocation/GOP failure ownership, same-mode clear, 2x/1x transitions passed");
 puts("console scale: mode bounds, pixel replication, cursor roundtrip, narrow/wide edge wrap, real scrolling, cursor errors, failure cleanup passed");
}
'''
text=pre+'\n#include "ConsoleScale.h"\n'+''.join(function(n) for n in ['InitializeGraphicsConsoleTextMode','DrawScaledConsoleString','DrawUnicodeWeightAtCursorN','FlushCursor','GraphicsConsoleConOutSetCursorPosition','GraphicsConsoleConOutOutputString','GraphicsConsoleConOutSetMode','GraphicsConsoleConOutQueryMode'])+hii_pre+function('ScreenDimensionInfoValidate',hii_source)+main
with tempfile.TemporaryDirectory(prefix='t70-console-scale-') as d:
    c=Path(d)/'test.c';exe=Path(d)/'test';c.write_text(text)
    subprocess.run(['clang','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror','-DT70PRO_RAM_DIAGNOSTICS','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(folder),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    subprocess.run(['clang','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(folder),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Additional source assertions alongside actual SetMode execution.
draw=function('DrawUnicodeWeightAtCursorN')
assert 'return DrawScaledConsoleString (This, UnicodeWeight, Count);' in draw
assert 'GopHeight) == 2)' in draw
output=function('GraphicsConsoleConOutOutputString')
assert 'Height    = (MaxRow - 1) * CellHeight' in output
assert 'DeltaY + CellHeight' in output and 'Width     = MaxColumn * CellWidth' in output
assert 'CellHeight,' in output
setmode=function('GraphicsConsoleConOutSetMode')
assert setmode.count('ConsoleScale (ModeData->GopWidth, ModeData->GopHeight)')==2
print('scroll geometry and line-buffer allocation wiring assertions passed')
