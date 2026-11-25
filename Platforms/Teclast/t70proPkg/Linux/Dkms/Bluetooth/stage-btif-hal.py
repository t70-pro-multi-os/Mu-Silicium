#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the pinned vendor BTIF transport HAL for an object-only ABI check."""
import argparse
from pathlib import Path
import subprocess

REV = "a71515dbaf0d539ffb36416f14186dad17eef743"
SOURCE = "mindone/modules/btif_drv"
FILES = (
    "btif_dma_plat.c", "btif_plat.c", "btif_dma_priv.h", "btif_dma_pub.h",
    "btif_priv.h", "btif_pub.h", "mtk_btif.h", "mtk_btif_exp.h", "plat_common.h",
)


def strip_test_interfaces(data):
    text = data.decode()
    start = text.index("/*-----------device property----------------*/")
    end = text.index("/*-----------End of platform bus related operation APIs------------*/", start)
    if "static DRIVER_ATTR(flag," not in text[start:end]:
        raise ValueError("unexpected BTIF flag sysfs handlers")
    text = text[:start] + text[end:]
    start = text.index("\ti_ret = driver_create_file(&mtk_btif_dev_drv.driver, &driver_attr_flag);")
    end = text.index("\n\n", start)
    text = text[:start] + text[end + 2:]
    remove = "\tdriver_remove_file(&mtk_btif_dev_drv.driver, &driver_attr_flag);\n"
    if text.count(remove) != 2 or text.count("#define BTIF_CDEV_SUPPORT 0") != 1:
        raise ValueError("unexpected BTIF test interface cleanup")
    text = text.replace(remove, "")
    if "driver_attr_flag" in text:
        raise ValueError("BTIF test interface is still referenced")
    return text.encode()


