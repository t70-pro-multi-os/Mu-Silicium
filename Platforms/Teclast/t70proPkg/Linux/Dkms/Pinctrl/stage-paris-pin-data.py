#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage exact T70 MT6789 pad data in a form the generic Paris shim can consume."""
import argparse
from pathlib import Path
import re
import subprocess

PINNED = "a71515dbaf0d539ffb36416f14186dad17eef743"
ROOT = "mindone/modules/pinctrl_mediatek/"
PHYSICAL = 188
EINT = 160
BANKS = 7


def stock(repo, filename):
    return subprocess.check_output(["git", "-C", str(repo), "show",
                                    f"{PINNED}:{ROOT}{filename}"], text=True)


def pin_table(header):
    table = re.search(r"(?ms)^static const struct mtk_pin_desc mtk_pins_mt6789\[\] = "
                      r"\{(.*?)^\};", header)[1]
    physical = re.findall(r'MTK_PIN_NODRV\(\s*(\d+), "GPIO(\d+)",\s*'
                          r'MTK_EINT_FUNCTION\((\w+),\s*(\w+)\)', table)
    virtual = re.findall(r'MTK_PIN_EINT\((\d+),\s*(\d+)\)', table)
    if ([(int(pin), int(name)) for pin, name, _, _ in physical] != [(i, i) for i in range(PHYSICAL)]
            or [int(pin) for pin, _ in virtual] != list(range(PHYSICAL, PHYSICAL + len(virtual)))):
        raise ValueError("stock pin sequence changed")
    irq = {}
    for pin, _, mux, number in physical:
        if mux == number == "NO_EINT_SUPPORT":
            continue
        if int(mux) != 0:
            raise ValueError("stock pin uses a non-GPIO EINT mux")
        irq[int(pin)] = (int(mux), int(number))
    irq.update({int(pin): (None, int(number)) for pin, number in virtual})
    if sorted(number for _, number in irq.values()) != list(range(EINT)):
        raise ValueError("stock physical/virtual EINT coverage is not bijective")
    if any(irq.get(gpio) != (0, gpio) for gpio in (0, 7, 9)) or 152 in irq:
        raise ValueError("board GPIO route changed")
    return (table.replace("MTK_PIN_NODRV(", "T70_PIN(")
                 .replace("MTK_PIN_EINT(", "T70_VIRTUAL(")
                 .replace("MTK_EINT_FUNCTION(", "T70_EINT(")
                 .replace("MTK_FUNCTION(", "T70_FUNC(")), irq


def reg_tables(src):
    a = src.index("#define PIN_FIELD_BASE(")
    b = src.index("static const struct mtk_eh_pin_pinmux ", a)
    tables = src[a:b]
    for name, items in re.findall(r"(?ms)^static const struct mtk_pin_field_calc "
                                  r"(\w+)\[\] = \{(.*?)^\};", tables):
        prior = -1
        for m in re.finditer(r"((?:PINS|PIN)_FIELD_BASE)\(([^)]+)\)", items):
            low, high, bank, offset, stride, bit, width = [
                int(v.strip(), 0) for v in m[2].split(",")]
            end = bit + (high - low) * width if m[1] == "PIN_FIELD_BASE" else bit
            if not (prior < low <= high < PHYSICAL and 0 <= bank < BANKS and
                    0 <= bit < 32 and 0 < width <= 32 and
                    offset + ((end + width - 1) // 32) * stride <= 0xffc):
                raise ValueError(f"invalid field geometry {name}: {m[0]}")
            prior = high
        if prior < 0:
            raise ValueError(f"no verified fields in {name}")
    # The core indexes pull_type by any logical pin. Virtual EINT-only
    # descriptors get no pull capabilities, not stock pad values.
    pull_entries = re.search(r"(?ms)^static const unsigned int mt6789_pull_type\[\] "
                             r"= \{(.*?)^\};", tables)[1]
    physical_entries = re.sub(r"/\*.*?\*/", "", pull_entries, flags=re.S)
    if len(re.findall(r"\b\d+\b", physical_entries)) != PHYSICAL:
        raise ValueError("unexpected number of stock pull capabilities")
    tables = tables.replace("mt6789_pull_type[]", "mt6789_pull_type[ARRAY_SIZE(mtk_pins_mt6789)]")
    calc = re.search(r"(?ms)^static const struct mtk_pin_reg_calc "
                     r"mt6789_reg_cals\[PINCTRL_PIN_REG_MAX\] = \{(.*?)^\};", src)[1]
    # EH has vendor pinmux-dependent policy, not interchangeable with generic
    # DRV_ADV. Preserve its data, but do not advertise that policy in Paris.
    calc, n = re.subn(r"(?m)^\s*\[PINCTRL_PIN_REG_DRV_EH\].*$", "", calc)
    if n != 1:
        raise ValueError("unexpected stock enhanced-drive table")
    # Stock uses its own GPIO-direction alias. Generic slew-rate must
    # not publish a pinconf field that writes GPIO direction.
    calc, n = re.subn(r"(?m)^[ \t]*\[PINCTRL_PIN_REG_SR\].*$", "", calc)
    if n != 1:
        raise ValueError("unexpected stock slew-rate alias")
    return tables, calc


def stage(repo, dest):
    pin_data, irq = pin_table(stock(repo, "pinctrl-mtk-mt6789.h"))
    tables, calc = reg_tables(stock(repo, "pinctrl-mt6789.c"))
    code = """/* SPDX-License-Identifier: GPL-2.0 */
/* MT6789 pad tables (C) 2021 MediaTek Inc.; generated from pinned stock source. */
#define T70_EINT(mux, irq) { .eint_m = (mux), .eint_n = (irq) }
#define T70_FUNC(val, label) { .muxval = (val), .name = (label) }
#define T70_PIN(pin, label, route, ...) { .number = (pin), .name = (label), \\
    .eint = route, .funcs = (struct mtk_func_desc[]){ __VA_ARGS__, { } } }
#define T70_VIRTUAL(pin, irq) { .number = (pin), .name = "GPIO" #pin, \\
    .eint = T70_EINT(0, irq), .funcs = (struct mtk_func_desc[]){ { } } }
static const struct mtk_pin_desc mtk_pins_mt6789[] = {
""" + pin_data + """};
#undef T70_VIRTUAL
#undef T70_PIN
#undef T70_FUNC
#undef T70_EINT

""" + tables + """static const struct mtk_pin_reg_calc mt6789_reg_cals[PINCTRL_PIN_REG_MAX] = {""" + calc + "\n};\n"
    with dest.open("x") as out:
        out.write(code)
    print(f"Staged {PHYSICAL} physical pads, {len(irq)} EINT routes and stock register ranges.")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference", type=Path)
    p.add_argument("destination", type=Path)
    a = p.parse_args()
    try:
        stage(a.reference, a.destination)
    except (OSError, ValueError, TypeError, subprocess.CalledProcessError) as error:
        p.exit(2, f"Paris pin data refused: {error}\n")
