#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a small stock-kernel early userspace; never boot or install it."""
import gzip,hashlib,importlib.util,json,os,re,struct,subprocess,sys
from pathlib import Path
PKG=Path(__file__).resolve().parent.parent
REPO=PKG.parents[2]
PROJECT=REPO.parent
OUT=REPO/'Build/t70proPkg/Linux'
DHCP_APPLETS=['udhcpc','rm','rmdir','chmod','cmp']
RESOLVER_LINK=('etc/resolv.conf',0o120777,b'/run/teclast-connectivity/resolv.conf')

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def exact_kernel_release(image):
    releases=set(re.findall(rb"Linux version ([a-zA-Z0-9._+\-]+) ",image))
    if len(releases)!=1:raise ValueError("Kernel banner release is absent or ambiguous")
    return releases.pop().decode("ascii")

def required_symbols(raw):
    """Weak unresolved ELF symbols are optional, not load-order dependencies."""
    shoff=struct.unpack_from('<Q',raw,40)[0]
    shsize,count=struct.unpack_from('<HH',raw,58)
    sections=[struct.unpack_from('<IIQQQQIIQQ',raw,shoff+i*shsize) for i in range(count)]
    out=set()
    for section in sections:
        if section[1]!=2:continue
        strings=sections[section[6]];table=raw[strings[4]:strings[4]+strings[5]]
        for off in range(section[4],section[4]+section[5],section[9]):
            name,info,_,index,_,_=struct.unpack_from('<IBBHQQ',raw,off)
            if index==0 and info>>4==1:
                end=table.find(b'\0',name)
                if end<0:raise ValueError('Unterminated ELF symbol')
                out.add(table[name:end].decode('ascii'))
    return out

GPL_COMPATIBLE_LICENSES={
    'GPL','GPL v2','GPL and additional rights','Dual BSD/GPL','Dual MIT/GPL','Dual MPL/GPL'
}

def module_namespace_target(namespace,module_name):
    if not namespace.startswith('module:'):return None
    names=namespace[len('module:'):]
    if not names or names.strip()!=names:return {'supported':False,'allowed':False,'reason':'empty or whitespace-padded module namespace'}
    candidates=names.split(',')
    if any(not item for item in candidates):return {'supported':False,'allowed':False,'reason':'empty module-name item'}
    allowed=False
    for item in candidates:
        wildcard=item.endswith('*')
        name=item[:-1] if wildcard else item
        if not name or '*' in name or not re.fullmatch('[A-Za-z0-9_-]+',name):
            return {'supported':False,'allowed':False,'reason':'unsupported module namespace pattern'}
        if (module_name.startswith(name) if wildcard else module_name==name):allowed=True
    return {'supported':True,'allowed':allowed,'reason':'module name matched' if allowed else 'module name not listed'}

def import_policy(importer,provider,provider_module,stock_config):
    errors=[];warnings=[];unknowns=[]
    licenses=importer.get('license',[])
    if provider.get('export')=='EXPORT_SYMBOL_GPL':
        compatibility=[value in GPL_COMPATIBLE_LICENSES for value in licenses]
        if not compatibility or not any(compatibility):
            errors.append({'error':'GPL-only export requires a GPL-compatible MODULE_LICENSE','license_tags':licenses})
        elif not all(compatibility):
            unknowns.append({'unknown':'duplicate MODULE_LICENSE tags disagree on GPL compatibility; exact stock loader selection is unavailable','license_tags':licenses})
    namespace=provider.get('namespace','')
    if not isinstance(namespace,str):
        errors.append({'error':'provider export namespace metadata is malformed'})
        return errors,warnings,unknowns
    if namespace.startswith('module:'):
        decision=module_namespace_target(namespace,importer.get('name') or provider_module)
        if not decision['supported']:
            errors.append({'error':'unsupported module: export namespace syntax','namespace':namespace,'reason':decision['reason']})
        elif not decision['allowed']:
            errors.append({'error':'module is not authorized by module: export namespace','namespace':namespace})
    elif namespace:
        imports=importer.get('import_ns',[])
        if namespace not in imports:
            allow_missing=stock_config.get('MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS')
            if allow_missing=='y':
                warnings.append({'warning':'missing namespace import is allowed by stock config with a kernel warning','namespace':namespace})
            elif allow_missing=='n':
                errors.append({'error':'namespaced export requires a matching MODULE_IMPORT_NS','namespace':namespace})
            else:
                unknowns.append({'unknown':'kernel missing-namespace import policy is not supplied','namespace':namespace})
    return errors,warnings,unknowns

