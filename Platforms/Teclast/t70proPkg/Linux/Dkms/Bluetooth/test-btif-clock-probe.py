#!/usr/bin/env python3
"""Exercise the staged BTIF registration, deferred probe and removal with mock clocks."""
from pathlib import Path
import re
import subprocess
import sys

folder = Path(sys.argv[1])
plat = (folder / "btif_plat.c").read_text()
core = (folder / "mtk_btif.c").read_text()

def fn(text, sig):
    result = re.search(r"^" + re.escape(sig) + r"\([^;{}]*\n\{.*?^\}", text, re.M | re.S)
    assert result, sig
    return result[0]

prefix = r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#define EPROBE_DEFER 517
#define BTIF_DBG_FUNC(...) ((void)0)
#define BTIF_ERR_FUNC(...) ((void)0)
#define BTIF_INFO_FUNC(...) ((void)0)
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define smp_store_release(p, value) (*(p) = (value))
#define smp_load_acquire(p) (*(p))
struct clk { int prepared, index; };
struct device { int unused; };
struct platform_device { struct device dev; void *data; };
struct singleton { struct device *private_data; };
static struct singleton g_btif[1];
static int btif_probed, fault, unprepare_count, unprepare_log[2];
static unsigned btif_probe_lock, inits, stops;
static int init_status, registration_status, probe_status;
static struct platform_device *boot_device;
static bool registered;
struct platform_driver {
    int (*probe)(struct platform_device *);
    void (*remove)(struct platform_device *);
};
static int mtk_btif_probe(struct platform_device *);
static void mtk_btif_remove(struct platform_device *);
static struct platform_driver mtk_btif_dev_drv = {mtk_btif_probe, mtk_btif_remove};
static void mutex_lock(unsigned *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(unsigned *lock) { assert(*lock); *lock = 0; }
static int platform_driver_register(struct platform_driver *driver)
{
    assert(!registered);
    if (registration_status) return registration_status;
    registered = true;
    if (boot_device) probe_status = driver->probe(boot_device);
    return 0;
}
static void platform_driver_unregister(struct platform_driver *driver)
{
    assert(registered);
    if (boot_device && boot_device->data) driver->remove(boot_device);
    registered = false;
}
static struct clk clocks[2], *clk_btif, *clk_btif_apdma;
static struct clk *devm_clk_get(struct device *dev, const char *name)
{
    int index = !strcmp(name, "apdmac");
    assert(dev && (index || !strcmp(name, "btifc")));
    return fault == index + 1 ? (struct clk *)(intptr_t)-EPROBE_DEFER : &clocks[index];
}
static int clk_prepare(struct clk *clk)
{
    assert(!IS_ERR(clk) && !clk->prepared);
    if (fault == clk->index + 3) return -12;
    clk->prepared = 1; return 0;
}
static void clk_unprepare(struct clk *clk)
{
    assert(clk->prepared && unprepare_count < 2);
    clk->prepared = 0; unprepare_log[unprepare_count++] = clk->index;
}
static void platform_set_drvdata(struct platform_device *p, void *value) { p->data = value; }
static int btif_device_init(void)
{
    assert(btif_probe_lock && registered && g_btif[0].private_data && !btif_probed);
    assert(clocks[0].prepared && clocks[1].prepared);
    inits++;
    return init_status;
}
static void btif_device_stop(void)
{
    assert(btif_probe_lock && registered && g_btif[0].private_data && !btif_probed);
    assert(clocks[0].prepared && clocks[1].prepared);
    stops++;
}
'''
suffix = r'''
int main(void)
{
    struct platform_device device = {0}, duplicate = {0};
    clocks[1].index = 1;

    registration_status = -ENOMEM;
    assert(BTIF_init() == -ENOMEM && !registered && !inits);
    registration_status = 0;
    assert(!BTIF_init() && registered && !t70_btif_ready() && !inits);
    BTIF_exit();
    assert(!registered && !stops);

    boot_device = &device;
    fault = 1;
    assert(!BTIF_init() && registered && probe_status == -EPROBE_DEFER);
    assert(!t70_btif_ready() && !device.data && !inits);
    for (fault = 2; fault <= 4; ++fault) {
        unprepare_count = 0;
        assert(mtk_btif_dev_drv.probe(&device) == (fault == 2 ? -EPROBE_DEFER : -ENOMEM));
        assert(registered && !device.data && !g_btif[0].private_data && !t70_btif_ready());
        assert(!clocks[0].prepared && !clocks[1].prepared);
        assert(unprepare_count == (fault == 4) && !inits);
    }
    fault = unprepare_count = 0;
    init_status = -ENOMEM;
    assert(mtk_btif_dev_drv.probe(&device) == -ENOMEM && inits == 1);
    assert(registered && !device.data && !g_btif[0].private_data && !t70_btif_ready());
    assert(unprepare_count == 2 && !clocks[0].prepared && !clocks[1].prepared);

    init_status = unprepare_count = 0;
    assert(!mtk_btif_dev_drv.probe(&device) && inits == 2);
    assert(device.data && g_btif[0].private_data == &device.dev && t70_btif_ready());
    assert(clocks[0].prepared && clocks[1].prepared);
    assert(mtk_btif_dev_drv.probe(&duplicate) == -EBUSY && !duplicate.data && inits == 2);

    BTIF_exit();
    assert(!registered && !device.data && !g_btif[0].private_data && !t70_btif_ready());
    assert(stops == 1 && !clocks[0].prepared && !clocks[1].prepared);
    assert(unprepare_count == 2 && unprepare_log[0] == 1 && unprepare_log[1] == 0);
    return 0;
}
'''
c = Path(sys.argv[2])
c.write_text("\n".join((prefix, fn(plat, "int hal_btif_clk_get_and_prepare"),
                         fn(plat, "int hal_btif_clk_unprepare"),
                         fn(core, "bool t70_btif_ready"),
                         fn(core, "static int mtk_btif_probe"),
                         fn(core, "static void mtk_btif_remove"),
                         fn(core, "static int BTIF_init"),
                         fn(core, "static void BTIF_exit"), suffix)))
exe = c.with_suffix("")
subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                str(c), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=3)
print("BTIF lifecycle: registration, deferred clocks, failed setup, later bind and balanced remove passed")
