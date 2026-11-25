#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the genuine CCMNI netdev core for target object-only checking."""
import argparse
import posixpath
import re
from pathlib import Path
import subprocess

REV = "a71515dbaf0d539ffb36416f14186dad17eef743"
BASE = "mindone/modules"
INC = BASE + "/include/drivers/misc/mediatek"
FILES = {
    "ccmni.c": BASE + "/ccmni/ccmni.c",
    "ccmni.h": INC + "/ccmni/ccmni.h",
    "ccci_debug.h": INC + "/eccci/inc/ccci_debug.h",
    "rps_perf.h": INC + "/rps/rps_perf.h",
    "mt-plat/mtk_ccci_common.h": INC + "/include/mt-plat/mtk_ccci_common.h",
}


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"unexpected pinned CCMNI source near {old.strip()}")
    return text.replace(old, new)


def stage_rx_admission(src):
    a = src.index("\nstatic int ccmni_rx_callback(")
    b = src.index("\n\t//skb->ip_summed", a)
    body = src[a:b]
    beg = body.index("\n\tif (md_id < 0")
    if "pkt_type = skb->data[0] & 0xF0;" not in body[beg:]:
        raise ValueError("unexpected CCMNI RX packet admission")
    body = body[:beg]
    body += """
	/* The CCCI port transfers skb ownership even when RX rejects it. */
	if (!skb)
		return -EINVAL;
	if (md_id < 0 || md_id >= MAX_MD_NUM || ccmni_idx < 0) {
		dev_kfree_skb_any(skb);
		return -EINVAL;
	}
	ctlb = ccmni_ctl_blk[md_id];
	if (!ctlb || !ctlb->ccci_ops ||
	    ccmni_idx >= ctlb->ccci_ops->ccmni_num ||
	    ccmni_idx >= ARRAY_SIZE(ctlb->ccmni_inst)) {
		dev_kfree_skb_any(skb);
		return -ENODEV;
	}
	ccmni = ctlb->ccmni_inst[ccmni_idx];
	if (!ccmni || !ccmni->dev) {
		dev_kfree_skb_any(skb);
		return -ENODEV;
	}
	dev = ccmni->dev;

	if (!pskb_may_pull(skb, 1))
		goto invalid;
	pkt_type = skb->data[0] & 0xf0;
	if (pkt_type == 0x60) {
		if (!pskb_may_pull(skb, sizeof(struct ipv6hdr)))
			goto invalid;
		skb->protocol = htons(ETH_P_IPV6);
	} else if (pkt_type == 0x40) {
		if (!pskb_may_pull(skb, sizeof(struct iphdr)))
			goto invalid;
		skb->protocol = htons(ETH_P_IP);
	} else {
		goto invalid;
	}

	skb_reset_transport_header(skb);
	skb_reset_network_header(skb);
	skb_set_mac_header(skb, 0);
	skb_reset_mac_len(skb);
	skb->dev = dev;

"""
    src = src[:a] + body + src[b:]
    end = src.index("\n}\n", a)
    rx = replace_once(src[a:end], "\n\treturn 0;", """
	return 0;

invalid:
	dev->stats.rx_dropped++;
	dev_kfree_skb_any(skb);
	return -EPROTO;""")
    return src[:a] + rx + src[end:]


def stage_callback_bounds(src):
    helper = """
static struct ccmni_instance *ccmni_callback_instance(
	struct ccmni_ctl_block *ctlb, int index)
{
	struct ccmni_instance *instance;

	if (!ctlb || !ctlb->ccci_ops || index < 0 ||
	    index >= ARRAY_SIZE(ctlb->ccmni_inst))
		return NULL;
	/* Init registers the optional tethering LAN at ccmni_num, after
	 * the modem channels. All other out-of-range slots stay invalid.
	 */
	if (index >= ctlb->ccci_ops->ccmni_num &&
	    (index != ctlb->ccci_ops->ccmni_num ||
	     !(ctlb->ccci_ops->md_ability & MODEM_CAP_DIRECT_TETHERING)))
		return NULL;
	instance = ctlb->ccmni_inst[index];
	return instance && instance->dev ? instance : NULL;
}
"""
    a = src.index("\nstatic void ccmni_queue_state_callback(")
    b = src.index("\nstruct ccmni_dev_ops ccmni_ops", a)
    callbacks = src[a:b]
    old = "\tccmni_tmp = ctlb->ccmni_inst[ccmni_idx];\n\tdev = ccmni_tmp->dev;"
    new = ("\tccmni_tmp = ccmni_callback_instance(ctlb, ccmni_idx);\n"
           "\tif (!ccmni_tmp)\n\t\treturn;\n\tdev = ccmni_tmp->dev;")
    if callbacks.count(old) != 2:
        raise ValueError("unexpected CCMNI state callback lookup")
    callbacks = callbacks.replace(old, new)
    callbacks = replace_once(callbacks,
        "\tccmni_tmp = ctlb->ccmni_inst[ccmni_idx];\n\tif (unlikely(ccmni_tmp == NULL))",
        "\tccmni_tmp = ccmni_callback_instance(ctlb, ccmni_idx);\n\tif (!ccmni_tmp)")
    a_status = callbacks.index("\nstatic void ccmni_dump_rx_status(")
    a_channel = callbacks.index("\nstatic struct ccmni_ch *ccmni_get_ch(", a_status)
    status = replace_once(callbacks[a_status:a_channel],
                          "\tif (md_id < 0", "\tif (!status)\n\t\treturn;\n\tif (md_id < 0")
    channel = replace_once(callbacks[a_channel:], "\tstruct ccmni_ctl_block *ctlb = NULL;",
        "\tstruct ccmni_ctl_block *ctlb = NULL;\n\tstruct ccmni_instance *instance;")
    channel = replace_once(channel, "\treturn &ctlb->ccmni_inst[ccmni_idx]->ch;",
        "\tinstance = ccmni_callback_instance(ctlb, ccmni_idx);\n"
        "\treturn instance ? &instance->ch : NULL;")
    return src[:a] + helper + callbacks[:a_status] + status + channel + src[b:]


