#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Cross-compile the selected Windows ARM64 driver. Does not sign or install."""
from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile
from build_output import output_directory
root=Path(__file__).resolve().parent.parent
if "--help" in sys.argv[1:] or "-h" in sys.argv[1:]:
    print(
        "usage: Windows/build.py [TARGET] [--test] [--output-dir PATH]\n"
        "Targets: --sd, --sd-power-test, --pmic-prm-test, --cpu, --thermal, --buttons, --battery,\n"
        "         --usb, --gpio, --himax, --ufs, --ufs-power, --display, --wwan-driver, --wwan-ring-test (CCCI/CCIF and DPMAIF cores)\n"
        "Default target: I2C. --output-dir selects an artifact directory."
    )
    raise SystemExit(0)
try:
    output=output_directory(sys.argv[1:], root)
except ValueError as error:
    raise SystemExit(str(error))
if '--locked' not in sys.argv:
    os.execv(root/'Resources/Scripts/with-build-lock',[
        str(root/'Resources/Scripts/with-build-lock'),'T70Pro','Compile Windows ARM64 '+('T70 NDIS WWAN miniport' if '--wwan-driver' in sys.argv else 'CCCI/CCIF, DPMAIF and WDM DMA cores with host checks' if '--wwan-ring-test' in sys.argv else 'PMIC PRM host client test' if '--pmic-prm-test' in sys.argv else 'SD power client' if '--sd-power-test' in sys.argv else 'MSDC SD host' if '--sd' in sys.argv else 'MediaTek CPU performance domains' if '--cpu' in sys.argv else 'MediaTek thermal sensors' if '--thermal' in sys.argv else 'Windows battery and gauge miniport' if '--battery' in sys.argv else 'physical power/volume button provider' if '--buttons' in sys.argv else 'MUSB UCX USB2 host controller' if '--usb' in sys.argv else 'GpioClx controller' if '--gpio' in sys.argv else 'Himax VHF touch driver' if '--himax' in sys.argv else 'UFS PRM power filter' if '--ufs-power' in sys.argv else 'UFS Storport miniport' if '--ufs' in sys.argv else 'display-only driver' if '--display' in sys.argv else 'SpbCx driver'),'--',
        sys.executable,__file__,'--locked',*sys.argv[1:]])
os.chdir(root)
w=root/'Windows'; t=w/'Toolchain'; b=output; b.mkdir(parents=True, exist_ok=True)
Darwin=sys.platform=='darwin'
Python=Path(sys.executable) if Darwin else root.parent/'.venv/bin/python'
Zig=os.environ.get('T70_WINDOWS_ZIG')
wdk=t/'microsoft.windows.wdk.arm64/c'; sdk=t/'microsoft.windows.sdk.cpp/c'
usb='--usb' in sys.argv
wwan_driver='--wwan-driver' in sys.argv
thermal='--thermal' in sys.argv
cpu='--cpu' in sys.argv
buttons='--buttons' in sys.argv
battery='--battery' in sys.argv
crt=next((t/'msvc-crt-headers').rglob('excpt.h')).parent
includes=[wdk/'Include/10.0.26100.0/km',wdk/'Include/10.0.26100.0/shared',sdk/'Include/10.0.26100.0/shared',sdk/'Include/10.0.26100.0/ucrt',
          sdk/'Include/10.0.26100.0/um',crt,wdk/'Include/wdf/kmdf/1.33',wdk/'Include/10.0.26100.0/km/spb/1.1']
if usb: includes.append(wdk/'Include/10.0.26100.0/km/ucx/1.6')
# Do not rename/copy Microsoft's includes on a case-sensitive Linux host.
roots=[]
for folder in includes:
    roots.append({'type':'directory','name':str(folder),'contents':[
        {'type':'file','name':str(p.relative_to(folder)),'external-contents':str(p)}
        for p in folder.rglob('*') if p.is_file()]})
overlay=b/'wdk-overlay.json'; overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':roots}))
flags=['clang-cl','--target=arm64-pc-windows-msvc','/nologo','/c','/kernel','/O2','/GS','/Zl','/W3',
       '/D_ARM64_','/DWIN64','/DWINVER=0x0A00','/D_WIN32_WINNT=0x0A00',
       # The WDK uses KMDF_VERSION_MINOR to set the minimum runtime version by default.
       '/DKMDF_VERSION_MAJOR=1','/DKMDF_VERSION_MINOR=33',
       # WDK relies on MSVC accepting invalid ## token pastes and intrinsic annotations.
       '/clang:-Wno-invalid-token-paste','/clang:-Wno-nonportable-include-path','/clang:-Wno-ignored-attributes',
       '/clang:-ivfsoverlay','/clang:'+str(overlay)]
flags += ['/imsvc'+str(p) for p in includes]
if wwan_driver: flags += ['/DNDIS_MINIPORT_DRIVER=1','/DNDIS630_MINIPORT=1','/DNTDDI_VERSION=0x0A000010']
flags += ['/I'+str(root/'Silicon/MediaTek/MediaTekPkg/Include'),'/clang:-Werror=incompatible-pointer-types']
himax='--himax' in sys.argv
gpio='--gpio' in sys.argv
ufs='--ufs' in sys.argv
ufs_power='--ufs-power' in sys.argv
display='--display' in sys.argv
sd='--sd' in sys.argv
wwan_ring_test='--wwan-ring-test' in sys.argv
i2c=not any((battery,buttons,display,sd,ufs_power,ufs,gpio,himax,cpu,thermal,usb,wwan_ring_test,wwan_driver))
if sum((buttons,battery,usb,gpio,himax,ufs,ufs_power,display,thermal,cpu,sd,wwan_ring_test,wwan_driver))>1: raise SystemExit('Choose one target')
framework_driver=not (display or ufs or ufs_power or sd or wwan_driver)
wdm_image=True
driver_entry='DriverEntry' if wwan_driver else 'FxDriverEntry' if framework_driver else 'GsDriverEntry'
if wwan_ring_test:
    sources=[w/'MtkWwan/CcciRing.c',w/'MtkWwan/CcciNet.c',w/'MtkWwan/DpmaifDescriptor.c']
    driver_name='CcciRing'
    inf_source=None
elif wwan_driver:
    sources=[w/'MtkWwan/Driver.c',w/'MtkWwan/DpmaifResources.c',
             w/'MtkWwan/DpmaifLayout.c',w/'MtkWwan/DpmaifHwCore.c',
             w/'MtkWwan/DpmaifDescriptor.c',w/'MtkWwan/DpmaifRing.c',
             w/'MtkWwan/DpmaifBatCore.c',w/'MtkWwan/DpmaifWdmDma.c',
             w/'MtkWwan/DpmaifSgCore.c']
    driver_name='MtkWwan'
    inf_source=w/'MtkWwan/MtkWwan.inf'
elif battery:
    flags += ['/DNTDDI_VERSION=0x0A000010']
    sources=[w/'MtkBattery/Driver.c',w/'MtkBattery/BatteryCore.c',w/'MtkMali/PmicPrm.c',
             root/'Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkGaugeCore.c']
    driver_name='MtkBattery'
    inf_source=w/'MtkBattery/MtkBattery.inf'
elif buttons:
    sources=[w/'MtkButtons/Driver.c',w/'MtkButtons/ButtonsCore.c',w/'MtkMali/PmicPrm.c',
             root/'Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkGaugeCore.c']
    driver_name='MtkButtons'
    inf_source=w/'MtkButtons/MtkButtons.inf'
elif display:
    flags += ['/std:c++17','/GR-','/EHs-c-','/Gy']
    sources=[w/'MtkDisplay'/n for n in ('bdd.cxx','bdd_ddi.cxx','bdd_dmm.cxx','bdd_util.cxx','blthw.cxx','memory.cxx','Framebuffer.c')]
    driver_name='MtkDisplay'
    inf_source=w/'MtkDisplay/MtkDisplay.inf'
