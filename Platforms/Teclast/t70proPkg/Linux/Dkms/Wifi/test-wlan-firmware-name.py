#!/usr/bin/env python3
"""Run the staged executable name selector on mock observed hardware flavors."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
f = re.search(r"^void soc2_1x1ConstructFirmwarePrio\([^{}]*\n\{.*?^\}", source, re.M | re.S)
assert f
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CFG_FW_NAME_MAX_LEN 128
#define CFG_WIFI_IP_SET 1
struct GLUE_INFO {};
static const char *apucSoc2_1x1FwName[] = {"WIFI_RAM_CODE_soc2_2", NULL};
static int detected;
static uint8_t observed;
static int kalGetFwFlavor(uint8_t *out) { *out=observed; return detected; }
'''
test = r'''
int main(void)
{
    uint8_t body[CFG_FW_NAME_MAX_LEN] = {0};
    uint8_t *slots[] = {body, NULL};
    uint8_t index = 0;
    observed='d'; detected=0;
    soc2_1x1ConstructFirmwarePrio(NULL, NULL, slots, &index, 1);
    assert(!index && !body[0]);
    detected=-ENODEV;
    soc2_1x1ConstructFirmwarePrio(NULL, NULL, slots, &index, 1);
    assert(!index && !body[0]);
    detected=1; observed='x';
    soc2_1x1ConstructFirmwarePrio(NULL, NULL, slots, &index, 1);
    assert(!index && !body[0]);
    observed='d';
    soc2_1x1ConstructFirmwarePrio(NULL, NULL, slots, &index, 1);
    assert(index==1 && !strcmp((char *)body, "WIFI_RAM_CODE_soc2_2_1d_1.bin") && !slots[1]);
    observed='e';
    soc2_1x1ConstructFirmwarePrio(NULL, NULL, slots, &index, 1);
    assert(index==1 && !strcmp((char *)body, "WIFI_RAM_CODE_soc2_2_1d_1.bin"));
    index=0;
    soc2_1x1ConstructFirmwarePrio(NULL, NULL, slots, &index, 1);
    assert(index==1 && !strcmp((char *)body, "WIFI_RAM_CODE_soc2_2_1e_1.bin"));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-wlan-filename-") as tmp:
    c = Path(tmp) / "test.c"
    c.write_text(prefix + f[0] + test)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("WLAN only selects exact 1d/1e names with a confirmed flavor and available output slot")
