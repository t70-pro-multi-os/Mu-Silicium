#!/usr/bin/env python3
"""Check the generated Wi-Fi firmware status formatters' output contract."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys

folder = Path(sys.argv[1])
chip = (folder / "chips/soc2_1x1/soc2_1x1.c").read_text()
fw = (folder / "chips/common/fw_dl.c").read_text()
def fn(src, name):
    return re.search(r"^(?:static )?uint32_t " + name
                     + r"\([^{};]*\n\{.*?^\}", src, re.M | re.S)[0]

prefix = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CFG_MTK_ANDROID_WMT 1
#define BITS(a,b) ((1U<<((b)+1))-1U)
struct TAILER_COMMON_FORMAT_T {
    unsigned ucFormatVer, ucFormatFlag, ucChipInfo, ucEcoCode;
    char aucRamVersion[10], aucRamBuiltDate[15];
};
struct WIFI_VER_INFO {
    struct TAILER_COMMON_FORMAT_T rCommonTailer;
    struct { char aucPlatform[4], aucBuildDate[16]; unsigned u4PatchVersion; } rPatchHeader;
    char aucReleaseManifest[256], aucFwBranchInfo[4], aucFwDateCode[16];
    unsigned u2FwOwnVersion, ucFwBuildNumber, fgPatchIsDlByDrv, u2FwPeerVersion;
};
struct ADAPTER;
struct FWDL_OPS_T { uint32_t (*getFwDlInfo)(struct ADAPTER *,char *,int); };
struct ADAPTER {
    struct WIFI_VER_INFO rVerInfo;
    struct { struct FWDL_OPS_T *fw_dl_ops; } *chip_info;
};
static int scnprintf(char *buf,size_t size,const char *fmt,...)
{
    if (!size) return 0;
    va_list args; va_start(args,fmt);
    int result=vsnprintf(buf,size,fmt,args);
    va_end(args);
    assert(result>=0);
    return (size_t)result>=size ? (int)size-1 : result;
}
'''
test = r'''
int main(void)
{
    struct ADAPTER a = {0};
    struct FWDL_OPS_T ops = { t70_wlan_chip_info };
    typeof(*a.chip_info) chip = { &ops };
    char storage[512];
    a.chip_info = &chip;
    a.rVerInfo.rCommonTailer.ucFormatFlag = 1;
    memcpy(a.rVerInfo.aucFwBranchInfo, "ABCD", 4);
    memcpy(a.rVerInfo.aucFwDateCode, "2026112310000000", 16);
    memcpy(a.rVerInfo.rCommonTailer.aucRamVersion, "0123456789", 10);
    memcpy(a.rVerInfo.rCommonTailer.aucRamBuiltDate, "202611231111111", 15);
    memcpy(a.rVerInfo.aucReleaseManifest,"release-1d",10);
    memset(storage, '#', sizeof(storage));
    assert(!fwDlGetFwdlInfo(&a, storage+1, 0) && storage[1]=='#');
    assert(!fwDlGetFwdlInfo(&a, storage+1, -1) && storage[1]=='#');
    for (int len=1; len <= 300; len++) {
        memset(storage, '#', sizeof(storage));
        unsigned used=fwDlGetFwdlInfo(&a,storage+1,len);
        assert(used < (unsigned)len && strlen(storage+1)==used);
        assert(storage[0]=='#' && storage[len+1]=='#');
    }
    assert(strstr(storage+1,"ABCD-") && strstr(storage+1,"0123456789 (202611231111111)"));
    assert(strstr(storage+1,"Release manifest: release-1d\n") && strstr(storage+1,"Drv version"));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-wlan-report-") as d:
    c = Path(d) / "test.c"
    c.write_text(prefix + fn(chip, "t70_wlan_chip_info") + "\n" +
                 fn(fw, "fwDlGetFwdlInfo") + test)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("WLAN report: zero, short and complete buffers retain valid bounds and output")