def bind_btif_lifetime(files):
    name = "mtk_btif.c"
    text = files[name].decode()
    a = text.index("\nstatic int mtk_btif_probe(struct platform_device *pdev)\n{")
    b = text.index("\nint _btif_suspend(", a)
    text = text[:a] + """
static DEFINE_MUTEX(btif_probe_lock);
static int btif_device_init(void);
static void btif_device_stop(void);

bool t70_btif_ready(void)
{
	return smp_load_acquire(&btif_probed);
}

static int mtk_btif_probe(struct platform_device *pdev)
{
	int ret;

	mutex_lock(&btif_probe_lock);
	if (g_btif[0].private_data) {
		ret = -EBUSY;
		goto out;
	}
#if !defined(CONFIG_MTK_CLKMGR)
	ret = hal_btif_clk_get_and_prepare(pdev);
	if (ret)
		goto out;
#endif
	g_btif[0].private_data = &pdev->dev;
	ret = btif_device_init();
	if (ret) {
		g_btif[0].private_data = NULL;
#if !defined(CONFIG_MTK_CLKMGR)
		hal_btif_clk_unprepare();
#endif
		goto out;
	}
	platform_set_drvdata(pdev, &g_btif[0]);
	smp_store_release(&btif_probed, 1);
out:
	mutex_unlock(&btif_probe_lock);
	return ret;
}

static void mtk_btif_remove(struct platform_device *pdev)
{
	mutex_lock(&btif_probe_lock);
	smp_store_release(&btif_probed, 0);
	btif_device_stop();
	platform_set_drvdata(pdev, NULL);
	g_btif[0].private_data = NULL;
#if !defined(CONFIG_MTK_CLKMGR)
	hal_btif_clk_unprepare();
#endif
	mutex_unlock(&btif_probe_lock);
}
""" + text[b:]
    # This fixed platform device exposes singleton APIs to other kernel modules.
    driver_name = "\t\t\t.name = DRV_NAME,\n"
    if text.count(driver_name) != 1:
        raise ValueError("unexpected BTIF platform driver")
    text = text.replace(driver_name, driver_name + "\t\t\t.suppress_bind_attrs = true,\n")

    a = text.index("\nstatic int BTIF_init(void)\n{")
    b = text.index("\nstatic void BTIF_exit(void)\n{", a)
    end = text.index("\nint mtk_btif_hal_get_log_lvl(", b)
    init = text[a:b]
    first = init.index("/*Platform Driver initialization*/")
    last = init.index("/*SW init*/", first)
    init = init[:first] + "\tBUILD_BUG_ON(BTIF_PORT_NR != 1);\n" + init[last:]
    first = init.index("\t\tp_btif_buffer = kmalloc(")
    last = init.index("\t\tINIT_LIST_HEAD", first)
    alloc = init[first:last]
    if alloc.count("return -1;") != 3 or alloc.count("GFP_ATOMIC") != 3:
        raise ValueError("unexpected BTIF probe buffer allocations")
    init = init[:first] + alloc.replace("return -1;", "return -ENOMEM;").replace(
        "GFP_ATOMIC", "GFP_KERNEL") + init[last:]
    first = init.index("/*Debug purpose initialization*/")
    last = init.index("\n\treturn 0;", first)
    init = init[:first] + init[last:]
    start_unwind = init.index("\nerr_exit2:")
    init = init[:start_unwind] + """
err_exit2:
	btif_free_vfifos(&g_btif[0]);
	btif_free_buffers(&g_btif[0]);
	return i_ret;
}
"""
    init = init.replace("static int BTIF_init(void)", "static int btif_device_init(void)", 1)
    cleanup = """
static void btif_free_vfifos(struct _mtk_btif_ *port)
{
#if ENABLE_BTIF_RX_DMA
	if (port->p_rx_dma)
		_btif_vfifo_deinit(port->p_rx_dma);
#endif
#if ENABLE_BTIF_TX_DMA
	if (port->p_tx_dma)
		_btif_vfifo_deinit(port->p_tx_dma);
#endif
	port->p_rx_dma = NULL;
	port->p_tx_dma = NULL;
}

static void btif_free_buffers(struct _mtk_btif_ *port)
{
	kfree(port->rx_log.p_queue);
	port->rx_log.p_queue = NULL;
	kfree(port->tx_log.p_queue);
	port->tx_log.p_queue = NULL;
	kfree(port->btif_buf.p_buf);
	port->btif_buf.p_buf = NULL;
}
"""
    stop = """
static void btif_device_stop(void)
{
	struct _mtk_btif_ *port = &g_btif[0];

	if (port->enable)
		btif_close(port);
	_btif_tx_ctx_deinit(port);
	_btif_rx_btm_deinit(port);
	_btif_state_deinit(port);
	mutex_destroy(&port->ops_mtx);
	btif_free_vfifos(port);
	btif_free_buffers(port);
}

static int BTIF_init(void)
{
	return platform_driver_register(&mtk_btif_dev_drv);
}

static void BTIF_exit(void)
{
	platform_driver_unregister(&mtk_btif_dev_drv);
}
"""
    files[name] = (text[:a] + cleanup + init + stop + text[end:]).encode()

    name = "mtk_btif.h"
    anchor = b"extern struct _mtk_btif_ g_btif[];"
    if files[name].count(anchor) != 1:
        raise ValueError("unexpected BTIF header")
    files[name] = files[name].replace(anchor, anchor + b"\nbool t70_btif_ready(void);")

    name = "mtk_btif_exp.c"
    text = files[name].decode()
    a = text.index("\nstruct _mtk_btif_ *btif_exp_srh_id(")
    b = text.index("\nint mtk_wcn_btif_open(", a)
    split = text.index("\n\tfor (index", a, b)
    text = text[:split] + "\n\tif (!t70_btif_ready())\n\t\treturn NULL;\n" + text[split:]
    a = text.index("\nint mtk_wcn_btif_open(")
    split = text.index('\n\tBTIF_DBG_FUNC("++");', a)
    text = text[:split] + """
	if (!t70_btif_ready()) {
		if (p_id)
			*p_id = 0;
		return -EAGAIN;
	}
""" + text[split:]
    files[name] = text.encode()


