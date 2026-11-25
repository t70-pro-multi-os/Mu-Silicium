#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the actual staged probe prefix with mocked driver-core and owner."""
import argparse
from pathlib import Path
import subprocess
import tempfile

BEFORE = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define EPROBE_DEFER 517
#define GFP_KERNEL 0
#define SKIP_SB_SMC 1
#define CONFIG_FPGA_EARLY_PORTING 1
#define pr_info(...) do {} while (0)
#define DMA_BIT_MASK(n) ((1ULL << (n)) - 1)
struct device { int unused; };
struct platform_device { struct device dev; };
struct mtk_base_afe { int unused; };
static int claims, releases, refs, masks, entries, fail_alloc, nalloc;
static int claim_error, action_error, mask_error;
static bool available;
static const char *reason;
static void (*release_action)(void *);
static void *action_data, *allocations[2];
static void *devm_kzalloc(struct device *dev, size_t n, int flag)
{
    (void)dev; (void)flag;
    assert(nalloc < 2);
    if (fail_alloc == nalloc + 1)
        return NULL;
    return allocations[nalloc++] = calloc(1, n);
}
#define symbol_get(name) (available ? (refs++, &name) : NULL)
#define symbol_put(name) do { assert(refs == 1); refs--; } while (0)
static int dev_err_probe(struct device *dev, int err, const char *fmt, ...)
{ (void)dev; reason = fmt; return err; }
static int devm_add_action_or_reset(struct device *dev, void (*f)(void *), void *p)
{
    (void)dev;
    if (action_error) {
        f(p);
        return action_error;
    }
    release_action = f; action_data = p; return 0;
}
static int dma_set_mask_and_coherent(struct device *dev, unsigned long long mask)
{
    (void)dev;
    assert(claims == 1 && refs == 1 && mask == DMA_BIT_MASK(34));
    masks++; return mask_error;
}
"""

AFTER = r"""
int t70_mt6789_audio_owner_v1(struct device *dev, struct mtk_base_afe *afe,
                             enum t70_audio_owner_op op)
{
    assert(dev && afe && refs == 1);
    if (op == T70_AUDIO_CLAIM) {
        claims++; return claim_error;
    }
    assert(op == T70_AUDIO_RELEASE);
    releases++; return 0;
}

static void cleanup(void)
{
    if (release_action) release_action(action_data);
    release_action = NULL;
    for (int i = 0; i < nalloc; i++) free(allocations[i]);
    claims = releases = refs = masks = entries = nalloc = 0;
    reason = NULL;
}

static int driver_core_fixture(bool links_ready)
{
    static struct platform_device pdev;
    if (!links_ready)
        return -EPROBE_DEFER;
    entries++;
    return mt6789_afe_pcm_dev_probe(&pdev);
}

int main(void)
{
    assert(driver_core_fixture(false) == -EPROBE_DEFER);
    assert(!entries && !reason && !masks);
    assert(driver_core_fixture(true) == -EPROBE_DEFER);
    assert(entries == 1 && reason && strstr(reason, "owner not available"));
    assert(!claims && !refs && !masks);
    cleanup();

    available = true; claim_error = -EBUSY;
    assert(driver_core_fixture(true) == -EBUSY);
    assert(claims == 1 && !releases && !refs && !masks);
    cleanup();

    claim_error = 0; action_error = -ENOMEM;
    assert(driver_core_fixture(true) == -ENOMEM);
    assert(claims == 1 && releases == 1 && !refs && !masks);
    cleanup();

    action_error = 0; mask_error = -EIO;
    assert(driver_core_fixture(true) == -EIO);
    assert(claims == 1 && refs == 1 && masks == 1 && release_action);
    release_action(action_data); release_action = NULL;
    assert(releases == 1 && !refs);
    cleanup();

    mask_error = 0; available = false;
    assert(driver_core_fixture(true) == -EPROBE_DEFER && !masks);
    cleanup();
    available = true;
    assert(driver_core_fixture(true) == 0);
    assert(entries == 1 && claims == 1 && masks == 1 && refs == 1);
    release_action(action_data); release_action = NULL;
    assert(releases == 1 && !refs);
    cleanup();

    fail_alloc = 2;
    assert(driver_core_fixture(true) == -ENOMEM);
    assert(!claims && !masks && !refs);
    cleanup();
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("staged_pcm_c", type=Path)
    args = parser.parse_args()
    text = args.staged_pcm_c.read_text()
    begin = text.index("enum t70_audio_owner_op {")
    end = text.index("\n\tplatform_set_drvdata(pdev, afe);", begin)
    prefix = text[begin:end]
    assert prefix.count("dma_set_mask_and_coherent") == 1
    with tempfile.TemporaryDirectory(prefix="t70-audio-owner-test-") as name:
        tmp = Path(name)
        c, exe = tmp / "owner-test.c", tmp / "owner-test"
        c.write_text(BEFORE + prefix + "\n\treturn 0;\n}\n" + AFTER)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Wno-unused-variable", "-Werror",
                        "-fsanitize=undefined", "-o", exe, c], check=True)
        subprocess.run([exe], check=True)
    print("PASS: core vs driver deferral, claim/busy, action/mask errors, guarded retry, allocation failure")


if __name__ == "__main__":
    main()
