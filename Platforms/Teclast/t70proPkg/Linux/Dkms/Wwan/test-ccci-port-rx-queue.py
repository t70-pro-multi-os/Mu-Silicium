#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the staged CCCI closed-port admission under its real queue lock scope."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
struct sk_buff { unsigned state; };
struct sk_buff_head { int lock; unsigned qlen; struct sk_buff *q[64]; };
struct port_t { int is_up, md_id, idx; struct sk_buff_head port_rx_list; };
#define GET_CCMNI_IDX(port) ((port)->idx)
static struct port_t endpoint;
static int locked, open_on_lock, drops, delivered;
#define spin_lock_irqsave(l, f) ((f) = 0, spin_lock(l))
#define spin_unlock_irqrestore(l, f) ((void)(f), spin_unlock(l))
static void spin_lock(int *lock)
{
    assert(lock == &endpoint.port_rx_list.lock && !locked);
    if (open_on_lock) endpoint.is_up = 1;
    locked = 1;
}
static void spin_unlock(int *lock) { assert(lock && locked); locked = 0; }
static int atomic_read(int *v) { assert(locked); return *v; }
static unsigned skb_queue_len(struct sk_buff_head *q) { assert(locked); return q->qlen; }
static int skb_queue_empty(struct sk_buff_head *q) { return q->qlen == 0; }
static void __skb_queue_tail(struct sk_buff_head *q, struct sk_buff *s)
{ assert(locked && q->qlen < 64 && !s->state); s->state = 1; q->q[q->qlen++] = s; }
static void dev_kfree_skb_any(struct sk_buff *s)
{ assert(!locked && !s->state); s->state = 2; ++drops; }
static int receive(int md, int idx, struct sk_buff *s, void *unused)
{
    assert(md == 0 && idx == 0 && unused == NULL && !locked && s->state < 2);
    s->state = 3;
    ++delivered;
    return 0;
}
static const struct { int (*rx_callback)(int,int,struct sk_buff*,void*); } ccmni_ops = {receive};
static void recv_from_port_list(struct port_t *port)
{
    struct sk_buff_head *q = &port->port_rx_list;
    struct sk_buff *s = q->q[0];
    assert(q->qlen > 0 && s->state == 1);
    for (unsigned i = 1; i < q->qlen; i++) q->q[i - 1] = q->q[i];
    --q->qlen;
    receive(port->md_id, port->idx, s, NULL);
}
'''
TEST = r'''
int main(void)
{
    struct sk_buff pending[66] = {0};
    for (unsigned i = 0; i < 64; i++) ccmni_queue_recv_skb(&endpoint, &pending[i]);
    assert(endpoint.port_rx_list.qlen == 64 && !drops && !delivered);
    ccmni_queue_recv_skb(&endpoint, &pending[64]);
    assert(drops == 1 && pending[64].state == 2 && endpoint.port_rx_list.qlen == 64);
    open_on_lock = 1;
    ccmni_queue_recv_skb(&endpoint, &pending[65]);
    assert(drops == 1 && delivered == 65 && !endpoint.port_rx_list.qlen);
    for (unsigned i = 0; i < 64; i++) assert(pending[i].state == 3);
    assert(pending[65].state == 3);
    return 0;
}
'''

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    src = p.parse_args().source.read_text()
    match = re.search(r"(?ms)^#define T70_CCMNI_RX_BACKLOG_PACKETS[^\n]+\n+"
                      r"static void ccmni_queue_recv_skb\([^{}]*\n\{.*?^\}", src)
    if not match:
        p.error("staged CCCI backlog not found")
    with tempfile.TemporaryDirectory() as directory:
        c = Path(directory) / "test.c"
        out = c.with_suffix("")
        c.write_text(SHIM + match[0] + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print("CCCI RX queue: 64 pending, 65th retired once, transition to open drains exactly once")
