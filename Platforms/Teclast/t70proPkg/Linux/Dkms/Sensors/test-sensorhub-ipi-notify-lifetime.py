#!/usr/bin/env python3
"""Run the exact staged notify bridge with modeled RCU publish and drain."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

p = Path(sys.argv[1])
src = p.read_text()
funcs = []
for name in ("ipi_comm_notify_handler", "ipi_comm_notify_handler_register",
             "ipi_comm_notify_handler_unregister"):
    m = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;{}]*\n\{.*?^\}", src, re.S | re.M)
    assert m, name
    funcs.append(m[0])
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#define IPI_IN_SENSOR_NOTIFY 32
typedef void ipi_notify_fn(int id, void *data, unsigned int len);
static struct { ipi_notify_fn *notify_callback; } controller;
static int grace, calls, payload, held;
static void unlock(int *v) { assert(*v == 1 && held == 1); held = 0; }
#define guard(t) fake_guard_##t
#define fake_guard_rcu() int read_section __attribute__((cleanup(unlock))) = ++held
#define rcu_dereference(p) (assert(held), (p))
#define rcu_assign_pointer(p, v) ((p) = (v))
static void synchronize_rcu(void)
{ assert(!held && !controller.notify_callback); ++grace; }
static void receiver(int id, void *p, unsigned int len)
{ assert(id == 32 && p == &payload && len == 4 && held); ++calls; }
'''
suffix = r'''
int main(void)
{
    assert(!ipi_comm_notify_handler(32, NULL, &payload, 4) && !calls && !held);
    ipi_comm_notify_handler_register(receiver);
    assert(ipi_comm_notify_handler(31, NULL, &payload, 4) == -EINVAL && !calls && !held);
    assert(!ipi_comm_notify_handler(32, NULL, &payload, 4) && calls == 1 && !held);
    ipi_comm_notify_handler_unregister();
    assert(!ipi_comm_notify_handler(32, NULL, &payload, 4) && calls == 1 && grace == 1 && !held);
    ipi_comm_notify_handler_register(receiver);
    assert(!ipi_comm_notify_handler(32, NULL, &payload, 4) && calls == 2 && !held);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-ipi-notify-") as tmp:
    c = Path(tmp) / "notify.c"
    c.write_text(prefix + "\n".join(funcs) + suffix)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("Sensor IPI notify callback publish, protected dispatch and synchronous unregister passed")
