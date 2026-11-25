/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T70UfsMemory.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
  assert(T70UfsRangeOutsideOwnedRam(0x43f7f000,0x1000,0x50000000,0x10000000));
  assert(T70UfsRangeOutsideOwnedRam(0x70000000,0x1000,0x50000000,0x10000000));
  assert(T70UfsRangeOutsideOwnedRam(0x4ffff000,0x1000,0x50000000,0x10000000));
  assert(T70UfsRangeOutsideOwnedRam(0x60000000,0x1000,0x50000000,0x10000000));
  assert(!T70UfsRangeOutsideOwnedRam(0x50000000,0x1000,0x50000000,0x10000000));
  assert(!T70UfsRangeOutsideOwnedRam(0x4ffff000,0x2000,0x50000000,0x10000000));
  assert(!T70UfsRangeOutsideOwnedRam(0x5ffff000,0x2000,0x50000000,0x10000000));
  assert(!T70UfsRangeOutsideOwnedRam(0x40000000,0x40000000,0x50000000,0x10000000));
  assert(!T70UfsRangeOutsideOwnedRam(UINT64_MAX-0xfff,0x1000,0x50000000,0x10000000));
  assert(!T70UfsRangeOutsideOwnedRam(0x40000000,0x1000,UINT64_MAX-0xfff,0x1000));
  assert(!T70UfsRangeOutsideOwnedRam(0x40000000,0,0x50000000,0x10000000));
  assert(!T70UfsRangeOutsideOwnedRam(0x40000000,0x1000,0x50000000,0));
  puts("PASS: firmware range boundaries, allocator overlap, invalid sizes and overflow");
  return 0;
}
