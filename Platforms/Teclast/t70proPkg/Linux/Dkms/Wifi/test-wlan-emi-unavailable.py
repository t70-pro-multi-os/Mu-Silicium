#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test that absent EMI support cannot report a completed WLAN boot."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define IN
#define CFG_MTK_ANDROID_EMI 0
#define CFG_DOWNLOAD_DYN_MEMORY_MAP 0
#define WLAN_STATUS_SUCCESS 0u
#define WLAN_STATUS_FAILURE 0xc0000001u
#define WLAN_STATUS_NOT_SUPPORTED 0xc00000bbu
#define TRUE 1
#define FALSE 0
#define PDA_N9 1
#define PDA_CR4 2
#define DBGLOG(...) ((void)0)
typedef uint8_t u_int8_t;
enum ENUM_IMG_DL_IDX_T { IMG_DL_IDX_N9_FW };
struct WIFI_VER_INFO { struct { uint8_t ucRegionNum; } rCommonTailer; };
struct CHIP { int (*checkbushang)(void *, int); };
struct ADAPTER { void *prGlueInfo; struct WIFI_VER_INFO rVerInfo; struct CHIP *chip_info; };
static uint8_t image[32];
static unsigned started, unmapped, mapped, emi;
static void kalFirmwareImageMapping(void *glue, void **buffer,
                                    uint32_t *size, enum ENUM_IMG_DL_IDX_T index)
{
    (void)glue; (void)index; *buffer = image; *size = sizeof(image); mapped++;
}
static uint32_t wlanGetConnacTailerInfo(struct WIFI_VER_INFO *ver, void *buffer,
                                        uint32_t size, enum ENUM_IMG_DL_IDX_T index)
{
    (void)index; assert(buffer == image && size == sizeof(image));
    ver->rCommonTailer.ucRegionNum = 1;
    return WLAN_STATUS_SUCCESS;
}
static uint32_t wlanDetectRamEntry(struct WIFI_VER_INFO *ver)
{
    (void)ver; return 0;
}
static uint32_t wlanConfigWifiFunc(struct ADAPTER *adapter, int flag, uint32_t entry, uint8_t pda)
{
    (void)adapter; (void)flag; (void)entry; (void)pda; started++;
    return WLAN_STATUS_SUCCESS;
}
static void kalFirmwareImageUnmapping(void *glue, void *unused, void *buffer)
{
    (void)glue; (void)unused; assert(buffer == image); unmapped++;
}
'''
TAIL = r'''
static uint32_t wlanImageSectionDownloadStage(struct ADAPTER *adapter, void *buffer,
        uint32_t size, uint8_t sections, enum ENUM_IMG_DL_IDX_T index, u_int8_t *dyn)
{
    (void)index; (void)dyn; assert(sections == 1);
    return emi ? wlanDownloadEMISection(adapter, 0, size, buffer) : WLAN_STATUS_SUCCESS;
}
'''
TEST = r'''
int main(void)
{
    struct CHIP chip = {0};
    struct ADAPTER adapter = {.chip_info = &chip};
    assert(wlanConnacFormatDownload(&adapter, IMG_DL_IDX_N9_FW) == WLAN_STATUS_SUCCESS);
    assert(started == 1 && unmapped == 1 && mapped == 1);
    emi = 1;
#ifdef EXPECT_UNFIXED
    assert(wlanConnacFormatDownload(&adapter, IMG_DL_IDX_N9_FW) == WLAN_STATUS_SUCCESS);
    assert(started == 2 && unmapped == 2 && mapped == 2);
#else
    assert(wlanConnacFormatDownload(&adapter, IMG_DL_IDX_N9_FW) == WLAN_STATUS_FAILURE);
    assert(started == 1 && unmapped == 2 && mapped == 2);
#endif
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("--expect-unfixed", action="store_true")
    a = p.parse_args()
    src = a.source.read_text()

    def extract(name):
        return re.search(r"(?ms)^uint32_t " + name + r"\([^{}]*\n\{.*?^\}", src)[0]

    guard = "== WLAN_STATUS_FAILURE" if a.expect_unfixed else "!= WLAN_STATUS_SUCCESS"
    assert f"if (u4Status {guard})\n\t\t\t\tbreak;" in src
    with tempfile.TemporaryDirectory(prefix="t70-wlan-emi-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(SHIM + extract("wlanDownloadEMISection") + "\n" + TAIL +
                     extract("wlanConnacFormatDownload") + TEST)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter",
                        *(["-DEXPECT_UNFIXED"] if a.expect_unfixed else []),
                        "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Confirmed original false success" if a.expect_unfixed
          else "Unsupported EMI aborts before start, releases image, keeps non-EMI path")


if __name__ == "__main__":
    main()
