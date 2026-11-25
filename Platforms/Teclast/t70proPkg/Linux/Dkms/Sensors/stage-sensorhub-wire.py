#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the pinned T70 sensorhub wire layer for an object-only Linux build."""
import argparse
from pathlib import Path
import subprocess

REV = "a71515dbaf0d539ffb36416f14186dad17eef743"
BASE = "mindone/modules/sensorhub"
FILES = ("sensor_comm.c", "sensor_comm.h", "tiny_crc8.c", "tiny_crc8.h", "ipi_comm.h", "ready.h")


SCP_HEADERS = {
    "scp.h": "drivers/misc/mediatek/scp/include/scp.h",
    "scp_rv.h": "drivers/misc/mediatek/scp/include/scp_rv.h",
    "linux/soc/mediatek/mtk_tinysys_ipi.h": "include/linux/soc/mediatek/mtk_tinysys_ipi.h",
    "linux/soc/mediatek/mtk-mbox.h": "include/linux/soc/mediatek/mtk-mbox.h",
}

def replace_once(body, old, new):
    if body.count(old) != 1:
        raise ValueError(f"unexpected upstream sensorhub code near {old.strip()}")
    return body.replace(old, new)


def harden(text):
    a = text.index("\nstatic void sensor_comm_notify_handler(")
    b = text.index("\nstatic int sensor_comm_ctrl_seq_send(", a)
    handler = text[a:b]
    check = "\tif (sizeof(*notify) != len)\n\t\treturn;\n"
    handler = replace_once(handler, check, "")
    handler = replace_once(handler, "\tcrc = tiny_crc8(", check + "\tcrc = tiny_crc8(")
    text = text[:a] + handler + text[b:]
    a = text.index("\nint sensor_comm_ctrl_send(")
    b = text.index("\n/*\n * no need check scp_status", a)
    send = text[a:b]
    old = "\t\treturn 0;\n\t}"
    if send.count(old) != 2:
        raise ValueError("unexpected sensorhub offline-send branches")
    text = text[:a] + send.replace(old, "\t\treturn -EAGAIN;\n\t}") + text[b:]
    return replace_once(text,
                        "\tatomic_set(&sensor_comm_sequence, 0);\n\tipi_comm_init();",
                        "\tint ret;\n\n\tatomic_set(&sensor_comm_sequence, 0);\n"
                        "\tret = ipi_comm_init();\n\tif (ret)\n\t\treturn ret;")


def lock_sensor_notify_lifetime(text):
    text = replace_once(text, "#include <linux/atomic.h>\n",
        "#include <linux/atomic.h>\n#include <linux/mutex.h>\n#include <linux/rcupdate.h>\n")
    text = replace_once(text,
        "struct sensor_notify_handle sens_notify_handle[MAX_SENS_COMM_NOTIFY_CMD];",
        """struct sensor_notify_handle sens_notify_handle[MAX_SENS_COMM_NOTIFY_CMD];
static struct sensor_notify_handle __rcu *sens_notify_live[MAX_SENS_COMM_NOTIFY_CMD];
static DEFINE_MUTEX(sens_notify_lock);""")
    text = replace_once(text, "\thandle = &sens_notify_handle[notify->command];\n\tif (!handle->handler)",
        "\tguard(rcu)();\n\thandle = rcu_dereference(sens_notify_live[notify->command]);\n\tif (!handle)")
    text = replace_once(text, """	if (cmd >= MAX_SENS_COMM_NOTIFY_CMD)
		return;

	sens_notify_handle[cmd].private_data = private_data;
	sens_notify_handle[cmd].handler = f;""", """	if (cmd >= MAX_SENS_COMM_NOTIFY_CMD || !f)
		return;

	guard(mutex)(&sens_notify_lock);
	/* A command has one owner until its synchronous unregister. */
	if (rcu_access_pointer(sens_notify_live[cmd]))
		return;
	sens_notify_handle[cmd].private_data = private_data;
	sens_notify_handle[cmd].handler = f;
	rcu_assign_pointer(sens_notify_live[cmd], &sens_notify_handle[cmd]);""")
    text = replace_once(text,
        "\tsens_notify_handle[cmd].handler = NULL;\n\tsens_notify_handle[cmd].private_data = NULL;",
        """	guard(mutex)(&sens_notify_lock);
	if (!rcu_access_pointer(sens_notify_live[cmd]))
		return;
	rcu_assign_pointer(sens_notify_live[cmd], NULL);
	synchronize_rcu();
	sens_notify_handle[cmd].handler = NULL;
	sens_notify_handle[cmd].private_data = NULL;""")
    return text


def drain_sensor_ipi_callback(text):
    text = replace_once(text, "struct ipi_controller {",
        """#include <linux/rcupdate.h>

typedef void ipi_notify_fn(int id, void *data, unsigned int len);

struct ipi_controller {""")
    text = replace_once(text,
        "\tvoid (*notify_callback)(int id, void *data, unsigned int len);",
        "\tipi_notify_fn __rcu *notify_callback;")
    text = replace_once(text, """	WARN_ON(id != IPI_IN_SENSOR_NOTIFY);
	if (controller.notify_callback)
		controller.notify_callback(id, data, len);""", """	ipi_notify_fn *callback;

	if (id != IPI_IN_SENSOR_NOTIFY)
		return -EINVAL;
	guard(rcu)();
	callback = rcu_dereference(controller.notify_callback);
	if (callback)
		callback(id, data, len);""")
    text = replace_once(text, "\tcontroller.notify_callback = f;",
        "\t/* The sensor_comm lifecycle serializes register and unregister. */\n"
        "\trcu_assign_pointer(controller.notify_callback, f);")
    return replace_once(text, "\tcontroller.notify_callback = NULL;",
        "\trcu_assign_pointer(controller.notify_callback, NULL);\n\tsynchronize_rcu();")


