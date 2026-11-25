#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise TX queue-to-port fanout against the observed T70 table shape."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#define CCCI_NET_MINOR_BASE 200
#define MD_SYS1 0
#define DPMAIF_HIF_ID 2
#define OUT 1
#define TX_FULL 2
#define TX_IRQ 3
#define READY 4
struct port_t;
struct port_ops { void (*queue_state_notify)(struct port_t *, int, int, unsigned); };
struct port_t { struct port_ops *ops; int md_id, hif_id, minor, txq_index, txq_exp_index; };
static int state = READY, enabled = 1, delivered[22], lookups;
static void notify(struct port_t *port, int dir, int q, unsigned status)
{
    int idx = port->minor - CCCI_NET_MINOR_BASE;
    assert(dir == OUT && (q == 0 || q == 1) && (status == TX_FULL || status == TX_IRQ));
    assert(idx >= 0 && idx < 22 && idx != 8);
    ++delivered[idx];
}
static struct port_ops net_port_ops = {notify}, ctl_port_ops = {NULL};
static struct port_t ports[23];
static int ccci_fsm_get_md_state(int md)
{ assert(md == MD_SYS1); return state; }
static int port_get_cfg(int md, struct port_t **p)
{ assert(md == MD_SYS1); ++lookups; *p = enabled ? ports : NULL; return enabled ? 23 : 0; }
'''
TEST = r'''
int main(void)
{
    for (int j = 0; j < 22; ++j)
        ports[j] = (struct port_t){.ops=&net_port_ops, .hif_id=DPMAIF_HIF_ID,
            .minor = j + CCCI_NET_MINOR_BASE, .txq_exp_index=j == 21 ? 0xf0 : 0xf1};
    ports[8].minor = 8;    /* No initialized socket at the reserved slot. */
    ports[22].ops = &ctl_port_ops;
    assert(t70_ccci_net_tx_notify(2, TX_IRQ) == -EINVAL && !lookups);
    assert(t70_ccci_net_tx_notify(0, 5) == -EINVAL && !lookups);
    state = 0;
    assert(t70_ccci_net_tx_notify(0, TX_IRQ) == -ENETDOWN && !lookups);
    state = READY;
    enabled = 0;
    assert(t70_ccci_net_tx_notify(0, TX_IRQ) == -ENODEV && lookups == 1);
    enabled = 1;
    assert(t70_ccci_net_tx_notify(0, TX_FULL) == 21);
    assert(t70_ccci_net_tx_notify(1, TX_IRQ) == 20 && delivered[21] == 1);
    for (int j = 0; j < 21; ++j) assert(delivered[j] == (j == 8 ? 0 : 2));
    ports[0].txq_exp_index = 0xff;
    ports[1].hif_id = 0;
    assert(t70_ccci_net_tx_notify(1, TX_IRQ) == 18 && delivered[0] == 2 && delivered[1] == 2);
}
'''


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    src = p.parse_args().source.read_text()
    match = re.search(r"(?ms)^int t70_ccci_net_tx_notify\([^{}]*\n\{.*?^\}", src)
    if not match:
        p.error("missing staged T70 CCCI net fanout")
    with tempfile.TemporaryDirectory() as t:
        c = Path(t) / "test.c"
        exe = c.with_suffix("")
        c.write_text(SHIM + match[0] + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("CCCI fanout: Q0 to 21, Q1 to 20, excludes unready/foreign ports and invalid IRQs")
