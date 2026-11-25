#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile and test generated hardware data, without a kernel or MMIO access."""
import argparse
from pathlib import Path
import subprocess
import tempfile

CHECK = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define NO_EINT_SUPPORT UINT16_MAX
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MTK_RANGE(x) {x, ARRAY_SIZE(x)}
#define PIN_FIELD_CALC(...) { __VA_ARGS__ }
struct mtk_func_desc { int muxval; const char *name; };
struct mtk_eint_desc { unsigned eint_m, eint_n; };
struct mtk_pin_desc { unsigned number; const char *name; struct mtk_eint_desc eint;
                      struct mtk_func_desc *funcs; };
struct mtk_pin_field_calc { unsigned lo, hi, bank, off, stride, bit, bits, word, fixed; };
struct mtk_pin_reg_calc { const struct mtk_pin_field_calc *range; size_t nranges; };
enum { PINCTRL_PIN_REG_MODE, PINCTRL_PIN_REG_DIR, PINCTRL_PIN_REG_DI,
       PINCTRL_PIN_REG_DO, PINCTRL_PIN_REG_SR, PINCTRL_PIN_REG_SMT, PINCTRL_PIN_REG_IES,
       PINCTRL_PIN_REG_PU, PINCTRL_PIN_REG_PD, PINCTRL_PIN_REG_DRV, PINCTRL_PIN_REG_PUPD,
       PINCTRL_PIN_REG_R0, PINCTRL_PIN_REG_R1, PINCTRL_PIN_REG_RSEL, PINCTRL_PIN_REG_MAX };
#include "t70-paris-pad-data.h"

static const struct mtk_pin_field_calc *field(unsigned field, unsigned pin)
{
    const struct mtk_pin_reg_calc *r = &mt6789_reg_cals[field];
    for (size_t i = 0; i < r->nranges; ++i)
        if (pin >= r->range[i].lo && pin <= r->range[i].hi) return &r->range[i];
    return NULL;
}

int main(void)
{
    _Static_assert(ARRAY_SIZE(mtk_pins_mt6789) == 222, "188 physical plus 34 virtual");
    _Static_assert(ARRAY_SIZE(mt6789_pull_type) == ARRAY_SIZE(mtk_pins_mt6789),
                   "core must be able to index every logical pin");
    assert(!mt6789_reg_cals[PINCTRL_PIN_REG_SR].range);
    unsigned char seen[160] = {0};
    for (size_t i = 0; i < ARRAY_SIZE(mtk_pins_mt6789); ++i) {
        const struct mtk_pin_desc *d = &mtk_pins_mt6789[i];
        assert(d->number == i && d->funcs);
        if (i < 188) {
            assert(d->funcs[0].name && d->funcs[0].muxval == 0);
            assert(field(PINCTRL_PIN_REG_MODE, i) && field(PINCTRL_PIN_REG_DIR, i));
        } else {
            assert(d->eint.eint_m == 0 && !d->funcs[0].name);
            assert(!field(PINCTRL_PIN_REG_MODE, i));
            assert(mt6789_pull_type[i] == 0);
        }
        if (d->eint.eint_n != NO_EINT_SUPPORT) {
            assert(d->eint.eint_n < ARRAY_SIZE(seen) && !seen[d->eint.eint_n]);
            seen[d->eint.eint_n] = 1;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(seen); ++i) assert(seen[i]);
    assert(mtk_pins_mt6789[0].eint.eint_n == 0);
    assert(mtk_pins_mt6789[7].eint.eint_n == 7);
    assert(mtk_pins_mt6789[9].eint.eint_n == 9);
    assert(mtk_pins_mt6789[152].eint.eint_n == NO_EINT_SUPPORT);
    assert(field(PINCTRL_PIN_REG_MODE, 9)->off == 0x310);
    assert(field(PINCTRL_PIN_REG_MODE, 152)->off == 0x430);
}
'''


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("staged", type=Path)
    source = p.parse_args().staged.resolve()
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "test.c"
        exe = c.with_suffix("")
        c.write_text(CHECK)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-I", source, str(c), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    print("Paris pin data: 222 entries, 160 unique IRQ routes, GPIO0/7/9/152 verified")