def fix_btif_rx_thread_stop(files):
    changes = {
        "mtk_btif.h": (
            ("#include <linux/kthread.h>\n",
             "#include <linux/kthread.h>\n#include <linux/atomic.h>\n#include <linux/wait.h>\n"),
            ("\tstruct completion rx_comp;", "\twait_queue_head_t rx_wait;\n\tatomic_t rx_pending;"),
        ),
        "mtk_btif.c": (
            ("\t\tif (wait_for_completion_interruptible(&p_btif->rx_comp))\n"
             "\t\t\tBTIF_WARN_FUNC(\"wait_for_completion is interrupted\");",
             "\t\tif (wait_event_interruptible(p_btif->rx_wait,\n"
             "\t\t\tkthread_should_stop() || atomic_read(&p_btif->rx_pending)))\n"
             "\t\t\tcontinue;\n"
             "\t\tif (kthread_should_stop())\n"
             "\t\t\tbreak;\n"
             "\t\tatomic_xchg(&p_btif->rx_pending, 0);"),
            ("\t\tinit_completion(&p_btif->rx_comp);",
             "\t\tinit_waitqueue_head(&p_btif->rx_wait);\n\t\tatomic_set(&p_btif->rx_pending, 0);"),
            ("\t\tcomplete(&p_btif->rx_comp);",
             "\t\tatomic_set(&p_btif->rx_pending, 1);\n\t\twake_up_interruptible(&p_btif->rx_wait);"),
            ("\t\tif (p_btif->p_task == NULL) {\n"
             "\t\t\tBTIF_ERR_FUNC(\"kthread_create fail\\n\");\n"
             "\t\t\ti_ret = -ENOMEM;",
             "\t\tif (IS_ERR(p_btif->p_task)) {\n"
             "\t\t\ti_ret = PTR_ERR(p_btif->p_task);\n"
             "\t\t\tp_btif->p_task = NULL;\n"
             "\t\t\tBTIF_ERR_FUNC(\"kthread_create failed: %d\\n\", i_ret);"),
            ("\t\t\tkthread_stop(p_btif->p_task);",
             "\t\t\tkthread_stop(p_btif->p_task);\n\t\t\tp_btif->p_task = NULL;"),
        )
    }
    for name, replacements in changes.items():
        text = files[name].decode()
        for old, new in replacements:
            if text.count(old) != 1:
                raise ValueError(f"unexpected BTIF RX thread lifecycle in {name}: {old}")
            text = text.replace(old, new)
        files[name] = text.encode()


def fix_btif_coherent_rx(files):
    name = "mtk_btif.c"
    text = files[name].decode()
    streaming_unmap = """	/* Although we use coherent DMA, we still need to flush cache. */
	/* However, btif cannot invoke cache API directly because btif is KO. */
	/* We unmap dma buffer to resolve this problem */
	dma_unmap_single(p_btif->private_data, p_dma_info->p_vfifo->phy_addr,
			p_dma_info->p_vfifo->vfifo_size, DMA_FROM_DEVICE);
"""
    dma_detach = ("\thal_btif_dma_rx_cb_reg(p_dma_info, (dma_rx_buf_write) NULL);\n"
                  "\t_btif_irq_free(p_irq, p_btif);")
    pio_detach = ("\thal_btif_rx_cb_reg(p_btif->p_btif_info, (btif_rx_buf_write) NULL);\n"
                  "\t_btif_irq_free(p_btif->p_btif_info->p_irq, p_btif);")
    for old, new in ((streaming_unmap, "\t/* vFIFO stays coherent until dma_free_coherent(). */\n"),
                     (dma_detach, "\n".join(reversed(dma_detach.splitlines()))),
                     (pio_detach, "\n".join(reversed(pio_detach.splitlines())))):
        if text.count(old) != 1:
            raise ValueError("unexpected BTIF RX ownership source")
        text = text.replace(old, new)
    files[name] = text.encode()

    name = "btif_dma_plat.c"
    text = files[name].decode()
    old = "\t\tif (rx_cb != NULL) {\n\t\t\ttail_len = vff_size - rpt;"
    if text.count(old) != 1:
        raise ValueError("unexpected BTIF RX vFIFO read")
    new = ("\t\tif (rx_cb != NULL) {\n"
           "\t\t\t/* The DMA producer publishes WPT after the payload. */\n"
           "\t\t\tdma_rmb();\n\t\t\ttail_len = vff_size - rpt;")
    files[name] = text.replace(old, new).encode()


