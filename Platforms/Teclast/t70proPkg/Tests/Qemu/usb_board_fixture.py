# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Original USB DT subset plus explicit retained-board register fixture."""
import hashlib,json,runpy,struct


def runtime_tree(base,k,p,cold_attach=False):
    dt=runpy.run_path(str(k/'Linux/platform-modules.py'))
    parse=dt['tree']
    nodes,_=parse(base)
    stock=(p/'analysis/stock-fdt/effective.dtb').read_bytes()
    assert hashlib.sha256(stock).hexdigest()=='a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391'
    src,ph=parse(stock)
    paths=['/soc','/soc/usb0@11200000','/soc/usb-phy@11f40000',
           '/soc/usb-phy@11f40000/usb-phy@11f40000','/soc/i2c@1101a000',
           '/soc/i2c@1101a000/sc8989x@6A',
           '/soc/i2c@1101a000/sc8989x@6A/usb-otg-vbus',
           '/soc/i2c@1101a000/husb311@4e',
           '/soc/extcon_usb','/soc/pinctrl','/syscon@1101B000',
           '/syscon@10001000','/syscon@10000000','/syscon@1000C000',
           '/clocks','/clocks/clk26m','/interrupt-controller',
           '/soc/usb0@11200000/port','/soc/usb0@11200000/port/endpoint@0',
           '/soc/extcon_usb/port','/soc/extcon_usb/port/endpoint@0',
           '/soc/pericfg@10003000','/soc/infracfg_ao@10001000',
           '/soc/efuse@11c10000','/soc/efuse@11c10000/u2_phy_data',
           '/soc/otg_iddig']
    for path in paths:
        chain=[];at=path
        while at not in nodes:
            chain.append(at);at=at.rpartition('/')[0] or '/'
        for item in reversed(chain):
            nodes[item]={'parent':item.rpartition('/')[0] or '/', 'properties':{}}
        nodes[path]['properties']=dict(src[path]['properties'])
    nodes['/']['properties']['compatible']=src['/']['properties']['compatible']
    nodes['/']['properties']['interrupt-parent']=src['/']['properties']['interrupt-parent']
    if cold_attach:
        # Entirely synthetic calibration, never copied from device identifiers.
        tag=bytearray(440)
        struct.pack_into('<I',tag,0,109)
        struct.pack_into('<I',tag,436,0xc3b158ae)
        nodes['/chosen']['properties']['atag,devinfo']=bytes(tag)
    strings=bytearray();names={}
    cells=lambda *v:struct.pack('>'+len(v)*'I',*v)
    pad=lambda b:b+bytes(-len(b)%4)
    def node(path):
        data=cells(1)+pad((path.rsplit('/',1)[-1] if path!='/' else '').encode()+b'\0')
        for name,value in nodes[path]['properties'].items():
            if name not in names:names[name]=len(strings);strings.extend(name.encode()+b'\0')
            data+=cells(3,len(value),names[name])+pad(value)
        return data+b''.join(node(c) for c in nodes if nodes[c]['parent']==path)+cells(2)
    structure=node('/')+cells(9)
    pos=struct.unpack_from('>I',base,16)[0];start=pos
    while struct.unpack_from('>QQ',base,pos)!=(0,0):pos+=16
    reserves=base[start:pos+16];off=40+len(reserves);end=off+len(structure)
    tree=cells(0xd00dfeed,end+len(strings),off,end,40,17,16,0,len(strings),len(structure))+reserves+structure+strings
    runtime,handles=parse(tree)
    validate_usb_topology(runtime,handles)
    _,errors=dt['graph'](runtime,handles)
    unresolved={path:items for path,items in errors.items() if items}
    if unresolved:raise ValueError('Unresolved runtime DT suppliers: '+json.dumps(unresolved))
    return tree,dict(stock_dtb_sha256=hashlib.sha256(stock).hexdigest(),source_nodes=paths,
        supplier_references_validated=True,
        calibration_fixture="synthetic 109-word atag" if cold_attach else "none",
        usb_role_graph_and_custom_references_validated=True,
        runtime_dtb_sha256=hashlib.sha256(tree).hexdigest(),
        boundary='Actual I2C/SC8989x/HUSB311 and USB producer code; deterministic retained-source or named cold-attach chip states plus clock/GPIO/PHY register fixtures. No analog, physical cold-power or timing emulation.')


