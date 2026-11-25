#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify bounded event retries against a software HF callback."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
typedef uint64_t u64;
#define NSEC_PER_MSEC 1000000
#define READ_ONCE(x) (x)
#define FLUSH_ACTION 2
#define DATA_ACTION 0
#define BIAS_ACTION 3
#define CALI_ACTION 4
#define TEMP_ACTION 5
struct hf_manager_event { uint8_t action, sensor_type; };
struct hf_manager { int (*report)(struct hf_manager *, struct hf_manager_event *); };
struct transceiver_state { unsigned flush; };
struct transceiver_device {
    struct { struct hf_manager *manager; } hf_dev;
    struct transceiver_state state[2];
    unsigned flush_lock;
    void *wakeup_src;
};
static unsigned calls, sleeps, warnings, recovering_at;
static bool stopping, stop_on_sleep, transceiver_samples_online = true;
static int status;
static u64 now;
static u64 ktime_get_boottime_ns(void) { return now; }
static bool kthread_should_stop(void) { return stopping; }
static void usleep_range(unsigned int a, unsigned int b)
{
    assert(a == 2000 && b == 4000); sleeps++;
    now += 100 * NSEC_PER_MSEC;
    if (stop_on_sleep) stopping = true;
}
static void pr_warn_ratelimited(const char *f, int err) { assert(f && err); warnings++; }
static void mutex_lock(unsigned *l) { assert(!(*l)++); }
static void mutex_unlock(unsigned *l) { assert((*l)-- == 1); }
static void __pm_wakeup_event(void *w, int ms) { (void)w; (void)ms; }
static bool transceiver_wakeup_check(uint8_t action, uint8_t type) { (void)action; (void)type; return false; }
static void transceiver_update_config(struct transceiver_device *d, struct hf_manager_event *e)
{ (void)d; (void)e; }
static int report(struct hf_manager *m, struct hf_manager_event *e)
{
    assert(m && e);
    calls++;
    return recovering_at && calls == recovering_at ? 0 : status;
}
'''
TEST = r'''
int main(void)
{
    struct hf_manager m = {report};
    struct transceiver_device t = {.hf_dev={&m}};
    struct hf_manager_event e = {.action=DATA_ACTION, .sensor_type=1};
    status = -EIO;
    transceiver_report(&t, &e);
    assert(calls == 1 && !sleeps && warnings == 1);
    status = -EAGAIN;
    recovering_at = 3;
    transceiver_report(&t, &e);
    assert(calls == 3 && sleeps == 1 && warnings == 1);
    recovering_at = 0;
    transceiver_report(&t, &e);
    assert(calls == 15 && sleeps == 12 && warnings == 2);
    stop_on_sleep = true;
    transceiver_report(&t, &e);
    assert(calls == 16 && sleeps == 13);
    transceiver_report(&t, &e);
    assert(calls == 16);
    stopping = false;
    e.action = FLUSH_ACTION;
    t.state[1].flush = 1;
    status = -EIO;
    transceiver_report(&t, &e);
    assert(t.state[1].flush == 1 && !t.flush_lock);
    status = 0;
    transceiver_report(&t, &e);
    assert(!t.state[1].flush && !t.flush_lock);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    callback = re.search(r"(?ms)^static void transceiver_report\([^{}]*\n\{.*?^\}", src)[0]
    with tempfile.TemporaryDirectory(prefix="t70-sensor-report-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callback + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Sensor consumer retries: temporary recovery, deadline, stop, permanent error and flush")


if __name__ == "__main__":
    main()
