# SPDX-License-Identifier: BSD-2-Clause-Patent
"""QEMU UFS fixture: real HC/SCSI/FAT path, explicit inherited-power boundary."""
import hashlib,json,runpy,struct

CAPACITY=127959826432
BLOCK_SIZE=4096
HEAD_BYTES=8*1024*1024
TAIL=bytes(range(256))*16

def runtime_tree(base,k,p):
    parse=runpy.run_path(str(k/'Linux/platform-modules.py'))['tree']
    nodes,_=parse(base)
    original=p/'analysis/stock-fdt/effective.dtb'
    stock=original.read_bytes()
    assert hashlib.sha256(stock).hexdigest()=='a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391'
    src,ph= parse(stock)
    props=src['/soc/ufshci@11270000']['properties']
    vcc=ph[int.from_bytes(props['vcc-supply'],'big')]
    paths=['/soc','/soc/ufshci@11270000','/syscon@10001000',
           '/syscon@10001000/reset-controller','/syscon@10000000',vcc,
           '/interrupt-controller']
    for path in paths:
        chain=[];at=path
        while at not in nodes:
            chain.append(at);at=at.rpartition('/')[0] or '/'
        for item in reversed(chain):
            nodes[item]={'parent':item.rpartition('/')[0] or '/', 'properties':{}}
        nodes[path]['properties']=dict(src[path]['properties'])
    nodes['/']['properties']['compatible']=src['/']['properties']['compatible']
    nodes['/']['properties']['interrupt-parent']=src['/']['properties']['interrupt-parent']
    strings=bytearray();names={}
    cells=lambda *v:struct.pack('>'+len(v)*'I',*v)
    pad=lambda b:b+bytes(-len(b)%4)
    def node(path):
        data=cells(1)+pad((path.rsplit('/',1)[-1] if path!='/' else '').encode()+b'\0')
        for name,value in nodes[path]['properties'].items():
            if name not in names:names[name]=len(strings);strings.extend(name.encode()+b'\0')
            data+=cells(3,len(value),names[name])+pad(value)
        return data+b''.join(node(child) for child in nodes if nodes[child]['parent']==path)+cells(2)
    structure=node('/')+cells(9)
    # Preserve the base fixture's actual reservation map, including terminator.
    pos=struct.unpack_from('>I',base,16)[0];start=pos
    while struct.unpack_from('>QQ',base,pos)!=(0,0):pos+=16
    reserves=base[start:pos+16];off=40+len(reserves);end=off+len(structure)
    tree=cells(0xd00dfeed,end+len(strings),off,end,40,17,16,0,len(strings),len(structure))+reserves+structure+strings
    parse(tree) # malformed / colliding phandles must fail before execution
    return tree,dict(stock_dtb_sha256=hashlib.sha256(stock).hexdigest(),source_nodes=paths,
        runtime_dtb_sha256=hashlib.sha256(tree).hexdigest(),
        boundary='Original UFS DT supplier subset; synthetic retained clocks/VEMC/IP/link. No MediaTek PHY or SMC emulation.')

def media_digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        h.update(f.read(HEAD_BYTES));f.seek(CAPACITY-BLOCK_SIZE);h.update(f.read(BLOCK_SIZE))
    return h.hexdigest()

def make_media(efi,target):
    shell=efi.read_bytes();sector=BLOCK_SIZE;total=HEAD_BYTES//sector
    disk=bytearray(HEAD_BYTES);disk[:11]=b'\xeb\x3c\x90T70UFS  '
    struct.pack_into('<HBHBHHBHHHII',disk,11,sector,1,1,2,512,total,0xf8,1,18,2,0,0)
    disk[36:62]=bytes.fromhex('80 00 29 12 34 56 78')+b'T70UFSTEST '+b'FAT12   '
    disk[510:512]=b'\x55\xaa';fat=bytearray(sector)
    def entry(c,value):
        off=c*3//2;old=int.from_bytes(fat[off:off+2],'little')
        val=(old&0xf)|(value<<4) if c&1 else (old&0xf000)|value
        fat[off:off+2]=val.to_bytes(2,'little')
    for c,v in [(0,0xff8),(1,0xfff),(2,0xfff),(3,0xfff)]:entry(c,v)
    count=(len(shell)+sector-1)//sector
    assert 4+count<total-7
    for i in range(count):entry(4+i,0xfff if i+1==count else 5+i)
    disk[sector:2*sector]=fat;disk[2*sector:3*sector]=fat
    def dirent(name,c,size=0,directory=False):
        d=bytearray(32);d[:11]=name;d[11]=0x10 if directory else 0x20
        struct.pack_into('<H',d,26,c);struct.pack_into('<I',d,28,size);return d
    disk[3*sector:3*sector+32]=dirent(b'EFI        ',2,directory=True)
    for c,parent,name,child,size,isdir in [(2,0,b'BOOT       ',3,0,True),(3,2,b'BOOTAA64EFI',4,len(shell),False)]:
        off=(7+c-2)*sector
        for n,item in enumerate([dirent(b'.          ',c,directory=True),dirent(b'..         ',parent,directory=True),dirent(name,child,size,isdir)]):
            disk[off+32*n:off+32*(n+1)]=item
    disk[9*sector:9*sector+len(shell)]=shell
    assert len(disk)==HEAD_BYTES
    with target.open('wb') as f:
        f.write(disk);f.truncate(CAPACITY);f.seek(CAPACITY-sector);f.write(TAIL)
    return dict(path=str(target),bytes=CAPACITY,logical_block_size=sector,lun=2,
        sampled_content_sha256=media_digest(target),hash_scope='8 MiB FAT head plus final 4 KiB; backing node read-only; holes not hashed',
        boot_file='\\EFI\\BOOT\\BOOTAA64.EFI',boot_file_bytes=len(shell),
        boot_file_sha256=hashlib.sha256(shell).hexdigest(),qemu_readonly=True)