def validate_usb_topology(nodes,handles):
    usb='/soc/usb0@11200000';ext='/soc/extcon_usb'
    phy='/soc/usb-phy@11f40000/usb-phy@11f40000'
    refs=[(usb,'pericfg','/soc/pericfg@10003000'),
          (usb,'infracg','/soc/infracfg_ao@10001000'),
          (phy,'nvmem-cells','/soc/efuse@11c10000/u2_phy_data'),
          (ext,'charger','/soc/i2c@1101a000/sc8989x@6A'),
          (ext,'vbus-supply','/soc/i2c@1101a000/sc8989x@6A/usb-otg-vbus'),
          (usb+'/port/endpoint@0','remote-endpoint',ext+'/port/endpoint@0'),
          (ext+'/port/endpoint@0','remote-endpoint',usb+'/port/endpoint@0')]
    for path,prop,target in refs:
        if path not in nodes or target not in nodes or not nodes[path]['enabled'] or not nodes[target]['enabled']:
            raise ValueError('Missing/disabled USB topology node: '+path+' -> '+target)
        raw=nodes[path]['properties'].get(prop,b'')
        if len(raw)!=4 or handles.get(int.from_bytes(raw,'big'))!=target:
            raise ValueError('Wrong USB topology reference: '+path+':'+prop)
    tcpc='/soc/i2c@1101a000/husb311@4e'
    if (tcpc not in nodes or not nodes[tcpc]['enabled'] or
        nodes[ext]['properties'].get('tcpc')!=b'type_c_port0\0' or
        nodes[tcpc]['properties'].get('tcpc,name')!=b'type_c_port0\0'):
        raise ValueError('Type-C role provider name is disconnected')


