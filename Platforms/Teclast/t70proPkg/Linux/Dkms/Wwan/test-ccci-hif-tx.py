#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the staged HIF register and TX dispatch calls with software-owned inputs."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
enum { CLDMA_HIF_ID, CCIF_HIF_ID, DPMAIF_HIF_ID, CCCI_HIF_NUM };
#define READ_ONCE(p) __atomic_load_n(&(p), __ATOMIC_RELAXED)
#define WRITE_ONCE(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELAXED)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define CCCI_NORMAL_LOG(...) ((void)0)
#define CCCI_HISTORY_TAG_LOG(...) ((void)0)
struct sk_buff { int acceptance; };
struct ccci_hif_ops {
    int (*send_skb)(unsigned char, int, struct sk_buff *, int, int);
};
static void *ccci_hif[CCCI_HIF_NUM];
static struct ccci_hif_ops *ccci_hif_op[CCCI_HIF_NUM];
static int calls, result, expected_q = 1, expected_pool;
#define MD_SYS1 0
enum MD_STATE { GATED, READY, EXCEPTION };
struct port_t {
    int md_id;
    unsigned char hif_id, txq_index, txq_exp_index;
    int skb_from_pool;
};
static enum MD_STATE modem_state;
static int fsm_reads;
static enum MD_STATE ccci_fsm_get_md_state(int md)
{ assert(md == MD_SYS1); ++fsm_reads; return modem_state; }
static int send(unsigned char hif, int q, struct sk_buff *skb, int pool, int blocking)
{
    assert(hif == DPMAIF_HIF_ID && q == expected_q && pool == expected_pool && blocking == 0);
    ++calls;
    if (!result) ++skb->acceptance;
    return result;
}
static int ccci_cldma_hif_send_skb(unsigned char hif, int q, struct sk_buff *skb,
                                  int pool, int blocking)
{
    assert(hif == CLDMA_HIF_ID && q == 1 && skb && !pool && !blocking);
    return -ENETDOWN;
}
'''
TEST = r'''
int main(void)
{
    struct sk_buff packet = {0};
    struct ccci_hif_ops empty = {0}, tx = {.send_skb=send};
    int owner;
    assert(ccci_hif_send_skb(255, 1, &packet, 0, 0) == -EINVAL);
    assert(ccci_hif_send_skb(DPMAIF_HIF_ID, -1, &packet, 0, 0) == -EINVAL);
    assert(ccci_hif_send_skb(DPMAIF_HIF_ID, 1, NULL, 0, 0) == -EINVAL);
    assert(ccci_hif_send_skb(CCIF_HIF_ID, 1, &packet, 0, 0) == -ENODEV);
    assert(ccci_hif_send_skb(DPMAIF_HIF_ID, 1, &packet, 0, 0) == -ENODEV);
    assert(ccci_hif_send_skb(CLDMA_HIF_ID, 1, &packet, 0, 0) == -ENETDOWN);
    ccci_hif_register(DPMAIF_HIF_ID, &owner, NULL);
    assert(ccci_hif_send_skb(DPMAIF_HIF_ID, 1, &packet, 0, 0) == -ENODEV);
    ccci_hif_register(DPMAIF_HIF_ID, &owner, &empty);
    assert(ccci_hif_send_skb(DPMAIF_HIF_ID, 1, &packet, 0, 0) == -ENODEV);
    assert(!packet.acceptance && !calls);
    ccci_hif_register(DPMAIF_HIF_ID, &owner, &tx);
    result = -EBUSY;
    assert(ccci_hif_send_skb(DPMAIF_HIF_ID, 1, &packet, 0, 0) == -EBUSY);
    assert(calls == 1 && !packet.acceptance);
    result = 0;
    assert(ccci_hif_send_skb(DPMAIF_HIF_ID, 1, &packet, 0, 0) == 0);
    assert(calls == 2 && packet.acceptance == 1);
    struct port_t port = {.md_id=MD_SYS1, .hif_id=DPMAIF_HIF_ID,
                          .txq_index=0, .txq_exp_index=0xf1, .skb_from_pool=1};
    assert(port_net_send_skb_to_md(NULL, 0, &packet) == -EINVAL && !fsm_reads);
    assert(port_net_send_skb_to_md(&port, 2, &packet) == -EINVAL && !fsm_reads);
    assert(port_net_send_skb_to_md(&port, 0, NULL) == -EINVAL && !fsm_reads);
    port.hif_id = CCIF_HIF_ID;
    assert(port_net_send_skb_to_md(&port, 0, &packet) == -ENODEV && !fsm_reads);
    port.hif_id = DPMAIF_HIF_ID;
    for (int state = GATED; state <= EXCEPTION; state += 2) {
        modem_state = (enum MD_STATE)state;
        assert(port_net_send_skb_to_md(&port, 1, &packet) == -ENODEV);
    }
    assert(calls == 2 && fsm_reads == 2);
    modem_state = READY;
    port.txq_index = 2;
    assert(port_net_send_skb_to_md(&port, 0, &packet) == -EINVAL);
    port.txq_index = 0;
    port.txq_exp_index = 0xff;
    assert(port_net_send_skb_to_md(&port, 1, &packet) == -EINVAL);
    port.txq_exp_index = 0xf1;
    expected_q = 0;
    expected_pool = 1;
    assert(port_net_send_skb_to_md(&port, 0, &packet) == 0);
    expected_q = 1;
    result = -EBUSY;
    assert(port_net_send_skb_to_md(&port, 1, &packet) == -EBUSY);
    result = 0;
    assert(port_net_send_skb_to_md(&port, 1, &packet) == 0);
    assert(calls == 5 && packet.acceptance == 3 && fsm_reads == 7);
}
'''

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("proxy", type=Path)
    args = p.parse_args()
    src = args.source.read_text()
    functions = (re.search(pattern, src) for pattern in (
        r"(?ms)^int ccci_hif_send_skb\([^{}]*\n\{.*?^\}",
        r"(?ms)^void ccci_hif_register\([^{}]*\n\{.*?^\}",
    ))
    parts = [part[0] if part else p.error("staged HIF function missing") for part in functions]
    bridge = re.search(r"(?ms)^int port_net_send_skb_to_md\([^{}]*\n\{.*?^\}",
                       args.proxy.read_text())
    if not bridge:
        p.error("staged CCCI network TX bridge missing")
    parts.append(bridge[0])
    with tempfile.TemporaryDirectory() as directory:
        c = Path(directory) / "test.c"
        out = c.with_suffix("")
        c.write_text(SHIM + "\n".join(parts) + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print("CCCI HIF: missing provider/ops reject, CLDMA error passes, busy retry accepts once")
    print("CCCI port → HIF: Q0 data, Q1 ACK, no modem exception dispatch or invalid queue")
