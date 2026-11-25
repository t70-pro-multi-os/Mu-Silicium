#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Find exact stock DT supplier modules; no module/device execution."""
import argparse,hashlib,importlib.util,json,re,struct
from pathlib import Path

# Property formats are explicit. Never treat an arbitrary integer as a phandle.
SPECIFIERS={'clocks':'#clock-cells','assigned-clocks':'#clock-cells',
 'assigned-clock-parents':'#clock-cells','resets':'#reset-cells',
 'power-domains':'#power-domain-cells','performance-domains':'#performance-domain-cells',
 'iommus':'#iommu-cells',
 'phys':'#phy-cells','dmas':'#dma-cells','mboxes':'#mbox-cells',
 'io-channels':'#io-channel-cells','pwms':'#pwm-cells',
 'interconnects':'#interconnect-cells','sound-dai':'#sound-dai-cells',
 'interrupts-extended':'#interrupt-cells',
 'thermal-sensors':'#thermal-sensor-cells','cooling-device':'#cooling-cells',
 'nvmem-cells':'#nvmem-cell-cells'}
# gpio-ranges uses three fixed arguments, not a provider-declared cell count.
FIXED_SPECIFIERS={'gpio-ranges':3}
SINGLES={'interrupt-parent','next-level-cache','remote-endpoint','cpu','trip',
 'mediatek,mmsys','mediatek,mdpsys','mediatek,mutex','mediatek,scpsys',
 'mediatek,infracfg','mediatek,apmixedsys','mediatek,pericfg','mediatek,topckgen'}
LISTS={'mediatek,larb','cpu-idle-states','cpus','required-opps',
 'memory-region','nvmem','shmem','operating-points-v2'}
# These shipped vendor spellings are scoped to the actual consumer binding.
BOARD_SINGLES={
 'charger':{'mediatek,mt6358-gauge','mediatek,battery manager',
            'mediatek,extcon-usb','mediatek,charger'},
 'pericfg':{'mediatek,mt6789-usb20'},'infracg':{'mediatek,mt6789-usb20'}}
BOARD_GPIOS={
 'husb311pd,intr_gpio':{'hynetek,husb311'},
 'sc,intr-gpio':{'southchip,sc89890h'},
 'sc,sc8989x,en-gpio':{'southchip,sc89890h'},
 'sc,sc8989x,otg-gpio':{'southchip,sc89890h'},
 'focaltech,irq-gpio':{'focaltech,fts-spi'},
 'focaltech,reset-gpio':{'focaltech,fts-spi'},
 'headset-mic-sw-gpio':{'mediatek,mt6358-accdet'},
 'headset-lr-en-gpio':{'mediatek,mt6358-accdet'}}
# Exact shipped SMI node vectors: require both consumer and target types.
# This records DT reference facts; runtime consumption is not established.
BOARD_LIST_TARGETS={'mediatek-larb-supply':'mediatek,mt6789-smi-larb',
                    'mediatek-common-supply':'mediatek,mt6789-smi-common'}
# These parent clocks are referred to by stock clock names, not all by phandle.
# Keep this small board policy separate from inferred supplier edges.
FOUNDATION={'mediatek,mt6789-topckgen','mediatek,mt6789-apmixedsys',
 'mediatek,mt6789-infracfg_ao','mediatek,mt6789-pinctrl'}

# Board feature policy: explicit shipped module names, never a runtime autoload
# wildcard. Components and their machine/PD/thermal frontends are kept together.
BOARD_ROOTS={
 'input':{'mtk_kpd','mtk_pmic_keys','regulator_vibrator'},
 'display':{'leds_mtk','leds_mtk_disp','leds_mtk_pwm','pwm_mtk_disp','backlight_cooling'},
 'audio':{'mt6789_mt6366','mtk_btcvsd'},
 'camera':{'flashlight','flashlights_cp2155','mtk_composite'},
 'power':{'pmic_temp','soc_temp_lvts','board_temp','md_cooling_all','backlight_cooling',
          'mtk_cpu_power_throttling','mtk_gpu_power_throttling','mtk_md_power_throttling',
          'mtk_dynamic_loading_throttling','mtk_bp_thl','rtc_mt6397',
          'rt_pd_manager','mtk_pd_adapter','mtk_chg_type_det'},
 'usb':{'rt_pd_manager','mtk_pd_adapter','mtk_chg_type_det'},
 # The stock BT character driver does not register an HCI device.
 # The shipped FM module is loaded by Android without an OF match.
 'connectivity':{'hci_uart','bt_drv_connac1x','fmradio_drv_mt6631_6635'},
}
BOARD_MFD_SUPPLIERS={'mediatek,mt6358-regulator':('mt6358_regulator','platform:mt6358-regulator','mediatek,mt6366')}
# Native gpio-keys owns GPIO0; never insert the direct-init legacy hall module.
BOARD_EXCLUSIONS={'input':{'hall_sensor_p'},'sensors':{'imgsensor_isp6s','hall_sensor_p'}}

