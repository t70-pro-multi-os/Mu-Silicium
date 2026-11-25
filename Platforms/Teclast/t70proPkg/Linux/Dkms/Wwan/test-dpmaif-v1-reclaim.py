#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test staged reclaim using the real MT6789 descriptor and ring declarations."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t dma_addr_t;
typedef int atomic_t;
#define MD_SYS1 0
#define MODEM_CAP_TXBUSY_STOP 2
#define TX_IRQ 3
#define READ_ONCE(x) (x)
#define spin_lock_irqsave(lock, flags) ((flags) = 0, assert(lock), assert(!locked), locked = 1)
#define spin_unlock_irqrestore(lock, flags) ((void)(flags), assert(lock), assert(locked), locked = 0)
struct dpmaif_tx_queue { unsigned index; bool que_started; int tx_budget, tx_lock; };
static struct dpmaif_tx_queue tx = {.index=1, .que_started=true};
static unsigned locked, frees, kicks, last_q, stop_when_free;
static int capabilities = MODEM_CAP_TXBUSY_STOP;
struct sk_buff { unsigned int id; struct sk_buff *next; };
struct sk_buff_head { struct sk_buff *item[8]; unsigned int nr; };
typedef uint32_t u32;
#define __iomem
#define BIT(i) (1u << (i))
#define GENMASK(h, l) ((~0u >> (31 - (h))) & (~0u << (l)))
static unsigned int publication, readback, ops, change_desc_after_read;
static uint32_t *saved_desc; static unsigned int *released;
static uint32_t readl(const void *ptr)
{
    uint32_t v;
    ++ops;
    if (ptr == released && publication)
        readback = 1;
    __builtin_memcpy(&v, ptr, sizeof(v));
    if (ptr == saved_desc && change_desc_after_read) {
        *saved_desc |= BIT(2);
        change_desc_after_read = 0;
    }
    return v;
}
static void writel(uint32_t v, unsigned int *ptr)
{
    assert(ptr == released && !publication);
    *ptr = v;
    publication = v + 1;
}
static int atomic_read(const atomic_t *v) { return *v; }
static void atomic_add(unsigned n, atomic_t *budget)
{ assert(readback && publication == *released + 1); *budget += n; }
static void __skb_queue_head_init(struct sk_buff_head *q) { q->nr = 0; }
static struct sk_buff *__skb_dequeue(struct sk_buff_head *q)
{
    if (!q->nr) return NULL;
    struct sk_buff *first = q->item[0];
    for (unsigned int i = 1; i < q->nr; ++i) q->item[i-1] = q->item[i];
    --q->nr;
    first->next = NULL;
    return first;
}
static int ccci_md_get_cap_by_id(int md)
{ assert(md == MD_SYS1 && !locked); return capabilities; }
static void ccci_free_skb(struct sk_buff *skb)
{
    assert(skb->id == 61 && !locked);
    ++frees;
    if (stop_when_free) tx.que_started = false;
}
static int t70_ccci_net_tx_notify(int q, unsigned state)
{
    assert(!locked && readback && frees > kicks && tx.tx_budget > 1 &&
           state == TX_IRQ);
    ++kicks;
    last_q = q;
    return 1;
}
static void __skb_queue_tail(struct sk_buff_head *q, struct sk_buff *skb)
{
    assert(!publication && q->nr < 8 && skb && !skb->next);
    skb->next = skb; /* Private queue marks the packet as linked. */
    q->item[q->nr++] = skb;
}
'''
TEST = r'''
int main(void)
{
    _Static_assert(sizeof(struct dpmaif_drb_pd) == 8, "stock DRB size");
    _Static_assert(sizeof(struct drb_queue_info) == 16, "stock ring state size");
    struct drb_queue_info ring = {.rd=3, .wr=2, .rel=7, .sz=8};
    struct dpmaif_drb_pd drbs[8] = {[7]={.dtyp=DES_DTYP_MSG},
        [0]={.dtyp=DES_DTYP_PD, .c_bit=1}, [1]={.dtyp=DES_DTYP_PD}};
    struct sk_buff packet = {.id=61};
    struct dpmaif_drb_skb slots[8] = {[7]={.skb=&packet, .is_msg=1},
        [0]={.skb=&packet}, [1]={.skb=&packet, .is_last_one=1}};
    struct sk_buff_head retired = {0}; atomic_t budget = 5;
    released = &ring.rel;
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 7, &budget, &retired) == -EINVAL);
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 8, &budget, &retired) == -EIO);
    assert(!retired.nr && !publication && ring.rel == 7 && budget == 5);
    ring.rd = 8;
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 8, &budget, &retired) == -EIO);
    ring.rd = 2;
    drbs[1].dtyp = 3;
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 8, &budget, &retired) == -EIO);
    drbs[1].dtyp = DES_DTYP_PD;
    slots[1].skb = NULL;
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 8, &budget, &retired) == -EIO);
    slots[1].skb = &packet;
    assert(!retired.nr && !publication && ring.rel == 7 && budget == 5);
    saved_desc = (uint32_t *)&drbs[1];
    change_desc_after_read = 1;
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 8, &budget, &retired) == 3);
    assert(!slots[7].skb && !slots[0].skb && drbs[1].c_bit == 1);
    drbs[1].c_bit = 0;
    assert(__skb_dequeue(&retired) == &packet);
    retired.item[0] = &packet;
    retired.nr = 1;
    assert(retired.nr == 1 && retired.item[0] == &packet && !slots[1].skb);
    assert(ring.rel == 2 && budget == 8 && publication == 3 && readback && ops > 20);
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 8, &budget, &retired) == 0);
    assert(retired.nr == 1 && budget == 8);

    publication = readback = 0;
    ring.rel = 0; ring.rd = ring.wr = 2;
    drbs[0].c_bit = 0; drbs[1].c_bit = 0;
    slots[0].skb = slots[1].skb = &packet;
    slots[0].is_last_one = slots[1].is_last_one = 1;
    budget = 6;
    retired.nr = 0;
    assert(t70_dpmaif_v1_tx_reclaim(&ring, drbs, slots, 8, &budget, &retired) == 2);
    assert(retired.nr == 1 && !slots[0].skb && !slots[1].skb);
    assert(__skb_dequeue(&retired) == &packet);
    slots[0].is_last_one = 0;
    drbs[0].c_bit = 1;

    /* Reset the test map. Q1 completion is visible before its wake. */
    publication = readback = 0;
    ring.rel = 7; ring.wr = ring.rd = 2;
    slots[1].skb = &packet; tx.tx_budget = 5;
    assert(t70_dpmaif_v1_tx_complete(&tx, &ring, drbs, slots, 8) == 3);
    assert(ring.rel == 2 && tx.tx_budget == 8 && frees == 1 && kicks == 1 && last_q == 1);
    assert(t70_dpmaif_v1_tx_complete(&tx, &ring, drbs, slots, 8) == 0);
    assert(frees == 1 && kicks == 1);

    /* A stopped owner neither reads the map nor calls the network port. */
    tx.que_started = false;
    unsigned reads = ops;
    assert(t70_dpmaif_v1_tx_complete(&tx, &ring, drbs, slots, 8) == -ENETDOWN);
    assert(ops == reads && frees == 1 && kicks == 1);
    tx.que_started = true;
    tx.index = 2;
    assert(t70_dpmaif_v1_tx_complete(&tx, &ring, drbs, slots, 8) == -EINVAL);

    /* Closing after reclaim but before wake keeps the callback suppressed. */
    tx.index = 0;
    stop_when_free = 1;
    publication = readback = 0;
    ring.rel = 7; ring.wr = ring.rd = 2;
    slots[1].skb = &packet; tx.tx_budget = 5;
    assert(t70_dpmaif_v1_tx_complete(&tx, &ring, drbs, slots, 8) == 3);
    assert(frees == 2 && kicks == 1 && !tx.que_started);
}
'''


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("hif", type=Path)
    d = p.parse_args().hif
    header = (d / "ccci_hif_dpmaif_v1.h").read_text()
    definitions = [re.search(r"(?ms)^struct " + name + r" \{.*?^\};", header)[0]
                   for name in ("drb_queue_info", "dpmaif_drb_pd", "dpmaif_drb_skb")]
    definitions += [re.search(r"(?m)^#define " + name + r"\s+\S+$", header)[0]
                    for name in ("DES_DTYP_PD", "DES_DTYP_MSG", "DPMA_UL_Q0_SIZE")]
    geometry = (d / "dpmaif-v1-ring-geometry.h").read_text()
    src = (d / "dpmaif-v1-tx-reclaim.c").read_text()
    src = re.sub(r"(?m)^#include .*\n", "", src)
    with tempfile.TemporaryDirectory() as t:
        c = Path(t) / "test.c"
        out = c.with_suffix("")
        c.write_text(SHIM + "\n".join(definitions) + "\n" + geometry + src + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                        "-fsanitize=address,undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print("DPMAIF reclaim: bounded completion, wrap, atomic rejection, one retirement, single descriptor snapshot, duplicate skb once, publish before budget")
    print("DPMAIF worker entry: reclaim before CCCI wake, no duplicates, closing suppresses wake")
