#!/usr/bin/env python3
"""Verify the external SD remove path drains its watchdog and IRQ before teardown."""
from pathlib import Path
import sys

src = Path(sys.argv[1]).read_text()
remove = src.split("\nstatic void msdc_drv_remove(", 1)[1].split("\n}\n", 1)[0]
markers = ("mmc_remove_host(mmc);", "cancel_delayed_work_sync(&host->req_timeout);",
           "devm_free_irq(&pdev->dev, host->irq, host);",
           "platform_set_drvdata(pdev, NULL);", "msdc_deinit_hw(host);",
           "msdc_gate_clock(host);", "dma_free_coherent(")
order = [remove.index(x) for x in markers]
assert order == sorted(order)
assert remove.count("cancel_delayed_work_sync(") == 1
assert remove.count("devm_free_irq(") == 1

probe = src.split("\nstatic int msdc_drv_probe(", 1)[1].split("\n}\n", 1)[0]
end = probe.index("\nend:")
register_failure = probe[end:probe.index("\nrelease:", end)]
assert "if (ret)\n\t\tgoto end;" in probe[probe.index("ret = mmc_add_host(mmc);"):end]
assert register_failure.index("devm_free_irq(&pdev->dev, host->irq, host);") < register_failure.index("pm_runtime_disable(host->dev);")
assert "devm_free_irq(" not in probe[probe.index("\nrelease:", end):]
print("SD teardown: IRQ released before deinit on remove and MMC host registration failure")
