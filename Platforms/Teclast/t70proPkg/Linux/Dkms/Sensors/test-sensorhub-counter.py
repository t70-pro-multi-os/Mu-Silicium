#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check actual staged T70 counter conversion and filter handoff math."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
typedef int64_t s64;
typedef int32_t s32;
#define check_mul_overflow __builtin_mul_overflow
#define check_add_overflow __builtin_add_overflow
#define check_sub_overflow __builtin_sub_overflow
#define local_irq_save(f) ((f)=0)
#define local_irq_restore(f) ((void)(f))
#define spin_lock_irqsave(l,f) (*(l)=1,(f)=0)
#define spin_unlock_irqrestore(l,f) (*(l)=0,(void)(f))
struct timesync_filter { unsigned lock, tail, cnt; s64 offset, offset_debug, last_time; };
static s64 host_time, host_ticks;
static unsigned pairs;
static int64_t ktime_get_boottime_ns(void) { return host_time; }
static int64_t __arch_counter_get_cntvct(void) { return host_ticks; }
static s64 div_s64_rem(s64 a, s32 b, s32 *r) { *r = a%b; return a/b; }
static void timesync_filter_calculate(struct timesync_filter *f, s64 host, s64 adjusted)
{ pairs++; f->offset = host - adjusted; }
'''
TEST = r'''
int main(void)
{
    s64 ns;
    const s64 one_sec = 1000000000LL, rate = 13000000LL;
    struct timesync_filter f = {.cnt=5, .tail=6, .offset=100, .offset_debug=101, .last_time=1};
    assert(t70_counter_ns(13, &ns) && ns == 1000);
    assert(t70_counter_ns(-14, &ns) && ns == -1076);
    assert(t70_counter_ns(rate * 7200, &ns) && ns == 7200 * one_sec);
    assert(!t70_counter_ns(INT64_MAX, &ns));
    timesync_filter_reset(&f);
    assert(!f.lock && !f.tail && !f.cnt && !f.offset && !f.offset_debug && !f.last_time);

    host_time = 301 * one_sec;
    host_ticks = 41 * rate;
    timesync_filter_set(&f, 10 * one_sec, 40 * rate);
    assert(f.offset == 290 * one_sec && pairs == 1);
    host_time += 2 * one_sec;
    host_ticks += 2 * rate;
    timesync_filter_set(&f, 10 * one_sec, 40 * rate);
    assert(f.offset == 290 * one_sec && pairs == 2);

    timesync_filter_reset(&f);
    host_time = 900 * one_sec; /* boottime now includes the suspend gap */
    host_ticks = 70 * rate;
    timesync_filter_set(&f, 1 * one_sec, 69 * rate);
    assert(f.offset == 898 * one_sec && pairs == 3);

    timesync_filter_set(&f, INT64_MAX, 69 * rate);
    assert(pairs == 3 && f.offset == 898 * one_sec);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    a = p.parse_args()
    src = a.source.read_text()
    names = ("t70_counter_ns", "timesync_filter_reset", "timesync_filter_set")
    callbacks = "\n".join(re.search(r"(?ms)^(?:static )?(?:bool|void) " + name
                                   + r"\([^{}]*\n\{.*?^\}", src)[0] for name in names)
    with tempfile.TemporaryDirectory(prefix="t70-sensor-counter-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(SHIM + callbacks + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("13 MHz counter: long interval, delayed delivery, boottime gap and new offset passed")


if __name__ == "__main__":
    main()
