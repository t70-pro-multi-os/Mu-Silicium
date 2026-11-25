// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offer a local choice when the original LK selects a maintenance menu.
// Diagnostic modes also stop before Android factory startup.
#include "BootSelect.h"
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
#include "LkBootMode.h"
unsigned ChooseFactoryBoot(void);
unsigned ChooseRecoveryMenu(const void *fdt, unsigned size);
unsigned FactoryKeyHeld(void);

static u32 be32(const u8 *p) {
  return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
}
static u32 le32(const u8 *p) {
  return (u32)p[3]<<24 | (u32)p[2]<<16 | (u32)p[1]<<8 | p[0];
}
static unsigned eq(const u8 *a, const char *b, u32 limit) {
  u32 i=0;
  for (;i<limit && b[i];++i) if (a[i]!=(u8)b[i]) return 0;
  return i<limit && !a[i] && !b[i];
}
unsigned select_from_lk_boot_mode(const u8 *fdt) {
  u64 base=(u64)fdt;
  if ((base&7) || base<T70_FDT_LOW || base>T70_FDT_HIGH-40) return 2;
  u32 total=be32(fdt+4),at=be32(fdt+8),strings=be32(fdt+12);
  u32 ss=be32(fdt+32),ns=be32(fdt+36),reserve=be32(fdt+16);
  if (be32(fdt)!=0xd00dfeed || be32(fdt+20)!=17 || be32(fdt+24)>17 ||
      total<40 || total>T70_FDT_MAX || base+total>T70_FDT_HIGH ||
      at<40 || (at&3) || at>total || ns>total-at ||
      strings<40 || strings>total || ss>total-strings ||
      (ns && ss && at<strings+ss && strings<at+ns) ||
      reserve<40 || (reserve&7) || reserve>=at || reserve>=strings) return 2;
  u32 end=at+ns,root_count=0,chosen_count=0,mode_count=0,mode=0;
  int depth=-1,in_chosen=0;
  while (at<=end && end-at>=4) {
    u32 token=be32(fdt+at);at+=4;
    if (token==1) {
      u32 name=at;
      while (at<end && fdt[at]) ++at;
      if (at==end || (depth==-1 && at!=name) || depth>=64) return 2;
      if (depth==-1 && ++root_count!=1) return 2;
      ++depth;
      if (depth==1 && eq(fdt+name,"chosen",at-name+1)) {
        if (++chosen_count!=1) return 2;
        in_chosen=1;
      }
      at=(at+4)&~3U;
    } else if (token==2) {
      if (depth<0) return 2;
      if (depth==1) in_chosen=0;
      --depth;
    } else if (token==3) {
      if (depth<0 || at>end || end-at<8) return 2;
      u32 size=be32(fdt+at),name=be32(fdt+at+4);at+=8;
      if (size>end-at || name>=ss) return 2;
      const u8 *key=fdt+strings+name;
      u32 bound=ss-name,z=0;
      while (z<bound && key[z]) ++z;
      if (z==bound) return 2;
      if (in_chosen && depth==1 && eq(key,"atag,boot",bound)) {
        if (++mode_count!=1 || size!=16 ||
            le32(fdt+at)!=T70_ATAG_BOOT_SIZE || le32(fdt+at+4)!=T70_ATAG_BOOT_MAGIC ||
            le32(fdt+at+12)!=T70_ATAG_BOOT_TRAILER || le32(fdt+at+8)>255) return 2;
        mode=le32(fdt+at+8);
      }
      at=(at+size+3)&~3U;
    } else if (token==9) {
      if (depth!=-1 || at!=end || mode_count!=1) return 2;
      if (mode==T70_LK_FASTBOOT_MODE) return 1;
      if (mode==T70_LK_FACTORY_MODE) return ChooseFactoryBoot();
      if (mode==T70_LK_RECOVERY_MODE) return ChooseRecoveryMenu(fdt,total);
      if (mode==T70_LK_NORMAL_MODE) return FactoryKeyHeld();
      if (mode==3 || mode==7 || mode==8 || mode==9) return 0;
      return 2;
    } else if (token!=4) return 2;
  }
  return 2;
}
