#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the actual staged regmap probe, without touching hardware."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
fn = re.search(r"(?ms)^static int t70_afe_probe_regmap\([^{}]*\n\{.*?^\}", src)[0]
probe = src.split("static int mt6789_afe_pcm_dev_probe(", 1)[1].split("\n}\n", 1)[0]
assert re.search(r"ret = t70_afe_probe_regmap\(afe\);\n\tif \(ret\)\n\t\tgoto err_pm_disable;", probe)
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
struct regmap { int token; };
struct device { int token; };
struct mtk_base_afe { struct device *dev; void *base_addr; struct regmap *regmap; };
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((intptr_t)(p))
static const int mt6789_afe_regmap_config;
static struct regmap valid_map;
static struct mtk_base_afe afe;
static int usage, resumes, maps, puts, resume_err, map_err;
static int pm_runtime_resume_and_get(struct device *dev)
{
    assert(dev == afe.dev && !usage);
    resumes++;
    if (resume_err)
        return resume_err;
    usage++;
    return 0;
}
static struct regmap *devm_regmap_init_mmio(struct device *dev, void *base, const int *config)
{
    assert(dev == afe.dev && usage == 1 && base == afe.base_addr);
    assert(config == &mt6789_afe_regmap_config);
    maps++;
    return map_err ? (struct regmap *)(intptr_t)map_err : &valid_map;
}
static int pm_runtime_put_sync(struct device *dev)
{
    assert(dev == afe.dev && usage == 1);
    assert(afe.regmap == (map_err ? 0 : &valid_map));
    puts++;
    return --usage;
}
'''
cases = r'''
int main(void)
{
    struct device dev;
    afe.dev = &dev;
    afe.base_addr = &dev;
    resume_err = -EIO;
    assert(t70_afe_probe_regmap(&afe) == -EIO);
    assert(resumes == 1 && !maps && !puts && !usage && !afe.regmap);
    resume_err = 0;
    map_err = -ENOMEM;
    assert(t70_afe_probe_regmap(&afe) == -ENOMEM);
    assert(resumes == 2 && maps == 1 && puts == 1 && !usage && !afe.regmap);
    map_err = 0;
    assert(t70_afe_probe_regmap(&afe) == 0);
    assert(resumes == 3 && maps == 2 && puts == 2 && !usage && afe.regmap == &valid_map);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-afe-regmap-pm-") as tmp:
    c = Path(tmp) / "regmap-pm.c"
    c.write_text(shim + fn + cases)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("AFE probe: failed runtime resume, failed regmap, and balanced success passed")