def plans(audit,inventory,members,elf,kernel,kernel_release=None,platform=None,policy=None,certificates=None):
    policy=policy or module('stock_module_policy',PKG/'Linux/prepare-stock-modules.py')
    modules={};exports={};signatures={}
    stock_config=audit.get('stock_kernel_config',{})
    for item in inventory:
        data=members[item['path']]['data']
        if hashlib.sha256(data).hexdigest()!=item['sha256']:raise ValueError('Module hash mismatch')
        parsed=elf(data);key=item['path']
        modules[key]=(item,parsed,required_symbols(data))
        signatures[key]=policy.verify_module_pkcs7(data,certificates,stock_config)
    by_name={}
    for key,(item,_,_) in modules.items():by_name.setdefault(item['name'],[]).append(key)
    # Resolve duplicate binaries only for this exact Image. A unique matching
    # release is a selection rule, never a substitute for the CRC/dependency
    # checks below. No directory precedence or arbitrary first match is used.
    selections={};excluded=set()
    if kernel_release:
        for name,keys in by_name.items():
            if len(keys)<2:continue
            exact=[k for k in keys if (modules[k][1].get('vermagic') or '').split(' ',1)[0]==kernel_release]
            if len(exact)!=1:continue
            chosen=exact[0];excluded.update(set(keys)-{chosen})
            selections[name]={'name':name,'selected_path':chosen,
                'selected_sha256':modules[chosen][0]['sha256'],
                'kernel_release':kernel_release,
                'criterion':'only duplicate variant with exact kernel Image release; imports and dependencies must still pass',
                'variants':[{'path':k,'sha256':modules[k][0]['sha256'],'vermagic':modules[k][1].get('vermagic')} for k in keys],
                'runtime_validated':False}
    for name,keys in by_name.items():by_name[name]=[k for k in keys if k not in excluded]
    for key,(_,parsed,_) in modules.items():
        if key in excluded:continue
        info=parsed.get('exported_symbol_info')
        for name,crc in parsed['exported_symbols'].items():
            detail=info.get(name) if isinstance(info,dict) else None
            if detail is None:
                detail={'export':'EXPORT_SYMBOL','namespace':'','namespace_valid':True}
            else:
                detail=dict(detail)
                detail['namespace_valid']=isinstance(detail.get('namespace'),str)
            exports.setdefault(name,[]).append((key,{'crc':crc,**detail}))
    dependencies={};problems={};aliases={};policy_warnings={};policy_unknowns={}
    stock_config=audit.get('stock_kernel_config',{})
    protection=audit.get('module_load_policy',{}).get('symbol_protection',{})
    protected_evidence=policy.read_symbol_policy_evidence(protection,'protected_symbol_exports_evidence')
    access_evidence=policy.read_symbol_policy_evidence(protection,'vendor_symbol_access_evidence')
    if access_evidence is not None:
        access_evidence={**access_evidence,'names':frozenset(access_evidence['names'])}
    def check_access(key,parsed,name,provider_kind,issues,unknown):
        if stock_config.get('MODULE_SIG_PROTECT')!='y':return
        decision=policy.vendor_symbol_access_check(name,parsed.get('signed',False),provider_kind,access_evidence,
            caller_trusted=signatures[key].get('trusted_for_offline_policy',False))
        if decision=='rejected':
            issues.append({'symbol':name,'error':'unsigned module import is absent from exact runtime vendor symbol whitelist'})
        elif decision!='clear':
            unknown.append({'symbol':name,'unknown':'runtime vendor symbol access: '+decision,'provider_kind':provider_kind})
    known_aliases={"device_apc_common_legacy":"device_apc_common", "mediatek_drm":"mediatek_drm_v1",
                   "mtk_gpufreq_wrapper":"mtk_gpufreq_wrapper_legacy",
                   "tmem_ffa":"trusted_mem", "mcDrvModule_ffa":"mcDrvModule", "mcupm_v3":"mcupm"}
    for key,(item,parsed,required) in modules.items():
        deps=set();issues=[];resolved_aliases=[];warnings=[];unknown=[]
        signature=signatures[key]
        trusted=signature.get('trusted_for_offline_policy',False)
        if parsed.get('signed',False) and not trusted:
            unknown.append({'unknown':'exact CPIO module signature trust was not verified',
                            'signature_verification':signature})
        if signature.get('status')=='invalid':
            issues.append({'error':'malformed module PKCS#7 signature header','reason':signature['reason']})
        for export_error in parsed.get('export_integrity_errors',[]):
            issues.append({'error':'export namespace/CRC metadata integrity failure','detail':export_error})
        if stock_config.get('MODULE_SIG_FORCE')=='y' and not parsed.get('signed',False):
            issues.append({'error':'CONFIG_MODULE_SIG_FORCE rejects a module without a signature trailer'})
        if stock_config.get('MODULE_SIG_PROTECT')=='y':
            decision=policy.protected_export_check(parsed['exported_symbols'],parsed.get('signed',False),protected_evidence,trusted=trusted)
            if decision['status']=='rejected':
                issues.append({'error':'unsigned module exports protected symbols','symbols':decision['symbols']})
            elif decision['status']!='clear':
                unknown.append({'unknown':'protected-export policy: '+decision['status'],'symbols':decision['symbols']})
        for namespace in parsed.get('import_ns',[]):
            if isinstance(namespace,str) and namespace.startswith('module:'):
                issues.append({'error':'MODULE_IMPORT_NS may not request the reserved module: namespace','namespace':namespace})
        importer={**parsed,'name':parsed.get('name') or item['name']}
        for name in sorted(required-parsed['modversions'].keys()):
            issues.append({'symbol':name,'error':'strong import has no version CRC'})
        for name,crc in parsed['modversions'].items():
            if name not in required:continue
            if name in kernel and kernel[name]['crc']==crc:
                provider={**kernel[name],'namespace':kernel[name].get('namespace',''),
                          'export':kernel[name].get('export','EXPORT_SYMBOL')}
                policy_errors,policy_warnings_for_import,policy_unknowns_for_import=import_policy(
                    importer,provider,'stock-vmlinux',stock_config)
                issues.extend({'symbol':name,**value} for value in policy_errors)
                warnings.extend({'symbol':name,**value} for value in policy_warnings_for_import)
                unknown.extend({'symbol':name,**value} for value in policy_unknowns_for_import)
                check_access(key,parsed,name,'kernel',issues,unknown)
                continue
            matching=[(provider_key,details) for provider_key,details in exports.get(name,[])
                      if details['crc']==crc and provider_key!=key]
            if len(matching)!=1:
                issues.append({'symbol':name,'provider_count':len(matching)})
                continue
            provider_key,provider=matching[0]
            deps.add(provider_key)
            provider_kind=('trusted-module' if signatures[provider_key].get('trusted_for_offline_policy',False) else
                           'signature-trailer' if modules[provider_key][1].get('signed',False) else 'unsigned-module')
            check_access(key,parsed,name,provider_kind,issues,unknown)
            if not provider.get('namespace_valid',True):
                policy_errors=[{'error':'exact CRC provider has malformed export namespace metadata'}]
                policy_warnings_for_import=[];policy_unknowns_for_import=[]
            else:
                policy_errors,policy_warnings_for_import,policy_unknowns_for_import=import_policy(
                    importer,provider,modules[provider_key][0]['name'],stock_config)
            issues.extend({'symbol':name,'provider':modules[provider_key][0]['name'],**value}
                          for value in policy_errors)
            warnings.extend({'symbol':name,'provider':modules[provider_key][0]['name'],**value}
                            for value in policy_warnings_for_import)
            unknown.extend({'symbol':name,'provider':modules[provider_key][0]['name'],**value}
                           for value in policy_unknowns_for_import)
        # Module initialization can require a declared provider without a direct
        # ELF call; keep that ordering dependency after symbol policy checks.
        for declared in parsed['depends']:
            keys=by_name.get(declared,[])
            if len(keys)==1:
                deps.add(keys[0]);continue
            candidate=by_name.get(known_aliases.get(declared,''),[])
            if not keys and len(candidate)==1 and candidate[0] in deps:
                resolved_aliases.append({'declared':declared,'provider':modules[candidate[0]][0]['name'],'evidence':'unique matching strong import CRC'})
            else:issues.append({'declared_dependency':declared,'provider_count':len(keys),'error':'provider or variant unresolved'})
        dependencies[key]=deps;problems[key]=issues;aliases[key]=resolved_aliases
        policy_warnings[key]=warnings;policy_unknowns[key]=unknown
    result={}
    for feature,plan in audit['all_hardware_feature_plans'].items():
        visiting=set();done=set();order=[];errors=[]
        def visit(key):
            if key in done:return
            if key in visiting:raise ValueError('dependency cycle: '+key)
            visiting.add(key)
            if problems[key]:errors.append({'module':modules[key][0]['name'],'imports':problems[key]})
            for dep in sorted(dependencies[key]):visit(dep)
            visiting.remove(key);done.add(key);order.append(key)
        board=(platform or {}).get(feature,{})
        errors.extend({'dt_supplier':issue} for issue in board.get('parse_errors',[]))
        roots=list(dict.fromkeys(board.get('roots',[])+board.get('requested_module_roots',plan['roots'])))
        for name in roots:
            keys=by_name.get(name,[])
            if len(keys)!=1:errors.append({'root':name,'variants':keys});continue
            try:visit(keys[0])
            except ValueError as e:errors.append({'error':str(e)})
        load_policy=audit.get('module_load_policy',{})
        result[feature]={'status':'blocked' if errors else 'offline order prepared',
                         'modules':[{'name':modules[k][0]['name'],'path':k} for k in order],
                         'errors':errors,'variant_selections':[v for v in selections.values() if v['selected_path'] in done],
                         'resolved_stock_name_aliases':[{'module':modules[k][0]['name'],**a} for k in order for a in aliases[k]],'runtime_validated':False,
                         'load_policy_ready':bool(load_policy.get('shared_policy_ready',load_policy.get('policy_ready',False))) and not errors and not any(policy_unknowns[k] for k in done),
                         'load_policy_unknowns':load_policy.get('shared_unknowns',load_policy.get('unknowns',[])),
                         'module_signatures':[{'module':modules[k][0]['name'],'sha256':modules[k][0]['sha256'],
                                               **signatures[k]} for k in order if modules[k][1].get('signed',False)],
                         'provider_policy_warnings':[{'module':modules[k][0]['name'],**value}
                                                     for k in order for value in policy_warnings[k]],
                         'provider_policy_unknowns':[{'module':modules[k][0]['name'],**value}
                                                     for k in order for value in policy_unknowns[k]],
                         'platform_suppliers':board}
    return result

