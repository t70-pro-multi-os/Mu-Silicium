#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the real staged sensor ring with a host RAM buffer, not an SCP map."""
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
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define smp_wmb() ((void)0)
struct share_mem_base { u32 rp, wp, buffer_size, item_size; uint8_t data[120]; };
struct share_mem {
    struct share_mem_base *base;
    unsigned int lock;
    u32 host_capacity, item_size, write_position, last_write_position;
    u32 buffer_full_written, buffer_full_threshold;
    bool buffer_full_detect;
    const char *name;
};
struct share_mem_config { struct share_mem_base *base; u32 buffer_size; };
static unsigned io;
static void mutex_lock(unsigned *p) { assert(!(*p)++); }
static void mutex_unlock(unsigned *p) { assert((*p)-- == 1); }
static void mock_copy(void *dst, const void *src, size_t count)
{ if (count) { io++; memcpy(dst, src, count); } }
#define memcpy_fromio mock_copy
#define memcpy_toio mock_copy
static void share_mem_buffer_full_detect(struct share_mem *shm, u32 write)
{ (void)shm; (void)write; }
'''
TEST = r'''
int main(void)
{
    struct share_mem_base ring = {.wp=17, .rp=17, .buffer_size=17, .item_size=17};
    struct share_mem s = {.name="sensor_list", .item_size=40};
    struct share_mem_config c = {.base=&ring, .buffer_size=15};
    uint8_t dst[80] = {0};
    assert(share_mem_init(&s, &c) == -EINVAL && !s.base && ring.wp == 17);
    c.buffer_size = 56;
    assert(share_mem_init(&s, &c) == -EINVAL && !s.base && ring.wp == 17);
    c.buffer_size = sizeof(ring);
    assert(!share_mem_init(&s, &c) && s.host_capacity == 120 && ring.item_size == 40);
    assert(!share_mem_init(&s, &c) && s.lock == 0);
    assert(!share_mem_read_dram(&s, dst, sizeof(dst)) && !io);
    assert(share_mem_seek(&s, 15) == -EINVAL && !s.write_position);
    assert(share_mem_seek(&s, 120) == -EINVAL && !s.write_position);
    ring.buffer_size = 80;
    assert(share_mem_read_dram(&s, dst, sizeof(dst)) == -EIO && !io);
    assert(share_mem_write_dram(&s, dst, sizeof(dst)) == -EIO && !io);
    ring.buffer_size = 120;
    ring.rp = 80;
    memset(ring.data + 80, 0x13, 40);
    memset(ring.data, 0x47, 40);
    assert(!share_mem_seek(&s, 40));
    assert(share_mem_read_dram(&s, dst, sizeof(dst)) == 80 && io == 2 && ring.rp == 40);
    for (unsigned i = 0; i < 40; i++) assert(dst[i] == 0x13 && dst[i+40] == 0x47);
    ring.wp = 80;
    assert(share_mem_write_dram(&s, dst, 40) == 40 && ring.wp == 0 && io == 3);
    assert(share_mem_write_dram(&s, dst, 40) == 0 && ring.wp == 0 && io == 3);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    functions = ("share_mem_ring_valid", "share_mem_seek", "share_mem_read_dram",
                 "share_mem_write_dram", "share_mem_init")
    callbacks = "\n".join(re.search(r"(?ms)^(?:static )?(?:bool|int) " + name
                                   + r"\([^{}]*\n\{.*?^\}", src)[0] for name in functions)
    with tempfile.TemporaryDirectory(prefix="t70-sensor-shm-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callbacks + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("Sensor ring: capacity, alignment, wrapped read and full-write behavior pass")


if __name__ == "__main__":
    main()
