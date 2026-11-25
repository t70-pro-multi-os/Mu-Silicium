/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T70UfsDeviceTree.h"
#include <libfdt.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

static uint64_t Be64Cells(const fdt32_t *Cells)
{
  return ((uint64_t)fdt32_to_cpu(Cells[0]) << 32) | fdt32_to_cpu(Cells[1]);
}

static int Enabled(const void *Fdt, int Node)
{
  int Length;
  const char *Status = fdt_getprop(Fdt, Node, "status", &Length);
  if (Status == NULL) return Length == -FDT_ERR_NOTFOUND ? 1 : -1;
  if (Length <= 0 || Status[Length - 1] != '\0') return -1;
  if ((Length == 3 && memcmp(Status, "ok", 3) == 0) ||
      (Length == 5 && memcmp(Status, "okay", 5) == 0)) return 1;
  return 0;
}

static int Names(const void *Fdt, int Node, const char *Property,
                 const char *const *Expected, int Count)
{
  int Index;
  if (fdt_stringlist_count(Fdt, Node, Property) != Count) return 0;
  for (Index = 0; Index < Count; ++Index) {
    if (fdt_stringlist_search(Fdt, Node, Property, Expected[Index]) != Index) return 0;
  }
  return 1;
}

int T70UfsValidateDeviceTree(const void *Fdt, size_t Bytes)
{
  static const char *const Clocks[] = {"ufs", "ufs_mp", "unipro_tick", "unipro_sys", "ufs_aes", "ufs_ck"};
  static const char *const Resets[] = {"hci_rst", "unipro_rst", "crypto_rst"};
  int Node, Parent, Root, Current, Length, Result, Gic;
  const fdt32_t *Cells;
  uint32_t InterruptParent = 0;
  const uint64_t Base = UINT64_C(0x11270000), Size = UINT64_C(0x2300);
  if (Fdt == NULL || Bytes < sizeof(struct fdt_header) || Bytes > 0x100000 ||
      fdt_check_full(Fdt, Bytes) != 0) return -1;
  Root = fdt_path_offset(Fdt, "/");
  Parent = fdt_path_offset(Fdt, "/soc");
  Node = fdt_path_offset(Fdt, "/soc/ufshci@11270000");
  if (Root < 0 || Parent < 0 || Node < 0 || fdt_parent_offset(Fdt, Node) != Parent ||
      fdt_address_cells(Fdt, Root) != 2 || fdt_size_cells(Fdt, Root) != 2 ||
      fdt_address_cells(Fdt, Parent) != 2 || fdt_size_cells(Fdt, Parent) != 2 ||
      fdt_node_check_compatible(Fdt, Node, "mediatek,mt8183-ufshci") != 0) return -1;
  for (Current = Node; Current >= 0; Current = fdt_parent_offset(Fdt, Current)) {
    Result = Enabled(Fdt, Current);
    if (Result != 1) return Result;
    if (InterruptParent == 0) {
      Cells = fdt_getprop(Fdt, Current, "interrupt-parent", &Length);
      if (Cells != NULL) {
        if (Length != 4) return -1;
        InterruptParent = fdt32_to_cpu(Cells[0]);
      } else if (Length != -FDT_ERR_NOTFOUND) return -1;
    }
  }
  Cells = fdt_getprop(Fdt, Node, "reg", &Length);
  if (Cells == NULL || Length != 16 || Be64Cells(Cells) != Base || Be64Cells(Cells + 2) != Size) return -1;
  Cells = fdt_getprop(Fdt, Parent, "ranges", &Length);
  if (Cells == NULL) return -1;
  if (Length != 0) {
    uint64_t Child, Physical, Span;
    if (Length != 24) return -1;
    Child = Be64Cells(Cells); Physical = Be64Cells(Cells + 2); Span = Be64Cells(Cells + 4);
    if (Child != Physical || Base < Child || Span < Size || Base - Child > Span - Size) return -1;
  }
  Cells = fdt_getprop(Fdt, Node, "interrupts", &Length);
  if (Cells == NULL || Length != 16 || fdt32_to_cpu(Cells[0]) != 0 ||
      fdt32_to_cpu(Cells[1]) != 137 || fdt32_to_cpu(Cells[2]) != 4 || fdt32_to_cpu(Cells[3]) != 0) return -1;
  Gic = fdt_node_offset_by_phandle(Fdt, InterruptParent);
  if (Gic < 0 || fdt_node_check_compatible(Fdt, Gic, "arm,gic-v3") != 0) return -1;
  Cells = fdt_getprop(Fdt, Gic, "#interrupt-cells", &Length);
  if (Cells == NULL || Length != 4 || fdt32_to_cpu(Cells[0]) != 4) return -1;
  if (!Names(Fdt, Node, "clock-names", Clocks, 6) || !Names(Fdt, Node, "reset-names", Resets, 3)) return -1;
  Cells = fdt_getprop(Fdt, Node, "clocks", &Length);
  if (Cells == NULL || Length != 48) return -1;
  Cells = fdt_getprop(Fdt, Node, "resets", &Length);
  if (Cells == NULL || Length != 24) return -1;
  Cells = fdt_getprop(Fdt, Node, "vcc-supply", &Length);
  if (Cells == NULL || Length != 4 || fdt_node_offset_by_phandle(Fdt, fdt32_to_cpu(Cells[0])) < 0) return -1;
  return 1;
}