def drain_sensor_ipi_queue(text):
    text = replace_once(text, "\tbool running;", "\tbool running;\n\tbool accepting;")
    text = replace_once(text,
        "static void ipi_prefetch_messages(void)\n{\n\tipi_transfer_messages();\n}\n\n", "")
    text = replace_once(text, "\t\tipi_prefetch_messages();\n", "")
    text = replace_once(text,
        "\tspin_lock_irqsave(&controller.lock, flags);\n\tlist_add_tail(&m->list, &controller.head);",
        "\tspin_lock_irqsave(&controller.lock, flags);\n"
        "\tif (!controller.accepting) {\n"
        "\t\tspin_unlock_irqrestore(&controller.lock, flags);\n"
        "\t\treturn -ESHUTDOWN;\n\t}\n"
        "\tlist_add_tail(&m->list, &controller.head);")
    text = replace_once(text, "\t\tm = list_first_entry(&controller.head,",
        "\t\tstatus = 0;\n\t\tm = list_first_entry(&controller.head,")
    text = replace_once(text, "\t\tgoto unregister_ctrl;\n\treturn 0;",
        "\t\tgoto unregister_ctrl;\n"
        "\tspin_lock_irq(&controller.lock);\n"
        "\tcontroller.accepting = true;\n\tspin_unlock_irq(&controller.lock);\n\treturn 0;")
    return replace_once(text, """void ipi_comm_exit(void)
{
	mtk_ipi_unregister(&scp_ipidev, IPI_IN_SENSOR_CTRL);
	mtk_ipi_unregister(&scp_ipidev, IPI_IN_SENSOR_NOTIFY);
	flush_workqueue(controller.workqueue);
	destroy_workqueue(controller.workqueue);
}""", """void ipi_comm_exit(void)
{
	spin_lock_irq(&controller.lock);
	controller.accepting = false;
	spin_unlock_irq(&controller.lock);

	/* Only the workqueue dispatches queued CTRL messages. Keep its
	 * ACK callback registered until accepted operations are complete.
	 */
	flush_workqueue(controller.workqueue);
	mtk_ipi_unregister(&scp_ipidev, IPI_IN_SENSOR_CTRL);
	mtk_ipi_unregister(&scp_ipidev, IPI_IN_SENSOR_NOTIFY);
	destroy_workqueue(controller.workqueue);
	controller.workqueue = NULL;
}""")


def harden_ipi(text):
    old = """	ret = ipi_retry_transfer(hw->id, (void *)hw->tx, hw->tx_len);
	if (ret < 0)
		return ret;"""
    new = """	ret = ipi_retry_transfer(hw->id, (void *)hw->tx, hw->tx_len);
	if (ret < 0) {
		spin_lock_irqsave(&hw_transfer_lock, flags);
		hw->context = NULL;
		spin_unlock_irqrestore(&hw_transfer_lock, flags);
		return ret;
	}"""
    text = replace_once(text, old, new)
    text = replace_once(
        text, "\t/* only copy hw->rx_len bytes to hw->rx to avoid memory corruption */\n"
              "\tmemcpy(hw->rx, buffer, hw->rx_len);",
        "\tif (!buffer || len != hw->rx_len) {\n"
        "\t\thw->count = -EBADMSG;\n\t\tcomplete(hw->context);\n"
        "\t\thw->context = NULL;\n\t\tgoto out;\n\t}\n"
        "\tmemcpy(hw->rx, buffer, len);")
    text = replace_once(text, "\thw->count = len;\n\tcomplete(hw->context);",
                        "\thw->count = len;\n\tcomplete(hw->context);\n\thw->context = NULL;")
    text = replace_once(text, "static inline int ipi_retry_transfer(int id, void *tx, int tx_len)\n"
                              "{\n\tint ret = 0, retry = 0;\n",
        """static inline int ipi_retry_transfer(int id, const void *tx, unsigned int tx_len)
{
	u32 wire[PIN_OUT_SIZE_SENSOR_CTRL] = {};
	unsigned int max_slots;
	int ret = 0, retry = 0;

	switch (id) {
	case IPI_OUT_SENSOR_CTRL: max_slots = PIN_OUT_SIZE_SENSOR_CTRL; break;
	case IPI_OUT_SENSOR_NOTIFY: max_slots = PIN_OUT_SIZE_SENSOR_NOTIFY; break;
	default: return -EINVAL;
	}
	if (!tx || !tx_len || tx_len > max_slots * MBOX_SLOT_SIZE)
		return -EINVAL;
	/* The provider copies whole mailbox words even for a short last word. */
	memcpy(wire, tx, tx_len);
""")
    text = replace_once(text, "id, 0, tx, ipi_len(tx_len), 0);",
                              "id, 0, wire, ipi_len(tx_len), 0);")
    text = replace_once(text, "\tstruct ipi_hw_transfer *hw = &hw_transfer;\n\n"
                              "\tspin_lock_irqsave(&hw_transfer_lock, flags);",
        "\tstruct ipi_hw_transfer *hw = &hw_transfer;\n\n"
        "\tif (!t || t->id != IPI_OUT_SENSOR_CTRL ||\n"
        "\t    (t->rx_len && !t->rx_buf) || t->rx_len > sizeof(ctrl_payload))\n"
        "\t\treturn -EINVAL;\n\n"
        "\tspin_lock_irqsave(&hw_transfer_lock, flags);\n"
        "\thw->count = -ETIMEDOUT;")
    text = replace_once(text, "ipi_retry_transfer(hw->id, (void *)hw->tx, hw->tx_len)",
                              "ipi_retry_transfer(t->id, t->tx_buf, t->tx_len)")
    a = text.index("\n\tif (!timeout) {")
    b = text.index("\n\treturn hw->count;", a) + len("\n\treturn hw->count;")
    if "hw->tx[4]" not in text[a:b]:
        raise ValueError("unexpected stock IPI timeout handling")
    text = text[:a] + """
	if (!timeout && hw->context)
		pr_err_ratelimited("IPI %d timed out: %u TX bytes\\n", t->id, t->tx_len);
	ret = hw->count;
	hw->context = NULL;
	spin_unlock_irqrestore(&hw_transfer_lock, flags);
	return ret;""" + text[b:]
    text = replace_once(text, "\tmemcpy(hw->rx, buffer, len);",
                              "\tif (len)\n\t\tmemcpy(hw->rx, buffer, len);")
    start = text.index("\nint ipi_comm_init(")
    end = text.index("\nvoid ipi_comm_exit(", start)
    init = text[start:end]
    init = replace_once(init, "\t\treturn -1;", "\t\treturn -ENOMEM;")
    init = replace_once(init, '\tif (ret < 0)\n\t\tpr_err("register ipi %u fail %d\\n", IPI_IN_SENSOR_CTRL, ret);',
                        "\tif (ret < 0)\n\t\tgoto free_queue;")
    init = replace_once(init, '\tif (ret < 0)\n\t\tpr_err("register ipi %u fail %d\\n", IPI_IN_SENSOR_NOTIFY, ret);',
                        "\tif (ret < 0)\n\t\tgoto unregister_ctrl;")
    init = replace_once(init, "\treturn 0;\n}", """	return 0;

unregister_ctrl:
	mtk_ipi_unregister(&scp_ipidev, IPI_IN_SENSOR_CTRL);
free_queue:
	destroy_workqueue(controller.workqueue);
	controller.workqueue = NULL;
	return ret;
}""")
    return text[:start] + init + text[end:]


