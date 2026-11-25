#!/usr/bin/env python3
"""Check the staged sample translator without Android vendor-only diagnostics."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
assert not re.search(r"awinic_get_global_val|custom_cmd_(init|exit|comm_with)|debug_(init|exit|get_debug)", src)
assert "dev->hf_dev.rawdata = transceiver_rawdata;" in src
assert "dev->hf_dev.selftest = transceiver_selftest;" in src
translate = re.search(r"^static int transceiver_translate\([^;{}]*\n\{.*?^\}", src, re.M | re.S)
assert translate
sensor_ids = sorted(set(re.findall(r"case (SENSOR_TYPE_\w+):", translate[0])) | {"SENSOR_TYPE_SAR"})
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#define min(a,b) ((a)<(b)?(a):(b))
#define pr_err_ratelimited(...) ((void)0)
enum { DATA_ACTION, FLUSH_ACTION, RAW_ACTION, MAX_ACTION };
struct transceiver_device { int filter; };
struct hf_manager_event { int64_t timestamp; uint8_t sensor_type, accurancy, action; int32_t word[6]; };
struct share_mem_data { int64_t timestamp; uint8_t sensor_type, accurancy, action; int32_t value[6]; };
static int64_t timesync_filter_get(int *p) { return *p; }
'''
test = r'''
int main(void)
{
    struct transceiver_device d = {23};
    struct share_mem_data in = {
        .timestamp=50, .sensor_type=SENSOR_TYPE_SAR, .accurancy=2,
        .action=DATA_ACTION, .value={0xff, -17, 31, 37, 41, 43}
    };
    struct hf_manager_event out = {0};
    assert(!transceiver_translate(&d, &out, &in) && out.timestamp == 73);
    assert(out.sensor_type == SENSOR_TYPE_SAR && out.accurancy == 2);
    assert(!memcmp(out.word, in.value, sizeof(in.value)));
    in.action = RAW_ACTION; in.value[0] = -53;
    assert(!transceiver_translate(&d, &out, &in) && out.word[0] == -53 && out.action == RAW_ACTION);
    in.action = DATA_ACTION; in.sensor_type = SENSOR_TYPE_ACCELEROMETER;
    assert(!transceiver_translate(&d, &out, &in) && out.word[2] == 31);
    in.sensor_type = SENSOR_TYPE_SENSOR_MAX;
    assert(transceiver_translate(&d, &out, &in) == -EINVAL);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-generic-samples-") as tmp:
    c = Path(tmp) / "test.c"
    c.write_text(prefix + "enum {" + ",".join(sensor_ids) + ",SENSOR_TYPE_SENSOR_MAX};\n" + translate[0] + test)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("Generic sensor samples: SAR data/raw, accel and timestamps pass without vendor diagnostics")
