#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the staged I2C supplier check using an in-memory clock mock."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
probe = src.split("static int mtk_i2c_probe(", 1)[1].split("\n}\n", 1)[0]
assert probe.count("ret = dma_set_mask(&pdev->dev,") == 1
assert probe.index('devm_clk_get_optional') < probe.index('mtk_i2c_init_hw')
check = re.search(r'(?ms)^\ti2c->clk_arb = devm_clk_get_optional.*?(?=^\s*clk = i2c->clk_main;)', probe)[0]
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#define EPROBE_DEFER 517
struct clk { int unused; };
struct device { int unused; };
struct platform_device { struct device dev; };
struct fake_i2c { struct clk *clk_arb; };
static struct clk clock;
static int clock_err, absent, logs;
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((intptr_t)(p))
static struct clk *devm_clk_get_optional(struct device *dev, const char *name)
{
    assert(dev && !strcmp(name, "arb"));
    return clock_err ? (struct clk *)(intptr_t)clock_err : (absent ? 0 : &clock);
}
static int dev_err_probe(struct device *dev, int err, const char *message)
{
    assert(dev && message && err < 0);
    logs++;
    return err;
}
static int setup(struct platform_device *pdev, struct fake_i2c *i2c)
{
'''
cases = r'''
    return 0;
}
int main(void)
{
    struct platform_device dev;
    struct fake_i2c i;
    assert(!setup(&dev, &i) && i.clk_arb == &clock);
    absent = 1;
    assert(!setup(&dev, &i) && !i.clk_arb);
    clock_err = -EPROBE_DEFER;
    assert(setup(&dev, &i) == -EPROBE_DEFER && logs == 1);
    clock_err = -EINVAL;
    assert(setup(&dev, &i) == -EINVAL && logs == 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-i2c-probe-") as tmp:
    c = Path(tmp) / "check.c"
    c.write_text(shim + check + cases)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("I2C probe: optional absent vs deferred/error provider distinguished; DMA mask setup once")