elif sd:
    flags += ['/W4','/WX','/DNTDDI_VERSION=0x0A000010']
    sources=[w/'MtkSd'/name for name in ('Driver.c','SdCommand.c','SdDma.c','SdHost.c','SdPower.c')]
    driver_name='MtkSd'
    inf_source=w/'MtkSd/MtkSd.inf'
elif ufs_power:
    sources=[w/'MtkUfsPower/Driver.c',w/'MtkUfsPower/PrmClient.c']
    driver_name='MtkUfsPower'
    inf_source=w/'MtkUfsPower/MtkUfsPower.inf'
elif ufs:
    sources=[w/'MtkUfs/Driver.c', root/'Silicon/MediaTek/MediaTekPkg/Library/UfsCoreLib/MtkUfsTransport.c',
             root/'Silicon/MediaTek/MediaTekPkg/Library/UfsCoreLib/MtkUfsCore.c']
    driver_name='MtkUfs'
    inf_source=w/'MtkUfs/MtkUfs.inf'
elif gpio:
    sources=[w/'MtkGpio/Driver.c',w/'MtkGpio/Hardware.c']
    driver_name='MtkGpio'
    inf_source=w/'MtkGpio/MtkGpio.inf'
elif himax:
    sources=[w/'HimaxTouch/Driver.c',w/'HimaxTouch/InputReport.c',
             root/'Silicon/MediaTek/MediaTekPkg/Library/HimaxCoreLib/HimaxCore.c']
    driver_name='HimaxTouch'
    inf_source=w/'HimaxTouch/HimaxTouch.inf'
elif cpu:
    flags += ['/DNTDDI_VERSION=0x0A000000']
    sources=[w/'MtkCpu/Driver.c',w/'MtkCpu/CpuPpmCore.c']
    driver_name='MtkCpu'
    inf_source=w/'MtkCpu/MtkCpu.inf'
elif thermal:
    sources=[w/'MtkThermal/Driver.c', root/'Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkThermalCore.c']
    driver_name='MtkThermal'
    inf_source=w/'MtkThermal/MtkThermal.inf'
elif usb:
    sources=[w/'MtkUsb/Driver.c',root/'Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MusbHostCore.c',root/'Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MtkMusbPlatformCore.c',root/'Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MtkUsbPhyCore.c']
    driver_name='MtkUsb'
    inf_source=w/'MtkUsb/MtkUsb.inf'
else:
    sources=[w/'MtkI2c/Driver.c',root/'Silicon/MediaTek/MediaTekPkg/Library/MtkI2cCoreLib/MtkI2cCore.c']
    driver_name='MtkI2c'
    inf_source=w/'MtkI2c/MtkI2c.inf'
boot_inf_validation=None
if sd or usb or ufs or ufs_power:
    boot_inf_validation=subprocess.check_output(
        [str(Python),str(w/'test-storage-boot-inf.py')],text=True).strip()
    print(boot_inf_validation)
battery_core_validation=None
if cpu:
    cpu_package_validation=subprocess.check_output(
        [str(Python),str(w/'MtkCpu/test-package.py')],text=True).strip()
    print(cpu_package_validation)
if battery:
    battery_core_report=b/'MtkBattery-core-tests.json'
    subprocess.run(
        [str(Python),str(w/'MtkBattery/test-core.py'),'--report',str(battery_core_report)],
        check=True)
    battery_core_validation=json.loads(battery_core_report.read_text())
    battery_package_validation=subprocess.check_output(
        [str(Python),str(w/'MtkBattery/test-package.py')],text=True).strip()
    print(battery_package_validation)
def verify_dpmaif_reference(root):
    kernel=root.parent/'kernel-reference'
    commit=subprocess.check_output(['git','-C',str(kernel),'rev-parse','HEAD'],text=True).strip()
    expected={
        'mindone/modules/include/drivers/misc/mediatek/eccci/hif/ccci_hif_dpmaif_v3.h':
            'b3d8edac78acc50b77b3c7d5a212e05d378c5f76dab649317a90b99868f889b4',
        'mindone/modules/ccci_dpmaif/ccci_hif_dpmaif_v3.c':
            'c5bcc95806062222b9600e67ce613a35df0a75bca393fab52e140e9d7d576407',
        'mindone/modules/ccci_dpmaif/dpmaif_bat_v3.c':
            '3d3bd9205c91dfe937148e5908c6fc0c9f16b56e889b2eea9114cc55ac89495c',
        'mindone/modules/include/drivers/misc/mediatek/eccci/hif/dpmaif_arch_v3.h':
            '2d72dc477811e5d662c52b541952fc27ba1a4cd8cb39cbecafbd74bd75acbe02',
        'mindone/modules/include/drivers/misc/mediatek/eccci/hif/dpmaif_reg_v3.h':
            '05a22a13a06e6a03539f877d758d7218d4beae1a8ea433eac3be1a4c636939a8',
        'mindone/modules/ccci_dpmaif/dpmaif_drv_v3.c':
            'a5db4d1c8d6bd66e4eef9e795995cb3267c27e727b06a56b96b0284b2435b814',
        'mindone/modules/ccci_ccif/ccci_hif_ccif.c':
            '83343a84a66b4f83d2e19e9118115638687967653ae7ae488ce87d5a47bba6e9',
        'mindone/modules/include/drivers/misc/mediatek/eccci/hif/ccci_hif_ccif.h':
            '4728685d80def9ad79d5d12a90309de925b4db4465e431485cb362a00ffbd7fc',
        'mindone/modules/include/drivers/misc/mediatek/eccci/hif/ccif_hif_reg.h':
            'f41b4504be841dc8b70043316323f1b825eb32f5980b8379548e1b2343cbaf7b',
    }
    hashes={}
    acpi_resource_path=root/'Silicium-ACPI/Platforms/Teclast/t70pro/Cellular.asl'
    acpi_resource_digest=hashlib.sha256(acpi_resource_path.read_bytes()).hexdigest()
    expected_acpi_resource_digest='8d38fe5c48adaafac2e4dcabe0fcf4e8275240692903dc56cc0e65f81a54b2f2'
    if acpi_resource_digest!=expected_acpi_resource_digest:
        raise SystemExit('T70 HANA6797/HANA6798 ACPI resource map changed; review the WWAN map')
    hashes['Silicium-ACPI/Platforms/Teclast/t70pro/Cellular.asl']=acpi_resource_digest
    for path,expected_digest in expected.items():
        source=subprocess.check_output(['git','-C',str(kernel),'show','HEAD:'+path])
        digest=hashlib.sha256(source).hexdigest()
        if digest!=expected_digest:
            raise SystemExit('T70 CCCI/DPMAIF kernel source changed; review it before rebuilding: '+path)
        hashes[path]=digest
    return {'commit':commit,'source_sha256':hashes}

commands=[]
def run(cmd,name):
    if Darwin and cmd[0]=='clang-cl':
        cmd=['clang','--driver-mode=cl',*[(('/Tc' if Path(str(a)).suffix=='.c' else '/Tp')+str(a))
            if str(a).startswith('/') and Path(str(a)).suffix in ('.c','.cpp','.cxx') else a for a in cmd[1:]]]
    if Darwin and Zig and cmd[0]=='lld-link': cmd=[Zig,'lld-link',*cmd[1:]]
    commands.append([str(x) for x in cmd])
    r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (b/(name+'.log')).write_text(r.stdout)
    if r.returncode:
        errors=[x for x in r.stdout.splitlines() if 'error:' in x or 'lld-link:' in x]; print('\n'.join(errors or r.stdout.splitlines()[-15:])); raise SystemExit(r.returncode)
    if r.stdout: print('\n'.join(r.stdout.splitlines()[-10:]))
