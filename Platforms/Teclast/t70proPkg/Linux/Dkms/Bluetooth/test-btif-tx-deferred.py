#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the staged BTIF worker under no-progress, short-send, and stop."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct fifo { unsigned char data[8]; unsigned int used; };
struct _mtk_btif_ {
    struct delayed_work tx_work;
    bool tx_shutdown;
    int tx_mtx, tx_fifo_producer_lock;
    struct fifo *p_tx_fifo;
    void *p_tx_wq;
};
static int calls, outcome, deferred, lock_count;
static unsigned long last_delay;
#define to_delayed_work(p) ((struct delayed_work *)(p))
#define container_of(p, t, f) ((t *)((char *)(p) - offsetof(t, f)))
#define READ_ONCE(v) (v)
#define msecs_to_jiffies(v) (v)
#define spin_lock_irqsave(p, f) ((f)=0, mutex_lock(p))
#define spin_unlock_irqrestore(p, f) ((void)(f), mutex_unlock(p))
static void mutex_lock(int *p) { assert(!*p && !lock_count); *p = ++lock_count; }
static void mutex_unlock(int *p) { assert(*p == 1 && lock_count == 1); *p = --lock_count; }
static unsigned kfifo_out_peek(struct fifo *f, unsigned char *b, size_t max)
{ assert(f->used <= max); memcpy(b, f->data, f->used); return f->used; }
static void kfifo_skip_count(struct fifo *f, unsigned n)
{ assert(n <= f->used); f->used -= n; memmove(f->data, f->data + n, f->used); }
static void kfifo_reset_out(struct fifo *f) { f->used = 0; }
static bool kfifo_is_empty(struct fifo *f) { return !f->used; }
static int _btif_send_data(struct _mtk_btif_ *d, const unsigned char *buf, unsigned count)
{
    assert(d->tx_mtx && count && buf[0] == (count == 4 ? 0x11 : 0x33));
    ++calls;
    if (outcome == 2 && calls == 2) return 2;
    if (outcome == 2) return 0;
    return outcome ? (int)count : 0;
}
static void queue_delayed_work(void *q, struct delayed_work *w, unsigned long delay)
{ assert(q && w && lock_count); ++deferred; last_delay = delay; }
'''
TEST = r'''
int main(void)
{
    struct fifo f = {.data={0x11,0x22,0x33,0x44}, .used=4};
    struct _mtk_btif_ d = {.p_tx_fifo=&f,.p_tx_wq=&f};
    btif_tx_worker(&d.tx_work.work);
    assert(calls == 1 && f.used == 4 && deferred == 1 && last_delay == 10);
    outcome = 2;
    btif_tx_worker(&d.tx_work.work);
    assert(calls == 3 && f.used == 2 && f.data[0] == 0x33 &&
           deferred == 2 && last_delay == 10);
    d.tx_shutdown = true;
    btif_tx_worker(&d.tx_work.work);
    assert(calls == 3 && f.used == 2 && deferred == 2);
    d.tx_shutdown = false;
    outcome = 1;
    btif_tx_worker(&d.tx_work.work);
    assert(calls == 4 && !f.used && deferred == 2);
}
'''
if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    src = p.parse_args().source.read_text()
    body = re.search(r"(?ms)^static void btif_tx_worker\([^{};]*\n\{.*?^\}", src)
    if not body:
        p.error("missing BTIF TX worker")
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "test.c"
        exe = c.with_suffix("")
        c.write_text(SHIM + body[0] + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-sign-compare", "-fsanitize=undefined", str(c), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    print("BTIF worker: zero-progress defers, partial suffix retained, shutdown does not reschedule")
