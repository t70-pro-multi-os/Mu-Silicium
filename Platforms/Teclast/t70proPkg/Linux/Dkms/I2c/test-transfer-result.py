#!/usr/bin/env python3
"""Test the actual staged I2C completion classifier without a device."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
fn = re.search(r"^static int t70_i2c_transfer_result\([^;{}]*\n\{.*?^\}", src, re.M | re.S)
assert fn
body = src.split("\nstatic int mtk_i2c_do_transfer(", 1)[1].split("\n}\n", 1)[0]
assert body.count("i2c_put_dma_safe_msg_buf(dma_rd_buf, msgs, result == 0);") == 1
assert body.count("i2c_put_dma_safe_msg_buf(dma_rd_buf, (msgs + 1), result == 0);") == 1
assert body.index("if (result)\n\t\treturn result;") < body.index("while (data_size--) {\n\t\t\t*ptr")
test = r'''
#include <assert.h>
#include <errno.h>
typedef unsigned short u16;
#define I2C_ARB_LOST 1
#define I2C_HS_NACKERR 2
#define I2C_ACKERR 4
#define I2C_TRANSAC_COMP 8
'''
runner = r'''
int main(void)
{
    assert(t70_i2c_transfer_result(0, 8, 16) == -ETIMEDOUT);
    assert(t70_i2c_transfer_result(1, 1, 16) == -EAGAIN);
    assert(t70_i2c_transfer_result(1, 1 | 8, 16) == -EAGAIN);
    assert(t70_i2c_transfer_result(1, 2 | 8, 16) == -ENXIO);
    assert(t70_i2c_transfer_result(1, 4 | 16, 16) == -ENXIO);
    assert(t70_i2c_transfer_result(1, 8, 16) == 0);
    assert(t70_i2c_transfer_result(1, 16, 16) == 0);
    assert(t70_i2c_transfer_result(1, 16, 0) == -EIO);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-i2c-result-") as tmp:
    c = Path(tmp) / "outcome.c"
    c.write_text(test + fn[0] + runner)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("I2C: timeout, arbitration loss, NACK, completion/restart and read-copy guards passed")
