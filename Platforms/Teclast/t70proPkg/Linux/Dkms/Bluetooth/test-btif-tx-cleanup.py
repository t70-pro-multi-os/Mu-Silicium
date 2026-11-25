#!/usr/bin/env python3
"""Compile the exact staged TX init/deinit against software-only allocation shims."""
import argparse
from pathlib import Path
import signal
import subprocess


SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#define BTIF_TX_SINGLE_CTX 1
#define BTIF_TX_USER_CTX 2
#define E_BTIF_INVAL_PARAM (-EINVAL)
#define GFP_ATOMIC 0
#define BTIF_TX_BUFFER_FIFO_SIZE 1024
#define BTIF_ERR_FUNC(...) ((void)0)
#define BTIF_INFO_FUNC(...) ((void)0)
#define INIT_WORK(...) ((void)0)
#define mutex_init(...) ((void)0)
struct kfifo { int unused; };
struct _mtk_btif_ { int tx_ctx; void *p_tx_wq; struct kfifo *p_tx_fifo; };
static struct kfifo descriptor;
static int queue, alloc_fault, queue_live, desc_live, buffer_live;
static void *create_singlethread_workqueue(const char *name)
{
    (void)name;
    if (alloc_fault == 1) return NULL;
    assert(!queue_live++);
    return &queue;
}
static void destroy_workqueue(void *ptr)
{
    assert(ptr == &queue && queue_live-- == 1);
}
static struct kfifo *kzalloc(size_t size, int flags)
{
    (void)flags;
    assert(size == sizeof(descriptor));
    if (alloc_fault == 2) return NULL;
    assert(!desc_live++);
    return &descriptor;
}
static int kfifo_alloc(struct kfifo *ptr, int size, int flags)
{
    (void)flags;
    assert(ptr == &descriptor && size == BTIF_TX_BUFFER_FIFO_SIZE);
    if (alloc_fault == 3) return -ENOMEM;
    assert(!buffer_live++);
    return 0;
}
static void kfifo_free(struct kfifo *ptr)
{
    assert(ptr == &descriptor && desc_live == 1 && buffer_live-- == 1);
}
static void kfree(struct kfifo *ptr)
{
    if (ptr) assert(ptr == &descriptor && desc_live-- == 1 && !buffer_live);
}
'''
TEST = r'''
int main(void)
{
    struct _mtk_btif_ ctx = { .tx_ctx = BTIF_TX_SINGLE_CTX };
    for (alloc_fault = 1; alloc_fault < 4; alloc_fault++) {
        assert(_btif_tx_ctx_init(&ctx) == -ENOMEM);
        assert(!queue_live && !desc_live && !buffer_live);
        assert(_btif_tx_ctx_deinit(&ctx) == 0);
        assert(!ctx.p_tx_fifo && !ctx.p_tx_wq);
    }
    alloc_fault = 0;
    assert(_btif_tx_ctx_init(&ctx) == 0);
    assert(queue_live && desc_live && buffer_live);
    assert(_btif_tx_ctx_deinit(&ctx) == 0);
    assert(_btif_tx_ctx_deinit(&ctx) == 0);
    assert(!queue_live && !desc_live && !buffer_live);
    ctx.tx_ctx = BTIF_TX_USER_CTX;
    assert(_btif_tx_ctx_init(&ctx) == 0);
    assert(_btif_tx_ctx_deinit(&ctx) == 0);
    assert(!queue_live && !desc_live && !buffer_live);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("staged_source", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--expect-unfixed", action="store_true")
    a = p.parse_args()
    source = a.staged_source.read_text()
    start = source.index("\nstatic int _btif_tx_ctx_init(struct _mtk_btif_ *p_btif)\n{")
    end = source.index("\nstatic int _btif_rx_btm_init(", start)
    if a.expect_unfixed:
        exit_start = source.index("\nstatic void BTIF_exit(void)\n{")
        exit_end = source.index("\nint mtk_btif_hal_get_log_lvl(", exit_start)
        assert "_btif_tx_ctx_deinit(" not in source[exit_start:exit_end]
    else:
        stop_start = source.index("\nstatic void btif_device_stop(void)\n{")
        stop_end = source.index("\nstatic int BTIF_init(void)\n", stop_start)
        stop = source[stop_start:stop_end]
        cleanup = "_btif_tx_ctx_deinit(port);"
        assert stop.count(cleanup) == 1
        assert stop.index("btif_close(port);") < stop.index(cleanup)
        assert stop.index(cleanup) < stop.index("btif_free_vfifos(")
    c = a.output.with_suffix(".c")
    c.write_text(SHIM + source[start:end] + TEST)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(c), "-o", str(a.output)], check=True)
    result = subprocess.run([str(a.output)], capture_output=True)
    if a.expect_unfixed:
        assert result.returncode == -signal.SIGABRT and b"desc_live == 1" in result.stderr, result
        print("Original staged source leaves a freed descriptor for caller teardown")
    else:
        result.check_returncode()
        print("TX allocation faults (3), repeat teardown, and user TX mode passed")


if __name__ == "__main__":
    main()
