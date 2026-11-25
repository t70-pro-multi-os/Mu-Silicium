// SPDX-License-Identifier: BSD-2-Clause-Patent
// Normal SEC watchdog stop only. No BootShim, Linux, MMIO or device execution.
#include <stdio.h>
#include <assert.h>
#include "../Library/PlatformSecLib/PlatformSec.c"
static UINT32 InitialMode, WrittenMode;
static unsigned ModeReads, ModeWrites, StageWrites;
UINT32 EFIAPI MmioRead32(UINTN Address) {
  assert(Address==T70_WDT_BASE);++ModeReads;return InitialMode;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  assert(Address==T70_WDT_BASE);++ModeWrites;WrittenMode=Value;return Value;
}
UINT64 EFIAPI MmioRead64(UINTN Address) {
  assert(Address==T70_HANDOFF_BASE);return 0; // Normal path, not an interactive trial.
}
UINT64 EFIAPI MmioWrite64(UINTN Address,UINT64 Value) {
  assert(Address==T70_HANDOFF_BASE+0x38 && Value==3);++StageWrites;return Value;
}
static unsigned Patterns;
static int CheckMode(UINT32 Mode) {
  InitialMode=Mode;ModeReads=ModeWrites=StageWrites=0;WrittenMode=0;
  PlatformInitialize();
  UINT32 Expected=0x22000000U|(Mode&0xfffffffeU);
  if (WrittenMode!=Expected || ModeReads!=1 || ModeWrites!=1 || StageWrites!=1) {
    fprintf(stderr,"normal stop: mode=0x%x wrote=0x%x expected=0x%x reads=%u\n",Mode,WrittenMode,Expected,ModeReads);
    return 1;
  }
  ++Patterns;return 0;
}
int main(void) {
  // Stock mtk_wdt_stop, module SHA ae3c62a502af885b1bd67f45c4ede2afc40df9ee2f2fce1931d65d4326b1f308:
  // .text 0x3c8 AND #0xfffffffe; 0x3d0 ORR with key 0x22000000.
  for (UINT32 Index=0;Index<65536;++Index)
    if (CheckMode((Index+2U)&0xffffU)) return 1;
  // Upper-bit readback is modeled, not evidence that reserved hardware bits
  // exist. Require exact stock arithmetic, including key OR semantics.
  for (unsigned Bit=16;Bit<32;++Bit) {
    UINT32 One=1U<<Bit;
    if (CheckMode(One) || CheckMode(One|1U) ||
        CheckMode(~One&0xfffffffeU) || CheckMode(~One|1U)) return 1;
  }
  static const UINT32 Boundaries[]={0xffffffffU,0xfffffffeU,0xffff0000U,
    0xffff0001U,0x80000000U,0x80000001U,0xaaaaaaaaU,0x55555555U};
  for (unsigned I=0;I<sizeof(Boundaries)/sizeof(Boundaries[0]);++I)
    if (CheckMode(Boundaries[I])) return 1;
  printf("Watchdog normal stop: %u MODE patterns including upper bits 16..31, complements and boundaries; stock instruction contract; synthetic MMIO only\n",Patterns);
  return 0;
}