if wwan_ring_test:
    dpmaif_reference=verify_dpmaif_reference(root)
    ring_source=w/'MtkWwan/CcciRing.c'
    net_source=w/'MtkWwan/CcciNet.c'
    dpmaif_source=w/'MtkWwan/DpmaifDescriptor.c'
    dpmaif_ring_source=w/'MtkWwan/DpmaifRing.c'
    dpmaif_bat_source=w/'MtkWwan/DpmaifBatCore.c'
    dpmaif_tx_source=w/'MtkWwan/DpmaifTxPacket.c'
    dpmaif_resources_source=w/'MtkWwan/DpmaifResources.c'
    dpmaif_sg_source=w/'MtkWwan/DpmaifSgCore.c'
    dpmaif_wdm_source=w/'MtkWwan/DpmaifWdmDma.c'
    dpmaif_hw_source=w/'MtkWwan/DpmaifHwCore.c'
    dpmaif_layout_source=w/'MtkWwan/DpmaifLayout.c'
    ccif_source=w/'MtkWwan/CcciCcifCore.c'
    ccif_resources_source=w/'MtkWwan/CcciCcifResources.c'
    ring_obj=b/'CcciRing-arm64.obj'
    net_obj=b/'CcciNet-arm64.obj'
    dpmaif_obj=b/'DpmaifDescriptor-arm64.obj'
    dpmaif_ring_obj=b/'DpmaifRing-arm64.obj'
    dpmaif_bat_obj=b/'DpmaifBatCore-arm64.obj'
    dpmaif_tx_obj=b/'DpmaifTxPacket-arm64.obj'
    dpmaif_resources_obj=b/'DpmaifResources-arm64.obj'
    dpmaif_sg_obj=b/'DpmaifSgCore-arm64.obj'
    dpmaif_wdm_obj=b/'DpmaifWdmDma-arm64.obj'
    dpmaif_hw_obj=b/'DpmaifHwCore-arm64.obj'
    dpmaif_layout_obj=b/'DpmaifLayout-arm64.obj'
    ccif_obj=b/'CcciCcifCore-arm64.obj'
    ccif_resources_obj=b/'CcciCcifResources-arm64.obj'
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(ring_source),'/Fo'+str(ring_obj)],'CcciRing-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(net_source),'/Fo'+str(net_obj)],'CcciNet-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_source),'/Fo'+str(dpmaif_obj)],'DpmaifDescriptor-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_ring_source),'/Fo'+str(dpmaif_ring_obj)],'DpmaifRing-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_bat_source),'/Fo'+str(dpmaif_bat_obj)],'DpmaifBatCore-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_tx_source),'/Fo'+str(dpmaif_tx_obj)],'DpmaifTxPacket-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_resources_source),'/Fo'+str(dpmaif_resources_obj)],'DpmaifResources-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_sg_source),'/Fo'+str(dpmaif_sg_obj)],'DpmaifSgCore-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_wdm_source),'/Fo'+str(dpmaif_wdm_obj)],'DpmaifWdmDma-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_hw_source),'/Fo'+str(dpmaif_hw_obj)],'DpmaifHwCore-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(dpmaif_layout_source),'/Fo'+str(dpmaif_layout_obj)],'DpmaifLayout-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(ccif_source),'/Fo'+str(ccif_obj)],'CcciCcifCore-arm64')
    run([*flags,'/W4','/WX','/DNTDDI_VERSION=0x0A000010',str(ccif_resources_source),'/Fo'+str(ccif_resources_obj)],'CcciCcifResources-arm64')
    host_result=subprocess.check_output([str(Python),str(w/'MtkWwan/test-ring.py')],text=True).strip()
    objects=[ring_obj,net_obj,dpmaif_obj,dpmaif_ring_obj,dpmaif_bat_obj,dpmaif_tx_obj,dpmaif_resources_obj,dpmaif_sg_obj,dpmaif_wdm_obj,dpmaif_hw_obj,dpmaif_layout_obj,ccif_obj,ccif_resources_obj]
    report={'status':'passed','arm64_objects':[str(obj.relative_to(root)) for obj in objects],
        'arm64_object_bytes':{str(obj.name):obj.stat().st_size for obj in objects},
        'host_test':host_result,
        'scope':'CCCI/CCIF ring, stock CCMNI channel map, DPMAIF v3 descriptor decode/encode, 36-bit source-derived DMA aperture checks, WDM DMA adapter and common-buffer lifecycle, WDM scatter/gather mapping lifetime, SG-list normalization, PIT segmentation, RX/TX ring lifecycle, BAT token/slot ownership, TX packet descriptor planning, HANA6797 ACPI resource-map validation, source-derived aligned queue-memory layout, ordered common/interrupt/queue/init-done startup-plan composition, and source-locked CCCI CCIF send/ack and HANA6798 resource map, common SRAM/remap, interrupt-mask, RX PIT/BAT, four-TX DRB, and init-done plans with bounded fake-MMIO executor checks; no register operations are applied to mapped hardware, and no modem execution or WWAN/MBIM binding occurs',
        'kernel_reference':dpmaif_reference,
        'source_sha256':{str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (ring_source,w/'MtkWwan/CcciRing.h',net_source,w/'MtkWwan/CcciNet.h',
                         dpmaif_source,w/'MtkWwan/DpmaifDescriptor.h',w/'MtkWwan/DpmaifDescriptorTest.c',
                         dpmaif_ring_source,w/'MtkWwan/DpmaifRing.h',w/'MtkWwan/DpmaifRingTest.c',
                         dpmaif_bat_source,w/'MtkWwan/DpmaifBatCore.h',w/'MtkWwan/DpmaifDma.h',w/'MtkWwan/DpmaifBatCoreTest.c',
                         dpmaif_tx_source,w/'MtkWwan/DpmaifTxPacket.h',w/'MtkWwan/DpmaifDma.h',w/'MtkWwan/DpmaifTxPacketTest.c',
                         dpmaif_resources_source,w/'MtkWwan/DpmaifResources.h',w/'MtkWwan/DpmaifResourcesTest.c',
                         dpmaif_sg_source,w/'MtkWwan/DpmaifSgCore.h',w/'MtkWwan/DpmaifSgCoreTest.c',
                         dpmaif_wdm_source,w/'MtkWwan/DpmaifWdmDma.h',
                         w/'MtkWwan/DpmaifWdmDmaTest.c',w/'MtkWwan/Tests/include/ntddk.h',
                         dpmaif_hw_source,w/'MtkWwan/DpmaifHwCore.h',w/'MtkWwan/DpmaifHwCoreTest.c',
                         dpmaif_layout_source,w/'MtkWwan/DpmaifLayout.h',w/'MtkWwan/DpmaifLayoutTest.c',
                         ccif_source,w/'MtkWwan/CcciCcifCore.h',w/'MtkWwan/CcciCcifCoreTest.c',
                         ccif_resources_source,w/'MtkWwan/CcciCcifResources.h',w/'MtkWwan/CcciCcifResourcesTest.c',
                         root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkCcciMemoryHandoff.h',
                         w/'MtkWwan/CcciMemoryHandoffTest.c',
                         w/'MtkWwan/CcciRingTest.c',w/'MtkWwan/test-ring.py',
                         root/'Silicium-ACPI/Platforms/Teclast/t70pro/Cellular.asl')}}
    (b/'CcciRing-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(host_result)
    print(f"ARM64 CCCI ring object: {ring_obj.stat().st_size} bytes")
    print(f"ARM64 CCCI network map object: {net_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF descriptor object: {dpmaif_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF ring lifecycle object: {dpmaif_ring_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF BAT ownership object: {dpmaif_bat_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF TX packet planner object: {dpmaif_tx_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF ACPI resource mapper object: {dpmaif_resources_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF SG normalization object: {dpmaif_sg_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF WDM DMA adapter object: {dpmaif_wdm_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF queue initialization planner object: {dpmaif_hw_obj.stat().st_size} bytes")
    print(f"ARM64 DPMAIF queue layout object: {dpmaif_layout_obj.stat().st_size} bytes")
    print(f"ARM64 CCCI CCIF channel core object: {ccif_obj.stat().st_size} bytes")
    print(f"ARM64 HANA6798 CCIF resource mapper object: {ccif_resources_obj.stat().st_size} bytes")
    raise SystemExit(0)
if wwan_driver:
    dpmaif_reference=verify_dpmaif_reference(root)
    wwan_host_result=subprocess.check_output([str(Python),str(w/'MtkWwan/test-ring.py')],text=True).strip()
    wwan_package_validation=subprocess.check_output([str(Python),str(w/'MtkWwan/test-package.py')],text=True).strip()
    print(wwan_package_validation)
if '--pmic-prm-test' in sys.argv:
    sources_to_hash=[
        w/'MtkMali/PmicPrmTests.c',w/'MtkMali/PmicPrmHostShim.h',
        w/'MtkMali/PmicPrm.c',w/'MtkMali/PmicPrm.h',
        root/'Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkGaugeCore.c',
        root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkGaugeCore.h',
        root/'Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsPrm/MtkPmicBroker.c',
        root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkPmicBroker.h',
        root/'Platforms/Teclast/t70proPkg/Tests/Ufs/MtkPmicBrokerTest.c',
        Path(__file__).resolve()]
    exe=b/'MtkMaliPmicPrmHostTests'
    cc='clang' if Darwin else 'gcc'
    subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-DMTK_PMIC_PRM_HOST_TEST','-I'+str(root/'Silicon/MediaTek/MediaTekPkg/Include'),
        str(w/'MtkMali/PmicPrmTests.c'),'-o',str(exe)],check=True)
    native_result=subprocess.check_output([str(exe)],text=True).strip()
    broker_exe=b/'MtkPmicBrokerHostTests'
    subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-I'+str(root/'Silicon/MediaTek/MediaTekPkg/Include'),
        str(root/'Platforms/Teclast/t70proPkg/Tests/Ufs/MtkPmicBrokerTest.c'),
        str(root/'Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsPrm/MtkPmicBroker.c'),
        '-o',str(broker_exe)],check=True)
    broker_result=subprocess.check_output([str(broker_exe)],text=True).strip()
    report={'status':'passed','native_test':native_result,'broker_test':broker_result,
        'scope':'actual Windows PMIC PRM client, firmware PMIC broker and stock gauge transaction core under ASan/UBSan; PRM/WACS and hardware responses modeled; no Windows or tablet execution',
        'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sources_to_hash}}
    (b/'MtkMaliPmicPrm-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print(native_result)
    print(broker_result)
    raise SystemExit(0)
if '--sd-power-test' in sys.argv:
    power = w/'MtkSd/SdPower.c'
    test = w/'MtkSd/SdPowerTest.c'
    broker = root/'Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsPrm/MtkPmicBroker.c'
    lib = wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib'
    for source in (power, test, broker):
        test_flags = ['/Gy', '/GS-', '/D_NTOSKRNL_'] if source != power else []
        run([*flags, '/W4', '/WX', *test_flags, str(source),
             '/Fo'+str(b/(source.stem+'.obj'))], source.stem)
    exe = b/'MtkSdPowerTest.exe'
    run(['lld-link', '/nologo', '/machine:arm64', '/subsystem:native',
         '/entry:TestMain', '/nodefaultlib', '/opt:ref', '/out:'+str(exe),
         str(b/'SdPowerTest.obj'), str(b/'MtkPmicBroker.obj'), str(lib)], 'MtkSdPower-tests-link')
    subprocess.run([str(Python), str(w/'test-request.py'), str(exe),
                    '--test-name', 'MtkSdPower', '--report', str(b/'MtkSdPower-tests.json'),
                    '--production-source', str(power), '--test-source', str(test)], check=True)
    raise SystemExit(0)
if '--test' in sys.argv and sd:
    test_source=w/'MtkSd/DriverTest.c'
    test_inputs=[test_source,*sources[1:-1]]
    test_objects=[b/(path.stem+'-sd-test.obj') for path in test_inputs]
    for path,obj in zip(test_inputs,test_objects):
        run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(path),'/Fo'+str(obj)],obj.stem)
    exe=b/'MtkSdDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native',
         '/entry:TestMain','/nodefaultlib','/opt:ref','/out:'+str(exe),
         *[str(obj) for obj in test_objects],str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib')],'MtkSd-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(exe),
        '--test-name','MtkSd','--report',str(b/'MtkSd-tests.json'),
        '--production-source',str(sources[0]),'--test-source',str(test_source)],check=True)
    raise SystemExit(0)