def stage_ready(text):
    # Remain offline if the protocol never becomes ready; hardware reset
    # is not part of this open source readiness/notification stage.
    a = text.index("\nstatic void sensor_rescure_work_fn(")
    b = text.index("\nint host_ready_init(", a)
    if text[a:b].count("scp_wdt_reset(0);") != 1:
        raise ValueError("unexpected sensorhub recovery work")
    text = text[:a] + text[b:]
    for line in (
        "static struct delayed_work sensor_rescure_work;\n",
        "\t\tqueue_delayed_work(sensor_ready_workqueue,\n"
        "\t\t\t&sensor_rescure_work, msecs_to_jiffies(5000));\n",
        "\tINIT_DELAYED_WORK(&sensor_rescure_work, sensor_rescure_work_fn);\n",
    ):
        text = replace_once(text, line, "")
    cancel = "\t\tcancel_delayed_work(&sensor_rescure_work);\n"
    if text.count(cancel) != 2:
        raise ValueError("unexpected sensorhub recovery cancellation")
    text = text.replace(cancel, "")
    # The no-ack path always sends 28 bytes even for zero payload length.
    return replace_once(text, "\tstruct sensor_comm_notify notify;",
                        "\tstruct sensor_comm_notify notify = { 0 };")


def stage_sensor_list(text):
    text = replace_once(text, "static struct sensor_comm_notify rx_notify;",
                        "static u8 pending_sequence;\nstatic bool list_pending;\n"
                        "static u32 list_write_position;")
    text = replace_once(
        text, "\tmemcpy(&rx_notify, n, sizeof(rx_notify));\n"
              "\tspin_unlock(&rx_notify_lock);\n\tcomplete(&sensor_list_done);",
        "\tif (list_pending && n->sequence == pending_sequence &&\n"
        "\t    n->sensor_type == SENSOR_TYPE_INVALID &&\n"
        "\t    n->command == SENS_COMM_NOTIFY_LIST_CMD) {\n"
        "\t\tlist_write_position = n->value[0];\n"
        "\t\tlist_pending = false;\n\t\tcomplete(&sensor_list_done);\n\t}\n"
        "\tspin_unlock(&rx_notify_lock);")
    text = replace_once(
        text, "\tnotify.length = 0;\n\tret = sensor_comm_notify(&notify);",
        "\tnotify.length = 0;\n\tspin_lock_irqsave(&rx_notify_lock, flags);\n"
        "\tpending_sequence = notify.sequence;\n\tlist_pending = true;\n"
        "\tspin_unlock_irqrestore(&rx_notify_lock, flags);\n"
        "\tret = sensor_comm_notify(&notify);")
    text = replace_once(
        text, '\t\tpr_err("notify fail %d\\n", ret);\n\t\treturn ret;',
        "\t\tspin_lock_irqsave(&rx_notify_lock, flags);\n\t\tlist_pending = false;\n"
        "\t\tspin_unlock_irqrestore(&rx_notify_lock, flags);\n"
        '\t\tpr_err("notify fail %d\\n", ret);\n\t\treturn ret;')
    a = text.index("\tif (!timeout) {")
    b = text.index("\n\tret = share_mem_seek(", a)
    if "rx_notify.sequence != notify.sequence &&" not in text[a:b]:
        raise ValueError("unexpected upstream sensorhub enumeration reply check")
    text = text[:a] + """	spin_lock_irqsave(&rx_notify_lock, flags);
	list_pending = false;
	write_position = list_write_position;
	spin_unlock_irqrestore(&rx_notify_lock, flags);
	if (!timeout)
		return -ETIMEDOUT;
""" + text[b:]
    text = replace_once(text, "\tstruct sensor_comm_notify notify;",
                        "\tstruct sensor_comm_notify notify = { 0 };")
    return replace_once(text, "\tmemset(&rx_notify, 0, sizeof(rx_notify));",
                        "\tlist_pending = false;\n\tlist_write_position = 0;")


def stage_share_memory(text):
    text = replace_once(
        text, "\tmutex_lock(&shm->lock);\n\tshm->write_position = write_position;",
        "\tmutex_lock(&shm->lock);\n"
        "\tif (!shm->item_size || write_position >= shm->host_capacity ||\n"
        "\t    write_position % shm->item_size) {\n"
        "\t\tmutex_unlock(&shm->lock);\n\t\treturn -EINVAL;\n\t}\n"
        "\tshm->write_position = write_position;")
    anchor = "\nstatic int share_mem_read_dram("
    helper = """
/* Validate peer-owned ring metadata against the locally configured span. */
static bool share_mem_ring_valid(struct share_mem *shm, u32 rp, u32 wp,
				u32 buffer_size, u32 item_size)
{
	return item_size && item_size == shm->item_size &&
	       buffer_size && buffer_size == shm->host_capacity &&
	       rp < buffer_size && wp < buffer_size &&
	       rp % item_size == 0 && wp % item_size == 0;
}
"""
    text = replace_once(text, anchor, helper + anchor)
    old = "\tif (item_size != shm->item_size)\n\t\treturn -EIO;"
    if text.count(old) != 2:
        raise ValueError("unexpected sensorhub ring validation")
    text = text.replace(old,
                        "\tif (!share_mem_ring_valid(shm, rp, wp, buffer_size, item_size))\n"
                        "\t\treturn -EIO;")
    for field in ("rp", "wp", "buffer_size", "item_size"):
        text = text.replace(f"{field} = shm->base->{field};",
                            f"{field} = READ_ONCE(shm->base->{field});")
    a = text.index("\nint share_mem_init(")
    b = text.index("\nstatic int share_mem_send_config(", a)
    init = """
int share_mem_init(struct share_mem *shm, struct share_mem_config *cfg)
{
	const u32 header = offsetof(struct share_mem_base, data);
	u32 slots, capacity, threshold = 0;

	if (!cfg || !cfg->base || !shm->name || !shm->item_size ||
	    cfg->buffer_size <= header)
		return -EINVAL;
	slots = (cfg->buffer_size - header) / shm->item_size;
	if (slots < 2)
		return -EINVAL;
	capacity = slots * shm->item_size;
	if (shm->buffer_full_detect) {
		threshold = ((u64)(slots - 1) * 8 / 10) * shm->item_size;
		if (threshold <= shm->item_size)
			return -EINVAL;
	}

	mutex_lock(&shm->lock);
	shm->host_capacity = capacity;
	cfg->base->wp = 0;
	cfg->base->rp = 0;
	cfg->base->item_size = shm->item_size;
	cfg->base->buffer_size = capacity;
	shm->write_position = 0;
	shm->last_write_position = 0;
	shm->buffer_full_written = 0;
	shm->buffer_full_threshold = threshold;
	shm->base = cfg->base;
	mutex_unlock(&shm->lock);
	return 0;
}
"""
    return text[:a] + init + text[b:]


