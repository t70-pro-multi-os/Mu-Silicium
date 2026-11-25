#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the unchanged T70Pro image on QEMU TCG, with explicit board fixtures."""
from pathlib import Path
import argparse,hashlib,json,re,socket,struct,subprocess,time,runpy,tempfile

class Remote:
    def __init__(self,path):
        self.s=socket.socket(socket.AF_UNIX);self.s.connect(str(path));self.s.settimeout(5)
    def receive(self):
        while True:
            c=self.s.recv(1)
            if not c:raise EOFError("QEMU GDB socket closed")
            if c==b"$":break
        data=bytearray()
        while True:
            c=self.s.recv(1)
            if not c:raise EOFError()
            if c==b"#":break
            data+=c
        checksum=self.s.recv(2);assert int(checksum,16)==sum(data)%256
        self.s.sendall(b"+")
        out=bytearray();i=0
        while i<len(data):
            c=data[i];i+=1
            if c==125:c=data[i]^32;i+=1
            elif c==42:
                out+=out[-1:]*(data[i]-29);i+=1;continue
            out.append(c)
        return out.decode()
    def send(self,text,wait=True):
        b=text.encode();self.s.sendall(b"$"+b+b"#"+f"{sum(b)%256:02x}".encode())
        if wait:return self.receive()
    def regs(self):
        b=bytes.fromhex(self.send("g"));return dict(x0=struct.unpack_from("<Q",b,0)[0],sp=struct.unpack_from("<Q",b,31*8)[0],pc=struct.unpack_from("<Q",b,32*8)[0],cpsr=struct.unpack_from("<I",b,33*8)[0])
    def setreg(self,index,value,n=8):
        assert self.send(f"P{index:x}="+value.to_bytes(n,"little").hex())=="OK"
    def memory(self,address,n):
        out=bytearray()
        while n:
            size=min(n,2048);s=self.send(f"m{address:x},{size:x}")
            assert not s.startswith("E"),s
            out+=bytes.fromhex(s);address+=size;n-=size
        return bytes(out)
    def resume_for(self,seconds):
        self.send("c",False);time.sleep(seconds);self.s.sendall(b"\x03")
        return self.receive()
    def breakpoint(self,address,enabled=True):
        assert self.send(f"{'Z' if enabled else 'z'}1,{address:x},4")=="OK"
    def run_to_break(self,timeout=30):
        self.s.settimeout(timeout);self.send("c",False)
        try:return self.receive()
        except TimeoutError:
            self.s.sendall(b"\x03");self.s.settimeout(5);self.receive()
            raise TimeoutError(f"QEMU breakpoint deadline; stopped at {self.regs()}")
        finally:self.s.settimeout(5)

class Monitor:
    def __init__(self,path):
        self.s=socket.socket(socket.AF_UNIX);self.s.connect(str(path));self.s.settimeout(5);self.f=self.s.makefile("rwb")
        self.read();self.command("qmp_capabilities")
    def read(self):
        while True:
            line=self.f.readline()
            if not line:raise EOFError()
            d=json.loads(line)
            if "event" not in d:return d
    def command(self,name,**args):
        self.f.write((json.dumps(dict(execute=name,arguments=args))+"\n").encode());self.f.flush()
        d=self.read();assert "error" not in d,d
        return d.get("return")
    def hmp(self,text):return self.command("human-monitor-command",**{"command-line":text})

def file_sha256(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f,"sha256").hexdigest()

