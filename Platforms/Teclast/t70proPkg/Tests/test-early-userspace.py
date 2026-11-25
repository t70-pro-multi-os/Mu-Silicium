#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise real ELF import extraction, dependency selection and produced archives."""
import gzip,hashlib,importlib.util,json,logging,os,struct,subprocess,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
PKG=Path(__file__).resolve().parent.parent
REPO=PKG.parents[2]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
pack=load('early_pack',PKG/'Linux/build-early-userspace.py')
stock=load('stock_pack',PKG/'Linux/prepare-stock-modules.py')
platform=load('platform_pack',PKG/'Linux/platform-modules.py')
firmware=load('firmware_pack',PKG/'Linux/prepare-stock-firmware.py')
kernel=load('kernel_providers',PKG/'inspect-stock-kernel.py')

class GraphicsNativeTests(unittest.TestCase):
    def test_production_kms_lifecycle_and_guarded_framebuffers(self):
        with tempfile.TemporaryDirectory(prefix='graphics-test-',dir=REPO/'Build') as directory:
            exe=Path(directory)/'graphics-test'
            compiled=subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                str(PKG/'Linux/Tests/GraphicsTest.c'),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            completed=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
            self.assertIn('checks PASS',completed.stdout)
            self.assertIn('no device access',completed.stdout)
            report={'status':'passed','scope':'Actual Graphics.c; mocked kernel ioctls and guarded mappings; no DRM device access',
                'output':completed.stdout.strip(),'sanitizers':['address','undefined'],
                'source_sha256':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in [PKG/'Linux/Graphics.c',PKG/'Linux/Tests/GraphicsTest.c',Path(__file__)]}}
            (REPO.parent/'analysis/linux-graphics-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    def test_graphics_binary_is_in_verified_archive(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        raw=gzip.decompress(pack.host_path(report['artifacts'][1]['path']).read_bytes())
        members=stock.read_newc(raw);data=members['sbin/teclast-graphics']['data']
        self.assertEqual(hashlib.sha256(data).hexdigest(),report['graphics']['sha256'])
        self.assertEqual(data[:6],b'\x7fELF\x02\x01')
        self.assertEqual(struct.unpack_from('<H',data,18)[0],183)
        self.assertFalse(report['graphics']['gpu_acceleration'])
        self.assertFalse(report['graphics']['device_execution'])
        self.assertEqual(report['graphics']['source_sha256'],hashlib.sha256((PKG/'Linux/Graphics.c').read_bytes()).hexdigest())

class SharedRuntimeArchive(unittest.TestCase):
    def test_real_shared_interpreter_survives_cpio_assembly(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        members=stock.read_newc(gzip.decompress(pack.host_path(report['artifacts'][1]['path']).read_bytes()))
        names=['lib/ld-linux-aarch64.so.1','sbin/wpa_supplicant','sbin/teclast-wmt-control']
        original=[(name,members[name]['mode'],members[name]['data']) for name in names]
        self.assertEqual(original[0][2][:4],b'\x7fELF')
        # Both console and radio request the same loader path in a full build.
        merged=pack.merge_entries(original+original[:1])
        decoded=stock.read_newc(stock.cpio_newc(merged))
        self.assertEqual(set(decoded),set(names)|{'lib','sbin'})
        for directory in ['lib','sbin']:
            self.assertEqual(decoded[directory],{'mode':0o040755,'data':b''})
        for name,mode,data in original:
            self.assertEqual((decoded[name]['mode'],decoded[name]['data']),(mode,data))

    def test_shared_runtime_with_conflicting_bytes_is_rejected(self):
        first=('lib/ld-linux-aarch64.so.1',0o100755,b'loader-a')
        with self.assertRaisesRegex(ValueError,'Conflicting early-userspace member'):
            pack.merge_entries([first,(first[0],first[1],b'loader-b')])

    def test_shared_runtime_with_conflicting_permissions_is_rejected(self):
        first=('lib/ld-linux-aarch64.so.1',0o100755,b'loader-a')
        with self.assertRaisesRegex(ValueError,'Conflicting early-userspace member'):
            pack.merge_entries([first,(first[0],0o120777,first[2])])

class KernelBuiltinProviders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        previous=logging.root.manager.disable;logging.disable(logging.CRITICAL)
        try:
            cls.raw=kernel.read_kernel();k=kernel.KallsymsFinder(cls.raw,bit_size=64)
        finally:logging.disable(previous)
        cls.symbols=k.name_to_symbol;cls.base=cls.symbols['_text'].virtual_address
        cls.values_by_name={}
        for symbol in k.symbols:
            cls.values_by_name.setdefault(symbol.name,set()).add(symbol.virtual_address)
    def check_mutation(self,pos,data):
        raw=bytearray(self.raw);raw[pos:pos+len(data)]=data
        with self.assertRaises(ValueError):kernel.builtin_provider_evidence(raw,self.symbols,self.base)
    def test_real_image_driver_pointers_registration_and_early_clock(self):
        result=kernel.builtin_provider_evidence(self.raw,self.symbols,self.base)
        clock,regulator=result['providers']
        self.assertEqual(clock['compatible'],['fixed-clock']);self.assertIn('early_declaration',clock)
        self.assertEqual(regulator['compatible'],['regulator-fixed','regulator-fixed-clock','regulator-fixed-domain'])
        self.assertFalse(clock['runtime_registration_verified'])
        self.assertFalse(regulator['runtime_registration_verified'])
    def test_mutated_driver_probe_table_and_callback_pointers_rejected(self):
        for name,delta in [('of_fixed_clk_driver',0),('regulator_fixed_voltage_driver',80),('__of_table_fixed_clk',192)]:
            self.check_mutation(self.symbols[name].virtual_address-self.base+delta,bytes(8))
    def test_wrong_registration_branch_and_initcall_rejected(self):
        self.check_mutation(self.symbols['regulator_fixed_voltage_init'].virtual_address-self.base+24,struct.pack('<I',0x94000000))
        name=next(n for n in self.symbols if n.startswith('__initcall__') and n.endswith('_of_fixed_clk_driver_init6'))
        self.check_mutation(self.symbols[name].virtual_address-self.base,bytes(4))
    def test_unterminated_compatible_cannot_be_treated_as_builtin(self):
        self.check_mutation(self.symbols['fixed_of_match'].virtual_address-self.base+64,b'A'*128)

    def test_exact_firmware_path_parameter_and_malformed_callbacks(self):
        result=kernel.firmware_parameter_evidence(self.raw,self.symbols,self.base)
        self.assertEqual((result['parameter'],result['value']),('firmware_class.path','/lib/firmware'))
        self.assertFalse(result['runtime_parameter_acceptance_verified'])
        for name,delta,width in [('__param_path',0,8),('__param_path',16,8),('__param_path',24,2),('firmware_param_ops',8,8),('firmware_param_ops',16,8),('__param_str_path',0,1)]:
            raw=bytearray(self.raw);pos=self.symbols[name].virtual_address-self.base+delta
            raw[pos:pos+width]=bytes(width)
            with self.assertRaises(ValueError):kernel.firmware_parameter_evidence(raw,self.symbols,self.base)

    def test_protected_export_names_and_unsigned_module_check_are_recovered(self):
        result=kernel.protected_symbol_exports_evidence(
            self.raw,self.symbols,self.base,self.values_by_name)
        self.assertEqual(result['status'],'verified')
        self.assertEqual(result['image_sha256'],kernel.EXPECTED)
        self.assertEqual(result['count'],590)
        self.assertEqual(len(result['names']),590)
        self.assertEqual(result['names'][0],'__SCK__tp_func_9p_fid_ref')
        self.assertEqual(result['names'][-1],'wwan_unregister_ops')
        self.assertEqual(result['names'],sorted(set(result['names'])))
        self.assertEqual(result['verified_callsite']['status'],'verified')
        self.assertEqual(result['verified_callsite']['function'],'complete_formation')
        self.assertEqual(result['verified_callsite']['search'],'bsearch')
        self.assertEqual(result['verified_callsite']['unsigned_module_export_match_returns'],
                         '-EACCES (-13)')
        self.assertFalse(result['verified_callsite']['runtime_execution_verified'])
        self.assertEqual(result['source_module_names']['status'],'not_recovered_from_Image')
        self.assertFalse(result['source_module_names']['runtime_required_for_protected_export_check'])

    def test_vendor_import_allowlist_callsite_and_adjacent_ranges_are_verified(self):
        protected=kernel.protected_symbol_exports_evidence(
            self.raw,self.symbols,self.base,self.values_by_name)
        result=kernel.vendor_symbol_access_evidence(
            self.raw,self.symbols,self.base,self.values_by_name,protected)
        self.assertEqual(result['status'],'verified')
        self.assertEqual(result['count'],9176)
        self.assertEqual(len(result['names']),9176)
        self.assertEqual(result['names'],sorted(set(result['names'])))
        self.assertEqual(result['array_image_range'][0],
                         protected['array_image_range'][1])
        self.assertEqual(result['count_storage'],'protected_symbol_exports_count + 8')
        self.assertEqual(result['verified_callsite']['status'],'verified')
        self.assertEqual(result['verified_callsite']['function'],'resolve_symbol')
        self.assertTrue(result['verified_callsite']['unsigned_caller_to_kernel_or_signed_provider_is_checked'])
        self.assertTrue(result['verified_callsite']['unsigned_provider_bypasses_import_allowlist'])
        self.assertEqual(result['verified_callsite']['missing_name_returns'],'-EACCES (-13)')
        self.assertFalse(result['verified_callsite']['runtime_execution_verified'])
        self.assertEqual(result['source_kmi_whitelist']['status'],'not_recovered_from_Image')
        self.assertFalse(result['source_kmi_whitelist']['source_input_required_for_runtime_check'])
        self.assertEqual(result['signature_trust']['status'],'unknown')

    def test_builtin_public_certificate_table_is_bounded_without_private_key_access(self):
        evidence=kernel.trusted_public_certificate_evidence(self.raw,self.symbols,self.base,self.values_by_name)
        self.assertEqual(evidence["count"],1)
        self.assertEqual(evidence["certificates"][0]["sha256"],"799764380a24c8d38d971588961dadd118ab1f013797aad81a5529b8e720f080")
        self.assertFalse(evidence["private_key_access"])
        offset=int(evidence["image_range"][0],16)
        for relative,value in ((0,0),(1,0x80),(2,0xff)):
            raw=bytearray(self.raw);raw[offset+relative]=value
            with self.assertRaises(ValueError):
                kernel.trusted_public_certificate_evidence(raw,self.symbols,self.base,self.values_by_name)

    def test_protected_export_bad_count_pointer_terminator_and_callsite_are_rejected(self):
        count_off=self.symbols['protected_symbol_exports_count'].virtual_address-self.base
        array_off=self.symbols['protected_symbol_exports'].virtual_address-self.base
        function_off=self.symbols['complete_formation'].virtual_address-self.base
        raw=bytearray(self.raw);struct.pack_into('<Q',raw,count_off,589)
        with self.assertRaises(ValueError):
            kernel.protected_symbol_exports_evidence(raw,self.symbols,self.base,self.values_by_name)
        raw=bytearray(self.raw);struct.pack_into('<Q',raw,array_off,self.base+len(raw))
        with self.assertRaises(ValueError):
            kernel.protected_symbol_exports_evidence(raw,self.symbols,self.base,self.values_by_name)
        raw=bytearray(self.raw);tail=len(raw)-512;raw[tail:]=b'A'*512
        struct.pack_into('<Q',raw,array_off,self.base+tail)
        with self.assertRaises(ValueError):
            kernel.protected_symbol_exports_evidence(raw,self.symbols,self.base,self.values_by_name)
        raw=bytearray(self.raw);raw[function_off+0xec:function_off+0xf0]=bytes(4)
        with self.assertRaises(ValueError):
            kernel.protected_symbol_exports_evidence(raw,self.symbols,self.base,self.values_by_name)
        for offset in (0xcc,0xd0,0xdc,0xe0,0xe4,0xe8,0xf0,0x16c,0x170,0x180,0x190):
            raw=bytearray(self.raw);raw[function_off+offset:function_off+offset+4]=bytes(4)
            with self.subTest(critical_instruction=hex(offset)), self.assertRaises(ValueError):
                kernel.protected_symbol_exports_evidence(raw,self.symbols,self.base,self.values_by_name)

    def test_vendor_import_bad_count_overlap_pointer_and_callsite_are_rejected(self):
        protected=kernel.protected_symbol_exports_evidence(
            self.raw,self.symbols,self.base,self.values_by_name)
        count_off=self.symbols['protected_symbol_exports_count'].virtual_address-self.base
        resolve_off=self.symbols['resolve_symbol'].virtual_address-self.base
        raw=bytearray(self.raw);struct.pack_into('<Q',raw,count_off+8,9175)
        with self.assertRaises(ValueError):
            kernel.vendor_symbol_access_evidence(raw,self.symbols,self.base,self.values_by_name,protected)
        raw=bytearray(self.raw)
        overlap_add=0x91000000 | (0x8a8 << 10) | (1 << 5) | 1
        struct.pack_into('<I',raw,resolve_off+0x17c,overlap_add)
        with self.assertRaises(ValueError):
            kernel.vendor_symbol_access_evidence(raw,self.symbols,self.base,self.values_by_name,protected)
        raw=bytearray(self.raw)
        import_off=int(protected['array_image_range'][1],16)
        struct.pack_into('<Q',raw,import_off,self.base+len(raw))
        with self.assertRaises(ValueError):
            kernel.vendor_symbol_access_evidence(raw,self.symbols,self.base,self.values_by_name,protected)
        raw=bytearray(self.raw);raw[resolve_off+0x194:resolve_off+0x198]=bytes(4)
        with self.assertRaises(ValueError):
            kernel.vendor_symbol_access_evidence(raw,self.symbols,self.base,self.values_by_name,protected)

class FirmwarePayload(unittest.TestCase):
    def fixture(self,root):
        p=root/'vendor_a_firmware';p.mkdir()
        data=b"stock firmware fixture";(p/'Himax_firmware.bin').write_bytes(data)
        row={'name':'Himax_firmware.bin','path':'vendor_a_firmware/Himax_firmware.bin','source_path':'/firmware/Himax_firmware.bin','bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
        return {'schema':1,'device_access':False,'partition':'vendor_a','total_bytes':len(data),'files':[row]}
    def test_erofs_listing_regular_files_only(self):
        names,excluded=firmware.firmware_names("NID TYPE FILENAME\n1 1 Himax_firmware.bin\n2 7 sensor\n3 1 unexpected.txt\n4 2 nested\n")
        self.assertEqual(names,['Himax_firmware.bin']);self.assertEqual(len(excluded),3)
        for text in ["<E> read failed","NID TYPE FILENAME\n1 1 ../escape.bin\n","NID TYPE FILENAME\n1 1 a.bin\n2 1 a.bin\n"]:
            with self.assertRaises(ValueError):firmware.firmware_names(text)
    def test_validated_files_survive_cpio_roundtrip(self):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);m=self.fixture(root);entries=firmware.entries(m,root)
            members=stock.read_newc(stock.cpio_newc(entries))
            self.assertEqual(members['lib/firmware/Himax_firmware.bin']['data'],b"stock firmware fixture")
            self.assertEqual(len(entries),1)
    def test_hash_size_path_duplicates_and_symlinks_rejected(self):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);base=self.fixture(root)
            for key,value in [('sha256','0'*64),('bytes',1),('path','../outside.bin'),('name','../outside.bin'),('source_path','/userdata/private')]:
                m=json.loads(json.dumps(base));m['files'][0][key]=value
                with self.assertRaises(ValueError):firmware.entries(m,root)
            m=json.loads(json.dumps(base));m['files'].append(m['files'][0])
            with self.assertRaises(ValueError):firmware.entries(m,root)
            p=root/base['files'][0]['path'];p.unlink();p.symlink_to('/dev/null')
            with self.assertRaises(ValueError):firmware.entries(base,root)
    def test_empty_config_is_valid_but_empty_binary_is_not(self):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);m=self.fixture(root);row=m['files'][0]
            for name,valid in [('connfem.cfg',True),('empty.bin',False)]:
                row.update(name=name,path='vendor_a_firmware/'+name,source_path='/firmware/'+name,bytes=0,sha256=hashlib.sha256(b'').hexdigest())
                (root/row['path']).write_bytes(b'');m['total_bytes']=0
                if valid:self.assertEqual(firmware.entries(m,root)[0][2],b'')
                else:
                    with self.assertRaises(ValueError):firmware.entries(m,root)
    def test_produced_firmware_archive_matches_all_original_hashes(self):
        m=json.loads((REPO.parent/'analysis/stock-binaries/firmware-manifest.json').read_text())
        entries=firmware.entries(m,REPO.parent/'analysis/stock-binaries')
        members=stock.read_newc(gzip.decompress((REPO/'Build/t70proPkg/Linux/early-userspace.cpio.gz').read_bytes()))
        self.assertEqual(len(entries),m['file_count'])
        for name,mode,data in entries:
            self.assertEqual(members[name]['mode'],mode);self.assertEqual(members[name]['data'],data)
        self.assertNotIn('lib/firmware/sensor',members)

