#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the exact staged sensor worker handles a wake for stop with no samples."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdint.h>
struct transceiver_device { int normal_wp_dropped, super_wp_dropped; };
static int stopped, waits, samples, transceiver_pending, sensor_boot_lock;
static int transceiver_samples_online = 1;
#define READ_ONCE(v) (v)
#define unlikely(v) (v)
#define pr_err_ratelimited(...) ((void)0)
static int kthread_should_stop(void) { return stopped; }
static int atomic_read(int *p) { return *p; }
static int atomic_xchg(int *p, int n) { int old = *p; *p = n; return old; }
#define wait_event_interruptible(q, cond) \
    ({ ++waits; if (!(cond)) stopped = 1; assert(cond); 0; })
static void mutex_lock(int *p) { assert(!(*p)++); }
static void mutex_unlock(int *p) { assert((*p)-- == 1); }
static void transceiver_process(struct transceiver_device *dev)
{ assert(dev && sensor_boot_lock); ++samples; }
static void transceiver_process_super(struct transceiver_device *dev)
{ assert(dev && sensor_boot_lock); ++samples; }
'''
TEST = r'''
int main(void)
{
    struct transceiver_device dev = {0};
    assert(!transceiver_thread(&dev));
    assert(waits == 1 && !samples && !sensor_boot_lock);
    stopped = 0;
    transceiver_pending = 1;
    assert(!transceiver_thread(&dev));
    assert(waits == 3 && samples == 2 && !transceiver_pending && !sensor_boot_lock);
}
'''
if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    src = p.parse_args().source.read_text()
    match = re.search(r"(?ms)^static int transceiver_thread\([^{}]*\n\{.*?^\}", src)
    if not match:
        p.error("missing staged transceiver worker")
    with tempfile.TemporaryDirectory() as t:
        c = Path(t) / "test.c"
        exe = c.with_suffix("")
        c.write_text(SHIM + match[0] + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("Sensor worker: no-input stop exits; pending work drains then stop exits")
