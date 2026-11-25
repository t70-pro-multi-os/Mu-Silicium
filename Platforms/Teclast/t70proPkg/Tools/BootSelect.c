// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "BootSelect.h"
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
static u32 get32(const u8 *p) { return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]; }
static u64 get64(const u8 *p) { return (u64)get32(p)<<32 | get32(p+4); }
static void put32(u8 *p,u32 v) { p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static void put64(u8 *p,u64 v) { put32(p,v>>32);put32(p+4,v); }


static unsigned eq(const u8 *a,const char *b,u32 limit) {
  u32 i=0;
  for (;i<limit && b[i];++i) if (a[i]!=(u8)b[i]) return 0;
  return i<limit && a[i]==0 && b[i]==0;
}
static unsigned overlaps(u64 address,u64 size) {
  return size && address<T70_REQUEST_PAGE+4096ULL && T70_REQUEST_PAGE<address+size;
}
// A fixed reserved-memory owner must never be borrowed for a boot request.
static unsigned owners_clear(const u8 *fdt,u32 structure,u32 bytes,u32 strings,u32 string_bytes) {
  u32 at=structure,end=structure+bytes;
  int depth=-1,owner=-1;
  unsigned owner_cells=0;
  while (at<=end && end-at>=4) {
    u32 token=get32(fdt+at);at+=4;
    if (token==1) {
      u32 name=at;
      while (at<end && fdt[at]) ++at;
      if (at==end) return 0;
      ++depth;
      if (depth==1 && eq(fdt+name,"reserved-memory",at-name+1)) { owner=depth;owner_cells=0; }
      at=(at+4)&~3U;
    } else if (token==2) {
      if (depth<0) return 0;
      if (depth==owner) owner=-1;
      --depth;
    } else if (token==3) {
      if (depth<0 || at>end || end-at<8) return 0;
      u32 size=get32(fdt+at),name=get32(fdt+at+4);at+=8;
      if (size>end-at || name>=string_bytes) return 0;
      const u8 *key=fdt+strings+name;
      u32 bound=string_bytes-name,z=0;
      while (z<bound && key[z]) ++z;
      if (z==bound) return 0;
      if (owner>=0) {
        if ((eq(key,"#address-cells",bound) || eq(key,"#size-cells",bound)) &&
            (size!=4 || get32(fdt+at)!=2)) return 0;
        if (depth==owner && eq(key,"#address-cells",bound)) owner_cells|=1;
        if (depth==owner && eq(key,"#size-cells",bound)) owner_cells|=2;
        if (depth>owner && eq(key,"reg",bound)) {
          if (owner_cells!=3 || size%16) return 0;
          for (u32 i=0;i<size;i+=16) {
            u64 address=get64(fdt+at+i),length=get64(fdt+at+i+8);
            if (length>(~(u64)0)-address || overlaps(address,length)) return 0;
          }
        }
      }
      at=(at+size+3)&~3U;
    } else if (token==9) return depth==-1;
    else if (token!=4) return 0;
  }
  return 0;
}

// Reserve before Android uses RAM. Validate the entire table before changing it.
unsigned reserve_request_page(u8 *fdt) {
  u64 base=(u64)fdt;
  if ((base&7) || base<T70_FDT_LOW || base>T70_FDT_HIGH-40) return 0;
  u32 total=get32(fdt+4),structure=get32(fdt+8),strings=get32(fdt+12);
  u32 reserve=get32(fdt+16),version=get32(fdt+20);
  u32 string_size=get32(fdt+32),struct_size=get32(fdt+36);
  if (get32(fdt)!=0xd00dfeed || version!=17 || get32(fdt+24)>17 ||
      total<40 || total>T70_FDT_MAX || base+total>T70_FDT_HIGH ||
      structure<40 || (structure&3) || strings<40 ||
      structure>total || struct_size>total-structure ||
      strings>total || string_size>total-strings ||
      (struct_size && string_size && structure<strings+string_size && strings<structure+struct_size) ||
      reserve<40 || (reserve&7) || reserve>=structure || reserve>=strings) return 0;
  if (!owners_clear(fdt,structure,struct_size,strings,string_size)) return 0;
  u32 limit=structure<strings?structure:strings,at=reserve;
  for (;;) {
    if (at>limit || limit-at<16) return 0;
    u64 address=get64(fdt+at),size=get64(fdt+at+8);
    if (!address && !size) break;
    if (size>(~(u64)0)-address) return 0;
    if (overlaps(address,size)) return 0;
    at+=16;
  }
  if (total>T70_FDT_MAX-16 || base+total+16>T70_FDT_HIGH) return 0;
  for (u32 i=total;i>at;--i) fdt[i+15]=fdt[i-1];
  put64(fdt+at,T70_REQUEST_PAGE);put64(fdt+at+8,4096);
  put32(fdt+8,structure+16);put32(fdt+12,strings+16);put32(fdt+4,total+16);
  return 1;
}