def module_scripts(loadplans,release,sysfs='/sys'):
    # All commands use literal validated catalog names/paths. No arbitrary module
    # alias or user-supplied module path is passed to insmod.
    import shlex
    q=shlex.quote
    status=['#!/bin/sh','set -eu','[ "$#" = 1 ] || exit 64',
      'sysfs='+q(sysfs),'failed=0',
      'check_node() {',
      '  node="$1"; expected="$sysfs/firmware/devicetree/base$node"; found=0; bound=0',
      '  if [ ! -d "$expected" ]; then echo "node=$node state=missing-dt-node"; failed=1; return; fi',
      '  expected=$(readlink -f "$expected")',
      '  for device in "$sysfs"/bus/*/devices/*; do',
      '    [ -L "$device/of_node" ] || continue',
      '    [ "$(readlink -f "$device/of_node")" = "$expected" ] || continue',
      '    found=1',
      '    if [ -L "$device/driver" ] && [ -d "$device/driver" ]; then',
      '      echo "node=$node state=bound driver=$(readlink -f "$device/driver")"; bound=1',
      '    else echo "node=$node state=unbound-device"; failed=1; fi',
      '  done',
      '  if [ "$found" = 0 ]; then echo "node=$node state=no-enumerated-device"; failed=1; fi',
      '  [ "$bound" = 0 ] || echo "node=$node binding-observed=true hardware-operation-tested=false"',
      '}', 'case "$1" in']
    loader=['#!/bin/sh','set -eu',f'[ "$(uname -r)" = {q(release)} ] || {{ echo "Exact stock kernel required" >&2; exit 1; }}',
      '[ "$#" = 1 ] || { echo "Specify a module feature group" >&2; exit 64; }',
      'sha256sum -s -c /etc/teclast/modules.sha256 || { echo "Stock module integrity check failed" >&2; exit 1; }','case "$1" in']
    for feature,plan in sorted(loadplans.items()):
        if not re.fullmatch('[a-z_]+',feature):raise ValueError('Unsafe feature name')
        status.append(feature+')');loader.append(feature+')')
        for item in plan['modules']:
            if not re.fullmatch('[A-Za-z0-9_]+',item['name']) or not re.fullmatch('lib/modules/[A-Za-z0-9_./-]+',item['path']) or '..' in item['path'].split('/'):
                raise ValueError('Unsafe module identity/path')
        if plan['errors']:
            loader.append('  echo "Module providers/variants or dependency order unresolved for this group" >&2; exit 1;;')
        elif plan.get('load_policy_ready') is not True:
            loader.append('  echo "Module signature or symbol access policy unresolved for this group" >&2; exit 1;;')
        else:
            for item in plan['modules']:
                loader.append('  [ -d /sys/module/'+item['name']+' ] || insmod '+q('/'+item['path'])+' || { echo "Module insertion failed" >&2; exit 1; }')
            loader.append('  ;;')
        board=plan.get('platform_suppliers',{});nodes=list(board.get('inspected_device_nodes',[]))
        if board.get('native_lid_node'):nodes.append(board['native_lid_node'])
        for node in nodes:
            if not re.fullmatch('/[A-Za-z0-9,._+/@-]+',node) or '..' in node.split('/'):raise ValueError('Unsafe DT path')
            status.append('  check_node '+q(node))
        if not nodes:status.append('  echo "No exact DT device binding checks prepared for this group"; failed=1')
        for item in board.get('unresolved_nodes',[]):
            if item.get('static_kernel_provider_evidence'):
                status.append('  echo '+q('provider='+item['node']+' state=built-in-registration-present runtime-registration=unverified'))
            else:status.append('  echo '+q('provider='+item['node']+' state=unresolved-to-module built-in-status=unproven'))
        if board.get('unresolved_nodes') or board.get('multiple_match_nodes'):
            status.append('  echo "Supplier coverage needs further binding evidence"; failed=1')
        status.append('  ;;')
    loader+=['*) echo "Unknown module feature group" >&2; exit 64;;','esac','exec /sbin/teclast-devices "$1"']
    status+=['*) echo "Unknown module feature group" >&2; exit 64;;','esac',
      '[ "$failed" = 0 ] || exit 2','echo "DT driver bindings observed; hardware operation remains untested"']
    return ('\n'.join(loader)+'\n').encode(),('\n'.join(status)+'\n').encode()

def startup_script(features,run='/run/teclast-startup',loader='/sbin/teclast-modules',timeout='/bin/timeout',mdev='/bin/mdev'):
    """One feature at a time; binding incompleteness differs from a load failure."""
    import shlex
    q=shlex.quote
    names=sorted(features)
    if not names or any(not re.fullmatch('[a-z_]+',name) for name in names):
        raise ValueError('Unsafe/empty startup feature catalog')
    lines=['#!/bin/sh','set -eu','set -f','umask 077','export PATH=/bin:/sbin LC_ALL=C',
        '[ "$#" = 1 ] || exit 64','selection="$1"',
        '[ "${#selection}" -le 256 ] || exit 64',
        'run='+q(run),'loader='+q(loader),'timeout='+q(timeout),'mdev='+q(mdev),
        'case "$selection" in ""|,*|*,|*,,*) echo "Invalid module selection" >&2; exit 64;; esac',
        'oldifs="$IFS"; IFS=,; set -- $selection; IFS="$oldifs"',
        'seen=,',
        'for feature in "$@"; do',
        '  case "$feature" in none) [ "$#" = 1 ] || exit 64;; '+ '|'.join(names)+') :;; *) echo "Unknown module feature" >&2; exit 64;; esac',
        '  case "$seen" in *,$feature,*) echo "Duplicate module feature" >&2; exit 64;; esac',
        '  seen="$seen$feature,"',
        'done',
        # This directory is a per-boot lock; an existing run is never overwritten.
        'mkdir -m 700 "$run" || exit 73',
        'printf "feature\\texit_code\\tresult\\n" > "$run/results.tsv"',
        'state() { printf "%s\\n" "$1" > "$run/state.tmp"; mv "$run/state.tmp" "$run/state"; }',
        'trap \'state interrupted; exit 143\' HUP INT TERM',
        'if [ "$selection" = none ]; then state disabled; exit 0; fi',
        'state running','incomplete=0',
        'for feature in "$@"; do',
        '  printf "Starting feature %s\\n" "$feature"',
        '  if "$timeout" -s TERM -k 2 30 "$loader" "$feature" > "$run/$feature.log" 2>&1; then code=0; else code=$?; fi',
        '  case "$code" in',
        '    0) result=bindings-observed;;',
        '    2) result=bindings-incomplete; incomplete=1;;',
        '    *) result=load-failed;;',
        '  esac',
        '  printf "%s\\t%s\\t%s\\n" "$feature" "$code" "$result" >> "$run/results.tsv"',
        # A load/timeout failure is never automatically retried by another group.
        '  if [ "$code" != 0 ] && [ "$code" != 2 ]; then state failed; exit "$code"; fi',
        'done',
        'if "$timeout" -s TERM -k 2 10 "$mdev" -s > "$run/devices.log" 2>&1; then code=0; else code=$?; fi',
        'printf "device-nodes\\t%s\\tscan\\n" "$code" >> "$run/results.tsv"',
        'if [ "$code" != 0 ]; then state device-node-scan-failed; exit 1; fi',
        'if [ "$incomplete" = 1 ]; then state bindings-incomplete; exit 2; fi',
        'state bindings-observed',
        'echo "Driver binding checks finished; hardware operation remains untested"']
    return ('\n'.join(lines)+'\n').encode()


def payload_header(hashes):
    def digest(n):return ','.join('0x'+hashes[n]['sha256'][i:i+2] for i in range(0,64,2))
    return ('// Generated by Linux/build-early-userspace.py; exact approved payload identities.\n#ifndef T70_BOOT_PAYLOAD_H\n#define T70_BOOT_PAYLOAD_H\n'+
        '\n'.join(f'#define T70_{name}_BYTES {hashes[i]["bytes"]}U\n#define T70_{name}_SHA256 {{{digest(i)}}}' for i,name in enumerate(['KERNEL','INITRD','MODULES']))+'\n#define T70_FIRMWARE_OPTIONS L" firmware_class.path=/lib/firmware"\n#endif\n')

