#!/usr/bin/env python3
"""Check pinned SD software watchdog callback independently of any card."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
def function(name):
    m = re.search(r"^static void " + name + r"\([^;{}]*\n\{.*?^\}", source, re.M | re.S)
    assert m, name
    return m[0]

for name in ("msdc_start_command", "msdc_start_data"):
    code = function(name)
    ordered = [code.index(x) for x in ("spin_lock_irqsave", "host->req_deadline =",
                                       "mod_delayed_work", "spin_unlock_irqrestore")]
    assert ordered == sorted(ordered)

prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <limits.h>
#define MSDC_INT_CMDTMO 1
#define MSDC_INT_DATTMO 2
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define container_of(p, t, fld) ((t *)((char *)(p) - offsetof(t, fld)))
#define spin_lock_irqsave(l, f) do { assert(irq_off && !locked++); (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); assert(locked-- == 1); } while (0)
struct work_struct { int unused; };
struct mmc_request { int unused; };
struct mmc_command { int unused; };
struct mmc_data { int unused; };
struct msdc_host {
    struct { struct work_struct work; } req_timeout;
    int irq, lock;
    struct mmc_request *mrq;
    struct mmc_command *cmd;
    struct mmc_data *data;
    unsigned long req_deadline;
};
static int irq_off, locked, cmd_done, data_done, advance;
static unsigned long jiffies;
static struct msdc_host *host_current;
static void disable_irq(int n)
{
    assert(n == 7 && !irq_off++);
    if (advance) host_current->req_deadline = jiffies + 100;
}
static void enable_irq(int n) { assert(n == 7 && irq_off-- == 1); }
static void msdc_cmd_done(struct msdc_host *h, int mask, struct mmc_request *m, struct mmc_command *cmd)
{ assert(irq_off && !locked && m == h->mrq && cmd == h->cmd && mask == 1); ++cmd_done; }
static void msdc_data_xfer_done(struct msdc_host *h, int mask, struct mmc_request *m, struct mmc_data *data)
{ assert(irq_off && !locked && m == h->mrq && data == h->data && mask == 2); ++data_done; }
'''
test = r'''
int main(void)
{
    struct mmc_request mrq = {0};
    struct mmc_command cmd = {0};
    struct mmc_data data = {0};
    struct msdc_host h = {.irq=7,.req_deadline=1,.mrq=&mrq,.cmd=&cmd};
    host_current = &h;
    jiffies = ULONG_MAX;
    msdc_request_timeout(&h.req_timeout.work);
    assert(!cmd_done && !data_done && !irq_off);
    jiffies = 2; advance = 1;
    msdc_request_timeout(&h.req_timeout.work);
    assert(!cmd_done && !data_done && !irq_off);
    jiffies = h.req_deadline; advance = 0;
    msdc_request_timeout(&h.req_timeout.work);
    assert(cmd_done == 1 && !data_done && !irq_off);
    h.cmd = NULL; h.data = &data;
    msdc_request_timeout(&h.req_timeout.work);
    assert(cmd_done == 1 && data_done == 1 && !irq_off);
    h.mrq = NULL;
    msdc_request_timeout(&h.req_timeout.work);
    assert(cmd_done == 1 && data_done == 1 && !irq_off);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-sd-watchdog-") as tmp:
    c = Path(tmp) / "test.c"
    c.write_text(prefix + function("msdc_request_timeout") + test)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("SD watchdog: stage advancement, wrap, command/data expiry, idle and balanced IRQ pass")