def propagate_btif_dma_stop_errors(files):
    name = "btif_dma_plat.c"
    text = files[name].decode()
    old = '\t\t\t\tBTIF_ERR_FUNC("stop dma timeout!\\n");\n\t\t\t\tbreak;'
    if text.count(old) != 2:
        raise ValueError("unexpected BTIF TX/RX DMA stop loops")
    files[name] = text.replace(old, old.replace("break;", "return -ETIMEDOUT;")).encode()

    name = "mtk_btif.c"
    text = files[name].decode()
    old = "\thal_btif_dma_ctrl(p_dma_info, DMA_CTRL_DISABLE);"
    if text.count(old) != 2:
        raise ValueError("unexpected BTIF TX/RX DMA stop callers")
    # A failed stop must not be hidden by successful clock gating.
    for direction in ("tx", "rx"):
        a = text.index(f"\nint _btif_{direction}_dma_free(")
        b = text.index("\n}\n", a)
        free = text[a:b].replace("\n{\n", "\n{\n\tint ret;\n", 1)
        free = free.replace(old, "\tret = hal_btif_dma_ctrl(p_dma_info, DMA_CTRL_DISABLE);\n"
            "\tif (ret)\n\t\treturn ret;")
        text = text[:a] + free + text[b:]
        a = text.index(f"\nint _btif_controller_{direction}_free(")
        b = text.index("\n}\n", a)
        controller = text[a:b]
        if controller.count("\tint i_ret = -1;") != 1:
            raise ValueError("unexpected BTIF PIO release")
        text = text[:a] + controller.replace("\tint i_ret = -1;", "\tint i_ret = 0;") + text[b:]
    a = text.index("\nint _btif_suspend(")
    b = text.index("\n}\n", a)
    suspend = text[a:b]
    old = """					i_ret += _btif_controller_free(p_btif);
					i_ret = _btif_controller_tx_free
							(p_btif);
					i_ret += _btif_controller_rx_free
							(p_btif);"""
    new = """					int tx_ret, rx_ret;

					i_ret = _btif_controller_free(p_btif);
					tx_ret = _btif_controller_tx_free(p_btif);
					rx_ret = _btif_controller_rx_free(p_btif);
					if (!i_ret)
						i_ret = tx_ret ?: rx_ret;"""
    if suspend.count(old) != 1:
        raise ValueError("unexpected BTIF PM stop status handling")
    files[name] = (text[:a] + suspend.replace(old, new) + text[b:]).encode()


def register_btif_dma_irq_before_start(data):
    text = data.decode()

    def edit(name, fn, src):
        a = src.index("\nint " + name + "(")
        b = src.index("\n}\n", a)
        return src[:a] + fn(src[a:b]) + src[b:]

    sig = "irq_handler_t irq_handler, void *data)"
    if text.count(sig) != 2:
        raise ValueError("unexpected BTIF IRQ registration signature")
    text = text.replace(sig, "irq_handler_t irq_handler, void *data, bool initially_masked)")
    def irq(body):
        body = body.replace('\t\tBTIF_WARN_FUNC("%s is not supported\\n", p_irq->name);\n\t\treturn 0;',
            '\t\tBTIF_WARN_FUNC("%s is not supported\\n", p_irq->name);\n'
            '\t\treturn initially_masked ? -ENXIO : 0;')
        return body.replace("\tp_irq->p_irq_handler = irq_handler;",
            "\tif (initially_masked)\n\t\tflag |= IRQF_NO_AUTOEN;\n"
            "\tp_irq->p_irq_handler = irq_handler;")
    text = edit("_btif_irq_reg", irq, text)
    old = "_btif_irq_reg(p_btif_irq, btif_irq_handler, p_btif)"
    if text.count(old) != 1:
        raise ValueError("unexpected BTIF PIO IRQ registration")
    text = text.replace(old, old[:-1] + ", false)")
    for direction in ("tx", "rx"):
        setup = f"_btif_{direction}_dma_setup"
        pio = f"_btif_{direction}_pio_setup"
        def controller(body):
            a = body.index("\n{")
            return body[:a] + (
                "\n{\n"
                f"\tif (p_btif->{direction}_mode == BTIF_MODE_DMA)\n"
                f"\t\treturn {setup}(p_btif);\n"
                f"\treturn {pio}(p_btif);")
        def dma(body):
            a = body.index("\n{")
            if "DMA_CTRL_ENABLE" not in body[a:] or "_btif_irq_reg(" not in body[a:]:
                raise ValueError("unexpected BTIF DMA setup body")
            rx_attach = ("\thal_btif_dma_rx_cb_reg(dma, (dma_rx_buf_write) btif_dma_rx_data_receiver);\n"
                         if direction == "rx" else "")
            rx_detach = ("\thal_btif_dma_rx_cb_reg(dma, NULL);\n" if direction == "rx" else "")
            return body[:a] + f"""
{{
	int ret;
	struct _MTK_DMA_INFO_STR_ *dma = p_btif->p_{direction}_dma->p_dma_info;
	struct _MTK_BTIF_IRQ_STR_ *irq = dma->p_irq;

	ret = hal_btif_dma_clk_ctrl(dma, CLK_OUT_ENABLE);
	if (ret)
		return ret;
	/* A failed IRQ request must leave the DMA engine stopped. */
	ret = _btif_irq_reg(irq, btif_{direction}_dma_irq_handler, p_btif, true);
	if (ret)
		goto disable_clock;

	hal_btif_vfifo_reset(dma);
	ret = hal_btif_dma_hw_init(dma);
	if (ret)
		goto free_masked_irq;
	ret = hal_btif_{direction}_mode_ctrl(p_btif->p_btif_info, BTIF_MODE_DMA);
	if (ret)
		goto free_masked_irq;
{rx_attach}	ret = hal_btif_dma_ctrl(dma, DMA_CTRL_ENABLE);
	if (ret)
		goto detach_callback;
	_btif_irq_ctrl(irq, true);
	return 0;

detach_callback:
{rx_detach}free_masked_irq:
	/* NO_AUTOEN: this handler has not been enabled. */
	free_irq(irq->irq_id, p_btif);
	irq->reg_flag = false;
disable_clock:
	hal_btif_dma_clk_ctrl(dma, CLK_OUT_DISABLE);
	return ret;"""
        text = edit(f"_btif_controller_{direction}_setup", controller, text)
        text = edit(setup, dma, text)

    def init(body):
        # open and the power-state transitions own this mutex. An IRQ
        # request failure does not transfer ownership to the helper.
        unlock = "\t\tBTIF_STATE_RELEASE(p_btif);\n"
        if body.count(unlock) != 3:
            raise ValueError("unexpected BTIF init failure lock ownership")
        return body.replace(unlock, "")
    return edit("_btif_init", init, text).encode()