def unit_descriptor_probe(p):
    """Read real virtual-device descriptors through the guest config protocol."""
    guid='b81bfab0-0eb3-4cf9-8465-7fa986361664'
    handles=p.handles(guid)
    assert len(handles)==1,handles
    protocol=p.protocol(handles[0],guid)
    scratch=p.scratch+0x90000
    result=[]
    for lun in range(3):
        p.write(scratch,bytes(256));p.write(scratch+0x100,struct.pack('<I',255))
        status=p.call_address(p.read64(protocol),'UfsPassThruDxe','ReadUnitDescriptor',protocol,1,2,lun,0,scratch,scratch+0x100)
        size=struct.unpack('<I',p.r.memory(scratch+0x100,4))[0]
        item=dict(lun=lun,status=hex(status),returned_bytes=size)
        if status==0:
            raw=p.r.memory(scratch,min(size,255))
            assert len(raw)>=19 and raw[1]==2 and raw[2]==lun
            item.update(enable=raw[3],boot_lun_id=raw[4],block_shift=raw[10],
                blocks=int.from_bytes(raw[11:19],'big'))
        result.append(item)
    return result

def scsi_response_probe(p):
    """Observe the real validator's DMA input; never alter response bytes."""
    guid='143b7632-b81b-4cb7-abd3-b625a5b9bffe'
    p.application_boundary()
    handles=p.handles(guid)
    if not handles:return dict(available=False)
    protocol=p.protocol(handles[0],guid);s=p.scratch+0x80000
    p.write(s,bytes(4096))
    p.write(s+0x100,bytes([0x12,0,0,0,96,0]))
    p.write(s+0x200,struct.pack('<QQQQQII5B3x',30000000,s+0x400,0,s+0x300,s+0x100,96,0,6,0,0,0,18))
    saved=p.r.send('g');ret=p.scratch+0xff000
    entry=p.address('UfsPassThruDxe','UfsValidateScsiResponse')
    result=dict(available=True)
    p.r.breakpoint(ret);p.r.breakpoint(entry)
    entry_armed=True
    try:
        for i,value in enumerate((protocol,s,0x200,s+0x200,0)):p.r.setreg(i,value)
        p.r.setreg(31,p.scratch+0xf0000);p.r.setreg(30,ret)
        p.r.setreg(32,p.address('UfsPassThruDxe','UfsPassThruPassThru'))
        p.r.run_to_break(20);regs=p.r.regs()
        if regs['pc']==ret:
            result.update(validator_reached=False,status=hex(regs['x0']))
            return result
        assert regs['pc']==entry,('SCSI response validator not reached',regs)
        result['validator_reached']=True
        tr=regs['x0'];command=p.read64(tr+48)
        request=p.r.memory(command,32);response=p.r.memory(command+32,52)
        result.update(request_hex=request.hex(),response_hex=response.hex(),
            transaction_type=response[0]&0x3f,lun=response[2],task_tag=response[3],
            data_segment_length=int.from_bytes(response[10:12],'big'),
            sense_data_length=int.from_bytes(response[32:34],'big'))
        p.r.breakpoint(entry,False);entry_armed=False;p.r.run_to_break(20)
        regs=p.r.regs();assert regs['pc']==ret,regs
        result['status']=hex(regs['x0']);result['packet_hex']=p.r.memory(s+0x200,56).hex()
        return result
    finally:
        if entry_armed:p.r.breakpoint(entry,False)
        p.r.breakpoint(ret,False)
        assert p.r.send('G'+saved)=='OK'

