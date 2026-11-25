#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the pinned vendor STP/BTIF TX adapter without opening a controller."""
import argparse
from pathlib import Path
import subprocess

BASE = "a71515dbaf0d539ffb36416f14186dad17eef743:mindone/modules/wmt_drv/common_main"
SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
typedef int32_t INT32;
typedef uint32_t UINT32, *PUINT32;
typedef uint8_t *PUINT8;
#define WMT_ERR_FUNC(...) ((void)0)
#define WMT_WARN_FUNC(...) ((void)0)
#define WMT_INFO_FUNC(...) ((void)0)
static struct { unsigned long stpBtifId; } g_stp_btif = {1};
static const uint8_t frame[] = {0x80, 0x00, 0x02, 0x00, 0x01, 0x02, 0x00, 0x00};
static uint8_t wire[3 * sizeof(frame)];
static unsigned int calls, sleeps, wire_len, next_offset, count;
static const int *responses;
static void osal_sleep_ms(unsigned int ms) { assert(ms == 5); sleeps++; }
static int mtk_wcn_btif_write(unsigned long id, const uint8_t *buf, unsigned int len)
{
    int result = calls < count ? responses[calls] : 0;
    assert(id == 1 && buf == &frame[next_offset] && len == sizeof(frame) - next_offset);
    calls++;
    if (result > 0) {
        assert((unsigned int)result <= len);
        memcpy(wire + wire_len, buf, result);
        wire_len += result;
        next_offset += result;
    }
    return result;
}
'''
TEST = r'''
static int tx(const int *script, unsigned int n, unsigned int *written)
{
    calls = sleeps = next_offset = 0;
    responses = script;
    count = n;
    return mtk_wcn_consys_stp_btif_tx((PUINT8)frame, sizeof(frame), written);
}
int main(void)
{
    const int recovered[] = {3, 0, 5}, partial_error[] = {3, -EIO};
    const int partial_stall[] = {3}, whole[] = {sizeof(frame)};
    unsigned int written = 99;
    assert(tx(recovered, 3, &written) == sizeof(frame));
    assert(written == sizeof(frame) && calls == 3 && sleeps == 2);
    assert(wire_len == sizeof(frame) && !memcmp(wire, frame, wire_len));
    wire_len = 0;
    assert(tx(partial_error, 2, &written) == -3);
    assert(!written && wire_len == 3 && !memcmp(wire, frame, 3));
    assert(tx(whole, 1, &written) == sizeof(frame));
    assert(wire_len == 3 + sizeof(frame) && !memcmp(wire + 3, frame, sizeof(frame)));
    wire_len = 0;
    assert(tx(partial_stall, 1, &written) == -3);
    assert(!written && calls == 11 && sleeps == 10 && wire_len == 3);
    wire_len = 0;
    assert(tx(NULL, 0, &written) == 0);
    assert(!written && calls == 11 && sleeps == 10 && !wire_len);
    g_stp_btif.stpBtifId = 0;
    written = 99;
    assert(tx(NULL, 0, &written) == -1 && written == 99 && calls == 0);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference", type=Path)
    p.add_argument("output", type=Path)
    a = p.parse_args()

    def show(file):
        return subprocess.check_output(
            ["git", "-C", a.reference, "show", f"{BASE}/{file}"], text=True)

    btif = show("linux/stp_btif.c")
    begin = btif.index("\nINT32 mtk_wcn_consys_stp_btif_tx(")
    end = btif.index("\nINT32 mtk_wcn_consys_stp_btif_rx(", begin)
    defines = "\n".join(line for line in btif.splitlines()
                        if line.startswith(("#define STP_MAX_PACKAGE_ALLOWED ",
                                            "#define STP_BTIF_TX_RTY_")))
    assert len(defines.splitlines()) == 3
    core = show("core/stp_core.c")
    begin_core = core.index("\nINT32 mtk_wcn_stp_send_data(")
    mand = core.index("|| mtk_wcn_stp_is_btif_mand_mode())", begin_core)
    end_core = core.index("|| mtk_wcn_stp_is_btif_fullset_mode())", mand)
    assert "(*sys_if_tx)" in core[mand:end_core]
    assert "if ((MTKSTP_HEADER_SIZE + length + 2) != ret)" in core[mand:end_core]
    assert "osal_assert(0);\n\t\t\tret = 0;" in core[mand:end_core]
    a.output.with_suffix(".c").write_text(SHIM + defines + btif[begin:end] + TEST)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-sign-compare", str(a.output.with_suffix(".c")),
                    "-o", str(a.output)], check=True)
    subprocess.run([str(a.output)], check=True)
    print("Pinned STP TX: suffix recovery works; exhaustion/error lose the count.")
    print("Full replay duplicates the accepted prefix; zero progress leaves wire empty.")


if __name__ == "__main__":
    main()
