#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise exact stock DPMAIF v1 shared-ring retirement with an in-memory consumer."""
import argparse
import hashlib
import re
import subprocess
import tempfile
from pathlib import Path

SOURCE = "a71515dbaf0d539ffb36416f14186dad17eef743"
PREFIX = "mindone/modules/"

SHIM = r'''
#include <assert.h>
#include <stddef.h>
#define MODEM_CAP_TXBUSY_STOP 1
#define TX_IRQ 2
#define OUT 1
#define DES_DTYP_PD 1
#define DES_DTYP_MSG 0
#define likely(x) (x)
#define CCCI_ERROR_LOG(...) ((void)0)
#define CCCI_NOTICE_LOG(...) ((void)0)
struct drb_queue_info { unsigned sz, rel, rd; };
struct dpmaif_drb_pd { unsigned dtyp, c_bit; };
struct dpmaif_drb_skb { void *skb; };
struct dpmaif_tx_queue { unsigned index, tx_budget; };
static struct hif {
    struct drb_queue_info *smem_drb_qinfo[3];
    struct dpmaif_drb_pd *smem_drb_qbase[3];
    struct dpmaif_drb_skb *dpma_drb_qskb[3];
    struct dpmaif_tx_queue txq[3];
    unsigned wakeup_src;
} ctrl, *dpmaif_ctrl = &ctrl;
static int wakes, frees, closing, cap = MODEM_CAP_TXBUSY_STOP;
static unsigned int visible_rel, wake_q;
static unsigned atomic_read(unsigned *value) { return *value; }
static void atomic_inc(unsigned *value) { ++*value; }
static unsigned atomic_cmpxchg(unsigned *value, unsigned old, unsigned new)
{ unsigned prev = *value; if (prev == old) *value = new; return prev; }
static unsigned ccci_md_get_cap_by_id(int id) { assert(id == 0); return cap; }
static unsigned ringbuf_get_next_idx(unsigned sz, unsigned pos, unsigned count)
{ return (pos + count) % sz; }
static int ringbuf_releasable(unsigned sz, unsigned rel, unsigned rd)
{ return (sz + rd - rel) % sz; }
static void ccci_free_skb(void *skb) { assert(skb); ++frees; }
static void dpmaif_queue_broadcast_state(struct hif *h, int state, int dir, unsigned q)
{
    assert(h == &ctrl && state == TX_IRQ && dir == OUT);
    wake_q = q;
    visible_rel = h->smem_drb_qinfo[q == 3 ? 2 : q]->rel;
    if (!closing) ++wakes;
}
'''

TEST = r'''
int main(void)
{
    struct drb_queue_info state = { .sz = 8, .rel = 7, .rd = 1 };
    struct dpmaif_drb_pd drb[8] = { [0] = { DES_DTYP_PD, 0 } };
    struct dpmaif_drb_skb packet[8] = { [0] = { &state } };
    ctrl.smem_drb_qinfo[2] = &state;
    ctrl.smem_drb_qbase[2] = drb;
    ctrl.dpma_drb_qskb[2] = packet;
    ctrl.txq[2].index = 2;
    assert(dpmaif_smem_release_tx_buffer(2) == 2);
    assert(state.rel == 1 && !packet[0].skb && frees == 1);
    assert(wakes == 1 && wake_q == 3 && visible_rel == EXPECT_REL);
    assert(dpmaif_smem_release_tx_buffer(2) == 0 && wakes == 1 && frees == 1);
    state.rd = 2;
    drb[1].dtyp = DES_DTYP_PD;
    packet[1].skb = &state;
    closing = 1;
    assert(dpmaif_smem_release_tx_buffer(2) == 1 && state.rel == 2);
    assert(wakes == 1 && frees == 2 && !packet[1].skb);
    return 0;
}
'''


def need(condition, detail):
    if not condition:
        raise SystemExit(detail)


def read(repo, name):
    return subprocess.check_output(
        ["git", "-C", str(repo), "show", SOURCE + ":" + PREFIX + name], text=True)


