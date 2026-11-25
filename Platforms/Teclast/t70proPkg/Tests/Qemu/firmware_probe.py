# SPDX-License-Identifier: BSD-2-Clause-Patent
"""QEMU observations; real readiness is the default, injected readiness is explicit."""
from pathlib import Path
import struct, re, json, uuid, hashlib, time, xml.etree.ElementTree as ET
from PIL import Image

def symbols(path):
    return {n:int(a,16) for a,n in re.findall(
        r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$",
        path.read_text(),re.M)}

def identify(memory, build):
    catalog={}
    for f in (build/"AARCH64").glob("*.efi"):
        d=f.read_bytes()
        if d[:2]!=b"MZ":continue
        pe=struct.unpack_from("<I",d,0x3c)[0]
        sh=pe+24+struct.unpack_from("<H",d,pe+20)[0]
        _,rva,size,raw=struct.unpack_from("<IIII",d,sh+8)
        catalog.setdefault((rva,d[raw:raw+128]),[]).append(f.stem)
    found={}
    for off in range(0,len(memory)-4096,4096):
        if memory[off:off+2]!=b"MZ":continue
        try:
            pe=off+struct.unpack_from("<I",memory,off+0x3c)[0]
            if memory[pe:pe+4]!=b"PE\0\0":continue
            sh=pe+24+struct.unpack_from("<H",memory,pe+20)[0]
            rva=struct.unpack_from("<I",memory,sh+12)[0]
            for name in catalog.get((rva,memory[off+rva:off+rva+128]),[]):
                found[name]=max(found.get(name,0),0x44400000+off)
        except struct.error:continue
    return found

