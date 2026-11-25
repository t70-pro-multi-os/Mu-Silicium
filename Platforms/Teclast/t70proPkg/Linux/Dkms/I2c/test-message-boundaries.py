#!/usr/bin/env python3
"""Exercise the actual staged I2C adapter dispatch, with no controller I/O."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
assert "I2C_MASTER_CONTINUOUS_WR" not in source
transfer = re.search(r"^static int mtk_i2c_transfer\([^;{}]*\n\{.*?^\}", source, re.M | re.S)
assert transfer
stubs = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#define I2C_MAX_FAST_MODE_PLUS_FREQ 1000000
#define I2C_M_RD 1
#define I2C_M_DMA_SAFE 0x200
enum { I2C_MASTER_WR, I2C_MASTER_RD, I2C_MASTER_WRRD };
struct i2c_msg { uint16_t addr, flags, len; void *buf; };
struct compat { bool auto_restart; };
struct mtk_i2c {
    int op, speed_hz;
    bool auto_restart, ignore_restart_irq;
    struct compat *dev_comp;
    void *dev;
};
struct i2c_adapter { struct mtk_i2c *data; };
static struct i2c_msg *expected, original[3];
static int calls, fail_at, clocks, total, combined;
#define i2c_get_adapdata(a) ((a)->data)
#define dev_dbg(...) do {} while (0)
#define mtk_i2c_clock_enable(i2c) (++clocks, 0)
#define mtk_i2c_clock_disable(i2c) (--clocks)
static int mtk_i2c_do_transfer(struct mtk_i2c *i2c, struct i2c_msg *msg,
                               int num, int left)
{
    assert(num == total);
    assert(msg == &expected[calls]);
    assert(msg->addr == original[calls].addr && msg->flags == original[calls].flags);
    assert(msg->len == original[calls].len && msg->buf == original[calls].buf);
    assert(left == (combined ? 0 : total - calls - 1));
    assert(i2c->auto_restart == !combined);
    assert(i2c->op == (combined ? I2C_MASTER_WRRD :
           ((msg->flags & I2C_M_RD) ? I2C_MASTER_RD : I2C_MASTER_WR)));
    return ++calls == fail_at ? -EIO : 0;
}
'''
cases = r'''
int main(void)
{
    static unsigned char buffers[3][32768];
    struct compat compat = { true };
    struct mtk_i2c i2c = { .speed_hz = 400000, .dev_comp = &compat };
    struct i2c_adapter adapter = { &i2c };
    struct i2c_msg msgs[] = {
        { 0x34, I2C_M_DMA_SAFE, 32768, buffers[0] },
        { 0x34, 0, 32768, buffers[1] },
        { 0x35, I2C_M_RD, 8, buffers[2] },
    };
    expected = msgs;
    original[0] = msgs[0];
    original[1] = msgs[1];
    original[2] = msgs[2];
    total = 2;  /* Equal sizes: old aggregate narrowed 65536 to zero. */
    assert(mtk_i2c_transfer(&adapter, msgs, total) == 2 && calls == 2 && !clocks);
    calls = 0;
    msgs[0].len = msgs[1].len = original[0].len = original[1].len = 4;
    assert(mtk_i2c_transfer(&adapter, msgs, total) == 2 && calls == 2 && !clocks);
    calls = 0;
    total = 3;
    assert(mtk_i2c_transfer(&adapter, msgs, total) == 3 && calls == 3 && !clocks);
    calls = 0;
    fail_at = 2;
    assert(mtk_i2c_transfer(&adapter, msgs, total) == -EIO && calls == 2 && !clocks);
    calls = fail_at = 0;
    total = 2;
    msgs[1].flags = I2C_M_RD;
    combined = 1;
    assert(mtk_i2c_transfer(&adapter, msgs, total) == 2 && calls == 1 && !clocks);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-i2c-boundaries-") as tmp:
    unit = Path(tmp) / "dispatch.c"
    unit.write_text(stubs + transfer[0] + cases)
    exe = unit.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(unit), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("I2C: original write messages, mixed read, error stop, and WRRD dispatch passed")
