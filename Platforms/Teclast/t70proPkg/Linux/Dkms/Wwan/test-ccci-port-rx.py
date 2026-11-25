#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check staged CCCI receive admission and ownership, independent of the modem."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#define MAX_MD_NUM 1
#define MD1_NET_HIF 2
#define CLDMA_HIF_ID 0
#define GET_CCMNI_IDX(p) ((p)->idx)
#define CCCI_ERROR_LOG(...) ((void)0)
#define CCCI_DEBUG_LOG(md, tag, fmt, name, ...) log_values(__VA_ARGS__)
struct port_t { int md_id, hif_id, idx; const char *name; };
typedef uint16_t u16;
typedef uint8_t u8;
#define __packed __attribute__((packed))
LHIF_LAYOUT
_Static_assert(sizeof(struct lhif_header) == 4, "stock LHIF wire size");
struct ccci_header { uint32_t data[2]; uint16_t channel, seq; uint32_t reserved; };
struct sk_buff { unsigned char *data, *linear; size_t len; };
static int mbim_ccmni_index[] = { -1 };
static int checked, queued, pulls;
static unsigned logged;
static void log_values(unsigned first, ...) { logged = first; }
static int atomic_read(int *v) { return *v; }
static int pskb_may_pull(struct sk_buff *skb, size_t n)
{
    ++checked;
    if (n > skb->len) return 0;
    skb->data = skb->linear;
    return 1;
}
static void skb_pull(struct sk_buff *skb, size_t n)
{ assert(skb->len >= n); skb->data += n; skb->len -= n; ++pulls; }
static void ccmni_queue_recv_skb(struct port_t *p, struct sk_buff *skb)
{ assert(p->md_id == 0 && skb->len == 1 && *skb->data == 0x45); ++queued; }
'''

TEST = r'''
int main(void)
{
    struct port_t p = { .hif_id=MD1_NET_HIF };
    unsigned char old[32] = {0};
    _Alignas(uint32_t) unsigned char linear[32] = {4, 6, 5, 0xa7, 0x45};
    struct sk_buff b = { .data=old, .linear=linear, .len=3 };
    assert(port_net_recv_skb(NULL, &b) == -EINVAL);
    assert(port_net_recv_skb(&p, NULL) == -EINVAL);
    assert(port_net_recv_skb(&p, &b) == -EMSGSIZE);
    assert(b.data == old && b.len == 3 && !pulls && !queued && checked == 1);
    b.len = 5;
    mbim_ccmni_index[0] = 0;
    assert(port_net_recv_skb(&p, &b) == -EOPNOTSUPP);
    assert(b.data == old && b.len == 5 && !pulls && !queued && checked == 1);
    mbim_ccmni_index[0] = -1;
    assert(port_net_recv_skb(&p, &b) == 0);
    assert(b.data == linear + 4 && logged == 7 && queued == 1 && pulls == 1);
    p.hif_id = 1;
    b.data = old;
    b.len = 15;
    assert(port_net_recv_skb(&p, &b) == -EMSGSIZE);
    assert(b.data == old && b.len == 15 && queued == 1 && pulls == 1);
    b.len = 17;
    linear[16] = 0x45;
    assert(port_net_recv_skb(&p, &b) == 0);
    assert(queued == 2 && pulls == 2 && b.data == linear + 16);
    return 0;
}
'''


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    source = p.parse_args().source
    src = source.read_text()
    header = (source.parents[2] / "mt-plat" / "mtk_ccci_common.h").read_text()
    lhif = re.search(r"(?ms)^struct lhif_header \{.*?^\} __packed;", header)
    if not lhif:
        p.error("missing exact LHIF layout")
    match = re.search(r"(?ms)^static int port_net_recv_skb\([^{}]*\n\{.*?^\}", src)
    if not match:
        p.error("missing staged receive function")
    with tempfile.TemporaryDirectory() as directory:
        c = Path(directory) / "test.c"
        out = c.with_suffix("")
        c.write_text(SHIM.replace('LHIF_LAYOUT', lhif[0]) + match[0] + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print("CCCI RX: short headers rejected without handoff, moved headers reloaded, MBIM rejects, valid frames queued")
