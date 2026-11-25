#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify pinned Neo reference artifacts without replacing the T70PRO bootstrap."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tomllib
from types import SimpleNamespace

here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Prepare pinned MacBook Neo reference", "--",
              sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
sys.dont_write_bytecode = True
profile_path = here / "targets/macbook-neo.toml"
profile = tomllib.loads(profile_path.read_text())
out = project / "tools/xnu-os-neo"
report_path = project / "analysis/xnu-macos-neo-target.json"
report = {"status": "running", "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "device_boot": "not_run", "target_os_runtime_qualified": False,
          "neo_modules_prelinked": False, "whole_ipsw_downloaded": False}

def digest(data):
    return hashlib.sha256(data).hexdigest()

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def tree(blob):
    nodes_left = 8192
    def read(at, depth=0):
        nonlocal nodes_left
        require(depth < 64 and nodes_left > 0 and at + 8 <= len(blob), "Bounded ADT node required")
        nodes_left -= 1
        count, children = struct.unpack_from("<II", blob, at)
        at += 8
        require(count + children <= 8192, "Excessive ADT count")
        properties, nodes = {}, []
        for _ in range(count):
            require(at + 36 <= len(blob), "Truncated ADT property")
            name, size = struct.unpack_from("<32sI", blob, at)
            at += 36
            name = name.split(b"\0")[0].decode("ascii")
            size &= 0x7fffffff
            require(name not in properties and at + size <= len(blob), "Invalid ADT property")
            properties[name] = blob[at:at + size]
            at += (size + 3) & ~3
        for _ in range(children):
            child, at = read(at, depth + 1)
            name = child["properties"]["name"].split(b"\0")[0].decode("ascii")
            nodes.append(child)
        return {"properties": properties, "children": nodes}, at
    result, end = read(0)
    require(end == len(blob), "Trailing ADT data")
    return result