class Probe:
    scratch=0x40100000
    def __init__(self,remote,monitor,build,out):
        self.r=remote;self.q=monitor;self.b=build;self.out=out
        self.images={};self.maps={};self.calls=[]
    def read64(self,addr):return struct.unpack("<Q",self.r.memory(addr,8))[0]
    def write(self,addr,data):
        for off in range(0,len(data),1024):
            part=data[off:off+1024]
            assert self.r.send(f"M{addr+off:x},{len(part):x}:"+part.hex())=="OK"
    def refresh(self):
        target=self.out/"loaded-memory.raw"
        self.q.hmp(f'pmemsave 0x44400000 0xbc00000 "{target}"')
        self.images=identify(target.read_bytes(),self.b)
        (self.out/"identified-modules.json").write_text(json.dumps(
            {n:hex(a) for n,a in sorted(self.images.items())},indent=2)+"\n")
    def address(self,module,name):
        if module not in self.maps:
            paths=list((self.b/"AARCH64").rglob(module+".map"))
            path=next(x for x in paths if "/DEBUG/" in str(x))
            self.maps[module]=symbols(path)
        return self.images[module]+self.maps[module][name]
    def call(self,module,name,*args):
        return self.call_address(self.address(module,name),module,name,*args)
    def application_boundary(self):
        # Resume naturally to an application-level event check. Services must
        # not be injected while an idle sleep has TPL_NOTIFY held.
        tpl=self.address("DxeCore","gEfiCurrentTpl")
        if self.read64(tpl)==4:return
        check=self.address("DxeCore","CoreCheckEvent")
        self.r.breakpoint(check)
        try:
            for _ in range(64):
                if self.r.regs()["pc"]==check:self.r.send("s")
                self.r.run_to_break(5)
                assert self.r.regs()["pc"]==check
                if self.read64(tpl)==4:return
            raise AssertionError("No application-level event boundary")
        finally:self.r.breakpoint(check,False)
    def call_address(self,address,module,name,*args):
        self.application_boundary()
        saved=self.r.send("g");state=self.r.regs()
        # Dedicated test scratch/stack lies in the reserved image-load window,
        # below the relocated SEC/DXE heap; no firmware instruction is patched.
        # A firmware PC may be visited inside a nested service before return.
        # Break in reserved mapped RAM so only this call's LR can reach it.
        ret=self.scratch+0xff000;self.r.breakpoint(ret)
        try:
            for i,arg in enumerate(args):self.r.setreg(i,arg)
            self.r.setreg(31,self.scratch+0xf0000)
            self.r.setreg(30,ret);self.r.setreg(32,address)
            response=self.r.run_to_break(20);result=self.r.regs()
            assert result["pc"]==ret,(name,response,result)
            self.calls.append(dict(module=module,name=name,result=hex(result["x0"]),entry_tpl=4))
            return result["x0"]
        finally:
            self.r.breakpoint(ret,False)
            assert self.r.send("G"+saved)=="OK"
    def framebuffer(self,name):
        path=self.out/(name+".raw")
        self.q.hmp(f'pmemsave 0x49000000 0xc08000 "{path}"')
        d=path.read_bytes()
        Image.frombytes("RGB",(1408,2240),d,"raw","BGRX").crop((0,0,1400,2240)).save(self.out/(name+".png"))
        return hashlib.sha256(d).hexdigest()
    def dirty_pages(self):
        path=self.out/"framebuffer-dirty-count.bin"
        self.q.hmp(f'pmemsave 0x1000f000 4 "{path}"')
        data=path.read_bytes();assert len(data)==4
        return int.from_bytes(data,"little")
    def handles(self,guid):
        s=self.scratch
        self.write(s+0x400,uuid.UUID(guid).bytes_le)
        self.write(s+0x420,bytes(16))
        rc=self.call("DxeCore","CoreLocateHandleBuffer",2,s+0x400,0,s+0x420,s+0x428)
        if rc==0:
            count=self.read64(s+0x420);pointer=self.read64(s+0x428)
            assert count<256,count
            return list(struct.unpack("<"+"Q"*count,self.r.memory(pointer,count*8)))
        assert rc==0x800000000000000e,hex(rc)
        return []
    def acpi_tables(self):
        # Read installed firmware tables through the live EFI system table.
        # This catches a successful standalone ASL build left out of the image.
        st=self.read64(self.address("DxeCore","gDxeCoreST"))
        assert self.r.memory(st,8)==b"IBI SYST","invalid EFI system table"
        count=self.read64(st+104);config=self.read64(st+112)
        assert 0<count<64
        entries=self.r.memory(config,24*count)
        acpi2=uuid.UUID("8868e871-e4f1-11d3-bc22-0080c73c8881").bytes_le
        roots=[struct.unpack_from("<Q",entries,i+16)[0] for i in range(0,len(entries),24)
               if entries[i:i+16]==acpi2]
        assert len(roots)==1,"ACPI 2.0 configuration table missing/duplicated"
        rsdp=self.r.memory(roots[0],36)
        assert rsdp[:8]==b"RSD PTR " and rsdp[15]>=2
        assert struct.unpack_from("<I",rsdp,20)[0]==36
        assert sum(rsdp[:20])%256==sum(rsdp)%256==0,"RSDP checksum"
        def table(address):
            head=self.r.memory(address,36);length=struct.unpack_from("<I",head,4)[0]
            assert 36<=length<=65536,("ACPI table length",length)
            data=self.r.memory(address,length)
            assert sum(data)%256==0,("ACPI checksum",head[:4])
            return data
        xsdt=table(struct.unpack_from("<Q",rsdp,24)[0])
        assert xsdt[:4]==b"XSDT" and (len(xsdt)-36)%8==0
        def table_key(data):
            signature=data[:4].decode("ascii")
            if signature=="SSDT":
                return signature+"-"+data[16:24].decode("ascii").rstrip("\x00 ")
            return signature
        installed={}
        for at in range(36,len(xsdt),8):
            address=struct.unpack_from("<Q",xsdt,at)[0];data=table(address)
            key=table_key(data)
            assert key not in installed,("duplicate ACPI table",key)
            installed[key]=(address,data)
        assert "FACP" in installed and "DSDT" not in installed
        fadt=installed["FACP"][1]
        dsdt=self.read64(installed["FACP"][0]+140) or struct.unpack_from("<I",fadt,40)[0]
        installed["DSDT"]=(dsdt,table(dsdt))
        expected_dir=self.b/"AARCH64/t70pro/AcpiTables/OUTPUT"
        expected={sig:expected_dir/"MT6789"/(name+".acpi")
            for sig,name in (("FACP","FADT"),("APIC","MADT"),("GTDT","GTDT"),("PPTT","PPTT"))}
        for path in sorted(expected_dir.glob("*.aml")):
            data=path.read_bytes()
            assert len(data)>=36 and data[:4] in (b"DSDT",b"SSDT"),path
            key=table_key(data)
            assert key not in expected,("duplicate compiled ACPI table",key)
            expected[key]=path
        assert "DSDT" in expected
        assert set(installed)==set(expected)|{"PRMT"},("installed ACPI table set",sorted(installed))
        # PRMT is generated at DXE time, rather than a compiled ASL/C table.
        # Verify the one configured module and both actual exported handlers.
        prm_address,prm=installed["PRMT"]
        assert prm[8]==0 and prm[36:52]==uuid.UUID("a16813b1-6789-4b71-90df-d68d15d18156").bytes_le
        module_offset,module_count=struct.unpack_from("<II",prm,52)
        assert module_count==1 and module_offset==60
        module=prm[module_offset:]
        revision,length=struct.unpack_from("<HH",module)
        assert revision==0 and length==len(module)
        assert module[4:20]==uuid.UUID("8b81c558-6f0c-47a0-9d6c-195930c4fd75").bytes_le
        handler_count=struct.unpack_from("<H",module,24)[0]
        handler_offset=struct.unpack_from("<I",module,26)[0]
        assert handler_count==2 and handler_offset==38
        assert handler_offset+handler_count*44==len(module)
        expected_handlers={"c9aef12d-e26c-4be7-b345-62c5f306d184":"MtkUfsPrmHandler",
                           "9f58ca73-6bc4-45b8-b69c-f33c6261d081":"MtkPmicPrmHandler"}
        handlers=[]
        for off in range(handler_offset,len(module),44):
            revision,size=struct.unpack_from("<HH",module,off)
            guid=str(uuid.UUID(bytes_le=module[off+4:off+20]))
            address=struct.unpack_from("<Q",module,off+20)[0]
            assert revision==0 and size==44 and guid in expected_handlers
            name=expected_handlers.pop(guid)
            assert address==self.address("MtkUfsPrm",name),("PRMT exported handler address",name)
            handlers.append(dict(guid=guid,name=name,address=hex(address)))
        assert not expected_handlers
        (self.out/"installed-PRMT.bin").write_bytes(prm)
        result={"PRMT":dict(address=hex(prm_address),bytes=len(prm),
            installed_sha256=hashlib.sha256(prm).hexdigest(),
            dynamic_module_and_handlers_validated=True,handlers=handlers,
            handler_execution_validated=False)}
        for sig,path in expected.items():
            address,actual=installed[sig];original=path.read_bytes()
            assert actual[:4]==original[:4] and len(actual)==len(original),(sig,len(actual),len(original))
            normalized=bytearray(actual)
            # AcpiTableDxe fixes FACS/DSDT addresses and the FADT checksum.
            fields=[(9,1),(36,8),(132,16)] if sig=="FACP" else []
            for off,size in fields:normalized[off:off+size]=original[off:off+size]
            assert normalized==original,("installed ACPI differs from current build",sig)
            (self.out/("installed-"+sig+".bin")).write_bytes(actual)
            result[sig]=dict(address=hex(address),bytes=len(actual),
                installed_sha256=hashlib.sha256(actual).hexdigest(),
                compiled_sha256=hashlib.sha256(original).hexdigest(),
                compiled_match_after_runtime_address_fixups=True,
                normalized_runtime_fields=fields)
        return dict(rsdp=hex(roots[0]),tables=result,
            method="live EFI configuration table -> RSDP -> XSDT -> FADT -> DSDT; checksums and compiled bytes compared",
            physical_hardware_validated=False)

    def timer_check(self):
        address=self.address("ArmTimerDxe","TimerInterruptHandler")
        observations=[]
        for _ in range(3):
            self.r.breakpoint(address)
            self.r.run_to_break(3)
            state=self.r.regs();assert state["pc"]==address,state
            observations.append(state)
            self.r.breakpoint(address,False);self.r.resume_for(.03)
        return dict(hits=observations,engine="QEMU architectural timer -> GICv3 -> real ArmTimerDxe")
    def sysregs(self):
        def xml(name):
            result="";off=0
            while True:
                s=self.r.send(f"qXfer:features:read:{name}:{off:x},800")
                assert s and s[0] in "ml",s
                result+=s[1:];off+=len(s)-1
                if s[0]=="l":return result
        top=xml("target.xml");result={};index=0
        for name in re.findall(r'<xi:include href="([^"]+)"',top):
            doc=ET.fromstring(xml(name))
            for reg in doc.iter("reg"):
                number=int(reg.attrib.get("regnum",index));index=number+1
                if reg.attrib["name"].upper() in {
                    "SCTLR_EL1","SCTLR_EL2","TCR_EL1","TCR_EL2","TTBR0_EL1","TTBR0_EL2",
                    "VBAR_EL1","VBAR_EL2","HCR_EL2","CNTHP_CTL_EL2","CNTV_CTL_EL0"}:
                    value=self.r.send(f"p{number:x}")
                    if value and not value.startswith("E"):
                        result[reg.attrib["name"]]=hex(int.from_bytes(bytes.fromhex(value),"little"))
        return result
    def prepare_usb(self,power_path="board-retained-source",expect_unready=False):
        if power_path in ("board-retained-source","board-cold-attach"):
            from usb_board_fixture import discover_usb
            return discover_usb(self,expect_unready)
        assert power_path in ("typec-source","id-low-hub","typec-only")
        flags,path={"id-low-hub":(0xef,2),"typec-source":(0x7f,1),"typec-only":(0x7f,3)}[power_path]
        s=self.scratch;self.write(s,bytes(0x400))
        guid=uuid.UUID("6c62f8ab-4ac1-4aa7-9d43-48bc48120d7e").bytes_le
        ready=bytearray(72)
        struct.pack_into("<IHHI",ready,0,int.from_bytes(b"MUSB","little"),3,72,flags)
        struct.pack_into("<QQQQI4BIHHI",ready,16,
            0x11200000,0x10000,0x11f40000,0x10000,160,16,1,1,0x30,path,0,0,0)
        self.write(s+0x20,guid);self.write(s+0x80,ready)
        rc=self.call("DxeCore","CoreInstallMultipleProtocolInterfaces",s,s+0x20,s+0x80,0)
        assert rc==0,("install ready fixture",hex(rc),self.calls)
        handle=self.read64(s)
        assert handle!=0
        rc=self.call("DxeCore","CoreConnectController",handle,0,0,1)
        return dict(ready="synthetic preconfigured power/PHY/FIFO producer",
                    power_path=power_path,flags=hex(flags),handle=hex(handle),connect_status=hex(rc),
                    host_started=self.r.memory(self.address("MusbHostDxe","mHostStarted"),1).hex())
    def idle(self):
        address=self.address("ArmCpuDxe","CpuSleep")
        self.r.breakpoint(address)
        try:
            self.r.run_to_break(5)
            assert self.r.regs()["pc"]==address
        finally:self.r.breakpoint(address,False)
    def key(self,code,*,wait_for_idle=True):
        sample=self.out/"key-latency-sample.raw"
        def pixels():
            self.q.hmp(f'pmemsave {0x49000000+640*5632:#x} {768*5632:#x} "{sample}"')
            return hashlib.sha256(sample.read_bytes()).digest()
        initial=pixels()
        self.r.send("c",False)
        deadline=time.monotonic()+2
        while not self.q.command("query-status")["running"]:
            assert time.monotonic()<deadline,"QEMU did not resume before key injection"
            time.sleep(.001)
        begin=time.monotonic()
        self.q.command("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":code}}}])
        latency=None
        while time.monotonic()-begin<1:
            time.sleep(.005)
            if pixels()!=initial:
                latency=(time.monotonic()-begin)*1000;break
        time.sleep(max(0,.12-(time.monotonic()-begin)))
        self.q.command("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":code}}}])
        time.sleep(.15);self.r.s.sendall(b"\x03");self.r.receive()
        if wait_for_idle:self.idle()
        return latency

    def protocol(self,handle,guid):
        s=self.scratch;self.write(s+0x500,uuid.UUID(guid).bytes_le);self.write(s+0x520,bytes(8))
        rc=self.call("DxeCore","CoreHandleProtocol",handle,s+0x500,s+0x520)
        assert rc==0,("HandleProtocol",guid,hex(rc),hex(handle))
        result=self.read64(s+0x520);assert result;return result
    def file_call(self,file,offset,name,*args):
        return self.call_address(self.read64(file+offset),"Fat",name,file,*args)
    def storage_read(self,handle,expected):
        s=self.scratch;fs=self.protocol(handle,"964e5b22-6459-11d2-8e39-00a0c969723b")
        assert self.call_address(self.read64(fs+8),"Fat","OpenVolume",fs,s+0x600)==0
        root=self.read64(s+0x600)
        self.write(s+0x1000,("\\EFI\\BOOT\\BOOTAA64.EFI\0").encode("utf-16le"))
        assert self.file_call(root,8,"Open",s+0x608,s+0x1000,1,0)==0
        file=self.read64(s+0x608);self.write(s+0x610,struct.pack("<Q",len(expected)))
        assert self.file_call(file,32,"Read",s+0x610,s+0x10000)==0
        size=self.read64(s+0x610);data=self.r.memory(s+0x10000,size)
        assert size==len(expected) and data==expected,("EFI file bytes differ",size,len(expected))
        assert self.file_call(file,16,"Close")==0
        self.write(s+0x1000,("\\EFI\\BOOT\\MISSING.EFI\0").encode("utf-16le"))
        missing=self.file_call(root,8,"Open-missing",s+0x608,s+0x1000,1,0)
        assert missing==0x800000000000000e,hex(missing)
        assert self.file_call(root,16,"Close-root")==0
        block=self.protocol(handle,"964e5b21-6459-11d2-8e39-00a0c969723b")
        media=self.read64(block+8);raw=self.r.memory(media,48)
        media_id=struct.unpack_from("<I",raw)[0];block_size=struct.unpack_from("<I",raw,12)[0]
        assert raw[7] == 1, "read-only SD/USB media advertised writable"
        write_rc=self.call_address(self.read64(block+32),"BlockIo","WriteBlocks-readonly",block,media_id,0,block_size,s+0x10000)
        assert write_rc==0x8000000000000008,("read-only media write accepted or wrong error",hex(write_rc))
        return dict(bytes=size,sha256=hashlib.sha256(data).hexdigest(),exact_source_match=True,
                    missing_file_status=hex(missing),media_readonly=bool(raw[7]),write_status=hex(write_rc))
    def boot_external(self,handle,report):
        s=self.scratch
        source_dp=self.protocol(handle,"09576e91-6d3f-11d2-8e39-00a0c969723b")
        dp=bytearray();at=source_dp
        for _ in range(64):
            hdr=self.r.memory(at,4);length=struct.unpack_from("<H",hdr,2)[0]
            assert 4<=length<=4096
            if hdr[0]==0x7f:break
            dp+=self.r.memory(at,length);at+=length
        else:raise AssertionError("unterminated external device path")
        filename=("\\EFI\\BOOT\\BOOTAA64.EFI\0").encode("utf-16le")
        dp+=struct.pack("<BBH",4,4,4+len(filename))+filename+b"\x7f\xff\x04\x00"
        self.write(s+0x2000,dp);self.write(s+0x6000,bytes(24))
        parent=self.read64(self.address("UiApp","gImageHandle"))
        rc=self.call("DxeCore","CoreLoadImage",0,parent,s+0x2000,0,0,s+0x6000)
        assert rc==0,("external CoreLoadImage",hex(rc))
        image_handle=self.read64(s+0x6000)
        loaded=self.protocol(image_handle,"5b1b31a1-9562-11d2-8e3f-00a0c969723b")
        base=self.read64(loaded+64);size=self.read64(loaded+72)
        shell_map=next(x for x in (self.b/"AARCH64").rglob("Shell.map") if "/DEBUG/" in str(x))
        syms=symbols(shell_map);entry=base+syms["_ModuleEntryPoint"]
        prompt=base+syms["DoShellPrompt"]
        start=self.address("DxeCore","CoreStartImage")
        self.r.setreg(0,image_handle);self.r.setreg(1,s+0x6008);self.r.setreg(2,s+0x6010)
        self.r.setreg(31,s+0xf0000);self.r.setreg(30,self.r.regs()["pc"]);self.r.setreg(32,start)
        observations=[]
        for name,address in (("Shell._ModuleEntryPoint",entry),("Shell.DoShellPrompt",prompt)):
            self.r.breakpoint(address)
            try:
                response=self.r.run_to_break(30);state=self.r.regs()
                assert state["pc"]==address,(name,response,state)
                observations.append(dict(name=name,pc=hex(address),registers=state))
            finally:self.r.breakpoint(address,False)
        self.r.resume_for(1)
        report["external_EFI_boot"]=dict(path="\\EFI\\BOOT\\BOOTAA64.EFI",
            image_handle=hex(image_handle),image_base=hex(base),image_size=size,
            milestones=observations,framebuffer=self.framebuffer("external-shell"),
            launch="real CoreLoadImage from USB device path + CoreStartImage; no source buffer",
            kernel_booted=False)