if '--test' in sys.argv and buttons:
    checked=[*sources,w/'MtkButtons/ButtonsCore.h',w/'MtkButtons/ButtonsCoreTest.c',
             w/'MtkButtons/DriverTest.c',w/'MtkMali/PmicPrm.h',w/'MtkMali/Mfg.h',
             root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkPmicBroker.h',
             inf_source,Path(__file__).resolve(),w/'test-request.py']
    before={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in checked}
    with tempfile.TemporaryDirectory(prefix='buttons-core-',dir=None if Darwin else b) as tmp:
        bridge=Path(tmp)/'bridge.c';exe=Path(tmp)/'ButtonsCoreTests'
        bridge.write_text('#include <stdio.h>\n#include '+json.dumps(str(w/'MtkButtons/ButtonsCoreTest.c'))+
            '\nint main(void) { int r=TestMain(); if(r) { fprintf(stderr,"button check failed at line %d\\n",r);return 1; }'
            ' printf("%u checks\\n",(unsigned)mChecks);return 0; }\n')
        subprocess.run(['clang' if Darwin else 'gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(bridge),'-o',str(exe)],check=True)
        native=subprocess.check_output([str(exe)],text=True).strip()
    obj=b/'MtkButtons-driver-test.obj';core_obj=b/'MtkButtons-core-test.obj'
    run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(w/'MtkButtons/DriverTest.c'),'/Fo'+str(obj)],'MtkButtons-driver-test')
    run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(w/'MtkButtons/ButtonsCore.c'),'/Fo'+str(core_obj)],'MtkButtons-core-test')
    exe=b/'MtkButtonsDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain',
         '/nodefaultlib','/opt:ref','/out:'+str(exe),str(obj),str(core_obj),
         str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib')],'MtkButtons-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(exe),
        '--test-name','MtkButtons','--report',str(b/'MtkButtons-driver-tests.json'),
        '--production-source',str(w/'MtkButtons/Driver.c'),
        '--test-source',str(w/'MtkButtons/DriverTest.c')],check=True)
    after={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in checked}
    assert before==after,'Button sources changed during execution'
    report={'status':'passed','native_core':native,
        'scope':'actual portable button core with ASan/UBSan and actual Driver.c ARM64 callbacks; kernel/WDF/MMIO/inbox button interface and PMIC client entry mocked; no Windows or device execution',
        'driver_harness':str(b/'MtkButtons-driver-tests.json'),'source_sha256':after,
        'inputs_unchanged_during_execution':True,'physical_wake_implemented':False,
        'PMIC_client_independent_evidence':'Windows/Build/mali-verification.json PmicPrm group; shared actual source, not linked into this mocked callback test'}
    (b/'MtkButtons-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Buttons core:',native,'and actual ARM64 driver callbacks passed')
    raise SystemExit(0)