def stage_callback_lifetime(src):
    src = replace_once(src, "#include <linux/module.h>\n",
        "#include <linux/module.h>\n#include <linux/mutex.h>\n#include <linux/rcupdate.h>\n")
    anchor = "struct ccmni_ctl_block *ccmni_ctl_blk[MAX_MD_NUM];"
    src = replace_once(src, anchor, anchor + """
/* netdev ops need the internal array through unregister and NAPI drain.
 * External non-sleeping callbacks use the independently published view.
 */
static struct ccmni_ctl_block __rcu *ccmni_callback_blk[MAX_MD_NUM];
static DEFINE_MUTEX(ccmni_lifetime_lock);
""")
    a = src.index("\nstatic int ccmni_init(")
    b = src.index("\nstatic void ccmni_exit(", a)
    c = src.index("\nstatic int ccmni_rx_callback(", b)
    init = replace_once(src[a:b], "\tif (ccmni_ctl_blk[md_id])",
        "\tguard(mutex)(&ccmni_lifetime_lock);\n\tif (ccmni_ctl_blk[md_id])")
    init = replace_once(init, "\treturn 0;\n\nalloc_netdev_fail:",
        "\trcu_assign_pointer(ccmni_callback_blk[md_id], ctlb);\n"
        "\treturn 0;\n\nalloc_netdev_fail:")
    init = replace_once(init,
        "\tkfree(ctlb->ccci_ops);\n\tkfree(ctlb);\n\n\tccmni_ctl_blk[md_id] = NULL;",
        "\tccmni_ctl_blk[md_id] = NULL;\n\tkfree(ctlb->ccci_ops);\n\tkfree(ctlb);")
    exit_body = replace_once(src[b:c],
        "\tctlb = ccmni_ctl_blk[md_id];\n\tif (ctlb) {",
        "\tguard(mutex)(&ccmni_lifetime_lock);\n"
        "\tctlb = ccmni_ctl_blk[md_id];\n\tif (ctlb) {\n"
        "\t\trcu_assign_pointer(ccmni_callback_blk[md_id], NULL);\n"
        "\t\tsynchronize_rcu();")
    exit_body = replace_once(exit_body,
        "\t\tkfree(ctlb->ccci_ops);\n\nccmni_exit_ret:",
        "\nccmni_exit_ret:\n\t\tccmni_ctl_blk[md_id] = NULL;\n\t\tkfree(ctlb->ccci_ops);")
    exit_body = replace_once(exit_body, "\t\tkfree(ctlb);\n\t\tccmni_ctl_blk[md_id] = NULL;",
        "\t\tkfree(ctlb);")
    end = src.index("\nstruct ccmni_dev_ops ccmni_ops", c)
    callbacks = src[c:end]
    old = "\tctlb = ccmni_ctl_blk[md_id];"
    if callbacks.count(old) != 6:
        raise ValueError("unexpected number of external CCMNI lookups")
    callbacks = callbacks.replace(old, "\tguard(rcu)();\n"
        "\tctlb = rcu_dereference(ccmni_callback_blk[md_id]);")
    old_fn = "static struct ccmni_ch *ccmni_get_ch(int md_id, int ccmni_idx)"
    split = callbacks.index(old_fn)
    get_ch = callbacks[split:].replace(old_fn,
        "static bool ccmni_get_ch(int md_id, int ccmni_idx, struct ccmni_ch *out)")
    get_ch = replace_once(get_ch, "\tif (md_id < 0",
        "\tif (!out)\n\t\treturn false;\n\tif (md_id < 0")
    get_ch = get_ch.replace("return NULL;", "return false;")
    get_ch = replace_once(get_ch, "\treturn instance ? &instance->ch : NULL;",
        "\tif (!instance)\n\t\treturn false;\n\t*out = instance->ch;\n\treturn true;")
    tail = replace_once(src[end:], "\t.get_ch = ccmni_get_ch,", "\t.copy_ch = ccmni_get_ch,")
    return src[:a] + init + exit_body + callbacks[:split] + get_ch + tail


