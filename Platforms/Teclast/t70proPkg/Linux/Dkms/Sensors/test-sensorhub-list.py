#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the staged enumeration ACK correlation without a firmware peer."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define SENSOR_TYPE_INVALID 0
#define SENS_COMM_NOTIFY_LIST_CMD 5
struct sensor_comm_notify { uint8_t sequence, sensor_type, command; int32_t value[5]; };
static bool list_pending;
static uint8_t pending_sequence;
static uint32_t list_write_position;
static unsigned rx_notify_lock, sensor_list_done, acknowledgements;
static void spin_lock(unsigned *l) { assert(!(*l)++); }
static void spin_unlock(unsigned *l) { assert((*l)-- == 1); }
static void complete(unsigned *n) { assert(rx_notify_lock && n == &sensor_list_done); acknowledgements++; }
'''
TEST = r'''
int main(void)
{
    struct sensor_comm_notify n = {.sequence=1, .command=5, .value={40}};
    sensor_list_notify_handler(&n, 0);
    assert(!acknowledgements);
    list_pending = true;
    pending_sequence = 1;
    n.sequence = 2;
    sensor_list_notify_handler(&n, 0);
    n.sequence = 1;
    n.sensor_type = 3;
    sensor_list_notify_handler(&n, 0);
    n.sensor_type = 0;
    n.command = 4;
    sensor_list_notify_handler(&n, 0);
    assert(list_pending && !acknowledgements && !list_write_position);
    n.command = 5;
    sensor_list_notify_handler(&n, 0);
    assert(!list_pending && acknowledgements == 1 && list_write_position == 40);
    n.value[0] = 80;
    sensor_list_notify_handler(&n, 0);
    assert(!list_pending && acknowledgements == 1 && list_write_position == 40);
    pending_sequence = 2;
    list_pending = true;
    sensor_list_notify_handler(&n, 0);
    assert(list_pending && acknowledgements == 1 && list_write_position == 40);
    n.sequence = 2;
    sensor_list_notify_handler(&n, 0);
    assert(!list_pending && acknowledgements == 2 && list_write_position == 80);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    callback = re.search(r"(?ms)^static void sensor_list_notify_handler\([^{}]*\n\{.*?^\}", src)[0]
    exchange = re.search(r"(?ms)^static int sensor_list_seq_get_list\([^{}]*\n\{.*?^\}", src)[0]
    assert exchange.index("reinit_completion") < exchange.index("list_pending = true;")
    assert exchange.index("list_pending = true;") < exchange.index("ret = sensor_comm_notify")
    assert exchange.count("list_pending = false;") == 2
    assert exchange.index("if (!timeout)") < exchange.index("share_mem_seek(")
    with tempfile.TemporaryDirectory(prefix="t70-sensor-list-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callback + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Sensor-list ACK: rejects unrelated, unsolicited, duplicate and late replies")


if __name__ == "__main__":
    main()