def check_btif_clock_probe(files):
    name = "btif_plat.c"
    text = files[name].decode()
    marker = "\t\ti_ret = clk_prepare(clk_btif_apdma);\n\t\tif (i_ret != 0) {\n"
    if text.count(marker) != 1:
        raise ValueError("unexpected BTIF APDMA prepare")
    text = text.replace(marker, marker + "\t\t\tclk_unprepare(clk_btif);\n")
    old = "\tclk_unprepare(clk_btif);\n\tclk_unprepare(clk_btif_apdma);"
    if text.count(old) != 1:
        raise ValueError("unexpected BTIF unprepare")
    files[name] = text.replace(old, "\n".join(reversed(old.splitlines()))).encode()



def report_rx_overrun(data):
    text = data.decode()
    start = text.index("\nunsigned int btif_bbs_write(")
    end = text.index("\nunsigned int btif_bbs_wr_direct(", start)
    body = text[start:end]
    first = body.index("\tif (!ava_len || ava_len < buf_len || large_pkt || reset) {")
    last = body.index("\n\treturn wr_len;", first)
    raw = body[first:last]
    if "hal_dma_dump_reg(p_btif->p_rx_dma->p_dma_info" not in raw or "_btif_dump_memory" not in raw:
        raise ValueError("unexpected BTIF RX overrun diagnostics")
    # An RX ring can overflow in PIO mode too. Keep diagnostics that do
    # not touch registers or dereference the absent DMA channel in IRQ.
    body = body[:first] + body[last:]
    a = body.index("\tstruct _mtk_btif_ *p_btif = container_of(")
    b = body.index("\n\n", a)
    body = body[:a] + body[b + 1:]
    body = body.replace("return wr_len;", "return reset ? 0 : wr_len;")
    return (text[:start] + body + text[end:]).encode()