def stage_config_handoff(text):
    text = replace_once(text, "\tbool init_status;\n", "")
    text = replace_once(text, "static int share_mem_send_config(void)",
                        "static int share_mem_send_config(const u32 *bases)")
    a = text.index("\t\t/* host init share mem ready")
    b = text.index("\n\t\tif (index == ARRAY_SIZE", a)
    if "scp_get_reserve_mem_phys(usage->id)" not in text[a:b]:
        raise ValueError("unexpected SCP base lookup in share config")
    text = text[:a] + """		comm_shm->base_info[index].payload_type = usage->payload_type;
		comm_shm->base_info[index].payload_base = bases[i];
		comm_shm->available_num = ++index;""" + text[b:]
    a = text.index("\nint share_mem_config(void)")
    b = text.index("\nvoid share_mem_config_handler_register(", a)
    return text[:a] + """
static bool share_mem_valid_extent(phys_addr_t phys, phys_addr_t virt,
				   phys_addr_t size)
{
	return phys && virt && size > offsetof(struct share_mem_base, data) &&
	       IS_ALIGNED(phys, 4) && IS_ALIGNED(virt, 4) &&
	       phys <= U32_MAX && size <= U32_MAX && size - 1 <= U32_MAX - phys;
}

int share_mem_config(void)
{
	struct share_mem_config configs[ARRAY_SIZE(shm_usage_table)] = { 0 };
	u32 bases[ARRAY_SIZE(shm_usage_table)];
	unsigned int i, j;
	int ret;

	/* Before callbacks can write metadata, validate all provider extents. */
	for (i = 0; i < ARRAY_SIZE(shm_usage_table); i++) {
		const struct share_mem_usage *usage = &shm_usage_table[i];
		struct share_mem_config *cfg = &configs[i];
		phys_addr_t phys, virt, size;

		if (usage->payload_type >= MAX_SHARE_MEM_PAYLOAD_TYPE)
			return -EINVAL;
		if (!shm_handle[usage->payload_type].handler)
			return -ENODEV;

		phys = scp_get_reserve_mem_phys(usage->id);
		virt = scp_get_reserve_mem_virt(usage->id);
		size = scp_get_reserve_mem_size(usage->id);
		if (!share_mem_valid_extent(phys, virt, size))
			return -ERANGE;

		for (j = 0; j < i; j++)
			if (phys < (u64)bases[j] + configs[j].buffer_size &&
			    bases[j] < phys + size)
				return -EINVAL;

		bases[i] = phys;
		cfg->payload_type = usage->payload_type;
		cfg->base = (void *)(uintptr_t)virt;
		cfg->buffer_size = size;
	}

	for (i = 0; i < ARRAY_SIZE(configs); i++) {
		struct share_mem_config *cfg = &configs[i];
		const struct share_mem_config_handle *handle = &shm_handle[cfg->payload_type];

		ret = handle->handler(cfg, handle->private_data);
		if (ret)
			return ret;
	}

	return share_mem_send_config(bases);
}
""" + text[b:]


def stage_transceiver(text):
    text = replace_once(text, "#include <linux/kthread.h>\n",
                        "#include <linux/kthread.h>\n#include <linux/workqueue.h>\n")
    a = text.index("\nstatic void transceiver_sensor_bootup(")
    b = text.index("\nstatic struct notifier_block transceiver_ready_notifier", a)
    if text[a:b].count("atomic_xchg(&dev->first_bootup, false)") != 1:
        raise ValueError("unexpected T70 first-ready flow")
    return text[:a] + """
#define TRANSCEIVER_BOOT_ATTEMPTS 5
#define TRANSCEIVER_BOOT_RETRY_MS 100
static unsigned int transceiver_boot_remaining;
static void transceiver_boot_retry(struct work_struct *work);
static DECLARE_DELAYED_WORK(transceiver_boot_work, transceiver_boot_retry);

/* sensor_boot_lock serializes retries, READY/STOP and the report worker. */
static int transceiver_sensor_bootup(struct transceiver_device *dev)
{
	int ret = share_mem_config();

	if (ret) {
		pr_err("share mem config fail %d\\n", ret);
		return ret;
	}
	if (atomic_read(&dev->first_bootup)) {
		ret = transceiver_create_manager(dev);
		if (ret) {
			pr_err("create manager fail %d\\n", ret);
			return ret;
		}
		atomic_set(&dev->first_bootup, false);
	} else {
		transceiver_restore_sensor(dev);
	}
	timesync_filter_reset(&dev->filter);
	transceiver_sample_gate(true);
	timesync_start();
	return 0;
}

static void transceiver_try_boot(void)
{
	if (!transceiver_boot_remaining)
		return;
	transceiver_boot_remaining--;
	if (!transceiver_sensor_bootup(&transceiver_dev))
		transceiver_boot_remaining = 0;
	else if (transceiver_boot_remaining)
		mod_delayed_work(system_dfl_wq, &transceiver_boot_work,
				 msecs_to_jiffies(TRANSCEIVER_BOOT_RETRY_MS));
}

static void transceiver_boot_retry(struct work_struct *work)
{
	mutex_lock(&sensor_boot_lock);
	transceiver_try_boot();
	mutex_unlock(&sensor_boot_lock);
}

static void transceiver_stop_boot(void)
{
	/* Wake a reporting loop before waiting for its lock. */
	transceiver_sample_gate(false);
	mutex_lock(&sensor_boot_lock);
	transceiver_boot_remaining = 0;
	cancel_delayed_work(&transceiver_boot_work);
	transceiver_sample_gate(false);
	timesync_stop();
	mutex_unlock(&sensor_boot_lock);
}

static int transceiver_ready_notifier_call(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	if (event) {
		mutex_lock(&sensor_boot_lock);
		transceiver_sample_gate(false);
		timesync_stop();
		cancel_delayed_work(&transceiver_boot_work);
		transceiver_boot_remaining = TRANSCEIVER_BOOT_ATTEMPTS;
		transceiver_try_boot();
		mutex_unlock(&sensor_boot_lock);
	} else {
		transceiver_stop_boot();
	}
	return NOTIFY_DONE;
}
""" + text[b:]