if '--test' in sys.argv and (usb or thermal or cpu):
    if cpu:
        native_exe=b/'CpuPpmCoreTests'
        subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            str(w/'MtkCpu/CpuPpmCore.c'),str(w/'MtkCpu/CpuPpmCoreTest.c'),'-o',str(native_exe)],check=True)
        native_result=subprocess.check_output([str(native_exe)],text=True).strip()
        print(native_result)
    library_headers=root/'Silicon/MediaTek/MediaTekPkg/Include/Library'
    if usb:
        test_headers=[library_headers/'MusbHostCore.h',library_headers/'MtkMusbPlatformCore.h']
        test_libraries=[str(wdk/'Lib/10.0.26100.0/km/ARM64/ucx/1.6/ucxstub.lib')]
        test_kind='usb'
        scope='actual MtkUsb ARM64 callbacks, core root-port suspend/resume and host PHY validation under WDF/UCX/kernel/MMIO mocks'
    elif cpu:
        test_headers=[w/'MtkCpu/CpuIo.h',w/'MtkCpu/CpuPpmCore.h',w/'MtkCpu/CpuPpmCoreTest.c']
        test_libraries=[]
        test_kind='cpu'
        scope='actual MtkCpu ARM64 resource/OPP and PEP PPM callbacks under KMDF, PoFx-handle and MMIO mocks; native PPM domain core under ASan/UBSan'
    else:
        test_headers=[library_headers/'MtkThermalCore.h',w/'MtkThermal/ThermalIo.h']
        test_libraries=[]
        test_kind='thermal'
        scope='actual MtkThermal ARM64 callbacks and NTC/LVTS conversion with KMDF, ACPI, time and MMIO mocks'
    test_source=w/driver_name/'DriverTest.c'
    test_sources=[test_source,*sources[1:]]
    test_objects=[]
    for src in test_sources:
        name=src.stem+'-'+test_kind+'-test'
        obj=b/(name+'.obj')
        run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(src),'/Fo'+str(obj)],name)
        test_objects.append(str(obj))
    test_exe=b/(driver_name+'DriverTests.exe')
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain',
         '/nodefaultlib','/opt:ref','/out:'+str(test_exe),*test_objects,
         str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib'),
         *test_libraries],driver_name+'-tests-link')
    harness_report=b/(driver_name+'-driver-tests.json')
    subprocess.run([str(Python),str(w/'test-request.py'),str(test_exe),
        '--test-name',driver_name,'--report',str(harness_report),
        '--production-source',str(sources[0]), '--test-source',str(test_source)],check=True)
    report={'status':'passed',
            'scope':scope+'; no Windows load or tablet access',
            'driver_harness':str(harness_report),
            'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sources+[test_source,*test_headers,inf_source,Path(__file__).resolve(),w/'test-request.py']}}
    if cpu:
        report['native_core']=native_result
    (b/(driver_name+'-tests.json')).write_text(json.dumps(report,indent=2)+'\n')
    raise SystemExit(0)
if '--test' in sys.argv and display:
    with tempfile.TemporaryDirectory(prefix='display-core-',dir=None if Darwin else b) as tmp:
        exe=Path(tmp)/'FramebufferTests'
        subprocess.run(['clang' if Darwin else 'gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            str(w/'MtkDisplay/Framebuffer.c'),str(w/'MtkDisplay/Tests.c'),'-o',str(exe)],check=True)
        native=subprocess.check_output([str(exe)],text=True).strip()
    objects=[]
    for src in [*sources,w/'MtkDisplay/DriverTest.cxx']:
        obj=b/(src.stem+'-display-test.obj')
        run([x for x in flags if src.suffix != '.c' or x != '/std:c++17']+['/GS-','/D_NTOSKRNL_','/DNTKERNELAPI=',str(src),'/Fo'+str(obj)],src.stem+'-display-test')
        objects.append(str(obj))
    exe=b/'MtkDisplayDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
         '/opt:ref','/out:'+str(exe),*objects,str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib'),
         str(wdk/'Lib/10.0.26100.0/km/ARM64/libcntpr.lib')],'MtkDisplay-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(exe),
        '--test-name','MtkDisplay','--report',str(b/'MtkDisplay-driver-tests.json'),
        '--production-source',str(w/'MtkDisplay/bdd.cxx'),
        '--test-source',str(w/'MtkDisplay/DriverTest.cxx')],check=True)
    report={'status':'passed','native_framebuffer':native,
        'scope':'actual DOD ARM64 production callbacks and software present under kernel/Dxgk/ACPI/MDL mocks; portable framebuffer ASan/UBSan; no Windows load or tablet access',
        'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in list((w/'MtkDisplay').glob('*'))+[Path(__file__).resolve(),w/'test-request.py'] if p.is_file()}}
    (b/'MtkDisplay-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print(native)
    raise SystemExit(0)
if '--test' in sys.argv and ufs:
    test_sources=[w/'MtkUfs/DriverTest.c',*sources[1:]]
    test_objects=[]
    for src in test_sources:
        obj=b/(src.stem+'-ufs-test.obj')
        run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(src),'/Fo'+str(obj)],src.stem+'-ufs-test')
        test_objects.append(str(obj))
    test_exe=b/'MtkUfsDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain',
         '/nodefaultlib','/opt:ref','/out:'+str(test_exe),*test_objects,
         str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib')],'MtkUfs-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(test_exe),
        '--test-name','MtkUfs','--report',str(b/'MtkUfs-driver-tests.json'),
        '--production-source',str(w/'MtkUfs/Driver.c'),
        '--test-source',str(w/'MtkUfs/DriverTest.c')],check=True)
    report={'status':'passed','scope':'actual ARM64 Driver.c Storport callbacks and portable UFS engine; fake kernel, IRQ, DMA and MMIO; no Windows load or tablet access',
            'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sources+[w/'MtkUfs/DriverTest.c',inf_source,
                  root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsTransport.h',
                  root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsCore.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsPower.h',*list((w/'MtkUfs').glob('*.h')),Path(__file__).resolve(),w/'test-request.py']}}
    (b/'MtkUfs-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    raise SystemExit(0)
if '--test' in sys.argv and gpio:
    with tempfile.TemporaryDirectory(prefix='gpio-core-',dir=None if Darwin else b) as tmp:
        exe=Path(tmp)/'GpioHardwareTests'
        subprocess.run(['clang' if Darwin else 'gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            str(w/'MtkGpio/Hardware.c'),str(w/'MtkGpio/Tests.c'),'-o',str(exe)],check=True)
        native=subprocess.check_output([str(exe)],text=True).strip()
    test_objects=[]
    for src in [w/'MtkGpio/DriverTest.c',w/'MtkGpio/Hardware.c']:
        obj=b/(src.stem+'-gpio-test.obj')
        run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(src),'/Fo'+str(obj)],src.stem+'-gpio-test')
        test_objects.append(str(obj))
    test_exe=b/'MtkGpioDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain',
         '/nodefaultlib','/opt:ref','/out:'+str(test_exe),*test_objects,
         str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib')],'MtkGpio-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(test_exe),
        '--test-name','MtkGpio','--report',str(b/'MtkGpio-driver-tests.json'),
        '--production-source',str(w/'MtkGpio/Driver.c'),
        '--test-source',str(w/'MtkGpio/DriverTest.c')],check=True)
    def function_span(path,signature):
        lines=path.read_text().splitlines()
        start=next((i for i,line in enumerate(lines) if signature in line),None)
        if start is None: raise SystemExit('Missing GPIO drive evidence function: '+signature)
        depth=0; opened=False
        for end in range(start,len(lines)):
            depth+=lines[end].count('{')-lines[end].count('}')
            opened=opened or '{' in lines[end]
            if opened and depth==0: return [start+1,end+1]
        raise SystemExit('Unterminated GPIO drive evidence function: '+signature)
    def declaration_span(path,signature):
        lines=path.read_text().splitlines()
        start=next((i for i,line in enumerate(lines) if signature in line),None)
        if start is None: raise SystemExit('Missing GPIO drive evidence declaration: '+signature)
        end=next((i for i in range(start,len(lines)) if lines[i].strip()=='};'),None)
        if end is None: raise SystemExit('Unterminated GPIO drive evidence declaration: '+signature)
        return [start+1,end+1]
    def marker_lines(path,marker):
        return [i+1 for i,line in enumerate(path.read_text().splitlines()) if marker in line]
    evidence_root=root.parent
    pinctrl=evidence_root/'kernel-reference/mindone/modules/pinctrl_mediatek'
    evidence_paths=[pinctrl/'pinctrl-paris.c',pinctrl/'pinctrl-mtk-common-v2.c',
                    pinctrl/'pinctrl-mt6789.c']
    stock_module=evidence_root/'analysis/stock-binaries/vendor_boot_a_0/pinctrl-mt6789.ko'
    stock_module_path=str(stock_module.relative_to(evidence_root))
    # The binary hash documents provenance for the existing pin map.
    if stock_module.is_file():
        evidence_paths.append(stock_module)
    evidence_hashes={str(p.relative_to(evidence_root)):hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in evidence_paths}
    gpio_driver=w/'MtkGpio/Driver.c'
    paris=pinctrl/'pinctrl-paris.c'; common=pinctrl/'pinctrl-mtk-common-v2.c'; mt6789=pinctrl/'pinctrl-mt6789.c'
    report={'status':'passed','native_hardware':native,
            'scope':'Production portable hardware core with ASan/UBSan; actual ARM64 Driver.c callbacks under WDF/GpioClx/ACPI/MMIO mocks; no Windows load or tablet access',
            'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
                for p in list((w/'MtkGpio').glob('*'))+[Path(__file__).resolve(),w/'test-request.py'] if p.is_file()},
            'drive_strength_policy':{
                'gpioclx':{'field':'GPIO_CONNECT_IO_PINS_PARAMETERS.DriveStrength',
                    'units':'10 microamperes per unit',
                    'documentation':'https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gpioclx/ns-gpioclx-_gpio_connect_io_pins_parameters'},
                'nonzero_request':'STATUS_NOT_SUPPORTED',
                'conversion':'none; available sources do not prove a GpioClx-unit to MT6789 vendor-EH mapping',
                'implementation':{'path':str(gpio_driver.relative_to(root)),
                    'function':'MgConnectIo','lines':function_span(gpio_driver,'NTSTATUS MgConnectIo('),
                    'core_does_not_write_EH_for_GpioClx_drive_strength':True},
                'stock_semantics':{
                    'vendor_mt6789':{'path_base':str(evidence_root),'pinctrl_paris':{'path':str(paris.relative_to(evidence_root)),
                        'advanced_drive_dispatch_lines':marker_lines(paris,'case MTK_PIN_CONFIG_DRV_ADV:'),
                        'advanced_argument_is_passed_to_soc_callback_without_unit_conversion':True},
                    'advanced_drive_callback':{'path':str(common.relative_to(evidence_root)),
                        'function':'mtk_pinconf_adv_drive_set','lines':function_span(common,'int mtk_pinconf_adv_drive_set('),
                        'tries_raw_five_bit_EH_field_before_EN_E0_E1_fallback':True},
                    'mt6789_soc_and_EH_table':{'path':str(mt6789.relative_to(evidence_root)),
                        'EH_field_table_lines':declaration_span(mt6789,'static const struct mtk_pin_field_calc mt6789_pin_eh_range[]'),
                        'SoC_registration_lines':declaration_span(mt6789,'static const struct mtk_pin_soc mt6789_data ='),
                        'EH_registration_lines':marker_lines(mt6789,'[PINCTRL_PIN_REG_DRV_EH] = MTK_RANGE(mt6789_pin_eh_range)'),
                        'adv_drive_set_registration_lines':marker_lines(mt6789,'.adv_drive_set = mtk_pinconf_adv_drive_set'),
                        'raw_drv_flag_lines':marker_lines(mt6789,'| FLAG_DRIVE_SET_RAW'),
                        'raw_drive_flag_applies_to_regular_DRV_not_advanced_EH':True},
                    'stock_module_path':stock_module_path,
                    'stock_module_available':stock_module_path in evidence_hashes,
                    'stock_module_sha256':evidence_hashes.get(stock_module_path)},
                    'upstream_v6_18':{'mapping_scope':'EN/E0/E1 fields only; this conversion is not an EH encoding',
                        'documented_code_values':'1/3/5/7 apply to EN/E0/E1',
                        'advanced_drive_set_has_no_EH_path':True,
                        'vendor_eh_conversion_inferred_from_upstream_mapping':False,
                        'raw_drive_handler_is_separate':True}},
                'evidence_source_sha256':evidence_hashes}
            }
    (b/'MtkGpio-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print(native)
    raise SystemExit(0)
if '--test' in sys.argv and himax:
    core=root/'Silicon/MediaTek/MediaTekPkg/Library/HimaxCoreLib/HimaxCore.c'
    mapper=w/'HimaxTouch/InputReport.c'
    mapper_test=w/'HimaxTouch/Tests.c'
    with tempfile.TemporaryDirectory(prefix='himax-mapper-',dir=None if Darwin else b) as tmp:
        exe=Path(tmp)/'HimaxTouchMapperTests'
        subprocess.run(['clang' if Darwin else 'gcc','-std=c11','-Wall','-Wextra','-Werror',
            '-I'+str(root/'Silicon/MediaTek/MediaTekPkg/Include'),
            '-I'+str(w/'HimaxTouch'),str(core),str(mapper),str(mapper_test),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        if result.returncode:
            print(result.stdout+result.stderr); raise SystemExit(result.returncode)
    test_sources=[w/'HimaxTouch/DriverTest.c',mapper,core]
    test_objects=[]
    for src in test_sources:
        obj=b/(src.stem+'-himax-test.obj')
        run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(src),'/Fo'+str(obj)],src.stem+'-himax-test')
        test_objects.append(str(obj))
    test_exe=b/'HimaxTouchDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain',
         '/nodefaultlib','/opt:ref','/out:'+str(test_exe),*test_objects,
         str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib')],'HimaxTouch-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(test_exe),
        '--test-name','HimaxTouch','--report',str(b/'HimaxTouch-driver-tests.json'),
        '--production-source',str(w/'HimaxTouch/Driver.c'),
        '--test-source',str(w/'HimaxTouch/DriverTest.c')],check=True)
    report={'scope':'HimaxCore native parser/mapper checks plus production Driver.c ARM64 callback execution in Unicorn; WDF/VHF/SPB/kernel mocked; no tablet access',
            'status':'passed',
            'checks':['10-contact HID mapping and transactional errors',
                      'actual HmInterruptIsr SPB command 0x30, two-transfer sequence, 56-byte read and 50 ms timeout',
                      'actual HMXR D0 readiness gate',
                      'actual HmReadWork synchronous VHF-ready callback token race',
                      'actual failed VHF submission consumes one token and preserves pending report until the next callback',
                      'actual three consecutive decode failures submit coordinate-preserving all-up and disable WDF interrupt',
                      'actual queue overflow sends all-up before the latest frame',
                      'actual HmD0Exit release submission and retained release when no VHF token exists'],
            'driver_harness':str(b/'HimaxTouch-driver-tests.json'),
            'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
             for p in [core,mapper,mapper_test,w/'HimaxTouch/DriverTest.c',w/'HimaxTouch/Driver.c',
                       w/'HimaxTouch/InputReport.h',w/'HimaxTouch/HimaxTouch.inf',w/'test-request.py',Path(__file__).resolve()]}}
    (b/'HimaxTouch-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Himax native parser/mapper checks and production Driver.c ARM64 callback execution passed')
    raise SystemExit(0)