def stage_registration(src):
    a = src.index("\nstatic int ccmni_init(")
    b = src.index("\nstatic int ccmni_rx_callback(", a)
    reg = src[a:b]
    reg = replace_once(reg,
        "\tctlb = kzalloc(sizeof(struct ccmni_ctl_block), GFP_KERNEL);",
        "\tif (ccmni_ctl_blk[md_id])\n\t\treturn -EBUSY;\n"
        "\tif (ccci_info->md_ability & MODEM_CAP_CCMNI_IRAT)\n\t\treturn -EOPNOTSUPP;\n"
        "\tif (!ccci_info->get_ccmni_ch || !ccci_info->send_pkt ||\n"
        "\t    !ccci_info->ccci_net_init || !ccci_info->ccci_handle_port_list ||\n"
        "\t    ccci_info->ccmni_num <= 0 ||\n"
        "\t    ccci_info->ccmni_num > ARRAY_SIZE(ctlb->ccmni_inst) ||\n"
        "\t    ((ccci_info->md_ability & MODEM_CAP_DIRECT_TETHERING) &&\n"
        "\t     ccci_info->ccmni_num == ARRAY_SIZE(ctlb->ccmni_inst)))\n"
        "\t\treturn -EINVAL;\n"
        "\n\tctlb = kzalloc(sizeof(struct ccmni_ctl_block), GFP_KERNEL);")
    reg = replace_once(reg,
        "\tif (unlikely(ccci_info->md_ability & MODEM_CAP_CCMNI_DISABLE)) {",
        "\tif (!ccci_info)\n\t\treturn -EINVAL;\n"
        "\tif (unlikely(ccci_info->md_ability & MODEM_CAP_CCMNI_DISABLE)) {")
    wake_start = reg.index("\tscnprintf(ctlb->wakelock_name,")
    wake_end = reg.index("\n\n\treturn 0;\n\nalloc_netdev_fail:", wake_start)
    wake = reg[wake_start:wake_end]
    wake = replace_once(wake, "\t\treturn -1;",
                        "\t\tret = -ENOMEM;\n\t\tgoto alloc_mem_fail;")
    reg = reg[:wake_start] + reg[wake_end:]
    reg = replace_once(reg, "\tccmni_ctl_blk[md_id] = ctlb;\n\n"
                       "\tmemcpy(ctlb->ccci_ops, ccci_info, sizeof(struct ccmni_ccci_ops));",
                       wake + "\n\n"
                       "\tmemcpy(ctlb->ccci_ops, ccci_info, sizeof(struct ccmni_ccci_ops));\n"
                       "\tccmni_ctl_blk[md_id] = ctlb;")
    reg = replace_once(reg, "\t\tctlb->ccci_ops->ccci_net_init(dev->name);\n\t}",
                       "\t\tctlb->ccci_ops->ccci_net_init(dev->name);\n\t\tdev = NULL;\n\t}")
    reg = replace_once(reg, "\t\t\tgoto alloc_netdev_fail;\n\t\t}\n\t}\n\n\n\treturn 0;",
                       "\t\t\tgoto alloc_netdev_fail;\n\t\t}\n"
                       "\t\tctlb->ccci_ops->ccci_net_init(dev->name);\n\t}\n\n\treturn 0;")
    reg = replace_once(reg, "alloc_mem_fail:\n\tkfree(ctlb->ccci_ops);",
                       "alloc_mem_fail:\n\tif (ctlb->ccmni_wakelock)\n"
                       "\t\twakeup_source_unregister(ctlb->ccmni_wakelock);\n"
                       "\tkfree(ctlb->ccci_ops);")
    exit_at = reg.index("\nstatic void ccmni_exit(")
    reg = reg[:exit_at] + replace_once(reg[exit_at:],
                       "\t\tfor (i = 0; i < ctlb->ccci_ops->ccmni_num; i++) {",
                       "\t\tfor (i = 0; i < ARRAY_SIZE(ctlb->ccmni_inst); i++) {")
    reg = replace_once(reg, "ccmni_exit_ret:\n\t\tkfree(ctlb);",
                       "ccmni_exit_ret:\n\t\tif (ctlb->ccmni_wakelock)\n"
                       "\t\t\twakeup_source_unregister(ctlb->ccmni_wakelock);\n"
                       "\t\tkfree(ctlb);")
    before = replace_once(src[:a], "dev->priv_destructor = free_netdev;",
                          "dev->needs_free_netdev = true;")
    return before + reg + src[b:]


def stage_instance_lifetime(files, src):
    hdr = files["ccmni.h"].decode()
    hdr = replace_once(hdr, "\tstruct delayed_work pkt_queue_work;\n",
                       "\tstruct delayed_work pkt_queue_work;\n"
                       "\tbool timer_ready;\n\tbool napi_added;\n")
    files["ccmni.h"] = hdr.encode()
    a = src.index("\nstatic inline int ccmni_inst_init(")
    b = src.index("\nstatic inline void ccmni_dev_init(", a)
    init = src[a:b]
    start = init.index("\tccmni->napi = kzalloc(")
    end = init.index("\tccmni->ack_prio_en", start)
    init = init[:start] + """
	ccmni->napi = kzalloc(sizeof(*ccmni->napi), GFP_KERNEL);
	if (!ccmni->napi)
		return -ENOMEM;
	ccmni->timer = kzalloc(sizeof(*ccmni->timer), GFP_KERNEL);
	if (!ccmni->timer)
		return -ENOMEM;
	ccmni->spinlock = kzalloc(sizeof(*ccmni->spinlock), GFP_KERNEL);
	if (!ccmni->spinlock)
		return -ENOMEM;
""" + init[end:]
    start = init.index("\t/* register napi device */")
    end = init.index("\n\tatomic_set(&ccmni->usage", start)
    napi = """
	/* Generic CCMNI stages the non-GRO path. Initialize NAPI only when used. */
	if (ctlb->ccci_ops->md_ability & MODEM_CAP_NAPI) {
		timer_setup(ccmni->timer, ccmni_napi_poll_timeout, 0);
		ccmni->timer_ready = true;
		netif_napi_add(dev, ccmni->napi, ccmni_napi_poll);
		ccmni->napi_added = true;
	}
"""
    init = init[:start] + init[end:]
    init = replace_once(init, "\t\treturn -1;\n\t}\n\tINIT_DELAYED_WORK",
                        "\t\treturn -ENOMEM;\n\t}\n\tINIT_DELAYED_WORK")
    init = replace_once(init, "\tINIT_DELAYED_WORK(&ccmni->pkt_queue_work, get_queued_pkts);\n",
                        "\tINIT_DELAYED_WORK(&ccmni->pkt_queue_work, get_queued_pkts);\n" + napi)
    destroy = """
static void ccmni_dev_destroy(struct net_device *dev)
{
	struct ccmni_instance *ccmni = netdev_priv(dev);

	if (ccmni->worker) {
		cancel_delayed_work_sync(&ccmni->pkt_queue_work);
		destroy_workqueue(ccmni->worker);
		ccmni->worker = NULL;
	}
	if (ccmni->timer_ready) {
		timer_shutdown_sync(ccmni->timer);
		ccmni->timer_ready = false;
	}
	if (ccmni->napi_added) {
		netif_napi_del(ccmni->napi);
		ccmni->napi_added = false;
	}
	kfree(ccmni->napi);
	kfree(ccmni->timer);
	kfree(ccmni->spinlock);
	ccmni->napi = NULL;
	ccmni->timer = NULL;
	ccmni->spinlock = NULL;
}
"""
    src = src[:a] + init + destroy + src[b:]
    src = replace_once(src, "\tdev->needs_free_netdev = true;",
                       "\tdev->priv_destructor = ccmni_dev_destroy;\n"
                       "\tdev->needs_free_netdev = true;")
    # Core may already have called the destructor when register_netdev fails;
    # constructor failures can bypass it. This helper is safe in both cases.
    src = replace_once(src, "alloc_netdev_fail:\n\tif (dev) {\n\t\tfree_netdev(dev);",
                       "alloc_netdev_fail:\n\tif (dev) {\n"
                       "\t\tccmni_dev_destroy(dev);\n\t\tfree_netdev(dev);")
    src = replace_once(src, "\tcase RX_IRQ:\n\t\tmod_timer(ccmni->timer, jiffies + HZ);",
                       "\tcase RX_IRQ:\n\t\tif (!ccmni->timer_ready || !netif_running(dev) ||\n"
                       "\t\t    atomic_read(&ccmni->usage) <= 0)\n\t\t\tbreak;\n"
                       "\t\tmod_timer(ccmni->timer, jiffies + HZ);")
    return src


