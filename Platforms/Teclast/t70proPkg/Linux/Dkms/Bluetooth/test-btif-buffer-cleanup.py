#!/usr/bin/env python3
"""Verify staged BTIF singleton buffer ownership on probe failure and removal."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
cleanup, rest = source.split("\nstatic void btif_free_buffers(struct _mtk_btif_ *port)\n", 1)[1].split(
    "\nstatic int btif_device_init(void)\n", 1)
init, stop = rest.split("\nstatic void btif_device_stop(void)\n", 1)
stop = stop.split("\nstatic int BTIF_init(void)\n", 1)[0]
unwind = init.split("\nerr_exit2:", 1)[1]
assert init.count("return -ENOMEM;") == 3
assert init.index("BUILD_BUG_ON(BTIF_PORT_NR != 1)") < init.index("kmalloc(")
assert "platform_driver_register(" not in init
assert unwind.index("btif_free_vfifos(") < unwind.index("btif_free_buffers(")
assert stop.index("_btif_tx_ctx_deinit(") < stop.index("btif_free_vfifos(")
assert stop.index("_btif_rx_btm_deinit(") < stop.index("btif_free_buffers(")

for applied in ("rx_log.p_queue", "tx_log.p_queue", "btif_buf.p_buf"):
    pointer = "port->" + applied
    assert cleanup.count(f"kfree({pointer});") == 1
    assert cleanup.index(f"kfree({pointer});") < cleanup.index(f"{pointer} = NULL;")

print("BTIF singleton: partial probe and registered-device teardown free the owned buffers")