if '--test' in sys.argv and ufs_power:
    source=w/'MtkUfsPower/DriverTest.c'
    obj=b/'MtkUfsPower-driver-test.obj'
    run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(source),'/Fo'+str(obj)],'MtkUfsPower-driver-test')
    exe=b/'MtkUfsPowerDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain',
         '/nodefaultlib','/opt:ref','/out:'+str(exe),str(obj),
         str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib')],'MtkUfsPower-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(exe),
        '--test-name','MtkUfsPower','--report',str(b/'MtkUfsPower-driver-tests.json'),
        '--production-source',str(w/'MtkUfsPower/Driver.c'),
        '--test-source',str(source)],check=True)
    checked=[*sources,source,w/'MtkUfsPower/PrmClient.h',inf_source,
             root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsPower.h',
             Path(__file__).resolve(),w/'test-request.py']
    report={'status':'passed',
            'scope':'actual ARM64 WDM upper power filter and PRM client under I/O-manager, power, storage and PRM mocks; no Windows load or tablet access',
            'driver_harness':str(b/'MtkUfsPower-driver-tests.json'),
            'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in checked}}
    (b/'MtkUfsPower-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    raise SystemExit(0)
if '--test' in sys.argv and battery:
    source=w/'MtkBattery/DriverTest.c'
    driver_obj=b/'MtkBattery-driver-test.obj';core_obj=b/'MtkBattery-core-driver-test.obj'
    run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(source),'/Fo'+str(driver_obj)],'MtkBattery-driver-test')
    run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(w/'MtkBattery/BatteryCore.c'),'/Fo'+str(core_obj)],'MtkBattery-core-driver-test')
    exe=b/'MtkBatteryDriverTests.exe'
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
         '/opt:ref','/out:'+str(exe),str(driver_obj),str(core_obj),
         str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib'),
         str(wdk/'Lib/10.0.26100.0/km/ARM64/wmilib.lib')],'MtkBattery-tests-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(exe),
        '--test-name','MtkBattery','--report',str(b/'MtkBattery-driver-tests.json'),
        '--production-source',str(w/'MtkBattery/Driver.c'),'--test-source',str(source)],check=True)
    raise SystemExit(0)