def check_front_page_keys(p):
    front_page=uuid.UUID("9e0c30bc-3f06-4ba6-8288-09179b855dbe")
    boot_manager=uuid.UUID("847bc3fe-b974-446d-9449-5ad5412e993b")
    display_form=p.address("SetupBrowser","gDisplayFormData")
    def formset():
        # FormSetGuid in the AArch64 FORM_DISPLAY_ENGINE_FORM layout.
        return uuid.UUID(bytes_le=p.r.memory(display_form+56,16))

    assert formset()==front_page,"fixture should begin in the front page"
    keys=[]
    # Boot Manager follows the six RAM snapshot rows, language and Device Manager.
    for code in ("down",)*8+("ret","esc"):
        latency=p.key(code)
        current=formset()
        expected=boot_manager if code=="ret" else front_page
        assert current==expected,(code,str(current),str(expected))
        assert latency is not None,("no visible key response within 1 s QEMU wall time",code)
        keys.append(dict(qcode=code,latency_ms=latency,formset=str(current)))
    return keys

def exercise(remote,monitor,build,out,mode,report):
    p=Probe(remote,monitor,build,out);report["probe_calls"]=p.calls;p.refresh()
    report["loaded_modules"]={n:hex(a) for n,a in sorted(p.images.items())}
    report["installed_acpi"]=p.acpi_tables()
    report["system_registers"]=p.sysregs()
    report["timer_interrupts"]=p.timer_check()
    p.idle()
    before=p.framebuffer("idle-before")
    dirty_initial=p.dirty_pages()
    remote.resume_for(1)
    dirty_idle=p.dirty_pages()
    report["idle_framebuffer_writes"]=dict(initial_dirty_pages=dirty_initial,idle_dirty_pages=dirty_idle,
        interval_seconds=1,method="QEMU RAM dirty bitmap, counts identical-value writes too")
    assert dirty_initial>0 and dirty_idle==0,("idle framebuffer was rewritten",dirty_initial,dirty_idle)
    after=p.framebuffer("idle-after")
    report["idle_framebuffer"]=dict(before=before,after=after,unchanged=before==after)
    if report.get("ufs_no_lu"):
        assert "MtkUfsHandoffDxe" in p.images, "UFS host driver did not load"
        ufs_blocks = []
        ufs_driver = "MtkUfsReadOnlyBlockIoDxe"
        if ufs_driver in p.images:
            block_guid = "964e5b21-6459-11d2-8e39-00a0c969723b"
            ufs_read = p.address(ufs_driver, "MtkUfsReadBlocks")
            ufs_blocks = [hex(h) for h in p.handles(block_guid) if p.read64(p.protocol(h, block_guid)+24)==ufs_read]
        report["ufs_blocks"] = ufs_blocks
        assert not ufs_blocks, ("Empty UFS host published BlockIO", ufs_blocks)
    if mode=="sd":
        fs_guid="964e5b22-6459-11d2-8e39-00a0c969723b"
        pass_guid="716ef0d9-ff83-4f69-81e9-518bd39a8e70"
        report["sd_passthrough_handles"]=[hex(x) for x in p.handles(pass_guid)]
        report["sd_filesystems"]=[hex(x) for x in p.handles(fs_guid)]
        report["MSDC_counters"]=list(struct.unpack("<5I",remote.memory(0x11240f00,20)))
        (out/"sd-before-read.json").write_text(json.dumps(report,indent=2)+"\n")
        if not report["sd_filesystems"]:
            scratch=p.scratch
            report["sd_diagnostics"]={}
            d=report["sd_diagnostics"]
            d["detect_mmio"]=remote.memory(0x10005200,4).hex()
            d["get_present"]=hex(p.call("MsdcDxe","GetCardPresent",1,scratch+0x700))
            d["present"]=remote.memory(scratch+0x700,1).hex()
            if report["sd_passthrough_handles"]:
                ptr=p.protocol(int(report["sd_passthrough_handles"][0],16),pass_guid)-16
                d["private_address"]=hex(ptr)
                d["private_bytes"]=remote.memory(ptr,240).hex()
                d["init"]=hex(p.call("MsdcDxe","MsdcInit",ptr))
                d["power"]=hex(p.call("MsdcDxe","PowerControl",1,1))
                d["clock"]=hex(p.call("MsdcDxe","GetSourceClockRate",1,scratch+0x708))
                d["clock_hz"]=p.read64(scratch+0x708)
            (out/"sd-diagnostics.json").write_text(json.dumps(report,indent=2)+"\n")
        assert report["sd_filesystems"],("SD FAT filesystem did not mount",report["MSDC_counters"],report.get("sd_diagnostics"))
        first=int(report["sd_filesystems"][0],16)
        report["sd_file_read"]=p.storage_read(first,(build/"AARCH64/Shell.efi").read_bytes())
        block=p.protocol(first,"964e5b21-6459-11d2-8e39-00a0c969723b")
        media=remote.memory(p.read64(block+8),48)
        media_id=struct.unpack_from("<I",media)[0]
        block_size=struct.unpack_from("<I",media,12)[0]
        last_block=struct.unpack_from("<Q",media,24)[0]
        assert block_size==512 and (last_block+1)*block_size==report["media_fixture"]["bytes"]
        capacity=dict(block_size=block_size,last_block=last_block,bytes=(last_block+1)*block_size)
        s=p.scratch
        rc=p.call_address(p.read64(block+24),"SdDxe","ReadBlocks-last-sector",block,media_id,last_block,512,s+0x10000)
        assert rc==0,hex(rc)
        tail=remote.memory(s+0x10000,512)
        if "last_sector_hex" in report["media_fixture"]:
            assert tail.hex()==report["media_fixture"]["last_sector_hex"],"SDHC last sector differs"
        capacity["last_sector_sha256"]=hashlib.sha256(tail).hexdigest()
        rc=p.call_address(p.read64(block+24),"SdDxe","ReadBlocks-past-end",block,media_id,last_block+1,512,s+0x10000)
        assert rc==0x8000000000000002,hex(rc)
        capacity["past_end_status"]=hex(rc)
        report["sd_capacity"]=capacity
        monitor.command("blockdev-remove-medium",id="sd")
        remote.resume_for(2);p.idle()
        report["sd_removed_filesystems"]=[hex(x) for x in p.handles(fs_guid)]
        if report["sd_removed_filesystems"]:
            scratch=p.scratch
            ptr=p.protocol(int(report["sd_passthrough_handles"][0],16),pass_guid)-16
            d=report["sd_remove_diagnostics"]={}
            d["detect_mmio"]=remote.memory(0x10005200,4).hex()
            d["query_block"]=monitor.command("query-block")
            d["private_before"]=remote.memory(ptr,184).hex()
            d["get_child_state"]=hex(p.call("MsdcDxe","MsdcGetChildState",ptr,scratch+0x730))
            d["child_state"]=remote.memory(scratch+0x730,4).hex()
            p.write(scratch+0x720,uuid.UUID(pass_guid).bytes_le)
            d["manual_reinstall"]=hex(p.call("DxeCore","CoreReinstallProtocolInterface",int(report["sd_passthrough_handles"][0],16),scratch+0x720,ptr+16,ptr+16))
            d["private_after"]=remote.memory(ptr,184).hex()
            d["filesystems_after_manual"]=[hex(x) for x in p.handles(fs_guid)]
            (out/"sd-remove-diagnostics.json").write_text(json.dumps(report,indent=2)+"\n")
        assert not report["sd_removed_filesystems"],"SD filesystem survived card removal"
        monitor.command("blockdev-insert-medium",id="sd",**{"node-name":"sdmedium"})
        remote.resume_for(3);p.idle()
        reinserted=p.handles(fs_guid)
        report["sd_reinserted_filesystems"]=[hex(x) for x in reinserted]
        assert reinserted,"SD card did not remount after removal"
        # DXE pool allocation may reuse an address after the old handle is gone.
        report["sd_handle_address_reused"]=hex(reinserted[0])==report["sd_filesystems"][0]
        report["sd_read_after_reinsert"]=p.storage_read(reinserted[0],(build/"AARCH64/Shell.efi").read_bytes())
        p.boot_external(reinserted[0],report)
        report["external_EFI_boot"]["launch"]="real CoreLoadImage from SD device path + CoreStartImage; no source buffer"
        report["MSDC_counters_after"]=list(struct.unpack("<5I",remote.memory(0x11240f00,20)))
    if mode=="ufs":
        from ufs_fixture import exercise
        exercise(p,report)
    if mode not in ("none","sd","ufs"):
        p.idle();report["usb_fixture"]=p.prepare_usb(report["usb_power_path_fixture"],report.get("usb_expect_active_rejection",False))
        if report.get("usb_expect_active_rejection"):
            report["driver_calls"]=p.calls
            return
        remote.resume_for(2);p.idle()
        report["usb_fixture"]["host_started_after_dispatch"]=remote.memory(p.address("MusbHostDxe","mHostStarted"),1).hex()
        handle=int(report["usb_fixture"]["handle"],16)
        report["usb_fixture"]["connect_after_dispatch"]=hex(p.call("DxeCore","CoreConnectController",handle,0,0,1))
        remote.resume_for(2);p.idle()
        report["usb_bus"]=monitor.hmp("info usb")
        report["USB_IO_handles"]=[hex(x) for x in p.handles("2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75")]
        report["MUSB_MMIO_counters_before_keys"]=list(struct.unpack("<6I",remote.memory(0x1120f000,24)))
        (out/"usb-before-keys.json").write_text(json.dumps(report,indent=2)+"\n")
        assert report["USB_IO_handles"],("no USB device enumerated",report["usb_fixture"],report["MUSB_MMIO_counters_before_keys"])
        report["SimpleTextIn_handles"]=[hex(x) for x in p.handles("387477c1-69c7-11d2-8e39-00a0c969723b")]
        p.framebuffer("usb-connected")
        if mode=="keyboard":
            report["keys"]=check_front_page_keys(p)
            monitor.command("device_del",id="kbd");remote.resume_for(2);p.idle()
            report["detached_USB_IO_handles"]=[hex(x) for x in p.handles("2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75")]
            assert not report["detached_USB_IO_handles"],"USB handle survived detach"
            monitor.command("device_add",driver="usb-kbd",id="kbd2",bus="usb-bus.0",port="1")
            remote.resume_for(2);p.idle()
            report["reattached_USB_IO_handles"]=[hex(x) for x in p.handles("2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75")]
            assert report["reattached_USB_IO_handles"] and report["reattached_USB_IO_handles"]!=report["USB_IO_handles"],"no new handle after reattach"
            before_rekey=p.framebuffer("reconnected-before")
            p.key("down");report["reconnected_key_framebuffer"]=p.framebuffer("reconnected-down")
            assert before_rekey!=report["reconnected_key_framebuffer"],"reconnected keyboard did not move menu"
        if mode=="storage":
            report["SimpleFileSystem_handles"]=[hex(x) for x in p.handles("964e5b22-6459-11d2-8e39-00a0c969723b")]
            report["BlockIO_handles"]=[hex(x) for x in p.handles("964e5b21-6459-11d2-8e39-00a0c969723b")]
            assert report["SimpleFileSystem_handles"],"USB storage did not mount FAT"
            fs_handle=int(report["SimpleFileSystem_handles"][0],16)
            report["storage_file_read"]=p.storage_read(fs_handle,(build/"AARCH64/Shell.efi").read_bytes())
            monitor.command("device_del",id="storage");remote.resume_for(2);p.idle()
            report["storage_detached_filesystems"]=[hex(x) for x in p.handles("964e5b22-6459-11d2-8e39-00a0c969723b")]
            assert not report["storage_detached_filesystems"],"filesystem survived storage detach"
            monitor.command("device_add",driver="usb-storage",id="storage2",drive="stick",bus="usb-bus.0",port="1")
            remote.resume_for(2);p.idle()
            reattached=p.handles("964e5b22-6459-11d2-8e39-00a0c969723b")
            report["storage_reattached_filesystems"]=[hex(x) for x in reattached]
            assert reattached and hex(reattached[0])!=report["SimpleFileSystem_handles"][0],"no new filesystem on reattach"
            report["storage_read_after_reattach"]=p.storage_read(reattached[0],(build/"AARCH64/Shell.efi").read_bytes())
            p.boot_external(reattached[0],report)
        if mode=="hub":
            report["combined_topology"]="one QEMU USB hub with storage on port 1.1 and keyboard on 1.2"
            fs_guid="964e5b22-6459-11d2-8e39-00a0c969723b"
            io_guid="2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75"
            initial_interfaces=set(p.handles(io_guid))
            assert len(initial_interfaces)==2 and not p.handles(fs_guid),initial_interfaces
            before_otg=[p.key(code) for code in ("down","up")]
            assert all(latency is not None for latency in before_otg),"pogo keyboard did not work before OTG attached"
            report["keyboard_before_otg_ms"]=before_otg

            monitor.command("device_add",driver="usb-storage",id="storage",drive="stick",bus="usb-bus.0",port="1.1")
            remote.resume_for(2);p.idle()
            interfaces=set(p.handles(io_guid))
            filesystems=p.handles(fs_guid)
            report["combined_filesystems"]=[hex(x) for x in filesystems]
            report["combined_interfaces_after_otg"]=[hex(x) for x in sorted(interfaces)]
            (out/"combined-before-files.json").write_text(json.dumps(report,indent=2)+"\n")
            assert initial_interfaces < interfaces,("hub or keyboard changed when OTG attached",initial_interfaces,interfaces)
            assert len(filesystems)==1,"storage behind hub did not mount FAT"
            expected=(build/"AARCH64/Shell.efi").read_bytes()
            report["combined_file_read"]=p.storage_read(filesystems[0],expected)
            report["combined_keys"]=check_front_page_keys(p)
            storage_id="storage";keyboard_id="kbd"
            report["combined_reconnect_cycles"]=[]
            for cycle in range(3):
                monitor.command("device_del",id=storage_id);remote.resume_for(2);p.idle()
                assert not p.handles(fs_guid),(cycle,"removed storage filesystem survived behind hub")
                key_without_storage=p.key("down")
                assert key_without_storage is not None,(cycle,"keyboard stopped after storage removal")
                storage_id=f"storage{cycle+2}"
                monitor.command("device_add",driver="usb-storage",id=storage_id,drive="stick",bus="usb-bus.0",port="1.1")
                remote.resume_for(2);p.idle()
                reattached=p.handles(fs_guid)
                assert len(reattached)==1,(cycle,"storage did not return while keyboard remained attached")
                storage_read=p.storage_read(reattached[0],expected)
                monitor.command("device_del",id=keyboard_id);remote.resume_for(2);p.idle()
                remaining=p.handles(fs_guid)
                assert remaining==reattached,(cycle,"storage filesystem changed when keyboard was removed")
                read_without_keyboard=p.storage_read(remaining[0],expected)
                keyboard_id=f"kbd{cycle+2}"
                monitor.command("device_add",driver="usb-kbd",id=keyboard_id,bus="usb-bus.0",port="1.2")
                remote.resume_for(2);p.idle()
                key_after_reattach=p.key("down")
                assert key_after_reattach is not None,(cycle,"keyboard did not return while storage remained attached")
                read_after_keyboard=p.storage_read(remaining[0],expected)
                assert len(p.handles("2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75"))>=3,cycle
                report["combined_reconnect_cycles"].append(dict(cycle=cycle+1,
                    key_without_storage_ms=key_without_storage,
                    storage_read=storage_read,read_without_keyboard=read_without_keyboard,
                    key_after_reattach_ms=key_after_reattach,
                    read_after_keyboard=read_after_keyboard))
            report["combined_storage_reattach_read"]=storage_read
            report["combined_after_keyboard_detach_read"]=read_without_keyboard
            report["combined_after_keyboard_reattach_read"]=read_after_keyboard
            boot_handle=remaining[0]
            if report.get('usb_board_cold_attach'):
                from usb_board_fixture import exercise_typec_id_change
                boot_handle=exercise_typec_id_change(p,report,expected)
            elif report['usb_power_path_fixture']=='board-retained-source':
                from usb_board_fixture import exercise_board_route
                boot_handle=exercise_board_route(p,report,expected)
            p.boot_external(boot_handle,report)
        report["MUSB_MMIO_counters"]=list(struct.unpack("<6I",remote.memory(0x1120f000,24)))
        if report.get("usb_exit"):
            from usb_board_fixture import exercise_exit_boot_services
            exercise_exit_boot_services(p,report)
    report["probe_calls"]=p.calls
    (out/"probe.json").write_text(json.dumps(report,indent=2)+"\n")
