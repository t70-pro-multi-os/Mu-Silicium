#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Reject unproved entry routes and preserve previous installation bundles."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
spec=importlib.util.spec_from_file_location("bundle_builder",Path(__file__).with_name("build-install-bundle.py"))
mod=importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def run(boot,selector,tests):
    with tempfile.TemporaryDirectory(prefix="t70-bundle-tests-") as temp:
        temp=Path(temp)
        evidence=temp/"lk.json"
        report=temp/"lk.md"
        report.write_text("Synthetic negative test: no boot-kernel route established.\n")
        actual=json.loads(selector.read_text())
        evidence.write_text(json.dumps({"lk_sha256":actual["original_lk_sha256"],
                                        "selection_mode":actual["boot_mode_tag"]["recovery_mode"],
                                        "selection_mode_uses_boot_kernel":False,
                                        "evidence_report_sha256":hashlib.sha256(report.read_bytes()).hexdigest()}))
        out=temp/"output"
        try:
            mod.assemble(boot,selector,tests,evidence,None,None,out)
        except ValueError as exc:
            assert "evidence" in str(exc)
        else:
            raise AssertionError("Unproved boot route was published as ready")
        assert not out.exists()
        out.mkdir()
        saved=out/"keep"
        saved.write_bytes(b"existing installation bundle")
        try:
            mod.assemble(None,None,None,None,None,None,out)
        except ValueError as exc:
            assert "exists" in str(exc)
        else:
            raise AssertionError("Existing bundle was overwritten")
        assert saved.read_bytes()==b"existing installation bundle"
    return {"schema":1,"device_access":False,"count":2,
            "passed":["unproved_LK_entry_route_rejected_without_output",
                      "existing_install_bundle_preserved"]}


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("boot","selector","tests","report"):
        p.add_argument("--"+name,type=Path,required=True)
    a=p.parse_args()
    result=run(a.boot,a.selector,a.tests)
    a.report.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2))
