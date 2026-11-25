#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the staged modem-state reader never invents READY."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <stddef.h>
#define MD_SYS1 0
#define READ_ONCE(v) (v)
struct ccci_fsm_ctl { int md_id; enum MD_STATE md_state; };
static struct ccci_fsm_ctl ctrl, *registered;
static int lookups;
static struct ccci_fsm_ctl *fsm_get_entity_by_md_id(int md)
{ assert(md == MD_SYS1); ++lookups; return registered; }
'''
TEST = r'''
int main(void)
{
    assert(ccci_fsm_get_md_state(-1) == INVALID);
    assert(ccci_fsm_get_md_state(1) == INVALID && !lookups);
    assert(ccci_fsm_get_md_state(MD_SYS1) == INVALID && lookups == 1);
    registered = &ctrl;
    ctrl.md_id = 1;
    ctrl.md_state = READY;
    assert(ccci_fsm_get_md_state(MD_SYS1) == INVALID);
    ctrl.md_id = MD_SYS1;
    assert(ccci_fsm_get_md_state(MD_SYS1) == READY);
    for (int s = INVALID; s <= WAITING_TO_STOP; ++s) {
        ctrl.md_state = s;
        assert(ccci_fsm_get_md_state(MD_SYS1) == (enum MD_STATE)s);
    }
    ctrl.md_state = 255;
    assert(ccci_fsm_get_md_state(MD_SYS1) == INVALID);
}
'''

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("stage", type=Path)
    d = p.parse_args().stage
    hdr = (d / "mt-plat/mtk_ccci_common.h").read_text()
    src = (d / "ccci/fsm/ccci_fsm_tx_state.c").read_text()
    states = re.search(r"(?ms)^enum MD_STATE \{.*?^\};", hdr)
    getter = re.search(r"(?ms)^enum MD_STATE ccci_fsm_get_md_state\([^{}]*\n\{.*?^\}", src)
    if not states or not getter:
        p.error("missing genuine FSM enum or staged reader")
    with tempfile.TemporaryDirectory() as t:
        c = Path(t) / "test.c"
        exe = c.with_suffix("")
        c.write_text(states[0] + SHIM + getter[0] + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("CCCI FSM: only owner can supply READY; invalid owner/state rejects, reset preserved")