def module_config(files):
    src = files["ccmni.c"].decode()
    # Use the existing non-GRO RX path. Workqueue GRO would need the
    # kernel's unexported netif_receive_skb_list_internal symbol.
    hdr = replace_once(files["ccmni.h"].decode(), "#define ENABLE_WQ_GRO\n", "")
    files["ccmni.h"] = hdr.encode()
    a = src.index("/*\n * Register the sysctl to set tcp_pacing_shift.")
    b = src.index("\nvoid ccmni_set_cur_speed(", a)
    if src[a:b].count("EXPORT_SYMBOL(set_ccmni_rps);") != 1:
        raise ValueError("unexpected CCMNI vendor performance policy")
    src = src[:a] + src[b:]
    for old in ('#include "rps_perf.h"\n', "static int s_call_times;\n",
                "\tif (s_call_times == 0)\n\t\tset_ccmni_rps(0x70);\n\ts_call_times++;\n",
                "\tsk_pacing_shift_update(skb->sk, sysctl_tcp_pacing_shift);\n",
                "\tif (register_tcp_pacing_sysctl() == -1)\n\t\treturn 0;\n"
                "\tsysctl_tcp_pacing_shift = 6;\n",
                "\tunregister_tcp_pacing_sysctl();\n"):
        src = replace_once(src, old, "")
    src = replace_once(src, "del_timer(ccmni->timer);", "timer_delete(ccmni->timer);")
    src = stage_callback_bounds(stage_registration(stage_rx_admission(src)))
    files["ccmni.c"] = stage_callback_lifetime(stage_instance_lifetime(files, src)).encode()
    files["ccmni.h"] = replace_once(files["ccmni.h"].decode(),
        "struct ccmni_ch *(*get_ch)(int md_id, int ccmni_idx);",
        "bool (*copy_ch)(int md_id, int ccmni_idx, struct ccmni_ch *out);").encode()
    files["Kbuild"] = b"obj-m := t70-ccmni.o\nt70-ccmni-y := ccmni.o\nccflags-y += -I$(src)\n"
    files["Makefile"] = (Path(__file__).parent / "Makefile").read_bytes()
    files["dkms.conf"] = (Path(__file__).parent / "dkms.conf").read_bytes()


PORT_HEADERS = (
    "ccci_common_config.h", "hif/ccif_hif_reg.h",
    "inc/ccci_bm.h", "inc/ccci_core.h", "inc/ccci_debug.h", "inc/ccci_hif.h",
    "inc/ccci_modem.h", "platform/ccci_config.h", "port/port_net.h", "port/port_t.h",
)


def pinned_header(git, path):
    for _ in range(5):
        row = subprocess.check_output(git + ["ls-tree", REV, path], text=True)
        data = subprocess.check_output(git + ["show", f"{REV}:{path}"])
        if row.startswith(("100644 blob ", "100755 blob ")):
            return data
        if not row.startswith("120000 blob "):
            break
        path = posixpath.normpath(posixpath.join(posixpath.dirname(path), data.decode().strip()))
        if not path.startswith(BASE + "/"):
            break
    raise ValueError(f"unresolved CCCI header: {path}")


def stage_port_rx_queue(src):
    a = src.index("\nstatic void recv_from_port_list(")
    b = src.index("\nstatic int port_net_recv_skb(", a)
    old = src[a:b]
    locked = "\tspin_lock_irqsave(&port->port_rx_list.lock, flags);"
    old = replace_once(old, "\tif (status)\n\t\tatomic_set(&port->is_up, 1);\n"
                       "\telse {\n\t\tatomic_set(&port->is_up, 0);\n"
                       "\t\tspin_lock_irqsave(&port->port_rx_list.lock, flags);",
                       locked + "\n\tatomic_set(&port->is_up, !!status);\n\tif (!status) {")
    old = replace_once(old, "\t\treturn ret;\n\t}\n\twhile (!skb_queue_empty",
                       "\t\treturn ret;\n\t}\n"
                       "\tspin_unlock_irqrestore(&port->port_rx_list.lock, flags);\n"
                       "\twhile (!skb_queue_empty")
    # Keep the stock grace period between ndo_open and the delayed port-open
    # callback, but never leave an unlimited backlog on an unopened netdev.
    q = old.index("\nstatic void ccmni_queue_recv_skb(")
    if "__skb_queue_tail(&port->port_rx_list, skb);" not in old[q:]:
        raise ValueError("missing pinned CCCI net RX staging")
    old = old[:q] + """
#define T70_CCMNI_RX_BACKLOG_PACKETS 64

static void ccmni_queue_recv_skb(struct port_t *port, struct sk_buff *skb)
{
	unsigned long flags;
	bool deliver, queued = false;

	spin_lock_irqsave(&port->port_rx_list.lock, flags);
	deliver = atomic_read(&port->is_up);
	if (!deliver && skb_queue_len(&port->port_rx_list) < T70_CCMNI_RX_BACKLOG_PACKETS) {
		__skb_queue_tail(&port->port_rx_list, skb);
		queued = true;
	}
	spin_unlock_irqrestore(&port->port_rx_list.lock, flags);

	if (queued)
		return;
	if (!deliver) {
		dev_kfree_skb_any(skb);
		return;
	}
	while (!skb_queue_empty(&port->port_rx_list))
		recv_from_port_list(port);
	ccmni_ops.rx_callback(port->md_id, GET_CCMNI_IDX(port), skb, NULL);
}
"""
    return src[:a] + old + src[b:]


