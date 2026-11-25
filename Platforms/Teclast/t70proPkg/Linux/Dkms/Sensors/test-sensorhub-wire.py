#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test the staged sensorhub notification/readiness callbacks without SCP."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define pr_err(...) ((void)0)
#define pr_err_ratelimited(...) ((void)0)
#define pr_notice(...) ((void)0)
#define READ_ONCE(x) (x)
#define MAX_SENS_COMM_NOTIFY_CMD 9
struct sensor_comm_notify {
    uint8_t sequence, sensor_type, command, length, crc8, pad[3];
    int32_t value[5];
};
struct sensor_comm_ctrl { uint8_t sequence, sensor_type, command, length, crc8; };
struct sensor_notify_handle {
    void (*handler)(struct sensor_comm_notify *, void *);
    void *private_data;
};
static struct sensor_notify_handle sens_notify_handle[MAX_SENS_COMM_NOTIFY_CMD];
static struct sensor_notify_handle *sens_notify_live[MAX_SENS_COMM_NOTIFY_CMD];
static int sens_notify_lock, grace;
#define guard(t) fake_guard_##t
#define fake_guard_mutex(p) ((void)(p))
#define fake_guard_rcu() ((void)0)
#define rcu_dereference(p) (p)
#define rcu_access_pointer(p) (p)
#define rcu_assign_pointer(p, v) ((p) = (v))
static void synchronize_rcu(void)
{
    assert(!sens_notify_live[5] && sens_notify_handle[5].handler &&
           sens_notify_handle[5].private_data);
    ++grace;
}
static bool scp_status;
static int init_rc, notified, registered, transfers, received, sensor_comm_sequence;
static int sensor_comm_ready_notifier;
static void atomic_set(int *v, int n) { *v = n; }
static int ipi_comm_init(void) { return init_rc; }
static void ipi_comm_notify_handler_register(void (*f)(int, void *, unsigned int))
{ assert(f); notified++; }
static void sensor_ready_notifier_chain_register(int *n) { assert(n); registered++; }
static int64_t ktime_get_boottime_ns(void) { return 0; }
static int sensor_comm_ctrl_seq_send(struct sensor_comm_ctrl *c, unsigned int n)
{ assert(c && n == 8); transfers++; return 0; }
static int get_notify_id(void) { return 31; }
static int ipi_comm_noack(int id, unsigned char *p, unsigned int n)
{ assert(id == 31 && p && n == 28); transfers++; return 0; }
'''
TEST = r'''
static void receive(struct sensor_comm_notify *n, void *ctx)
{ assert(n->sequence == 7 && ctx == &received); received++; }
int main(void)
{
    struct sensor_comm_notify n = {.sequence=7, .command=5, .length=4};
    struct sensor_comm_ctrl c = {0};
    init_rc = -ENODEV;
    assert(sensor_comm_init() == -ENODEV && !registered && !notified);
    init_rc = 0;
    assert(sensor_comm_init() == 0 && registered == 1 && notified == 1);
    assert(sensor_comm_notify(&n) == -EAGAIN && sensor_comm_ctrl_send(&c, 8) == -EAGAIN);
    assert(!transfers);
    scp_status = true;
    assert(!sensor_comm_notify(&n) && !sensor_comm_ctrl_send(&c, 8) && transfers == 2);
    assert(tiny_crc8((uint8_t *)"123456789", 9) == 0xa1);
    sensor_comm_notify_handler_register(5, receive, &received);
    sensor_comm_notify_handler_register(5, receive, &notified); /* no replacement */
    sensor_comm_notify_handler(32, &n, sizeof(n));
    assert(received == 1);
    sensor_comm_notify_handler_unregister(5);
    sensor_comm_notify_handler(32, &n, sizeof(n));
    assert(received == 1 && grace == 1);
    sensor_comm_notify_handler_unregister(5);
    assert(grace == 1);
    sensor_comm_notify_handler_register(5, receive, &received);
    n.crc8 ^= 1;
    sensor_comm_notify_handler(32, &n, sizeof(n));
    assert(received == 1);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    args = parser.parse_args()
    src = (args.stage / "sensor_comm.c").read_text()
    names = ("sensor_comm_notify_handler", "sensor_comm_ctrl_send", "sensor_comm_notify",
             "sensor_comm_init", "sensor_comm_notify_handler_register",
             "sensor_comm_notify_handler_unregister")
    callbacks = "\n".join(
        re.search(r"(?ms)^(?:static )?(?:int|void) " + name + r"\([^{}]*\n\{.*?^\}", src)[0]
        for name in names)
    assert callbacks.index("sizeof(*notify) != len") < callbacks.index("crc = tiny_crc8")
    crc = (args.stage / "tiny_crc8.c").read_text()
    crc = re.sub(r"(?m)^#include .*\n", "", crc)
    with tempfile.TemporaryDirectory(prefix="t70-sensorhub-wire-fixture-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(SHIM + crc + callbacks + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Sensorhub wire: real CRC, online/offline state and IPI init error pass")


if __name__ == "__main__":
    main()
