#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual HCI transmit callbacks with a transport and queue mock."""
import re
import subprocess
import tempfile
from pathlib import Path

src = Path(__file__).with_name("t70-stp-hci.c").read_text()
head = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
typedef unsigned char u8;
#define HCI_COMMAND_PKT 1
#define HCI_ACLDATA_PKT 2
#define HCI_SCODATA_PKT 3
#define system_dfl_wq 0
#define msecs_to_jiffies(x) (x)
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
static unsigned long jiffies;
#define hci_skb_pkt_type(s) ((s)->type)
#define to_delayed_work(w) ((struct delayed_work *)(w))
#define container_of(ptr, type, member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define mutex_lock(m) ((void)(m))
#define mutex_unlock(m) ((void)(m))
struct work_struct { int dummy; };
struct delayed_work { struct work_struct w; };
struct sk_buff { u8 type; unsigned len,id,freed; };
struct sk_buff_head { struct sk_buff *items[128]; unsigned count; };
struct hci_dev { void *priv; struct { unsigned err_tx, byte_tx; } stat; };
struct t70_h4_rx { int ignored; };
struct t70_stp_bt_link {
    void *priv;
    int (*send)(void *, u8, const struct sk_buff *);
    void (*close)(void *), (*flush)(void *);
};
static unsigned queued, delay, canceled, tries, closed, flushed, sent;
static unsigned last_id[4];
static int provider_ret;
static void *hci_get_drvdata(struct hci_dev *h) { return h->priv; }
static void t70_h4_reset(struct t70_h4_rx *r) { (void)r; }
static void queue_delayed_work(int w,struct delayed_work *d,unsigned n)
{ (void)w;(void)d;queued++;delay=n; }
static void cancel_delayed_work_sync(struct delayed_work *d) { (void)d;canceled++; }
static unsigned skb_queue_len(struct sk_buff_head *h) { return h->count; }
static void skb_queue_tail(struct sk_buff_head *h, struct sk_buff *s) { h->items[h->count++]=s; }
static struct sk_buff *skb_peek(struct sk_buff_head *h) { return h->count ? h->items[0] : NULL; }
static struct sk_buff *skb_dequeue(struct sk_buff_head *h)
{
    struct sk_buff *s=skb_peek(h);
    if (s) {h->count--; for (unsigned i=0;i<h->count;i++) h->items[i]=h->items[i+1];}
    return s;
}
static void kfree_skb(struct sk_buff *s) { assert(s && !s->freed);s->freed++; }
static void skb_queue_purge(struct sk_buff_head *h)
{ while (h->count) kfree_skb(skb_dequeue(h)); }
static int provider(void *p,u8 type,const struct sk_buff *skb)
{
    (void)p; assert(skb->type==type); tries++;
    if (!provider_ret) last_id[sent++]=skb->id;
    return provider_ret;
}
static void close_provider(void *p) { (void)p;closed++; }
static void flush_provider(void *p) { (void)p;flushed++; }
"""
tail = r"""
int main(void)
{
    struct hci_dev h={0};
    struct t70_hci t={.hdev=&h,.opened=true,.link={NULL,provider,close_provider,flush_provider}};
    struct sk_buff a={1,4,1,0},b={2,6,2,0},c={3,2,3,0},d={1,1,4,0};
    struct sk_buff many[T70_TX_DEPTH]={0};
    h.priv=&t;
    assert(t70_send(&h,&a)==0 && t70_send(&h,&b)==0 && queued==2);
    provider_ret=-EAGAIN;
    t70_tx_work(&t.tx_work.w);
    assert(tries==1 && delay==T70_TX_RETRY_MS && t.tx.count==2 && !a.freed && !b.freed);
    provider_ret=-ENOSPC;
    t70_tx_work(&t.tx_work.w);
    assert(tries==2 && t.tx.count==2);
    provider_ret=0;
    t70_tx_work(&t.tx_work.w);
    assert(a.freed && b.freed && !t.tx.count && sent==2 && last_id[0]==1 && last_id[1]==2);
    assert(h.stat.byte_tx==12 && !h.stat.err_tx);
    assert(t70_send(&h,&c)==0);
    provider_ret=-EIO;
    t70_tx_work(&t.tx_work.w);
    assert(c.freed && h.stat.err_tx==1 && !t.tx.count);
    for (unsigned i=0;i<T70_TX_DEPTH;i++) {many[i].type=1;assert(t70_send(&h,&many[i])==0);}
    assert(t70_send(&h,&d)==-ENOBUFS && !d.freed && t.tx.count==T70_TX_DEPTH);
    assert(t70_flush(&h)==0 && flushed==1 && !t.tx.count);
    assert(t70_send(&h,&d)==0);
    assert(t70_close(&h)==0 && canceled==1 && !t.opened && closed==1 && d.freed);
    provider_ret=0;
    t70_tx_work(&t.tx_work.w);
    assert(sent==2 && t70_send(&h,&d)==-ENETDOWN);
    struct sk_buff stuck={1,1,5,0}, next={2,1,6,0}, fresh={1,1,7,0};
    t.opened=true;
    assert(!t70_send(&h,&stuck) && !t70_send(&h,&next));
    provider_ret=-EAGAIN;
    jiffies=20;
    t70_tx_work(&t.tx_work.w);
    assert(t.tx_retrying && t.tx_retry_deadline==1020 && t.tx.count==2);
    jiffies=1019;
    t70_tx_work(&t.tx_work.w);
    assert(t.tx.count==2 && !stuck.freed);
    unsigned before=tries;
    jiffies=1020;
    provider_ret=0;
    t70_tx_work(&t.tx_work.w);
    assert(tries==before+1 && stuck.freed && next.freed && !t.tx.count);
    assert(h.stat.err_tx==2 && sent==3 && last_id[2]==6 && !t.tx_retrying);
    assert(!t70_send(&h,&fresh));
    provider_ret=-ENOSPC;
    t70_tx_work(&t.tx_work.w);
    assert(t.tx_retrying && t.tx_retry_deadline==2020);
    assert(!t70_flush(&h) && !t.tx_retrying && fresh.freed);
    return 0;
}
"""
original = re.search(r"(?ms)^struct t70_hci \{.*?^\};", src)[0]
original = original.replace("struct mutex lock;", "int lock;")
funcs = "\n".join(re.search(r"(?ms)^static (?:int|void) " + name + r"\([^\n]*\n\{.*?^\}", src)[0]
                  for name in ("t70_tx_work", "t70_close", "t70_flush", "t70_send"))
defines = "\n".join(re.findall(r"(?m)^#define T70_TX_[A-Z_]+ \d+$", src))
with tempfile.TemporaryDirectory(prefix="t70-hci-tx-") as tmp:
    main, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    main.write_text(head + defines + "\n" + original + "\n" + funcs + tail)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-o", exe, main], check=True)
    subprocess.run([exe], check=True)
print("PASS ordered backpressure retry, hard error, bounded queue, flush and close")