def main():
    ap=argparse.ArgumentParser();ap.add_argument("--output",type=Path,required=True);ap.add_argument("--seconds",type=int,default=12);ap.add_argument("--el",type=int,choices=[1,2],default=2)
    ap.add_argument("--build-dir",type=Path,help="firmware build directory containing T70Pro-UEFI-Trial.Image")
    ap.add_argument("--usb",choices=["none","keyboard","storage","hub"],default="none")
    ap.add_argument("--usb-power-path",choices=["typec-source","id-low-hub","typec-only","board-retained-source"],default="board-retained-source",help="default: actual readiness producer over retained-register fixtures; other paths explicitly inject readiness")
    ap.add_argument("--usb-board-active",action="store_true",help="negative case: retain enabled USB interrupts and require host rejection")
    ap.add_argument("--usb-board-cold-attach",action="store_true",help="explicit HUSB311/SC8989x cold-attach register fixture; guest must clear the TCPC event and establish source power")
    ap.add_argument("--usb-exit",choices=["ready","id-changed"],
                    help="exit Boot Services after USB boot and verify the OS handoff record")
    ap.add_argument("--sd",action="store_true")
    ap.add_argument("--ufs",action="store_true",help="execute the production LU2 path against QEMU UFS; inherited-power fixture")
    ap.add_argument("--ufs-boot-lus",action="store_true",help="add two synthetic read-only 4 MiB boot LUs at 0/1 alongside the main LU2")
    ap.add_argument("--ufs-no-lu",action="store_true",help="expose an empty UFS host and verify it publishes no disk")
    ap.add_argument("--sdhc",action="store_true",help="use a sparse 4 GiB SDHC card with a last-sector fixture")
    args=ap.parse_args()
    board_usb=args.usb!="none" and args.usb_power_path=="board-retained-source"
    if args.usb_board_active:
        assert board_usb
    if args.usb_board_cold_attach:
        assert board_usb and not args.usb_board_active, "cold attach requires the production USB board path without active-controller rejection"
    if args.usb_exit:
        assert args.usb == "hub" and board_usb and not args.usb_board_active
    if args.usb_exit == "id-changed":
        assert not args.usb_board_cold_attach, "the ID change test starts on the retained low-ID route"
    args.sd=args.sd or args.sdhc
    assert not args.ufs_boot_lus or args.ufs, "Synthetic boot LUs require the populated --ufs main LU2"
    assert not (args.ufs and args.ufs_no_lu), "Choose either a populated UFS LU or an unbound UFS host"
    ufs_host = args.ufs or args.ufs_no_lu
    assert sum((args.sd,args.ufs,args.usb in ("storage","hub")))<=1,"Use one populated storage medium in a run"
    k=Path(__file__).resolve().parents[2];m=k.parents[2];p=m.parent
    b=args.build_dir.resolve() if args.build_dir else m/"Build/t70proPkg/RELEASE_CLANGPDB"
    out=args.output
    if (out/"execution.json").exists():raise SystemExit("Use a new output directory; prior evidence is immutable.")
    out.mkdir(parents=True,exist_ok=True)
    qemu=p/"tools/t70-qemu/build/qemu-system-aarch64"
    image=b/"T70Pro-UEFI-Trial.Image"
    v=runpy.run_path(str(k/"verify-boot.py"))
    tree=v["runtime_fdt_fixture"](simple=True,console=True,ccci_base=0x4f000100)
    if ufs_host:
        from ufs_fixture import runtime_tree
        tree,ufs_fixture=runtime_tree(tree,k,p)
    if board_usb:
        from usb_board_fixture import runtime_tree
        tree, usb_board_fixture = runtime_tree(tree,k,p,cold_attach=args.usb_board_cold_attach)
    # Reserve probe scratch as normal mapped RAM, not allocatable DXE heap.
    hdr=list(struct.unpack(">10I",tree[:40]));at=hdr[2]
    tree=tree[:at-16]+struct.pack(">QQ",0x40100000,0x100000)+tree[at-16:]
    hdr[1]+=16;hdr[2]+=16;hdr[3]+=16
    tree=struct.pack(">10I",*hdr)+tree[40:]
    dtb=out/"explicit-runtime-fixture.dtb";dtb.write_bytes(tree)
    tags=out/"lk-ccci-tags.bin"
    tags.write_bytes(v["runtime_ccci_tags"](0x4e000000,0x100000))
    sec={n:int(a,16) for a,n in re.findall(r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$",(b/"AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map").read_text(),re.M)}
    socket_dir=tempfile.TemporaryDirectory(prefix="run-",dir=p/"tools/t70-qemu")
    gdb=Path(socket_dir.name)/"gdb.sock";qmp=Path(socket_dir.name)/"qmp.sock"
    machine="t70-test"
    if ufs_host:machine+=",ufs=on"
    if board_usb:machine+=",usb-board=on,usb-board-active="+("on" if args.usb_board_active else "off")
    if args.usb_board_cold_attach:machine+=",usb-board-cold-attach=on"
    cmd=[str(qemu),"-M",machine,"-accel","tcg,thread=single","-cpu","cortex-a76","-m","256M","-display","none","-nodefaults","-S","-gdb",f"unix:{gdb},server=on,wait=off","-qmp",f"unix:{qmp},server=on,wait=off","-device",f"loader,file={image},addr=0x40000000,force-raw=on,cpu-num=0","-device",f"loader,file={dtb},addr=0x47c80000,force-raw=on"]
    cmd+=["-device",f"loader,file={tags},addr=0x4f000100,force-raw=on"]
    if args.usb=="keyboard":cmd+=["-device","usb-kbd,id=kbd,bus=usb-bus.0,port=1"]
    if args.usb in ("storage","hub"):
        from media_fixture import make_media
        media=out/"test-usb-fat.img";media_metadata=make_media(b/"AARCH64/Shell.efi",media)
        cmd+=["-blockdev",json.dumps({"driver":"file","node-name":"stick-file","filename":str(media),"read-only":True}),
              "-blockdev",json.dumps({"driver":"raw","node-name":"stick","file":"stick-file","read-only":True}),
              "-trace",f"enable=usb_msd*,file={out}/usb-storage.trace"]
        if args.usb=="hub":
            cmd+=["-device","usb-hub,id=hub,bus=usb-bus.0,port=1","-device","usb-kbd,id=kbd,bus=usb-bus.0,port=1.2"]
        else:
            cmd+=["-device","usb-storage,id=storage,drive=stick,bus=usb-bus.0,port=1"]
    if args.ufs:
        from ufs_fixture import make_media
        media=out/"test-ufs-lu2.img";media_metadata=make_media(b/"AARCH64/Shell.efi",media)
        cmd += ["-blockdev",json.dumps({"driver":"file","node-name":"ufs-file","filename":str(media),"read-only":True}),
                "-blockdev",json.dumps({"driver":"raw","node-name":"ufsmedium","file":"ufs-file","read-only":True}),
                "-device","ufs-lu,id=ufslu2,drive=ufsmedium,bus=ufs0,lun=2",
                "-trace",f"enable=ufs*,file={out}/ufs.trace"]
        if args.ufs_boot_lus:
            # Zero-filled test data only; these are not captured boot partitions.
            for lun in (0, 1):
                boot_media=out/f"synthetic-ufs-lu{lun}.img"
                with boot_media.open("xb") as stream:stream.write(bytes(4*1024*1024))
                file_node=f"ufs-boot-file{lun}";raw_node=f"ufs-boot{lun}"
                cmd += ["-blockdev",json.dumps({"driver":"file","node-name":file_node,"filename":str(boot_media),"read-only":True}),
                        "-blockdev",json.dumps({"driver":"raw","node-name":raw_node,"file":file_node,"read-only":True}),
                        "-device",f"ufs-lu,id=ufslu{lun},drive={raw_node},bus=ufs0,lun={lun}"]
    if args.sd:
        from media_fixture import make_media
        media=out/"test-sd-fat.img";media_metadata=make_media(b/"AARCH64/Shell.efi",media)
        if args.sdhc:
            capacity=4*1024**3;tail=bytes(range(256))*2
            with media.open("r+b") as f:
                f.truncate(capacity);f.seek(capacity-512);f.write(tail)
            media_metadata.update(bytes=capacity,sha256=file_sha256(media),card_type="SDHC",last_sector_hex=tail.hex())
        else:media_metadata["card_type"]="SDSC"
        media_metadata.update(qemu_readonly=False,firmware_readonly_policy=True)
        cmd+=["-blockdev",json.dumps({"driver":"file","node-name":"sd-file","filename":str(media)}),
              "-blockdev",json.dumps({"driver":"raw","node-name":"sdmedium","file":"sd-file"}),
              "-device","sd-card,id=sd,drive=sdmedium,bus=sd-bus",
              "-trace",f"enable=sdcard_normal_command,file={out}/sd-command.trace"]
    report=dict(status="running",engine="QEMU TCG full-system",cpu="cortex-a76",el=args.el,command=cmd,artifact=dict(path=str(image),sha256=hashlib.sha256(image.read_bytes()).hexdigest()),boundaries=["CPU, privileged instructions, MMU, GICv3 and timers are QEMU implementations","PWRAP/MMIO responses and runtime DTB are synthetic fixtures","No physical tablet, Android, modem or user storage accessed"],milestones=[],snapshots=[])
    report["validation_scope"]="firmware execution against explicit virtual hardware fixtures"
    report["physical_hardware_validated"]=False
    if args.ufs_boot_lus:report["ufs_boot_lus"]=dict(logical_units=[0,1],bytes_each=4*1024*1024,synthetic_zero_filled=True,readonly=True)
    report["physical_upload_eligible"]=False
    report["usb_ready_protocol_injected"]=args.usb!="none" and not board_usb
    report["usb_power_path_fixture"]="board-cold-attach" if args.usb_board_cold_attach else args.usb_power_path
    report["usb_expect_active_rejection"]=args.usb_board_active
    report["usb_board_cold_attach"]=args.usb_board_cold_attach
    if args.usb_exit:report["usb_exit"]=args.usb_exit
    if board_usb:report["usb_board_fixture"]=usb_board_fixture
    report["qemu_binary"]=dict(path=str(qemu),sha256=hashlib.sha256(qemu.read_bytes()).hexdigest())
    report["harness_sha256"]={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in Path(__file__).parent.iterdir() if f.is_file() and f.suffix in (".py",".c",".inc",".patch",".json")}
    report["support_source_sha256"]={str(f.relative_to(m)):file_sha256(f) for f in (k/"verify-boot.py",k/"Linux/platform-modules.py")}
    if args.usb in ("storage","hub") or args.sd or args.ufs:report["media_fixture"]=media_metadata
    if ufs_host:
        report["ufs_board_fixture"]=ufs_fixture
        report["ufs_board_fixture"]["unit_enable_fixture"]={"0":1,"1":1,"2":2} if args.ufs else {}
        report["ufs_no_lu"]=args.ufs_no_lu
        report["ufs_board_fixture"]["hpb_acceleration_emulated"]=False
    log=(out/"qemu.log").open("w")
    proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
    try:
        for _ in range(200):
            if proc.poll() is not None:raise RuntimeError((out/"qemu.log").read_text())
            if gdb.exists() and qmp.exists():break
            time.sleep(.05)
        remote=Remote(gdb);monitor=Monitor(qmp)
        remote.send("qSupported");remote.setreg(0,0x47c80000);remote.setreg(33,0x3c0|(args.el<<2)|1,4)
        report["initial_registers"]=remote.regs()
        for name in ("SecEntry","PlatformInitialize","DecompressFirstFv","LoadDxeCoreFromFv"):
            address=0x44000000+sec[name];remote.breakpoint(address)
            response=remote.run_to_break();state=remote.regs()
            assert state["pc"]==address,(name,response,state)
            report["milestones"].append(dict(name=name,pc=address,registers=state))
            remote.breakpoint(address,False)
            print(json.dumps(report["milestones"][-1]),flush=True)
        for _ in range(args.seconds//3):
            remote.resume_for(3)
            report["snapshots"].append(remote.regs())
            print(json.dumps(dict(snapshot=report["snapshots"][-1])),flush=True)
        from firmware_probe import exercise
        exercise(remote,monitor,b,out,"ufs" if args.ufs else "sd" if args.sd else args.usb,report)
        report["cpu_state"]=monitor.hmp("info registers")
        monitor.hmp(f'pmemsave 0x49000000 0xc08000 "{out}/framebuffer.raw"')
        fb=(out/"framebuffer.raw").read_bytes()
        report["framebuffer"]=dict(bytes=len(fb),sha256=hashlib.sha256(fb).hexdigest(),nonzero_bytes=sum(c!=0 for c in fb))
        report["handoff_record"]=remote.memory(0x43f7f000,0x850).hex()
        monitor.hmp(f'pmemsave 0x44400000 0xbc00000 "{out}/dxe-memory.raw"')
        assert report["idle_framebuffer"]["unchanged"],"idle framebuffer changed"
        assert "UiApp" in report["loaded_modules"],"UI application did not load"
        sctlr=report["system_registers"].get(f"SCTLR_EL{args.el}", "0")
        assert int(sctlr,16)&0x1005==0x1005,("MMU/cache disabled",sctlr)
        assert len(report["timer_interrupts"]["hits"])==3
        if args.usb in ("storage","hub") or args.sd:
            media_after=file_sha256(media)
            assert media_after==report["media_fixture"]["sha256"],"virtual read-only disk changed"
            report["media_fixture"]["after_sha256"]=media_after
        if args.ufs:
            from ufs_fixture import media_digest
            assert media_digest(media)==report["media_fixture"]["sampled_content_sha256"],"virtual UFS changed"
            report["media_fixture"]["after_sampled_content_sha256"]=media_digest(media)
        report["status"]="passed"
        (out/"probe.json").write_text(json.dumps(report,indent=2)+"\n")
    except BaseException as e:
        report["status"]="failed";report["error"]=str(e)
        if "monitor" in locals():
            try:
                report["failure_cpu_state"]=monitor.hmp("info registers")
                monitor.hmp(f'pmemsave 0x49000000 0xc08000 "{out}/failure-framebuffer.raw"')
            except Exception as capture_error:report["failure_capture_error"]=str(capture_error)
        raise
    finally:
        (out/"execution.json").write_text(json.dumps(report,indent=2)+"\n")
        proc.terminate()
        try:proc.wait(timeout=3)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        log.close();socket_dir.cleanup()
    print(json.dumps({k:report[k] for k in ("status","framebuffer")}),flush=True)

if __name__=="__main__":main()
