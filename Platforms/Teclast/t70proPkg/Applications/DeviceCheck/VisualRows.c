// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "VisualRows.h"
static void Newline(const uint16_t *T,size_t N,T70_VISUAL_CURSOR *C) {
  uint16_t V=T[C->Offset++];
  if(V==13 && C->Offset<N && T[C->Offset]==10) C->Offset++;
}
int T70VisualRow(const uint16_t *T,size_t N,unsigned Columns,
 T70_VISUAL_CURSOR *C,uint16_t *Out,size_t Capacity,size_t *Written) {
  size_t Used=0;unsigned Limit;
  if(!T||!C||!Out||!Written||Columns<2||Columns>512||Capacity<Columns||
     C->Offset>N||C->Spaces>4) return -1;
  Limit=Columns-1;
  *Written=0;Out[0]=0;
  if(C->Offset==N&&!C->Spaces)return 0;
  while(Used<Limit) {
    uint16_t V;
    if(C->Spaces){Out[Used++]=32;C->Spaces--;continue;}
    if(C->Offset==N)break;
    V=T[C->Offset];
    if(V==13||V==10){Newline(T,N,C);break;}
    C->Offset++;
    if(V==9){C->Spaces=4-(unsigned)(Used%4);continue;}
    Out[Used++]=V<32?63:V;
  }
  /* Avoid an extra empty visual row when text fits exactly before CR/LF. */
  if(Used==Limit&&!C->Spaces&&C->Offset<N&&
     (T[C->Offset]==13||T[C->Offset]==10))Newline(T,N,C);
  Out[Used]=0;*Written=Used;return 1;
}
