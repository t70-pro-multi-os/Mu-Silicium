#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the staged host IPI transaction and registration lifecycle."""
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
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
#define IPI_OUT_SENSOR_CTRL 29
#define IPI_OUT_SENSOR_NOTIFY 31
#define PIN_OUT_SIZE_SENSOR_CTRL 16
#define PIN_OUT_SIZE_SENSOR_NOTIFY 7
#define MBOX_SLOT_SIZE 4
#define IPI_PIN_BUSY -5
#define ipi_len(x) (((x)+3)/4)
#define IPI_IN_SENSOR_CTRL 30
#define IPI_IN_SENSOR_NOTIFY 32
#define WQ_MEM_RECLAIM 1
#define WQ_HIGHPRI 2
#define INIT_WORK(w, f) ((void)(w))
#define INIT_LIST_HEAD(x) ((void)(x))
#define spin_lock_init(x) ((void)(x))
#define pr_err(...) ((void)0)
#define pr_err_ratelimited(...) ((void)0)
#define msecs_to_jiffies(x) (x)
static unsigned int locked, active, registered, unregistered, fault, waits, wrong_length;
static int scp_ipidev, hw_transfer_lock, queue;
static uint8_t ctrl_payload[8], notify_payload[28];
struct completion { unsigned int done; };
struct ipi_transfer { int id; const unsigned char *tx_buf; unsigned char *rx_buf; unsigned int tx_len, rx_len; };
struct ipi_hw_transfer {
    struct completion done;
    int count, id;
    const unsigned char *tx;
    unsigned char *rx;
    unsigned int tx_len, rx_len;
    void *context;
};
static struct ipi_hw_transfer hw_transfer;
static struct { int work, head, lock, *workqueue, accepting; } controller;
struct ipi_message { int status, list; };
static int scheduled, flushed;
static int __ipi_transfer(struct ipi_message *m);
#define spin_lock_irqsave(l, f) do { (f) = 0; spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); spin_unlock(l); } while (0)
static void spin_lock(int *lock) { assert(lock && !locked++); }
static void spin_unlock(int *lock) { assert(lock && locked-- == 1); }
#define spin_lock_irq(l) spin_lock(l)
#define spin_unlock_irq(l) spin_unlock(l)
static void list_add_tail(int *list, int *head)
{ assert(list && head == &controller.head && locked && controller.accepting); }
static void queue_work(int *w, int *work)
{ assert(w == &queue && work == &controller.work && locked); ++scheduled; }
static void flush_workqueue(int *w)
{
    struct ipi_message late = {0};
    assert(w == &queue && !locked && !controller.accepting);
    assert(__ipi_transfer(&late) == -ESHUTDOWN);
    ++flushed;
}
static void init_completion(struct completion *d) { d->done = 0; }
static void reinit_completion(struct completion *d) { init_completion(d); }
static void complete(struct completion *d) { assert(locked && !d->done++); }
static int *alloc_workqueue(const char *name, unsigned flags, int max)
{
    assert(name && flags && !max);
    if (fault == 1) return NULL;
    assert(!active++);
    return &queue;
}
static void destroy_workqueue(int *w) { assert(w == &queue && active-- == 1); }
static int ipi_comm_ctrl_handler(unsigned id, void *p, void *buf, unsigned size)
{ (void)id; (void)p; (void)buf; (void)size; return 0; }
#define ipi_comm_notify_handler ipi_comm_ctrl_handler
static int mtk_ipi_register(int *dev, unsigned id,
                            int (*cb)(unsigned, void *, void *, unsigned),
                            void *ctx, uint8_t *data)
{
    assert(dev == &scp_ipidev && cb && !ctx && data);
    registered++;
    return (fault == 2 && id == 30) || (fault == 3 && id == 32) ? -ENODEV : 0;
}
static void mtk_ipi_unregister(int *dev, unsigned id)
{ assert(dev == &scp_ipidev && (id == 30 || id == 32));
  if (!fault) assert(flushed == 1 && !controller.accepting);
  unregistered++; }
