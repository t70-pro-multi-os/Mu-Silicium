#!/usr/bin/env python3
"""Verify exact staged SD completion status for ordinary controller error flags."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]).read_text()
helper = re.search(r"^static int t70_msdc_completion_status\([^;{}]*\n\{.*?^\}", src, re.M | re.S)
assert helper
body = src.split("\nstatic void msdc_data_xfer_done(", 1)[1].split("\n}\n", 1)[0]
assert "dma_timeout = ret != 0;" in body and "dma_timeout |= ret != 0;" in body
assert body.index("t70_msdc_completion_status(") < body.index("data->bytes_xfered = data->blocks")
assert "data->bytes_xfered = 0;" in body and "data->error = ret;" in body
flags = "\n".join(line.split("/*", 1)[0] for line in src.splitlines()
    if re.match(r"#define MSDC_INT_(XFER_COMPL|DATCRCERR|DATTMO|DMA_BDCSERR|DMA_GPDCSERR|DMA_PROTECT)\s", line))
prefix = "#include <errno.h>\n#include <stdbool.h>\n#include <assert.h>\ntypedef unsigned u32;\n#define BIT(n) (1U << (n))\n"
test = r'''
int main(void)
{
    u32 ok = MSDC_INT_XFER_COMPL;
    assert(t70_msdc_completion_status(ok, 0, false) == 0);
    assert(t70_msdc_completion_status(ok, 0, true) == -ETIMEDOUT);
    assert(t70_msdc_completion_status(ok | MSDC_INT_DATTMO, 0, false) == -ETIMEDOUT);
    assert(t70_msdc_completion_status(ok | MSDC_INT_DATCRCERR, 0, false) == -EILSEQ);
    assert(t70_msdc_completion_status(ok | MSDC_INT_DMA_BDCSERR, 0, false) == -EIO);
    assert(t70_msdc_completion_status(ok | MSDC_INT_DMA_GPDCSERR, 0, false) == -EIO);
    assert(t70_msdc_completion_status(ok | MSDC_INT_DMA_PROTECT, 0, false) == -EIO);
    assert(t70_msdc_completion_status(ok, -EILSEQ, false) == -EILSEQ);
    assert(t70_msdc_completion_status(0, 0, false) == -EIO);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t70-sd-completion-") as tmp:
    c = Path(tmp) / "test.c"
    c.write_text(prefix + flags + "\n" + helper[0] + test)
    exe = c.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=3)
print("SD completion: success, CRC, DMA faults, both stop failures and zero-event checks passed")
