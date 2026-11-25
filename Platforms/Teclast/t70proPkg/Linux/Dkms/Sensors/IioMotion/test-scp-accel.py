#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the actual IIO consumer callbacks with an in-memory test provider."""
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
#define CLOCK_BOOTTIME 7
#define IIO_CHAN_INFO_SCALE 1
#define IIO_ACCEL 2
#define IIO_VAL_FRACTIONAL 10
#define __aligned(n) __attribute__((aligned(n)))
struct iio_dev { int clock; void *priv; };
struct iio_chan_spec { int type; };
typedef int (*t70_motion_sample_fn)(void *ctx, const s32 xyz[3], s64 boottime_ns);
struct t70_motion_link {
        void *priv;
        int scale_num, scale_den;
        int (*start)(void *, t70_motion_sample_fn, void *);
        void (*stop)(void *);
        void (*release)(void *);
};
static unsigned delivered, starts, stops;
static bool want_ts;
static int fail_start;
static const s32 xyz[3] = {17,-23,199};
static const s64 acquired = 900123456789;
static int iio_device_get_clock(struct iio_dev *indio) { return indio->clock; }
static void *iio_priv(struct iio_dev *indio) { return indio->priv; }
static int iio_push_to_buffers_with_ts(struct iio_dev *indio, void *data, size_t len, s64 ts)
{
        unsigned char *bytes = data;
        assert(indio && len == 24 && ts == acquired);
        assert(!memcmp(bytes, xyz, sizeof(xyz)));
        if (want_ts) {
                for (unsigned int i=12; i<16; i++) assert(!bytes[i]);
                memcpy(bytes + 16, &ts, sizeof(ts));
                assert(!memcmp(bytes + 16, &acquired, sizeof(acquired)));
        }
        delivered++;
        return 0;
}
static int fake_start(void *priv, t70_motion_sample_fn sample, void *ctx)
{
        assert(!priv);
        starts++;
        return fail_start ? -EAGAIN : sample(ctx, xyz, acquired);
}
static void fake_stop(void *priv) { assert(!priv); stops++; }
'''
TEST = r'''
int main(void)
{
        struct t70_accel a = {.link = {.scale_num=1, .scale_den=1024,
                                       .start=fake_start, .stop=fake_stop}};
        struct iio_dev io = {.clock=0, .priv=&a};
        struct iio_chan_spec c = {.type=IIO_ACCEL};
        int v, v2;
        assert(t70_read_raw(&io, &c, &v, &v2, IIO_CHAN_INFO_SCALE) == IIO_VAL_FRACTIONAL);
        assert(v == 1 && v2 == 1024);
        assert(t70_postenable(&io) == -EOPNOTSUPP && !starts);
        assert(t70_sample(&io, xyz, acquired) == -EOPNOTSUPP && !delivered);
        io.clock = CLOCK_BOOTTIME;
        assert(t70_sample(&io, xyz, 0) == -EINVAL && !delivered);
        fail_start = 1;
        assert(t70_postenable(&io) == -EAGAIN && starts == 1 && !a.started);
        assert(!t70_predisable(&io) && !stops);
        fail_start = 0;
        assert(!t70_postenable(&io) && a.started && starts == 2 && delivered == 1);
        assert(!t70_predisable(&io) && !a.started && stops == 1);
        assert(!t70_predisable(&io) && stops == 1);
        want_ts = true;
        assert(!t70_postenable(&io) && a.started && starts == 3 && delivered == 2);
        assert(!t70_predisable(&io) && !a.started && stops == 2);
        return 0;
}
'''

src = Path(__file__).with_name("t70-scp-accel.c").read_text()
types = "\n".join(re.search(r"(?ms)^struct " + n + r" \{.*?^\};", src)[0]
                  for n in ("t70_scan", "t70_accel"))
functions = "\n".join(re.search(r"(?ms)^static int " + n + r"\([^{}]*\n\{.*?^\}", src)[0]
                      for n in ("t70_sample", "t70_postenable", "t70_predisable", "t70_read_raw"))
with tempfile.TemporaryDirectory(prefix="t70-iio-consumer-") as tmp:
    c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    c.write_text(SHIM + types + "\n" + functions + TEST)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-O2", "-ftrivial-auto-var-init=pattern", "-fsanitize=undefined", "-o", exe, c], check=True)
    subprocess.run([exe], check=True)
print("IIO callbacks: XYZ, zero timestamp alignment gap, original acquisition clock and stop passed")
