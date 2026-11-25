#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the staged AFE IRQ handler against an in-memory register mock."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
fn = re.search(r"(?ms)^static irqreturn_t mt6789_afe_irq_handler\([^{}]*\n\{.*?^\}", src)[0]
shim = r'''
#include <assert.h>
#include <errno.h>
typedef int irqreturn_t;
enum { IRQ_NONE, IRQ_HANDLED };
enum { AFE_IRQ_MCU_EN, AFE_IRQ_MCU_STATUS, AFE_IRQ_MCU_CLR };
#define AFE_IRQ_STATUS_BITS 0xfU
#define MT6789_MEMIF_NUM 2
#define dev_warn(...) do {} while (0)
struct timespec64 { int unused; };
struct irq_data { int irq_en_shift; };
struct mtk_base_afe_irq { struct irq_data *irq_data; };
struct mtk_base_afe_memif { void *substream; int irq_usage; };
struct mtk_base_afe {
    void *regmap;
    struct mtk_base_afe_memif *memif;
    struct mtk_base_afe_irq *irqs;
    int irqs_size;
};
static int reads, writes, fail_reg = -1, elapsed;
static unsigned int masks[2], ack;
static void ktime_get_ts64(struct timespec64 *ts) { ts->unused = 0; }
static unsigned long long timespec64_to_ns(const struct timespec64 *ts) { return ts->unused; }
static void snd_pcm_period_elapsed(void *substream) { assert(substream); elapsed++; }
static int regmap_read(void *map, int reg, unsigned int *value)
{
    assert(map && reg < 2);
    reads++;
    *value = masks[reg];
    return reg == fail_reg ? -EIO : 0;
}
static int regmap_write(void *map, int reg, unsigned int value)
{
    assert(map && reg == AFE_IRQ_MCU_CLR);
    writes++;
    ack = value;
    return 0;
}
'''
cases = r'''
int main(void)
{
    struct irq_data info = { 1 };
    struct mtk_base_afe_irq irqs[] = { { &info } };
    struct mtk_base_afe_memif memifs[] = { { &info, 1 }, { &info, 0 } };
    struct mtk_base_afe afe = { &info, memifs, irqs, 1 };
    masks[0] = 0x13; masks[1] = 0x16;
    fail_reg = AFE_IRQ_MCU_EN;
    assert(mt6789_afe_irq_handler(9, &afe) == IRQ_NONE && reads == 1 && !writes && !elapsed);
    fail_reg = AFE_IRQ_MCU_STATUS;
    assert(mt6789_afe_irq_handler(9, &afe) == IRQ_NONE && reads == 3 && !writes && !elapsed);
    fail_reg = -1; masks[1] = 0x14;
    assert(mt6789_afe_irq_handler(9, &afe) == IRQ_NONE && reads == 5 && !writes && !elapsed);
    masks[1] = 0x16;
    assert(mt6789_afe_irq_handler(9, &afe) == IRQ_HANDLED && reads == 7);
    assert(writes == 1 && ack == 2 && elapsed == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-afe-irq-status-") as tmp:
    c = Path(tmp) / "irq-status.c"
    c.write_text(shim + fn + cases)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-fsanitize=undefined", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("AFE IRQ: rejected read errors and unrelated status; dispatched and acknowledged only valid owned status")
