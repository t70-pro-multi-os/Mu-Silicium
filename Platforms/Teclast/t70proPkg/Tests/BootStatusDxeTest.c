// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native VM test of the actual driver; no tablet access or hardware emulation.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Library/IoLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/FdtLib.h>

#define HANDOFF 0x43F7F000UL
#define DTB 0x47C80000UL
#define DTB_END 0x48080000UL
EFI_GUID gFdtTableGuid = FDT_TABLE_GUID;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
static void *Allocation, *Installed;
static UINTN AllocatedPages;
static int AllocCalls, FreeCalls, InstallCalls, FlushCalls, FailAllocate, FailInstall;
static unsigned char *Input;
static size_t InputSize;
static int NotificationCalls, ExpectT70Sd, ExpectT70Ufs;
void EFIAPI MemoryFence(void) { __sync_synchronize(); }
EFI_STATUS EFIAPI BootStatusEntry(EFI_HANDLE, EFI_SYSTEM_TABLE *);

UINT64 EFIAPI MmioRead64(UINTN a) {
  assert(a >= HANDOFF && a + 8 <= HANDOFF + 4096); return *(UINT64 *)a;
}
UINT64 EFIAPI MmioWrite64(UINTN a, UINT64 v) {
  assert(a >= HANDOFF && a + 8 <= HANDOFF + 4096); return *(UINT64 *)a = v;
}
void *EFIAPI CopyMem(void *d, const void *s, UINTN n) { return memcpy(d, s, n); }
void *EFIAPI ZeroMem(void *d, UINTN n) { return memset(d, 0, n); }
void *EFIAPI WriteBackDataCacheRange(void *a, UINTN n) {
  assert((UINTN)a == HANDOFF && n == 4096); FlushCalls++; return a;
}
static EFI_STATUS EFIAPI Allocate(EFI_ALLOCATE_TYPE t, EFI_MEMORY_TYPE m, UINTN n,
                                  EFI_PHYSICAL_ADDRESS *a) {
  AllocCalls++;
  assert(t == AllocateAnyPages && m == EfiACPIReclaimMemory);
  assert(n == EFI_SIZE_TO_PAGES(InputSize));
  if (FailAllocate) return EFI_OUT_OF_RESOURCES;
  Allocation = mmap(NULL, n * 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(Allocation != MAP_FAILED);
  memset(Allocation, 0xA5, n * 4096);
  AllocatedPages = n; *a = (UINTN)Allocation; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free(EFI_PHYSICAL_ADDRESS a, UINTN n) {
  assert(a == (UINTN)Allocation && n == AllocatedPages);
  FreeCalls++; assert(munmap(Allocation, n * 4096) == 0); Allocation = NULL;
  return EFI_SUCCESS;
}
static void CheckProperty(const void *dtb, INT32 node, const char *key,
                          const char *expected, size_t size) {
  INT32 length;
  const void *value = FdtGetProp(dtb, node, key, &length);
  if (!value || length != (INT32)size || memcmp(value, expected, size) != 0) {
    fprintf(stderr, "T70 DTB property mismatch: %s\n", key);
    exit(2);
  }
}
static const UINT32 *CheckCells(const void *dtb, INT32 node, const char *key,
                                const UINT32 *expected, size_t count, size_t start) {
  INT32 length;
  const UINT32 *value = FdtGetProp(dtb, node, key, &length);
  if (!value || length != (INT32)(count * sizeof(*value))) {
    fprintf(stderr, "T70 DTB %s length mismatch\n", key);
    exit(2);
  }
  for (size_t n = start; n < count; n++) {
    if (Fdt32ToCpu(value[n]) != expected[n]) {
      fprintf(stderr, "T70 DTB %s cell %zu mismatch\n", key, n);
      exit(2);
    }
  }
  return value;
}
static void CheckT70Sd(const void *dtb) {
  const INT32 node = FdtPathOffset(dtb, "/soc/mmc@11240000");
  const char compatible[] = "mediatek,mt6789-mmc";
  const char clocks[] = "source\0hclk\0source_cg";
  const char model[] = "MT6789";
  const char rootCompatible[] = "mediatek,MT6789";
  const char enabled[] = "okay";
  const UINT32 expectedReg[] = {0, 0x11240000, 0, 0x1000, 0, 0x11ef0000, 0, 0x1000};
  const UINT32 expectedIrq[] = {0, 0x87, 4, 0};
  const UINT32 hostIndex[] = {1}, width[] = {4}, debounce[] = {0};
  const UINT32 cardDetect[] = {0, 7, 0}, gpioCells[] = {2};
  const UINT32 *detect;
  INT32 provider;
  if (node < 0) {
    fputs("T70 DTB SD node missing: /soc/mmc@11240000\n", stderr);
    exit(2);
  }
  CheckProperty(dtb, 0, "model", model, sizeof(model));
  CheckProperty(dtb, 0, "compatible", rootCompatible, sizeof(rootCompatible));
  CheckProperty(dtb, node, "compatible", compatible, sizeof(compatible));
  CheckProperty(dtb, node, "status", enabled, sizeof(enabled));
  CheckProperty(dtb, node, "clock-names", clocks, sizeof(clocks));
  CheckCells(dtb, node, "reg", expectedReg, 8, 0);
  CheckCells(dtb, node, "bus-width", width, 1, 0);
  CheckCells(dtb, node, "host-index", hostIndex, 1, 0);
  CheckCells(dtb, node, "interrupts", expectedIrq, 4, 0);
  CheckCells(dtb, node, "cd-debounce-delay-ms", debounce, 1, 0);
  // Phandle numbers can be reassigned; require the referenced GPIO provider.
  detect = CheckCells(dtb, node, "cd-gpios", cardDetect, 3, 1);
  provider = FdtNodeOffsetByPhandle(dtb, Fdt32ToCpu(detect[0]));
  if (provider < 0 || provider != FdtPathOffset(dtb, "/soc/pinctrl")) {
    fputs("T70 DTB SD card detect provider mismatch\n", stderr);
    exit(2);
  }
  CheckCells(dtb, provider, "#gpio-cells", gpioCells, 1, 0);
}
static void CheckT70Ufs(const void *dtb) {
  const INT32 node = FdtPathOffset(dtb, "/soc/ufshci@11270000");
  const char compatible[] = "mediatek,mt8183-ufshci";
  const char model[] = "MT6789";
  const UINT32 reg[] = {0, 0x11270000, 0, 0x2300};
  const UINT32 irq[] = {0, 0x89, 4, 0};
  if (node < 0) {
    fputs("T70 DTB UFS node missing: /soc/ufshci@11270000\n", stderr);
    exit(2);
  }
  CheckProperty(dtb, 0, "model", model, sizeof(model));
  CheckProperty(dtb, node, "compatible", compatible, sizeof(compatible));
  CheckCells(dtb, node, "reg", reg, 4, 0);
  CheckCells(dtb, node, "interrupts", irq, 4, 0);
}
static EFI_STATUS EFIAPI Install(EFI_GUID *g, void *p) {
  InstallCalls++;
  assert(memcmp(g, &gFdtTableGuid, sizeof(*g)) == 0 && p == Allocation);
  if (FailInstall) return EFI_DEVICE_ERROR;
  // Model the synchronous table notification that starts GOP/DSI. These are
  // the consumer's exact prerequisites while InstallConfigurationTable runs.
  Installed = p;
  NotificationCalls++;
  assert(MmioRead64(HANDOFF+0x48)==(UINTN)p);
  assert(MmioRead64(HANDOFF+0x50)==InputSize);
  assert(MmioRead64(HANDOFF+0x58)==EFI_SUCCESS);
  assert(FdtCheckFull(p,InputSize)==0);
  if (ExpectT70Sd) CheckT70Sd(p);
  if (ExpectT70Ufs) CheckT70Ufs(p);
  return EFI_SUCCESS;
}
static void Reset(void) {
  if (Allocation) assert(munmap(Allocation, AllocatedPages * 4096) == 0);
  Allocation = Installed = NULL; NotificationCalls = 0;
  AllocCalls = FreeCalls = InstallCalls = FlushCalls = FailAllocate = FailInstall = 0;
  memset((void *)HANDOFF, 0, 4096);
  memset((void *)DTB, 0, DTB_END - DTB);
  memcpy((void *)DTB, Input, InputSize);
  MmioWrite64(HANDOFF, 0x0000000148303754ULL);
  MmioWrite64(HANDOFF + 8, DTB);
  MmioWrite64(HANDOFF + 16, InputSize);
}
static void Check(EFI_STATUS expected) {
  EFI_SYSTEM_TABLE *system = (EFI_SYSTEM_TABLE *)0x12345000UL;
  assert(BootStatusEntry(NULL, system) == expected);
  assert(MmioRead64(HANDOFF + 0x40) == (UINTN)system);
  assert(MmioRead64(HANDOFF + 0x38) == 4 && FlushCalls == 1);
  assert(MmioRead64(HANDOFF + 0x58) == expected);
  if (expected == EFI_SUCCESS) {
    assert(AllocCalls == 1 && InstallCalls == 1 && FreeCalls == 0 && NotificationCalls == 1);
    assert(MmioRead64(HANDOFF + 0x48) == (UINTN)Installed);
    assert(MmioRead64(HANDOFF + 0x50) == InputSize);
    assert(memcmp(Installed, Input, InputSize) == 0);
    assert(memcmp((void *)DTB, Input, InputSize) == 0);
    for (size_t n = InputSize; n < AllocatedPages * 4096; n++)
      assert(((unsigned char *)Installed)[n] == 0);
  } else {
    assert(Installed == NULL && Allocation == NULL && NotificationCalls == 0);
    assert(MmioRead64(HANDOFF + 0x48) == 0 && MmioRead64(HANDOFF + 0x50) == 0);
    if (expected == EFI_COMPROMISED_DATA) assert(AllocCalls == 0 && InstallCalls == 0);
  }
}
int main(int argc, char **argv) {
  assert(argc >= 2 && argc <= 4);
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--expect-t70-sd") == 0 && !ExpectT70Sd) ExpectT70Sd = 1;
    else if (strcmp(argv[i], "--expect-t70-ufs") == 0 && !ExpectT70Ufs) ExpectT70Ufs = 1;
    else return 2;
  }
  FILE *f = fopen(argv[1], "rb"); assert(f);
  assert(fseek(f, 0, SEEK_END) == 0); long n = ftell(f); assert(n >= 40 && n <= 0x200000);
  InputSize = n; Input = malloc(InputSize); assert(Input);
  rewind(f); assert(fread(Input, 1, InputSize, f) == InputSize); fclose(f);
  assert(FdtCheckFull(Input, InputSize) == 0);
  assert(mmap((void *)HANDOFF, 4096, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)HANDOFF);
  assert(mmap((void *)DTB, DTB_END - DTB, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)DTB);
  Services.AllocatePages = Allocate; Services.FreePages = Free;
  Services.InstallConfigurationTable = Install;
  Reset(); Check(EFI_SUCCESS);
  // Repeat entry in a fresh mocked firmware environment, with an offset pointer.
  Reset(); memmove((void *)(DTB + 8), (void *)DTB, InputSize);
  MmioWrite64(HANDOFF + 8, DTB + 8);
  // Check offset preservation separately: original now starts at DTB + 8.
  assert(BootStatusEntry(NULL, NULL) == EFI_SUCCESS);
  assert(memcmp(Installed, Input, InputSize) == 0);
  const UINT64 bad[] = {DTB - 8, DTB + 1, DTB_END, DTB_END - 32, 0xFFFFFFFFFFFFFFF8ULL};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    Reset(); MmioWrite64(HANDOFF + 8, bad[i]); Check(EFI_COMPROMISED_DATA);
  }
  const UINT64 sizes[] = {0, 39, 0x200001, InputSize - 1};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    Reset(); MmioWrite64(HANDOFF + 16, sizes[i]); Check(EFI_COMPROMISED_DATA);
  }
  Reset(); *(UINT32 *)DTB = 0; Check(EFI_COMPROMISED_DATA);
  Reset(); ((UINT32 *)DTB)[2] = 0xFFFFFFFF; Check(EFI_COMPROMISED_DATA);
  Reset(); ((UINT32 *)DTB)[4] = 0xFFFFFFFF; Check(EFI_COMPROMISED_DATA);
  Reset();
  // Header remains valid, but the root structure token is invalid.
  *(UINT32 *)(DTB + FdtOffsetDtStruct((void *)DTB)) = 0xFFFFFFFF;
  Check(EFI_COMPROMISED_DATA);
  Reset(); FailAllocate = 1; Check(EFI_OUT_OF_RESOURCES); assert(AllocCalls == 1);
  Reset(); FailInstall = 1; Check(EFI_DEVICE_ERROR); assert(FreeCalls == 1);
  Reset(); MmioWrite64(HANDOFF, 0);
  assert(BootStatusEntry(NULL, NULL) == EFI_NOT_FOUND);
  assert(AllocCalls == 0 && InstallCalls == 0 && FlushCalls == 0);
  Reset(); free(Input); munmap((void *)HANDOFF, 4096); munmap((void *)DTB, DTB_END - DTB);
  if (ExpectT70Sd) puts("T70 root and SD properties match at EFI table publication");
  if (ExpectT70Ufs) puts("T70 root and UFS properties match at EFI table publication");
  puts("18 cases passed: exact DTB preservation, input bounds, full parser, allocation and table failures");
  return 0;
}
