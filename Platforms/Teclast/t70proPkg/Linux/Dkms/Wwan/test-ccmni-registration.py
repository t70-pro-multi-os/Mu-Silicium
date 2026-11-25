#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the staged CCMNI register/exit pair with a fault-injectable netdev shim."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_MD_NUM 1
#define MODEM_CAP_CCMNI_DISABLE 1
#define MODEM_CAP_CCMNI_IRAT 2
#define MODEM_CAP_CCMNI_MQ 4
#define MODEM_CAP_DIRECT_TETHERING 8
#define ARPHRD_NONE 0xfffe
#define IFF_BROADCAST 2
#define IFF_MULTICAST 4
#define GFP_KERNEL 0
#define CCMNI_INF_MSG(...) ((void)0)
#define CCMNI_DBG_MSG(...) ((void)0)
#define CCMNI_PR_DBG(...) ((void)0)
#define unlikely(x) (x)
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof(*(a))))
#define scnprintf snprintf
static const int ccmni_plain_eth_header_ops;
struct net_device;
struct ccmni_instance { int index; struct net_device *dev; };
struct net_device {
    struct ccmni_instance priv;
    char name[16];
    int type, flags, state, resources;
    const int *header_ops;
};
struct ccmni_ccci_ops {
    int md_ability, ccmni_num, irat_md_id;
    const char *name;
    void (*get_ccmni_ch)(void), (*send_pkt)(void), (*ccci_handle_port_list)(void);
    void (*ccci_net_init)(char *);
};
struct ccmni_ctl_block {
    struct ccmni_instance *ccmni_inst[4];
    struct ccmni_ccci_ops *ccci_ops;
    void *ccmni_wakelock;
    char wakelock_name[16];
};
static struct ccmni_ctl_block *ccmni_ctl_blk[1], *ccmni_callback_blk[1];
static int ccmni_lifetime_lock, grace_done;
#define guard(type) fake_guard_##type
#define fake_guard_mutex(p) ((void)(p))
#define rcu_assign_pointer(p, value) ((p) = (value))
static void synchronize_rcu(void)
{ assert(!ccmni_callback_blk[0] && ccmni_ctl_blk[0]); ++grace_done; }
static int wake_fails, fail_device, fail_register, attempt, registered, freed, ws, port_init, destroyed;
static void *kzalloc(size_t size, int flags) { (void)flags; return calloc(1, size); }
static void kfree(void *p) { free(p); }
static void *wakeup_source_register(void *parent, const char *name)
{
    assert(!parent && name[0] && !ccmni_ctl_blk[0]);
    if (wake_fails) return NULL;
    ++ws; return &ws;
}
static void wakeup_source_unregister(void *w)
{ assert(w == &ws && ws == 1 && !registered); --ws; }
static struct net_device *alloc_etherdev(size_t size)
{
    assert(ws == 1 && ccmni_ctl_blk[0]->ccci_ops->get_ccmni_ch);
    assert(size == sizeof(struct ccmni_instance));
    return ++attempt == fail_device ? NULL : calloc(1, sizeof(struct net_device));
}
static struct net_device *alloc_etherdev_mqs(size_t size, int tx, int rx)
{ assert(tx == 2 && rx == 1); return alloc_etherdev(size); }
static struct ccmni_instance *netdev_priv(struct net_device *d) { return &d->priv; }
static void ccmni_dev_init(int md, struct net_device *d) { assert(md == 0 && d); }
static int ccmni_inst_init(int md, struct ccmni_instance *priv, struct net_device *d)
{ assert(md == 0); priv->dev = d; d->resources = 1; return 0; }
static void ccmni_dev_destroy(struct net_device *d)
{ if (d->resources) { d->resources = 0; ++destroyed; } }
static int register_netdev(struct net_device *d)
{
    assert(ws && !d->state);
    if (attempt == fail_register) { ccmni_dev_destroy(d); return -EIO; }
    d->state = 1; ++registered; return 0;
}
static void free_netdev(struct net_device *d)
{ assert(!d->state && !d->resources); ++freed; free(d); }
static void unregister_netdev(struct net_device *d)
{ assert(ws && d->state == 1 && registered && !ccmni_callback_blk[0] && ccmni_ctl_blk[0]); --registered; ccmni_dev_destroy(d); free(d); }
static void callback(void) {}
static void ccci_net_init(char *name) { assert(name[0] && ws); ++port_init; }
'''

TEST = r'''
int main(void)
{
    struct ccmni_ccci_ops ops = { .name="ccmni", .ccmni_num=2,
        .get_ccmni_ch=callback, .send_pkt=callback, .ccci_net_init=ccci_net_init,
        .ccci_handle_port_list=callback };
    assert(ccmni_init(0, NULL) == -EINVAL);
    wake_fails = 1;
    assert(ccmni_init(0, &ops) == -ENOMEM && !attempt && !ws && !ccmni_ctl_blk[0]);
    wake_fails = 0;
    fail_device = 2;
    assert(ccmni_init(0, &ops) == -ENOMEM && attempt == 2 && !ws && !ccmni_ctl_blk[0]);
    assert(freed == 0 && !registered);
    attempt = fail_device = 0;
    fail_register = 2;
    assert(ccmni_init(0, &ops) == -EIO && freed == 1 && !registered && !ws && !ccmni_ctl_blk[0]);
    attempt = fail_register = port_init = 0;
    ops.md_ability = MODEM_CAP_DIRECT_TETHERING;
    assert(!ccmni_init(0, &ops) && registered == 3 && port_init == 3 && ws == 1);
    assert(ccmni_init(0, &ops) == -EBUSY && registered == 3 && ws == 1);
    assert(ccmni_callback_blk[0] == ccmni_ctl_blk[0] && !grace_done);
    ccmni_exit(0);
    assert(!ccmni_callback_blk[0] && grace_done == 1);
    ccmni_exit(0);
    assert(grace_done == 1);
    assert(!registered && !ws && !ccmni_ctl_blk[0] && destroyed == 6);
    return 0;
}
'''

DESTROY_SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
struct ccmni_instance {
    int pkt_queue_work;
    bool timer_ready, napi_added;
    int *worker, *timer, *napi, *spinlock;
};
struct net_device { struct ccmni_instance p; };
static struct ccmni_instance *netdev_priv(struct net_device *dev) { return &dev->p; }
static int step, frees;
static void cancel_delayed_work_sync(int *work)
{ assert(*work == 11 && step == 0); ++step; }
static void destroy_workqueue(int *work)
{ assert(*work == 12 && step == 1); free(work); ++step; }
static void timer_shutdown_sync(int *timer)
{ assert(*timer == 13 && step == 2); ++step; }
static void netif_napi_del(int *napi)
{ assert(*napi == 14 && step == 3); ++step; }
static void kfree(int *entry)
{ if (entry) { assert(step == 4); ++frees; free(entry); } }
static int *make(int value) { int *p = malloc(sizeof(*p)); assert(p); *p = value; return p; }
'''
DESTROY_TEST = r'''
int main(void)
{
    struct net_device dev = {.p = {.pkt_queue_work=11, .timer_ready=true, .napi_added=true,
        .worker=make(12), .timer=make(13), .napi=make(14), .spinlock=make(15)}};
    ccmni_dev_destroy(&dev);
    assert(step == 4 && frees == 3 && !dev.p.worker && !dev.p.timer &&
           !dev.p.napi && !dev.p.spinlock && !dev.p.timer_ready && !dev.p.napi_added);
    ccmni_dev_destroy(&dev);
    assert(step == 4 && frees == 3);
    dev.p.napi = make(14);
    dev.p.timer = make(13);
    ccmni_dev_destroy(&dev);
    assert(step == 4 && frees == 5 && !dev.p.napi && !dev.p.timer);
}
'''


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    source = p.parse_args().source.read_text()
    match = re.search(r"(?ms)^static int ccmni_init\([^{}]*\n\{.*?^}\n\n"
                      r"static void ccmni_exit\([^{}]*\n\{.*?^}", source)
    destroy = re.search(r"(?ms)^static void ccmni_dev_destroy\([^{}]*\n\{.*?^}", source)
    if not match or not destroy or "dev->needs_free_netdev = true;" not in source:
        p.error("staged init/exit pair or netdev lifetime policy not found")
    with tempfile.TemporaryDirectory() as t:
        c = Path(t) / "test.c"
        out = c.with_suffix("")
        c.write_text(SHIM + match[0] + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
        c.write_text(DESTROY_SHIM + destroy[0] + DESTROY_TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print("CCMNI destructor: drain worker, stop timer, detach NAPI, partial and repeated cleanup")
    print("CCMNI: wake/alloc/register faults unwind, duplicate init rejects, LAN exits")