def stage_port_rx(src):
    a = src.index("\nstatic int port_net_recv_skb(")
    b = src.index("\nstatic void port_net_queue_state_notify(", a)
    rx = src[a:b]
    rx = replace_once(rx, "\tstruct lhif_header *lhif_h = (struct lhif_header *)skb->data;\n",
                      "\tstruct lhif_header *lhif_h;\n")
    rx = replace_once(rx, "\tif (port->md_id < 0 || port->md_id >= MAX_MD_NUM) {",
                      "\tif (!port || !skb)\n\t\treturn -EINVAL;\n"
                      "\tif (port->md_id < 0 || port->md_id >= MAX_MD_NUM) {")
    blocked = ("\tmbim_ccmni_current = atomic_read(&mbim_ccmni_index[port->md_id]);\n"
               "\tif (mbim_ccmni_current == GET_CCMNI_IDX(port)) {\n"
               "\t\tmbim_start_xmit(skb, mbim_ccmni_current);\n"
               "\t\treturn 0;\n\t}\n")
    rx = replace_once(rx, blocked, "")
    lhif = "\tif ((port->hif_id == MD1_NET_HIF) || (port->hif_id == CLDMA_HIF_ID)) {"
    rx = replace_once(rx, lhif,
                      "\t/* A negative return leaves the skb for the CCCI proxy to free. */\n"
                      "\tmbim_ccmni_current = atomic_read(&mbim_ccmni_index[port->md_id]);\n"
                      "\tif (mbim_ccmni_current == GET_CCMNI_IDX(port))\n"
                      "\t\treturn -EOPNOTSUPP;\n" + lhif)
    rx = replace_once(rx, "\t\tskb_pull(skb, sizeof(struct lhif_header));",
                      "\t\tif (!pskb_may_pull(skb, sizeof(*lhif_h)))\n"
                      "\t\t\treturn -EMSGSIZE;\n"
                      "\t\tlhif_h = (struct lhif_header *)skb->data;\n"
                      "\t\tskb_pull(skb, sizeof(*lhif_h));")
    rx = replace_once(rx, "\t\tstruct ccci_header *ccci_h = (struct ccci_header *)skb->data;\n"
                      "\n\t\tskb_pull(skb, sizeof(struct ccci_header));",
                      "\t\tstruct ccci_header *ccci_h;\n\n"
                      "\t\tif (!pskb_may_pull(skb, sizeof(*ccci_h)))\n"
                      "\t\t\treturn -EMSGSIZE;\n"
                      "\t\tccci_h = (struct ccci_header *)skb->data;\n"
                      "\t\tskb_pull(skb, sizeof(*ccci_h));")
    return src[:a] + rx + src[b:]


def stage_port(git, files):
    base = BASE + "/ccci_md_all/"
    for header in PORT_HEADERS:
        files["ccci/" + header] = pinned_header(git, base + header)
    src = pinned_header(git, base + "port/port_net.c").decode()
    a = src.index("\nint ccmni_send_pkt(")
    b = src.index("\nint ccmni_napi_poll(", a)
    tx = src[a:b]
    tx = replace_once(tx, "struct ccmni_ch *channel = ccmni_ops.get_ch(md_id, ccmni_idx);",
                      "struct ccmni_ch channel;")
    tx = replace_once(tx, "int tx_ch = is_ack ? channel->dl_ack : channel->tx;", "int tx_ch;")
    tx = replace_once(tx, "\tport = find_net_port_by_ccmni_idx(md_id, ccmni_idx);",
                      "\tif (!skb || !ccmni_ops.copy_ch(md_id, ccmni_idx, &channel))\n"
                      "\t\treturn CCMNI_ERR_TX_INVAL;\n"
                      "\ttx_ch = is_ack ? channel.dl_ack : channel.tx;\n"
                      "\tport = find_net_port_by_ccmni_idx(md_id, ccmni_idx);")
    tx = replace_once(tx, "\tccci_h = (struct ccci_header *)skb_push(skb,\n"
                      "\t\tsizeof(struct ccci_header));",
                      "\tif (skb_cow_head(skb, sizeof(*ccci_h)))\n\t\treturn CCMNI_ERR_TX_INVAL;\n"
                      "\tccci_h = (struct ccci_header *)skb_push(skb, sizeof(*ccci_h));\n"
                      "\tmemset(ccci_h, 0, sizeof(*ccci_h));")
    tx = replace_once(tx, "\t\tret = CCMNI_ERR_TX_BUSY;\n\t} else {",
                      "\t\tif (ret == -EBUSY || ret == -EAGAIN || ret == -ENOSPC)\n"
                      "\t\t\tret = CCMNI_ERR_TX_BUSY;\n"
                      "\t\telse if (ret == -ENODEV || ret == -ENETDOWN ||\n"
                      "\t\t\t ret == -CCCI_ERR_MD_NOT_READY ||\n"
                      "\t\t\t ret == -CCCI_ERR_HIF_NOT_POWER_ON)\n"
                      "\t\t\tret = CCMNI_ERR_MD_NO_READY;\n"
                      "\t\telse\n\t\t\tret = CCMNI_ERR_TX_INVAL;\n\t} else {")
    src = stage_port_rx(src[:a] + tx + src[b:])
    files["ccci/port/port_net.c"] = stage_port_rx_queue(src).encode()
    # CCMNI and port share the same generic ccmni.h and selected features.
    files["Kbuild"] = (b"obj-m := t70-ccmni-port.o\n"
                       b"t70-ccmni-port-y := ccmni.o ccci/port/port_net.o\n"
                       b"ccflags-y += -I$(src) -I$(src)/ccci -I$(src)/ccci/inc "
                       b"-I$(src)/ccci/platform -I$(src)/ccci/hif\n")
    del files["dkms.conf"]


