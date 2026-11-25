#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the pinned CCMNI-port TX handoff against in-memory queue shims."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define CCMNI_ERR_TX_INVAL -3
#define CCMNI_ERR_MD_NO_READY -2
#define CCCI_ERR_MD_NOT_READY 131
#define CCCI_ERR_HIF_NOT_POWER_ON 141
#define CCMNI_ERR_TX_BUSY -1
#define CCMNI_ERR_TX_OK 0
#define CCCI_ERROR_LOG(...) ((void)0)
#define CCCI_DEBUG_LOG(...) ((void)0)
struct port_t { const char *name; };
struct ccmni_ch { int dl_ack, tx; };
struct ccci_header { uint32_t data[2]; uint16_t channel, seq_assert; uint32_t reserved; };
struct sk_buff { unsigned int len, pos; unsigned char storage[96]; };
static struct port_t port = {.name="ccmni"};
static struct ccmni_ch ch = {.dl_ack=31, .tx=43};
static int ready, pressure, get_fail, sends;
static int copy_ch(int md, int idx, struct ccmni_ch *out)
{ assert(md == 0 && idx == 2 && out); if (ready) *out = ch; return ready; }
static const struct { int (*copy_ch)(int,int,struct ccmni_ch *); } ccmni_ops = {copy_ch};
static struct port_t *find_net_port_by_ccmni_idx(int md, int idx)
{ assert(md == 0 && idx == 2); return get_fail ? NULL : &port; }
static int skb_cow_head(struct sk_buff *skb, size_t len)
{ assert(skb && len == 16); return skb->pos < len; }
static void *skb_push(struct sk_buff *skb, size_t n)
{ assert(skb->pos >= n); skb->pos -= n; skb->len += n; return skb->storage + skb->pos; }
static void skb_pull(struct sk_buff *skb, size_t n)
{ skb->pos += n; skb->len -= n; }
static int port_net_send_skb_to_md(struct port_t *p, int ack, struct sk_buff *skb)
{
    struct ccci_header h;
    assert(p == &port && skb->len == 40 && skb->pos == 16);
    memcpy(&h, skb->storage + skb->pos, sizeof(h));
    assert(h.data[0] == 2 && h.data[1] == skb->len);
    assert(h.channel == (ack ? 31 : 43) && !h.seq_assert && !h.reserved);
    assert(skb->storage[32] == 0x45);
    sends++;
    return pressure;
}
'''
TEST = r'''
int main(void)
{
    struct sk_buff s = {.len=24, .pos=32};
    memset(s.storage, 0xa5, sizeof(s.storage));
    s.storage[32] = 0x45;
    assert(ccmni_send_pkt(0, 2, &s, 0) == CCMNI_ERR_TX_INVAL && !sends && s.len == 24);
    ready = 1;
    get_fail = 1;
    assert(ccmni_send_pkt(0, 2, &s, 0) == CCMNI_ERR_TX_INVAL && !sends && s.len == 24);
    get_fail = 0;
    pressure = -EBUSY;
    assert(ccmni_send_pkt(0, 2, &s, 0) == CCMNI_ERR_TX_BUSY && sends == 1);
    assert(s.len == 24 && s.pos == 32 && s.storage[s.pos] == 0x45);
    pressure = 0;
    assert(ccmni_send_pkt(0, 2, &s, 0) == CCMNI_ERR_TX_OK && sends == 2 && s.len == 40);
    s.len = 24;
    s.pos = 32;
    pressure = -ENOSPC;
    assert(ccmni_send_pkt(0, 2, &s, 1) == CCMNI_ERR_TX_BUSY && sends == 3);
    assert(s.len == 24 && s.pos == 32 && s.storage[s.pos] == 0x45);
    pressure = 0;
    assert(ccmni_send_pkt(0, 2, &s, 1) == CCMNI_ERR_TX_OK && sends == 4 && s.len == 40);
    s.len = 24;
    s.pos = 32;
    int not_ready[] = {-ENODEV, -ENETDOWN, -CCCI_ERR_MD_NOT_READY, -CCCI_ERR_HIF_NOT_POWER_ON};
    for (unsigned i = 0; i < sizeof(not_ready)/sizeof(*not_ready); ++i) {
        pressure = not_ready[i];
        assert(ccmni_send_pkt(0, 2, &s, 0) == CCMNI_ERR_MD_NO_READY);
        assert(s.len == 24 && s.pos == 32 && s.storage[32] == 0x45);
    }
    pressure = -EAGAIN;
    assert(ccmni_send_pkt(0, 2, &s, 0) == CCMNI_ERR_TX_BUSY);
    assert(s.len == 24 && s.pos == 32 && s.storage[32] == 0x45);
    int invalid[] = {-EINVAL, -EIO, 1};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        pressure = invalid[i];
        assert(ccmni_send_pkt(0, 2, &s, 0) == CCMNI_ERR_TX_INVAL);
        assert(s.len == 24 && s.pos == 32 && s.storage[32] == 0x45);
    }
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    callback = re.search(r"(?ms)^int ccmni_send_pkt\([^{}]*\n\{.*?^\}", src)[0]
    with tempfile.TemporaryDirectory(prefix="t70-ccci-port-tx-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callback + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("CCCI TX: empty endpoint, network/ACK channels, busy retry, offline/invalid no retry, and skb rollback pass")


if __name__ == "__main__":
    main()