if '--test' in sys.argv and not display:
    src=w/'Tests/RequestTest.c'
    run(flags+['/Gy','/GS-','/D_NTOSKRNL_',str(src),'/Fo'+str(b/'RequestTest.obj')],'RequestTest')
    run(['lld-link','/nologo','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
         '/opt:ref','/out:'+str(b/'RequestTest.exe'),str(b/'RequestTest.obj'),str(wdk/'Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib')],'RequestTest-link')
    subprocess.run([str(Python),str(w/'test-request.py'),str(b/'RequestTest.exe')],check=True)
    raise SystemExit(0)
objects=[b/((driver_name+'-'+src.stem if buttons else src.stem)+'.obj') for src in sources]
for src,obj in zip(sources,objects): run([x for x in flags if src.suffix != '.c' or x != '/std:c++17']+[str(src),'/Fo'+str(obj)],driver_name+'-'+src.stem if buttons else src.stem)
lib=wdk/'Lib/10.0.26100.0/km/ARM64'; wdf=wdk/'Lib/wdf/kmdf/ARM64/1.33'
# The Windows loader rejects native kernel images with LLD's default TSAWARE flag.
run(['lld-link','/nologo','/driver:wdm' if wdm_image else '/driver',*(['/release'] if wdm_image else []),'/machine:arm64','/subsystem:native,10.00','/entry:'+driver_entry,'/map:'+str(b/(driver_name+'.map')),'/nodefaultlib',
     '/dynamicbase','/nxcompat','/tsaware:no','/osversion:10.0','/Brepro','/out:'+str(b/(driver_name+'.sys')),
     *[str(obj) for obj in objects],
     *([str(lib/'displib.lib')] if display else [str(lib/'storport.lib')] if ufs else [str(lib/'sdport.lib')] if sd else [] if ufs_power else
       ([str(lib/'battc.lib'),str(lib/'wmilib.lib'),str(wdf/'wdfdriverentry.lib'),str(wdf/'wdfldr.lib')] if battery else [str(lib/'ndis.lib')] if wwan_driver else [str(wdf/'wdfdriverentry.lib'),str(wdf/'wdfldr.lib')])),str(lib/'ntoskrnl.lib'),str(lib/'hal.lib'),
     *([str(lib/'msgpioclxstub.lib')] if gpio else [str(lib/'spb/1.1/spbcxstubs.lib')] if i2c else []),
     *([str(lib/'ucx/1.6/ucxstub.lib')] if usb else []),
     *([str(lib/'vhfkm.lib'),str(lib/'libcntpr.lib')] if himax else [str(lib/'libcntpr.lib')] if display else []),str(lib/'bufferoverflowfastfailk.lib')],'link')
pe=b/(driver_name+'.sys')
if Darwin:
    info=subprocess.check_output(['xcrun','llvm-objdump','--file-headers','--private-headers',str(pe)],text=True)
    assert 'file format coff-arm64' in info and '(NT native)' in info
else:
    info=subprocess.check_output(['llvm-readobj','--file-headers','--coff-imports',str(pe)],text=True)
    assert 'IMAGE_FILE_MACHINE_ARM64' in info and 'IMAGE_SUBSYSTEM_NATIVE' in info
if 'TERMINAL_SERVER_AWARE' in info:
    raise SystemExit(driver_name+' kernel image has the Terminal Server flag')
if usb and 'spbcx' in info.lower(): raise SystemExit('MtkUsb PE unexpectedly imports SpbCx')
if wwan_driver:
    required_imports=('NdisMRegisterMiniportDriver','NdisMSetMiniportAttributes',
                      'NdisMGetDeviceProperty','NdisMMapIoSpace','NdisMUnmapIoSpace',
                      'IoGetDmaAdapter')
    missing=[name for name in required_imports if name not in info]
    if missing: raise SystemExit('MtkWwan PE is missing NDIS/WDM imports: '+', '.join(missing))
    if 'wdfldr.sys' in info.lower() or 'wdf01000.sys' in info.lower():
        raise SystemExit('MtkWwan NDIS miniport unexpectedly imports KMDF')
if wdm_image:
    import struct
    image=pe.read_bytes();pe_offset=struct.unpack_from('<I',image,0x3c)[0];optional=pe_offset+24
    checksum_offset=optional+64;checksum=struct.unpack_from('<I',image,checksum_offset)[0];total=0
    for offset in range(0,len(image),2):
        if checksum_offset<=offset<checksum_offset+4:continue
        total+=int.from_bytes(image[offset:offset+2],'little')
        total=(total&0xffff)+(total>>16)
    total=(total&0xffff)+(total>>16);expected_checksum=(total&0xffff)+len(image)
    if not checksum or checksum!=expected_checksum:raise SystemExit(driver_name+' PE checksum mismatch')
    if not struct.unpack_from('<H',image,optional+70)[0]&0x2000:raise SystemExit(driver_name+' PE is missing its WDM image flag')