def stage_sample_backpressure(text):
    a = text.index("\nstatic void transceiver_report(")
    b = text.index("\nstatic int transceiver_translate(", a)
    report = text[a:b]
    report = replace_once(report, "\tif (!manager)\n\t\treturn;",
                           "\tu64 started = ktime_get_boottime_ns();\n\n"
                           "\tif (!manager || kthread_should_stop())\n\t\treturn;")
    report = replace_once(report, "\tdo {\n\t\tif (action != FLUSH_ACTION)",
                           "\tdo {\n\t\tif (kthread_should_stop())\n\t\t\treturn;\n"
                           "\t\tif (action != FLUSH_ACTION)")
    report = replace_once(
        report, "\t\tif (ret < 0)\n\t\t\tusleep_range(2000, 4000);\n\t} while (ret < 0);",
        "\t\t/* The stock HF FIFO resets a stalled client at 1 second. */\n"
        "\t\tif (ret != -EAGAIN || kthread_should_stop() ||\n"
        "\t\t    ktime_get_boottime_ns() - started >= 1100ULL * NSEC_PER_MSEC)\n"
        "\t\t\tbreak;\n"
        "\t\tusleep_range(2000, 4000);\n"
        "\t} while (true);\n"
        "\tif (ret)\n"
        "\t\tpr_warn_ratelimited(\"sensor report failed: %d\\n\", ret);")
    return text[:a] + report + text[b:]


def stage_sample_gate(text):
    anchor = "DEFINE_KFIFO(transceiver_super_fifo, uint32_t, 32);\n"
    text = replace_once(text, anchor, anchor + """
static DEFINE_MUTEX(sensor_boot_lock);
static bool transceiver_samples_online;

static void transceiver_sample_gate(bool online)
{
	unsigned long flags;

	spin_lock_irqsave(&transceiver_fifo_lock, flags);
	WRITE_ONCE(transceiver_samples_online, online);
	if (!online) {
		kfifo_reset(&transceiver_fifo);
		kfifo_reset(&transceiver_super_fifo);
	}
	spin_unlock_irqrestore(&transceiver_fifo_lock, flags);
}
""")
    a = text.index("\nstatic void transceiver_notify_func(")
    b = text.index("\nstatic bool transceiver_wakeup_check(", a)
    notify = text[a:b]
    old = "\tspin_lock(&transceiver_fifo_lock);\n"
    if notify.count(old) != 2:
        raise ValueError("unexpected transceiver notify lock sites")
    notify = notify.replace(old, old +
                            "\tif (!READ_ONCE(transceiver_samples_online)) {\n"
                            "\t\tspin_unlock(&transceiver_fifo_lock);\n\t\treturn;\n\t}\n")
    text = text[:a] + notify + text[b:]
    text = replace_once(text, "\t\ttransceiver_process(dev);\n\t\ttransceiver_process_super(dev);",
                        "\t\tmutex_lock(&sensor_boot_lock);\n"
                        "\t\tif (READ_ONCE(transceiver_samples_online)) {\n"
                        "\t\t\ttransceiver_process(dev);\n\t\t\ttransceiver_process_super(dev);\n\t\t}\n"
                        "\t\tmutex_unlock(&sensor_boot_lock);")
    a = text.index("\nstatic void transceiver_report(")
    b = text.index("\nstatic int transceiver_translate(", a)
    report = text[a:b]
    if report.count("kthread_should_stop()") != 3:
        raise ValueError("unexpected transceiver report stop checks")
    report = report.replace("kthread_should_stop()", "(kthread_should_stop() || "
                            "!READ_ONCE(transceiver_samples_online))")
    text = text[:a] + report + text[b:]
    for name in ("transceiver_read", "transceiver_process", "transceiver_read_super",
                 "transceiver_process_super"):
        a = text.index(f"\nstatic void {name}(")
        b = text.index("\n}\n", a)
        fn = replace_once(text[a:b], "\twhile (1) {",
                          "\twhile (READ_ONCE(transceiver_samples_online)) {")
        text = text[:a] + fn + text[b:]
    return text


