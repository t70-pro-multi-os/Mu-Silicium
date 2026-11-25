#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the staged SD DMA admission with bounded, in-memory DMA shims."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64;
#define BIT_ULL(n) (1ULL << (n))
#define BDMA_DESC_BUFLEN 0xffff
#define BDMA_DESC_BUFLEN_EXT 0xffffff
#define MAX_BD_NUM 1024
#define MSDC_PREPARE_FLAG 1
#define MSDC_ASYNC_FLAG 2
struct scatterlist { u64 addr; unsigned len; };
struct mmc_data { struct scatterlist *sg; int sg_count, sg_len;
                  unsigned blksz, blocks, host_cookie; };
struct mtk_mmc_compatible { bool support_64g; };
struct msdc_host { void *dev; struct mtk_mmc_compatible *dev_comp; };
#define sg_dma_address(s) ((s)->addr)
#define sg_dma_len(s) ((s)->len)
#define for_each_sg(root, sg, count, i) \
    for ((i)=0; (i)<(unsigned)(count) && ((sg)=&(root)[i]); ++(i))
static int result, maps, unmaps;
static int mmc_get_dma_dir(struct mmc_data *data) { assert(data); return 1; }
static int dma_map_sg(void *dev, struct scatterlist *sg, int count, int dir)
{ assert(dev && sg && count > 0 && dir == 1); ++maps; return result; }
static void dma_unmap_sg(void *dev, struct scatterlist *sg, int count, int dir)
{ assert(dev && sg && count > 0 && dir == 1); ++unmaps; }
'''
TEST = r'''
int main(void)
{
    struct mtk_mmc_compatible ops = { .support_64g = false };
    struct msdc_host host = { .dev = &ops, .dev_comp = &ops };
    struct scatterlist sg[2] = {{0x1000, 512}, {0xfffffe00, 512}};
    struct mmc_data d = {.sg=sg, .sg_len=2, .blksz=512, .blocks=2};
    msdc_prepare_data(&host, &d);    /* DMA API returned failure; no unmap. */
    assert(!d.host_cookie && !d.sg_count && maps == 1 && !unmaps);
    result = 3;                    /* Impossible completion count. */
    msdc_prepare_data(&host, &d);
    assert(!d.host_cookie && !d.sg_count && maps == 2 && unmaps == 1);
    result = 2;
    sg[1].addr++;
    msdc_prepare_data(&host, &d);
    assert(!d.host_cookie && !d.sg_count && unmaps == 2);
    sg[1].addr--;
    sg[1].len--;
    msdc_prepare_data(&host, &d);
    assert(!d.host_cookie && !d.sg_count && unmaps == 3);
    sg[1].len++;
    msdc_prepare_data(&host, &d);
    assert(d.host_cookie == MSDC_PREPARE_FLAG && d.sg_count == 2 && maps == 5);
    msdc_prepare_data(&host, &d);
    assert(maps == 5 && unmaps == 3); /* Async-prepared request not mapped again. */
    d.host_cookie = MSDC_ASYNC_FLAG;
    ops.support_64g = true;
    sg[1].addr = (1ULL << 36) - 511;
    msdc_prepare_data(&host, &d);
    assert(d.host_cookie == MSDC_ASYNC_FLAG && !d.sg_count && unmaps == 4);
    sg[1].addr--;  /* Last address inside 36-bit aperture. */
    msdc_prepare_data(&host, &d);
    assert(d.host_cookie == (MSDC_PREPARE_FLAG | MSDC_ASYNC_FLAG) && d.sg_count == 2);
    d.host_cookie = 0;
    result = 1;
    sg[0].len = BDMA_DESC_BUFLEN_EXT + 1;
    d.blksz = sg[0].len; d.blocks = 1;
    msdc_prepare_data(&host, &d);
    assert(!d.host_cookie && !d.sg_count && maps == 8 && unmaps == 5);
}
'''


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    source = p.parse_args().source.read_text()
    functions = []
    for decl in ("static bool t70_msdc_dma_map_valid", "static void msdc_prepare_data"):
        hit = re.search(r"(?ms)^" + decl + r"\([^{}]*\n\{.*?^\}", source)
        if not hit:
            p.error(f"staged function missing: {decl}")
        functions.append(hit[0])
    with tempfile.TemporaryDirectory() as dirname:
        c = Path(dirname) / "test.c"
        out = c.with_suffix("")
        c.write_text(SHIM + "\n".join(functions) + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", str(c), "-o", str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print("SD DMA: zero map, malformed count/length, 32/36-bit aperture and async ownership pass")
