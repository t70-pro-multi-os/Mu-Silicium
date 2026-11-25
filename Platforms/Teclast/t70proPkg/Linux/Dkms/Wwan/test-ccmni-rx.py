#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check staged CCMNI raw-IP delivery and skb ownership without a modem."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ARRAY_SIZE(a) ((int)(sizeof(a)/sizeof(*(a))))
#define MAX_MD_NUM 2
#define MODEM_CAP_NAPI 1
#define likely(x) (x)
#define htons(x) (x)
#define ETH_P_IP 0x0800
#define ETH_P_IPV6 0x86dd
#define HZ 100
struct iphdr { uint8_t data[20]; };
struct ipv6hdr { uint8_t data[40]; };
struct net_device { struct { unsigned rx_packets, rx_bytes, rx_dropped; } stats; };
struct sk_buff { unsigned int len, protocol, freed, queued; uint8_t data[64]; struct net_device *dev; };
struct ccmni_instance { struct net_device *dev; };
struct ccmni_ops { int ccmni_num, md_ability; };
struct ccmni_ctl_block {
    struct ccmni_ops *ccci_ops;
    struct ccmni_instance *ccmni_inst[32];
    void *ccmni_wakelock;
};
#define guard(type) fake_guard_##type
#define fake_guard_rcu() ((void)0)
#define rcu_dereference(p) (p)
static struct ccmni_ctl_block *ccmni_callback_blk[MAX_MD_NUM];
static unsigned int napi, work;
static bool pskb_may_pull(struct sk_buff *s, unsigned int n) { return s->len >= n; }
static void dev_kfree_skb_any(struct sk_buff *s) { assert(!s->queued && !s->freed++); }
#define skb_reset_transport_header(s) ((void)(s))
#define skb_reset_network_header(s) ((void)(s))
#define skb_set_mac_header(s,o) ((void)(s),(void)(o))
#define skb_reset_mac_len(s) ((void)(s))
#define jiffies_to_msecs(t) (t)
#define __pm_wakeup_event(d,t) ((void)(d),(void)(t))
static void netif_receive_skb(struct sk_buff *s) { assert(s->dev && !s->queued++); napi++; }
static void netif_rx(struct sk_buff *s) { assert(s->dev && !s->queued++); work++; }
static int in_interrupt(void) { return 0; }
'''
TEST = r'''
int main(void)
{
    struct sk_buff unavailable = {.len=20, .data={0x45}};
    struct sk_buff v4 = {.len=24, .data={0x45}};
    struct sk_buff v6 = {.len=40, .data={0x60}};
    struct net_device dev = {0};
    struct ccmni_ops ops = {.ccmni_num=1, .md_ability=MODEM_CAP_NAPI};
    struct ccmni_instance inst = {.dev=&dev};
    struct ccmni_ctl_block ctl = {.ccci_ops=&ops, .ccmni_inst={&inst}};
    assert(ccmni_rx_callback(0, 0, &unavailable, 0) == -ENODEV);
    assert(unavailable.freed == 1 && !unavailable.queued && !work && !napi);
    ccmni_callback_blk[0] = &ctl;
    assert(ccmni_rx_callback(0, 0, &v4, 0) == 0 && v4.protocol == ETH_P_IP && napi == 1);
    ops.md_ability = 0;
    assert(ccmni_rx_callback(0, 0, &v6, 0) == 0 && v6.protocol == ETH_P_IPV6 && work == 1);
    assert(!v4.freed && !v6.freed && dev.stats.rx_packets == 2 && dev.stats.rx_bytes == 64);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    callback = re.search(r"(?ms)^static int ccmni_rx_callback\([^{}]*\n\{.*?^\}", src)[0]
    with tempfile.TemporaryDirectory(prefix="t70-ccmni-rx-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callback + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("CCMNI RX callback: absent transport consumes skb once; IPv4 and IPv6 reach proper queues")


if __name__ == "__main__":
    main()
