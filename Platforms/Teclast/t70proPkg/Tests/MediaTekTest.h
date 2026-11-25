// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-side mocks for executing the production drivers without tablet access.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Uefi.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/TimerLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
static unsigned Checks;
#define CHECK(x) do { Checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static UINT64 TimeNs;
UINTN EFIAPI MicroSecondDelay(UINTN us) { TimeNs += us * 1000ULL; return us; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return TimeNs; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 t) { return t; }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *a, CONST CHAR8 *b) { return strcmp(a, b); }
#ifdef TEST_DISPLAY_COPY_COUNTS
static UINTN TestCopyMemCalls, TestSetMem32Calls;
#endif
VOID *EFIAPI CopyMem(VOID *d, CONST VOID *s, UINTN n) {
#ifdef TEST_DISPLAY_COPY_COUNTS
  TestCopyMemCalls++;
#endif
  return memmove(d, s, n);
}
VOID *EFIAPI SetMem32(VOID *d, UINTN n, UINT32 value) {
  assert(((UINTN)d & 3U) == 0U && (n & 3U) == 0U);
#ifdef TEST_DISPLAY_COPY_COUNTS
  TestSetMem32Calls++;
#endif
  for (UINTN i=0;i<n/sizeof(value);++i) ((UINT32 *)d)[i]=value;
  return d;
}
VOID *EFIAPI ZeroMem(VOID *d, UINTN n) { return memset(d, 0, n); }
VOID *EFIAPI AllocateCopyPool(UINTN n, CONST VOID *s) { VOID *p=malloc(n); if (p) memcpy(p,s,n); return p; }
VOID EFIAPI FreePool(VOID *p) { free(p); }
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
static unsigned Installs;
static EFI_STATUS LocateStatus;
static EFI_STATUS LocateIoCfgRtStatus=EFI_SUCCESS;
static UINTN LocateIoCfgRtLength=0x1000;
EFI_STATUS EFIAPI LocateMemoryRegionByName(CHAR8 *name, EFI_MEMORY_REGION_DESCRIPTOR *r) {
  if (!strcmp(name,"IOCFG RT")) {
    if (EFI_ERROR(LocateIoCfgRtStatus)) return LocateIoCfgRtStatus;
    memset(r, 0, sizeof(*r)); r->Address = 0x200000; r->Length = LocateIoCfgRtLength; return EFI_SUCCESS;
  }
  if (EFI_ERROR(LocateStatus)) return LocateStatus;
  memset(r, 0, sizeof(*r)); r->Address = 0x100000; r->Length = 0x10000; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI __attribute__((unused)) Install(EFI_HANDLE *h, ...) { (VOID)h; Installs++; return EFI_SUCCESS; }

INTN EFIAPI LowBitSet32(UINT32 n) { return n ? __builtin_ctz(n) : -1; }

INTN EFIAPI CompareMem(CONST VOID *a, CONST VOID *b, UINTN n) { return memcmp(a,b,n); }
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *s) { return strlen(s); }