def exercise(p,report):
    fs_guid='964e5b22-6459-11d2-8e39-00a0c969723b'
    block_guid='964e5b21-6459-11d2-8e39-00a0c969723b'
    filesystems=p.handles(fs_guid);blocks=p.handles(block_guid)
    report['ufs_filesystems']=[hex(x) for x in filesystems]
    report['ufs_blocks']=[hex(x) for x in blocks]
    report['ufs_registers']=p.r.memory(0x11270000,0xa0).hex()
    report['ufs_unit_descriptors']=unit_descriptor_probe(p)
    assert report['ufs_unit_descriptors'][2]['enable']==2
    report['ufs_scsi_wire_probe']=scsi_response_probe(p)
    (p.out/'ufs-before-read.json').write_text(json.dumps(report,indent=2)+'\n')
    if not filesystems or not blocks:
        d=report['ufs_failure_diagnostics']={}
        d['handoff_header']=p.r.memory(0x43f7f000,0x100).hex()
        ctx=p.address('MtkUfsHandoffDxe','mContext')
        d['context']=p.r.memory(ctx,512).hex()
        d['board_resources_status']=hex(p.call('MtkUfsHandoffDxe','MtkUfsValidateBoardResources',p.scratch+0x700,p.scratch+0x708))
        tree=p.read64(p.scratch+0x700);d['runtime_fdt']=hex(tree)
        if tree:
            p.write(p.scratch+0x70000,bytes(4096))
            d['retained_resources_status']=hex(p.call('MtkUfsHandoffDxe','MtkUfsValidateRetainedHost',tree,p.scratch+0x70000))
        d['cold_host']=p.r.memory(p.scratch+0x70000,512).hex()
        d['clocks']=[]
        for name in ['IFRAO_UFS','IFRAO_UFS_SAP_BCLK','IFRAO_UNIPRO_TICK','IFRAO_UNIPRO_SYSCLK','IFRAO_UFS_AES','TOP_UFS']:
            p.write(p.scratch+0x800,name.encode()+b'\0')
            rc=p.call('ClockDxe','ClockGetId',p.scratch+0x800,p.scratch+0x880)
            item=dict(name=name,id_status=hex(rc));d['clocks'].append(item)
            if rc==0:
                cid=int.from_bytes(p.r.memory(p.scratch+0x880,4),'little');item['id']=cid
                item['enabled_status']=hex(p.call('ClockDxe','ClockIsEnabled',cid,p.scratch+0x888))
                item['enabled']=p.r.memory(p.scratch+0x888,1).hex()
                item['frequency_status']=hex(p.call('ClockDxe','ClockGetFrequency',cid,p.scratch+0x890))
                item['frequency']=p.read64(p.scratch+0x890)
        d['pmic_names']=[]
        for name in ['vemc','ldo_vemc']:
            p.write(p.scratch+0x800,name.encode()+b'\0')
            item=dict(name=name);d['pmic_names'].append(item)
            item['enabled_status']=hex(p.call('MT6358PmicDxe','RegulatorIsEnabled',p.scratch+0x800,p.scratch+0x888))
            item['enabled']=p.r.memory(p.scratch+0x888,1).hex()
            item['voltage_status']=hex(p.call('MT6358PmicDxe','RegulatorGetVoltage',p.scratch+0x800,p.scratch+0x890))
            item['voltage']=int.from_bytes(p.r.memory(p.scratch+0x890,4),'little')
        seed=p.read64(ctx+8);d['seed_handle']=hex(seed)
        if seed:
            d['supported_status']=hex(p.call('MtkUfsHandoffDxe','MtkUfsDriverSupported',p.address('MtkUfsHandoffDxe','mDriverBinding'),seed,0))
        (p.out/'ufs-failure-diagnostics.json').write_text(json.dumps(report,indent=2)+'\n')
    assert filesystems and blocks,'UFS LU2 did not publish BlockIO and FAT'
    assert 'MtkUfsReadOnlyBlockIoDxe' in p.images,'Production UFS BlockIO driver did not load'
    wire=report['ufs_scsi_wire_probe']
    assert wire['validator_reached'] and wire['status']=='0x0',wire
    assert wire['data_segment_length']==2 and wire['sense_data_length']==0,wire
    assert wire['transaction_type']==0x21 and wire['lun']==2,wire
    first=filesystems[0];expected=(p.b/'AARCH64/Shell.efi').read_bytes()
    report['ufs_file_read']=p.storage_read(first,expected)
    # Capacity must be checked on the whole LU, not an optional partition.
    for handle in blocks:
        block=p.protocol(handle,block_guid);media=p.read64(block+8)
        raw=p.r.memory(media,48)
        if raw[6]:continue
        media_id=struct.unpack_from('<I',raw)[0]
        block_size=struct.unpack_from('<I',raw,12)[0]
        last_block=struct.unpack_from('<Q',raw,24)[0]
        assert raw[7] and block_size==BLOCK_SIZE and (last_block+1)*block_size==CAPACITY
        s=p.scratch
        rc=p.call_address(p.read64(block+24),'MtkUfsReadOnlyBlockIoDxe','ReadBlocks-last-sector',block,media_id,last_block,block_size,s+0x10000)
        assert rc==0 and p.r.memory(s+0x10000,block_size)==TAIL,('UFS last-sector read',hex(rc))
        rc=p.call_address(p.read64(block+24),'MtkUfsReadOnlyBlockIoDxe','ReadBlocks-past-end',block,media_id,last_block+1,block_size,s+0x10000)
        assert rc==0x8000000000000002,('UFS past-end read accepted',hex(rc))
        report['ufs_capacity']=dict(bytes=CAPACITY,block_size=block_size,last_block=last_block,last_sector_match=True,past_end_status=hex(rc))
        break
    else:raise AssertionError('No whole-device UFS BlockIO')
    p.boot_external(first,report)
    report['external_EFI_boot']['launch']='real CoreLoadImage from UFS device path + CoreStartImage; no source buffer'