def bound_btif_tx(files):
    name = "mtk_btif.c"
    text = files[name].decode()
    start = text.index("\nint btif_send_data(")
    end = text.index("\nint _btif_send_data(", start)
    body = text[start:end]
    old = """		int length = 0;
/*tx fifo in*/
		length = kfifo_in(p_btif->p_tx_fifo,
				  (unsigned char *)p_buf, buf_len);"""
    new = """		unsigned long flags;
		unsigned int length = 0;

		/* STP retries a complete write when this reports zero. */
		spin_lock_irqsave(&p_btif->tx_fifo_producer_lock, flags);
		if (kfifo_avail(p_btif->p_tx_fifo) >= buf_len)
			length = kfifo_in(p_btif->p_tx_fifo, p_buf, buf_len);
		spin_unlock_irqrestore(&p_btif->tx_fifo_producer_lock, flags);"""
    if body.count(old) != 1:
        raise ValueError("unexpected BTIF TX FIFO admission")
    text = text[:start] + body.replace(old, new) + text[end:]
    old = "\t\tmutex_init(&g_btif[index].ops_mtx);"
    if text.count(old) != 1:
        raise ValueError("unexpected BTIF singleton mutex initialization")
    text = text.replace(old, old + "\n\t\tspin_lock_init(&g_btif[index].tx_fifo_producer_lock);")
    start = text.index("\nstatic void btif_tx_worker(struct work_struct *p_work)\n{")
    end = text.index("\nstatic void btif_rx_tasklet(", start)
    worker = text[start:end]
    old = "kfifo_reset(p_btif->p_tx_fifo);"
    if worker.count(old) != 1:
        raise ValueError("unexpected BTIF TX FIFO fatal-error reset")
    # This is the sole consumer, so it may discard by advancing out only.
    worker = worker.replace(old, "kfifo_reset_out(p_btif->p_tx_fifo);")
    files[name] = (text[:start] + worker + text[end:]).encode()
    name = "mtk_btif.h"
    old = b"\tstruct kfifo *p_tx_fifo;"
    if files[name].count(old) != 1:
        raise ValueError("unexpected BTIF TX FIFO declaration")
    files[name] = files[name].replace(old, old + b"\n\tspinlock_t tx_fifo_producer_lock;")


def fix_btif_tx_cleanup(data):
    text = data.decode()
    begin = text.index("\nstatic int _btif_tx_ctx_init(struct _mtk_btif_ *p_btif)\n{")
    end = text.index("\nstatic int _btif_tx_ctx_deinit(", begin)
    init = text[begin:end]
    old = "\t\tkfree(p_btif->p_tx_fifo);"
    if init.count(old) != 1:
        raise ValueError("unexpected BTIF TX init error cleanup")
    # The caller also runs deinit on init failure. Do not leave a freed
    # descriptor for the second cleanup or publish it on a successful retry.
    init = init.replace(old, old + "\n\t\tp_btif->p_tx_fifo = NULL;")
    text = text[:begin] + init + text[end:]
    return text.encode()


def fix_btif_pio_progress(data):
    text = data.decode()
    start = text.index("\nint _btif_pio_write(")
    end = text.index("\nint _btif_dump_memory(", start)
    body = text[start:end]
    replacements = (
        ("unsigned int i_ret = 0;", "int i_ret = 0;"),
        ("\t\t\tif (i_ret > 0) {",
         "\t\t\t/* A negative HAL result is never a byte count. */\n"
         "\t\t\tif (i_ret < 0)\n"
         "\t\t\t\treturn sent_len ? (int)sent_len : i_ret;\n"
         "\t\t\tif ((unsigned int)i_ret > buf_len - sent_len)\n"
         "\t\t\t\treturn sent_len ? (int)sent_len : -EIO;\n"
         "\t\t\tif (i_ret > 0) {"),
        ("if ((++retry > max_tx_retry) || (i_ret < 0)) {", "if (++retry > max_tx_retry) {"),
        ("\ti_ret = sent_len;\n\treturn i_ret;", "\treturn sent_len;"),
    )
    for old, new in replacements:
        if body.count(old) != 1:
            raise ValueError("unexpected BTIF PIO progress handling")
        body = body.replace(old, new)
    return (text[:start] + body + text[end:]).encode()


