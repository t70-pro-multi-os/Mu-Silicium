#!/usr/bin/env python3
"""Exercise the actual Linux resume function with mock I2C and IRQ."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
resume = re.search(r"^static int t70_hx_resume\([^;{}]*\n\{.*?^\}", source, re.M | re.S)
assert resume
rearm = re.search(r"^static void t70_hx_sync_and_enable_irq\([^;{}]*\n\{.*?^\}", source, re.M | re.S)
assert rearm
probe = source.split("static int t70_hx_probe(", 1)[1].split("\n}\n", 1)[0]
assert re.search(r"IRQF_ONESHOT \| IRQF_NO_AUTOEN,.*\n\tif \(ret\)\n\t\treturn.*\n\tt70_hx_sync_and_enable_irq\(hx\);", probe)
accept = source.split("static bool t70_hx_accept_report(", 1)[1].split("\n}\n", 1)[0]
irq = source.split("static irqreturn_t t70_hx_interrupt(", 1)[1].split("\n}\n", 1)[0]
assert accept.index("return false;") < accept.index("t70_hx_valid(")
assert irq.index("mutex_lock(") < irq.index("t70_hx_accept_report(") < irq.index("mutex_unlock(")
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
enum { HmOk, HmIoError };
struct i2c_client { int irq; };
struct t70_hx { struct i2c_client *client; int io, report_lock; bool irq_enabled; };
struct device { struct t70_hx *hx; };
static int id_error, bad_snapshot, reads, enabled;
static struct t70_hx *dev_get_drvdata(struct device *dev) { return dev->hx; }
static int dev_err_probe(struct device *dev, int err, const char *msg)
{ assert(dev && msg); return err; }
static int HmReadChipId(int *io) { assert(io); return id_error ? HmIoError : HmOk; }
static void mutex_lock(int *m) { assert(!(*m)++); }
static void mutex_unlock(int *m) { assert((*m)-- == 1); }
static bool t70_hx_accept_report(struct t70_hx *hx)
{ assert(!enabled && hx->report_lock == 1); ++reads; return !bad_snapshot; }
static void enable_irq(int irq) { assert(irq == 9 && !enabled++ && reads == 1); }
'''
test = r'''
int main(void)
{
    struct i2c_client client = {.irq=9};
    struct t70_hx hx = {.client=&client};
    struct device dev = {.hx=&hx};

    id_error = 1;
    assert(t70_hx_resume(&dev) == -ENODEV && !reads && !enabled && !hx.irq_enabled);
    id_error = 0;
    for (bad_snapshot = 0; bad_snapshot <= 1; ++bad_snapshot) {
        reads = enabled = hx.irq_enabled = 0;
        assert(!t70_hx_resume(&dev) && hx.irq_enabled && reads == 1 && enabled == 1 && !hx.report_lock);
        assert(!t70_hx_resume(&dev) && reads == 1 && enabled == 1);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-himax-resume-") as tmp:
    c = Path(tmp) / "test.c"
    c.write_text(prefix + rearm[0] + resume[0] + test)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("Himax probe/rearm and resume: ID failure, valid/invalid snapshot, IRQ rearm and repeated resume passed")
