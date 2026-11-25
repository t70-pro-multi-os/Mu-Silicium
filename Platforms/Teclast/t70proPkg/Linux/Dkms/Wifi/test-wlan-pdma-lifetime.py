#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check staged WLAN PDMA teardown ordering and phys_addr_t log arguments."""
from pathlib import Path
import re, sys
p = Path(sys.argv[1])
hal=(p/"hal_pdma.c").read_text()
final = re.search(r"(?ms)^void halHifSwInfoUnInit\(.*?^\}", hal)[0]
assert final.index("timer_shutdown_sync(") < final.index("halWpdmaFreeRing(")
assert hal.count("timer_delete_sync(&prHifInfo->rSerTimer)") == 1
kal=(p/"kal_pdma.c").read_text()
for function in ("wf_ioremap_read", "wf_ioremap_write"):
    body=re.search(r"(?ms)^int "+function+r"\(.*?^\}", kal)[0]
    assert "phys_addr_t addr" in body and "__func__, &addr" in body
    assert body.count("%pa") == 2
print("WLAN stage: final timer shutdown before freeing rings; recovery remains rearmable; address arguments match")
