#!/usr/bin/env python3
"""Exercise the exact staged channel lookup against sparse mocked net devices."""
from pathlib import Path
import subprocess
import sys

source = Path(sys.argv[1]).read_text()
start = source.index("\nstatic struct ccmni_instance *ccmni_callback_instance(")
end = source.index("\nstatic void ccmni_queue_state_callback(", start)
for callback in ("ccmni_queue_state_callback", "ccmni_md_state_callback",
                 "ccmni_dump(", "ccmni_get_ch("):
    body = source.split(callback, 1)[1].split("\n}\n", 1)[0]
    assert body.count("ccmni_callback_instance(") == 1 and "ccmni_inst[ccmni_idx]" not in body
buf = Path(sys.argv[2])
buf.write_text(r'''
#include <stddef.h>
#include <assert.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
struct ccmni_instance { void *dev; };
#define MODEM_CAP_DIRECT_TETHERING 2
struct ops { int ccmni_num; unsigned int md_ability; };
struct ccmni_ctl_block { struct ccmni_instance *ccmni_inst[22]; struct ops *ccci_ops; };
''' + source[start:end] + r'''
int main(void)
{
    int dev;
    struct ccmni_instance open = { &dev }, closed = { NULL };
    struct ops ops = { .ccmni_num = 21 };
    struct ccmni_ctl_block block = { .ccmni_inst = {[0] = &open,
        [3] = &closed, [21] = &open}, .ccci_ops = &ops };
    assert(!ccmni_callback_instance(NULL, 0));
    assert(!ccmni_callback_instance(&block, -1));
    assert(!ccmni_callback_instance(&block, 22));
    assert(!ccmni_callback_instance(&block, 1));
    assert(!ccmni_callback_instance(&block, 3));
    assert(ccmni_callback_instance(&block, 0) == &open);
    assert(!ccmni_callback_instance(&block, 21));
    ops.md_ability = MODEM_CAP_DIRECT_TETHERING;
    assert(ccmni_callback_instance(&block, 21) == &open);
    ops.ccmni_num = 20;
    assert(!ccmni_callback_instance(&block, 21));
    assert(!ccmni_callback_instance(&block, 20));
    block.ccci_ops = NULL;
    assert(!ccmni_callback_instance(&block, 0));
    return 0;
}
''')
exe = buf.with_suffix("")
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-sign-compare", str(buf), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
print("CCMNI callback bounds and sparse channels passed")
