#!/usr/bin/env python3
"""Audit generated BTIF coherent RX buffer ownership and IRQ detach order."""
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
core = (path / "mtk_btif.c").read_text()
dma = (path / "btif_dma_plat.c").read_text()
def body(src, function):
    match = re.search(r"^" + re.escape(function) + r"\([^;{}]*\n\{(.*?)^\}", src, re.M | re.S)
    assert match, function
    return match[1]

assert "dma_alloc_coherent(" in body(core, "static int _btif_vfifo_init")
assert "dma_free_coherent(" in body(core, "static int _btif_vfifo_deinit")
rx = body(core, "unsigned int btif_dma_rx_data_receiver")
assert "dma_unmap_single(" not in rx and "btif_bbs_write(" in rx
rx = dma.split("if (rx_cb != NULL) {\n\t\t\t/* The DMA producer", 1)[1].split("\n\t\tmb();", 1)[0]
assert rx.index("dma_rmb();") < rx.index("(*rx_cb)")
for name, detach in (("int _btif_rx_dma_free", "hal_btif_dma_rx_cb_reg("),
                     ("int _btif_controller_free", "hal_btif_rx_cb_reg(")):
    b = body(core, name)
    assert b.index("_btif_irq_free(") < b.index(detach)

print("BTIF coherent DMA pair, RX payload barrier and post-IRQ callback detach verified")
