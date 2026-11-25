#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the project-local QEMU implementation-test board. Run inside build lock."""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess, tarfile, urllib.request

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--configure",action="store_true")
    args=ap.parse_args()
    tests=Path(__file__).resolve().parent;k=tests.parents[1];p=k.parents[3]
    q=p/"tools/t70-qemu";q.mkdir(parents=True,exist_ok=True)
    manifest=json.loads((tests/"sources.json").read_text())
    sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
    for item in manifest["sources"]:
        f=q/item["path"]
        if not f.exists():
            assert item.get("url"),f"Missing pinned dependency archive: {f}"
            f.parent.mkdir(parents=True,exist_ok=True)
            with urllib.request.urlopen(item["url"],timeout=60) as src:
                with f.open("wb") as dst:shutil.copyfileobj(src,dst)
        assert sha(f)==item["sha256"],f"Source hash differs: {f}"
    src=q/"qemu-10.2.0";deps=q/"deps";build=q/"build"
    if not src.exists():
        with tarfile.open(q/"qemu-10.2.0.tar.xz") as archive:
            for member in archive:
                parts=Path(member.name).parts
                if "roms" in parts or not (member.isfile() or member.isdir()):continue
                archive.extract(member,q,filter="data")
    # Pinned upstream UFS fix: preserve wire validation in the guest driver.
    for item in manifest.get("qemu_patches",[]):
        patch=tests/item["path"]
        assert sha(patch)==item["sha256"],f"QEMU patch hash differs: {patch}"
        hashes=[sha(src/f["path"]) for f in item["files"]]
        before=[f["before_sha256"] for f in item["files"]]
        after=[f["after_sha256"] for f in item["files"]]
        if hashes==before:
            subprocess.run(["patch","--batch","--fuzz=0","-p1","-i",str(patch)],cwd=src,check=True)
        else:
            assert hashes==after,f"QEMU sources differ from pinned patch: {item['path']}"
        assert [sha(src/f["path"]) for f in item["files"]]==after
    deps.mkdir(exist_ok=True)
    for item in manifest["sources"]:
        if item["path"].endswith(".deb"):
            subprocess.run(["dpkg-deb","--extract",str(q/item["path"]),str(deps)],check=True)
    # Local headers use installed runtime libraries; no system install occurs.
    runtime=Path("/usr/lib/aarch64-linux-gnu")
    for link in (deps/"usr/lib/aarch64-linux-gnu").glob("*.so"):
        if link.is_symlink() and not link.exists():
            candidates=sorted(runtime.glob(link.name+".*"))
            if candidates:
                link.unlink();link.symlink_to(candidates[0])
    shutil.copyfile(tests/"t70-test.c",src/"hw/arm/t70-test.c")
    shutil.copyfile(tests/"t70-usb-board.inc",src/"hw/arm/t70-usb-board.inc")
    shutil.copyfile(tests/"t70-musb.c",src/"hw/usb/t70-musb.c")
    shutil.copyfile(tests/"t70-msdc.c",src/"hw/sd/t70-msdc.c")
    core=src/"hw/usb/t70-musb-core.inc"
    shutil.copyfile(q/"upstream-musb/hcd-musb.c",core)
    subprocess.run(["patch","--batch","--fuzz=0",str(core),str(tests/"musb-v8.2.0.patch")],check=True)
    assert sha(core)==manifest["patched_musb_sha256"]
    shutil.copyfile(q/"upstream-musb/hcd-musb.h",src/"include/hw/usb/hcd-musb.h")
    (src/"configs/devices/aarch64-softmmu/t70-test.mak").write_text("CONFIG_T70_TEST=y\n")
    f=src/"hw/arm/Kconfig";text=f.read_text()
    text=re.sub(r"(?m)^config T70_TEST\n(?:[ \t].*\n|[ \t]*\n)*","",text)
    f.write_text(text+"""
config T70_TEST
    bool
    default y
    depends on ARM && TCG
    select ARM_GIC
    select GENERIC_LOADER
    select USB
    select USB_HID
    select USB_HUB
    select USB_STORAGE_CLASSIC
    select SD
    select PCI_EXPRESS_GENERIC_BRIDGE
    select UFS_PCI
""")
    for file,line in (("hw/arm/meson.build","arm_common_ss.add(when: 'CONFIG_T70_TEST', if_true: files('t70-test.c'))"),
                      ("hw/sd/meson.build","system_ss.add(when: 'CONFIG_T70_TEST', if_true: files('t70-msdc.c'))"),
                      ("hw/usb/meson.build","system_ss.add(when: 'CONFIG_T70_TEST', if_true: files('t70-musb.c'))")):
        f=src/file;text=f.read_text()
        if line not in text:f.write_text(text+"\n"+line+"\n")
    env=os.environ.copy()
    env.update(PKG_CONFIG_SYSROOT_DIR=str(deps),PKG_CONFIG_PATH=str(deps/"usr/lib/aarch64-linux-gnu/pkgconfig"))
    build.mkdir(exist_ok=True)
    configure=[str(src/"configure"),"--target-list=aarch64-softmmu","--without-default-features","--without-default-devices",
        "--with-devices-aarch64=t70-test","--enable-tcg","--enable-fdt=system",
        "--extra-cflags=-I"+str(deps/"usr/include"),"--extra-ldflags=-L"+str(deps/"usr/lib/aarch64-linux-gnu"),
        "--disable-werror","--disable-docs","--disable-download","--disable-install-blobs"]
    if args.configure or not (build/"build.ninja").exists():
        subprocess.run(configure,cwd=build,env=env,check=True)
    subprocess.run(["ninja","-j6","qemu-system-aarch64"],cwd=build,env=env,check=True)
    binary=build/"qemu-system-aarch64"
    result=dict(binary=str(binary),sha256=sha(binary),configure=configure,
                version=subprocess.check_output([str(binary),"--version"],text=True).splitlines()[0],
                sources_sha256=sha(tests/"sources.json"),patch_sha256=sha(tests/"musb-v8.2.0.patch"),
                qemu_patches={item["path"]:item["sha256"] for item in manifest.get("qemu_patches",[])})
    result['board_source_sha256']={name:sha(tests/name) for name in
        ('t70-test.c','t70-musb.c','t70-msdc.c','t70-usb-board.inc')}
    (q/"build-provenance.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result),flush=True)

if __name__=="__main__":main()
