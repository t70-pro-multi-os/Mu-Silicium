#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the staged timesync work lifecycle without a timer, mailbox or device."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,n) ((x)=(n))
#define system_dfl_wq 0
#define msecs_to_jiffies(x) (x)
struct work_struct { int x; };
struct delayed_work { struct work_struct w; bool enabled, pending; };
static bool timesync_suspend_flag, timesync_requested, timesync_armed, timesync_dead;
static struct delayed_work timesync_work;
static unsigned timesync_run_lock, calls, releases, requeue_blocked, wakeup_src;
static bool simulate_running;
static void mutex_lock(unsigned *lock) { assert(!(*lock)++); }
static void mutex_unlock(unsigned *lock) { assert((*lock)-- == 1); }
static void timesync_comm_with(void) { assert(!releases); if (!timesync_suspend_flag) calls++; }
static void enable_delayed_work(struct delayed_work *w) { assert(!w->enabled); w->enabled = true; }
static void queue_delayed_work(int q, struct delayed_work *w, unsigned d)
{ assert(!q && d == 10000); if (w->enabled) w->pending = true; else requeue_blocked++; }
#define mod_delayed_work queue_delayed_work
static void timesync_work_func(struct work_struct *work);
static void disable_delayed_work_sync(struct delayed_work *w)
{
    assert(w->enabled); w->enabled = w->pending = false;
    if (simulate_running) timesync_work_func(&w->w);
}
static void wakeup_source_unregister(unsigned w)
{ (void)w; assert(!timesync_work.enabled && !timesync_work.pending); releases++; }
'''
TEST = r'''
int main(void)
{
    timesync_resume();
    assert(!calls && !timesync_work.pending);
    timesync_start();
    assert(timesync_work.pending);
    timesync_work.pending = false;
    timesync_work_func(&timesync_work.w);
    assert(calls == 1 && timesync_work.pending);
    simulate_running = true;
    timesync_suspend();
    assert(!timesync_work.pending && requeue_blocked == 1 && calls == 1);
    timesync_resume();
    assert(timesync_work.pending && calls == 2);
    timesync_stop();
    assert(!timesync_work.pending && requeue_blocked == 2);
    timesync_resume();
    assert(!timesync_work.pending && calls == 3);
    timesync_start();
    timesync_exit();
    assert(releases == 1 && !timesync_work.pending && requeue_blocked == 3 && calls == 4);
    timesync_resume();
    timesync_start();
    assert(calls == 4 && !timesync_work.pending && !timesync_run_lock);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    names = ("timesync_work_func", "timesync_arm_locked", "timesync_drain_locked",
             "timesync_start", "timesync_stop", "timesync_resume", "timesync_suspend", "timesync_exit")
    callbacks = "\n".join(re.search(r"(?ms)^(?:static )?void " + name
                                   + r"\([^{}]*\n\{.*?^\}", src)[0] for name in names)
    with tempfile.TemporaryDirectory(prefix="t70-timesync-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callbacks + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Timesync: no resume before first start, drain wins over requeue, no restart after exit")


if __name__ == "__main__":
    main()
