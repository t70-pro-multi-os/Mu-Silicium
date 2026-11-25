// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Opt-in host test, no EFI/device access. Compile with VisualRows.c. */
#include "../../Applications/DeviceCheck/VisualRows.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 uint16_t Text[2048],Out[512];size_t N,Used,Total,Rows;T70_VISUAL_CURSOR C;
 for(unsigned Width=2;Width<=512;Width++)for(N=0;N<=1024;N++){
  for(size_t I=0;I<N;I++)Text[I]=(uint16_t)(65+I%26);
  C=(T70_VISUAL_CURSOR){0,0};Rows=Total=0;
  while(T70VisualRow(Text,N,Width,&C,Out,512,&Used)==1){
   assert(Used<Width&&Out[Used]==0);
   for(size_t I=0;I<Used;I++)assert(Out[I]==Text[Total+I]);
   Total+=Used;Rows++;assert(Rows<=N+1);
  }
  assert(Total==N&&Rows==(N+Width-2)/(Width-1));
 }
 const uint16_t Lines[]={65,13,10,13,66,10,67,13};
 C=(T70_VISUAL_CURSOR){0,0};
 for(unsigned I=0;I<4;I++){
  assert(T70VisualRow(Lines,8,80,&C,Out,512,&Used)==1);
  assert(Used==(I==1?0:1));
 }
 assert(T70VisualRow(Lines,8,80,&C,Out,512,&Used)==0);
 const uint16_t Tab[]={65,9,66};
 C=(T70_VISUAL_CURSOR){0,0};
 assert(T70VisualRow(Tab,3,4,&C,Out,512,&Used)==1&&Used==3&&Out[0]==65&&Out[1]==32&&Out[2]==32);
 assert(T70VisualRow(Tab,3,4,&C,Out,512,&Used)==1&&Used==2&&Out[0]==32&&Out[1]==66);
 const uint16_t Tail[]={65,66,67,13,10,68};
 C=(T70_VISUAL_CURSOR){0,0};
 assert(T70VisualRow(Tail,6,4,&C,Out,512,&Used)==1&&Used==3&&C.Offset==5);
 T70_VISUAL_CURSOR Saved=C;
 assert(T70VisualRow(Tail,6,1,&C,Out,512,&Used)==-1&&!memcmp(&C,&Saved,sizeof C));
 assert(T70VisualRow(Tail,6,80,&C,Out,2,&Used)==-1&&!memcmp(&C,&Saved,sizeof C));
 assert(T70VisualRow(Tail,6,4,&C,Out,512,&Used)==1&&Used==1&&Out[0]==68);
 puts("PASS: widths 2..512 / lengths 0..1024 preserve tails; CR/LF/tab, exact wrap, cursor resume, invalid geometry");
}
