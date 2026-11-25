#!/usr/bin/env python3
"""Check that the staged BTIF PM path propagates DMA stop errors."""
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
dma = (p / "btif_dma_plat.c").read_text()
core = (p / "mtk_btif.c").read_text()

def body(src, name):
    match = re.search(r"^int " + re.escape(name) + r"\([^;{}]*\n\{(.*?)^\}", src, re.M | re.S)
    assert match, name
    return match[1]

for direction in ("tx", "rx"):
    ctrl = body(dma, f"btif_{direction}_dma_ctrl")
    assert 'BTIF_ERR_FUNC("stop dma timeout!\\n");\n\t\t\t\treturn -ETIMEDOUT;' in ctrl
    release = body(core, f"_btif_{direction}_dma_free")
    assert (release.index("ret = hal_btif_dma_ctrl(") < release.index("if (ret)\n\t\treturn ret;")
            < release.index("hal_btif_dma_clk_ctrl("))
    assert "int i_ret = 0;" in body(core, f"_btif_controller_{direction}_free")

suspend = body(core, "_btif_suspend")
assert "i_ret = tx_ret ?: rx_ret;" in suspend
assert suspend.index("i_ret = tx_ret ?: rx_ret;") < suspend.index("if (i_ret != 0)")
assert "return i_ret;" in body(core.replace("static int mtk_btif_suspend", "int mtk_btif_suspend"),
                                "mtk_btif_suspend")
print("BTIF TX/RX stop error is not replaced by success or clock return; suspend consumes status")