def cells(data):
    if len(data)%4:raise ValueError('non-cell DT property length')
    return list(struct.unpack('>'+str(len(data)//4)+'I',data))

def strings(data):
    if not data or not data.endswith(b'\0'):raise ValueError('unterminated DT string')
    return data[:-1].decode('ascii').split('\0')

def tree(data):
    if len(data)<40:raise ValueError('short DT header')
    magic,total,so,st,ro,version,compat,_,ns,ss=struct.unpack_from('>10I',data)
    if magic!=0xd00dfeed or total!=len(data) or version<17 or compat>17 or total>4*1024*1024:
        raise ValueError('unsupported DT header')
    if so<40 or so%4 or st<40 or ro<40 or ro%8 or not ss or so+ss>total or st+ns>total or so<st+ns and st<so+ss:
        raise ValueError('invalid DT ranges')
    names=data[st:st+ns];pos=so;end=so+ss;stack=[];nodes={};ended=False
    while pos<end:
        if pos+4>end:raise ValueError('short DT token')
        token=struct.unpack_from('>I',data,pos)[0];pos+=4
        if token==1:
            stop=data.find(b'\0',pos,end)
            if stop<0:raise ValueError('unterminated DT node')
            name=data[pos:stop].decode('ascii');pos=(stop+4)&~3
            if '/' in name or (not stack and (name or nodes)):raise ValueError('invalid DT root/name')
            path=(stack[-1].rstrip('/')+'/'+name) if stack else '/'
            if path in nodes or len(nodes)>=16384 or len(stack)>=128:raise ValueError('duplicate/oversized DT tree')
            nodes[path]={'parent':stack[-1] if stack else None,'properties':{}}
            stack.append(path)
        elif token==2:
            if not stack:raise ValueError('unbalanced DT node')
            stack.pop()
        elif token==3:
            if not stack or pos+8>end:raise ValueError('misplaced/short DT property')
            size,no=struct.unpack_from('>II',data,pos);pos+=8
            if pos+size>end or no>=len(names):raise ValueError('invalid DT property range')
            stop=names.find(b'\0',no)
            if stop<0:raise ValueError('unterminated DT property name')
            name=names[no:stop].decode('ascii');props=nodes[stack[-1]]['properties']
            if not name or name in props:raise ValueError('duplicate/empty DT property')
            props[name]=data[pos:pos+size];pos=(pos+size+3)&~3
        elif token==4:continue
        elif token==9:
            if stack or '/' not in nodes or any(data[pos:end]):raise ValueError('unbalanced DT end')
            ended=True;break
        else:raise ValueError('unknown DT token')
    if not ended:raise ValueError('missing DT end')
    phandles={}
    for path,node in nodes.items():
        props=node['properties'];status=strings(props['status']) if 'status' in props else ['okay']
        if len(status)!=1:raise ValueError('invalid DT status')
        node['enabled']=status[0] in ('ok','okay') and (node['parent'] is None or nodes[node['parent']]['enabled'])
        node['compatible']=strings(props['compatible']) if 'compatible' in props else []
        hs=[cells(props[k]) for k in ('phandle','linux,phandle') if k in props]
        if hs:
            if any(len(h)!=1 or h[0] in (0,0xffffffff) for h in hs) or any(h!=hs[0] for h in hs) or hs[0][0] in phandles:
                raise ValueError('invalid/duplicate DT phandle')
            phandles[hs[0][0]]=path
    return nodes,phandles

def of_matches(raw,issues=None):
    """Follow ARM64 ABS64 device_driver.of_match_table pointers, not table names.

    Linux 6.12 device_driver places of_match_table 40 bytes after name.
    The exact stock has RANDSTRUCT_NONE. Supported enclosing prefixes are
    device_driver (0), spi_driver (32), platform_driver (40), and i2c_driver (48).
    Each result requires both relocations, a bounded driver object and table.
    """
    if len(raw)<64 or raw[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<HH',raw,16)!=(1,183):
        raise ValueError('not ARM64 ELF module')
    start=struct.unpack_from('<Q',raw,40)[0];width,count,_=struct.unpack_from('<HHH',raw,58)
    if width!=64 or not count or start+count*64>len(raw):raise ValueError('invalid ELF sections')
    sections=[struct.unpack_from('<IIQQQQIIQQ',raw,start+i*64) for i in range(count)]
    def contents(index):
        if not 0<index<count:raise ValueError('invalid ELF section index')
        s=sections[index]
        if s[1]==8 or s[4]+s[5]>len(raw):raise ValueError('invalid ELF section contents')
        return raw[s[4]:s[4]+s[5]]
    catalogs={};objects={}
    for index,sec in enumerate(sections):
        if sec[1]!=2:continue
        if sec[9]!=24 or sec[5]%24 or not 0<sec[6]<count:raise ValueError('invalid ELF symbols')
        names=contents(sec[6]);symbols=contents(index);catalog=[]
        for pos in range(0,len(symbols),24):
            no,info,_,si,value,length=struct.unpack_from('<IBBHQQ',symbols,pos)
            if no>=len(names) or names.find(b'\0',no)<0:raise ValueError('invalid ELF symbol name')
            name=names[no:names.index(0,no)].decode('ascii')
            item={'name':name,'type':info&15,'section':si,'value':value,'bytes':length}
            catalog.append(item)
            if info&15==1 and 0<si<count and length:
                if value+length>sections[si][5]:raise ValueError('ELF object outside section')
                objects.setdefault((si,value),[]).append(item)
        catalogs[index]=catalog
    # RELA owns the addend. Keep source/target section identities; linked
    # virtual addresses are not meaningful for these relocatable modules.
    pointers={}
    for index,sec in enumerate(sections):
        if sec[1]!=4:continue
        if sec[9]!=24 or sec[5]%24 or sec[6] not in catalogs or not 0<sec[7]<count:
            raise ValueError('invalid ELF RELA section')
        catalog=catalogs[sec[6]]
        for pos in range(0,sec[5],24):
            offset,info,addend=struct.unpack_from('<QQq',contents(index),pos)
            symbol=info>>32;kind=info&0xffffffff
            if symbol>=len(catalog):raise ValueError('ELF relocation symbol out of range')
            if kind!=257:continue # R_AARCH64_ABS64 only.
            if offset%8 or offset+8>sections[sec[7]][5]:raise ValueError('invalid ELF pointer relocation')
            key=(sec[7],offset)
            if key in pointers:raise ValueError('duplicate ELF pointer relocation')
            target=catalog[symbol];value=target['value']+addend
            if 0<target['section']<count:
                if not 0<=value<=sections[target['section']][5]:raise ValueError('ELF pointer target outside section: '+target['name']+' '+str(value))
                pointers[key]={'target':(target['section'],value),'file_offset':sec[4]+pos}
    def text_at(location):
        section,offset=location
        if sections[section][1]==8 or sections[section][2]&4:return None
        data=contents(section);end=data.find(b'\0',offset,offset+129)
        if end<=offset:return None
        value=data[offset:end]
        return value.decode('ascii') if re.fullmatch(rb'[A-Za-z0-9,._+/@ -]+',value) else None
    imported={x['name'] for values in catalogs.values() for x in values if x['section']==0}
    registrations={0:{'driver_register'},32:{'__spi_register_driver'},40:{'__platform_driver_register','__platform_register_drivers','__platform_driver_probe'},48:{'i2c_register_driver'}}
    sizes={0:176,32:208,40:240,48:272} # Exact stock Android 6.12 KABI layouts.
    def table_issue(driver,message):
        if issues is None:raise ValueError(message)
        issues.append({'driver':driver['name'],'status':message})
    result={}
    for (si,value),aliases in objects.items():
        driver=next((x for x in aliases if x['bytes'] in sizes.values()),None)
        if driver is None:continue
        for prefix in (0,32,40,48):
            if driver['bytes']!=sizes[prefix] or not imported.intersection(registrations[prefix]):continue
            name_slot=value+prefix;match_slot=name_slot+40
            if match_slot+8>value+driver['bytes']:continue
            name_pointer=pointers.get((si,name_slot));match_pointer=pointers.get((si,match_slot))
            if not name_pointer or not match_pointer:continue
            driver_name=text_at(name_pointer['target'])
            if driver_name is None:continue
            # bool/padding and enum probe_type corroborate the supported prefix.
            flags=contents(si)[name_slot+32:name_slot+40]
            if flags[0]>1 or any(flags[1:4]) or struct.unpack_from('<I',flags,4)[0]>2:continue
            table_location=match_pointer['target'];candidates=objects.get(table_location,[])
            tables=[x for x in candidates if x['bytes']>=200 and x['bytes']%200==0]
            if not tables:continue # An unsupported object is not an OF-table fact.
            lengths={x['bytes'] for x in tables}
            if len(lengths)!=1:raise ValueError('ambiguous OF table bounds')
            table=min(tables,key=lambda x:(x['name'].startswith('__mod_'),x['name']))
            ti,tv=table_location;data=contents(ti)[tv:tv+table['bytes']]
            if data[-200:]!=bytes(200):
                table_issue(driver,'unterminated driver OF table: '+table['name']);continue
            for offset in range(0,len(data)-200,200):
                entry=data[offset:offset+200];fields=(entry[:32],entry[32:64],entry[64:192])
                if any(b'\0' not in field for field in fields):raise ValueError('unterminated OF field')
                name,kind,compatible=[field.split(b'\0',1)[0].decode('ascii') for field in fields]
                # Name/type restrictions require the full Linux OF matcher.
                if name or kind or not compatible:continue
                if any(ord(c)<32 or ord(c)>126 for c in compatible):
                    table_issue(driver,'non-printable OF compatible');continue
                row={'compatible':compatible,'symbol':table['name'],
                     'module_file_offset':sections[ti][4]+tv+offset,
                     'driver_symbol':driver['name'],'driver_name':driver_name,
                     'driver_file_offset':sections[si][4]+value,'driver_name_prefix':prefix,
                     'of_pointer_relocation_file_offset':match_pointer['file_offset']}
                result.setdefault((compatible,driver['name']),row)
    return list(result.values())

def graph(nodes,handles):
    edges={p:[] for p in nodes};errors={p:[] for p in nodes}
    for path,node in nodes.items():
        if not node['enabled'] or path in ('/__symbols__','/__fixups__'):continue
        props=node['properties']
        for key,raw in props.items():
            countprop=SPECIFIERS.get(key)
            fixed_count=FIXED_SPECIFIERS.get(key)
            if BOARD_GPIOS.get(key,set()).intersection(node['compatible']):countprop='#gpio-cells'
            if key=='gpios' or key.endswith('-gpios') or key in ('himax,irq-gpio','himax,rst-gpio'):countprop='#gpio-cells'
            # The stock camera rails use the fixed-regulator legacy singular
            # spelling. Its GPIO expander is a supplier, not a raw pin number.
            # Other bindings (and __symbols__) may use 'gpio' as opaque text.
            if key=='gpio' and 'regulator-fixed' in node['compatible']:countprop='#gpio-cells'
            board_list=key in BOARD_LIST_TARGETS and 'mediatek,mtk-smi-dbg' in node['compatible']
            listed=bool(re.fullmatch(r'pinctrl-[0-9]+',key)) or key in LISTS or board_list
            single=not board_list and (key in SINGLES or (key.endswith('-supply') and ',' not in key) or
                bool(BOARD_SINGLES.get(key,set()).intersection(node['compatible'])))
            if not (countprop or fixed_count is not None or listed or single):continue
            try:
                values=cells(raw);pos=0
                if single and len(values)!=1:raise ValueError('expected one phandle')
                while pos<len(values):
                    handle=values[pos];pos+=1
                    if handle==0:continue # Optional empty specifier, not an address.
                    if handle not in handles:raise ValueError('unknown supplier phandle')
                    target=handles[handle]
                    if board_list and BOARD_LIST_TARGETS[key] not in nodes[target]['compatible']:
                        raise ValueError('unexpected board reference target type')
                    if countprop or fixed_count is not None:
                        provider=nodes[target]['properties']
                        # Shipped MTK binding explicitly declares its own count.
                        actual='#mtk-interconnect-cells' if key=='interconnects' and countprop not in provider and '#mtk-interconnect-cells' in provider else countprop
                        # NVMEM cells without arguments predate #nvmem-cell-cells.
                        default=struct.pack('>I',0) if key=='nvmem-cells' else b''
                        count=[fixed_count] if fixed_count is not None else cells(provider.get(actual,default))
                        if len(count)!=1 or count[0]>64:raise ValueError('missing/invalid '+countprop)
                        if pos+count[0]>len(values):raise ValueError('truncated supplier specifier')
                        pos+=count[0]
                    # Pin configurations, regulator containers and graph endpoints
                    # belong to the nearest compatible ancestor device.
                    device=target
                    while not nodes[device]['compatible'] and nodes[device]['parent'] is not None:device=nodes[device]['parent']
                    if not nodes[target]['enabled'] or not nodes[device]['enabled']:raise ValueError('disabled supplier')
                    edges[path].append({'property':key,'target':target,'device':device})
            except ValueError as e:errors[path].append({'property':key,'error':str(e)})
        parent=node['parent']
        while parent and not nodes[parent]['compatible']:parent=nodes[parent]['parent']
        if parent and parent!='/':edges[path].append({'property':'parent-bus','target':parent,'device':parent})
    return edges,errors

def feature_plans(nodes,handles,modules,features):
    edges,parse_errors=graph(nodes,handles);matches={p:[] for p in nodes}
    for name,item in modules.items():
        compatible={row['compatible'] for row in item['matches']}
        for path,node in nodes.items():
            if node['enabled'] and compatible.intersection(node['compatible']):matches[path].append(name)
    result={}
    for feature,roots in features.items():
        seeds=sorted(p for p,drivers in matches.items() if set(drivers)&set(roots))
        seen=set();added=set();order=[];unknown=[];issues=[];connections=[];cycles=[];visiting=set();containers=[]
        def visit(path):
            if path in seen:return
            if path in visiting:cycles.append(path);return # Device cycles are handled by fw_devlink/deferred probe.
            visiting.add(path)
            issues.extend({'node':path,**e} for e in parse_errors[path])
            for edge in edges[path]:
                connections.append({'consumer':path,**edge});visit(edge['device'])
            visiting.remove(path);seen.add(path)
            candidates=sorted(set(matches[path]));preloads=[]
            parent=nodes[path]['parent']
            for compatible in nodes[path]['compatible']:
                hint=BOARD_MFD_SUPPLIERS.get(compatible)
                if not hint or not parent:continue
                name,alias,parent_type=hint
                if parent_type in nodes[parent]['compatible'] and alias in modules.get(name,{}).get('platform_aliases',[]):
                    preloads.append(name)
            # Multiple exact matches can be distinct component providers for the
            # same node. Retain them, report the overlap, never pick a random one.
            if not candidates and nodes[path]['compatible']==['simple-bus']:
                containers.append(path) # OF population container; it has no hardware driver to load.
            elif not candidates and nodes[path]['compatible']:
                unknown.append({'node':path,'compatible':nodes[path]['compatible'],'status':'no descriptor-linked OF driver; built-in or named-bus binding requires runtime evidence','named_bus_preload_modules':preloads})
            for name in candidates+preloads:
                if name not in added:added.add(name);order.append(name)
        foundation=sorted(p for p,n in nodes.items() if n['enabled'] and FOUNDATION.intersection(n['compatible'])) if seeds else []
        for seed in foundation+seeds:visit(seed)
        result[feature]={'seed_nodes':seeds,'foundation_nodes':foundation,'inspected_device_nodes':sorted(p for p in seen if matches[p]),'roots':order,'dt_edges':connections,'parse_errors':issues,
          'unresolved_nodes':unknown,'of_population_containers':containers,'device_cycles':sorted(set(cycles)),
          'multiple_match_nodes':[{'node':p,'modules':sorted(set(matches[p]))} for p in sorted(seen) if len(set(matches[p]))>1],
          'runtime_validated':False}
    return result

def symbol_dependency_graph(binaries,catalog,kernel):
    """Close only unique, CRC-matched strong imports; ambiguity stays explicit."""
    exports={}
    for name,variants in catalog.items():
        for info in variants.values():
            for symbol,entry in info.get('exported_symbol_info',{}).items():
                exports.setdefault((symbol,entry['crc']),set()).add(name)
    graph={};evidence=[];issues=[]
    for selection,binary in sorted(binaries.items()):
        name=binary['name'];deps=graph.setdefault(name,set())
        for symbol,crc in sorted(binary['strong_imports'].items()):
            if crc is None:
                issues.append({'binary':selection,'symbol':symbol,'error':'strong import has no version CRC'});continue
            if symbol in kernel and kernel[symbol]['crc']==crc:continue
            providers=exports.get((symbol,crc),set())-{name}
            if len(providers)==1:
                provider=next(iter(providers));deps.add(provider)
                evidence.append({'binary':selection,'symbol':symbol,'crc':crc,'provider':provider})
            else:
                issues.append({'binary':selection,'symbol':symbol,'crc':crc,'providers':sorted(providers),'error':'strong import provider absent or ambiguous'})
    return {name:sorted(v) for name,v in graph.items()},evidence,issues

def complete_feature_plans(nodes,handles,modules,features,dependencies):
    # A supplier can itself have an ELF-declared module whose DT suppliers are
    # absent from the first traversal. Close both graphs until no roots change.
    roots={k:set(v) for k,v in features.items()}
    for iteration in range(len(modules)+1):
        plans=feature_plans(nodes,handles,modules,roots)
        expanded={}
        for feature,plan in plans.items():
            todo=list(roots[feature]|set(plan['roots']));done=set()
            while todo:
                name=todo.pop()
                if name in done:continue
                done.add(name);todo.extend(dependencies.get(name,[]))
            expanded[feature]=done
        if expanded==roots:
            for feature,plan in plans.items():
                plan['roots']=list(dict.fromkeys(plan['roots']+sorted(roots[feature])))
                plan['requested_module_roots']=sorted(features[feature])
                plan['dependency_closure_iterations']=iteration+1
            return plans
        roots=expanded
    raise ValueError('DT/module supplier closure did not converge')

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract',action='store_true',help='extract only missing original supplier modules, preserving the existing manifest')
    args=parser.parse_args()
    pkg=Path(__file__).resolve().parent.parent;repo=pkg.parents[2];project=repo.parent
    analysis=project/'analysis';backup=Path('/mnt/mac/Users/you/.tmp/teclast-t70pro/device-backup')
    sm=load('platform_stock_modules',pkg/'inspect-stock-modules.py')
    catalog=json.loads((analysis/'stock-modules.json').read_text());hardware=json.loads((analysis/'runtime-hardware.json').read_text())
    manifest=json.loads((analysis/'stock-binaries/manifest.json').read_text());audit=json.loads((analysis/'linux-stock-module-audit.json').read_text())
    if 'CONFIG_RANDSTRUCT_NONE=y' not in (analysis/'stock-kernel.config').read_text().splitlines():
        raise ValueError('device_driver layout needs exact stock RANDSTRUCT_NONE evidence')
    modules={};binaries={};binary_imports={}
    early=load('platform_early_userspace',pkg/'Linux/build-early-userspace.py')
    def add(source,name,raw,expected,aliases=()):
        digest=hashlib.sha256(raw).hexdigest()
        if digest!=expected:raise ValueError('original module hash differs from catalog: '+name)
        issues=[]
        try:matches=of_matches(raw,issues)
        except ValueError as e:raise ValueError(source+':'+name+': '+str(e)) from e
        info={'source':source,'sha256':digest,'matches':matches,'table_issues':issues,'platform_aliases':sorted(a for a in aliases if a.startswith('platform:'))}
        binaries[source+':'+name]=info
        parsed=sm._elf_parser.elf_module(raw)
        binary_imports[source+':'+name]={'name':name,'strong_imports':{symbol:parsed['modversions'].get(symbol) for symbol in early.required_symbols(raw)}}
        if name not in modules:modules[name]=info
        elif modules[name]['matches']!=matches:
            # Different source offsets can coexist; compatible facts must agree.
            if {m['compatible'] for m in modules[name]['matches']}!={m['compatible'] for m in matches}:
                raise ValueError('OF match variant ambiguity: '+name)
    class Capture:
        def save(self,source,name,filename,raw,info):
            original=(catalog['vendor_boot_a']['fragments'][int(source.split(':')[1])]['modules'][name]
                      if source.startswith('vendor_boot_a:') else catalog['partitions'][source]['modules'][name])
            add(source,name,raw,original['sha256'],original['modinfo'].get('alias',[]))
    layout=json.loads((backup/'partition-layout.json').read_text())
    if not layout['primary_header_crc_valid'] or not layout['partition_entries_crc_valid']:raise ValueError('original GPT evidence missing')
    super_part=next(p for p in layout['partitions'] if p['name']=='super')
    inspected={}
    with (backup/'images/ufs-lu2.img').open('rb') as stream:
        sm.inspect_vendor_boot(stream,next(p for p in layout['partitions'] if p['name']=='vendor_boot_a'),hardware,Capture())
        logical,_=sm.logical_partitions(stream,super_part)
        for source in ('vendor_dlkm_a','system_dlkm_a','odm_dlkm_a'):
            if source not in logical or not logical[source]['bytes']:continue
            inspected[source]=sm.inspect_dlkm(stream,super_part,logical[source],source,hardware,Capture())['module_count']
    expected={source+':'+name for source,part in catalog['partitions'].items() for name in part.get('modules',{})}
    expected|={'vendor_boot_a:'+str(i)+':'+name for i,part in enumerate(catalog['vendor_boot_a']['fragments']) for name in part.get('modules',{})}
    if set(binaries)!=expected:raise ValueError('incomplete original module catalog coverage')
    for item in manifest['modules']:
        if item['selection'] in binaries:continue
        raw=Path(item['path']).read_bytes();add(item['source'],item['name'],raw,item['sha256'])
    dtb=analysis/'stock-fdt/effective.dtb';nodes,handles=tree(dtb.read_bytes())
    features={k:(set(v['roots'])|BOARD_ROOTS.get(k,set()))-BOARD_EXCLUSIONS.get(k,set()) for k,v in audit['all_hardware_feature_plans'].items()}
    full=sm._elf_parser.module_catalog(catalog)
    if any(name not in full for roots in BOARD_ROOTS.values() for name in roots):
        raise ValueError('board feature module absent from original catalog')
    _,kernel=sm._elf_parser.read_stock_kernel_exports(analysis/'stock-kernel-exports.json',analysis/'kernel-info.json')
    symbol_graph,symbol_edges,symbol_issues=symbol_dependency_graph(binary_imports,full,kernel)
    dependencies={name:sorted(set(catalog['dependency_analysis']['graph'].get(name,[]))|set(symbol_graph.get(name,[]))) for name in full}
    plans=complete_feature_plans(nodes,handles,modules,features,dependencies)
    if 'CONFIG_KEYBOARD_GPIO=y' not in (analysis/'stock-kernel.config').read_text().splitlines():
        raise ValueError('Native magnetic lid requires exact stock built-in gpio-keys')
    if any('hall_sensor_p' in plan['roots'] for plan in plans.values()):
        raise ValueError('Legacy hall module conflicts with native Linux SW_LID')
    for feature in ('input','sensors'):
        plans[feature]['native_lid_node']='/teclast-cover-lid'
    kernel_report=json.loads((analysis/'stock-kernel-exports.json').read_text())
    if kernel_report.get('parser_sha256')!=hashlib.sha256((pkg/'inspect-stock-kernel.py').read_bytes()).hexdigest():
        raise ValueError('Stale built-in kernel provider evidence')
    builtin=kernel_report['built_in_provider_evidence']
    for plan in plans.values():
        for item in plan['unresolved_nodes']:
            evidence=[p for p in builtin['providers'] if set(p['compatible']).intersection(item['compatible'])]
            if evidence:item['static_kernel_provider_evidence']=evidence

    # Retain declared and unique CRC-matched strong-import suppliers. The early
    # planner still validates the exact selected variants and topological order.
    selected={n for p in plans.values() for n in p['roots']};pending=list(selected)
    full=sm._elf_parser.module_catalog(catalog)
    missing=[]
    while pending:
        name=pending.pop()
        if name not in full:missing.append(name);continue
        for dep in dependencies.get(name,[]):
            if dep and dep not in selected and dep in full:selected.add(dep);pending.append(dep)
    extractions=[]
    for name in sorted(selected):
        if name not in full:continue
        # Keep all distinct source variants; exact Image selection remains later.
        hashes=set()
        for source,info in sorted(full[name].items()):
            if info['sha256'] in hashes:continue
            hashes.add(info['sha256']);selection=source+':'+name
            if not any(e['name']==name and e['sha256']==info['sha256'] for e in manifest['modules']):extractions.append(selection)
    if args.extract and extractions:
        extraction=sm.ModuleExtraction(analysis/'stock-binaries',extractions)
        with (backup/'images/ufs-lu2.img').open('rb') as stream:
            if any(x.startswith('vendor_boot_a:') for x in extractions):
                sm.inspect_vendor_boot(stream,next(p for p in layout['partitions'] if p['name']=='vendor_boot_a'),hardware,extraction)
            for source in ('vendor_dlkm_a','system_dlkm_a','odm_dlkm_a'):
                if any(x.startswith(source+':') for x in extractions):
                    sm.inspect_dlkm(stream,super_part,logical[source],source,hardware,extraction)
        extraction.finish()
    by_compatible={}
    for name,item in modules.items():
        for row in item['matches']:by_compatible.setdefault(row['compatible'],set()).add(name)
    coverage=[]
    for path,node in nodes.items():
        if not node['enabled'] or not node['compatible']:continue
        candidates=sorted({name for c in node['compatible'] for name in by_compatible.get(c,[])})
        coverage.append({'node':path,'compatible':node['compatible'],'modules':candidates,
          'status':'descriptor-linked-module' if candidates else 'OF-population-container' if node['compatible']==['simple-bus'] else 'requires-built-in-or-other-bus-evidence'})
    supplier_edges,supplier_errors=graph(nodes,handles)
    catalog_coverage={'expected_binary_count':len(expected),'inspected_binary_count':len(binaries),'missing':sorted(expected-set(binaries)),
      'logical_partition_module_counts':inspected,'unmatched_enabled_nodes':[x for x in coverage if x['status']=='requires-built-in-or-other-bus-evidence'],
      'note':'Complete binary enumeration is separate from complete device support; other buses, built-ins and malformed/unsupported descriptors remain explicit.'}
    report={'status':'offline DT supplier plan prepared','scope':'Exact stock device_driver OF relocation facts plus typed DT phandles; driver binding and provider availability remain unproven',
      'dtb':str(dtb),'dtb_sha256':hashlib.sha256(dtb.read_bytes()).hexdigest(),
      'nodes':len(nodes),'enabled_nodes':sum(n['enabled'] for n in nodes.values()),
      'binary_tables':binaries,'catalog_coverage':catalog_coverage,'built_in_provider_evidence':builtin,'enabled_device_coverage':coverage,
      'supplier_graph':{'scope':'Typed static references in every enabled node; an issue does not alone establish runtime use or failure',
        'enabled_node_count':sum(n['enabled'] for n in nodes.values()),'edge_count':sum(map(len,supplier_edges.values())),
        'issues':{path:values for path,values in supplier_errors.items() if values},'runtime_validated':False},
      'table_issues':{name:x['table_issues'] for name,x in binaries.items() if x['table_issues']},
      'strong_import_suppliers':{'edges':symbol_edges,'issues':symbol_issues,'scope':'All original module strong imports; only unique matching module CRC providers extend roots. Exact selected variants are validated again before loading.'},'features':plans,'payload_extra_roots':sorted(selected),'missing_declared_modules':missing,
      'stock_catalog_sha256':hashlib.sha256((analysis/'stock-modules.json').read_bytes()).hexdigest(),
      'manifest_sha256':hashlib.sha256((analysis/'stock-binaries/manifest.json').read_bytes()).hexdigest(),
      'extraction_required':[] if args.extract else extractions,'extracted_this_run':extractions if args.extract else [],'source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
      'support_source_sha256':{str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [pkg/'inspect-stock-modules.py',pkg/'Linux/prepare-stock-modules.py',pkg/'Linux/build-early-userspace.py',pkg/'inspect-stock-kernel.py']},
      'driver_layout':{'kernel_config_sha256':hashlib.sha256((analysis/'stock-kernel.config').read_bytes()).hexdigest(),'randstruct':'NONE','name_to_of_match_table_bytes':40,'supported_enclosing_prefixes':[0,32,40,48],'exact_driver_object_bytes':{0:176,32:208,40:240,48:272},'reference':'https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/device/driver.h','spi_reference':'https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/spi/spi.h','runtime_registration_proven':False},
      'named_bus_supplier_policy':BOARD_MFD_SUPPLIERS,'feature_root_policy':{k:sorted(v) for k,v in BOARD_ROOTS.items()},'feature_root_exclusions':{k:sorted(v) for k,v in BOARD_EXCLUSIONS.items()},
      'device_access':False,'module_loading':False,'original_open_mode':'rb',
      'limitations':['Only bounded device_driver, SPI, platform_driver and I2C descriptor prefixes are supported; other layouts and non-OF runtime-created devices require further evidence','No claim that an unresolved node is built-in','Multiple OF matches are retained as separate potential components, not assumed simultaneously bound','Device/provider cycles are recorded; ELF import cycles still block loading','Vendor-specific reference properties outside the explicit formats are not inferred']}
    (analysis/'linux-platform-modules.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'nodes':len(nodes),'modules_with_tables':sum(bool(x['matches']) for x in binaries.values()),'extraction_required':report['extraction_required'],'extracted_this_run':report['extracted_this_run'],
     'features':{k:{'seeds':len(v['seed_nodes']),'platform_roots':len(v['roots']),'errors':len(v['parse_errors']),'unresolved_nodes':len(v['unresolved_nodes'])} for k,v in plans.items()}}))
if __name__=='__main__':main()
