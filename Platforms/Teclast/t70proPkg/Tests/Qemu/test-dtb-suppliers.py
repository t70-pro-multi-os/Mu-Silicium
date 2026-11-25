#!/usr/bin/env python3
"""Check original USB/UFS supplier links and the DTs used for full-system execution."""
import argparse, copy, hashlib, json, runpy, sys
from pathlib import Path

ap=argparse.ArgumentParser()
ap.add_argument('--root',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args()
k=a.root/'Mu-Silicium/Platforms/Teclast/t70proPkg'
parse=runpy.run_path(str(k/'Linux/platform-modules.py'))
stock=a.root/'analysis/stock-fdt/effective.dtb'
raw=stock.read_bytes()
ns,hs=parse['tree'](raw)
edges,errors=parse['graph'](ns,hs)
usb='/soc/usb0@11200000';ufs='/soc/ufshci@11270000';ext='/soc/extcon_usb'
paths=[usb,ufs,ext,'/soc/i2c@1101a000','/soc/i2c@1101a000/sc8989x@6A','/soc/i2c@1101a000/husb311@4e']
assert all(ns[p]['enabled'] and not errors[p] for p in paths)
assert parse['cells'](ns[usb]['properties']['reg'])==[0,0x11200000,0,0x10000,0,0x11f40000,0,0x10000]
assert parse['cells'](ns[ufs]['properties']['reg'])==[0,0x11270000,0,0x2300]
assert 'phys' not in ns[ufs]['properties']
assert parse['strings'](ns[ufs]['properties']['clock-names'])==['ufs','ufs_mp','unipro_tick','unipro_sys','ufs_aes','ufs_ck']
assert parse['strings'](ns[ufs]['properties']['reset-names'])==['hci_rst','unipro_rst','crypto_rst']
for prop,pin in [('id-gpios',10),('usb_msel-gpios',99),('usb_5v_en-gpios',4),('hub_reset-gpios',104)]:
    val=parse['cells'](ns[ext]['properties'][prop]);assert hs[val[0]]=='/soc/pinctrl' and val[1:]==[pin,0]
vbus=hs[parse['cells'](ns[ext]['properties']['vbus-supply'])[0]]
assert vbus=='/soc/i2c@1101a000/sc8989x@6A/usb-otg-vbus'
vcc=hs[parse['cells'](ns[ufs]['properties']['vcc-supply'])[0]]
assert parse['strings'](ns[vcc]['properties']['regulator-name'])==['vemc']
report={'stock_dtb':{'path':str(stock),'sha256':hashlib.sha256(raw).hexdigest()},'original_supplier_links':{p:edges[p] for p in paths},'cases':[],
        'limits':'Checks typed DT references and preserved original properties; does not prove electrical state, cold initialization or physical storage operation.',
        'physical_hardware_validated':False,'physical_upload_eligible':False}
# Requiring only copied parents hid omitted graph endpoints and custom references.
usb_topology_paths=[usb,ext,usb+'/port',usb+'/port/endpoint@0',
    ext+'/port',ext+'/port/endpoint@0','/soc/pericfg@10003000',
    '/soc/infracfg_ao@10001000','/soc/efuse@11c10000',
    '/soc/efuse@11c10000/u2_phy_data','/soc/otg_iddig',
    '/soc/usb-phy@11f40000/usb-phy@11f40000',
    '/soc/i2c@1101a000/husb311@4e']
sys.path.insert(0,str(k/'Tests/Qemu'))
from usb_board_fixture import runtime_tree as usb_tree
from ufs_fixture import runtime_tree as ufs_tree
base=runpy.run_path(str(k/'verify-boot.py'))['runtime_fdt_fixture'](simple=True,console=True,ccci_base=0x4f000100)
for name,builders in [('usb',[usb_tree]),('ufs',[ufs_tree]),('usb+ufs',[ufs_tree,usb_tree])]:
    tree=base;sources=[]
    for build in builders:
        tree,info=build(tree,k,a.root);sources+=info['source_nodes']
    nodes,handles=parse['tree'](tree);e,err=parse['graph'](nodes,handles)
    failures={p:x for p,x in err.items() if x}
    assert all(nodes[p]['properties']==ns[p]['properties'] for p in set(sources))
    if usb_tree in builders:
        for path in usb_topology_paths:
            if path not in nodes:failures[path]=['missing original USB topology node']
            elif not nodes[path]['enabled'] or nodes[path]['properties']!=ns[path]['properties']:
                failures[path]=['disabled or changed original USB topology node']
    report['cases'].append({'name':name,'nodes':len(nodes),'sha256':hashlib.sha256(tree).hexdigest(),'supplier_reference_errors':failures,'all_copied_properties_match_stock':True})
report['status']='passed' if all(not c['supplier_reference_errors'] for c in report['cases']) else 'failed'
a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'status':report['status'],'cases':[{'name':c['name'],'errors':c['supplier_reference_errors']} for c in report['cases']]}))
assert report['status']=='passed', 'QEMU runtime DT has unresolved supplier references'
from usb_board_fixture import validate_usb_topology
negative_cases=[]
for name,path,prop,value in [
    ('missing controller endpoint',usb+'/port/endpoint@0',None,None),
    ('missing extcon endpoint',ext+'/port/endpoint@0',None,None),
    ('missing peripheral registers','/soc/pericfg@10003000',None,None),
    ('missing PHY calibration cell','/soc/efuse@11c10000/u2_phy_data',None,None),
    ('wrong endpoint target',usb+'/port/endpoint@0','remote-endpoint',ns[usb+'/port/endpoint@0']['properties']['phandle']),
    ('wrong Type-C port name',ext,'tcpc',b'wrong_port\0'),
    ('disabled supplier','/soc/infracfg_ao@10001000','status',b'disabled\0')]:
    bad=copy.deepcopy(ns)
    if prop is None:del bad[path]
    else:
        bad[path]['properties'][prop]=value
        if prop=='status':bad[path]['enabled']=False
    try:validate_usb_topology(bad,hs)
    except ValueError:negative_cases.append({'name':name,'rejected':True})
    else:raise AssertionError('Malformed USB topology accepted: '+name)
validate_usb_topology(ns,hs)
report['negative_topology_cases']=negative_cases
a.output.write_text(json.dumps(report,indent=2)+'\n')
print('USB topology: 7 missing/miswired/disabled cases rejected')
