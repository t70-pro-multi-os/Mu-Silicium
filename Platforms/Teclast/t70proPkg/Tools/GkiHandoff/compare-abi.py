#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare stock module imports with their kernel/module providers, offline only."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
PACKAGE = HERE.parents[1]
PROJECT = PACKAGE.parents[3]
spec = importlib.util.spec_from_file_location("stock_module_elf", PACKAGE / "Linux/prepare-stock-modules.py")
elf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symvers(text):
    result = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) not in (4, 5):
            raise ValueError(f"Module.symvers line {number}: expected 4 or 5 tab-separated fields")
        crc, name, provider, export = fields[:4]
        namespace = fields[4] if len(fields) == 5 else ""
        if (not re.fullmatch(r"0x[0-9a-fA-F]{8}", crc) or not name or not provider
                or export not in ("EXPORT_SYMBOL", "EXPORT_SYMBOL_GPL")):
            raise ValueError(f"Module.symvers line {number}: invalid entry")
        item = {"crc": int(crc, 16), "provider": provider,
                "export": export, "namespace": namespace}
        if name in result:
            raise ValueError(f"Module.symvers line {number}: duplicate export {name}")
        result[name] = item
    if not result:
        raise ValueError("Module.symvers has no exports")
    return result


def compare(modules, stock_kernel, candidate):
    providers = {}
    for module in modules:
        for symbol, info in module["exported_symbol_info"].items():
            providers.setdefault(symbol, []).append((module, info))
    errors, warnings, counts = [], [], Counter()

    def issue(module, symbol, reason, **extra):
        errors.append({"module": module["name"], "source": module["source"],
                       "symbol": symbol, "reason": reason, **extra})

    def policy(module, symbol, item):
        if item["export"] == "EXPORT_SYMBOL_GPL" and not any(
                value in elf.GPL_COMPATIBLE_LICENSES for value in module["license"]):
            issue(module, symbol, "GPL-only export denied")
        namespace = item.get("namespace")
        if namespace is None:
            issue(module, symbol, "export namespace could not be recovered")
        elif namespace and namespace not in module["import_ns"]:
            issue(module, symbol, "namespace not imported", namespace=namespace)

    # Kernel/module duplicate exports can fail module loading even if no importer
    # happens to reference the colliding symbol in this catalog.
    for symbol, entries in providers.items():
        if symbol in candidate and candidate[symbol]["provider"] == "vmlinux":
            for module, _ in entries:
                issue(module, symbol, "kernel/module export collision")

    for module in modules:
        counts["module_binaries"] += 1
        if module["modversion_status"] == "extended/legacy CRC mismatch" and module.get("modversion_format") == "extended":
            warnings.append({"module": module["name"], "source": module["source"],
                             "reason": "valid extended CRC table takes precedence over differing legacy table"})
        elif module["modversion_status"] != "parsed":
            issue(module, "", "module version sections are not valid")
        for message in module["export_integrity_errors"]:
            issue(module, "", "module export parsing error", detail=message)
        for symbol, crc in module["modversions"].items():
            counts["import_records"] += 1
            if symbol in stock_kernel:
                counts["kernel_import_records"] += 1
                if stock_kernel[symbol]["crc"] != crc:
                    issue(module, symbol, "original kernel/import CRC differs")
                    continue
                item = candidate.get(symbol)
                if item is None:
                    issue(module, symbol, "candidate kernel export missing")
                elif item["provider"] != "vmlinux":
                    issue(module, symbol, "kernel export moved to a module", provider=item["provider"])
                else:
                    if item["crc"] != crc:
                        issue(module, symbol, "candidate kernel CRC differs",
                              expected=f"0x{crc:08x}", actual=f"0x{item['crc']:08x}")
                    policy(module, symbol, item)
                continue
            counts["module_import_records"] += 1
            possible = [(m, e) for m, e in providers.get(symbol, []) if m["name"] != module["name"]]
            direct = [(m, e) for m, e in possible if m["name"] in module["depends"]]
            eligible = direct or possible
            matching = [(m, e) for m, e in eligible if e["crc"] == crc]
            if not matching:
                issue(module, symbol, "stock module provider missing or CRC differs")
                continue
            if not direct:
                warnings.append({"module": module["name"], "source": module["source"],
                                 "symbol": symbol, "reason": "provider not declared in module depends"})
            alternatives = {(m["name"], m["sha256"]) for m, _ in matching}
            if len(alternatives) > 1:
                counts["provider_variant_ambiguities"] += 1
            # If variants disagree on policy, preserve that uncertainty as an error.
            policies = {(e["export"], e.get("namespace")) for _, e in matching}
            if len(policies) != 1:
                issue(module, symbol, "module provider variants disagree on export policy")
            else:
                policy(module, symbol, matching[0][1])
    return {"static_import_compatible": not errors, "counts": dict(counts),
            "error_count": len(errors), "errors_by_reason": dict(Counter(x["reason"] for x in errors)),
            "errors": errors, "warnings": warnings}


