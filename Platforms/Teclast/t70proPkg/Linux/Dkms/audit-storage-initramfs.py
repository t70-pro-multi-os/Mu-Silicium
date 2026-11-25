#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check storage host and independent DT suppliers in an offline archive list."""
import argparse
import fnmatch
import json
from pathlib import Path, PurePosixPath
import re
import runpy
import struct

HERE = Path(__file__).resolve().parent
STOCK = runpy.run_path(str(HERE / "verify-storage-dt.py"))
COMMON = {
    "top clocks": "mediatek,mt6789-topckgen",
    "infra clocks": "mediatek,mt6789-infracfg_ao",
    "PMIC transport": "mediatek,mt6789-pwrap",
}
COMPAT = {
    "ufs": {"host": "mediatek,mt8183-ufshci", "reset": "ti,syscon-reset", **COMMON},
    "sd": {"host": "teclast,t70pro-mt6789-ext-sd",
           "pinctrl / GPIO7": "mediatek,mt6789-pinctrl", **COMMON},
}
CONFIG = {
    "ufs": {"SCSI_UFS_MEDIATEK": "ufs-mediatek", "RESET_TI_SYSCON": "reset-ti-syscon"},
    "sd": {"MMC": "mmc_core", "MMC_BLOCK": "mmc_block"},
}
COMMON_CONFIG = {"MFD_MT6397": "mt6397", "REGULATOR_MT6358": "mt6358-regulator"}


def module_name(path):
    name = PurePosixPath(path).name
    m = re.fullmatch(r"([A-Za-z0-9_+-]+)\.ko(?:\.(?:xz|gz|zst))?", name)
    if not m:
        raise ValueError("invalid module path: " + path)
    return m[1].replace("-", "_")


def effective_sd_contract(stock, effective):
    expected, actual = stock[STOCK["SD"]], effective[STOCK["SD"]]
    if actual.get("compatible") != b"teclast,t70pro-mt6789-ext-sd\0":
        raise ValueError("effective SD host is not the audited external-slot driver")
    for key in ("reg", "interrupts", "clocks", "clock-names", "vmmc-supply", "vqmmc-supply",
                "cd-gpios", "bus-width", "pinctrl-names", "pinctrl-0", "pinctrl-1", "pinctrl-2"):
        if actual.get(key) != expected[key]:
            raise ValueError("effective SD changed the stock " + key)
    if "no-mmc" not in actual or "non-removable" in actual or "supports-cqe" in actual:
        raise ValueError("effective SD is no longer the removable four-bit slot")
    phandle, line, _ = struct.unpack(">III", actual["cd-gpios"])
    providers = [(path, node) for path, node in effective.items()
                 if node.get("phandle") == struct.pack(">I", phandle)]
    if len(providers) != 1 or providers[0][0] != "/soc/pinctrl" or line != 7:
        raise ValueError("effective SD card detect lost the stock GPIO7 provider")
    path, pin = providers[0]
    entries = (b"mediatek,mt6789-pinctrl\0", b"teclast,t70pro-mt6789-pinctrl\0")
    if pin.get("compatible") not in entries:
        raise ValueError("unsupported effective GPIO7 pinctrl compatible")
    if pin["compatible"] == entries[1]:
        original = stock[path]
        eint_candidates = [(name, node) for name, node in stock.items()
                           if node.get("phandle") == original["mediatek,eint"]]
        if len(eint_candidates) != 1:
            raise ValueError("ambiguous stock EINT resource")
        eint_path, eint = eint_candidates[0]
        if not (pin.get("reg") == original["reg"] + eint["reg"]
                and pin.get("reg-names") == original["reg-names"] + b"eint\0"
                and pin.get("interrupts") == eint["interrupts"]
                and pin.get("interrupt-parent") == stock["/"]["interrupt-parent"]
                and effective[eint_path].get("status") == b"disabled\0"):
            raise ValueError("effective Paris does not own exactly the stock EINT resource")
    issues = []
    for role, node_path in (("SD host", STOCK["SD"]), ("GPIO7 provider", path)):
        if effective[node_path].get("status", b"okay\0") not in (b"okay\0", b"ok\0"):
            issues.append(role + ": disabled in effective DT")
        for parent in PurePosixPath(node_path).parents:
            status = effective.get(str(parent), {}).get("status", b"okay\0")
            if status not in (b"okay\0", b"ok\0"):
                issues.append(f"{role}: ancestor {parent} disabled in effective DT")
    return pin["compatible"][:-1].decode("ascii"), issues


