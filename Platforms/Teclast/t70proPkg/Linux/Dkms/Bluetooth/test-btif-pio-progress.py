#!/usr/bin/env python3
"""Compile the stock or staged direct PIO callback with a scripted HAL."""
import argparse
from pathlib import Path
import subprocess


SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
#define BTIF_DBG_FUNC(...) ((void)0)
#define BTIF_INFO_FUNC(...) ((void)0)
struct _MTK_BTIF_INFO_STR_ { int unused; };
struct _mtk_btif_ { struct _MTK_BTIF_INFO_STR_ *p_btif_info; };
static const unsigned char input[] = { 0x11, 0x22, 0x33, 0x44 };
static unsigned char received[sizeof(input)];
static unsigned int offset, calls, response_count;
static const int *responses;
static int hal_btif_is_tx_allow(struct _MTK_BTIF_INFO_STR_ *unused)
{
    (void)unused;
    return 1;
}
static int hal_btif_send_data(struct _MTK_BTIF_INFO_STR_ *unused,
                              const unsigned char *p, unsigned int n)
{
    int result = calls < response_count ? responses[calls] : 0;
    (void)unused;
    calls++;
    assert(offset <= sizeof(input) && n <= sizeof(input) - offset);
    assert(p == input + offset);
    if (result > 0 && (unsigned int)result <= n) {
        memcpy(received + offset, p, result);
        offset += result;
    }
    return result;
}
'''
TEST = r'''
static int run(const int *script, unsigned int count, unsigned int start)
{
    struct _mtk_btif_ btif = {0};
    calls = 0;
    offset = start;
    responses = script;
    response_count = count;
    return _btif_pio_write(&btif, input + start, sizeof(input) - start);
}
int main(void)
{
    const int error[] = {-EIO};
    const int partial_error[] = {2, -EIO};
    const int partial[] = {2};
    const int resumed[] = {0, 2};
    const int overreport[] = {5};
#ifdef EXPECT_UNFIXED
    (void)error; (void)partial; (void)resumed; (void)overreport;
    return run(partial_error, 2, 0) == 2 && calls == 2 ? 0 : 55;
#else
    assert(run(error, 1, 0) == -EIO && calls == 1);
    assert(run(partial_error, 2, 0) == 2 && calls == 2);
    assert(run(NULL, 0, 0) == 0 && calls == 11);
    assert(run(partial, 1, 0) == 2 && calls > 1 && calls <= 12);
    assert(run(resumed, 2, 2) == 2 && calls == 2);
    assert(!memcmp(input, received, sizeof(input)));
    assert(run(overreport, 1, 0) == -EIO && offset == 0 && calls == 1);
    return 0;
#endif
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("staged_source", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--expect-unfixed", action="store_true")
    a = p.parse_args()
    source = a.staged_source.read_text()
    start = source.index("\nint _btif_pio_write(")
    end = source.index("\nint _btif_dump_memory(", start)
    c = a.output.with_suffix(".c")
    c.write_text(SHIM + source[start:end] + TEST)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *(["-Wno-type-limits", "-DEXPECT_UNFIXED"] if a.expect_unfixed else []),
                    str(c), "-o", str(a.output)], check=True)
    result = subprocess.run([str(a.output)])
    if a.expect_unfixed:
        assert result.returncode == 55, result
        print("Original direct PIO callback hides valid progress when a later HAL error occurs")
    else:
        result.check_returncode()
        print("Direct PIO: signed error, partial+error, zero, suffix retry and bounds passed")


if __name__ == "__main__":
    main()
