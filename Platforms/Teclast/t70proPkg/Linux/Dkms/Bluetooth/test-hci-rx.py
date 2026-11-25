#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual HCI RX callbacks with allocator and HCI core fault shims."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "t70-h4-rx.h"
typedef unsigned char u8;
#define GFP_ATOMIC 0
#define hci_skb_pkt_type(skb) ((skb)->type)
struct sk_buff { u8 type, data[16]; size_t len; };
struct hci_dev { struct { unsigned int err_rx; } stat; };
struct t70_hci { struct hci_dev *hdev; struct t70_h4_rx rx; };
static struct sk_buff skb;
static unsigned int failures, rejects, allocated, consumed, accepted, live;
static struct sk_buff *bt_skb_alloc(size_t len, int flags)
{
    (void)flags;
    assert(!live && len <= sizeof(skb.data));
    if (failures) { failures--; return NULL; }
    live++;
    allocated++;
    return &skb;
}
static void skb_put_data(struct sk_buff *s, const u8 *data, size_t len)
{
    assert(s == &skb && live == 1);
    memcpy(s->data, data, len);
    s->len = len;
}
static int hci_recv_frame(struct hci_dev *dev, struct sk_buff *s)
{
    (void)dev;
    assert(s == &skb && live-- == 1 && s->type == T70_H4_EVENT);
    consumed++;
    if (rejects) { rejects--; return -ENXIO; }
    assert(s->len == 3 && s->data[0] == 0x0e && s->data[1] == 1);
    assert(s->data[2] == 0xb9);
    accepted++;
    return 0;
}
'''
TEST = r'''
int main(void)
{
    const u8 two_frames[] = {4,0x0e,1,0xa2, 4,0x0e,1,0xb9};
    const u8 bad_then_valid[] = {0xff, 4,0x0e,1,0xb9};
    struct hci_dev h = {0};
    struct t70_hci t = {.hdev = &h};
    size_t used;
    int ret;
    failures = 1;
    ret = t70_receive(&t, two_frames, sizeof(two_frames), &used);
#ifdef EXPECT_UNFIXED
    return ret == -ENOMEM && used == 4 && !accepted ? 54 : 1;
#endif
    assert(!ret && used == sizeof(two_frames) && h.stat.err_rx == 1 && accepted == 1);
    rejects = 1;
    assert(t70_receive(&t, two_frames, 3, &used) == 0 && used == 3);
    assert(t70_receive(&t, two_frames + 3, 5, &used) == 0 && used == 5);
    assert(h.stat.err_rx == 2 && accepted == 2 && !live && allocated == consumed);
    assert(t70_receive(&t, bad_then_valid, sizeof(bad_then_valid), &used) == -EPROTO);
    assert(used == 1 && accepted == 2 && h.stat.err_rx == 2);
    assert(t70_receive(&t, two_frames + 4, 4, &used) == 0 && used == 4 && accepted == 3);
    return 0;
}
'''


def main():
    root = Path(__file__).resolve().parent
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=root / "t70-stp-hci.c")
    p.add_argument("--expect-unfixed", action="store_true")
    a = p.parse_args()
    src = a.source.read_text()
    funcs = "\n".join(re.search(r"(?ms)^static int " + name + r"\([^\n]*\n\{.*?^\}", src)[0]
                      for name in ("t70_deliver", "t70_receive"))
    with tempfile.TemporaryDirectory(prefix="t70-hci-rx-") as tmp:
        main, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        main.write_text(SHIM + funcs + TEST)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-I", str(root),
                        *(["-DEXPECT_UNFIXED"] if a.expect_unfixed else []),
                        "-o", exe, main], check=True)
        result = subprocess.run([exe])
    if a.expect_unfixed:
        assert result.returncode == 54, result
        print("Original RX drops later complete H4 frame on one allocation failure")
    else:
        result.check_returncode()
        print("RX allocation/core rejection preserve next frame; bad H4 still stops parsing")


if __name__ == "__main__":
    main()