def audit(repo, dtb):
    def prop(node, name, mode="x"):
        return subprocess.check_output(
            ["fdtget", "-t", mode, str(dtb), node, name], text=True).strip()

    dp = "/soc/dpmaif@10014000"
    md = "/soc/mddriver"
    need(prop(dp, "mediatek,dpmaif-ver") == "1"
         and prop(dp, "mediatek,plat-info") == "1a85"
         and prop(md, "compatible", "s") == "mediatek,mddriver"
         and prop(md, "mediatek,md-generation") == "1897"
         and prop(md, "mediatek,mdhif-type") == "6",
         "not the stock MT6789 DPMAIF v1 / modem 6295 binding")
    md_props = subprocess.check_output(["fdtget", "-p", str(dtb), md], text=True)
    proxy = read(repo, "ccci_md_all/port/port_proxy.c")
    cfg = read(repo, "ccci_md_all/port/port_cfg.c")
    port = read(repo, "ccci_md_all/port/port_net.c")
    for macro, expected in {"DATA_TX_Q": 0, "DATA1_TX_Q": 0,
                            "DATA_TX_ACK_Q": 1, "DATA_MDT_Q": 0}.items():
        need(re.search(r"^#define\s+" + macro + r"\s+" + str(expected) + r"$",
                       cfg, re.M), "changed queue " + macro)
    table = cfg.split("static struct port_t md1_ccci_ports[] = {", 1)[1]
    net = table.split("/* char port,", 1)[0]
    rows = re.findall(
        r"\{CCCI_CCMNI\w+_TX,\s*CCCI_CCMNI\w+_RX,\s*(\w+),\s*\w+,\s*"
        r"0xF0\s*\|\s*(\w+),\s*0xFF,\s*MD1_NET_HIF,\s*0,\s*"
        r'&net_port_ops,\s*(\d+),\s*"([^"]+)"', net)
    need(len(rows) == len(re.findall(r"&net_port_ops", net)) == 21
         and {int(i) for _, _, i, _ in rows} == set(range(22)) - {8},
         "changed CCMNI port population")
    need(all((normal, ack) == ("DATA_MDT_Q", "DATA_MDT_Q") if idx == "21"
             else normal in {"DATA_TX_Q", "DATA1_TX_Q"} and ack == "DATA_TX_ACK_Q"
             for normal, ack, idx, _ in rows), "changed CCMNI data/ACK sharing")
    need("return ARRAY_SIZE(md1_ccci_ports_6293);" in cfg
         and "port_md_gen == 6293" in cfg
         and "(port->txq_exp_index&0x0F)" in proxy
         and "proxy_p->queue_ports[hif][qno]" in proxy
         and '"mediatek,md_generation", &port_md_gen' in proxy
         and "mediatek,md_generation" not in md_props.splitlines()
         and "(port_md_gen > 6293) && (state == TX_FULL)" in port,
         "changed port generation lookup or queue recheck")
    hif = read(repo, "include/drivers/misc/mediatek/eccci/inc/ccci_hif.h")
    need(re.search(r"#define\s+MD1_NET_HIF\s+DPMAIF_HIF_ID", hif),
         "CCMNI is not routed to DPMAIF")
    print("MT6789 v1: 20 CCMNI data/ACK on shared Q0/Q1; LAN shares Q0 for both; no CCMNI Q2/Q3.")
    print("Pinned source looks up md_generation on /soc/mddriver; stock DT uses md-generation.")


def audit_binary(stock):
    outputs = {}
    for name, digest in {
        "ccci_md_all": "c7a0435c27c27d1efdd5076ee3d656b383141c6252417e1975ed6d0550e0762b",
        "ccci_dpmaif": "48439cbc6214ca6d0f8aabee7073aa5342f8e2c115bf65d5f69dfc47a8295677",
    }.items():
        binary = stock / (name + ".ko")
        need(hashlib.sha256(binary.read_bytes()).hexdigest() == digest,
             "not the exact stock " + name)
        outputs[name] = binary
    dt_names = subprocess.check_output(
        ["strings", "-a", str(outputs["ccci_md_all"])], text=True).splitlines()
    need("mediatek,md-generation" in dt_names and "mediatek,md_generation" not in dt_names,
         "different stock CCCI property strings")
    reloc = subprocess.check_output(
        ["aarch64-linux-gnu-objdump", "-dr", "--disassemble=dpmaif_smem_release_tx_buffer",
         str(outputs["ccci_dpmaif"])], text=True)
    need(re.search(r"16a4c:\s+R_AARCH64_CALL26\s+dpmaif_start_dev_queue", reloc)
         and re.search(r"16ac4:\s+b9000b7a\s+str\s+w26,\s*\[x27, #8\]", reloc),
         "stock disassembly no longer matches the reviewed wake and rel operations")
    # Stock binary differs from the source: it calls dpmaif_start_dev_queue,
    # not the port broadcast shim. In the exact ELF the wake call is in the
    # loop and rel is stored on exit (rel was loaded from x27+8).
    print("Exact stock ELF uses md-generation and calls its wake helper before the rel store.")


def exercise(repo):
    src = read(repo, "ccci_dpmaif/ccci_hif_dpmaif_v1.c")
    need("ringbuf_writeable(drb_qinfo->sz, drb_qinfo->rel, drb_qinfo->wr)" in src
         and "tx_sw_solution_enable = 1" in src
         and "get_smem_start_addr(0, SMEM_USER_MD_DATA," in src,
         "shared-ring room or selection changed")
    complete = "static int dpmaif_smem_release_tx_buffer(unsigned int qno)\n{"
    body = src[src.index(complete):].split("\nstatic void dpmaif_smem_tx_stop", 1)[0]
    needle = "\t\tatomic_inc(&txq->tx_budget);"
    publish = "\tdrb_qinfo->rel = cur_idx;"
    need(body.count(needle) == body.count(publish) == 1, "retirement source changed")
    # This is a candidate ordering in a host test, not a kernel patch. A real
    # provider also needs default-path recovery, synchronized close, and ring
    # publication ordering against the producer on another CPU.
    candidate = body.replace("\n" + publish + "\n", "\n").replace(
        needle, "\t" + publish + "\n" + needle)
    with tempfile.TemporaryDirectory() as tmp:
        for version, implementation, expected in (("stock", body, 7),
                                                  ("publish-first", candidate, 1)):
            c = Path(tmp) / (version + ".c")
            exe = c.with_suffix("")
            c.write_text(SHIM + implementation + TEST.replace("EXPECT_REL", str(expected)))
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                           check=True)
            subprocess.run([str(exe)], check=True)
    print("Pinned-source wake sees rel=7; publish-first candidate sees rel=1 after wrap.")
    print("Model asserts one retirement, no duplicate wake, and no netdev wake while inactive.")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("kernel", type=Path)
    p.add_argument("dtb", type=Path)
    p.add_argument("stock_modules", type=Path)
    args = p.parse_args()
    audit(args.kernel, args.dtb)
    audit_binary(args.stock_modules)
    exercise(args.kernel)