def defer_btif_tx(files):
    header = files["mtk_btif.h"]
    field = b"\tstruct work_struct tx_work;"
    if header.count(field) != 1:
        raise ValueError("unexpected BTIF worker declaration")
    files["mtk_btif.h"] = header.replace(field,
        b"\tstruct delayed_work tx_work;\n\tbool tx_shutdown;")
    text = files["mtk_btif.c"].decode()
    a = text.index("\nstatic void btif_tx_worker(struct work_struct *p_work)\n{")
    b = text.index("\nstatic void btif_rx_tasklet(", a)
    text = text[:a] + """
static void btif_tx_worker(struct work_struct *p_work)
{
	struct _mtk_btif_ *p_btif = container_of(to_delayed_work(p_work),
						struct _mtk_btif_, tx_work);
	unsigned char local_buf[384];
	unsigned long flags, delay = 0;
	unsigned int i, count;
	int sent;

	mutex_lock(&p_btif->tx_mtx);
	for (i = 0; i < 16 && !READ_ONCE(p_btif->tx_shutdown); ++i) {
		count = kfifo_out_peek(p_btif->p_tx_fifo, local_buf, sizeof(local_buf));
		if (!count)
			break;
		sent = _btif_send_data(p_btif, local_buf, count);
		if (!sent || sent == -EAGAIN || sent == -ENOSPC || sent == -EBUSY) {
			delay = msecs_to_jiffies(10);
			break;
		}
		if (sent < 0 || sent > count) {
			/* Preserve the old fatal-transfer policy, never treat an
			 * invalid count as bytes consumed.
			 */
			kfifo_reset_out(p_btif->p_tx_fifo);
			break;
		}
		kfifo_skip_count(p_btif->p_tx_fifo, sent);
	}
	mutex_unlock(&p_btif->tx_mtx);
	spin_lock_irqsave(&p_btif->tx_fifo_producer_lock, flags);
	if (!p_btif->tx_shutdown && !kfifo_is_empty(p_btif->p_tx_fifo))
		queue_delayed_work(p_btif->p_tx_wq, &p_btif->tx_work, delay);
	spin_unlock_irqrestore(&p_btif->tx_fifo_producer_lock, flags);
}
""" + text[b:]
    pairs = (
        ("INIT_WORK(&(p_btif->tx_work), btif_tx_worker);",
         "INIT_DELAYED_WORK(&p_btif->tx_work, btif_tx_worker);"),
        ("\tBTIF_INFO_FUNC(\"succeed\\n\");\n\n\ti_ret = 0;\n\treturn i_ret;\nbtm_init_err:\n",
         "\tWRITE_ONCE(p_btif->tx_shutdown, false);\n"
         "\tBTIF_INFO_FUNC(\"succeed\\n\");\n\n\ti_ret = 0;\n\treturn i_ret;\nbtm_init_err:\n"),
        ("\t\tif (kfifo_avail(p_btif->p_tx_fifo) >= buf_len)\n"
         "\t\t\tlength = kfifo_in(p_btif->p_tx_fifo, p_buf, buf_len);\n"
         "\t\tspin_unlock_irqrestore(&p_btif->tx_fifo_producer_lock, flags);\n"
         "\t\tif (length == buf_len) {\n"
         "\t\t\tqueue_work(p_btif->p_tx_wq, &(p_btif->tx_work));",
         "\t\tif (!p_btif->tx_shutdown && p_btif->p_tx_fifo && p_btif->p_tx_wq) {\n"
         "\t\t\tif (kfifo_avail(p_btif->p_tx_fifo) >= buf_len)\n"
         "\t\t\t\tlength = kfifo_in(p_btif->p_tx_fifo, p_buf, buf_len);\n"
         "\t\t\tif (length && !delayed_work_pending(&p_btif->tx_work))\n"
         "\t\t\t\tqueue_delayed_work(p_btif->p_tx_wq, &p_btif->tx_work, 0);\n"
         "\t\t}\n\t\tspin_unlock_irqrestore(&p_btif->tx_fifo_producer_lock, flags);\n"
         "\t\tif (length == buf_len) {"),
    )
    # Restrict the init-tail replacement to the TX constructor.
    a = text.index("\nstatic int _btif_tx_ctx_init(struct _mtk_btif_ *p_btif)\n{")
    b = text.index("\nstatic int _btif_tx_ctx_deinit(", a)
    for index, (old, new) in enumerate(pairs):
        if index == 1:
            before = text[a:b]
            if before.count(old) != 1:
                raise ValueError("unexpected BTIF tx init tail")
            text = text[:a] + before.replace(old, new) + text[b:]
        else:
            if text.count(old) != 1:
                raise ValueError("unexpected BTIF TX scheduling")
            text = text.replace(old, new)
            a = text.index("\nstatic int _btif_tx_ctx_init(struct _mtk_btif_ *p_btif)\n{")
            b = text.index("\nstatic int _btif_tx_ctx_deinit(", a)
    a = text.index("\nstatic int _btif_tx_ctx_deinit(struct _mtk_btif_ *p_btif)\n{")
    b = text.index("\nstatic int _btif_rx_btm_init(", a)
    before = text[a:b]
    old = "\t\t\tdestroy_workqueue(p_btif->p_tx_wq);"
    if before.count(old) != 1:
        raise ValueError("unexpected BTIF TX teardown")
    before = before.replace(old,
        "\t\t\tunsigned long flags;\n\n"
        "\t\t\tspin_lock_irqsave(&p_btif->tx_fifo_producer_lock, flags);\n"
        "\t\t\tWRITE_ONCE(p_btif->tx_shutdown, true);\n"
        "\t\t\tspin_unlock_irqrestore(&p_btif->tx_fifo_producer_lock, flags);\n"
        "\t\t\tcancel_delayed_work_sync(&p_btif->tx_work);\n" + old)
    files["mtk_btif.c"] = (text[:a] + before + text[b:]).encode()


