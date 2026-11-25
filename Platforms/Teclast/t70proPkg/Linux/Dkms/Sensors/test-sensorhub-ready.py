#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the staged two-sided readiness handshake with host-only provider shims."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define SCP_EVENT_STOP 0
#define SCP_EVENT_READY 1
#define SENSOR_TYPE_INVALID 0
#define SENS_COMM_NOTIFY_READY_CMD 4
#define NOTIFY_DONE 0
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define pr_err(...) ((void)0)
struct notifier_block { int unused; };
struct sensor_comm_notify {
    uint8_t sequence, sensor_type, command, length, crc8, pad[3];
    int32_t value[5];
};
static bool scp_platform_ready, scp_sensor_ready, sensor_ready;
static unsigned scheduled, sent, immediate_ack;
static int sensor_ready_lock, sensor_ready_workqueue, sensor_ready_work;
static void queue_work(int q, int *w) { (void)q; assert(w); scheduled++; }
static void scp_sensor_ready_notify_handler(struct sensor_comm_notify *, void *);
static int sensor_comm_notify_bypass(struct sensor_comm_notify *n)
{
    assert(n->sequence == 0 && n->sensor_type == SENSOR_TYPE_INVALID);
    assert(n->command == 4 && n->length == 0 && n->crc8 == 0);
    for (unsigned i = 0; i < 3; i++) assert(!n->pad[i]);
    for (unsigned i = 0; i < 5; i++) assert(!n->value[i]);
    sent++;
    if (immediate_ack) scp_sensor_ready_notify_handler(n, 0);
    return 0;
}
'''
TEST = r'''
int main(void)
{
    assert(!scp_platform_ready && !sensor_ready);
    scp_platform_ready_notifier_call(0, SCP_EVENT_READY, 0);
    assert(scp_platform_ready && sent == 1 && !sensor_ready && !scheduled);
    scp_sensor_ready_notify_handler(0, 0);
    assert(sensor_ready && scheduled == 1);
    scp_platform_ready_notifier_call(0, SCP_EVENT_STOP, 0);
    assert(!sensor_ready && !scp_platform_ready && !scp_sensor_ready && scheduled == 2);
    immediate_ack = 1;
    scp_platform_ready_notifier_call(0, SCP_EVENT_READY, 0);
    assert(sensor_ready && scp_platform_ready && scp_sensor_ready && sent == 2 && scheduled == 3);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("stage", type=Path)
    a = p.parse_args()
    src = (a.stage / "ready.c").read_text()
    callbacks = "\n".join(
        re.search(r"(?ms)^static (?:int|void) " + name + r"\([^{}]*\n\{.*?^\}", src)[0]
        for name in ("scp_sensor_ready_notify_handler", "scp_platform_ready_notifier_call"))
    with tempfile.TemporaryDirectory(prefix="t70-sensorhub-ready-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(SHIM + callbacks + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Two-sided READY, initialized output, early ACK and STOP passed")


if __name__ == "__main__":
    main()