def connectivity_services():
    helper=PKG/'Linux/connectivity-services.sh'
    return {'helper':'/sbin/teclast-connectivity',
        'helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),
        'startup':'explicit only; PID1 never starts Wi-Fi/BT/GNSS services',
        'wifi':'external foreground wpa_supplicant nl80211 service with external config; wpa_state-only query',
        'wmt_control':{'helper':'/sbin/teclast-wmt-control','source_sha256':hashlib.sha256((PKG/'Linux/WmtControl.c').read_bytes()).hexdigest(),
            'readiness_header_sha256':hashlib.sha256((PKG/'Linux/WmtReady.h').read_bytes()).hexdigest(),
            'activation':'explicit wmt-run; cold SoC/common initialization and chip-selected ROM catalog; chip detection may power RF hardware',
            'readiness':'live service-loop handshake before/after radio start; not firmware-load or radio-link proof'},
        'bt_hci_bridge':{'helper':'/sbin/teclast-bt-hci','source_sha256':hashlib.sha256((PKG/'Linux/BtHciBridge.c').read_bytes()).hexdigest(),
            'activation':'explicit bt-run; stock transport open owns power; Linux HCI UART H4 via PTY',
            'prerequisite':'WMT ROM patch catalog and persistent control daemon; bluetooth/hci_uart modules',
            'radio_execution':False},
        'dhcp':'explicit foreground BusyBox udhcpc; validated IPv4/subnet/routers/DNS/RFC3442 routes; unchanged renew keeps addresses/routes; only successfully added resources are removed on deconfig/exit; cleanup failure retains ownership and blocks restart',
        'resolver':'/etc/resolv.conf -> /run/teclast-connectivity/resolv.conf; atomically aggregated per-interface fragments; no foreign resolver replacement',
        'dhcp_applets':DHCP_APPLETS,
        'radio_services':'bundled ARM64 wpa_supplicant / wpa_cli / BlueZ / D-Bus and exact DT_NEEDED libraries; no automatic association or pairing',
        'lte_inventory':{'helper':'/sbin/teclast-lte-inspect',
            'source_sha256':hashlib.sha256((PKG/'Linux/LteInspect.c').read_bytes()).hexdigest(),
            'operation':'stat/sysfs/proc comm inventory only; never opens CCCI monitor; no modem boot, ioctl, SIM query or attach'},
        'not_bundled':['gpsd'],
        'runtime_service_or_radio_execution':False}

def host_path(value):
    path=Path(value)
    # Existing reports use the original builder paths on either host. Never
    # copy a VM source/output tree just to read an existing artifact on macOS.
    if sys.platform=='darwin' and path.is_relative_to('/home/builder/teclast-t70pro'):
        return PROJECT/path.relative_to('/home/builder/teclast-t70pro')
    return path

def repack_seed(allowed,allow_platform_change=False):
    """Require the exact previous payloads and unchanged inputs outside this repack."""
    report_path=PROJECT/'analysis/linux-early-userspace.json'
    seed_bytes=report_path.read_bytes();report=json.loads(seed_bytes)
    for path,digest in report['source_sha256'].items():
        if path not in allowed and hashlib.sha256((REPO/path).read_bytes()).hexdigest()!=digest:
            raise ValueError('Repack requires unchanged compiled/planner inputs: '+path)
    for artifact in report['artifacts']:
        actual=host_path(artifact['path'])
        if actual.stat().st_size!=artifact['bytes'] or hashlib.sha256(actual.read_bytes()).hexdigest()!=artifact['sha256']:
            raise ValueError('Seed artifact does not match the previous report: '+artifact['path'])
    for path,digest in [(PROJECT/'analysis/linux-platform-modules.json',report['platform_plan_sha256']),
                        (host_path(report['firmware']['manifest']),report['firmware']['manifest_sha256'])]:
        if allow_platform_change and path==PROJECT/'analysis/linux-platform-modules.json':continue
        if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise ValueError('Repack requires unchanged plan/firmware manifest')
    header=PKG/'Applications/LinuxBoot/BootPayload.h'
    if header.read_text()!=payload_header(report['artifacts']):raise ValueError('Seed BootPayload.h is not current')
    mod=module('stock_modules_repack',PKG/'Linux/prepare-stock-modules.py')
    initrd=host_path(report['artifacts'][1]['path'])
    seed=mod.read_newc(gzip.decompress(initrd.read_bytes()))
    return report_path,seed_bytes,report,mod,seed

def repack_connectivity():
    """Update script/applets only, preserving verified compiled inputs exactly."""
    helper=PKG/'Linux/connectivity-services.sh'
    allowed={str(Path(__file__).relative_to(REPO)),str(helper.relative_to(REPO))}
    report_path,seed_bytes,report,mod,seed=repack_seed(allowed)
    checks=json.loads((PROJECT/'analysis/linux-connectivity-tests.json').read_text())
    if checks.get('status')!='passed' or checks.get('device_execution') is not False:raise ValueError('Connectivity tests are absent or failed')
    for path in [helper,PKG/'Linux/Tests/test_connectivity_services.py']:
        if checks['source_sha256'].get(str(path.relative_to(REPO)))!=hashlib.sha256(path.read_bytes()).hexdigest():
            raise ValueError('Connectivity source checks are stale')
    busybox=seed['bin/busybox']['data'];bb_sha=hashlib.sha256(busybox).hexdigest()
    proof=checks.get('busybox_offline_cli',{})
    if bb_sha!=report['busybox']['sha256'] or proof.get('binary_sha256')!=bb_sha:
        raise ValueError('Exact embedded BusyBox identity/CLI proof is missing')
    if proof.get('shell_syntax',{}).get('helper_sha256')!=hashlib.sha256(helper.read_bytes()).hexdigest():
        raise ValueError('Exact BusyBox shell syntax proof is stale')
    if proof['shell_syntax']['exit_code']!=0 or proof.get('hardware_execution') is not False:
        raise ValueError('BusyBox offline shell proof failed')
    required=set(DHCP_APPLETS+['ip','mv','readlink','sleep','cat','grep','sh'])
    if not required.issubset(proof.get('applets',{}).get('available',[])):
        raise ValueError('Exact BusyBox is missing required applets')
    for member,sha in [('init',report['init_sha256']),('sbin/teclast-graphics',report['graphics']['sha256'])]:
        if hashlib.sha256(seed[member]['data']).hexdigest()!=sha:raise ValueError('Compiled member identity mismatch')
    replacements={'sbin/teclast-connectivity':{'mode':0o100755,'data':helper.read_bytes()},
        RESOLVER_LINK[0]:{'mode':RESOLVER_LINK[1],'data':RESOLVER_LINK[2]}}
    replacements.update({'bin/'+name:{'mode':0o120777,'data':b'busybox'} for name in DHCP_APPLETS})
    publish_repack(report_path,seed_bytes,report,mod,seed,replacements,{
        'scope':'Connectivity script, DHCP applet links and resolver delegation only; compiled Init/Graphics/BusyBox and all module/firmware members preserved byte for byte',
        'compile_or_kernel_execution':False,
        'busybox_CLI':'Actual embedded ARM64 binary under bounded syscall mocks; no network or device execution'},[Path(__file__),helper])

def publish_repack(report_path,seed_bytes,report,mod,seed,replacements,provenance,sources):
    initrd=host_path(report['artifacts'][1]['path'])
    header=PKG/'Applications/LinuxBoot/BootPayload.h'
    members={**seed,**replacements}
    for name in list(members):
        for parent in Path(name).parents:
            if str(parent)!='.':members.setdefault(str(parent),{'mode':0o040755,'data':b''})
    base=mod.cpio_newc([(name,value['mode'],value['data']) for name,value in members.items()
                        if value['mode'] & 0o170000 != 0o040000])
    verify=mod.read_newc(base)
    if set(verify)!=set(members):raise ValueError('Repacked archive member set mismatch')
    for name,value in members.items():
        if verify[name]['data']!=value['data'] or verify[name]['mode']!=value['mode']:
            raise ValueError('Repacked archive readback mismatch: '+name)
    changed=sorted(name for name,value in replacements.items() if name not in seed or seed[name]!=value)
    packed=gzip.compress(base,mtime=0)
    next_artifacts=[dict(value) for value in report['artifacts']]
    next_artifacts[1].update(bytes=len(packed),sha256=hashlib.sha256(packed).hexdigest())
    report.update(artifacts=next_artifacts,connectivity_services=connectivity_services(),member_count=len(verify),
        repack_provenance={'seed_report_sha256':hashlib.sha256(seed_bytes).hexdigest(),
            'seed_archive_sha256':report['artifacts'][1]['sha256'],
            'host':sys.platform,'changed_members':changed,'other_members_preserved_byte_for_byte':True,
            'mode_preservation':True,**provenance})
    report['source_sha256'].update({str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources})
    # All input validation and CPIO readback precede publication. Keep the latest
    # artifact in place; the generated EFI header pins this exact new digest.
    temporary=initrd.with_suffix(initrd.suffix+'.pending')
    try:
        temporary.write_bytes(packed);os.replace(temporary,initrd)
    finally:
        if temporary.exists():temporary.unlink()
    header.write_text(payload_header(next_artifacts))
    report_path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':'repacked; not booted','changed_members':changed,'artifacts':next_artifacts}))

