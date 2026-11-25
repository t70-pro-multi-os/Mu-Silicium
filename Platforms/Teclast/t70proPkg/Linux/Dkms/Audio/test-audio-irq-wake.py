#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test the staged AFE IRQ wake reference; no kernel IRQs are touched."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
fns = [re.search(r"(?ms)^static (?:int|void) " + n + r"\([^{}]*\n\{.*?^\}", src)[0]
       for n in ("t70_afe_release_irq_wake", "t70_afe_acquire_irq_wake")]
probe = src.split("static int mt6789_afe_pcm_dev_probe(", 1)[1].split("\n}\n", 1)[0]
assert probe.index("devm_request_irq(") < probe.index("t70_afe_acquire_irq_wake(dev, irq_id)")
assert re.search(r"t70_afe_acquire_irq_wake\(dev, irq_id\);\n\tif \(ret\)\n\t\tgoto err_pm_disable;", probe)
shim = r'''
#include <assert.h>
#include <errno.h>
struct device { int token; };
static int fail_enable, fail_action, held, logs, released;
static void (*saved)(void *);
static void *datum;
#define dev_err(dev, ...) do { assert(dev); logs++; } while (0)
static int enable_irq_wake(unsigned int irq)
{
    assert(irq == 23 && !held);
    if (fail_enable)
        return -EINVAL;
    held++;
    return 0;
}
static int disable_irq_wake(unsigned int irq)
{
    assert(irq == 23 && held == 1);
    held--;
    released++;
    return 0;
}
static int devm_add_action_or_reset(struct device *dev, void (*cb)(void *), void *arg)
{
    assert(dev && !saved && held);
    if (fail_action) {
        cb(arg);
        return -ENOMEM;
    }
    saved = cb;
    datum = arg;
    return 0;
}
'''
cases = r'''
int main(void)
{
    struct device dev;
    fail_enable = 1;
    assert(!t70_afe_acquire_irq_wake(&dev, 23));
    assert(logs == 1 && !held && !saved && !released);
    fail_enable = 0;
    fail_action = 1;
    assert(t70_afe_acquire_irq_wake(&dev, 23) == -ENOMEM);
    assert(!held && !saved && released == 1);
    fail_action = 0;
    assert(!t70_afe_acquire_irq_wake(&dev, 23));
    assert(held == 1 && saved && released == 1);
    saved(datum);
    assert(!held && released == 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-afe-irq-wake-") as tmp:
    c = Path(tmp) / "irq-wake.c"
    c.write_text(shim + "\n".join(fns) + cases)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("AFE IRQ wake: optional failure, action allocation unwind, and detach passed")
