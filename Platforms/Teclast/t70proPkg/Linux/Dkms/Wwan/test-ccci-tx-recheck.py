#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the staged CCCI stop/recheck and DPMAIF space query together."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#define MD_SYS1 0
#define MD_SYS3 2
#define DPMAIF_HIF_ID 2
#define PORT_F_TX_DATA_FULLED 1
#define PORT_F_TX_ACK_FULLED 2
#define NET_ACK_TXQ_INDEX(p) ((p)->txq_exp_index & 0x0f)
#define GET_CCMNI_IDX(p) ((p)->idx)
#define READ_ONCE(p) __atomic_load_n(&(p), __ATOMIC_RELAXED)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
enum { IN, OUT };
enum { TX_IRQ = 1, TX_FULL };
struct port_t { int md_id, hif_id, txq_index, txq_exp_index, idx, flags, flag_lock; };
struct ccci_hif_ops { int (*empty_query)(int); };
static void *ccci_hif[3];
static struct ccci_hif_ops *ccci_hif_op[3];
static struct port_t port_a = {.hif_id=DPMAIF_HIF_ID, .txq_exp_index=0xf1, .idx=1};
static struct port_t port_b = {.hif_id=DPMAIF_HIF_ID, .txq_exp_index=0xf1, .idx=2};
static int port_md_gen, locked, on_stop, room[2], queries, stops[3][2], wakes[3][2];
static int empty_query(int q) { assert(locked && q >= 0 && q < 2); ++queries; return room[q]; }
static struct ccci_hif_ops tx_ops = {.empty_query=empty_query};
#define spin_lock_irqsave(l, flags) ((flags) = 0, assert(l), assert(!locked), locked = 1)
#define spin_unlock_irqrestore(l, flags) ((void)(flags), assert(l), assert(locked), locked = 0)
static void queue_state(int md, int idx, int state, int ack)
{
    assert(md == 0 && locked && idx >= 1 && idx <= 2 && ack >= 0 && ack <= 1);
    if (state == TX_FULL) {
        ++stops[idx][ack];
        if (on_stop) room[ack] = 1;
    } else {
        assert(state == TX_IRQ);
        ++wakes[idx][ack];
    }
}
static const struct { void (*queue_state_callback)(int,int,int,int); } ccmni_ops = {queue_state};
'''
TEST = r'''
int main(void)
{
    assert(ccci_dpmaif_empty_query(-1) == -EINVAL);
    assert(ccci_dpmaif_empty_query(4) == -EINVAL);
    assert(ccci_dpmaif_empty_query(0) == -ENODEV);
    ccci_hif[DPMAIF_HIF_ID] = &port_a;
    assert(ccci_dpmaif_empty_query(0) == -ENODEV);
    ccci_hif_op[DPMAIF_HIF_ID] = &tx_ops;

    port_net_queue_state_notify(&port_a, OUT, 0, TX_IRQ);
    assert(!stops[1][0] && !wakes[1][0]);
    room[0] = 1;  /* The only completion arrived before the stop. */
    port_net_queue_state_notify(&port_a, OUT, 0, TX_FULL);
    assert(stops[1][0] == 1 && wakes[1][0] == 1 && !port_a.flags);

    room[1] = 0;
    on_stop = 1;  /* Completion publishes room during stop but sends no new IRQ. */
    port_net_queue_state_notify(&port_a, OUT, 1, TX_FULL);
    assert(stops[1][1] == 1 && wakes[1][1] == 1 && !port_a.flags);
    on_stop = 0;
    room[0] = 0;
    port_net_queue_state_notify(&port_a, OUT, 0, TX_FULL);
    port_net_queue_state_notify(&port_b, OUT, 0, TX_FULL);
    port_net_queue_state_notify(&port_b, OUT, 0, TX_FULL);
    assert(stops[1][0] == 2 && stops[2][0] == 1 &&
           port_a.flags == PORT_F_TX_DATA_FULLED && port_b.flags == PORT_F_TX_DATA_FULLED);
    room[0] = 1;  /* Completion arrives after both netdevs stopped on the shared ring. */
    port_net_queue_state_notify(&port_a, OUT, 0, TX_IRQ);
    port_net_queue_state_notify(&port_b, OUT, 0, TX_IRQ);
    port_net_queue_state_notify(&port_b, OUT, 0, TX_IRQ);
    assert(!port_a.flags && !port_b.flags && wakes[1][0] == 2 && wakes[2][0] == 1);
    ccci_hif_op[DPMAIF_HIF_ID] = NULL;
    port_net_queue_state_notify(&port_b, OUT, 1, TX_FULL);
    port_net_queue_state_notify(&port_b, OUT, 2, TX_IRQ);
    assert(port_b.flags == PORT_F_TX_ACK_FULLED && !wakes[2][1] && queries == 5);
}
'''


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("ccci", type=Path)
    ccci = p.parse_args().ccci
    parts = []
    for file, declaration in (
        ("hif/ccci_hif.c", "int ccci_dpmaif_empty_query"),
        ("port/port_net.c", "static void port_net_queue_state_notify"),
    ):
        match = re.search(r"(?ms)^" + declaration + r"\([^{}]*\n\{.*?^\}",
                          (ccci / file).read_text())
        if not match:
            p.error(f"missing staged {declaration}")
        parts.append(match[0])
    with tempfile.TemporaryDirectory() as t:
        c = Path(t) / "test.c"
        out = c.with_suffix("")
        c.write_text(SHIM + "\n".join(parts) + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print("CCCI TX queue: early / during-stop / late completion, shared ring, duplicate and offline pass")