def tested_helper(source_name,binary_name,test_name):
    """Build a stock-device CLI and exercise its actual entry point with a mock."""
    source=PKG/'Linux'/f'{source_name}.c'
    fixture=PKG/'Linux/Tests'/f'{source_name}Test.c'
    before={str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest() for path in (source,fixture)}
    binary=OUT/binary_name
    test=OUT/test_name
    flags=['gcc','-static','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wl,--build-id=none']
    for path,output in ((source,binary),(fixture,test)):
        subprocess.run([*flags,str(path),'-o',str(output)],check=True)
    result=subprocess.run([str(test)],text=True,capture_output=True,timeout=15)
    if result.returncode:
        print(result.stdout+result.stderr,file=sys.stderr,flush=True)
        result.check_returncode()
    if before!={str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest() for path in (source,fixture)}:
        raise ValueError(binary_name+' inputs changed during build')
    data=binary.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=183:
        raise ValueError(binary_name+' is not ARM64 ELF')
    phoff=struct.unpack_from('<Q',data,32)[0];ents,count=struct.unpack_from('<HH',data,54)
    if any(struct.unpack_from('<I',data,phoff+i*ents)[0]==3 for i in range(count)):
        raise ValueError(binary_name+' is not static')
    path='sbin/'+binary_name
    proof={'helper':'/'+path,'source_sha256':before,'sha256':hashlib.sha256(data).hexdigest(),
           'mock_execution':result.stdout.strip(),'device_execution':False}
    return (path,0o100755,data),proof

