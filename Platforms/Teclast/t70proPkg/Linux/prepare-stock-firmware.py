#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Prepare immutable, non-calibration vendor firmware from the existing backup."""
import argparse,hashlib,importlib.util,json,os,re,subprocess
from pathlib import Path
PKG=Path(__file__).resolve().parent.parent
REPO=PKG.parents[2]
PROJECT=REPO.parent
MAX_VENDOR=300*1024*1024
MAX_PAYLOAD=128*1024*1024
NAME=re.compile(r"(?:[A-Za-z0-9][A-Za-z0-9_.-]*\.(?:bin|cfg)|remoteproc_scp)\Z")

def digest(data):return hashlib.sha256(data).hexdigest()
def firmware_names(listing):
    if "<E>" in listing:raise ValueError("EROFS firmware listing failed")
    if "FILENAME" not in listing:raise ValueError("Missing EROFS directory listing")
    names=[];excluded=[]
    for line in listing.split("FILENAME",1)[1].splitlines():
        fields=line.split()
        if len(fields)!=3 or not fields[0].isdigit() or fields[2] in (".",".."):continue
        name=fields[2]
        if fields[1]=="1" and NAME.fullmatch(name):
            if name in names:raise ValueError("Duplicate firmware basename")
            names.append(name)
        else:excluded.append({"name":name,"erofs_type":fields[1]})
    if not names:raise ValueError("No regular stock firmware files")
    return sorted(names),excluded

def entries(manifest,root):
    """Verify every bounded original before exposing any archive entries."""
    if manifest.get("schema")!=1 or manifest.get("device_access") is not False or manifest.get("partition")!="vendor_a":
        raise ValueError("Unsupported firmware manifest")
    root=Path(root);rows=manifest.get("files")
    if not isinstance(rows,list) or not rows:raise ValueError("Empty firmware manifest")
    result=[];seen=set();total=0
    for item in rows:
        name=item.get("name","")
        if not NAME.fullmatch(name) or name in seen:raise ValueError("Unsafe or duplicate firmware name")
        seen.add(name)
        if item.get("path")!="vendor_a_firmware/"+name or item.get("source_path")!="/firmware/"+name:
            raise ValueError("Unexpected firmware source identity")
        path=root/item["path"]
        if path.is_symlink() or path.parent.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            raise ValueError("Firmware path leaves extracted originals")
        size=item.get("bytes")
        if not isinstance(size,int) or not 0<=size<=MAX_PAYLOAD or path.stat().st_size!=size:
            raise ValueError("Firmware length mismatch")
        if not size and not name.endswith(".cfg"):raise ValueError("Empty binary firmware")
        total+=size
        if total>MAX_PAYLOAD:raise ValueError("Firmware payload exceeds bound")
        data=path.read_bytes()
        if digest(data)!=item.get("sha256"):raise ValueError("Firmware SHA-256 mismatch")
        result.append(("lib/firmware/"+name,0o100644,data))
    if total!=manifest.get("total_bytes"):raise ValueError("Firmware total mismatch")
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--backup",type=Path,required=True)
    a=ap.parse_args()
    spec=importlib.util.spec_from_file_location("stock_inspect",PKG/"inspect-stock-modules.py")
    stock=importlib.util.module_from_spec(spec);spec.loader.exec_module(stock)
    layout=json.loads((a.backup/"partition-layout.json").read_text())
    if not layout["primary_header_crc_valid"] or not layout["partition_entries_crc_valid"]:raise ValueError("Original GPT checks missing")
    sup=next(x for x in layout["partitions"] if x["name"]=="super")
    base=PROJECT/"analysis/stock-binaries";target=base/"vendor_a_firmware"
    if base.resolve().is_relative_to(a.backup.resolve()):raise ValueError("Output overlaps original backup")
    target.mkdir(exist_ok=True)
    files=[]
    with (a.backup/"images/ufs-lu2.img").open("rb") as stream:
        parts,geometry=stock.logical_partitions(stream,sup);p=parts["vendor_a"]
        if not 0<p["bytes"]<=MAX_VENDOR:raise ValueError("Vendor exceeds memory bound")
        fd=os.memfd_create("t70pro-static-vendor-firmware")
        try:
            os.ftruncate(fd,p["bytes"]);cur=0
            for e in p["extents"]:
                for off in range(0,e["bytes"],4*1024*1024):
                    n=min(4*1024*1024,e["bytes"]-off)
                    if e["type"]==0:
                        data=stock.read_at(stream,sup["offset_bytes"]+e["super_offset"]+off,n)
                        if os.pwrite(fd,data,cur+off)!=n:raise ValueError("Short memory staging write")
                cur+=e["bytes"]
            def erofs(option):
                r=subprocess.run(["dump.erofs",*option,f"/proc/self/fd/{fd}"],pass_fds=(fd,),capture_output=True,check=True)
                if r.stderr:raise ValueError("EROFS read reported an error")
                return r.stdout
            names,excluded=firmware_names(erofs(["--ls","--path=/firmware"]).decode("utf-8"))
            total=0
            for name in names:
                data=erofs(["--cat","--path=/firmware/"+name]);total+=len(data)
                if (not data and not name.endswith(".cfg")) or total>MAX_PAYLOAD:raise ValueError("Invalid firmware payload size: "+name+" ("+str(len(data))+")")
                out=target/name
                if out.is_symlink():raise ValueError("Refusing firmware symlink output")
                if out.exists():
                    if out.read_bytes()!=data:raise ValueError("Existing original firmware differs")
                else:out.write_bytes(data)
                files.append({"name":name,"path":"vendor_a_firmware/"+name,"source_path":"/firmware/"+name,
                              "bytes":len(data),"sha256":digest(data)})
        finally:os.close(fd)
    manifest={"schema":1,"scope":"Static shipped vendor firmware only; no calibration, modem partitions, userdata or firmware update",
              "device_access":False,"partition":"vendor_a","original_access":"rb",
              "source_backup":str(a.backup/"images/ufs-lu2.img"),"logical_partition_copies_saved":False,
              "lp_checks":geometry,"excluded_nonregular_or_nonfirmware":excluded,
              "file_count":len(files),"total_bytes":sum(f["bytes"] for f in files),"files":files,
              "source_sha256":digest(Path(__file__).read_bytes())}
    checked=entries(manifest,base)
    manifest["verified_archive_file_count"]=len(checked)
    out=base/"firmware-manifest.json";out.write_text(json.dumps(manifest,indent=2)+"\n")
    print(json.dumps({"manifest":str(out),"files":len(files),"bytes":manifest["total_bytes"],"excluded":excluded}))
if __name__=="__main__":main()