def exercise_board_route(p, report, expected):
    # PC mode keeps the root controller in host mode. GPIO10 may describe the
    # downstream pogo exposure, but it must not recreate the HCD or USB tree.
    ready_guid='6c62f8ab-4ac1-4aa7-9d43-48bc48120d7e'
    hc_guid='3e745226-9818-45b6-a2ac-d7cd0e8ba2bc'
    io_guid='2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75'
    fs_guid='964e5b22-6459-11d2-8e39-00a0c969723b'
    before={name:p.handles(guid) for name,guid in
        [('ready',ready_guid),('host',hc_guid),('interfaces',io_guid),('filesystems',fs_guid)]}
    assert len(before['ready'])==len(before['host'])==len(before['filesystems'])==1,before
    assert len(before['interfaces'])>=3,before
    ready=p.protocol(before['ready'][0],ready_guid)
    revision,length=struct.unpack('<HH',p.r.memory(ready+4,4))
    assert revision==4 and length==88,(revision,length)
    assert p.read64(ready+72) and p.read64(ready+80),'HCD release hook missing'
    address=0x10005200
    saved=p.r.memory(address,4);value=int.from_bytes(saved,'little')
    assert not value&(1<<10),'fixture must start on ID-low board route'
    result=report['board_route_reconnect']=dict(
        stimulus='GPIO10 physical input while firmware host role is locked',
        ready_revision=revision,ready_bytes=length,
        ready_before=[hex(h) for h in before['ready']],cold_power_emulated=False,
        firmware_default_role='host',os_role_change_allowed_after_exit_boot_services=True,
        changes={})
    for name,raw in (('id_high',value|(1<<10)),('id_low',value)):
        p.write(address,raw.to_bytes(4,'little'))
        p.r.resume_for(3);p.idle()
        current={key:p.handles(guid) for key,guid in
            [('ready',ready_guid),('host',hc_guid),('interfaces',io_guid),('filesystems',fs_guid)]}
        assert {key:set(handles) for key,handles in current.items()} == \
               {key:set(handles) for key,handles in before.items()},(name,before,current)
        assert p.r.memory(p.address('MusbHostDxe','mHostStarted'),1)==b'\1'
        pins=tuple((struct.unpack('<I',p.r.memory(0x10005200+(pin//32)*16,4))[0]
                    >>(pin%32))&1 for pin in (99,4,104))
        assert pins==(1,1,1),(name,pins)
        fs=current['filesystems'][0]
        latency=p.key('down')
        assert latency is not None,(name,'keyboard did not respond')
        result['changes'][name]=dict(handles={key:[hex(h) for h in handles]
            for key,handles in current.items()},hub_pins=pins,key_ms=latency,
            storage_read=p.storage_read(fs,expected))
    (p.out/'board-route-reconnect.json').write_text(json.dumps(result,indent=2)+'\n')
    return fs


def exercise_typec_id_change(p, report, expected):
    protocols = {
        'ready': '6c62f8ab-4ac1-4aa7-9d43-48bc48120d7e',
        'host': '3e745226-9818-45b6-a2ac-d7cd0e8ba2bc',
        'interfaces': '2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75',
        'filesystems': '964e5b22-6459-11d2-8e39-00a0c969723b',
    }
    def route_state():
        handles = {name: p.handles(guid) for name, guid in protocols.items()}
        assert len(handles['ready']) == len(handles['host']) == len(handles['filesystems']) == 1, handles
        assert len(handles['interfaces']) >= 3, handles
        ready = p.protocol(handles['ready'][0], protocols['ready'])
        path = struct.unpack('<I', p.r.memory(ready + 56, 4))[0]
        pins = tuple(
            (struct.unpack('<I', p.r.memory(0x10005200 + (pin // 32) * 16, 4))[0]
             >> (pin % 32)) & 1 for pin in (99, 4, 104)
        )
        return handles['filesystems'][0], path, pins

    address = 0x10005200
    value = int.from_bytes(p.r.memory(address, 4), 'little')
    assert value & (1 << 10), 'Type-C fixture must begin with ID high'
    initial_fs, path, initial_pins = route_state()
    assert path == 1 and initial_pins == (1, 1, 1), (path, initial_pins)
    initial_handles={name:set(p.handles(guid)) for name,guid in protocols.items()}
    # Firmware keeps host mode and the onboard hub active. A later OS may
    # explicitly request a role change after taking the handoff.
    power_counters = p.r.memory(0x1101af20, 16)
    changes = {}
    for name, attached, key in (
        ('id_low', True, 'down'),
        ('id_high', False, 'up'),
    ):
        value = int.from_bytes(p.r.memory(address, 4), 'little')
        id_value = value & ~(1 << 10) if attached else value | (1 << 10)
        p.write(address, id_value.to_bytes(4, 'little'))
        p.r.resume_for(3)
        p.idle()
        fs, path, pins = route_state()
        assert (path,pins)==(1,(1,1,1)),(name,path,pins)
        assert {n:set(p.handles(g)) for n,g in protocols.items()}==initial_handles,name
        assert p.r.memory(0x1101af20, 16) == power_counters, name
        latency = p.key(key)
        assert latency is not None, (name, 'keyboard did not respond')
        changes[name] = dict(path=path, hub_pins=pins, key_ms=latency,
                             storage_read=p.storage_read(fs, expected))
    report['typec_id_change'] = dict(firmware_default_role='host',changes=changes)
    return initial_fs


def exercise_exit_boot_services(p, report):
    # Use the live map and the EFI image handle obtained by booting from USB.
    # No injected USB readiness and no service calls after ExitBootServices.
    st = p.read64(p.address("DxeCore", "gDxeCoreST"))
    handle = int(report["external_EFI_boot"]["image_handle"], 16)
    record = 0x43f7f100
    assert p.read64(st+96) and p.r.memory(record,4) == bytes(4)
    s = p.scratch+0x700
    exit_attempts = []
    for retry in range(2):
        p.write(s, struct.pack("<QQQI", 0x40000, 0, 0, 0))
        rc = p.call("DxeCore", "CoreGetMemoryMap", s, p.scratch+0x20000, s+8, s+16, s+24)
        assert rc == 0, ("GetMemoryMap", hex(rc))
        assert 0 < p.read64(s) <= 0x40000 and p.read64(s+16) >= 40
        if report["usb_exit"] == "id-changed" and retry == 0:
            # Change GPIO10 after the final map read while Type-C still
            # supplies VBUS and the hub remains enabled.
            address = 0x10005200
            value = struct.unpack("<I", p.r.memory(address,4))[0]
            assert not value & (1 << 10)
            p.write(address, struct.pack("<I", value | (1 << 10)))
        rc = p.call("DxeCore", "CoreExitBootServices", handle, p.read64(s+8))
        exit_attempts.append(hex(rc))
        if rc == 0:
            break
        assert rc == 0x8000000000000002, ("ExitBootServices", hex(rc))
    words = struct.unpack("<16I", p.r.memory(record,64))
    result = report["usb_os_handoff"] = dict(
        exit_statuses=exit_attempts, boot_services=p.read64(st+96),
        record_words=[hex(word) for word in words])
    assert rc == 0 and result["boot_services"] == 0, result
    assert words[:12] == (
        1, 2, 0x7f, 0x11200000, 0x10000, 0x11f40000, 0x10000,
        160, 16, 1, 1, 0x30,
    ), ("missing or malformed USB OS handoff", result)
    assert words[12] & 0x0101 == 0x0101 and words[14] == 1, result
    assert words[15] in (1, 3), result
    if report["usb_exit"] == "id-changed":
        assert words[15] == 1, ("live Type-C source was not preserved", result)


def discover_usb(p,expect_unready=False):
    # Discovery only: the guest must install the ready and host protocols itself.
    guid='6c62f8ab-4ac1-4aa7-9d43-48bc48120d7e'
    handles=p.handles(guid)
    stats=struct.unpack('<4I',p.r.memory(0x1101af00,16))
    cold=struct.unpack('<29I',p.r.memory(0x1101af10,116))
    cold_names=('enabled','tcpc98_event_reads','tcpc98_ack_writes',
        'tcpc98_confirm_reads','sc03_boost_on_writes','sc03_boost_off_writes',
        'tcpc1a_open_writes','tcpc1a_source_writes',
        'tcpc_power_present_transitions','tcpc_vsafe0_clear_transitions',
        'tcpc_vsafe0_reassert_transitions','tcpc98_first_event_value',
        'tcpc98_last_ack_value','tcpc98_last_confirm_value','tcpc98_current',
        'tcpc1e_current','tcpc97_current','sc03_current','tcpc1a_current',
        'tcpc1d_current','tcpc1e_safe_reads','tcpc1e_present_reads',
        'tcpc97_safe_reads','tcpc97_clear_reads','sc03_boost_on_reads',
        'sc14_reads','sc14_last_read','sc0a_current','sc0a_at_boost')
    cold_evidence=dict(zip(cold_names,cold))
    result=dict(ready='production MusbHandoffDxe via actual I2cDxe and board providers',
        power_path='board-cold-attach' if cold_evidence['enabled'] else 'board-retained-source',handles=[hex(h) for h in handles],
        i2c_transactions=stats[0],chip_writes=stats[1],nacks=stats[2],bad_control_widths=stats[3],
        host_started=p.r.memory(p.address('MusbHostDxe','mHostStarted'),1).hex(),
        cold_attach_fixture=bool(cold_evidence['enabled']))
    if cold_evidence['enabled']:
        result['cold_attach_i2c_evidence']=cold_evidence
    p.write(p.scratch+0x700,bytes(32))
    status=p.call('MusbHandoffDxe','ReadHostDiagnostics',p.scratch+0x700)
    raw=p.r.memory(p.scratch+0x700,32)
    result['producer_diagnostics']=dict(call_status=hex(status),
        status=hex(struct.unpack_from('<Q',raw)[0]),bus_step=hex(struct.unpack_from('<H',raw,8)[0]),
        reason=raw[10],failed=raw[11],ready=raw[12],connected=raw[13],devctl=hex(raw[14]),id=raw[15],
        sense_stage=raw[16],sense_register=hex(raw[17]),sense_saved=hex(raw[18]),
        sense_expected=hex(raw[19]),sense_live=hex(raw[20]),sense_status=hex(struct.unpack_from('<Q',raw,24)[0]))
    result['controller_registers']={hex(off):p.r.memory(0x11200000+off,size).hex()
        for off,size in ((1,1),(2,2),(4,2),(6,2),(8,2),(0xa,1),(0xb,1),(0x60,1),(0x200,1))}
    (p.out/'usb-board-discovery.json').write_text(json.dumps(result,indent=2)+'\n')
    assert stats[0]>0 and stats[2:]==(0,0),result
    if cold_evidence['enabled']:
        assert not expect_unready,result
        assert stats[1]>0,result
        assert cold_evidence['tcpc98_event_reads']>0,result
        assert cold_evidence['tcpc98_ack_writes']>0,result
        assert cold_evidence['tcpc98_confirm_reads']>0,result
        assert cold_evidence['tcpc98_first_event_value']==0x02,result
        assert cold_evidence['tcpc98_last_ack_value']==0x02,result
        assert cold_evidence['tcpc98_last_confirm_value']==0x00,result
        assert cold_evidence['tcpc98_current']==0x00,result
        assert cold_evidence['sc03_boost_on_writes']>0,result
        assert cold_evidence['sc0a_at_boost']==0xcb,result
        assert cold_evidence['sc0a_current']==0xcb,result
        assert cold_evidence['tcpc_power_present_transitions']>0,result
        assert cold_evidence['tcpc_vsafe0_clear_transitions']>0,result
        assert cold_evidence['tcpc1e_safe_reads']>0,result
        assert cold_evidence['tcpc1e_present_reads']>0,result
        assert cold_evidence['tcpc97_safe_reads']>0,result
        assert cold_evidence['tcpc97_clear_reads']>0,result
        assert cold_evidence['sc03_boost_on_reads']>0,result
        assert cold_evidence['sc14_reads']>0,result
        assert cold_evidence['sc14_last_read']==0x64,result
        assert cold_evidence['tcpc1a_open_writes']>0,result
        assert cold_evidence['tcpc1a_source_writes']>0,result
        assert cold_evidence['tcpc1a_current']==0x05,result
        assert cold_evidence['tcpc1d_current']==0x02,result
        assert cold_evidence['tcpc1e_current']==0x0c,result
        assert cold_evidence['tcpc97_current']==0x00,result
        assert cold_evidence['sc03_current']&0x20,result
        diagnostics=result['producer_diagnostics']
        assert diagnostics['call_status']=='0x0' and diagnostics['status']=='0x0',result
        assert diagnostics['ready']==1 and not diagnostics['failed'],result
        assert len(handles)==1,result
    else:
        assert stats[1]==0,result
    if expect_unready:
        d=result['producer_diagnostics']
        assert not handles and d['status']=='0x8000000000000006' and d['reason']==5,result
        assert not d['ready'] and result['host_started']=='00',result
        assert result['controller_registers']['0xb']=='30',result
        assert not p.handles('2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75'),result
        result['expected_active_controller_rejection']=True
        return result
    assert len(handles)==1,('Production USB readiness not published',result)
    result['handle']=hex(handles[0])
    result['connect_status']=hex(p.call('DxeCore','CoreConnectController',handles[0],0,0,1))
    result['host_started']=p.r.memory(p.address('MusbHostDxe','mHostStarted'),1).hex()
    return result