def lte_inventory():
    """Compile and execute the inventory utility before packaging its exact ELF."""
    if sys.platform!='linux':raise ValueError('LTE inventory build requires ARM64 Linux builder')
    source=PKG/'Linux/LteInspect.c';test=PKG/'Linux/Tests/test_lte_inspect.py'
    before={str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [source,test]}
    binary=OUT/'teclast-lte-inspect'
    subprocess.run(['gcc','-static','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-Wl,--build-id=none',str(source),'-o',str(binary)],check=True)
    execution=subprocess.run([sys.executable,'-B',str(test),str(binary)],check=False,
        text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
    if execution.returncode:
        print(execution.stdout,file=sys.stderr,flush=True)
        execution.check_returncode()
    data=binary.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=183:raise ValueError('LTE inventory is not ARM64 ELF')
    phoff=struct.unpack_from('<Q',data,32)[0];ents,count=struct.unpack_from('<HH',data,54)
    if any(struct.unpack_from('<I',data,phoff+i*ents)[0]==3 for i in range(count)):raise ValueError('LTE inventory is not static')
    if before!={str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [source,test]}:
        raise ValueError('LTE inventory inputs changed during execution')
    proof={'status':'passed','source_sha256':before,'artifact':{'path':str(binary),'bytes':len(data),
        'sha256':hashlib.sha256(data).hexdigest()},'output':execution.stdout,
        'device_execution':False,'modem_control_implemented':False}
    (PROJECT/'analysis/linux-lte-inventory-tests.json').write_text(json.dumps(proof,indent=2)+'\n')
    return data,proof

def repack_radio():
    """Add the tested HCI bridge and refreshed module plan, preserving the desktop."""
    if sys.platform!='linux':raise ValueError('Radio repack requires ARM64 Linux builder')
    helper=PKG/'Linux/connectivity-services.sh';planner=PKG/'Linux/platform-modules.py'
    bridge=PKG/'Linux/BtHciBridge.c'
    control=PKG/'Linux/WmtControl.c'
    runtime=PKG/'Linux/radio-runtime.py'
    auditor=PKG/'Linux/prepare-stock-modules.py'
    sources=[Path(__file__),helper,planner,bridge,control,PKG/'Linux/WmtReady.h',
        PKG/'Linux/LteInspect.c',PKG/'Linux/Tests/test_lte_inspect.py',runtime,auditor]
    before={str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    report_path,seed_bytes,report,mod,seed=repack_seed(set(before),allow_platform_change=True)
    subprocess.run([sys.executable,str(PKG/'Linux/Tests/test_connectivity_services.py')],check=True)
    subprocess.run([sys.executable,str(PKG/'Linux/Tests/test_bt_hci_bridge.py'),'--locked'],check=True)
    proof=json.loads((PROJECT/'analysis/linux-bt-hci-tests.json').read_text())
    for source,digest in proof['source_sha256'].items():
        if hashlib.sha256((REPO/source).read_bytes()).hexdigest()!=digest:raise ValueError('Stale BT bridge proof')
    binary=host_path(proof['artifact']['path']);data=binary.read_bytes()
    if hashlib.sha256(data).hexdigest()!=proof['artifact']['sha256']:raise ValueError('BT bridge identity mismatch')
    if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=183:raise ValueError('BT bridge is not ARM64')
    phoff=struct.unpack_from('<Q',data,32)[0];ents,count=struct.unpack_from('<HH',data,54)
    if any(struct.unpack_from('<I',data,phoff+i*ents)[0]==3 for i in range(count)):raise ValueError('BT bridge is not static')
    subprocess.run([sys.executable,str(PKG/'Linux/Tests/test_wmt_control.py'),'--locked'],check=True)
    wmt_proof=json.loads((PROJECT/'analysis/linux-wmt-control-tests.json').read_text())
    for source,digest in wmt_proof['source_sha256'].items():
        if hashlib.sha256((REPO/source).read_bytes()).hexdigest()!=digest:raise ValueError('Stale WMT control proof')
    wmt_data=host_path(wmt_proof['artifact']['path']).read_bytes()
    if hashlib.sha256(wmt_data).hexdigest()!=wmt_proof['artifact']['sha256']:raise ValueError('WMT control identity mismatch')
    if wmt_data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',wmt_data,18)[0]!=183:raise ValueError('WMT control is not ARM64')
    wmt_phoff=struct.unpack_from('<Q',wmt_data,32)[0];wmt_ents,wmt_count=struct.unpack_from('<HH',wmt_data,54)
    if any(struct.unpack_from('<I',wmt_data,wmt_phoff+i*wmt_ents)[0]==3 for i in range(wmt_count)):raise ValueError('WMT control is not static')
    lte_data,lte_proof=lte_inventory()
    busybox=host_path(report['busybox']['path'])
    if hashlib.sha256(busybox.read_bytes()).hexdigest()!=report['busybox']['sha256']:raise ValueError('BusyBox identity changed')
    subprocess.run([str(busybox),'ash','-n',str(helper)],check=True)
    subprocess.run([sys.executable,str(planner)],check=True)
    platform_path=PROJECT/'analysis/linux-platform-modules.json'
    platform=json.loads(platform_path.read_text())
    for key,p in [('source_sha256',planner),('dtb_sha256',PROJECT/'analysis/stock-fdt/effective.dtb'),
                  ('stock_catalog_sha256',PROJECT/'analysis/stock-modules.json'),
                  ('manifest_sha256',PROJECT/'analysis/stock-binaries/manifest.json')]:
        if platform[key]!=hashlib.sha256(p.read_bytes()).hexdigest():raise ValueError('Stale platform plan: '+key)
    for source,digest in platform['support_source_sha256'].items():
        if hashlib.sha256((REPO/source).read_bytes()).hexdigest()!=digest:raise ValueError('Stale platform scanner')
    if platform['extraction_required']:raise ValueError('Missing supplier binaries')
    audit=json.loads((PROJECT/'analysis/linux-stock-module-audit.json').read_text())
    # The old archive belongs to the pinned seed auditor. The current auditor
    # regenerates the candidate below and its hash is pinned on publication.
    if audit['source_sha256']!=report['source_sha256'][str(auditor.relative_to(REPO))]:raise ValueError('Stock audit does not belong to the seed payload')
    raw=host_path(report['artifacts'][2]['path']).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=audit['package_sha256']:raise ValueError('Stock module package mismatch')
    # Prepare the refreshed closure separately; the current archive stays pinned
    # until every selected module and the new userspace archive pass validation.
    import argparse
    module_candidate=OUT/'radio-modules.cpio.pending'
    audit_candidate=PROJECT/'analysis/linux-radio-stock-audit.json'
    extra=set(audit['module_closure'])
    extra.update(name for feature in platform['features'].values() for name in feature['roots'])
    audit=mod.audit(argparse.Namespace(workspace=PROJECT,manifest=PROJECT/'analysis/stock-binaries/manifest.json',
        stock_modules=PROJECT/'analysis/stock-modules.json',hardware=PROJECT/'analysis/runtime-hardware.json',
        stock_config=PROJECT/'analysis/stock-kernel.config',feature=audit['requested_features'],extra_module=sorted(extra),
        kernel_release=report['exact_kernel_release'],candidate_config=None,candidate_vermagic=None,module_symvers=None,
        stock_kernel_exports=PROJECT/'analysis/stock-kernel-exports.json',stock_kernel_image=host_path(report['artifacts'][0]['path']),
        kernel_info=PROJECT/'analysis/kernel-info.json',package_output=module_candidate,report=audit_candidate))
    if not audit['package_output_written'] or audit['missing_modules'] or audit['payload_integrity_errors']:
        raise ValueError('Refreshed original module package failed validation')
    if audit['source_sha256']!=before[str(auditor.relative_to(REPO))]:raise ValueError('Refreshed stock auditor identity mismatch')
    raw=module_candidate.read_bytes()
    members=mod.read_newc(raw)
    inventory=json.loads(next(v['data'] for name,v in members.items() if name.endswith('/stock-binary-inventory.json')))
    kernel=json.loads((PROJECT/'analysis/stock-kernel-exports.json').read_text())
    image=host_path(report['artifacts'][0]['path']).read_bytes()
    if hashlib.sha256(image).hexdigest()!=kernel['image_sha256']:raise ValueError('Kernel catalog mismatch')
    certificates=mod.read_trusted_public_certificates(kernel,image,kernel['image_sha256'])
    loadplans=plans(audit,inventory['modules'],members,mod.elf_module,kernel['exports'],
        report['exact_kernel_release'],platform['features'],policy=mod,certificates=certificates)
    blocked=[name for name,plan in loadplans.items() if plan['errors'] or not plan['load_policy_ready']]
    if blocked:raise ValueError('Feature supplier/load plan is blocked: '+', '.join(sorted(blocked)))
    connectivity=loadplans['connectivity']
    (PROJECT/'analysis/linux-radio-module-plan.json').write_text(json.dumps(connectivity,indent=2)+'\n')
    if connectivity['errors'] or not connectivity['load_policy_ready']:raise ValueError('Connectivity load plan is blocked')
    required={'bt_drv_connac1x','btif_drv','wmt_drv','bluetooth','hci_uart'}
    if not required.issubset({x['name'] for x in connectivity['modules']}):raise ValueError('BT HCI transport modules absent from load plan')
    loader,status=module_scripts(loadplans,report['exact_kernel_release'])
    replacements={'sbin/teclast-wmt-control':{'mode':0o100755,'data':wmt_data},
        'sbin/teclast-lte-inspect':{'mode':0o100755,'data':lte_data},
        'sbin/teclast-bt-hci':{'mode':0o100755,'data':data},
        'sbin/teclast-connectivity':{'mode':0o100755,'data':helper.read_bytes()},
        'sbin/teclast-modules':{'mode':0o100755,'data':loader},
        'sbin/teclast-devices':{'mode':0o100755,'data':status},
        'sbin/teclast-start':{'mode':0o100755,'data':startup_script(loadplans)},
        'etc/teclast/module-plan.json':{'mode':0o100644,'data':(json.dumps(loadplans,indent=2)+'\n').encode()}}
    radio_entries,runtime_proof=module('radio_runtime',runtime).prepare()
    for name,mode,content in radio_entries:
        candidate={'mode':mode,'data':content}
        if name in seed and seed[name]!=candidate:raise ValueError('Radio runtime conflicts with existing payload: '+name)
        replacements[name]=candidate
    if before!={str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}:raise ValueError('Radio sources changed during repack')
    replacements['etc/teclast/modules.sha256']={'mode':0o100644,'data':''.join(
        item['sha256']+'  /'+item['path']+'\n' for item in inventory['modules']).encode()}
    report.update(module_plans=loadplans,platform_plan_sha256=hashlib.sha256(platform_path.read_bytes()).hexdigest())
    target=host_path(report['artifacts'][2]['path'])
    report['artifacts'][2].update(bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest())
    audit['package_output']=str(target)
    os.replace(module_candidate,target)
    (PROJECT/'analysis/linux-stock-module-audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    publish_repack(report_path,seed_bytes,report,mod,seed,replacements,{
        'scope':'Tested HCI H4 bridge, explicit connectivity helper and exact-stock module plans; existing desktop and all other members preserved',
        'BT_bridge_tests':proof,'WMT_control_tests':wmt_proof,'LTE_inventory_tests':lte_proof,
        'radio_userspace':runtime_proof,'kernel_HCI_attach':False,'radio_execution':False,'tablet_execution':False},sources)

def repack_console():
    """Recompile PID1 and package checked cross-built console; no VM/device boot."""
    if sys.platform!='darwin':
        raise ValueError('Use the full native Linux build on builder; cross repack is for macOS')
    sources=[Path(__file__),PKG/'Linux/Init.c',PKG/'Linux/console.py',PKG/'Linux/console-shell.sh']
    allowed={str(path.relative_to(REPO)) for path in sources}
    report_path,seed_bytes,report,mod,seed=repack_seed(allowed)
    for member,sha in [('init',report['init_sha256']),('bin/busybox',report['busybox']['sha256']),
                       ('sbin/teclast-graphics',report['graphics']['sha256'])]:
        if hashlib.sha256(seed[member]['data']).hexdigest()!=sha:
            raise ValueError('Seed compiled member identity mismatch: '+member)
    console=module('cross_console',PKG/'Linux/console.py')
    root=PROJECT.parents[2]
    entries,console_report=console.prepare_cross(root)
    for name,mode,data in entries:
        if name in seed and name not in {x['path'] for x in report.get('console',{}).get('package_files',[])}:
            raise ValueError('Console would replace an unrelated archive member: '+name)
    before={str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    pending=OUT/'init.pending';init=OUT/'init'
    command=['clang','--target=aarch64-linux-gnu','--sysroot='+str(root),
        '--gcc-toolchain='+str(root/'usr'),'-isystem',str(root/'usr/include/aarch64-linux-gnu'),
        '-B'+str(PROJECT/'tools/native-macos'),'-fuse-ld=lld',
        '-static','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wl,--build-id=none',
        str(PKG/'Linux/Init.c'),'-o',str(pending)]
    try:
        subprocess.run(command,check=True)
        data=pending.read_bytes()
        if console.arm64_elf(data) is not None:raise ValueError('Cross PID1 must be static ARM64 ELF')
        if before!={str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}:
            raise ValueError('Cross repack sources changed while compiling')
        report.update(status='cross-built and repacked; native Linux execution pending; not booted',
            init_sha256=hashlib.sha256(data).hexdigest(),init_source_sha256=before[str((PKG/'Linux/Init.c').relative_to(REPO))],
            console=console_report)
        publish_repack(report_path,seed_bytes,report,mod,seed,
            {'init':{'mode':0o100755,'data':data},**{n:{'mode':m,'data':d} for n,m,d in entries}},
            {'scope':'Recompiled PID1 plus cross-built console/runtime/configuration/licenses; all other members preserved byte for byte',
             'compiler':command,'native_linux_execution':False,'kernel_execution':False,'device_execution':False},sources)
        os.replace(pending,init)
    finally:
        if pending.exists():pending.unlink()

def desktop_payload(desktop):
    checks=json.loads((PROJECT/'analysis/linux-desktop-startup-tests.json').read_text())
    if checks.get('status')!='passed' or checks.get('device_execution') is not False:
        raise ValueError('Desktop startup tests are absent or failed')
    for path in [PKG/'Linux/Init.c',PKG/'Linux/desktop-start.sh',PKG/'Linux/Tests/test_desktop_startup.py']:
        if checks['source_sha256'].get(str(path.relative_to(REPO)))!=hashlib.sha256(path.read_bytes()).hexdigest():
            raise ValueError('Desktop startup tests are stale: '+str(path))
    return desktop.prepare_cross()


def repack_desktop():
    """Recompile PID1 and include the actual desktop; no OS/device execution."""
    desktop=module('cross_desktop',PKG/'Linux/desktop.py')
    sources=[Path(__file__),PKG/'Linux/Init.c',*desktop.packaging_sources()]
    allowed={str(path.relative_to(REPO)) for path in sources}
    report_path,seed_bytes,report,mod,seed=repack_seed(allowed)
    for member,digest in [('init',report['init_sha256']),('bin/busybox',report['busybox']['sha256']),
                          ('sbin/teclast-graphics',report['graphics']['sha256'])]:
        if hashlib.sha256(seed[member]['data']).hexdigest()!=digest:
            raise ValueError('Seed compiled member identity mismatch: '+member)
    entries,desktop_report=desktop_payload(desktop)
    owned={x['path'] for x in report.get('desktop',{}).get('package_files',[])}
    for name,mode,data in entries:
        if name in seed and name not in owned and seed[name]!={'mode':mode,'data':data}:
            raise ValueError('Desktop would replace an unrelated archive member: '+name)
    before={str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    pending=OUT/'init.pending'
    command=['clang','--target=aarch64-linux-gnu','--sysroot='+str(PROJECT.parents[2]),
        '--gcc-toolchain='+str(PROJECT.parents[2]/'usr'),'-isystem',str(PROJECT.parents[2]/'usr/include/aarch64-linux-gnu'),
        '-B'+str(PROJECT/'tools/native-macos'),'-fuse-ld=lld','-static','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-Wl,--build-id=none',str(PKG/'Linux/Init.c'),'-o',str(pending)]
    try:
        subprocess.run(command,check=True)
        data=pending.read_bytes()
        if desktop.elf_info(pending)['interpreter'] is not None:
            raise ValueError('Early PID1 must remain static ARM64 ELF')
        if before!={str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}:
            raise ValueError('Desktop repack sources changed during compile')
        report.update(status='cross-built and repacked with desktop; Linux execution pending; not booted',
            init_sha256=hashlib.sha256(data).hexdigest(),init_source_sha256=before[str((PKG/'Linux/Init.c').relative_to(REPO))],
            desktop=desktop_report)
        replacements={'init':{'mode':0o100755,'data':data},**{n:{'mode':m,'data':d} for n,m,d in entries}}
        publish_repack(report_path,seed_bytes,report,mod,seed,replacements,{
            'scope':'Recompiled PID1 plus actual Weston/PanVK runtime, input/cursor/font/configuration/licenses; all other members preserved byte for byte',
            'compiler':command,'native_linux_execution':False,'kernel_execution':False,'device_execution':False},sources)
        os.replace(pending,OUT/'init')
    finally:
        if pending.exists():pending.unlink()

def merge_entries(entries):
    """Share identical runtime files; reject conflicting file modes or bytes."""
    unique={}
    for name,mode,data in entries:
        entry=(name,mode,data)
        if name in unique and unique[name]!=entry:
            raise ValueError('Conflicting early-userspace member: '+name)
        unique[name]=entry
    return list(unique.values())

def main():
    if '--locked' not in sys.argv:
        os.execv(REPO/'Resources/Scripts/with-build-lock',[
          str(REPO/'Resources/Scripts/with-build-lock'),'T70Pro','Build exact-stock Linux early userspace and symbol-derived module plans','--',sys.executable,__file__,'--locked',*sys.argv[1:]])
    if '--repack-radio' in sys.argv:
        repack_radio();return
    if '--repack-connectivity' in sys.argv:
        repack_connectivity();return
    if '--repack-console' in sys.argv:
        repack_console();return
    if '--repack-desktop' in sys.argv:
        repack_desktop();return
    desktop=module('desktop_runtime',PKG/'Linux/desktop.py')
    desktop_entries,desktop_report=desktop_payload(desktop)
    OUT.mkdir(parents=True,exist_ok=True)
    mod=module('stock_modules',PKG/'Linux/prepare-stock-modules.py')
    firmware=module('stock_firmware',PKG/'Linux/prepare-stock-firmware.py')
    firmware_path=PROJECT/'analysis/stock-binaries/firmware-manifest.json'
    firmware_manifest=json.loads(firmware_path.read_text())
    if firmware_manifest['source_sha256']!=hashlib.sha256((PKG/'Linux/prepare-stock-firmware.py').read_bytes()).hexdigest():raise ValueError('Stale firmware preparation source')
    firmware_entries=firmware.entries(firmware_manifest,firmware_path.parent)
    stock=module('stock_kernel',PKG/'inspect-stock-kernel.py')
    audit=json.loads((PROJECT/'analysis/linux-stock-module-audit.json').read_text())
    if audit.get('source_sha256')!=hashlib.sha256((PKG/'Linux/prepare-stock-modules.py').read_bytes()).hexdigest():
        raise ValueError('Stale stock module signature/policy audit')
    raw=Path(audit['package_output']).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=audit['package_sha256']:raise ValueError('Module archive hash mismatch')
    members=mod.read_newc(raw)
    inv=json.loads(next(v['data'] for k,v in members.items() if k.endswith('/stock-binary-inventory.json')))
    kernel=json.loads((PROJECT/'analysis/stock-kernel-exports.json').read_text())
    kernel_image=stock.read_kernel()
    if hashlib.sha256(kernel_image).hexdigest()!=kernel['image_sha256']:raise ValueError('Kernel export catalog image mismatch')
    release=exact_kernel_release(kernel_image)
    firmware_search=kernel.get('firmware_search_parameter',{})
    if firmware_search.get('parameter')!='firmware_class.path' or firmware_search.get('value')!='/lib/firmware':
        raise ValueError('Exact stock firmware search parameter was not proved')
    platform_path=PROJECT/'analysis/linux-platform-modules.json'
    platform=json.loads(platform_path.read_text())
    planner=PKG/'Linux/platform-modules.py'
    for key,path in [('source_sha256',planner),('dtb_sha256',PROJECT/'analysis/stock-fdt/effective.dtb'),
                     ('stock_catalog_sha256',PROJECT/'analysis/stock-modules.json'),('manifest_sha256',PROJECT/'analysis/stock-binaries/manifest.json')]:
        if platform[key]!=hashlib.sha256(path.read_bytes()).hexdigest():raise ValueError('Stale platform supplier plan: '+key)
    for path,digest in platform['support_source_sha256'].items():
        if hashlib.sha256((REPO/path).read_bytes()).hexdigest()!=digest:raise ValueError('Stale platform scanner source: '+path)
    if platform['extraction_required']:raise ValueError('DT supplier modules must be extracted and audited first')
    certificates=mod.read_trusted_public_certificates(kernel,kernel_image,kernel['image_sha256'])
    loadplans=plans(audit,inv['modules'],members,mod.elf_module,kernel['exports'],release,platform['features'],
                    policy=mod,certificates=certificates)
    init=OUT/'init'
    subprocess.run(['gcc','-static','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wl,--build-id=none',str(PKG/'Linux/Init.c'),'-o',str(init)],check=True)
    graphics=OUT/'teclast-graphics'
    subprocess.run(['gcc','-static','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wl,--build-id=none',str(PKG/'Linux/Graphics.c'),'-o',str(graphics)],check=True)
    bt_bridge=OUT/'teclast-bt-hci'
    subprocess.run(['gcc','-static','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wl,--build-id=none',str(PKG/'Linux/BtHciBridge.c'),'-o',str(bt_bridge)],check=True)
    wmt_control=OUT/'teclast-wmt-control'
    subprocess.run(['gcc','-static','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wl,--build-id=none',str(PKG/'Linux/WmtControl.c'),'-o',str(wmt_control)],check=True)
    lte_data,lte_proof=lte_inventory()
    sensor_entry,sensor_proof=tested_helper('HfSensors','teclast-sensors','test-hf-sensors')
    camera_entry,camera_proof=tested_helper('CameraProbe','teclast-camera','test-camera-probe')
    busybox=Path('/usr/bin/busybox').resolve();bb=busybox.read_bytes()
    # Both binaries must be statically linked AArch64 executables.
    for name,data in [('init',init.read_bytes()),('busybox',bb),('graphics',graphics.read_bytes()),('bt-hci',bt_bridge.read_bytes()),('wmt-control',wmt_control.read_bytes())]:
        if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=183:raise ValueError(name+' is not ARM64 ELF')
        phoff=struct.unpack_from('<Q',data,32)[0];ents,count=struct.unpack_from('<HH',data,54)
        if any(struct.unpack_from('<I',data,phoff+i*ents)[0]==3 for i in range(count)):raise ValueError(name+' requires an ELF interpreter')
    helptext=subprocess.check_output([str(busybox),'mdev','--help'],stderr=subprocess.STDOUT,text=True)
    if 'netlink' not in helptext or '-df' not in helptext:raise ValueError('mdev netlink daemon unavailable')
    entries=[('init',0o100755,init.read_bytes()),('bin/busybox',0o100755,bb),('bin/sh',0o120777,b'busybox'),('sbin/teclast-graphics',0o100755,graphics.read_bytes())]
    entries.append(('sbin/teclast-bt-hci',0o100755,bt_bridge.read_bytes()))
    entries.append(('sbin/teclast-wmt-control',0o100755,wmt_control.read_bytes()))
    entries.append(('sbin/teclast-lte-inspect',0o100755,lte_data))
    entries.extend((sensor_entry,camera_entry))
    applets=['mount','umount','cat','ls','mkdir','dmesg','insmod','sha256sum','uname','ip','sleep','mdev','stat','readlink','kill','sync','stty','setsid','hexdump','grep','mv','timeout']+DHCP_APPLETS
    available=set(subprocess.check_output([str(busybox),'--list'],text=True).splitlines())
    if set(applets+['sh'])-available:raise ValueError('Required BusyBox applets unavailable')
    for app in applets:
        entries.append(('bin/'+app,0o120777,b'busybox'))
    radio_entries,radio_runtime=module('radio_runtime',PKG/'Linux/radio-runtime.py').prepare()
    entries+=radio_entries
    entries.append(RESOLVER_LINK)
    entries += [('etc/profile',0o100644,b'export PATH=/bin:/sbin\nexport HOME=/\n'),('etc/mdev.conf',0o100644,b'# Device nodes only; no command execution or implicit module loading.\n.* 0:0 0600\n')]
    checks=''.join(f"{item['sha256']}  /{item['path']}\n" for item in inv['modules'])
    entries.append(('etc/teclast/modules.sha256',0o100644,checks.encode()))
    loader,status=module_scripts(loadplans,release)
    entries.append(('sbin/teclast-modules',0o100755,loader))
    entries.append(('sbin/teclast-devices',0o100755,status))
    entries.append(('sbin/teclast-start',0o100755,startup_script(loadplans)))
    connectivity_helper=PKG/'Linux/connectivity-services.sh'
    entries.append(('sbin/teclast-connectivity',0o100755,connectivity_helper.read_bytes()))
    entries.append(('etc/teclast/module-plan.json',0o100644,(json.dumps(loadplans,indent=2)+'\n').encode()))
    console=module('stock_console',PKG/'Linux/console.py')
    console_entries,console_report=console.prepare()
    entries+=console_entries
    entries+=desktop_entries
    entries+=firmware_entries
    # Console, desktop and radio share libraries and the ELF interpreter.
    # Keep identical files once;
    # differing bytes or permissions must never silently replace another service.
    entries=merge_entries(entries)
    # /init creates /dev itself because the exact stock kernel has no devtmpfs.
    base=mod.cpio_newc(entries)
    verify=mod.read_newc(base)
    for name,mode,data in entries:
        if verify[name]['data']!=data or verify[name]['mode']!=mode:raise ValueError('Early archive readback mismatch')
    initrd=OUT/'early-userspace.cpio.gz';initrd.write_bytes(gzip.compress(base,mtime=0))
    image=OUT/'Image';image.write_bytes(kernel_image)
    files=[image,initrd,Path(audit['package_output'])]
    hashes=[]
    for p in files:
        with p.open('rb') as stream:digest_value=hashlib.file_digest(stream,'sha256').hexdigest()
        hashes.append({'path':str(p),'bytes':p.stat().st_size,'sha256':digest_value})
    app=PKG/'Applications/LinuxBoot';app.mkdir(parents=True,exist_ok=True)
    (app/'BootPayload.h').write_text(payload_header(hashes))
    report={'status':'built; not booted','scope':'Exact stock EFI-stub Image plus independent early-userspace and stock module archives; no device access/install',
            'artifacts':hashes,'busybox':{'version':subprocess.check_output(['dpkg-query','-W','-f=${Version}','busybox-static'],text=True),'path':str(busybox),'sha256':hashlib.sha256(bb).hexdigest()},
            'exact_kernel_release':release,'init_sha256':hashlib.sha256(init.read_bytes()).hexdigest(),'init_source_sha256':hashlib.sha256((PKG/'Linux/Init.c').read_bytes()).hexdigest(),
            'module_loading':'PID1 asynchronous command-line selected startup worker; none by default; strict feature allowlist, no retries after load failure, per-feature timeout/log/result; exact hashes/release and actual OF binding checks',
            'platform_plan_sha256':hashlib.sha256(platform_path.read_bytes()).hexdigest(),
            'graphics':{'helper':'/sbin/teclast-graphics','sha256':hashlib.sha256(graphics.read_bytes()).hexdigest(),'source_sha256':hashlib.sha256((PKG/'Linux/Graphics.c').read_bytes()).hexdigest(),'activation':'explicit --probe or --pattern on one primary DRM card; never auto-started','display':'stock DRM/KMS dumb XRGB8888 buffer, native panel mode, CPU color pattern and original CRTC restoration','gpu_acceleration':False,'device_execution':False},
            'console':console_report,
            'desktop':desktop_report,
            'connectivity_services':connectivity_services(),
            'LTE_inventory_tests':lte_proof,
            'sensor_helper':sensor_proof,
            'camera_helper':camera_proof,
            'firmware':{'manifest':str(firmware_path),'manifest_sha256':hashlib.sha256(firmware_path.read_bytes()).hexdigest(),'file_count':len(firmware_entries),'bytes':firmware_manifest['total_bytes'],'paths':[x[0] for x in firmware_entries],'activation':'EFI passes the exact stock firmware_class.path parameter before any module startup; kernel firmware requests can find these files; runtime loading untested','search_parameter':firmware_search},
            'source_sha256':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path(__file__),PKG/'Linux/Init.c',PKG/'Linux/Graphics.c',PKG/'Linux/BtHciBridge.c',PKG/'Linux/WmtControl.c',PKG/'Linux/WmtReady.h',PKG/'Linux/LteInspect.c',PKG/'Linux/Tests/test_lte_inspect.py',PKG/'Linux/radio-runtime.py',PKG/'Linux/prepare-stock-modules.py',PKG/'Linux/platform-modules.py',PKG/'Linux/prepare-stock-firmware.py',connectivity_helper,PKG/'Linux/console.py',PKG/'Linux/console-shell.sh']},'module_plans':loadplans,'member_count':len(verify),
            'limitations':['No tablet boot, driver load or graphical console test','Runtime DTB with modem/display reservations still required; static DTB is not bundled','Strong-import CRC and declared dependency order is not runtime hardware initialization proof','No modem firmware/calibration/Android RIL/userspace is copied; no SIM/radio operations','No internal partition/rootfs mount or persistent writes in PID1; runtime power/firmware policy remains unverified']}
    report['source_sha256'].update({
        str(path.relative_to(REPO)):hashlib.sha256(path.read_bytes()).hexdigest()
        for path in desktop.packaging_sources()
    })
    report['source_sha256'].update(sensor_proof['source_sha256'])
    report['source_sha256'].update(camera_proof['source_sha256'])
    (PROJECT/'analysis/linux-early-userspace.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'artifacts':hashes,'module_plans':{k:{'status':v['status'],'modules':len(v['modules']),'errors':len(v['errors'])} for k,v in loadplans.items()}}))
if __name__=='__main__':main()