def stage_tx_queue_recheck(files):
    name = "ccci/port/port_net.c"
    src = files[name].decode()
    a = src.index("\nstatic void port_net_queue_state_notify(")
    b = src.index("\nstatic void port_net_md_state_notify(", a)
    prior = src[a:b]
    check = "\tif (dir == OUT)\n\t\tspin_lock_irqsave(&port->flag_lock, flags);"
    prior = replace_once(prior, "\tint is_ack = 0;", "\tint is_ack = 0;\n\tunsigned int full;")
    prior = replace_once(prior, check, r"""
	/* T70's stock network ports use DPMAIF Q0 for data and Q1 for ACK.
	 * LAN shares Q0. Other HIF notifications retain the original path.
	 */
	if (dir == OUT && port->md_id == MD_SYS1 && port->hif_id == DPMAIF_HIF_ID &&
	    (state == TX_FULL || state == TX_IRQ)) {
		if (qno < 0 || qno > 1 ||
		    (qno != port->txq_index && qno != NET_ACK_TXQ_INDEX(port)))
			return;
		is_ack = qno != port->txq_index;
		full = is_ack ? PORT_F_TX_ACK_FULLED : PORT_F_TX_DATA_FULLED;
		spin_lock_irqsave(&port->flag_lock, flags);
		if (state == TX_FULL) {
			if (!(port->flags & full)) {
				port->flags |= full;
				ccmni_ops.queue_state_callback(port->md_id,
					GET_CCMNI_IDX(port), TX_FULL, is_ack);
			}
			/* Match the stop/recheck contract even if the last TX IRQ
			 * passed before the flag was set. A negative query is offline.
			 */
			if (ccci_dpmaif_empty_query(qno) > 0)
				state = TX_IRQ;
		}
		if (state == TX_IRQ && (port->flags & full)) {
			port->flags &= ~full;
			ccmni_ops.queue_state_callback(port->md_id,
				GET_CCMNI_IDX(port), TX_IRQ, is_ack);
		}
		spin_unlock_irqrestore(&port->flag_lock, flags);
		return;
	}
""".lstrip("\n") + "\n" + check)
    files[name] = (src[:a] + prior + src[b:]).encode()


def stage_proxy_net_tx(git, files):
    path = BASE + "/ccci_md_all/port/port_proxy.c"
    proxy = pinned_header(git, path).decode()
    cfg = BASE + "/ccci_md_all/port/"
    for name in ("port_cfg.c", "port_cfg.h"):
        files["ccci/port/" + name] = pinned_header(git, cfg + name)
    hdr = files["ccci/port/port_cfg.h"].decode()
    files["ccci/port/port_cfg.h"] = replace_once(hdr, "#endif /* __PORT_CFG_H__ */",
        "int port_get_cfg(int md_id, struct port_t **ports);\n"
        "#endif /* __PORT_CFG_H__ */").encode()
    dispatch_start = proxy.index("\nstatic inline void proxy_dispatch_queue_status(")
    dispatch_end = proxy.index("\nstatic inline void proxy_dispatch_md_status(", dispatch_start)
    stock_dispatch = proxy[dispatch_start:dispatch_end]
    if "qno == port->txq_index" not in stock_dispatch or \
       "qno == (port->txq_exp_index & 0x0F)" not in stock_dispatch:
        raise ValueError("unexpected T70 CCCI queue dispatch")
    a = proxy.index("\nint port_net_send_skb_to_md(")
    b = proxy.index("\nint port_send_skb_to_md(", a)
    tx = proxy[a:b]
    tx = replace_once(tx, "\tint tx_qno = 0;\n\tint md_id = port->md_id;\n\n"
                      "\tCHECK_MD_ID(md_id);\n\tif (ccci_fsm_get_md_state(md_id) != READY)\n"
                      "\t\treturn -ENODEV;\n\ttx_qno = port_get_queue_no(port, OUT, is_ack);",
                      "\tint tx_qno;\n\n"
                      "\tif (!port || !skb || (is_ack != 0 && is_ack != 1))\n"
                      "\t\treturn -EINVAL;\n"
                      "\tif (port->md_id != MD_SYS1 || port->hif_id != DPMAIF_HIF_ID)\n"
                      "\t\treturn -ENODEV;\n\n"
                      "\t/* Read READY once. Never select an exception TX queue for IP. */\n"
                      "\tif (ccci_fsm_get_md_state(port->md_id) != READY)\n"
                      "\t\treturn -ENODEV;\n"
                      "\tif (is_ack && port->txq_exp_index == 0xff)\n"
                      "\t\treturn -EINVAL;\n"
                      "\ttx_qno = is_ack ? port->txq_exp_index & 0x0f : port->txq_index;\n"
                      "\t/* T70 Gen6295 CCMNI uses Q0/Q1, not the DPMAIF special Q2/Q3. */\n"
                      "\tif (tx_qno > 1)\n\t\treturn -EINVAL;")
    files["ccci/port/port_proxy_net_tx.c"] = ("""// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2016 MediaTek Inc. */
/* Network TX slice of the pinned CCCI port proxy; not the complete proxy. */
#include <linux/errno.h>
#include <linux/skbuff.h>
#include "mt-plat/mtk_ccci_common.h"
#include "ccci_core.h"
#include "ccci_hif.h"
#include "ccci_fsm.h"
#include "port_t.h"
#include "port_cfg.h"
#include "t70-ccci-net-tx-notify.h"
""" + tx + """
/* Board network subset of proxy_dispatch_queue_status. The owner must have
 * finished port initialization before publishing READY, and quiesce all
 * completion workers before removing that port table.
 */
int t70_ccci_net_tx_notify(int qno, unsigned int state)
{
	struct port_t *ports = NULL, *port;
	int i, count, delivered = 0;

	if (qno < 0 || qno > 1 || (state != TX_FULL && state != TX_IRQ))
		return -EINVAL;
	if (ccci_fsm_get_md_state(MD_SYS1) != READY)
		return -ENETDOWN;
	count = port_get_cfg(MD_SYS1, &ports);
	if (count <= 0 || !ports)
		return -ENODEV;

	/* Each WAN port shares Q0/Q1; ccmni-lan uses Q0 for both classes. */
	for (i = 0; i < count; ++i) {
		port = &ports[i];
		/* The pinned T70 table places all net ports before char ports. */
		if (port->ops != &net_port_ops)
			break;
		if (port->md_id != MD_SYS1 || port->hif_id != DPMAIF_HIF_ID ||
		    port->minor < CCCI_NET_MINOR_BASE ||
		    !port->ops->queue_state_notify)
			continue;
		if (qno != port->txq_index &&
		    (port->txq_exp_index == 0xff || qno != (port->txq_exp_index & 0x0f)))
			continue;
		port->ops->queue_state_notify(port, OUT, qno, state);
		++delivered;
	}
	return delivered;
}
""").encode()
    files["ccci/inc/t70-ccci-net-tx-notify.h"] = b"""/* SPDX-License-Identifier: GPL-2.0 */
#ifndef T70_CCCI_NET_TX_NOTIFY_H
#define T70_CCCI_NET_TX_NOTIFY_H
int t70_ccci_net_tx_notify(int qno, unsigned int state);
#endif
"""
    files["Kbuild"] = files["Kbuild"].replace(b"ccci/hif/ccci_hif.o\n",
        b"ccci/hif/ccci_hif.o ccci/port/port_cfg.o ccci/port/port_proxy_net_tx.o\n")