def stock_inputs():
    _, kernel = elf.read_stock_kernel_exports(PROJECT / "analysis/stock-kernel-exports.json",
                                             PROJECT / "analysis/kernel-info.json")
    catalog = elf.module_catalog(json.loads((PROJECT / "analysis/stock-modules.json").read_text()))
    expected = {(name, source): value for name, variants in catalog.items() for source, value in variants.items()}
    manifest = PROJECT / "analysis/stock-binaries/manifest.json"
    rows = json.loads(manifest.read_text())["modules"]
    if len(rows) != len(expected) or len({(r["name"], r["source"]) for r in rows}) != len(rows):
        raise ValueError("All source-specific stock module binaries must be extracted first")
    modules = []
    for row in rows:
        key = (row["name"], row["source"])
        if key not in expected or row["sha256"] != expected[key]["sha256"]:
            raise ValueError("Stock module identity does not match original metadata")
        path = Path(row["path"])
        if digest(path) != row["sha256"]:
            raise ValueError("Stock module bytes do not match original metadata")
        item = elf.elf_module(path.read_bytes())
        if item["name"] != row["name"]:
            raise ValueError("Stock module embedded name differs")
        modules.append({**item, "source": row["source"], "sha256": row["sha256"]})
    return modules, kernel, {"stock_manifest_sha256": digest(manifest),
                             "stock_export_catalog_sha256": digest(PROJECT / "analysis/stock-kernel-exports.json")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--baseline-self-check", action="store_true", help="original-provider comparison only, not a candidate build")
    mode.add_argument("--candidate-out", type=Path, help="one kernel build output directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    modules, stock, identities = stock_inputs()
    candidate_ready = False
    if args.baseline_self_check:
        candidate = {name: {**value, "provider": "vmlinux"} for name, value in stock.items()}
    else:
        out = args.candidate_out.resolve()
        files = {"config": out / ".config", "symvers": out / "Module.symvers",
                 "image": out / "arch/arm64/boot/Image", "release": out / "include/config/kernel.release",
                 "probe": out / "abi-probe/t70_gki_abi_probe.ko"}
        for path in files.values():
            if not path.is_file():
                raise ValueError("Candidate output is incomplete: " + str(path))
        identities["candidate_artifacts"] = {name: {"path": str(path), "sha256": digest(path)}
                                              for name, path in files.items()}
        receipt = json.loads((out / "build-receipt.json").read_text())
        lock = json.loads((HERE / "source-lock.json").read_text())
        if (not receipt.get("kernel_image_built") or not receipt.get("modules_built")
                or receipt.get("error") or receipt.get("config_sha256") != digest(files["config"])
                or receipt.get("source", {}).get("commit") != lock["sources"]["common"]["commit"]):
            raise ValueError("Candidate build receipt does not match a complete pinned build")
        for artifact in ("Module.symvers", "arch/arm64/boot/Image", "include/config/kernel.release", "abi-probe/t70_gki_abi_probe.ko"):
            if receipt.get("artifacts", {}).get(artifact) != digest(out / artifact):
                raise ValueError("Candidate artifact differs from its build receipt: " + artifact)
        identities["build_receipt_sha256"] = digest(out / "build-receipt.json")
        config = elf.config(files["config"])
        if any(config.get(k) != "y" for k in ("MODULES", "MODVERSIONS", "EXTENDED_MODVERSIONS", "KEXEC")):
            raise ValueError("Candidate must retain MODULES/MODVERSIONS and enable KEXEC")
        candidate = symvers(files["symvers"].read_text())
        probe = elf.elf_module(files["probe"].read_bytes())
        release = files["release"].read_text().strip()
        if not probe["vermagic"] or probe["vermagic"].split()[0] != release:
            raise ValueError("Probe vermagic does not match candidate release")
        suffix = probe["vermagic"].split(maxsplit=1)[1]
        if any(not m["vermagic"] or m["vermagic"].split(maxsplit=1)[1] != suffix for m in modules):
            raise ValueError("Candidate vermagic flags differ from original modules")
        identities["candidate_release"] = release
        identities["candidate_vermagic"] = probe["vermagic"]
        candidate_ready = True
    report = {"schema": 1, "mode": "original-provider-self-check" if args.baseline_self_check else "candidate-static-comparison",
              **identities, **compare(modules, stock, candidate), "candidate_inputs_present": candidate_ready,
              "artifact_receipt_matched": candidate_ready, "module_load_verified": False, "installable": False,
              "unverified": ["module signatures and protected KMI policy",
                             "runtime module provider selection", "pKVM-disabled boot and Android services",
                             "EDK2 handoff and actual hardware recovery"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("mode", "counts", "error_count", "errors_by_reason", "static_import_compatible")}))
    return 0 if report["static_import_compatible"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