def stage_timesync(text):
    text = replace_once(text, "#include <linux/timer.h>", "#include <linux/mutex.h>")
    text = replace_once(text, "static struct timer_list timesync_timer;\n"
                        "static struct work_struct timesync_work;",
                        "static struct delayed_work timesync_work;\n"
                        "static DEFINE_MUTEX(timesync_run_lock);\n"
                        "static bool timesync_requested, timesync_armed, timesync_dead;")
    a = text.index("\nstatic void timesync_work_func(")
    b = text.index("\nvoid timesync_filter_set(", a)
    if text[a:b].count("schedule_work(&timesync_work);") != 1:
        raise ValueError("unexpected sensor timesync scheduling")
    text = text[:a] + """
static void timesync_work_func(struct work_struct *work)
{
	timesync_comm_with();
	queue_delayed_work(system_dfl_wq, &timesync_work, msecs_to_jiffies(10000));
}
""" + text[b:]
    a = text.index("\nvoid timesync_start(")
    b = text.index("\nint timesync_init(", a)
    if text[a:b].count("del_timer_sync(&timesync_timer);") != 1:
        raise ValueError("unexpected sensor timesync stop callback")
    text = text[:a] + """
static void timesync_arm_locked(void)
{
	if (timesync_dead || READ_ONCE(timesync_suspend_flag) || !timesync_requested)
		return;
	if (!timesync_armed) {
		enable_delayed_work(&timesync_work);
		timesync_armed = true;
	}
	mod_delayed_work(system_dfl_wq, &timesync_work, msecs_to_jiffies(10000));
}

static void timesync_drain_locked(void)
{
	if (timesync_armed) {
		disable_delayed_work_sync(&timesync_work);
		timesync_armed = false;
	}
}

void timesync_start(void)
{
	mutex_lock(&timesync_run_lock);
	timesync_requested = !timesync_dead;
	timesync_arm_locked();
	mutex_unlock(&timesync_run_lock);
}

void timesync_stop(void)
{
	mutex_lock(&timesync_run_lock);
	timesync_requested = false;
	timesync_drain_locked();
	mutex_unlock(&timesync_run_lock);
}

void timesync_resume(void)
{
	mutex_lock(&timesync_run_lock);
	WRITE_ONCE(timesync_suspend_flag, false);
	if (!timesync_dead && timesync_requested) {
		timesync_comm_with();
		timesync_arm_locked();
	}
	mutex_unlock(&timesync_run_lock);
}

void timesync_suspend(void)
{
	mutex_lock(&timesync_run_lock);
	WRITE_ONCE(timesync_suspend_flag, true);
	timesync_drain_locked();
	mutex_unlock(&timesync_run_lock);
}
""" + text[b:]
    text = replace_once(text, "\tINIT_WORK(&timesync_work, timesync_work_func);\n"
                        "\ttimer_setup(&timesync_timer, timesync_timer_func, 0);",
                        "\tINIT_DELAYED_WORK(&timesync_work, timesync_work_func);\n"
                        "\tdisable_delayed_work_sync(&timesync_work);")
    return replace_once(text, "\ttimesync_stop();\n\twakeup_source_unregister(wakeup_src);\n"
                        "\tflush_work(&timesync_work);",
                        "\tmutex_lock(&timesync_run_lock);\n\ttimesync_dead = true;\n"
                        "\ttimesync_requested = false;\n\ttimesync_drain_locked();\n"
                        "\twakeup_source_unregister(wakeup_src);\n"
                        "\tmutex_unlock(&timesync_run_lock);")


def stage_counter_conversion(text):
    text = replace_once(text, '#include <linux/mutex.h>',
                        '#include <linux/mutex.h>\n#include <linux/math64.h>\n#include <linux/overflow.h>')
    a = text.index("/* arch counter is 13M")
    b = text.index("\nstatic void timesync_filter_calculate(", a)
    if "ARCH_TIMER_MULT 161319385" not in text[a:b]:
        raise ValueError("unexpected T70 counter ratio")
    text = text[:a] + """
/* Stock T70 DT's architectural counter is 13 MHz: 1000 ns / 13 ticks. */
static bool t70_counter_ns(s64 ticks, s64 *ns)
{
	s32 rem;
	s64 whole = div_s64_rem(ticks, 13, &rem);

	if (check_mul_overflow(whole, 1000LL, &whole))
		return false;
	return !check_add_overflow(whole, (s64)(rem * 1000 / 13), ns);
}

void timesync_filter_reset(struct timesync_filter *filter)
{
	unsigned long flags;

	spin_lock_irqsave(&filter->lock, flags);
	filter->tail = 0;
	filter->cnt = 0;
	filter->offset = 0;
	filter->offset_debug = 0;
	filter->last_time = 0;
	spin_unlock_irqrestore(&filter->lock, flags);
}
""" + text[b:]
    text = replace_once(text, "\tint64_t ipi_transfer_time = 0;",
                        "\ts64 delta_ticks, ipi_transfer_time, adjusted;")
    return replace_once(text,
                        "\tipi_transfer_time = arch_counter_to_ns(host_archcounter -\n"
                        "\t\tscp_archcounter);\n\tscp_timestamp = scp_timestamp + ipi_transfer_time;\n\n"
                        "\ttimesync_filter_calculate(filter, host_timestamp, scp_timestamp);",
                        "\tif (check_sub_overflow(host_archcounter, scp_archcounter, &delta_ticks) ||\n"
                        "\t    !t70_counter_ns(delta_ticks, &ipi_transfer_time) ||\n"
                        "\t    check_add_overflow(scp_timestamp, ipi_transfer_time, &adjusted) ||\n"
                        "\t    adjusted < 0)\n\t\treturn;\n\n"
                        "\ttimesync_filter_calculate(filter, host_timestamp, adjusted);")


def drop_sensor_irq_latency_debug(text):
    # The vendor monitor is diagnostic only. Avoid replacing deferred
    # printk with synchronous logging on this IPI receive path.
    text = replace_once(text, "static DEFINE_RATELIMIT_STATE(ratelimit, 5 * HZ, 10);\n", "")
    a = text.index("\nstatic void transceiver_notify_func(")
    b = text.index("\nstatic void transceiver_super_notify_func(", a)
    body = text[a:b]
    for row in (
        "\tuint64_t start_time = 0, end_time = 0, timeout_ns = 1000000;\n",
        "\tuint64_t wait_spin_lock_end_time = 0, timesync_end_time = 0;\n",
        "\tuint64_t kfifo_end_time = 0, complete_end_time = 0;\n",
        "\tstart_time = ktime_get_boottime_ns();\n",
        "\twait_spin_lock_end_time = ktime_get_boottime_ns();\n",
        "\ttimesync_end_time = ktime_get_boottime_ns();\n",
        "\tkfifo_end_time = ktime_get_boottime_ns();\n",
        "\tcomplete_end_time = ktime_get_boottime_ns();\n",
        "\tend_time = ktime_get_boottime_ns();\n"
        "\tif (end_time - start_time > timeout_ns && __ratelimit(&ratelimit))\n"
        "\t\tprintk_deferred(\"time monitor:%llu, %llu, %llu, %llu, %llu, %llu\\n\",\n"
        "\t\t\tstart_time, wait_spin_lock_end_time, timesync_end_time,\n"
        "\t\t\tkfifo_end_time, complete_end_time, end_time);\n",
    ):
        body = replace_once(body, row, "")
    return text[:a] + body + text[b:]


