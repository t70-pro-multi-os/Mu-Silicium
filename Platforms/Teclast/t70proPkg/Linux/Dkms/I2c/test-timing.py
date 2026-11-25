#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the generated timing selection in isolation, without I2C devices."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
start = src.index("static int mtk_i2c_set_speed(")
end = src.index("\nstatic void mtk_i2c_dump_reg(", start)
function = src[start:end]
test = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#define MAX_CLOCK_DIV 256
#define I2C_MAX_FAST_MODE_PLUS_FREQ 1000000U
#define I2C_MAX_FAST_MODE_FREQ 400000U
#define I2C_TIME_DEFAULT_VALUE 3
#define I2C_TIME_CLR_VALUE 0
struct caps { int timing_adjust, ltiming_adjust; };
struct ac { unsigned int inter_clk_div; };
struct mtk_i2c {
  unsigned int speed_hz, clk_src_div, timing_reg, high_speed_reg, ltiming_reg;
  const struct caps *dev_comp;
  struct ac ac_timing;
};
static int failures;
static int calls;
static int mtk_i2c_calculate_speed(struct mtk_i2c *i2c, unsigned int src,
                                  unsigned int speed, unsigned int *step,
                                  unsigned int *sample) {
  (void)i2c;
  assert(src >= 2 && speed);
  ++calls;
  if (failures) return -EINVAL;
  *step = 2;
  *sample = 3;
  return 0;
}
"""
test += function
test += r"""
int main(int argc, char **argv) {
  (void)argv;
  struct caps c = {1, 1};
  struct mtk_i2c i = {.speed_hz = 400000, .clk_src_div = 1, .dev_comp = &c};
  i.ac_timing.inter_clk_div = 0x1234;
  failures = 1;
  int ret = mtk_i2c_set_speed(&i, 26000000);
  if (argc > 1) return ret == -EINVAL ? 0 : 1;
  assert(ret == -EINVAL && i.ac_timing.inter_clk_div == 0x1234);
  failures = calls = 0;
  assert(mtk_i2c_set_speed(&i, 0) == -EINVAL);
  unsigned long too_large = (unsigned long)UINT32_MAX + 26000000U;
  assert(mtk_i2c_set_speed(&i, too_large) == -EINVAL);
  i.clk_src_div = 0;
  assert(mtk_i2c_set_speed(&i, 26000000) == -EINVAL);
  i.clk_src_div = 3;
  assert(mtk_i2c_set_speed(&i, 4) == -EINVAL);
  i.clk_src_div = 1;
  i.speed_hz = 0;
  assert(mtk_i2c_set_speed(&i, 26000000) == -EINVAL);
  assert(calls == 0);
  i.speed_hz = 400000;
  assert(mtk_i2c_set_speed(&i, 26000000) == 0 && calls == 1);
  assert(i.timing_reg == 0x302 && i.ac_timing.inter_clk_div == 0);
  return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="t70-i2c-timing-") as d:
    source, binary = Path(d) / "test.c", Path(d) / "test"
    source.write_text(test)
    subprocess.run([os.environ.get("CC", "gcc"), "-Wall", "-Wextra", "-Werror", "-O1",
                    "-fsanitize=address,undefined", source, "-o", binary], check=True)
    subprocess.run([binary, *sys.argv[2:]], check=True)
    print("T70 I2C timing selection checks passed")