class LoadPlans(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(dir=REPO/'Build',prefix='early-plan-test-')
        cls.raw={}
        for name,body in {
            'importer':'extern int required_api(void); extern int optional_api(void) __attribute__((weak)); int entry(void) { return required_api()+(optional_api?optional_api():0); }',
            'plain':'int entry(void) { return 0; }',
            'cycle':'extern int cycle_api(void);int entry(void) { return cycle_api(); }',
        }.items():
            path=Path(cls.temp.name)/(name+'.o')
            subprocess.run(['gcc','-c','-x','c','-o',str(path),'-'],input=body.encode(),check=True)
            cls.raw[name]=path.read_bytes()
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def run_plan(self,definitions,kernel=None,root='client',release=None,versions=None,
                 module_metadata=None,stock_config=None):
        inventory=[];members={};metadata={};module_metadata=module_metadata or {}
        for index,(name,kind,imports,exports,depends) in enumerate(definitions):
            # One extra byte makes two provider variants individually identifiable.
            data=self.raw[kind]+bytes([index]);key=f'lib/modules/{index}/{name}.ko'
            inventory.append({'name':name,'path':key,'sha256':hashlib.sha256(data).hexdigest()})
            members[key]={'data':data}
            details=module_metadata.get(name,{})
            export_details=details.get('export_details',{})
            exported_symbol_info={symbol:{'crc':crc,
                'export':export_details.get(symbol,{}).get('export','EXPORT_SYMBOL'),
                'namespace':export_details.get(symbol,{}).get('namespace','')}
                for symbol,crc in exports.items()}
            metadata[data]={'name':name,'license':['GPL v2'],'import_ns':[],
                'modversions':imports,'exported_symbols':exports,
                'exported_symbol_info':exported_symbol_info,'depends':depends,'signed':False,
                'vermagic':(versions or {}).get(index),**details}
        audit={'all_hardware_feature_plans':{'case':{'roots':[root]}},
               'stock_kernel_config':stock_config if stock_config is not None else {'MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS':'n'},
               'module_load_policy':{'policy_ready':False,
                   'unknowns':['signature key/list not verified','KMI whitelist contents not supplied']}}
        return pack.plans(audit,inventory,members,metadata.__getitem__,kernel or {},release)['case']
    def test_actual_elf_strong_and_weak(self):
        self.assertEqual(pack.required_symbols(self.raw['importer']),{'required_api'})
    def test_symbol_and_declared_side_effect_provider_precede_client(self):
        result=self.run_plan([('client','importer',{'required_api':7,'optional_api':8},{},['side']),('api','plain',{}, {'required_api':7},[]),('side','plain',{}, {},[])])
        self.assertFalse(result['errors'])
        names=[m['name'] for m in result['modules']]
        self.assertLess(names.index('api'),names.index('client'));self.assertLess(names.index('side'),names.index('client'))
    def test_kernel_export_needs_no_module(self):
        result=self.run_plan([('client','importer',{'required_api':7},{},[])],{'required_api':{'crc':7}})
        self.assertEqual([m['name'] for m in result['modules']],['client']);self.assertFalse(result['errors'])
    def test_wrong_crc_blocks_plan(self):
        result=self.run_plan([('client','importer',{'required_api':7},{},[]),('api','plain',{}, {'required_api':8},[])])
        self.assertEqual(result['status'],'blocked')
    def test_variant_ambiguity_blocks_plan(self):
        result=self.run_plan([('client','importer',{'required_api':7},{},['rfkill']),('rfkill','plain',{}, {'required_api':7},[]),('rfkill','plain',{}, {'required_api':7},[])])
        self.assertEqual(result['status'],'blocked')
    def test_unique_exact_release_selects_duplicate_but_crc_is_still_required(self):
        definitions=[('client','importer',{'required_api':7},{},['rfkill']),
                     ('rfkill','plain',{}, {'required_api':7},[]),('rfkill','plain',{}, {'required_api':7},[])]
        versions={1:'6.12-exact SMP modversions aarch64',2:'6.12-other SMP modversions aarch64'}
        result=self.run_plan(definitions,release='6.12-exact',versions=versions)
        self.assertEqual(result['status'],'offline order prepared')
        self.assertEqual([m['path'] for m in result['modules']],['lib/modules/1/rfkill.ko','lib/modules/0/client.ko'])
        self.assertEqual(result['variant_selections'][0]['selected_path'],'lib/modules/1/rfkill.ko')
        self.assertEqual(len(result['variant_selections'][0]['variants']),2)
        self.assertFalse(result['variant_selections'][0]['runtime_validated'])
        definitions[1]=('rfkill','plain',{}, {'required_api':8},[])
        self.assertEqual(self.run_plan(definitions,release='6.12-exact',versions=versions)['status'],'blocked')
    def test_no_exact_release_or_two_exact_variants_remain_ambiguous(self):
        definitions=[('client','importer',{'required_api':7},{},['rfkill']),
                     ('rfkill','plain',{}, {'required_api':7},[]),('rfkill','plain',{}, {'required_api':7},[])]
        for versions in ({1:'6.12-other SMP',2:'6.12-other SMP'},
                         {1:'6.12-exact SMP',2:'6.12-exact SMP'},
                         {1:'6.12-exact-extra SMP',2:'6.12-exact-extra SMP'},{}):
            self.assertEqual(self.run_plan(definitions,release='6.12-exact',versions=versions)['status'],'blocked')
    def test_exact_variant_unresolved_import_cannot_be_hidden_by_other_variant(self):
        definitions=[('client','importer',{'required_api':7},{},['rfkill']),
                     ('rfkill','cycle',{'cycle_api':9},{'required_api':7},[]),
                     ('rfkill','plain',{}, {'required_api':7,'cycle_api':9},[])]
        result=self.run_plan(definitions,release='6.12-exact',versions={1:'6.12-exact SMP',2:'other SMP'})
        self.assertEqual(result['status'],'blocked')
        self.assertIn('cycle_api',str(result['errors']))
    def test_gpl_only_provider_checks_duplicate_preserved_license_tags(self):
        definitions=[('client','importer',{'required_api':7},{},[]),
                     ('api','plain',{}, {'required_api':7},[])]
        provider={'api':{'export_details':{'required_api':{'export':'EXPORT_SYMBOL_GPL'}}}}
        ambiguous=self.run_plan(definitions,module_metadata={
            **provider,'client':{'license':['GPL v2','Proprietary']}})
        self.assertEqual(ambiguous['status'],'offline order prepared')
        self.assertIn('exact stock loader selection',str(ambiguous['provider_policy_unknowns']))
        denied=self.run_plan(definitions,module_metadata={
            **provider,'client':{'license':['Proprietary']}})
        self.assertEqual(denied['status'],'blocked')
        self.assertIn('GPL-compatible',str(denied['errors']))
        allowed=self.run_plan(definitions,module_metadata={
            **provider,'client':{'license':['GPL v2','GPL']}})
        self.assertEqual(allowed['status'],'offline order prepared')

    def test_kernel_export_uses_exact_gpl_and_namespace_metadata(self):
        definitions=[('client','importer',{'required_api':7},{},[])]
        kernel={'required_api':{'crc':7,'export':'EXPORT_SYMBOL_GPL','namespace':'DMA_BUF'}}
        denied=self.run_plan(definitions,kernel=kernel,module_metadata={
            'client':{'license':['Proprietary'],'import_ns':[]}},
            stock_config={'MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS':'n'})
        self.assertEqual(denied['status'],'blocked')
        self.assertIn('GPL-compatible',str(denied['errors']))
        self.assertIn('MODULE_IMPORT_NS',str(denied['errors']))
        allowed=self.run_plan(definitions,kernel=kernel,module_metadata={
            'client':{'license':['GPL v2'],'import_ns':['DMA_BUF']}},
            stock_config={'MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS':'n'})
        self.assertEqual(allowed['status'],'offline order prepared')

    def test_missing_namespace_import_blocks_unless_exact_stock_config_allows_it(self):
        definitions=[('client','importer',{'required_api':7},{},[]),
                     ('api','plain',{}, {'required_api':7},[])]
        provider={'api':{'export_details':{'required_api':{'namespace':'DMA_BUF'}}}}
        blocked=self.run_plan(definitions,module_metadata=provider,
                              stock_config={'MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS':'n'})
        self.assertEqual(blocked['status'],'blocked')
        self.assertIn('MODULE_IMPORT_NS',str(blocked['errors']))
        allowed=self.run_plan(definitions,module_metadata=provider,
                              stock_config={'MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS':'y'})
        self.assertEqual(allowed['status'],'offline order prepared')
        self.assertIn('kernel warning',str(allowed['provider_policy_warnings']))
        imported=self.run_plan(definitions,module_metadata={
            **provider,'client':{'import_ns':['DMA_BUF']}},
            stock_config={'MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS':'n'})
        self.assertEqual(imported['status'],'offline order prepared')

    def test_unknown_missing_namespace_config_is_reported_without_blocking(self):
        definitions=[('client','importer',{'required_api':7},{},[]),
                     ('api','plain',{}, {'required_api':7},[])]
        provider={'api':{'export_details':{'required_api':{'namespace':'DMA_BUF'}}}}
        result=self.run_plan(definitions,module_metadata=provider,stock_config={})
        self.assertEqual(result['status'],'offline order prepared')
        self.assertIn('policy is not supplied',str(result['provider_policy_unknowns']))

    def test_reserved_module_namespace_grammar_and_import_restriction(self):
        definitions=[('client','importer',{'required_api':7},{},[]),
                     ('api','plain',{}, {'required_api':7},[])]
        provider={'api':{'export_details':{'required_api':{'namespace':'module:client'}}}}
        self.assertEqual(self.run_plan(definitions,module_metadata=provider)['status'],
                         'offline order prepared')
        wildcard={'api':{'export_details':{'required_api':{'namespace':'module:cli*'}}}}
        self.assertEqual(self.run_plan(definitions,module_metadata=wildcard)['status'],
                         'offline order prepared')
        denied={'api':{'export_details':{'required_api':{'namespace':'module:other'}}}}
        self.assertEqual(self.run_plan(definitions,module_metadata=denied)['status'],'blocked')
        unsupported={'api':{'export_details':{'required_api':{'namespace':'module:client**'}}}}
        self.assertIn('unsupported module:',str(
            self.run_plan(definitions,module_metadata=unsupported)['errors']))
        explicit_import={'client':{'import_ns':['module:client']}}
        self.assertIn('may not request the reserved module:',str(
            self.run_plan(definitions,module_metadata={**provider,**explicit_import})['errors']))

    def test_signature_force_confirms_unsigned_rejection_but_trailer_is_not_verified(self):
        definitions=[('client','plain',{}, {},[])]
        unsigned=self.run_plan(definitions,stock_config={'MODULE_SIG_FORCE':'y'})
        self.assertEqual(unsigned['status'],'blocked')
        self.assertIn('MODULE_SIG_FORCE',str(unsigned['errors']))
        signed=self.run_plan(definitions,module_metadata={'client':{'signed':True}},
                             stock_config={'MODULE_SIG_FORCE':'y'})
        self.assertEqual(signed['status'],'offline order prepared')
        self.assertFalse(signed['load_policy_ready'])
        self.assertTrue(signed['load_policy_unknowns'])

    def test_unresolved_signature_and_kmi_policy_is_reported_without_erasing_orders(self):
        definitions=[('client','plain',{}, {},[])]
        result=self.run_plan(definitions)
        self.assertEqual(result['status'],'offline order prepared')
        self.assertFalse(result['load_policy_ready'])
        self.assertTrue(result['load_policy_unknowns'])

    def test_cycle_blocks_plan(self):
        result=self.run_plan([('client','plain',{}, {},['api']),('api','plain',{}, {},['client'])])
        self.assertEqual(result['status'],'blocked')
    def test_missing_declared_provider_blocks_even_without_symbol(self):
        result=self.run_plan([('client','plain',{}, {},['side'])])
        self.assertEqual(result['status'],'blocked')
    def test_missing_version_for_strong_import_blocks(self):
        result=self.run_plan([('client','importer',{}, {},[])])
        self.assertEqual(result['status'],'blocked')
    def test_stock_alias_requires_actual_crc_edge(self):
        result=self.run_plan([('client','importer',{'required_api':7},{},['mediatek_drm']),('mediatek_drm_v1','plain',{}, {'required_api':7},[])])
        self.assertFalse(result['errors']);self.assertEqual(len(result['resolved_stock_name_aliases']),1)
        result=self.run_plan([('client','plain',{}, {},['mediatek_drm']),('mediatek_drm_v1','plain',{}, {'required_api':7},[])])
        self.assertEqual(result['status'],'blocked')
    def test_payload_hash_change_rejected(self):
        raw=self.raw['plain'];item={'name':'client','path':'x','sha256':'0'*64}
        with self.assertRaisesRegex(ValueError,'hash mismatch'):
            pack.plans({'all_hardware_feature_plans':{}},[item],{'x':{'data':raw}},lambda _:None,{})

class PlatformSuppliers(unittest.TestCase):
    def compile_tree(self,text):
        return platform.tree(subprocess.run(['dtc','-I','dts','-O','dtb','-q'],input=text.encode(),capture_output=True,check=True).stdout)
    def fixture(self,extra='',status='okay'):
        return self.compile_tree('/dts-v1/; / { compatible="board"; soc { compatible="simple-bus"; '
          'clock: clock { compatible="test,clock"; #clock-cells=<2>; }; '
          'pins: pins { compatible="test,pins"; gpio-controller; #gpio-cells=<2>; '
          'state: input-state { pinmux=<1>; }; }; '
          'bus: i2c { compatible="test,i2c"; clocks=<&clock 1 999>; '
          'touch { compatible="test,touch"; status="'+status+'"; pinctrl-0=<&state>; '
          'himax,irq-gpio=<&pins 9 0>; '+extra+' }; }; }; };')
    def test_real_tree_matches_independent_fdtget(self):
        dtb=REPO.parent/'analysis/stock-fdt/effective.dtb';nodes,hs=platform.tree(dtb.read_bytes())
        self.assertEqual(len(nodes),1095);self.assertEqual(len(hs),len(set(hs)))
        for node in ['/soc/pinctrl','/soc/i2c@11e00000','/soc/i2c@11e00000/himax_ts@48']:
            got=subprocess.check_output(['fdtget','-t','s',str(dtb),node,'compatible'],text=True).strip().split()
            self.assertEqual(nodes[node]['compatible'],got)
    def module_fixture(self,prefix=40,driver=True,local=False):
        body=('struct match { char name[32],type[32],compatible[128];const void *data; }; '+
          ('static ' if local else '')+
          'const struct match singular_of_device_id[]={{"","","test,touch",0},{"restricted","","test,restricted",0},{0}}; '+
          'const struct match unrelated_match_table[]={{"","","test,not-a-driver",0},{0}}; ')
        if driver:
            body+=('struct driver_prefix { const char *name; const void *bus,*owner; const char *mod_name; '
              'unsigned char flags; unsigned int probe; const struct match *of; void *rest[16]; }; '
              'struct wrapper { void *callbacks['+str(prefix//8)+']; struct driver_prefix driver; char tail['+str({0:0,32:0,40:24,48:48}[prefix])+']; }; '
              'struct wrapper test_driver = { .driver={.name="fixture",.of=singular_of_device_id} };')
        if driver:
            registration={0:'driver_register',32:'__spi_register_driver',40:'__platform_driver_register',48:'i2c_register_driver'}[prefix]
            body+='extern void '+registration+'(void); void register_fixture(void) { '+registration+'(); }'
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            path=Path(temp)/'module.o'
            subprocess.run(['gcc','-c','-x','c','-o',str(path),'-'],input=body.encode(),check=True)
            return path.read_bytes()
    def rela_record(self,raw):
        start=struct.unpack_from('<Q',raw,40)[0];size,count,_=struct.unpack_from('<HHH',raw,58)
        for i in range(count):
            section=struct.unpack_from('<IIQQQQIIQQ',raw,start+i*size)
            if section[1]==4 and section[5]>=48:return section
        self.fail('Fixture has no RELA section')
    def test_descriptor_relocations_accept_singular_id_and_ignore_unreferenced_tables(self):
        raw=self.module_fixture();matches=platform.of_matches(raw)
        self.assertEqual([m['compatible'] for m in matches],['test,touch'])
        row=matches[0];offset=row['module_file_offset']
        self.assertEqual(raw[offset+64:offset+75],b'test,touch\0')
        self.assertEqual(row['driver_symbol'],'test_driver')
        self.assertEqual(row['driver_name'],'fixture')
        self.assertEqual(row['driver_name_prefix'],40)
        corrupted=bytearray(raw);corrupted[offset+400]=1
        with self.assertRaisesRegex(ValueError,'unterminated driver OF table'):platform.of_matches(corrupted)
        for raw in (b'',b'\x7fELF'+bytes(128)):
            with self.assertRaises(ValueError):platform.of_matches(raw)
    def test_named_but_unreferenced_of_tables_do_not_claim_driver_binding(self):
        self.assertEqual(platform.of_matches(self.module_fixture(driver=False)),[])
    def test_platform_i2c_direct_driver_prefixes_and_section_relative_targets(self):
        for prefix in (0,32,40,48):
            for local in (False,True):
                with self.subTest(prefix=prefix,local=local):
                    rows=platform.of_matches(self.module_fixture(prefix=prefix,local=local))
                    self.assertEqual([r['compatible'] for r in rows],['test,touch'])
                    self.assertEqual(rows[0]['driver_name_prefix'],prefix)
    def test_duplicate_and_invalid_relocation_symbol_are_rejected(self):
        raw=self.module_fixture();sec=self.rela_record(raw)
        bad=bytearray(raw);bad[sec[4]+24:sec[4]+48]=bad[sec[4]:sec[4]+24]
        with self.assertRaisesRegex(ValueError,'duplicate ELF pointer'):platform.of_matches(bad)
        bad=bytearray(raw);struct.pack_into('<Q',bad,sec[4]+8,(0xffffffff<<32)|257)
        with self.assertRaisesRegex(ValueError,'relocation symbol'):platform.of_matches(bad)
    def test_out_of_bounds_pointer_and_driver_flags_are_rejected(self):
        raw=self.module_fixture();sec=self.rela_record(raw)
        bad=bytearray(raw);struct.pack_into('<q',bad,sec[4]+16,1<<60)
        with self.assertRaisesRegex(ValueError,'pointer target'):platform.of_matches(bad)
        row=platform.of_matches(raw)[0];bad=bytearray(raw)
        bad[row['driver_file_offset']+row['driver_name_prefix']+32]=2
        self.assertEqual(platform.of_matches(bad),[])
    def test_original_charger_uses_descriptor_not_a_match_name_heuristic(self):
        manifest=json.loads((REPO.parent/'analysis/stock-binaries/manifest.json').read_text())
        item=next(x for x in manifest['modules'] if x['name']=='sc8989x_charger')
        raw=Path(item['path']).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),item['sha256'])
        rows=platform.of_matches(raw)
        row=next(x for x in rows if x['compatible']=='southchip,sc89890h')
        self.assertEqual(row['driver_symbol'],'sc8989x_charger_driver')
        self.assertEqual(row['driver_name_prefix'],48)
    def test_rejected_original_table_is_reported_without_inventing_a_match(self):
        raw=bytearray(self.module_fixture());row=platform.of_matches(raw)[0]
        raw[row['module_file_offset']+400]=1
        issues=[]
        self.assertEqual(platform.of_matches(raw,issues),[])
        self.assertEqual(len(issues),1)
        self.assertIn('unterminated driver OF table',issues[0]['status'])
        raw[18]=0
        with self.assertRaises(ValueError):platform.of_matches(raw,issues)
    def test_shared_dlkm_reader_closes_anonymous_file_on_failure(self):
        import io
        inspector=load('shared_stock_inspector',PKG/'inspect-stock-modules.py')
        before=set(Path('/proc/self/fd').iterdir())
        stream=io.BytesIO(bytes(4096))
        partition={'bytes':4096,'extents':[{'bytes':4096,'type':0,'super_offset':0}]}
        with self.assertRaisesRegex(ValueError,'Unsupported module filesystem'):
            inspector.inspect_dlkm(stream,{'offset_bytes':0},partition,'system_dlkm_a',{'groups':{}})
        self.assertEqual(set(Path('/proc/self/fd').iterdir()),before)
        self.assertEqual(stream.getvalue(),bytes(4096))
        with self.assertRaisesRegex(ValueError,'bounded'):
            inspector.inspect_dlkm(stream,{}, {'bytes':256*1024*1024+1},'system_dlkm_a',{'groups':{}})
    def test_specifier_arguments_are_not_interpreted_as_handles(self):
        nodes,hs=self.fixture();edges,errors=platform.graph(nodes,hs)
        self.assertFalse(any(errors.values()))
        self.assertEqual([(e['property'],e['target']) for e in edges['/soc/i2c'] if e['property']=='clocks'],[('clocks','/soc/clock')])
        pin=[e for e in edges['/soc/i2c/touch'] if e['property']=='pinctrl-0'][0]
        self.assertEqual(pin['target'],'/soc/pins/input-state');self.assertEqual(pin['device'],'/soc/pins')
        gpio=[e for e in edges['/soc/i2c/touch'] if e['property']=='himax,irq-gpio'][0]
        self.assertEqual(gpio['device'],'/soc/pins')
    def test_invalid_supplier_is_reported_and_zero_placeholder_skipped(self):
        nodes,hs=self.fixture('clocks=<0 &clock 1>;')
        _,errors=platform.graph(nodes,hs)
        self.assertIn('truncated supplier specifier',str(errors['/soc/i2c/touch']))
        nodes,hs=self.fixture('clocks=<0 &clock 1 2>;')
        self.assertFalse(any(platform.graph(nodes,hs)[1].values()))
        nodes,hs=self.fixture('power-domains=<0x777>;')
        self.assertIn('unknown supplier phandle',str(platform.graph(nodes,hs)[1]))
    def test_disabled_ancestor_excludes_devices_and_disabled_supplier_rejected(self):
        nodes,hs=self.fixture(status='disabled')
        modules={'touch':{'matches':[{'compatible':'test,touch'}]}}
        plan=platform.feature_plans(nodes,hs,modules,{'input':['touch']})['input']
        self.assertEqual(plan['seed_nodes'],[])
        nodes['/soc/i2c/touch']['enabled']=True;nodes['/soc/pins']['enabled']=False
        self.assertIn('disabled supplier',str(platform.graph(nodes,hs)[1]))
    def test_legacy_fixed_regulator_gpio_requires_its_provider(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { '
          'pins: pins { compatible="test,pins"; gpio-controller; #gpio-cells=<2>; }; '
          'rail { compatible="regulator-fixed"; gpio=<&pins 2 0>; }; };')
        modules={'rail':{'matches':[{'compatible':'regulator-fixed'}]},
                 'pins':{'matches':[{'compatible':'test,pins'}]}}
        plan=platform.feature_plans(nodes,hs,modules,{'camera':['rail']})['camera']
        self.assertEqual(plan['roots'],['pins','rail'])
        nodes['/pins']['enabled']=False
        self.assertIn('disabled supplier',str(platform.graph(nodes,hs)[1]['/rail']))
        nodes['/pins']['enabled']=True
        nodes['/rail']['properties']['gpio']=struct.pack('>II',next(iter(hs)),2)
        self.assertIn('truncated supplier specifier',str(platform.graph(nodes,hs)[1]['/rail']))

    def test_gpio_property_is_not_inferred_for_unrelated_bindings(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { '
          'other { compatible="test,opaque"; gpio="arbitrary label"; }; };')
        edges,errors=platform.graph(nodes,hs)
        self.assertFalse(any(errors.values()))
        self.assertEqual(edges['/other'],[])

    def test_stock_camera_regulators_report_disabled_gpio_expander(self):
        nodes,hs=platform.tree((REPO.parent/'analysis/stock-fdt/effective.dtb').read_bytes())
        _,errors=platform.graph(nodes,hs)
        for index in (1,2,3):
            self.assertIn({'property':'gpio','error':'disabled supplier'},
                          errors['/soc/odm/rt5133-gpio%d'%index])

    def test_typed_supplier_arguments_are_not_phandles(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { '
          'provider: provider { compatible="test,provider"; #thermal-sensor-cells=<1>; '
          '#cooling-cells=<2>; #performance-domain-cells=<1>; }; '
          'client { compatible="test,client"; thermal-sensors=<&provider 1>; '
          'cooling-device=<&provider 0xffffffff 0xffffffff>; gpio-ranges=<&provider 1 2 188>; '
          'performance-domains=<&provider 1>; }; };')
        edges,errors=platform.graph(nodes,hs)
        self.assertFalse(any(errors.values()))
        self.assertEqual([(e['property'],e['target']) for e in edges['/client']],
                         [(k,'/provider') for k in ('thermal-sensors','cooling-device','gpio-ranges','performance-domains')])
        for prop in ('thermal-sensors','cooling-device','gpio-ranges','performance-domains'):
            raw=nodes['/client']['properties'][prop]
            nodes['/client']['properties'][prop]=raw[:-4]
            self.assertIn('truncated supplier specifier',str(platform.graph(nodes,hs)[1]['/client']))
            nodes['/client']['properties'][prop]=raw
        nodes['/provider']['enabled']=False
        self.assertEqual(len(platform.graph(nodes,hs)[1]['/client']),4)

    def test_nvmem_cells_support_zero_or_explicit_arguments(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { '
          'efuse { compatible="test,efuse"; '
          'cell0: cell0 { reg=<0 4>; }; '
          'cell1: cell1 { reg=<4 4>; #nvmem-cell-cells=<1>; }; }; '
          'client { compatible="test,client"; nvmem-cells=<&cell0 &cell1 999>; }; };')
        edges,errors=platform.graph(nodes,hs)
        self.assertFalse(any(errors.values()))
        self.assertEqual([e['target'] for e in edges['/client']],['/efuse/cell0','/efuse/cell1'])
        self.assertTrue(all(e['device']=='/efuse' for e in edges['/client']))
        nodes['/client']['properties']['nvmem-cells']=nodes['/client']['properties']['nvmem-cells'][:-4]
        self.assertIn('truncated supplier specifier',str(platform.graph(nodes,hs)[1]['/client']))

    def test_missing_reference_lists_are_validated_and_dangling_targets_rejected(self):
        for prop in ('cpu-idle-states','cpus','required-opps','memory-region','nvmem','shmem','cpu','trip','operating-points-v2'):
            with self.subTest(prop=prop):
                nodes,hs=self.compile_tree('/dts-v1/; / { '
                  'provider: provider { compatible="test,provider"; }; '
                  'client { compatible="test,client"; '+prop+'=<&provider>; }; };')
                edges,errors=platform.graph(nodes,hs)
                self.assertFalse(any(errors.values()))
                self.assertEqual([e['target'] for e in edges['/client']],['/provider'])
                nodes['/client']['properties'][prop]=struct.pack('>I',0x12345678)
                self.assertIn('unknown supplier phandle',str(platform.graph(nodes,hs)[1]['/client']))

    def test_board_charger_usb_and_gpio_spellings_require_matching_consumer(self):
        cases=[('charger','mediatek,battery manager',0),('pericfg','mediatek,mt6789-usb20',0),
          ('infracg','mediatek,mt6789-usb20',0),('husb311pd,intr_gpio','hynetek,husb311',2),
          ('sc,intr-gpio','southchip,sc89890h',2),('sc,sc8989x,en-gpio','southchip,sc89890h',2),
          ('sc,sc8989x,otg-gpio','southchip,sc89890h',2),('focaltech,irq-gpio','focaltech,fts-spi',2),
          ('focaltech,reset-gpio','focaltech,fts-spi',2),
          ('headset-mic-sw-gpio','mediatek,mt6358-accdet',2),
          ('headset-lr-en-gpio','mediatek,mt6358-accdet',2)]
        for prop,consumer,args in cases:
            with self.subTest(prop=prop):
                nodes,hs=self.compile_tree('/dts-v1/; / { '
                  'provider: provider { compatible="test,provider"; #gpio-cells=<2>; }; '
                  'client { compatible="'+consumer+'"; '+prop+'=<&provider '+('1 0' if args else '')+'>; }; };')
                edges,errors=platform.graph(nodes,hs)
                self.assertFalse(any(errors.values()))
                self.assertEqual([e['target'] for e in edges['/client']],['/provider'])
                nodes['/client']['compatible']=['test,unrelated']
                nodes['/client']['properties'][prop]=struct.pack('>I',999)
                self.assertFalse(any(platform.graph(nodes,hs)[1].values()))
                self.assertEqual(platform.graph(nodes,hs)[0]['/client'],[])

    def test_reference_named_symbols_are_metadata(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { __symbols__ { '
          'cpu="/cpus/cpu@0"; nvmem="/soc/efuse"; thermal-sensors="/thermal"; }; };')
        edges,errors=platform.graph(nodes,hs)
        self.assertFalse(any(errors.values()))
        self.assertEqual(edges['/__symbols__'],[])

    def test_opp_table_arrays_include_each_domain_and_reject_bad_suppliers(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { '
          'a: table-a { compatible="operating-points-v2"; }; '
          'b: table-b { compatible="operating-points-v2"; }; '
          'client { compatible="test,multidomain"; operating-points-v2=<&a &b>; }; };')
        edges,errors=platform.graph(nodes,hs)
        self.assertFalse(any(errors.values()))
        self.assertEqual([e['target'] for e in edges['/client']],['/table-a','/table-b'])
        nodes['/table-b']['enabled']=False
        self.assertIn('disabled supplier',str(platform.graph(nodes,hs)[1]['/client']))
        nodes['/table-b']['enabled']=True
        original=nodes['/client']['properties']['operating-points-v2']
        nodes['/client']['properties']['operating-points-v2']=original[:4]+struct.pack('>I',0x12345678)
        self.assertIn('unknown supplier phandle',str(platform.graph(nodes,hs)[1]['/client']))
        nodes['/client']['properties']['operating-points-v2']=original+b'\x01'
        self.assertIn('non-cell DT property length',str(platform.graph(nodes,hs)[1]['/client']))


    def test_stock_previously_omitted_reference_counts(self):
        nodes,hs=platform.tree((REPO.parent/'analysis/stock-fdt/effective.dtb').read_bytes())
        edges,errors=platform.graph(nodes,hs)
        counts={}
        for row in edges.values():
            for e in row:counts[e['property']]=counts.get(e['property'],0)+1
        for prop,count in {'thermal-sensors':24,'cooling-device':1,'nvmem-cells':8,'nvmem':5,
          'required-opps':15,'memory-region':1,'cpu-idle-states':56,'cpu':8,'cpus':8,'performance-domains':8,
          'operating-points-v2':12,'trip':1,'charger':4,'pericfg':1,'infracg':1,'gpio-ranges':1,'shmem':4}.items():
            with self.subTest(prop=prop):self.assertEqual(counts.get(prop,0),count)
        self.assertFalse(errors['/soc/usb0@11200000'])
        self.assertFalse(errors['/soc/i2c@1101a000/husb311@4e'])

    def test_supplier_and_parent_modules_are_included(self):
        nodes,hs=self.fixture();modules={name:{'matches':[{'compatible':'test,'+name}]} for name in ['touch','pins','i2c','clock']}
        plan=platform.feature_plans(nodes,hs,modules,{'input':['touch']})['input']
        self.assertFalse(plan['parse_errors']);self.assertFalse(plan['unresolved_nodes'])
        self.assertEqual(set(plan['roots']),set(modules))
        self.assertLess(plan['roots'].index('clock'),plan['roots'].index('i2c'))
        self.assertLess(plan['roots'].index('i2c'),plan['roots'].index('touch'))
        self.assertLess(plan['roots'].index('pins'),plan['roots'].index('touch'))
    def test_multiple_matches_are_retained_and_device_cycles_bounded(self):
        nodes,hs=self.fixture('remote-endpoint=<&bus>;')
        modules={name:{'matches':[{'compatible':'test,'+kind}]} for name,kind in [('touch','touch'),('bus1','i2c'),('bus2','i2c')]}
        nodes['/soc/i2c']['properties']['remote-endpoint']=nodes['/soc/i2c']['properties']['phandle']
        p=platform.feature_plans(nodes,hs,modules,{'input':['touch']})['input']
        self.assertTrue(p['multiple_match_nodes']);self.assertTrue(p['device_cycles'])
        self.assertTrue({'bus1','bus2'}.issubset(p['roots']))
    def test_module_dependency_DT_supplier_closure_reaches_fixed_point(self):
        nodes,hs=self.fixture()
        modules={name:{'matches':[{'compatible':'test,'+name}]} for name in ['touch','pins','i2c','clock']}
        # Root has no OF table: its declared side effect provider starts the DT chain.
        modules['entry']={'matches':[]}
        plan=platform.complete_feature_plans(nodes,hs,modules,{'input':{'entry'}},{'entry':['touch']})['input']
        self.assertEqual(set(plan['roots']),set(modules))
        self.assertEqual(plan['requested_module_roots'],['entry'])
        self.assertGreater(plan['dependency_closure_iterations'],1)
        self.assertFalse(plan['unresolved_nodes'])
    def test_board_smi_vectors_require_correct_consumer_and_target_types(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { '
          'a: larb { compatible="mediatek,mt6789-smi-larb"; }; '
          'b: larb2 { compatible="mediatek,mt6789-smi-larb"; }; '
          'dbg { compatible="mediatek,mtk-smi-dbg"; mediatek-larb-supply=<&a &b>; }; };')
        edges,errors=platform.graph(nodes,hs)
        self.assertFalse(any(errors.values()))
        self.assertEqual([e['device'] for e in edges['/dbg']],['/larb','/larb2'])
        nodes['/larb2']['compatible']=['not-a-larb']
        self.assertIn('unexpected board reference target',str(platform.graph(nodes,hs)[1]))
    def test_mfd_supplier_requires_exact_parent_and_platform_alias_without_claiming_of_binding(self):
        nodes,hs=self.compile_tree('/dts-v1/; / { '
          'pmic { compatible="mediatek,mt6366"; regulator { compatible="mediatek,mt6358-regulator"; }; }; };')
        modules={'parent':{'matches':[{'compatible':'mediatek,mt6366'}]},
          'mt6358_regulator':{'matches':[],'platform_aliases':['platform:mt6358-regulator']}}
        # Use the regulator as a visited parent supply from a separate consumer.
        nodes['/consumer']={'parent':'/','enabled':True,'compatible':['test,consumer'],'properties':{}}
        nodes['/consumer']['properties']['vdd-supply']=struct.pack('>I',1);hs[1]='/pmic/regulator'
        modules['consumer']={'matches':[{'compatible':'test,consumer'}]}
        p=platform.feature_plans(nodes,hs,modules,{'case':['consumer']})['case']
        self.assertIn('mt6358_regulator',p['roots'])
        self.assertEqual(p['unresolved_nodes'][0]['named_bus_preload_modules'],['mt6358_regulator'])
        self.assertNotIn('/pmic/regulator',p['inspected_device_nodes'])
        modules['mt6358_regulator']['platform_aliases']=[]
        self.assertNotIn('mt6358_regulator',platform.feature_plans(nodes,hs,modules,{'case':['consumer']})['case']['roots'])
    def test_malformed_tree_ranges_structure_and_duplicate_handles(self):
        raw=subprocess.run(['dtc','-I','dts','-O','dtb','-q'],input=b'/dts-v1/; / { one { phandle=<1>; }; two { phandle=<2>; }; };',capture_output=True,check=True).stdout
        damaged=[]
        for offset,value in [(0,0),(4,len(raw)+1),(8,len(raw)),(36,len(raw)),(20,16)]:
            b=bytearray(raw);struct.pack_into('>I',b,offset,value);damaged.append(b)
        damaged.extend([raw[:5],raw[:-1],raw.replace(b'\0\0\0\2',b'\0\0\0\1')])
        for b in damaged:
            with self.assertRaises((ValueError,UnicodeError)):platform.tree(b)

class MagneticLid(unittest.TestCase):
    def test_legacy_hall_is_not_a_native_lid_root(self):
        self.assertNotIn('hall_sensor_p',platform.BOARD_ROOTS['input'])
        for feature in ('input','sensors'):self.assertIn('hall_sensor_p',platform.BOARD_EXCLUSIONS[feature])
        self.assertIn('mtk_pmic_keys',platform.BOARD_ROOTS['input'])
    def test_native_lid_binding_is_required_and_checked(self):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp)
            plan={'errors':[],'modules':[],'platform_suppliers':{'native_lid_node':'/teclast-cover-lid'}}
            _,script=pack.module_scripts({'input':plan},'test-release',str(root))
            shell=['/bin/sh'] if sys.platform=='darwin' else ['/usr/bin/busybox','sh']
            subprocess.run(shell+['-n'],input=script,check=True)
            def run():return subprocess.run(shell+['-c',script.decode(),'check','input'],capture_output=True,text=True)
            self.assertEqual(run().returncode,2)
            dt=root/'firmware/devicetree/base/teclast-cover-lid';dt.mkdir(parents=True)
            device=root/'bus/platform/devices/teclast-cover-lid';device.mkdir(parents=True)
            (device/'of_node').symlink_to(dt)
            r=run();self.assertEqual(r.returncode,2,r.stderr);self.assertIn('unbound-device',r.stdout)
            driver=root/'bus/platform/drivers/gpio-keys';driver.mkdir(parents=True)
            (device/'driver').symlink_to(driver)
            r=run();self.assertEqual(r.returncode,0,r.stderr)
            self.assertIn('node=/teclast-cover-lid state=bound',r.stdout)
            self.assertIn('hardware-operation-tested=false',r.stdout)

class BindingStatus(unittest.TestCase):
    def execute(self,root,bindings,unresolved=None,overlap=None,feature='input'):
        plan={'errors':[],'modules':[],'platform_suppliers':{'inspected_device_nodes':bindings,'unresolved_nodes':unresolved or [],'multiple_match_nodes':overlap or []}}
        _,script=pack.module_scripts({'input':plan},'test-release',str(root))
        subprocess.run(['/usr/bin/busybox','sh','-n'],input=script,check=True)
        return subprocess.run(['/usr/bin/busybox','sh','-c',script.decode(),'check',feature],capture_output=True,text=True)
    def make_node(self,root,node='/soc/touch',bound=True):
        dt=root/('firmware/devicetree/base'+node);dt.mkdir(parents=True,exist_ok=True)
        device=root/'bus/i2c/devices/1-0048';device.mkdir(parents=True,exist_ok=True)
        (device/'of_node').symlink_to(dt)
        if bound:
            driver=root/'bus/i2c/drivers/himax';driver.mkdir(parents=True,exist_ok=True);(device/'driver').symlink_to(driver)
    def test_actual_sysfs_link_traversal_and_success_message(self):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);self.make_node(root)
            r=self.execute(root,['/soc/touch']);self.assertEqual(r.returncode,0,r.stderr)
            self.assertIn('state=bound',r.stdout);self.assertIn('hardware-operation-tested=false',r.stdout)
    def test_unbound_and_missing_devices_fail_even_if_module_loaded(self):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);self.make_node(root,bound=False);(root/'module/hxchipset_i2c').mkdir(parents=True)
            self.assertEqual(self.execute(root,['/soc/touch']).returncode,2)
            self.assertIn('unbound-device',self.execute(root,['/soc/touch']).stdout)
            self.assertIn('missing-dt-node',self.execute(root,['/soc/absent']).stdout)
            (root/'firmware/devicetree/base/soc/pins').mkdir()
            self.assertIn('no-enumerated-device',self.execute(root,['/soc/pins']).stdout)
    def test_no_nodes_unknown_feature_and_unresolved_provider_cannot_report_ready(self):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);self.make_node(root)
            self.assertEqual(self.execute(root,[]).returncode,2)
            self.assertEqual(self.execute(root,['/soc/touch'],feature='other').returncode,64)
            self.assertEqual(self.execute(root,['/soc/touch'],unresolved=[{'node':'/soc/provider'}]).returncode,2)
            self.assertEqual(self.execute(root,['/soc/touch'],overlap=[{'node':'/soc/touch'}]).returncode,2)
    def test_unsafe_generated_identities_are_rejected(self):
        for path in ['/soc/../x','/soc/$(touch BAD)','/soc/a\nexit 0']:
            with self.assertRaises(ValueError):pack.module_scripts({'input':{'errors':[],'modules':[],'platform_suppliers':{'inspected_device_nodes':[path]}}},'test')
        with self.assertRaises(ValueError):pack.module_scripts({'input;bad':{'errors':[],'modules':[]}},'test')

class ExecutableModuleLoader(unittest.TestCase):
    def run_loader(self,mode,feature='input',errors=None,policy_ready=True):
        plan={'errors':errors or [],'load_policy_ready':policy_ready,'modules':[{'name':'teclast_fixture_supplier','path':'lib/modules/fixture/supplier.ko'},
          {'name':'teclast_fixture_consumer','path':'lib/modules/fixture/consumer.ko'}]}
        loader,_=pack.module_scripts({'input':plan},'test-exact-release')
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);log=root/'calls';commands=root/'bin';commands.mkdir()
            for name,text in {
              'uname':'echo uname >> "$CALLS"; if [ "$MODE" = release ]; then echo different; else echo test-exact-release; fi',
              'sha256sum':'echo hash >> "$CALLS"; [ "$MODE" != hash ]',
              'insmod':'echo "insmod:$1" >> "$CALLS"; [ "$MODE" != insmod ]',
              'status':'echo "status:$1" >> "$CALLS"; [ "$MODE" != unbound ] || exit 2',
            }.items():
                p=commands/name;p.write_text('#!/bin/sh\n'+text+'\n');p.chmod(0o755)
            # BusyBox standalone applets can bypass PATH. Explicit shell
            # functions intercept every uname/hash/insmod before the production
            # script, so this test cannot reach a real module-loading syscall.
            prefix=''.join(name+'() { '+str(commands/name)+' "$@"; };\n' for name in ['uname','sha256sum','insmod'])
            script=prefix.encode()+loader.replace(b'/sbin/teclast-devices',str(commands/'status').encode())
            env={**os.environ,'PATH':str(commands)+':/usr/bin:/bin','CALLS':str(log),'MODE':mode}
            result=subprocess.run(['/usr/bin/busybox','sh','-c',script.decode(),'loader',feature],env=env,capture_output=True,text=True)
            return result,log.read_text().splitlines()
    def test_wrong_kernel_or_hash_never_reaches_insmod(self):
        for mode in ['release','hash']:
            r,calls=self.run_loader(mode);self.assertNotEqual(r.returncode,0);self.assertFalse(any(c.startswith('insmod:') for c in calls))
    def test_real_generated_shell_orders_suppliers_and_propagates_failures(self):
        r,calls=self.run_loader('okay');self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(calls,['uname','hash','insmod:/lib/modules/fixture/supplier.ko','insmod:/lib/modules/fixture/consumer.ko','status:input'])
        r,calls=self.run_loader('insmod');self.assertNotEqual(r.returncode,0);self.assertEqual(len([c for c in calls if c.startswith('insmod:')]),1)
        self.assertFalse(any(c.startswith('status:') for c in calls))
        r,calls=self.run_loader('unbound');self.assertEqual(r.returncode,2);self.assertEqual(calls[-1],'status:input')
    def test_unverified_signature_or_symbol_policy_never_reaches_insmod(self):
        for readiness in (False,None):
            r,calls=self.run_loader('okay',policy_ready=readiness)
            self.assertEqual(r.returncode,1)
            self.assertEqual(calls,['uname','hash'])
            self.assertIn('policy unresolved',r.stderr)

    def test_blocked_and_unknown_features_never_load(self):
        r,calls=self.run_loader('okay',errors=[{'root':'missing'}]);self.assertEqual(r.returncode,1);self.assertEqual(calls,['uname','hash'])
        r,calls=self.run_loader('okay',feature='unknown');self.assertEqual(r.returncode,64);self.assertEqual(calls,['uname','hash'])

class StartupWorker(unittest.TestCase):
    def execute(self,selection,mode='okay',existing=False):
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            root=Path(temp);run=root/'run';log=root/'calls';loader=root/'loader';timer=root/'timer';mdev=root/'mdev'
            loader.write_text('#!/bin/sh\nprintf "load:%s\\n" "$1" >> '+str(log)+'\n'
                'case "'+mode+'" in partial) [ "$1" != input ] || exit 2;; '
                'failed) exit 1;; hang) /bin/sleep 5;; esac\necho fixture-loaded\n')
            mdev.write_text('#!/bin/sh\n[ "$1" = -s ] || exit 1\necho mdev >> '+str(log)+'\n'+('exit 1\n' if mode=='nodes' else 'exit 0\n'))
            # Check production timeout arguments, then run real BusyBox timeout
            # with a short duration and inert fixtures; never call insmod/mdev.
            timer.write_text('#!/bin/sh\n[ "$1" = -s ] && [ "$2" = TERM ] && [ "$3" = -k ] && [ "$4" = 2 ] || exit 99\n'
                '[ "$5" = 30 ] || [ "$5" = 10 ] || exit 99\nshift 5\nexec /usr/bin/busybox timeout -s TERM -k 0.1 0.1 "$@"\n')
            for p in [loader,timer,mdev]:p.chmod(0o755)
            if existing:run.mkdir();(run/'state').write_text('previous-run\n')
            script=pack.startup_script(['input','display'],str(run),str(loader),str(timer),str(mdev))
            subprocess.run(['/usr/bin/busybox','sh','-n'],input=script,check=True)
            result=subprocess.run(['/usr/bin/busybox','sh','-c',script.decode(),'start',selection],capture_output=True,text=True,timeout=5)
            files={p.name:p.read_text() for p in run.glob('*') if p.is_file()} if run.exists() else {}
            return result,log.read_text().splitlines() if log.exists() else [],files
    def test_none_never_loads_or_scans(self):
        r,calls,files=self.execute('none');self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(calls,[]);self.assertEqual(files['state'],'disabled\n')
    def test_real_shell_runs_groups_in_selection_order_then_device_scan(self):
        r,calls,files=self.execute('input,display');self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(calls,['load:input','load:display','mdev'])
        self.assertEqual(files['state'],'bindings-observed\n')
        self.assertIn('input\t0\tbindings-observed\n',files['results.tsv'])
        self.assertIn('fixture-loaded',files['input.log'])
    def test_partial_binding_does_not_report_success_or_repeat_module_load(self):
        r,calls,files=self.execute('input,display','partial');self.assertEqual(r.returncode,2)
        self.assertEqual(calls,['load:input','load:display','mdev'])
        self.assertEqual(files['state'],'bindings-incomplete\n')
        self.assertIn('input\t2\tbindings-incomplete',files['results.tsv'])
    def test_load_failure_stops_before_following_group_or_scan(self):
        r,calls,files=self.execute('input,display','failed');self.assertEqual(r.returncode,1)
        self.assertEqual(calls,['load:input']);self.assertEqual(files['state'],'failed\n')
    def test_actual_timeout_stops_before_following_group(self):
        r,calls,files=self.execute('input,display','hang');self.assertNotIn(r.returncode,[0,2,99])
        self.assertEqual(calls,['load:input']);self.assertEqual(files['state'],'failed\n')
    def test_node_scan_failure_is_distinct_from_module_binding(self):
        r,calls,files=self.execute('input','nodes');self.assertEqual(r.returncode,1)
        self.assertEqual(calls,['load:input','mdev']);self.assertEqual(files['state'],'device-node-scan-failed\n')
    def test_all_names_validated_before_any_load_and_no_shell_evaluation(self):
        for value in ['',',input','input,','input,,display','input,input','none,input','input,none','input,unknown','../input','input;echo_bad','$(echo bad)','input *','a'*257]:
            r,calls,files=self.execute(value);self.assertEqual(r.returncode,64,value)
            self.assertEqual(calls,[],value);self.assertFalse(files,value)
    def test_existing_run_is_preserved_and_not_started_twice(self):
        r,calls,files=self.execute('input',existing=True);self.assertEqual(r.returncode,73)
        self.assertEqual(calls,[]);self.assertEqual(files['state'],'previous-run\n')
    def test_actual_c_cmdline_parser_bounds_and_duplicate_argument_rejection(self):
        source='#define main TeclastPid1Main\n#include "'+str(PKG/'Linux/Init.c')+'"\n#undef main\n#include <assert.h>\n'
        source+=r'''
int main(void) {
  char Out[257],Long[CMDLINE_LIMIT+1];
  const char *Valid[]={"","quiet","quiet teclast.modules=input,display loglevel=7","teclast.modules=none\n","xteclast.modules=bad"};
  const char *Expected[]={"none","none","input,display","none","none"};
  for (unsigned I=0;I<5;I++) { assert(!SelectStartup(Valid[I],strlen(Valid[I]),Out,sizeof(Out)));assert(!strcmp(Out,Expected[I])); }
  const char *Bad[]={"teclast.modules=","teclast.modules=input teclast.modules=none","teclast.modules=none\tteclast.modules=none"};
  for (unsigned I=0;I<3;I++) assert(SelectStartup(Bad[I],strlen(Bad[I]),Out,sizeof(Out))==-1);
  const char *ConsoleGood[]={"","quiet","teclast.console=none","teclast.console=drm loglevel=7","xteclast.console=bad","teclast.console=desktop-vulkan","teclast.console=desktop-pixman"};
  const char *ConsoleExpected[]={"none","none","none","drm","none","desktop-vulkan","desktop-pixman"};
  for (unsigned I=0;I<7;I++) {
    assert(!SelectConsole(ConsoleGood[I],strlen(ConsoleGood[I]),Out,sizeof(Out)));
    assert(!strcmp(Out,ConsoleExpected[I]));
  }
  const char *ConsoleBad[]={"teclast.console=","teclast.console=drm teclast.console=none",
    "teclast.console=drm,none","teclast.console=$(bad)","teclast.console=DRM"};
  for (unsigned I=0;I<5;I++) assert(SelectConsole(ConsoleBad[I],strlen(ConsoleBad[I]),Out,sizeof(Out))==-1);
  assert(SelectStartup("a\0b",3,Out,sizeof(Out))==-1);
  assert(SelectStartup("",0,Out,4)==-1);
  memset(Long,'a',sizeof(Long));assert(SelectStartup(Long,sizeof(Long),Out,sizeof(Out))==-1);
  memcpy(Long,"teclast.modules=",16);
  for (unsigned N=1;N<=300;N++) assert(SelectStartup(Long,16+N,Out,sizeof(Out))==(N<=256?0:-1));
  return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=REPO/'Build') as temp:
            exe=Path(temp)/'ParserTest'
            subprocess.run(['gcc','-x','c','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-o',str(exe),'-'],input=source.encode(),check=True)
            subprocess.run([str(exe)],check=True)

class ConnectivityArchive(unittest.TestCase):
    def test_actual_dhcp_archive_members_payload_hashes_and_independent_listing(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        for item in report['artifacts']:
            path=pack.host_path(item['path'])
            self.assertEqual(path.stat().st_size,item['bytes'])
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),item['sha256'])
        raw=gzip.decompress(pack.host_path(report['artifacts'][1]['path']).read_bytes())
        members=stock.read_newc(raw)
        listing=subprocess.run(['cpio','-it'],input=raw,capture_output=True,check=True).stdout.decode().splitlines()
        self.assertEqual(set(listing),set(members))
        self.assertEqual(members['sbin/teclast-connectivity']['data'],(PKG/'Linux/connectivity-services.sh').read_bytes())
        self.assertEqual(members['sbin/teclast-connectivity']['mode'],0o100755)
        for app in ['udhcpc','rm','rmdir','chmod','cmp','ip','mv','readlink','cat','grep','sleep']:
            self.assertEqual(members['bin/'+app]['mode'],0o120777)
            self.assertEqual(members['bin/'+app]['data'],b'busybox')
        self.assertEqual(members['etc/resolv.conf']['mode'],0o120777)
        self.assertEqual(members['etc/resolv.conf']['data'],b'/run/teclast-connectivity/resolv.conf')
        for member,digest in [('init',report['init_sha256']),('sbin/teclast-graphics',report['graphics']['sha256']),('bin/busybox',report['busybox']['sha256'])]:
            data=members[member]['data']
            self.assertEqual(hashlib.sha256(data).hexdigest(),digest)
            self.assertEqual(data[:6],b'\x7fELF\x02\x01')
            self.assertEqual(struct.unpack_from('<H',data,18)[0],183)
        self.assertNotIn(b'teclast-connectivity',members['sbin/teclast-start']['data'])
        self.assertFalse(report['connectivity_services']['runtime_service_or_radio_execution'])
        self.assertNotIn('IP configuration service',report['connectivity_services']['not_bundled'])
        self.assertEqual((PKG/'Applications/LinuxBoot/BootPayload.h').read_text(),pack.payload_header(report['artifacts']))

class ConsoleRepack(unittest.TestCase):
    def test_cross_console_runtime_and_changed_proof_rejection(self):
        helper=load('console_cross_packaging',PKG/'Linux/console.py')
        if sys.platform!='darwin':self.skipTest('macOS cross-build packaging path')
        root=REPO.parent.parents[2]
        entries,report=helper.prepare_cross(root)
        self.assertEqual(len(report['runtime']),9)
        self.assertEqual(len({name for name,_,_ in entries}),len(entries))
        self.assertFalse(report['native_linux_tests'])
        self.assertEqual(report['software_tests']['status'],'pending')
        proof=json.loads((REPO.parent/'analysis/linux-console-build.json').read_text())
        with tempfile.TemporaryDirectory(prefix='console-proof-',dir=REPO/'Build') as directory:
            target=Path(directory);(target/'analysis').mkdir()
            for change in ['options','source','checkout','configuration','binary','library']:
                changed=json.loads(json.dumps(proof));cross=changed['cross_build']
                if change=='options':cross['options']=[]
                if change=='source':cross['source_sha256']['kmscon/src/uterm_monitor.c']='0'*64
                if change=='checkout':cross['pinned_checkout_verified']=False
                if change=='configuration':
                    cross['configuration'][next(iter(cross['configuration']))]='0'*64
                if change=='binary':
                    next(x for x in cross['artifacts'] if x['path'].endswith('/src/kmscon'))['sha256']='0'*64
                if change=='library':cross['offline_dynamic_link_check']['runtime_files'][-1]['sha256']='0'*64
                (target/'analysis/linux-console-build.json').write_text(json.dumps(changed))
                with self.subTest(change=change),patch.object(helper,'PROJECT',target):
                    with self.assertRaises(ValueError):helper.prepare_cross(root)

    def publish_fixture(self,root,bad_directory=False):
        pkg=root/'pkg';(pkg/'Linux').mkdir(parents=True)
        (pkg/'Linux/connectivity-services.sh').write_text('# fixture helper\n')
        for name in ['WmtControl.c','WmtReady.h','BtHciBridge.c','LteInspect.c']:
            (pkg/'Linux'/name).write_text('/* fixture source */\n')
        (pkg/'Applications/LinuxBoot').mkdir(parents=True)
        header=pkg/'Applications/LinuxBoot/BootPayload.h';header.write_text('previous header')
        initrd=root/'early.cpio.gz';seed=stock.read_newc(stock.cpio_newc([
            ('init',0o100755,b'old init'),('etc/retained',0o100600,b'original bytes')]))
        if bad_directory:seed['etc']['mode']=0o040700
        previous=gzip.compress(b'previous fixture',mtime=0);initrd.write_bytes(previous)
        report={'artifacts':[{'path':str(root/'Image'),'bytes':1,'sha256':'1'*64},
            {'path':str(initrd),'bytes':len(previous),'sha256':hashlib.sha256(previous).hexdigest()},
            {'path':str(root/'modules'),'bytes':1,'sha256':'2'*64}], 'source_sha256':{}}
        path=root/'report.json';seed_report=json.dumps(report).encode();path.write_bytes(seed_report)
        replacements={'init':{'mode':0o100755,'data':b'new init'},
            'usr/libexec/console':{'mode':0o100755,'data':b'new console'}}
        with patch.object(pack,'PKG',pkg):
            if bad_directory:
                with self.assertRaisesRegex(ValueError,'readback mismatch'):
                    pack.publish_repack(path,seed_report,report,stock,seed,replacements,{'test':True},[])
                self.assertEqual(initrd.read_bytes(),previous)
                self.assertEqual(path.read_bytes(),seed_report)
                self.assertEqual(header.read_text(),'previous header')
                self.assertFalse(initrd.with_suffix('.gz.pending').exists())
                return
            pack.publish_repack(path,seed_report,report,stock,seed,replacements,{'test':True},[])
        result=stock.read_newc(gzip.decompress(initrd.read_bytes()))
        self.assertEqual(result['etc/retained'],seed['etc/retained'])
        self.assertEqual(result['init'],replacements['init'])
        self.assertEqual(result['usr/libexec/console'],replacements['usr/libexec/console'])
        self.assertEqual(result['usr/libexec']['mode'],0o040755)
        written=json.loads(path.read_text())
        self.assertEqual(written['artifacts'][1]['sha256'],hashlib.sha256(initrd.read_bytes()).hexdigest())
        self.assertEqual(written['repack_provenance']['seed_archive_sha256'],hashlib.sha256(previous).hexdigest())
        self.assertEqual(written['repack_provenance']['changed_members'],['init','usr/libexec/console'])
        self.assertEqual(header.read_text(),pack.payload_header(written['artifacts']))
        self.assertFalse(initrd.with_suffix('.gz.pending').exists())

    def test_real_cpio_publication_preserves_existing_members_and_adds_directories(self):
        with tempfile.TemporaryDirectory(prefix='console-repack-',dir=REPO/'Build') as directory:
            self.publish_fixture(Path(directory))

    def test_bad_readback_cannot_replace_any_published_file(self):
        with tempfile.TemporaryDirectory(prefix='console-repack-',dir=REPO/'Build') as directory:
            self.publish_fixture(Path(directory),True)

class ConsoleArchive(unittest.TestCase):
    def test_arm64_console_elf_header_and_interpreter_rejections(self):
        helper=load('console_elf_header',PKG/'Linux/console.py')
        def image(interpreters):
            data=bytearray(64+56*len(interpreters))
            data[:7]=b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HHI',data,16,3,183,1)
            struct.pack_into('<Q',data,32,64)
            struct.pack_into('<HHH',data,52,64,56,len(interpreters))
            for index,text in enumerate(interpreters):
                offset=len(data);data+=text
                struct.pack_into('<IIQQQQQQ',data,64+56*index,3,4,offset,0,0,len(text),len(text),1)
            return bytes(data)
        valid=image([b'/lib/ld-linux-aarch64.so.1\0'])
        self.assertEqual(helper.arm64_elf(valid),'/lib/ld-linux-aarch64.so.1')
        self.assertIsNone(helper.arm64_elf((REPO/'Build/t70proPkg/Linux/init').read_bytes()))
        bad=[b'',valid[:63]]
        for offset,value in [(0,b'BAD!'),(6,b'\2'),(16,b'\0\0'),(18,b'>\0'),
                (20,b'\0'*4),(52,b'\0\0'),(54,b'\0\0'),(56,b'\0\0'),(32,struct.pack('<Q',2**64-1))]:
            raw=bytearray(valid);raw[offset:offset+len(value)]=value;bad.append(bytes(raw))
        for value in [b'/tmp/loader\0',b'/lib/ld-linux-aarch64.so.1',b'/lib/\0bad\0',
                b'/'+b'x'*256+b'\0',b'/lib/\xff\0']:
            bad.append(image([value]))
        bad.append(image([b'/lib/ld-linux-aarch64.so.1\0']*2))
        raw=bytearray(valid);struct.pack_into('<Q',raw,64+8,2**64-1);bad.append(bytes(raw))
        for raw in bad:
            with self.assertRaises(ValueError):helper.arm64_elf(raw)

    def test_console_runtime_files_and_source_provenance(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        console=report['console']
        helper=load('console_archive',PKG/'Linux/console.py')
        members=stock.read_newc(gzip.decompress(pack.host_path(report['artifacts'][1]['path']).read_bytes()))
        self.assertEqual(console['commit'],helper.COMMIT)
        self.assertFalse(console['gpu_acceleration']);self.assertFalse(console['device_execution'])
        if console.get('native_linux_tests') is True:
            self.assertEqual(console['software_tests']['status'],'passed')
        else:
            self.assertFalse(console['native_linux_tests'])
            self.assertEqual(console['software_tests']['status'],'pending')
            self.assertFalse(console['software_tests']['native_linux_execution'])
        for path,digest in console['source_sha256'].items():
            self.assertEqual(hashlib.sha256((REPO.parent/path).read_bytes()).hexdigest(),digest,path)
        for item in console['package_files']:
            value=members[item['path']]
            self.assertEqual(value['mode'],int(item['mode'],8),item['path'])
            self.assertEqual(len(value['data']),item['bytes'],item['path'])
            self.assertEqual(hashlib.sha256(value['data']).hexdigest(),item['sha256'],item['path'])
        self.assertEqual(members['etc/kmscon/kmscon.conf']['data'],helper.CONFIG)
        self.assertEqual(members['sbin/teclast-shell']['data'],(PKG/'Linux/console-shell.sh').read_bytes())
        shell=['/bin/sh'] if sys.platform=='darwin' else ['/usr/bin/busybox','sh']
        subprocess.run(shell+['-n'],input=members['sbin/teclast-shell']['data'],check=True)
        for item in console['runtime']:
            helper.arm64_elf(members[item['path']]['data'])

    def test_archived_dynamic_console_starts_without_host_libraries_or_devices(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        members=stock.read_newc(gzip.decompress(pack.host_path(report['artifacts'][1]['path']).read_bytes()))
        with tempfile.TemporaryDirectory(prefix='console-loader-',dir=REPO/'Build') as directory:
            root=Path(directory)
            for item in report['console']['package_files']:
                name=item['path'];self.assertNotIn('..',Path(name).parts)
                self.assertFalse(Path(name).is_absolute())
                value=members[name];self.assertEqual(value['mode']&0o170000,0o100000)
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True)
                path.write_bytes(value['data']);path.chmod(value['mode']&0o777)
            # --version exits during option parsing; this root has no /dev or /sys.
            r=subprocess.run(['sudo','-n','chroot',str(root),'/usr/libexec/kmscon/kmscon','--version'],
                capture_output=True,text=True,timeout=10)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn('9.3.2',r.stdout+r.stderr)
            self.assertFalse((root/'dev').exists());self.assertFalse((root/'sys').exists())

class DesktopArchive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        cls.raw=gzip.decompress(pack.host_path(cls.report['artifacts'][1]['path']).read_bytes())
        cls.members=stock.read_newc(cls.raw)

    @classmethod
    def tearDownClass(cls):
        cls.raw=None;cls.members=None

    def test_desktop_archive_readback_and_every_member_identity(self):
        listing=subprocess.run(['cpio','-it'],input=self.raw,capture_output=True,check=True).stdout.decode().splitlines()
        self.assertEqual(set(listing),set(self.members))
        desktop=self.report['desktop'];self.assertFalse(desktop['device_execution'])
        self.assertFalse(desktop['Linux_runtime_executed']);self.assertFalse(desktop['Hangul_input_method_integrated'])
        for item in desktop['package_files']:
            value=self.members[item['path']]
            self.assertEqual(value['mode'],int(item['mode'],8),item['path'])
            self.assertEqual(len(value['data']),item['bytes'],item['path'])
            self.assertEqual(hashlib.sha256(value['data']).hexdigest(),item['sha256'],item['path'])
        for item in self.report['artifacts']:
            data=pack.host_path(item['path']).read_bytes()
            self.assertEqual((len(data),hashlib.sha256(data).hexdigest()),(item['bytes'],item['sha256']))
        self.assertEqual((PKG/'Applications/LinuxBoot/BootPayload.h').read_text(),pack.payload_header(self.report['artifacts']))
        self.assertEqual(self.members['sbin/teclast-desktop']['data'],(PKG/'Linux/desktop-start.sh').read_bytes())
        subprocess.run(['/bin/sh','-n'],input=self.members['sbin/teclast-desktop']['data'],check=True)
        self.assertEqual(hashlib.sha256(self.members['init']['data']).hexdigest(),self.report['init_sha256'])
        self.assertEqual(hashlib.sha256((PKG/'Linux/Init.c').read_bytes()).hexdigest(),self.report['init_source_sha256'])

    def test_direct_elf_dependencies_resolve_entirely_inside_actual_archive(self):
        helper=load('desktop_archive_elf',PKG/'Linux/console.py')
        def dynamic(data):
            helper.arm64_elf(data)
            start=struct.unpack_from('<Q',data,32)[0];size,count=struct.unpack_from('<HH',data,54)
            headers=[struct.unpack_from('<IIQQQQQQ',data,start+i*size) for i in range(count)]
            section=next((h for h in headers if h[0]==2),None)
            if section is None:return [],[]
            tags=[]
            for offset in range(section[2],section[2]+section[5],16):
                tag,value=struct.unpack_from('<QQ',data,offset)
                if tag==0:break
                tags.append((tag,value))
            else:raise AssertionError('Unterminated actual ELF dynamic section')
            address=next(value for tag,value in tags if tag==5)
            length=next(value for tag,value in tags if tag==10)
            load=next(h for h in headers if h[0]==1 and h[3]<=address and address+length<=h[3]+h[5])
            strings=data[load[2]+address-load[3]:load[2]+address-load[3]+length]
            def text(offset):
                assert offset<len(strings)
                end=strings.index(0,offset)
                return strings[offset:end].decode('ascii')
            return [text(v) for t,v in tags if t==1],[text(v) for t,v in tags if t in [15,29]]
        count=0
        for name,value in self.members.items():
            data=value['data']
            if value['mode']&0o170000!=0o100000 or data[:4]!=b'\x7fELF':continue
            interpreter=helper.arm64_elf(data)
            if interpreter:self.assertIn(interpreter[1:],self.members,name)
            needed,rpaths=dynamic(data)
            directories=[]
            for rpath in rpaths:
                for directory in rpath.split(':'):
                    directory=directory.replace('${ORIGIN}','/'+str(Path(name).parent)).replace('$ORIGIN','/'+str(Path(name).parent))
                    self.assertTrue(directory.startswith('/'),name)
                    directories.append(directory.lstrip('/'))
            directories+=['lib/aarch64-linux-gnu','usr/lib/aarch64-linux-gnu']
            for library in needed:
                self.assertTrue(any(d+'/'+library in self.members for d in directories),(name,library))
            count+=1
        self.assertGreaterEqual(count,self.report['desktop']['runtime_ELF_count'])

    def test_loader_input_cursor_font_and_udev_data_are_bundled(self):
        members=self.members
        icd=json.loads(members['usr/share/vulkan/icd.d/teclast-panvk.json']['data'])
        self.assertIn(icd['ICD']['library_path'][1:],members)
        self.assertEqual(icd['ICD']['library_arch'],'64')
        self.assertTrue(icd['ICD']['api_version'].startswith('1.4.'))
        for name in ['usr/lib/aarch64-linux-gnu/gbm/dri_gbm.so','usr/lib/udev/hwdb.bin',
                     'usr/share/X11/xkb/symbols/kr','usr/share/X11/xkb/rules/evdev',
                     'usr/share/icons/DMZ-White/cursors/left_ptr','usr/share/weston/terminal.png',
                     'lib/aarch64-linux-gnu/libasound.so.2','usr/share/alsa/alsa.conf',
                     'usr/share/alsa/cards/aliases.conf','usr/share/alsa/ctl/default.conf']:
            self.assertIn(name,members)
        for name,value in members.items():
            if not name.startswith('usr/share/icons/DMZ-White/') or value['mode']&0o170000!=0o120000:continue
            target=value['data'].decode();self.assertNotIn('/',target)
            self.assertIn(str(Path(name).parent/target),members)
        font=members['usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc']['data']
        self.assertEqual(font[:4],b'ttcf')
        self.assertTrue(any(n.startswith('usr/share/libinput/') and n.endswith('.quirks') for n in members))
        rules=members['usr/lib/udev/rules.d/40-teclast-devnodes.rules']['data']
        self.assertIn(b'PROGRAM=="/bin/mdev"',rules)
        self.assertIn(b'IMPORT{builtin}="input_id"',members['usr/lib/udev/rules.d/60-input-id.rules']['data'])
        self.assertNotIn('usr/lib/udev/rules.d/80-drivers.rules',members)

    def test_stock_kernel_module_and_console_payloads_remain_preserved(self):
        self.assertEqual(self.report['artifacts'][0]['sha256'],'5a8be048cddedd45da4d8a9ce415d3cbf89046d4ac549dfd8d398dd6359daa9d')
        audit=json.loads((REPO.parent/'analysis/linux-stock-module-audit.json').read_text())
        artifact=self.report['artifacts'][2]
        raw=pack.host_path(artifact['path']).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),audit['package_sha256'])
        self.assertEqual(artifact['sha256'],audit['package_sha256'])
        modules=stock.read_newc(raw)
        inventory=json.loads(next(v['data'] for n,v in modules.items()
            if n.endswith('/stock-binary-inventory.json')))
        for item in inventory['modules']:
            member=modules[item['path']]
            self.assertEqual(hashlib.sha256(member['data']).hexdigest(),item['sha256'])
            # LinuxBoot passes the module CPIO separately. No earlier archive may
            # shadow one of its files with a different payload or permissions.
            if item['path'] in self.members:
                self.assertEqual(self.members[item['path']],member,item['path'])
        for plan in self.report['module_plans'].values():
            for item in plan['modules']:
                self.assertIn(item['path'],modules)
        provenance=self.report.get('repack_provenance')
        if provenance is not None:
            allowed={i['path'] for i in self.report['desktop']['package_files']}|{'init'}
            if 'radio_userspace' in provenance:
                allowed|=set(provenance['radio_userspace']['members'])|{
                    'sbin/teclast-wmt-control','sbin/teclast-bt-hci','sbin/teclast-connectivity',
                    'sbin/teclast-modules','sbin/teclast-devices','sbin/teclast-start',
                    'etc/teclast/module-plan.json','etc/teclast/modules.sha256'}
            if 'LTE_inventory_tests' in provenance:
                allowed.add('sbin/teclast-lte-inspect')
            self.assertTrue(set(provenance['changed_members'])<=allowed)
            self.assertTrue(provenance['other_members_preserved_byte_for_byte'])
        else:
            self.assertEqual(self.report['status'], 'built; not booted')
            self.assertEqual(len(self.members), self.report['member_count'])
        lte=(provenance or {}).get('LTE_inventory_tests', self.report.get('LTE_inventory_tests'))
        if lte is not None:
            self.assertEqual(lte['status'],'passed')
            self.assertFalse(lte['device_execution'])
            self.assertFalse(lte['modem_control_implemented'])
            self.assertEqual(hashlib.sha256(self.members['sbin/teclast-lte-inspect']['data']).hexdigest(),lte['artifact']['sha256'])
        for item in self.report['console']['package_files']:
            self.assertEqual(hashlib.sha256(self.members[item['path']]['data']).hexdigest(),item['sha256'])
        for path,digest in self.report['source_sha256'].items():
            self.assertEqual(hashlib.sha256((REPO/path).read_bytes()).hexdigest(),digest,path)

class PreparedArchives(unittest.TestCase):
    def test_actual_archive_identities_and_shell_syntax(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        for item in report['artifacts']:
            path=Path(item['path']);self.assertEqual(path.stat().st_size,item['bytes'])
            with path.open('rb') as stream:self.assertEqual(hashlib.file_digest(stream,'sha256').hexdigest(),item['sha256'])
        raw=gzip.decompress(pack.host_path(report['artifacts'][1]['path']).read_bytes())
        members=stock.read_newc(raw)
        listing=subprocess.run(['cpio','-it','--quiet'],input=raw,capture_output=True,check=True).stdout.decode().splitlines()
        self.assertEqual(set(listing),set(members))
        self.assertEqual(members['bin/sh']['mode'],0o120777)
        self.assertEqual(members['bin/sh']['data'],b'busybox')
        subprocess.run(['/usr/bin/busybox','sh','-n'],input=members['sbin/teclast-modules']['data'],check=True)
        self.assertNotIn(b'None',members['sbin/teclast-modules']['data'])
        subprocess.run(['/usr/bin/busybox','sh','-n'],input=members['sbin/teclast-devices']['data'],check=True)
        subprocess.run(['/usr/bin/busybox','sh','-n'],input=members['sbin/teclast-start']['data'],check=True)
        self.assertEqual(members['sbin/teclast-start']['data'],pack.startup_script(report['module_plans']))
        helper=PKG/'Linux/connectivity-services.sh'
        self.assertEqual(members['sbin/teclast-connectivity']['data'],helper.read_bytes())
        self.assertEqual(members['sbin/teclast-connectivity']['mode'],0o100755)
        self.assertEqual(report['connectivity_services']['helper_sha256'],hashlib.sha256(helper.read_bytes()).hexdigest())
        self.assertFalse(report['connectivity_services']['runtime_service_or_radio_execution'])
        subprocess.run(['/usr/bin/busybox','sh','-n'],input=members['sbin/teclast-connectivity']['data'],check=True)
        self.assertNotIn(b'teclast-connectivity',members['sbin/teclast-start']['data'])
        loader,status=pack.module_scripts(report['module_plans'],report['exact_kernel_release'])
        self.assertEqual(members['sbin/teclast-modules']['data'],loader)
        self.assertEqual(members['sbin/teclast-devices']['data'],status)
        self.assertIn(report['exact_kernel_release'].encode(),members['sbin/teclast-modules']['data'])
    def test_real_wireless_plan_uses_only_exact_image_rfkill(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        plan=report['module_plans']['connectivity']
        self.assertFalse(plan['errors'])
        selected=plan['variant_selections']
        self.assertEqual(len(selected),1)
        self.assertEqual(selected[0]['name'],'rfkill')
        self.assertEqual(selected[0]['selected_sha256'],'fcd7fff4f61ec7de2164972283cafcd9d76d02d9a06542a5a401f6ec80820d76')
        self.assertEqual(selected[0]['kernel_release'],report['exact_kernel_release'])
        self.assertEqual(sum(m['name']=='rfkill' for m in plan['modules']),1)
        module_paths={m['path'] for m in plan['modules']}
        self.assertIn(selected[0]['selected_path'],module_paths)
        self.assertTrue(all(v['path'] not in module_paths for v in selected[0]['variants'] if v['path']!=selected[0]['selected_path']))
        members=stock.read_newc(gzip.decompress(pack.host_path(report['artifacts'][1]['path']).read_bytes()))
        commands=members['sbin/teclast-modules']['data'].decode().split('connectivity)\n',1)[1].split(';;',1)[0]
        self.assertIn('insmod /'+selected[0]['selected_path'],commands)
        self.assertNotIn('unresolved',commands)
    def test_real_input_includes_platform_suppliers_and_blocks_false_binding_success(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        plan=report['module_plans']['input'];names=[m['name'] for m in plan['modules']]
        self.assertFalse(plan['errors'])
        self.assertTrue(all(x['compatible'] in [['fixed-clock'],['mediatek,mt6358-regulator']] for x in plan['platform_suppliers']['unresolved_nodes']))
        for provider in ['pinctrl_mt6789','i2c_mt65xx','clk_mt6789','clk_mt6789_bus','clk_mt6789_i2c']:
            self.assertIn(provider,names);self.assertLess(names.index(provider),names.index('hxchipset_i2c'))
        self.assertIn('/soc/i2c@11e00000/himax_ts@48',plan['platform_suppliers']['inspected_device_nodes'])
        raw=pack.host_path(report['artifacts'][2]['path']).read_bytes();members=stock.read_newc(raw)
        inventory=json.loads(next(v['data'] for k,v in members.items() if k.endswith('stock-binary-inventory.json')))
        manifest=json.loads((REPO.parent/'analysis/stock-binaries/manifest.json').read_text())['modules']
        self.assertEqual(inventory['binary_count'],len(inventory['modules']))
        self.assertTrue({m['sha256'] for m in inventory['modules']}.issubset(m['sha256'] for m in manifest))
        self.assertTrue(all(m['path'] in members for m in plan['modules']))
    def test_unique_crc_strong_imports_extend_dependency_graph(self):
        binaries={'partition:consumer':{'name':'consumer','strong_imports':{'builtin':1,'needed':2,'ambiguous':3,'wrong':4,'unversioned':None}}}
        catalog={'supplier':{'a':{'exported_symbol_info':{'needed':{'crc':2}}}},
          'first':{'a':{'exported_symbol_info':{'ambiguous':{'crc':3},'wrong':{'crc':5}}}},
          'second':{'a':{'exported_symbol_info':{'ambiguous':{'crc':3}}}}}
        graph,edges,issues=platform.symbol_dependency_graph(binaries,catalog,{'builtin':{'crc':1}})
        self.assertEqual(graph,{'consumer':['supplier']})
        self.assertEqual([x['symbol'] for x in edges],['needed'])
        self.assertEqual({x['symbol'] for x in issues},{'ambiguous','wrong','unversioned'})
    def test_complete_original_catalog_and_board_frontends_are_connected(self):
        audit=json.loads((REPO.parent/'analysis/linux-platform-modules.json').read_text())
        self.assertEqual(audit['catalog_coverage']['expected_binary_count'],429)
        self.assertEqual(audit['catalog_coverage']['inspected_binary_count'],429)
        self.assertEqual(audit['catalog_coverage']['missing'],[])
        nodes,_=platform.tree((REPO.parent/'analysis/stock-fdt/effective.dtb').read_bytes())
        expected={p for p,n in nodes.items() if n['enabled'] and n['compatible']}
        self.assertEqual({r['node'] for r in audit['enabled_device_coverage']},expected)
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        groups={'input':['mtk_kpd','mtk_pmic_keys','regulator_vibrator','mt6358_regulator'],
          'audio':['mt6789_mt6366','mtk_btcvsd'],
          'camera':['flashlight','flashlights_cp2155','mtk_composite'],
          'usb':['phy_mtk_tphy','rt_pd_manager','mtk_pd_adapter','mtk_chg_type_det'],
          'power':['pmic_temp','soc_temp_lvts','board_temp','mtk_cpu_power_throttling','mtk_gpu_power_throttling','mtk_md_power_throttling'],'display':['fhctl','mcupm']}
        for feature,required in groups.items():
            plan=report['module_plans'][feature]
            self.assertFalse(plan['errors'],feature)
            self.assertTrue(set(required).issubset(m['name'] for m in plan['modules']),feature)
        for feature in ('input','sensors'):
            plan=report['module_plans'][feature]
            self.assertNotIn('hall_sensor_p',[m['name'] for m in plan['modules']])
            self.assertEqual(plan['platform_suppliers']['native_lid_node'],'/teclast-cover-lid')
        self.assertNotIn('imgsensor_isp6s',[m['name'] for m in report['module_plans']['sensors']['modules']])
        aliases=report['module_plans']['display']['resolved_stock_name_aliases']
        self.assertTrue(any(x['declared']=='mcupm_v3' and x['provider']=='mcupm' for x in aliases))
    def test_exact_cpio_signatures_are_reverified_and_excluded_variant_stays_excluded(self):
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        audit=json.loads((REPO.parent/'analysis/linux-stock-module-audit.json').read_text())
        self.assertFalse(audit['module_load_policy']['policy_ready'])
        self.assertTrue(audit['module_load_policy']['shared_policy_ready'])
        untrusted=audit['module_load_policy']['module_signature_unknowns']
        self.assertEqual([(row['module'],row['source']) for row in untrusted],[('rfkill','vendor_dlkm_a')])
        catalog=json.loads((REPO.parent/'analysis/stock-kernel-exports.json').read_text())
        raw=pack.host_path(report['artifacts'][2]['path']).read_bytes();members=stock.read_newc(raw)
        inventory=json.loads(next(v['data'] for k,v in members.items() if k.endswith('stock-binary-inventory.json')))['modules']
        suppliers=json.loads((REPO.parent/'analysis/linux-platform-modules.json').read_text())['features']
        certificates=stock.read_trusted_public_certificates(catalog,pack.host_path(report['artifacts'][0]['path']),catalog['image_sha256'])
        plans=pack.plans(audit,inventory,members,stock.elf_module,catalog['exports'],
                         report['exact_kernel_release'],suppliers,policy=stock,certificates=certificates)
        for feature,plan in plans.items():
            self.assertFalse(plan['errors'],feature)
            self.assertTrue(plan['load_policy_ready'],(feature,plan['provider_policy_unknowns']))
            self.assertFalse(plan['runtime_validated'])
            self.assertTrue(all(row['trusted_for_offline_policy'] for row in plan['module_signatures']))
        without=pack.plans(audit,inventory,members,stock.elf_module,catalog['exports'],
                           report['exact_kernel_release'],suppliers,policy=stock)
        self.assertFalse(without['connectivity']['load_policy_ready'])
        self.assertTrue(any('signature trust' in row.get('unknown','')
                            for row in without['connectivity']['provider_policy_unknowns']))

    def test_changed_cpio_signature_cannot_inherit_an_audit_trust_result(self):
        import copy
        report=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
        audit=json.loads((REPO.parent/'analysis/linux-stock-module-audit.json').read_text())
        catalog=json.loads((REPO.parent/'analysis/stock-kernel-exports.json').read_text())
        members=stock.read_newc(pack.host_path(report['artifacts'][2]['path']).read_bytes())
        inventory=json.loads(next(v['data'] for k,v in members.items() if k.endswith('stock-binary-inventory.json')))['modules']
        inventory=copy.deepcopy(inventory)
        selected=report['module_plans']['connectivity']['variant_selections'][0]['selected_path']
        row=next(item for item in inventory if item['path']==selected)
        changed=bytearray(members[selected]['data'])
        changed[-len(b"~Module signature appended~\\n")-13]^=1
        members[selected]={**members[selected],'data':bytes(changed)}
        row['sha256']=hashlib.sha256(changed).hexdigest()
        certificates=stock.read_trusted_public_certificates(catalog,pack.host_path(report['artifacts'][0]['path']),catalog['image_sha256'])
        suppliers=json.loads((REPO.parent/'analysis/linux-platform-modules.json').read_text())['features']
        plans=pack.plans(audit,inventory,members,stock.elf_module,catalog['exports'],
                         report['exact_kernel_release'],suppliers,policy=stock,certificates=certificates)
        self.assertFalse(plans['connectivity']['load_policy_ready'])
        result=next(row for row in plans['connectivity']['module_signatures'] if row['module']=='rfkill')
        self.assertFalse(result['trusted_for_offline_policy'])
        self.assertEqual(result['status'],'not-verified')

    def test_kernel_release_comes_from_unambiguous_image_banner(self):
        self.assertEqual(pack.exact_kernel_release(b'Linux version 6.12-test host \0Linux version 6.12-test host '),'6.12-test')
        with self.assertRaises(ValueError):pack.exact_kernel_release(b'Linux version 6.12-a host Linux version 6.12-b host ')
        with self.assertRaises(ValueError):pack.exact_kernel_release(b'not a kernel')

if __name__=='__main__':
    program=unittest.main(verbosity=2,exit=False)
    result=program.result
    report={'status':'passed' if result.wasSuccessful() else 'failed','test_count':result.testsRun,
            'scope':'Actual ELF import extraction, declared/symbol provider selection, hash/CRC/variant/cycle rejection, typed DT suppliers, exact ELF OF tables, executable sysfs binding checks, asynchronous startup shell with inert module/node fixtures, actual timeout and C selector ASan/UBSan, exact built-in public certificate PKCS7 checks on CPIO content and changed-signature rejection, generated archive hashes, independent GNU cpio listing and shell syntax; no real module loading or tablet boot',
            'source_sha256':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [PKG/'Linux/prepare-stock-modules.py',Path(__file__),PKG/'Linux/build-early-userspace.py',PKG/'Linux/platform-modules.py',PKG/'inspect-stock-modules.py',PKG/'Linux/prepare-stock-firmware.py',PKG/'inspect-stock-kernel.py',PKG/'Linux/connectivity-services.sh',PKG/'Linux/Graphics.c',PKG/'Linux/Tests/GraphicsTest.c',PKG/'Linux/Init.c',PKG/'Linux/console.py',PKG/'Linux/console-shell.sh',PKG/'Linux/Tests/ConsoleTest.c',PKG/'Linux/Tests/ConsoleMonitorTest.c']}}
    if len(sys.argv)>1:
        report['test_selection']=sys.argv[1:]
        report['scope']='Selected tests listed in test_selection; actual test code and generated shell against inert fixtures; no full suite, module loading or tablet boot'
    (REPO.parent/'analysis/linux-early-package-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    sys.exit(0 if result.wasSuccessful() else 1)