try:
    subprocess.run([sys.executable, str(here / "prepare-sources.py"), "--check"], check=True)
    common = project / "xnu-sources/UniXNU"
    kit = project / "xnu-sources/XnuPrelink"
    kit_pin = "c645d76bb0ab8c506a76840ddf93a2276d419a0e"
    require(subprocess.check_output(["git", "-C", str(kit), "rev-parse", "HEAD"], text=True).strip() == kit_pin
            and not subprocess.check_output(["git", "-C", str(kit), "status", "--porcelain"], text=True).strip(),
            "Pinned clean XnuPrelink is required")
    sys.path.insert(0, str(common / "scripts"))
    sys.path.insert(0, str(kit))
    from unixnu.ipsw import Ipsw, kernelcache, im4p_payload
    from unixnu.util import lzfse
    from kc_image import Image
    os.environ["PATH"] = str(project / "tools/unixnu-linux/bin") + os.pathsep + os.environ["PATH"]
    ctx = SimpleNamespace(board=profile, out_root=out, option_text=lambda *args: None)
    source = Ipsw(ctx)
    manifest = source.zip.read("BuildManifest.plist")
    require(digest(manifest) == profile["ipsw"]["manifest_sha256"], "Neo BuildManifest pin mismatch")
    components = {}
    for name in ("DeviceTree", "StaticTrustCache", "KernelCache"):
        member, size, packed = source.info(name)
        require(size <= 128 << 20, "Unexpected Neo component size")
        path = source.cache / Path(member).name
        pin = profile["ipsw"]["sha256"][path.name]
        if path.exists():
            require(digest(path.read_bytes()) == pin, "Changed cached artifact preserved: " + str(path))
        path = source.extract(name)
        data = path.read_bytes()
        require(digest(data) == pin, "Neo component SHA-256 mismatch: " + name)
        require(hashlib.sha384(data).digest() == source.identity["Manifest"][name]["Digest"],
                "Neo component differs from Apple's manifest digest: " + name)
        components[name] = {"path": str(path), "member": member, "bytes": len(data),
                            "sha256": pin, "apple_manifest_sha384_verified": True}
    payload = im4p_payload(Path(components["DeviceTree"]["path"]).read_bytes())
    payload_path = out / "DeviceTree.j700ap.bin"
    if payload_path.exists():
        require(payload_path.read_bytes() == payload, "Changed DT payload preserved")
    else:
        payload_path.parent.mkdir(parents=True, exist_ok=True)
        payload_path.write_bytes(payload)
    decoded = subprocess.check_output([lzfse(), "-decode", "-i", str(payload_path)])
    require(digest(decoded) == profile["reference"]["device_tree_sha256"], "Neo decoded DT pin mismatch")
    adt = tree(decoded)
    products = [n for n in adt["children"] if n["properties"]["name"] == b"product\0"]
    require(len(products) == 1, "Expected a unique product node at the root")
    root, product = adt["properties"], products[0]["properties"]
    text = lambda value: value.split(b"\0")[0].decode("ascii")
    identity = {"name": text(product["product-name"]), "identifier": text(root["model"]),
                "device_class": source.identity["Info"]["DeviceClass"],
                "target_type": text(root["target-type"]), "target_sub_type": text(root["target-sub-type"])}
    require(identity == profile["product"], "Actual Neo product identity differs from the selected target")
    require(text(product["fdr-product-type"]) == identity["identifier"] and
            text(product["unique-model"]) == identity["target_sub_type"], "Neo product DT fields disagree")
    path = source.cache / "kernelcache.decompressed"
    if path.exists():
        require(digest(path.read_bytes()) == profile["kernel"]["prelink"]["kernelcache_sha256"],
                "Changed decompressed Neo kernel preserved")
    path = kernelcache(ctx, source)
    data = path.read_bytes()
    require(digest(data) == profile["kernel"]["prelink"]["kernelcache_sha256"], "Neo kernel pin mismatch")
    header = struct.unpack_from("<8I", data)
    require(header[:4] == (0xfeedfacf, 0x100000c, 0xc0000002, 12), "Expected the pinned arm64e fileset")
    cache = Image(data)
    version_strings = sorted(set(v.decode("ascii") for v in
        re.findall(rb"Darwin Kernel Version [^\0]{0,240}", data)))
    require(len(version_strings) == 1 and
            version_strings[0].startswith("Darwin Kernel Version " + profile["reference"]["darwin_version"] + ":")
            and ("xnu-" + profile["reference"]["xnu_version"] + "~") in version_strings[0],
            "Actual Neo kernel version differs from the reference")
    kernel = Image(data, cache.entries["com.apple.kernel"][1])
    build_command = kernel.command(0x32)
    require(build_command is not None, "Neo kernel deployment command missing")
    deployment = struct.unpack_from("<3I", data, build_command[0] + 8)
    roots = ("com.apple.kec.corecrypto", "com.apple.security.AppleImage4",
             "com.apple.driver.AppleMobileFileIntegrity")
    security = {}
    for name in roots:
        va, offset = cache.entries[name]
        require((struct.unpack_from("<I", data, offset + 8)[0] & 0xffffff) == 2,
                "Unexpected Neo security component ABI")
        security[name] = {"header_file_offset": offset, "virtual_address": hex(va), "architecture": "arm64e"}
    tag = "refs/tags/xnu-" + profile["reference"]["xnu_version"]
    query = subprocess.run(["git", "ls-remote", "--tags",
        "https://github.com/apple-oss-distributions/xnu.git", tag],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=40, check=True)
    report.update(status="target_artifacts_verified_port_incomplete", product_target=identity,
        profile_sha256=digest(profile_path.read_bytes()), hardware=profile["hardware"],
        apple_source=source.spec, manifest_sha256=digest(manifest), components=components,
        os_version=profile["ipsw"]["version"], os_build=profile["ipsw"]["build"],
        device_tree_sha256=digest(decoded), product_app_architecture=text(product["app-macho-architecture"]),
        kernel={"path": str(path), "sha256": digest(data), "bytes": len(data), "architecture": "arm64e",
                "file_type": "MH_FILESET", "fileset_entries": len(cache.entries),
                "version_string": version_strings[0], "deployment": list(deployment), "security_components": security},
        prelink_commit=kit_pin, exact_apple_xnu_tag=tag, exact_apple_xnu_tag_found=bool(query.stdout.strip()),
        exact_apple_xnu_tag_query_exit_code=query.returncode,
        bootstrap=profile["bootstrap"],
        remaining=["Newer XNU/macOS KPI and syscall compatibility",
                   "arm64e/PAC compatibility on Cortex-A55/A76 and sufficient user virtual address space",
                   "Real platform drivers, root volume and userspace execution"])
    print("PASS: Apple manifest SHA-384, pinned kernel/DT and actual product identity:", identity)
    print("TARGET:", profile["ipsw"]["version"], profile["ipsw"]["build"], version_strings[0])
    print("Neo reference prepared; T70PRO bootstrap is separate and target OS execution is unqualified")
except Exception as error:
    report.update(status="failed", error=str(error))
    raise
finally:
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report["script_sha256"] = digest(Path(__file__).read_bytes())
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path)