def inspect(path, release, config_text, archive_text, dependency_text, builtin_text,
            alias_text, builtin_modinfo, effective_sd=None):
    config = {}
    for line in config_text.splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            k, v = line[7:].split("=", 1)
            config[k] = v

    prefixes = (f"lib/modules/{release}/", f"usr/lib/modules/{release}/")
    present = {}
    for line in archive_text.splitlines():
        value = line.strip().removeprefix("./").lstrip("/")
        if ".." in PurePosixPath(value).parts:
            raise ValueError("invalid initramfs listing path")
        prefix = next((p for p in prefixes if value.startswith(p)), None)
        if prefix and re.search(r"\.ko(?:\.(?:xz|gz|zst))?$", value):
            rel = value[len(prefix):]
            present.setdefault(module_name(rel), set()).add(rel)
    builtin = {module_name(v) for v in builtin_text.splitlines() if v.strip()}
    depend = {}
    for line in dependency_text.splitlines():
        if not line.strip():
            continue
        key, sep, values = line.partition(":")
        if not sep or key in depend:
            raise ValueError("malformed modules.dep")
        module_name(key)
        entries = values.split()
        for v in entries:
            module_name(v)
        depend[key] = entries
    aliases = []
    for line in alias_text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == "alias" and parts[1].startswith("of:"):
            aliases.append((parts[1], parts[2].replace("-", "_")))
    for raw in builtin_modinfo.split(b"\0"):
        if b".alias=of:" in raw:
            name, opt = raw.decode().split(".alias=", 1)
            aliases.append((opt, name.replace("-", "_")))

    def status(name):
        entries = present.get(name, set())
        if name in builtin and entries:
            return "conflicts_with_builtin"
        if name in builtin:
            return "builtin"
        if len(entries) != 1:
            return "absent" if not entries else "ambiguous_archive_paths"
        return "archive"

    def closure(name):
        issues = set()
        seen = set()
        active = set()

        def require_module(mod):
            if mod in active:
                issues.add(f"dependency cycle at {mod}")
                return
            if mod in seen:
                return
            seen.add(mod)
            state = status(mod)
            if state == "builtin":
                return
            if state != "archive":
                issues.add(f"{mod}: {state}")
                return
            rel, = present[mod]
            if rel not in depend:
                issues.add(f"{mod}: no exact modules.dep entry for {rel}")
                return
            active.add(mod)
            try:
                for dependency in depend[rel]:
                    target = module_name(dependency)
                    if status(target) == "archive" and dependency not in present[target]:
                        issues.add(f"{target}: dependencies refer to another archive path")
                    require_module(target)
            finally:
                active.remove(mod)
        require_module(name)
        return sorted(issues)

    result, issues = {"release": release, "root_device": path, "suppliers": {}, "config": {}}, []
    supplier_compat = dict(COMPAT[path])
    if effective_sd is not None:
        supplier_compat["pinctrl / GPIO7"], dt_issues = effective_sd
        issues.extend(dt_issues)
        result["effective_dt"] = "verified external SD host and GPIO7 resource graph"
    elif path == "sd":
        result["effective_dt"] = "not supplied"
        issues.append("SD effective DT not supplied: pass --effective-dtb")
    for role, compatible in supplier_compat.items():
        modalias = f"of:NfixtureTnullC{compatible}"
        matches = sorted({name for pattern, name in aliases if fnmatch.fnmatchcase(modalias, pattern)})
        entry = result["suppliers"][role] = {"compatible": compatible, "matches": matches}
        if len(matches) != 1:
            issues.append(f"{role}: expected one matching OF driver, got {len(matches)}")
        else:
            entry["status"] = status(matches[0])
            issues.extend(f"{role}: {i}" for i in closure(matches[0]))
    for option, mod in (CONFIG[path] | COMMON_CONFIG).items():
        value, name = config.get(option), mod.replace("-", "_")
        state = status(name)
        result["config"][option] = {"value": value, "module": name, "status": state}
        if value == "y" and state != "builtin":
            issues.append(f"{option}: built-in not corroborated by modules.builtin")
        elif value == "m" and state != "archive":
            issues.append(f"{option}: configured module not uniquely in target archive")
        elif value not in ("y", "m"):
            issues.append(f"{option}: not enabled")
        elif value == "m":
            issues.extend(f"{option}: {i}" for i in closure(name))
    result["issues"] = sorted(set(issues))
    result["scope"] = "offline DT/packaging check only; no hardware readiness assessment"
    return result


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", choices=COMPAT)
    ap.add_argument("release")
    ap.add_argument("--effective-dtb", type=Path, help="audit a staged external-SD DT")
    for arg in ("stock-dtb", "config", "archive-list", "modules-dep", "modules-builtin",
                "modules-alias", "builtin-modinfo"):
        ap.add_argument("--" + arg, type=Path, required=True)
    x = ap.parse_args()
    try:
        if not re.fullmatch(r"[a-zA-Z0-9_.+-]+", x.release):
            raise ValueError("invalid release")
        stock_data = x.stock_dtb.read_bytes()
        STOCK["verify"](stock_data)
        effective = None
        if x.effective_dtb:
            if x.root != "sd":
                raise ValueError("--effective-dtb supports only the external SD candidate")
            def props(data):
                nodes, _ = STOCK["FDT"]["tree"](data)
                return {path: node["properties"] for path, node in nodes.items()}
            effective = effective_sd_contract(props(stock_data), props(x.effective_dtb.read_bytes()))
        report = inspect(x.root, x.release, x.config.read_text(), x.archive_list.read_text(),
                         x.modules_dep.read_text(), x.modules_builtin.read_text(),
                         x.modules_alias.read_text(), x.builtin_modinfo.read_bytes(), effective)
        print(json.dumps(report, indent=2))
        raise SystemExit(1 if report["issues"] else 0)
    except (KeyError, OSError, ValueError) as error:
        ap.exit(2, f"initramfs audit refused: {error}\n")
