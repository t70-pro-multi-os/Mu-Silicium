#!/usr/bin/env python3
"""Run the exact staged TX/RX setup functions with ordinary mocked failures."""
from pathlib import Path
import re
import subprocess
import sys

src = Path(sys.argv[1]).read_text()
def function(name):
    match = re.search(r"^int " + re.escape(name) + r"\([^;{}]*\n\{.*?^\}", src, re.M | re.S)
    assert match, name
    return match[0]

assert "IRQF_NO_AUTOEN" in function("_btif_irq_reg")
assert "BTIF_STATE_RELEASE" not in function("_btif_init")
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define CLK_OUT_ENABLE 1
#define CLK_OUT_DISABLE 0
#define BTIF_MODE_DMA 1
#define DMA_CTRL_ENABLE 1
typedef void (*dma_rx_buf_write)(void);
struct _MTK_BTIF_IRQ_STR_ { int irq_id; bool reg_flag; };
struct _MTK_DMA_INFO_STR_ { struct _MTK_BTIF_IRQ_STR_ *p_irq; };
struct dma_owner { struct _MTK_DMA_INFO_STR_ *p_dma_info; };
struct _mtk_btif_ { struct dma_owner *p_rx_dma, *p_tx_dma; void *p_btif_info; };
static int fault, clock_live, step, irq_live, engine, attaches, frees;
static void btif_rx_dma_irq_handler(void) {}
static void btif_tx_dma_irq_handler(void) {}
static void btif_dma_rx_data_receiver(void) {}
static int hal_btif_dma_clk_ctrl(struct _MTK_DMA_INFO_STR_ *dma, int en)
{
    assert(dma);
    if (en) { assert(!clock_live && step++ == 0); if (fault == 1) return -1; ++clock_live; }
    else { assert(!irq_live && !engine && clock_live-- == 1); }
    return 0;
}
static int _btif_irq_reg(struct _MTK_BTIF_IRQ_STR_ *irq, void (*handler)(void),
                        struct _mtk_btif_ *b, bool masked)
{
    assert(irq && handler && b && masked && clock_live && step++ == 1);
    if (fault == 2) return -2;
    irq->reg_flag = true; ++irq_live; return 0;
}
static int hal_btif_vfifo_reset(struct _MTK_DMA_INFO_STR_ *dma)
{ assert(dma && irq_live && step++ == 2); return 0; }
static int hal_btif_dma_hw_init(struct _MTK_DMA_INFO_STR_ *dma)
{ assert(dma && irq_live && step++ == 3); return fault == 3 ? -3 : 0; }
static int set_mode(void *b, int mode)
{ assert(b && mode == BTIF_MODE_DMA && step++ == 4); return fault == 4 ? -4 : 0; }
#define hal_btif_rx_mode_ctrl set_mode
#define hal_btif_tx_mode_ctrl set_mode
static void hal_btif_dma_rx_cb_reg(struct _MTK_DMA_INFO_STR_ *dma, dma_rx_buf_write cb)
{ assert(dma && irq_live); attaches += cb ? 1 : -1; }
static int hal_btif_dma_ctrl(struct _MTK_DMA_INFO_STR_ *dma, int op)
{ assert(dma && op == DMA_CTRL_ENABLE && irq_live && step++ == 5);
  if (fault == 5) return -5;
  ++engine; return 0; }
static void _btif_irq_ctrl(struct _MTK_BTIF_IRQ_STR_ *irq, bool en)
{ assert(irq->reg_flag && en && engine && step++ == 6); }
static void free_irq(int irq, void *b)
{ assert(irq == 7 && b && irq_live-- == 1 && !engine && !attaches); ++frees; }
'''
suffix = r'''
int main(void)
{
    for (int rx = 0; rx < 2; ++rx) {
        for (fault = 0; fault <= 5; ++fault) {
            struct _MTK_BTIF_IRQ_STR_ irq = { .irq_id = 7 };
            struct _MTK_DMA_INFO_STR_ dma = { &irq };
            struct dma_owner owner = { &dma };
            struct _mtk_btif_ b = { &owner, &owner, &owner };
            clock_live = step = irq_live = engine = attaches = frees = 0;
            assert((rx ? _btif_rx_dma_setup(&b) : _btif_tx_dma_setup(&b)) == -fault);
            if (fault) {
                assert(!clock_live && !irq_live && !irq.reg_flag && !engine && !attaches);
                assert(frees == (fault >= 3));
                if (fault == 2) assert(step == 2);
            } else {
                assert(clock_live && irq_live && irq.reg_flag && engine && attaches == rx && step == 7);
            }
        }
    }
    return 0;
}
'''
c = Path(sys.argv[2])
c.write_text(prefix + function("_btif_rx_dma_setup") + "\n" + function("_btif_tx_dma_setup") + suffix)
exe = c.with_suffix("")
subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                str(c), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=3)
print("BTIF TX/RX IRQ-before-start: successful setup and five failure points per direction")