def unwind_sensor_transceiver_init(text):
    a = text.index("\nstatic int __init transceiver_init(")
    b = text.index("\nstatic void __exit transceiver_exit(", a)
    body = text[a:b]
    body = replace_once(body,
        '\t\tpr_err("register hf device fail %d\\n", ret);\n\t\treturn ret;',
        '\t\tpr_err("register hf device fail %d\\n", ret);\n\t\tgoto out_wakeup;')
    body = replace_once(body, "out_ready:\n\thost_ready_exit();",
        "out_ready:\n\t/* host_ready_init() did not create its workqueue. */")
    body = replace_once(body,
        "\tsensor_comm_notify_handler_unregister(SENS_COMM_NOTIFY_DATA_CMD);\nout_pm_notify:",
        "\tsensor_comm_notify_handler_unregister(SENS_COMM_NOTIFY_DATA_CMD);\n"
        "\tkthread_stop(dev->task);\n\tdev->task = NULL;\nout_pm_notify:")
    body = replace_once(body, "\thf_device_unregister(&dev->hf_dev);\n\twakeup_source_unregister",
        "\thf_device_unregister(&dev->hf_dev);\nout_wakeup:\n\twakeup_source_unregister")
    exit_body = text[b:]
    ready = ("\tsensor_ready_notifier_chain_unregister(&transceiver_ready_notifier);\n"
             "\thost_ready_exit();\n")
    exit_body = replace_once(exit_body, ready, "")
    exit_body = replace_once(exit_body,
        "\tshare_mem_config_handler_unregister(SHARE_MEM_SUPER_DATA_PAYLOAD_TYPE);",
        ready + "\ttransceiver_stop_boot();\n"
        "\tcancel_delayed_work_sync(&transceiver_boot_work);\n"
        "\tshare_mem_config_handler_unregister(SHARE_MEM_SUPER_DATA_PAYLOAD_TYPE);")
    return text[:a] + body + exit_body


def serialize_sensor_shm_config(text):
    text = replace_once(text,
        "static struct share_mem_config_handle shm_handle[MAX_SHARE_MEM_PAYLOAD_TYPE];",
        """/* Sleepable callbacks hold this lock until the configuration
 * transaction completes. They must not register or unregister themselves.
 */
static DEFINE_MUTEX(shm_handler_lock);
static struct share_mem_config_handle shm_handle[MAX_SHARE_MEM_PAYLOAD_TYPE];""")
    text = replace_once(text, "\t/* Before callbacks can write metadata, validate all provider extents. */",
        "\tguard(mutex)(&shm_handler_lock);\n"
        "\t/* Before callbacks can write metadata, validate all provider extents. */")
    text = replace_once(text,
        "\tshm_handle[payload_type].private_data = private_data;\n\tshm_handle[payload_type].handler = f;",
        "\tguard(mutex)(&shm_handler_lock);\n"
        "\tif (!f || shm_handle[payload_type].handler)\n\t\treturn;\n"
        "\tshm_handle[payload_type].private_data = private_data;\n"
        "\tshm_handle[payload_type].handler = f;")
    return replace_once(text, "\tshm_handle[payload_type].handler = NULL;",
        "\tguard(mutex)(&shm_handler_lock);\n\tshm_handle[payload_type].handler = NULL;")


def drop_sensor_vendor_diagnostics(text):
    for row in ('#include "debug.h"\n', '#include "custom_cmd.h"\n',
                'extern uint32_t *awinic_get_global_val(void);\n',
                '\tdev->hf_dev.debug = transceiver_debug;\n',
                '\tdev->hf_dev.custom_cmd = transceiver_custom_cmd;\n'):
        text = replace_once(text, row, "")
    a = text.index("/* awinic bob add start */")
    b = text.index("static int transceiver_translate(", a)
    text = text[:a] + text[b:]
    a = text.index("static int transceiver_translate(")
    b = text.index("\nstatic void transceiver_read(", a)
    body = text[a:b]
    for start, end in (
        ("\t\t\t//awinic bob add\n", "\t\t\tmemcpy(dst->word"),
        ("\t\t //awinic bob add start\n", "\t\tdst->timestamp"),
    ):
        i = body.index(start)
        j = body.index(end, i)
        if body[i:j].count("awinic_get_global_val()") != 1:
            raise ValueError("unexpected sensor SAR diagnostic hook")
        body = body[:i] + body[j:]
    text = text[:a] + body + text[b:]
    a = text.index("\nstatic int transceiver_debug(")
    b = text.index("\n}\n", text.index("\nstatic int transceiver_custom_cmd(", a)) + len("\n}\n")
    text = text[:a] + text[b:]
    a = text.index("\tret = debug_init();")
    b = text.index("\tdev->filter.max_diff", a)
    if text[a:b].count("ret = custom_cmd_init();") != 1:
        raise ValueError("unexpected vendor diagnostics startup")
    text = text[:a] + text[b:]
    text = replace_once(text, "\t\tgoto out_cust_cmd;", "\t\tgoto out_sensor_list;")
    text = replace_once(text, "out_cust_cmd:\n\tcustom_cmd_exit();\nout_debug:\n\tdebug_exit();\n", "")
    return replace_once(text, "\tcustom_cmd_exit();\n\tdebug_exit();\n", "")


def stage_sample_lifetime(text):
    text = replace_once(text, "#include <linux/completion.h>",
                        "#include <linux/wait.h>")
    text = replace_once(text, "#include <uapi/linux/sched/types.h>\n", "")
    text = replace_once(text, "static struct transceiver_device transceiver_dev;",
        """static struct transceiver_device transceiver_dev = {
\t.shm_reader.lock = __MUTEX_INITIALIZER(transceiver_dev.shm_reader.lock),
\t.shm_super_reader.lock = __MUTEX_INITIALIZER(transceiver_dev.shm_super_reader.lock),
};""")
    text = replace_once(text, "DECLARE_COMPLETION(transceiver_done);",
                        "static DECLARE_WAIT_QUEUE_HEAD(transceiver_wait);\n"
                        "static atomic_t transceiver_pending = ATOMIC_INIT(0);")
    if text.count("\tcomplete(&transceiver_done);") != 2:
        raise ValueError("unexpected sensor sample notifications")
    text = text.replace("\tcomplete(&transceiver_done);",
                        "\tatomic_set(&transceiver_pending, 1);\n"
                        "\twake_up_interruptible(&transceiver_wait);")
    text = replace_once(text, "\t\tret = wait_for_completion_interruptible(&transceiver_done);",
        "\t\tret = wait_event_interruptible(transceiver_wait,\n"
        "\t\t\tkthread_should_stop() || atomic_read(&transceiver_pending));\n"
        "\t\tif (kthread_should_stop())\n\t\t\tbreak;\n"
        "\t\tif (!ret)\n\t\t\tatomic_xchg(&transceiver_pending, 0);")
    text = replace_once(text,
        "\tstruct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };\n", "")
    text = replace_once(text, "\tsched_setscheduler(dev->task, SCHED_FIFO, &param);\n", "")
    return text


