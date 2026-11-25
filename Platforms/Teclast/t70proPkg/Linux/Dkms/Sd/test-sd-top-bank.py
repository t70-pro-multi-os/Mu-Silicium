#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile selected staged SD callbacks against two memory-backed banks."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HEAD = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
#define __maybe_unused
#define BIT(n) (1U << (n))
#define GENMASK(h,l) (((u32)~0U << (l)) & ((u32)~0U >> (31U - (h))))
"""

MOCK = r"""
struct mtk_mmc_compatible { u32 pad_tune_reg; };
struct msdc_host {
    unsigned char *base, *top_base;
    const struct mtk_mmc_compatible *dev_comp;
    struct msdc_save_para save_para;
    void *pins_eint, *pinctrl, *pins_uhs;
    int irq;
};
struct mmc_host { struct msdc_host *priv; };
struct device { struct mmc_host *mmc; };
static struct mmc_host mmc;
static unsigned char primary[4096], top[4096];
static bool clocks;
static int clk_error;
static unsigned reads, writes;
static u32 readl(unsigned char *p)
{
    u32 v;
    assert(clocks);
    reads++;
    memcpy(&v, p, sizeof(v));
    return v;
}
static void writel(u32 val, unsigned char *p)
{
    assert(clocks);
    writes++;
    memcpy(p, &val, sizeof(val));
}
static void sdr_set_field(unsigned char *p, u32 mask, u32 value)
{
    u32 v=readl(p);
    writel((v & ~mask) | ((value << __builtin_ctz(mask)) & mask), p);
}
static bool sdio_irq_claimed(struct mmc_host *m) { (void)m; return false; }
static void __msdc_enable_sdio_irq(struct msdc_host *h, int n) { (void)h; (void)n; assert(0); }
static void disable_irq(int n) { (void)n; assert(0); }
static void enable_irq(int n) { (void)n; assert(0); }
static void pinctrl_select_state(void *a, void *b) { (void)a; (void)b; assert(0); }
static struct mmc_host *mmc_from_priv(struct msdc_host *h) { assert(mmc.priv==h); return &mmc; }
static struct mmc_host *dev_get_drvdata(struct device *d) { return d->mmc; }
static struct msdc_host *mmc_priv(struct mmc_host *m) { return m->priv; }
static void msdc_gate_clock(struct msdc_host *h) { (void)h; clocks=false; }
static int msdc_ungate_clock(struct msdc_host *h)
{
    (void)h;
    if (clk_error) return clk_error;
    clocks=true; return 0;
}
"""

TAIL = r"""
static void verify_delay(struct msdc_host *h, u32 value, u32 first, u32 second)
{
    u32 cmd, dat, primary_before=readl(primary + MSDC_PAD_TUNE0);
    msdc_set_cmd_delay(h, value);
    msdc_set_data_delay(h, value);
    cmd=readl(top + EMMC_TOP_CMD); dat=readl(top + EMMC_TOP_CONTROL);
    assert((cmd & PAD_CMD_RXDLY) >> __builtin_ctz(PAD_CMD_RXDLY) == first);
    assert((cmd & PAD_CMD_RXDLY2) >> __builtin_ctz(PAD_CMD_RXDLY2) == second);
    assert((dat & PAD_DAT_RD_RXDLY) >> __builtin_ctz(PAD_DAT_RD_RXDLY) == first);
    assert((dat & PAD_DAT_RD_RXDLY2) >> __builtin_ctz(PAD_DAT_RD_RXDLY2) == second);
    assert((cmd & BIT(31)) && (dat & BIT(31)));
    assert(readl(primary + MSDC_PAD_TUNE0)==primary_before);
}
int main(void)
{
    struct mtk_mmc_compatible compat={MSDC_PAD_TUNE0};
    struct msdc_host h={.base=primary,.top_base=top,.dev_comp=&compat};
    struct device d={&mmc};
    mmc.priv=&h; clocks=true;
    writel(BIT(31),top + EMMC_TOP_CMD);
    writel(BIT(31),top + EMMC_TOP_CONTROL);
    writel(0xd5,top + EMMC50_PAD_DS_TUNE);
    writel(0x8272,primary + MSDC_PAD_TUNE0);
    writel(0x42,primary + MSDC_CFG);

    verify_delay(&h,0,0,0);
    verify_delay(&h,31,31,0);
    verify_delay(&h,32,31,0);
    verify_delay(&h,63,31,31);
    verify_delay(&h,5,5,0); /* old second delay must be cleared */
    u32 cmd=readl(top + EMMC_TOP_CMD), dat=readl(top + EMMC_TOP_CONTROL);
    assert(msdc_runtime_suspend(&d)==0 && !clocks);
    memset(top,0,sizeof(top)); memset(primary,0,sizeof(primary));
    unsigned nr=reads,nw=writes;
    clk_error=-EIO;
    assert(msdc_runtime_resume(&d)==-EIO && !clocks && reads==nr && writes==nw);
    clk_error=0;
    assert(msdc_runtime_resume(&d)==0 && clocks);
    assert(readl(top + EMMC_TOP_CMD)==cmd && readl(top + EMMC_TOP_CONTROL)==dat);
    assert(readl(top + EMMC50_PAD_DS_TUNE)==0xd5);
    assert(readl(primary + MSDC_CFG)==0x42);
    assert(readl(primary + MSDC_PAD_TUNE0)==0); /* do not restore the wrong bank */
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("staged_sd_source", type=Path)
    args = parser.parse_args()
    s = args.staged_sd_source.read_text()
    def find(pattern):
        m = re.search(pattern, s, re.M | re.S)
        if not m:
            raise ValueError(f"staged SD structure no longer has {pattern}")
        return m[0]

    callbacks = "\n".join(find(r"^static [^\n]+ " + fn + r"\([^\n]+\n\{.*?^\}")
                          for fn in ("msdc_set_cmd_delay", "msdc_set_data_delay",
                                     "msdc_save_reg", "msdc_restore_reg",
                                     "msdc_runtime_suspend", "msdc_runtime_resume"))
    macros = set(re.findall(r"\b[A-Z][A-Z_0-9]{2,}\b", callbacks + TAIL))
    defines = "\n".join(line for line in s.splitlines()
                        if line.startswith("#define ") and line.split()[1] in macros)
    with tempfile.TemporaryDirectory(prefix="t70-sd-top-bank-") as name:
        root = Path(name)
        c, exe = root / "test.c", root / "test"
        c.write_text(HEAD + defines + "\n" + find(r"^struct msdc_save_para \{.*?^\};")
                     + MOCK + callbacks + TAIL)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                        "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("PASS: staged command/data delay uses top bank; resume restores it only after clocks")


if __name__ == "__main__":
    main()
