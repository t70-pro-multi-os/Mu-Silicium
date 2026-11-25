#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check exact staged sample setup against host-only readiness stubs."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#define NOTIFY_DONE 0
#define WRITE_ONCE(x,v) ((x)=(v))
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define kfifo_reset(f) (*(f)=0)
#define pr_err(...) ((void)0)
struct work_struct { int unused; };
struct delayed_work { bool pending; void (*callback)(struct work_struct *); };
#define DECLARE_DELAYED_WORK(name, fn) struct delayed_work name = { .callback = fn }
#define system_dfl_wq 1
static unsigned msecs_to_jiffies(unsigned ms) { assert(ms > 0); return ms; }
static void mod_delayed_work(int queue, struct delayed_work *work, unsigned delay)
{ assert(queue == system_dfl_wq && delay > 0); work->pending = true; }
static void cancel_delayed_work(struct delayed_work *work) { work->pending = false; }
static void run_work(struct delayed_work *work)
{ assert(work->pending); work->pending = false; work->callback(0); }
struct transceiver_device { bool first_bootup; unsigned int filter; };
static int config_err, manager_err;
static unsigned config_calls;
struct notifier_block { int unused; };
static struct transceiver_device transceiver_dev;
static unsigned created, restored, synced, stopped, sensor_boot_lock;
static unsigned transceiver_fifo, transceiver_super_fifo, transceiver_fifo_lock;
static bool transceiver_samples_online, reopen_while_waiting;
static void mutex_lock(unsigned *m)
{
    assert(!(*m)++);
    if (reopen_while_waiting) {
        /* Model a retiring READY callback before STOP obtains the mutex. */
        transceiver_samples_online = true;
        transceiver_fifo = transceiver_super_fifo = 1;
    }
}
static void mutex_unlock(unsigned *m) { assert((*m)-- == 1); }
static bool atomic_read(bool *b) { return *b; }
static void atomic_set(bool *b, bool value) { *b = value; }
static int share_mem_config(void) { assert(sensor_boot_lock); config_calls++; return config_err; }
static int transceiver_create_manager(struct transceiver_device *d)
{ assert(d->first_bootup); created++; return manager_err; }
static void timesync_filter_reset(unsigned int *filter) { assert(!transceiver_samples_online); (*filter)++; }
static void timesync_start(void) { assert(transceiver_samples_online); synced++; }
static void timesync_stop(void) { assert(!transceiver_samples_online); stopped++; }
static void transceiver_restore_sensor(struct transceiver_device *d)
{ assert(!d->first_bootup && synced > 0); restored++; }
'''
TEST = r'''
int main(void)
{
    struct transceiver_device *dev = &transceiver_dev;
    dev->first_bootup = true;
    config_err = -1;
    transceiver_ready_notifier_call(0, 1, 0);
    assert(dev->first_bootup && !transceiver_samples_online && transceiver_boot_work.pending);
    assert(config_calls == 1 && !created && !synced);
    config_err = 0;
    manager_err = -1;
    run_work(&transceiver_boot_work);
    assert(dev->first_bootup && config_calls == 2 && created == 1 && transceiver_boot_work.pending);
    manager_err = 0;
    run_work(&transceiver_boot_work);
    assert(!dev->first_bootup && config_calls == 3 && created == 2 && synced == 1 && !restored);
    assert(transceiver_samples_online && !transceiver_boot_work.pending && !sensor_boot_lock);

    // STOP cancels a pending retry and closes the sample gate even if a
    // retiring callback briefly reopened it before STOP took the lock.
    config_err = -1;
    transceiver_ready_notifier_call(0, 1, 0);
    assert(!transceiver_samples_online && transceiver_boot_work.pending);
    reopen_while_waiting = true;
    transceiver_ready_notifier_call(0, 0, 0);
    reopen_while_waiting = false;
    transceiver_boot_work.callback(0);
    assert(config_calls == 4 && !transceiver_boot_work.pending && !transceiver_samples_online);
    assert(!transceiver_fifo && !transceiver_super_fifo && !restored && synced == 1);

    // A new READY can recover. An unresolved failure has a finite retry budget.
    transceiver_ready_notifier_call(0, 1, 0);
    for (unsigned i = 1; i < TRANSCEIVER_BOOT_ATTEMPTS; i++)
        run_work(&transceiver_boot_work);
    assert(!transceiver_boot_work.pending && !transceiver_samples_online);
    assert(config_calls == 4 + TRANSCEIVER_BOOT_ATTEMPTS && !restored && synced == 1);
    config_err = 0;
    transceiver_ready_notifier_call(0, 1, 0);
    assert(transceiver_samples_online && !transceiver_boot_work.pending);
    assert(created == 2 && restored == 1 && synced == 2 && dev->filter == 2);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    gate = re.search(r"(?ms)^static void transceiver_sample_gate\([^{}]*\n\{.*?^\}", src)[0]
    start = src.index("#define TRANSCEIVER_BOOT_ATTEMPTS")
    end = src.index("\nstatic struct notifier_block transceiver_ready_notifier", start)
    callback = gate + "\n" + src[start:end]
    with tempfile.TemporaryDirectory(prefix="t70-sensor-first-ready-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callback + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Sensor sample gate: failed start, retry, reconnection, STOP and queued samples")


if __name__ == "__main__":
    main()
