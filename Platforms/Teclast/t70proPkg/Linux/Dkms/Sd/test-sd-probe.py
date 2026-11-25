#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the exact staged external-SD checks with a fake MMC core."""
import argparse
from pathlib import Path
import subprocess
import tempfile

HEADER = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#define EPROBE_DEFER 517
#define MMC_CAP_4_BIT_DATA 4
#define MMC_CAP_8_BIT_DATA 8
#define IS_ERR_OR_NULL(x) (!(x) || (x) < 0)
struct mmc { int caps; struct { int vmmc, vqmmc; } supply; };
struct pdev { int dev; };
static int parse_error, supply_error, supply_reads;
static bool gpio;
static int mmc_of_parse(struct mmc *m) { (void)m; return parse_error; }
static bool mmc_host_can_gpio_cd(struct mmc *m) { (void)m; return gpio; }
static int mmc_regulator_get_supply(struct mmc *m)
{ (void)m; supply_reads++; return supply_error; }
#define dev_err_probe(dev, rc, ...) ((void)(dev), rc)
static int probe_checks(struct pdev *pdev, struct mmc *mmc)
{
    int ret;
"""

TEST = r"""
    return 0;
}
int main(void)
{
    struct pdev p={0};
    struct mmc m={4, {1, 1}};
    parse_error=-EPROBE_DEFER;
    assert(probe_checks(&p, &m)==-EPROBE_DEFER && supply_reads==0);
    parse_error=0;
    assert(probe_checks(&p, &m)==-ENODEV && supply_reads==0);
    gpio=true;
    m.caps=0;
    assert(probe_checks(&p, &m)==-EINVAL && supply_reads==0);
    m.caps=4|8;
    assert(probe_checks(&p, &m)==-EINVAL && supply_reads==0);
    m.caps=4; supply_error=-EPROBE_DEFER;
    assert(probe_checks(&p, &m)==-EPROBE_DEFER && supply_reads==1);
    supply_error=0; m.supply.vmmc=-ENODEV;
    assert(probe_checks(&p, &m)==-ENODEV);
    m.supply.vmmc=1; m.supply.vqmmc=0;
    assert(probe_checks(&p, &m)==-ENODEV);
    m.supply.vqmmc=1;
    assert(probe_checks(&p, &m)==0);
    return 0;
}
"""


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("staged_sd_source", type=Path)
    args = parser.parse_args()
    text = args.staged_sd_source.read_text()
    start = text.index("\tret = mmc_of_parse(mmc);")
    stop = text.index("\tres = platform_get_resource(pdev, IORESOURCE_MEM, 0);", start)
    with tempfile.TemporaryDirectory(prefix="t70-sd-probe-test-") as name:
        root = Path(name)
        c, exe = root / "check.c", root / "check"
        c.write_text(HEADER + text[start:stop] + TEST)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                        "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("PASS: 8 cases for MMC parse, bus width, card-detect, and rails before MMIO")