def stage(repo, dest, include_core=False, no_test_interfaces=False, probe_lifetime=False,
          software_alloc_check=False, rx_overrun_check=False, tx_fifo_check=False,
          tx_cleanup_check=False, pio_progress_check=False):
    tx_cleanup_check = tx_cleanup_check or pio_progress_check
    tx_fifo_check = tx_fifo_check or tx_cleanup_check
    rx_overrun_check = rx_overrun_check or tx_fifo_check
    software_alloc_check = software_alloc_check or rx_overrun_check
    include_core = include_core or no_test_interfaces or probe_lifetime or software_alloc_check
    files = {}
    for name in FILES + (("mtk_btif.c", "mtk_btif_exp.c") if include_core else ()):
        path = f"{SOURCE}/{name}"
        if name == "mtk_btif_exp.h":
            path = "mindone/modules/include/drivers/misc/mediatek/btif/common/inc/mtk_btif_exp.h"
        row = subprocess.check_output(["git", "-C", repo, "ls-tree", REV, path], text=True)
        if not row.startswith("100644 "):
            raise ValueError(f"unexpected source mode: {path}")
        files[name] = subprocess.check_output(["git", "-C", repo, "show", f"{REV}:{path}"])
    if include_core:
        # Linux 7.0 renamed the void platform remove callback.
        data = files["mtk_btif.c"]
        old = b".remove_new = mtk_btif_remove,"
        if data.count(old) != 1:
            raise ValueError("unexpected BTIF platform remove callback")
        files["mtk_btif.c"] = data.replace(old, b".remove = mtk_btif_remove,")
    if include_core:
        files["mtk_btif.c"] = strip_test_interfaces(files["mtk_btif.c"])
        fix_btif_rx_thread_stop(files)
        check_btif_clock_probe(files)
        files["mtk_btif.c"] = fix_btif_tx_cleanup(files["mtk_btif.c"])
    if software_alloc_check:
        fix_btif_coherent_rx(files)
        propagate_btif_dma_stop_errors(files)
        files["mtk_btif.c"] = register_btif_dma_irq_before_start(files["mtk_btif.c"])
    if rx_overrun_check:
        files["mtk_btif.c"] = report_rx_overrun(files["mtk_btif.c"])
    if tx_fifo_check:
        bound_btif_tx(files)
    if pio_progress_check:
        files["mtk_btif.c"] = fix_btif_pio_progress(files["mtk_btif.c"])
        defer_btif_tx(files)
    if include_core:
        bind_btif_lifetime(files)
    dest.mkdir()
    for name, data in files.items():
        (dest / name).write_bytes(data)
    (dest / "Kbuild").write_text("obj-m := t70_btif_hal.o\n"
                                "t70_btif_hal-y := btif_plat.o btif_dma_plat.o"
                                + (" mtk_btif.o mtk_btif_exp.o" if include_core else "") + "\n")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference", type=Path)
    p.add_argument("destination", type=Path)
    p.add_argument("--core-check", action="store_true")
    p.add_argument("--no-test-interfaces", action="store_true")
    p.add_argument("--probe-lifetime", "--fail-unbound", dest="probe_lifetime", action="store_true")
    p.add_argument("--software-alloc-check", action="store_true")
    p.add_argument("--rx-overrun-check", action="store_true")
    p.add_argument("--tx-fifo-check", action="store_true")
    p.add_argument("--tx-cleanup-check", action="store_true")
    p.add_argument("--pio-progress-check", action="store_true")
    a = p.parse_args()
    try:
        stage(a.reference, a.destination, a.core_check, a.no_test_interfaces, a.probe_lifetime,
              a.software_alloc_check, a.rx_overrun_check, a.tx_fifo_check,
              a.tx_cleanup_check, a.pio_progress_check)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        p.exit(2, f"BTIF staging refused: {error}\n")
