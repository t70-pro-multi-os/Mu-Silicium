#!/usr/bin/env python3
"""Check the generated BTIF RX worker's idle stop, retry, and pending drain."""
from pathlib import Path
import subprocess
import sys

src = Path(sys.argv[1]).read_text()
a = src.index("\nstatic int btif_rx_thread(void *p_data)\n{")
b = src.index("\nstatic void btif_rx_worker(", a)
assert "wait_for_completion_interruptible" not in src[a:b]
assert "i_ret = PTR_ERR(p_btif->p_task);" in src
assert "kthread_stop(p_btif->p_task);\n\t\t\tp_btif->p_task = NULL;" in src

test = Path(sys.argv[2])
test.write_text(r'''
#include <assert.h>
#define BTIF_RXD_BE_BLOCKED_DETECT 0
#define BTIF_ERR_FUNC(...) ((void)0)
#define BTIF_WARN_FUNC(...) ((void)0)
struct _mtk_btif_ { int rx_wait, rx_pending, rx_thread_mtx; };
static int waits, interrupted, stop, callbacks, locks, scenario;
static int kthread_should_stop(void) { return stop; }
static int atomic_read(int *p) { return *p; }
static void atomic_xchg(int *p, int value) { *p = value; }
static int mutex_lock_killable(int *p)
{
    assert(!*p && !stop);
    ++locks;
    return ++*p != 1;
}
static void mutex_unlock(int *p) { assert((*p)-- == 1); }
static void btif_rx_data_consummer(struct _mtk_btif_ *p)
{
    assert(p->rx_thread_mtx == 1 && !p->rx_pending);
    ++callbacks;
}
static int wait_step(void)
{
    ++waits;
    if (scenario == 1 && waits == 1)
        return ++interrupted;
    if (scenario != 2 || waits > 1)
        stop = 1;
    return 0;
}
#define wait_event_interruptible(queue, condition) \
    ({ int rc = wait_step(); if (!rc) assert(condition); rc; })
''' + src[a:b] + r'''
int main(void)
{
    for (scenario = 0; scenario < 3; ++scenario) {
        struct _mtk_btif_ state = { .rx_pending = scenario == 2 };
        waits = interrupted = stop = callbacks = locks = 0;
        assert(!btif_rx_thread(&state));
        assert(stop && !state.rx_thread_mtx);
        assert(interrupted == (scenario == 1));
        assert(callbacks == (scenario == 2) && locks == callbacks);
        assert(waits == (scenario == 0 ? 1 : 2));
    }
    return 0;
}
''')
exe = test.with_suffix("")
subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                str(test), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=3)
print("BTIF RX idle stop, interrupted wait, and one pending drain passed")
