#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the staged preflight with software-only SCP/provider shims."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t phys_addr_t;
#define U32_MAX UINT32_MAX
#define IS_ALIGNED(v,a) (((v)&((a)-1))==0)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define MAX_SHARE_MEM_PAYLOAD_TYPE 6
struct share_mem_base { u32 rp, wp, capacity, item_size; uint8_t data[128]; };
struct share_mem_config { unsigned payload_type; struct share_mem_base *base; u32 buffer_size; };
struct share_mem_usage { unsigned payload_type, id; };
struct share_mem_config_handle { int (*handler)(struct share_mem_config *, void *); void *private_data; };
static struct share_mem_usage shm_usage_table[]={{0,0},{1,1},{2,2},{3,3},{4,4},{5,5}};
static struct share_mem_config_handle shm_handle[6];
static int shm_handler_lock;
static void unlock(int **lk) { assert(**lk == 1); **lk = 0; }
#define guard(t) fake_guard_##t
#define fake_guard_mutex(p) \
    int *held __attribute__((cleanup(unlock))) = (assert(!*(p)), *(p) = 1, (p))
static struct share_mem_base maps[6];
static phys_addr_t bases[6], sizes[6];
static unsigned called, published, fail_at;
static int publish_status;
static phys_addr_t scp_get_reserve_mem_phys(unsigned i) { return bases[i]; }
static phys_addr_t scp_get_reserve_mem_size(unsigned i) { return sizes[i]; }
static phys_addr_t scp_get_reserve_mem_virt(unsigned i) { return (uintptr_t)&maps[i]; }
static int apply(struct share_mem_config *c, void *ctx)
{
    unsigned i = c->payload_type;
    assert(shm_handler_lock && i < 6 && ctx == &maps[i] && c->base == ctx && c->buffer_size == sizes[i]);
    called++;
    return i == fail_at ? -EIO : 0;
}
static int share_mem_send_config(const u32 *passed)
{
    assert(shm_handler_lock);
    for (unsigned int i=0; i<6; i++) assert(passed[i] == bases[i]);
    published++;
    return publish_status;
}
'''
TEST = r'''
int main(void)
{
    for (unsigned i=0; i<6; i++) {
        bases[i] = 0x1000 + i*0x1000;
        sizes[i] = 128;
        share_mem_config_handler_register(i, apply, &maps[i]);
    }
    share_mem_config_handler_register(0, apply, &maps[1]); /* no replace */
    share_mem_config_handler_unregister(5);
    assert(share_mem_config() == -ENODEV && !called && !published);
    share_mem_config_handler_register(5, apply, &maps[5]);
    sizes[5] = 0;
    assert(share_mem_config() == -ERANGE && !called && !published);
    sizes[5] = 128;
    bases[5] = 0xffffffc0ull;
    assert(share_mem_config() == -ERANGE && !called && !published);
    bases[5] = bases[4] + 64;
    assert(share_mem_config() == -EINVAL && !called && !published);
    bases[5] = 0x6000;
    fail_at = 2;
    assert(share_mem_config() == -EIO && called == 3 && !published);
    fail_at = 6;
    publish_status = -EAGAIN;
    assert(share_mem_config() == -EAGAIN && called == 9 && published == 1);
    publish_status = 0;
    assert(share_mem_config() == 0 && called == 15 && published == 2);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    functions = ("share_mem_valid_extent", "share_mem_config",
                 "share_mem_config_handler_register", "share_mem_config_handler_unregister")
    callbacks = "\n".join(re.search(r"(?ms)^(?:static )?(?:bool|int|void) " + name
                                   + r"\([^{}]*\n\{.*?^\}", src)[0] for name in functions)
    with tempfile.TemporaryDirectory(prefix="t70-sensor-config-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callbacks + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Sensor memory preflight rejects incomplete, overlapping and out-of-range maps")


if __name__ == "__main__":
    main()