def stage_hif(git, files):
    base = BASE + "/ccci_md_all/hif/"
    for header in ("ccci_cldma_plat.h", "ccci_hif_cldma.h", "ccci_hif_internal.h"):
        files["ccci/hif/" + header] = pinned_header(git, base + header)
    for header in ("inc/ccci_port.h", "inc/ccci_fsm.h"):
        files["ccci/" + header] = pinned_header(git, BASE + "/ccci_md_all/" + header)
    src = pinned_header(git, base + "ccci_hif.c").decode()
    a = src.index("\nint ccci_dpmaif_empty_query(")
    b = src.index("\nvoid ccci_hif_set_clk_cg(", a)
    if "ccci_hif_op[DPMAIF_HIF_ID]->empty_query(qno)" not in src[a:b]:
        raise ValueError("unexpected stock DPMAIF space query")
    src = src[:a] + """
int ccci_dpmaif_empty_query(int qno)
{
	const struct ccci_hif_ops *ops;

	/* v1 has four logical TX queue IDs, with Q3 aliased only by its provider. */
	if (qno < 0 || qno >= 4)
		return -EINVAL;
	if (!smp_load_acquire(&ccci_hif[DPMAIF_HIF_ID]))
		return -ENODEV;
	ops = READ_ONCE(ccci_hif_op[DPMAIF_HIF_ID]);
	if (!ops || !ops->empty_query)
		return -ENODEV;
	return ops->empty_query(qno);
}
""" + src[b:]
    a = src.index("\nint ccci_hif_send_skb(")
    b = src.index("\nint ccci_hif_send_data(", a)
    send = src[a:b]
    send = replace_once(send, "\tint ret = 0;\n",
                        "\tconst struct ccci_hif_ops *ops;\n\tint ret;\n"
                        "\n\tif (!skb || tx_qno < 0)\n\t\treturn -EINVAL;\n")
    send = replace_once(send,
        "\t\tif (ccci_hif[hif_id] && ccci_hif_op[hif_id]->send_skb)\n"
        "\t\t\tret = ccci_hif_op[hif_id]->send_skb(hif_id,\n"
        "\t\t\t\ttx_qno, skb, from_pool, blocking);\n"
        "\t\tbreak;\n\tdefault:\n\t\tbreak;",
        "\t\t/* Provider lifetime must be held by the CCCI owner. */\n"
        "\t\tif (!smp_load_acquire(&ccci_hif[hif_id]))\n\t\t\treturn -ENODEV;\n"
        "\t\tops = READ_ONCE(ccci_hif_op[hif_id]);\n"
        "\t\tif (!ops || !ops->send_skb)\n\t\t\treturn -ENODEV;\n"
        "\t\treturn ops->send_skb(hif_id, tx_qno, skb, from_pool, blocking);\n"
        "\tdefault:\n\t\treturn -EINVAL;")
    src = src[:a] + send + src[b:]
    src = replace_once(src,
                       "\t\tccci_hif[hif_id] = hif_per_data;\n\t\tccci_hif_op[hif_id] = ops;",
                       "\t\tWRITE_ONCE(ccci_hif_op[hif_id], ops);\n"
                       "\t\tsmp_store_release(&ccci_hif[hif_id], hif_per_data);")
    files["ccci/hif/ccci_hif.c"] = src.encode()
    files["Kbuild"] = files["Kbuild"].replace(
        b"ccci/port/port_net.o\n", b"ccci/port/port_net.o ccci/hif/ccci_hif.o\n")