def stage(reference, dest, with_ipi=False, with_ready=False, with_list=False, with_shm=False, with_config=False, with_samples=False):
    with_config = with_config or with_samples
    with_shm = with_shm or with_config
    with_list = with_list or with_shm
    with_ready = with_ready or with_list
    with_ipi = with_ipi or with_ready
    git = ["git", "-C", str(reference)]
    files = {}
    optional = (("ipi_comm.c",) if with_ipi else ()) + (("ready.c",) if with_ready else ())
    if with_list:
        optional += ("sensor_list.c", "sensor_list.h", "share_memory.h")
    if with_shm:
        optional += ("share_memory.c",)
    if with_samples:
        optional += ("transceiver.c", "timesync.c", "timesync.h")
    for name in FILES + optional:
        path = f"{BASE}/{name}"
        row = subprocess.check_output(git + ["ls-tree", REV, path], text=True)
        if not row.startswith(("100644 blob ", "100755 blob ")):
            raise ValueError(f"not a pinned regular file: {path}")
        files[name] = subprocess.check_output(git + ["show", f"{REV}:{path}"])
    files["sensor_comm.c"] = lock_sensor_notify_lifetime(harden(files["sensor_comm.c"].decode())).encode()
    files["sensor_comm.h"] = replace_once(files["sensor_comm.h"].decode(),
        "void sensor_comm_notify_handler_register(uint8_t cmd,",
        "/* Process context only. One owner per command. Unregister drains readers. */\n"
        "void sensor_comm_notify_handler_register(uint8_t cmd,").encode()
    files["Kbuild"] = (b"obj-m := t70_sensorhub_wire.o\n"
                       b"t70_sensorhub_wire-y := sensor_comm.o tiny_crc8.o wire_layout.o\n")
    if with_ipi:
        transport = drain_sensor_ipi_callback(harden_ipi(files["ipi_comm.c"].decode()))
        files["ipi_comm.c"] = drain_sensor_ipi_queue(transport).encode()
        for target, path in SCP_HEADERS.items():
            files["pinned/" + target] = subprocess.check_output(
                git + ["show", f"{REV}:mindone/modules/include/{path}"])
        files["Kbuild"] += (b"ccflags-y += -I$(src)/pinned\n"
                            b"t70_sensorhub_wire-y += ipi_comm.o\n")
    if with_ready:
        files["ready.c"] = stage_ready(files["ready.c"].decode()).encode()
        path = "mindone/modules/include/drivers/misc/mediatek/sensor/2.0/core/hf_sensor_type.h"
        files["pinned/hf_sensor_type.h"] = subprocess.check_output(
            git + ["show", f"{REV}:{path}"])
        files["Kbuild"] += b"t70_sensorhub_wire-y += ready.o\n"
    if with_list:
        inventory = stage_sensor_list(files["sensor_list.c"].decode())
        inventory = replace_once(inventory, "static struct share_mem sensor_list_shm_reader;",
            "static struct share_mem sensor_list_shm_reader = {\n"
            "\t.lock = __MUTEX_INITIALIZER(sensor_list_shm_reader.lock),\n};")
        files["sensor_list.c"] = inventory.encode()
        for header in ("hf_manager.h", "hf_sensor_io.h"):
            path = "mindone/modules/include/drivers/misc/mediatek/sensor/2.0/core/" + header
            files["pinned/" + header] = subprocess.check_output(git + ["show", f"{REV}:{path}"])
        files["Kbuild"] += b"t70_sensorhub_wire-y += sensor_list.o\n"
    if with_shm:
        files["share_memory.c"] = stage_share_memory(files["share_memory.c"].decode()).encode()
        header = files["share_memory.h"].decode()
        header = replace_once(header, "\tstruct mutex lock;", "\tstruct mutex lock;\n\tu32 host_capacity;")
        files["share_memory.h"] = header.encode()
        if with_config:
            files["share_memory.c"] = serialize_sensor_shm_config(
                stage_config_handoff(files["share_memory.c"].decode())).encode()
        files["Kbuild"] += b"t70_sensorhub_wire-y += share_memory.o\n"
    if with_samples:
        transceiver = stage_transceiver(files["transceiver.c"].decode())
        transceiver = stage_sample_backpressure(transceiver)
        transceiver = stage_sample_lifetime(stage_sample_gate(transceiver))
        transceiver = drop_sensor_irq_latency_debug(transceiver)
        transceiver = unwind_sensor_transceiver_init(transceiver)
        files["transceiver.c"] = drop_sensor_vendor_diagnostics(transceiver).encode()
        timer = files["timesync.c"].decode()
        files["timesync.c"] = stage_counter_conversion(stage_timesync(timer)).encode()
        header = files["timesync.h"].decode()
        decl = "int64_t timesync_filter_get(struct timesync_filter *filter);"
        files["timesync.h"] = replace_once(header, decl, decl +
                                          "\nvoid timesync_filter_reset(struct timesync_filter *filter);").encode()
        files["Kbuild"] += b"t70_sensorhub_wire-y += transceiver.o timesync.o\n"
    files["wire_layout.c"] = b"""// SPDX-License-Identifier: GPL-2.0-only
#include <linux/build_bug.h>
#include <linux/string.h>
#include "sensor_comm.h"

static_assert(sizeof(struct sensor_comm_ctrl) == 8);
static_assert(offsetof(struct sensor_comm_ctrl, data) == 8);
static_assert(sizeof(struct sensor_comm_ack) == 8);
static_assert(sizeof(struct sensor_comm_notify) == 28);
static_assert(offsetof(struct sensor_comm_notify, crc8) == 4);
static_assert(offsetof(struct sensor_comm_notify, value) == 8);
"""
    dest.mkdir()
    for name, payload in files.items():
        output = dest / name
        output.parent.mkdir(exist_ok=True, parents=True)
        output.write_bytes(payload)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference", type=Path)
    p.add_argument("destination", type=Path)
    p.add_argument("--with-ipi", action="store_true")
    p.add_argument("--with-ready", action="store_true")
    p.add_argument("--with-list", action="store_true")
    p.add_argument("--with-shm", action="store_true")
    p.add_argument("--with-config", action="store_true")
    p.add_argument("--with-samples", action="store_true")
    a = p.parse_args()
    try:
        stage(a.reference, a.destination, a.with_ipi, a.with_ready, a.with_list, a.with_shm, a.with_config, a.with_samples)
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as e:
        p.exit(2, f"sensorhub staging refused: {e}\n")
