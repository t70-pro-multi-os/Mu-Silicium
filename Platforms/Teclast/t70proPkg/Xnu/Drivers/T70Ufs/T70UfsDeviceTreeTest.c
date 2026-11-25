/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T70UfsDeviceTree.h"
#include <assert.h>
#include <libfdt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int Argc, char **Argv)
{
  FILE *File;
  long Size;
  unsigned char *Original, *Changed;
  int Node, Parent;
  fdt32_t Reg[4] = {0, cpu_to_fdt32(0x11271000), 0, cpu_to_fdt32(0x2300)};
  assert(Argc == 2);
  File = fopen(Argv[1], "rb"); assert(File != NULL);
  assert(fseek(File, 0, SEEK_END) == 0); Size = ftell(File); assert(Size > 0 && Size < 0x100000);
  rewind(File);
  Original = malloc((size_t)Size); Changed = malloc((size_t)Size + 4096);
  assert(Original != NULL && Changed != NULL);
  assert(fread(Original, 1, (size_t)Size, File) == (size_t)Size); fclose(File);
  assert(T70UfsValidateDeviceTree(Original, (size_t)Size) == 1);
  assert(T70UfsValidateDeviceTree(Original, 16) == -1);
  assert(T70UfsValidateDeviceTree(Original, (size_t)Size - 1) == -1);
  assert(T70UfsValidateDeviceTree(NULL, (size_t)Size) == -1);
#define RESTORE() do { assert(fdt_open_into(Original, Changed, (int)Size + 4096) == 0); \
  Node = fdt_path_offset(Changed, "/soc/ufshci@11270000"); Parent = fdt_path_offset(Changed, "/soc"); } while (0)
  RESTORE(); assert(fdt_setprop_string(Changed, Node, "status", "disabled") == 0);
  assert(T70UfsValidateDeviceTree(Changed, (size_t)Size + 4096) == 0);
  RESTORE(); assert(fdt_setprop_string(Changed, Parent, "status", "disabled") == 0);
  assert(T70UfsValidateDeviceTree(Changed, (size_t)Size + 4096) == 0);
  RESTORE(); assert(fdt_setprop(Changed, Node, "reg", Reg, sizeof(Reg)) == 0);
  assert(T70UfsValidateDeviceTree(Changed, (size_t)Size + 4096) == -1);
  RESTORE(); assert(fdt_setprop_string(Changed, Node, "compatible", "vendor,other-ufs") == 0);
  assert(T70UfsValidateDeviceTree(Changed, (size_t)Size + 4096) == -1);
  RESTORE(); assert(fdt_delprop(Changed, Node, "clocks") == 0);
  assert(T70UfsValidateDeviceTree(Changed, (size_t)Size + 4096) == -1);
  RESTORE(); assert(fdt_setprop_u32(Changed, Node, "interrupt-parent", 0xdead) == 0);
  assert(T70UfsValidateDeviceTree(Changed, (size_t)Size + 4096) == -1);
  RESTORE(); assert(fdt_setprop(Changed, Parent, "status", "okay", 4) == 0);
  assert(T70UfsValidateDeviceTree(Changed, (size_t)Size + 4096) == -1);
  free(Changed); free(Original);
  puts("PASS: actual stock FDT and ten invalid/disabled/changed-input refusals");
  return 0;
}