def stage_dpmaif_reclaim(git, files):
    base = BASE + "/ccci_md_all/hif/"
    header = "ccci_hif_dpmaif_v1.h"
    files["ccci/hif/" + header] = pinned_header(git, base + header)
    origin = pinned_header(git, BASE + "/ccci_dpmaif/ccci_hif_dpmaif_v1.c").decode()
    geometry = []
    for name in ("ringbuf_get_next_idx", "ringbuf_releasable"):
        hit = re.search(r"(?ms)^static unsigned int\s+" + name + r"\([^{}]*\n\{.*?^\}", origin)
        if not hit:
            raise ValueError("missing stock DPMAIF ring helper " + name)
        geometry.append(hit[0])
    files["ccci/hif/dpmaif-v1-ring-geometry.h"] = (
        "/* SPDX-License-Identifier: GPL-2.0 */\n"
        "/* Copyright (C) 2016 MediaTek Inc. */\n" + "\n\n".join(geometry) + "\n").encode()
    for suffix in ("c", "h"):
        name = "dpmaif-v1-tx-reclaim." + suffix
        files["ccci/hif/" + name] = Path(__file__).with_name(name).read_bytes()
    files["Kbuild"] = files["Kbuild"].replace(b"ccci/port/port_proxy_net_tx.o\n",
        b"ccci/port/port_proxy_net_tx.o ccci/hif/dpmaif-v1-tx-reclaim.o\n")


def stage_fsm_tx_state(git, files):
    base = BASE + "/ccci_md_all/fsm/"
    name = "ccci_fsm_internal.h"
    files["ccci/fsm/" + name] = pinned_header(git, base + name)
    origin = pinned_header(git, base + "ccci_fsm.c").decode()
    a = origin.index("\nenum MD_STATE ccci_fsm_get_md_state(")
    b = origin.index("\nenum MD_STATE_FOR_USER ccci_fsm_get_md_state_for_user(", a)
    state = origin[a:b]
    state = replace_once(state,
        "\tstruct ccci_fsm_ctl *ctl = fsm_get_entity_by_md_id(md_id);\n\n"
        "\tif (ctl)\n\t\treturn ctl->md_state;\n\telse\n\t\treturn INVALID;",
        "\tstruct ccci_fsm_ctl *ctl;\n\tenum MD_STATE state;\n\n"
        "\t/* The FSM owner keeps the control block alive until TX workers drain. */\n"
        "\tif (md_id != MD_SYS1)\n\t\treturn INVALID;\n"
        "\tctl = fsm_get_entity_by_md_id(md_id);\n"
        "\tif (!ctl || READ_ONCE(ctl->md_id) != md_id)\n\t\treturn INVALID;\n"
        "\tstate = READ_ONCE(ctl->md_state);\n"
        "\tswitch (state) {\n"
        "\tcase GATED:\n\tcase BOOT_WAITING_FOR_HS1:\n\tcase BOOT_WAITING_FOR_HS2:\n"
        "\tcase READY:\n\tcase EXCEPTION:\n\tcase RESET:\n\tcase WAITING_TO_STOP:\n"
        "\t\treturn state;\n\tdefault:\n\t\treturn INVALID;\n\t}")
    files["ccci/fsm/ccci_fsm_tx_state.c"] = ("""// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2016 MediaTek Inc. */
/* Read-only TX state slice of the real CCCI FSM; no state is synthesized. */
#include <linux/compiler.h>
#include <linux/export.h>
#include "ccci_fsm_internal.h"
""" + state).encode()
    files["Kbuild"] = files["Kbuild"].replace(b"ccci/hif/dpmaif-v1-tx-reclaim.o\n",
        b"ccci/hif/dpmaif-v1-tx-reclaim.o ccci/fsm/ccci_fsm_tx_state.o\n")
    files["Kbuild"] += b"ccflags-y += -I$(src)/ccci/port\n"


def stage(ref, dest, module=False, with_port=False, with_hif=False):
    if with_hif and not with_port:
        raise ValueError("--with-hif requires --with-port")
    git = ["git", "-C", str(ref)]
    files = {}
    for target, path in FILES.items():
        row = subprocess.check_output(git + ["ls-tree", REV, path], text=True)
        if not row.startswith(("100644 blob ", "100755 blob ")):
            raise ValueError(f"non-regular pinned CCMNI source: {path}")
        files[target] = subprocess.check_output(git + ["show", f"{REV}:{path}"])
    src = files["ccmni.c"].decode()
    src = replace_once(src, "#include <trace/hooks/ipv6.h>", "#include <net/gro.h>")
    a = src.index("\nstatic inline void napi_gro_list_flush(")
    b = src.index("\nstatic void ccmni_gro_flush(", a)
    if "napi_gro_flush(napi, false);" not in src[a:b] or "napi->rx_count" not in src[a:b]:
        raise ValueError("unexpected pinned CCMNI GRO flush")
    src = src[:a] + """
static inline void napi_gro_list_flush(struct ccmni_instance *ccmni)
{
	gro_flush_normal(&ccmni->napi->gro, false);
}
""" + src[b:]
    files["ccmni.c"] = src.encode()
    files["Kbuild"] = b"obj-m := ccmni.o\nccflags-y += -I$(src)\n"

    if module or with_port:
        module_config(files)
    if with_port:
        stage_port(git, files)
    if with_hif:
        stage_hif(git, files)
        stage_tx_queue_recheck(files)
        stage_proxy_net_tx(git, files)
        stage_dpmaif_reclaim(git, files)
        stage_fsm_tx_state(git, files)
    dest.mkdir()
    for name, data in files.items():
        path = dest / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference", type=Path)
    p.add_argument("destination", type=Path)
    p.add_argument("--module", action="store_true")
    p.add_argument("--with-port", action="store_true")
    p.add_argument("--with-hif", action="store_true")
    a = p.parse_args()
    try:
        stage(a.reference, a.destination, a.module, a.with_port, a.with_hif)
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as e:
        p.exit(2, f"CCMNI staging refused: {e}\n")