framework_entry_proof=None
if framework_driver:
    import re
    map_file=b/(driver_name+'.map')
    symbols={name:int(address,16) for name,address in re.findall(
        r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+',map_file.read_text(),re.M)}
    image_base=struct.unpack_from('<Q',image,optional+24)[0]
    entry_rva=struct.unpack_from('<I',image,optional+16)[0]
    assert symbols.get('FxDriverEntry')==image_base+entry_rva,'KMDF PE bypasses framework entry'
    assert 'DriverEntry' in symbols and symbols['DriverEntry']!=symbols['FxDriverEntry']
    assert 'WdfVersionBind' in info and 'wdfldr.sys' in info.lower(),'KMDF loader binding missing'
    framework_entry_proof={'status':'passed','PE_entrypoint':'FxDriverEntry',
        'entrypoint_rva':entry_rva,'driver_entry_rva':symbols['DriverEntry']-image_base,
        'WdfVersionBind_import_present':True,'WDFLDR_import_present':True,
        'link_map_sha256':hashlib.sha256(map_file.read_bytes()).hexdigest(),
        'documentation':'https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/building-and-loading-a-kmdf-driver',
        'scope':'Actual PE entry RVA equals WDK framework stub symbol in linker map; loader imports verified; execution reported separately',
        'Windows_loaded':False}

if battery:
    if 'BatteryClassInitializeDevice' not in info or 'BatteryClassIoctl' not in info:
        raise SystemExit('MtkBattery is missing the battery class driver imports')
    if 'ExGetPrmInterface' not in info:
        raise SystemExit('MtkBattery is missing the shared PMIC PRM client import')
if buttons:
    if 'ExGetPrmInterface' not in info: raise SystemExit('Buttons driver is missing its shared PRM client import')
    if any(n in info.lower() for n in ('spbcx.sys','ucx01000.sys','vhf.sys')):
        raise SystemExit('Buttons provider must use the inbox button interface, without USB/SPB/VHF')
if ufs and 'ExGetPrmInterface' in info:
    raise SystemExit('Physical Storport miniport must not call the PRM WDM interface')
if ufs_power:
    if 'ExGetPrmInterface' not in info:
        raise SystemExit('UFS power filter is missing its PRM interface import')
    forbidden=[name for name in ('storport.sys','wdfldr.sys','wdf01000.sys','spbcx.sys') if name in info.lower()]
    if forbidden:
        raise SystemExit('UFS WDM power filter has unexpected framework imports: '+', '.join(forbidden))

if sd:
    if any(symbol not in info for symbol in ('SdPortInitialize','SdPortCompleteRequest','ExGetPrmInterface')):
        raise SystemExit('MtkSd SDPORT or PRM imports are missing')
    if any(name in info.lower() for name in ('storport.sys','wdfldr.sys','spbcx.sys')):
        raise SystemExit('MtkSd has unexpected framework imports')

(b/(driver_name+'.inf')).write_bytes(inf_source.read_bytes())
source_files=list(sources)
if battery: source_files += [w/'MtkBattery/BatteryCore.h',w/'MtkBattery/BatteryCoreTest.c',w/'MtkBattery/StockProfile.h',w/'MtkBattery/MtkBattery.inf',
    w/'MtkBattery/generate-profile.py',w/'MtkBattery/test-core.py',w/'MtkBattery/test-package.py',w/'MtkBattery/DriverTest.c',w/'MtkMali/PmicPrm.h',w/'MtkMali/Mfg.h',
    root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkPmicBroker.h',
    root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkGaugeCore.h']
if buttons: source_files += [w/'MtkButtons/ButtonsCore.h',w/'MtkMali/PmicPrm.h',w/'MtkMali/Mfg.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkPmicBroker.h']
elif display: source_files += [w/'MtkDisplay/Framebuffer.h',w/'MtkDisplay/bdd.hxx',w/'MtkDisplay/upstream.json',w/'MtkDisplay/bdd_errorlog.hxx']
elif sd: source_files += [w/'MtkSd'/name for name in ('SdCommand.h','SdDma.h','SdHost.h','SdPower.h')]
elif ufs_power: source_files += [w/'MtkUfsPower/PrmClient.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsPower.h']
elif ufs: source_files += [root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsCore.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsTransport.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsPower.h',*list((w/'MtkUfs').glob('*.h'))]
elif gpio: source_files += [w/'MtkGpio'/name for name in ('PinMap.h','Hardware.h','GpsLnaInterface.h','ConnPinsInterface.h')]
elif usb: source_files += [root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MusbHostCore.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkMusbPlatformCore.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUsbPhyCore.h',w/'test-request.py']
elif cpu: source_files += [w/'MtkCpu/CpuIo.h',w/'MtkCpu/CpuPpmCore.h',
    w/'MtkCpu/CpuPpmCoreTest.c',w/'MtkCpu/DriverTest.c',w/'MtkCpu/test-package.py',
    w/'MtkCpu/MtkCpu.inf']
elif thermal: source_files += [w/'MtkThermal/ThermalIo.h',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkThermalCore.h']
elif wwan_driver: source_files += [w/'MtkWwan/MtkWwan.inf',w/'MtkWwan/test-package.py',w/'MtkWwan/test-ring.py',w/'MtkWwan/test-oid.py',w/'MtkWwan/NdisOidCallbackHarness.c',w/'MtkWwan/CcciMemoryHandoffTest.c',w/'MtkWwan/NdisCallbackCore.h',w/'MtkWwan/NdisCallbackCoreTest.c',w/'MtkWwan/DpmaifResources.h',w/'MtkWwan/DpmaifLayout.h',w/'MtkWwan/DpmaifLayoutTest.c',w/'MtkWwan/DpmaifHwCore.h',w/'MtkWwan/DpmaifHwCoreTest.c',w/'MtkWwan/DpmaifDma.h',w/'MtkWwan/DpmaifWdmDma.h',w/'MtkWwan/DpmaifSgCore.h',w/'MtkWwan/DpmaifTxPacket.h',w/'MtkWwan/DpmaifDescriptor.h',w/'MtkWwan/DpmaifRing.h',w/'MtkWwan/DpmaifBatCore.h',w/'MtkWwan/DpmaifBatCoreTest.c',root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkCcciMemoryHandoff.h',root/'Platforms/Teclast/t70proPkg/Library/MemoryMapLib/MemoryMapLib.c',root/'Platforms/Teclast/t70proPkg/Tests/RuntimeMemoryTest.c',root/'Platforms/Teclast/t70proPkg/verify-acpi.py',root/'Silicium-ACPI/Platforms/Teclast/t70pro/Cellular.asl']
elif himax: source_files += [root/'Silicon/MediaTek/MediaTekPkg/Include/Library/HimaxCore.h']
else: source_files += [root/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkI2cCore.h']
source_files += [inf_source,Path(__file__).resolve(),w/'build_output.py']
if boot_inf_validation is not None: source_files.append(w/'test-storage-boot-inf.py')
report={'state':'cross-compiled, unsigned, not installed or hardware-tested',
        'compiler':subprocess.check_output(['clang','--driver-mode=cl','--version'] if Darwin else ['clang-cl','--version'],text=True).splitlines()[0],
        'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files},
        'toolchain':{'nuget':json.loads((t/'manifest.json').read_text()),
                     'crt_headers':json.loads((t/'msvc-crt-headers/source.json').read_text())},
        'binary':{'path':str(pe),'bytes':pe.stat().st_size,'sha256':hashlib.sha256(pe.read_bytes()).hexdigest()},
        'commands':commands,'pe_headers_and_imports':info}
if ufs or ufs_power:
    report['coff_import_validation']={
        'tool':'xcrun llvm-objdump --file-headers --private-headers' if Darwin else 'llvm-readobj --file-headers --coff-imports',
        'result':'passed',
        'contract':'PRM is called by the same-stack WDM power filter; physical Storport miniport has no ExGetPrmInterface import',
        'PRM_interface_import_present':bool('ExGetPrmInterface' in info)}
if usb: report['coff_import_validation']={'tool':'xcrun llvm-objdump --file-headers --private-headers' if Darwin else 'llvm-readobj --file-headers --coff-imports','result':'passed; SpbCx imports absent'}
if wdm_image: report['PE_integrity_validation']={'checksum':checksum,'independently_computed_checksum':expected_checksum,'checksum_verified':True,'WDM_image_flag':True,'signed':False,'Windows_loaded':False}
if framework_entry_proof is not None: report['framework_entry_validation']=framework_entry_proof
if boot_inf_validation is not None: report['boot_inf_source_check']={'status':'passed','output':boot_inf_validation}
if wwan_driver:
    report['wwan_host_test']=wwan_host_result
    report['wwan_package_binding_check']={'status':'passed','output':wwan_package_validation}
    report['kernel_reference']=dpmaif_reference
if battery:
    report['battery_core_host_test']=battery_core_validation
    report['battery_package_binding_check']={'status':'passed','output':battery_package_validation}
(b/(driver_name.lower()+'-verification.json')).write_text(json.dumps(report,indent=2)+'\n')
print('ARM64 native driver linked:',pe.stat().st_size,'bytes; unsigned; no installation performed')
