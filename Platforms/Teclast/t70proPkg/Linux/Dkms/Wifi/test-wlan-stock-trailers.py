#!/usr/bin/env python3
"""Run the staged metadata parser only against the two verified stock files."""
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile

folder, stock = map(Path, sys.argv[1:])
source = (folder / "chips/common/fw_dl.c").read_text()
header = (folder / "include/chips/fw_dl.h").read_text()
def code(name):
    return re.search(r"^(?:static )?(?:bool|uint32_t|void) " + name
                     + r"\([^{};]*\n\{.*?^\}", source, re.M | re.S)[0]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IN
#define MAX_FWDL_SECTION_NUM 32
#define WLAN_STATUS_FAILURE 1
#define WLAN_STATUS_SUCCESS 0
#define kalMemCopy memcpy
#define kalMemZero(p,n) memset(p,0,n)
#define DBGLOG(...) ((void)0)
#define RELEASE_INFO_SEPARATOR_LEN 16
enum ENUM_IMG_DL_IDX_T { IMG_DL_IDX_N9_FW, IMG_DL_IDX_CR4_FW };
'''
structures = "\n".join(re.search(r"^struct " + name + r" \{.*?^\};", header, re.M | re.S)[0]
                        for name in ("TAILER_COMMON_FORMAT_T", "TAILER_REGION_FORMAT_T", "HEADER_RELEASE_INFO"))
prefix += structures + r'''
struct WIFI_VER_INFO {
    struct TAILER_COMMON_FORMAT_T rCommonTailer;
    struct TAILER_REGION_FORMAT_T rRegionTailers[MAX_FWDL_SECTION_NUM];
    uint8_t aucReleaseManifest[256];
};
'''
test = r'''
int main(int argc, char **argv)
{
    struct WIFI_VER_INFO info = {0};
    assert(argc == 3);
    assert(wlanGetConnacTailerInfo(&info, NULL, 0, IMG_DL_IDX_N9_FW) == WLAN_STATUS_FAILURE);
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "rb");
        assert(file && !fseek(file, 0, SEEK_END));
        long size = ftell(file);
        assert(size > 0 && size <= INT32_MAX && !fseek(file, 0, SEEK_SET));
        void *image = malloc(size);
        assert(image && fread(image, 1, size, file) == (size_t)size);
        fclose(file);
        assert(wlanGetConnacTailerInfo(&info, image, size, IMG_DL_IDX_N9_FW) == WLAN_STATUS_SUCCESS);
        assert(info.rCommonTailer.ucRegionNum == 5 && info.aucReleaseManifest[0]);
        free(image);
    }
    return 0;
}
'''
inputs = [stock / f"WIFI_RAM_CODE_soc2_2_1{flavor}_1.bin" for flavor in "de"]
expected = ("2654c3c4ffd7f32fc46a96a85a229d06d4a13210e8056980ba3576f3f63fed7f",
            "2bd73bd2c2bb338c5238074b848f045644fa3929859ecbde53b8066febe53848")
for path, digest in zip(inputs, expected):
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError(f"{path.name}: not the audited stock input")
with tempfile.TemporaryDirectory(prefix="t70-stock-trailers-") as tmp:
    src = Path(tmp) / "test.c"
    src.write_text(prefix + "\n".join(code(fn) for fn in
        ("fwDlGetReleaseManifest", "fwDlGetReleaseInfoSection",
         "t70_wlan_release_info_valid", "wlanGetConnacTailerInfo")) + test)
    exe = src.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe), *inputs], check=True, timeout=3)
for path in inputs:
    print(f"{path.name}: metadata accepted, SHA-256 {hashlib.sha256(path.read_bytes()).hexdigest()}")