static unsigned sent_slots, tail_byte;
static int mtk_ipi_send(int *dev, int id, int a, void *tx, unsigned words, int b)
{
    unsigned char *wire = tx;
    assert(dev == &scp_ipidev && (id == 29 || id == 31) && !a && !b && words);
    sent_slots = words;
    tail_byte = wire[words * 4 - 1];
    return fault == 4 ? -ENODEV : 0;
}
static void usleep_range(int a, int b) { assert(a <= b); }
static void ipi_comm_complete(unsigned char *buffer, unsigned int len);
static int wait_for_completion_timeout(struct completion *d, int j)
{
    unsigned char bytes[8] = {1,2,3,4,5,6,7,8};
    assert(d == &hw_transfer.done && !d->done && j == 100);
    waits++;
    if (fault == 5) return 0;
    ipi_comm_complete(bytes, sizeof(bytes) - wrong_length);
    assert(d->done);
    return fault == 6 ? 0 : 1;
}
'''
TEST = r'''
int main(void)
{
    unsigned char tx[8] = {0}, rx[8] = {0};
    struct ipi_transfer t = {.id=29, .tx_buf=tx, .rx_buf=rx, .tx_len=8, .rx_len=8};
    fault = 1;
    assert(ipi_comm_init() == -ENOMEM && !active && !registered);
    fault = 2;
    assert(ipi_comm_init() == -ENODEV && !active && registered == 1 && !unregistered);
    fault = 3;
    assert(ipi_comm_init() == -ENODEV && !active && registered == 3 && unregistered == 1);
    fault = 0;
    assert(ipi_comm_init() == 0 && active && registered == 5);
    fault = 4;
    assert(ipi_transfer_buffer(&t) == -EIO && !hw_transfer.context && !waits);
    fault = 5;
    assert(ipi_transfer_buffer(&t) == -ETIMEDOUT && !hw_transfer.context && waits == 1);
    fault = 0;
    wrong_length = 1;
    assert(ipi_transfer_buffer(&t) == -EBADMSG && !hw_transfer.context && !rx[0]);
    wrong_length = 0;
    assert(ipi_transfer_buffer(&t) == 8 && !hw_transfer.context && rx[0] == 1 && rx[7] == 8);
    fault = 6; /* ACK acknowledged under lock as the timeout wake arrives. */
    assert(ipi_transfer_buffer(&t) == 8 && !hw_transfer.context);
    fault = 0;
    tx[4] = 0x7b; tx[7] = 0x55;
    assert(ipi_retry_transfer(31, tx, 5) == 0 && sent_slots == 2 && tail_byte == 0);
    assert(ipi_retry_transfer(31, tx, 29) == -EINVAL);
    assert(ipi_retry_transfer(29, tx, 65) == -EINVAL);
    assert(ipi_retry_transfer(29, NULL, 4) == -EINVAL);
    t.rx_buf = NULL;
    assert(ipi_transfer_buffer(&t) == -EINVAL);
    assert(!locked && waits == 4);
    struct ipi_message normal = {0};
    assert(!__ipi_transfer(&normal) && normal.status == -EINPROGRESS && scheduled == 1);
    ipi_comm_exit();
    assert(!active && !controller.workqueue && !controller.accepting);
    assert(scheduled == 1 && flushed == 1 && unregistered == 3);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("src", type=Path)
    a = p.parse_args()
    src = a.src.read_text()
    callbacks = "\n".join(
        re.search(r"(?ms)^(?:static )?(?:inline )?(?:int|void) " + name + r"\([^{}]*\n\{.*?^\}", src)[0]
        for name in ("ipi_retry_transfer", "ipi_transfer_buffer", "ipi_comm_complete", "ipi_comm_init",
                     "__ipi_transfer", "ipi_comm_exit"))
    with tempfile.TemporaryDirectory(prefix="t70-ipi-fixture-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(SHIM + callbacks + TEST)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("IPI allocation, both registration failures, send error, timeout, and ACK validation passed")


if __name__ == "__main__":
    main()
