#!/usr/bin/env python3
"""Verify generated sensorhub init unwinds only resources actually acquired."""
from pathlib import Path
import sys

folder = Path(sys.argv[1])
src = (folder / "transceiver.c").read_text()
ready = (folder / "ready.c").read_text().split("int host_ready_init(void)", 1)[1].split("\n}\n", 1)[0]
assert ready.count("return -") == 1
assert ready.index("return -ENOMEM;") < ready.index("sensor_comm_notify_handler_register(")
init = src.split("static int __init transceiver_init(", 1)[1].split(
    "\nstatic void __exit transceiver_exit(", 1)[0]
failure = init.split("\nout_ready:\n", 1)[1]
assert "host_ready_exit(" not in failure
assert failure.index("sensor_comm_notify_handler_unregister(SENS_COMM_NOTIFY_DATA_CMD);") < (
    failure.index("kthread_stop(dev->task);"))
assert failure.index("kthread_stop(dev->task);") < failure.index("timesync_filter_exit(")
assert failure.index("dev->task = NULL;") < failure.index("out_pm_notify:")
assert "create thread fail %d\\n\", ret);\n\t\tgoto out_pm_notify;" in init
assert "register hf device fail %d\\n\", ret);\n\t\tgoto out_wakeup;" in init
assert failure.index("out_device:") < failure.index("out_wakeup:") < failure.index("wakeup_source_unregister")
exit_body = src.split("static void __exit transceiver_exit(", 1)[1]
assert exit_body.index("sensor_ready_notifier_chain_unregister(") < exit_body.index("host_ready_exit();")
assert exit_body.index("host_ready_exit();") < exit_body.index("transceiver_stop_boot();")
assert exit_body.index("transceiver_stop_boot();") < exit_body.index("cancel_delayed_work_sync(")
assert exit_body.index("cancel_delayed_work_sync(") < exit_body.index("share_mem_config_handler_unregister(")
print("Sensor init: failed ready does not destroy a missing queue and stops the live worker")
