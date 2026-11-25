#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the T70 ACPI WWAN miniport package and its not-ready behavior."""
from pathlib import Path
import re

here=Path(__file__).resolve().parent
root=here.parents[1]
inf=(here/'MtkWwan.inf').read_text()
driver=(here/'Driver.c').read_text()
asl=(root/'Silicium-ACPI/Platforms/Teclast/t70pro/Cellular.asl').read_text()
memory_map=(root/'Platforms/Teclast/t70proPkg/Library/MemoryMapLib/MemoryMapLib.c').read_text()
acpi_dir=root/'Silicium-ACPI/Platforms/Teclast/t70pro'
memory_ranges=[]
for source_path in sorted(acpi_dir.glob('*.asl')):
    source_text=re.sub(r'//.*$', '', source_path.read_text(), flags=re.M)
    depth=0
    devices=[]
    pending_device=None
    for line in source_text.splitlines():
        for token in re.finditer(r'Device\s*\(\s*(\w+)\s*\)|[{}]',line):
            if token.group(1) is not None:
                pending_device=token.group(1)
            elif token.group(0)=='{':
                depth+=1
                if pending_device is not None:
                    devices.append((depth,pending_device))
                    pending_device=None
            else:
                while devices and devices[-1][0]==depth:
                    devices.pop()
                depth-=1
        owner=(source_path.name,tuple(name for _,name in devices))
        for match in re.finditer(
                r'Memory32Fixed\s*\(\s*(?:ReadOnly|ReadWrite)\s*,\s*(0x[0-9a-fA-F]+|[0-9]+)\s*,\s*(0x[0-9a-fA-F]+|[0-9]+)\s*\)',
                line, re.I):
            start=int(match.group(1),0)
            length=int(match.group(2),0)
            if length:
                access=match.group(0).partition('(')[2].partition(',')[0].strip()
                memory_ranges.append((start,start+length,owner,access))
memory_ranges.sort()

def acpi_overlap_allowed(left,right):
    if left[2]==right[2]: return True
    # NTC0's two read-only clock-status words are subranges of MFG0's
    # read/write INFRACFG window. Keep this existing provider/observer pair
    # explicit; any other cross-device overlap fails the package check.
    return (left[2]==('DSDT.asl',('MFG0',)) and
            left[0]==0x10001000 and left[1]==0x10002000 and
            right[2]==('DSDT.asl',('NTC0',)) and
            right[0] in (0x10001090,0x10001094) and
            right[1]==right[0]+4 and right[3]=='ReadOnly')

memory_ranges_disjoint=all(
    left[1] <= right[0] or acpi_overlap_allowed(left,right)
    for i,left in enumerate(memory_ranges)
    for right in memory_ranges[i+1:])
usb_handoff=next((r for r in memory_ranges if r[0]==0x43F7F100),None)
ccci_handoff=next((r for r in memory_ranges if r[0]==0x43F7F180),None)
checks={
 'net_setup_class':'Class=Net' in inf and 'ClassGuid={4D36E972-E325-11CE-BFC1-08002BE10318}' in inf,
 'acpi_hana6797_binding':'ACPI\\HANA6797' in inf and
     re.search(r'Device\s*\(DPM0\).*?Name\s*\(_HID,\s*\"HANA6797\"\)',asl,re.S) is not None,
 'dpm0_noncoherent_dma_declared':re.search(r'Device\s*\(DPM0\).*?Name\s*\(_CCA,\s*Zero\)',asl,re.S) is not None,
 'acpi_interfaces_enabled':all(re.search(
     r'Device\s*\('+name+r'\).*?Name\s*\(_STA,\s*0x0F\)',asl,re.S) is not None
     for name in ('DPM0','CCF0')),
 'mb_raw_ip_inf_contract':all(v in inf for v in (
     '*IfType=243','*MediaType=9','*PhysicalMediaType=8','EnableDhcp=0',
     'UpperRange,0,"flpp4, flpp6"','LowerRange,0,"ppip"')),
 'ndis_service_binding':all(v in inf for v in (
     'AddService=MtkWwan,2,ServiceInstall','ServiceType=1','StartType=3',
     'ServiceBinary=%12%\\MtkWwan.sys','LoadOrderGroup=NDIS')),
 'source_derived_dma_and_resources':all(v in driver for v in (
     'T70DpmaifMapResources','T70DpmaifWdmDmaInitialize',
     'T70DpmaifWdmAllocateCommonBuffer','T70DpmaifWdmFreeCommonBuffer',
     'T70DpmaifWdmAllocateCommonBufferVector',
     'T70DpmaifWdmGetCommonBufferVectorElement',
     'T70DpmaifWdmFreeCommonBufferVector','T70DpmaifBatPostInitialBuffers',
     'T70DpmaifQueueMemorySizes','T70DpmaifBuildQueueLayout',
     'WwInitializeQueueMemory','PitMemory','RequestBatMemory',
     'FragmentBatMemory','TxDrbMemory','RequestBatBuffers',
     'FragmentBatBuffers','RequestBatSlots','FragmentBatSlots',
     'WwInitializeRxBatPools','T70DpmaifBuildStartupPlan',
     'StartupPlanCount','StartupPlanBuilt',
     'T70CcciMemoryHandoffIsValid','CcciHandoffLength',
     'NdisMSendNetBufferListsComplete(Adapter->NdisHandle',
     'NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL','T70WwSendCompleteFlags',
     'T70WwCopyFromDeviceMemory',
     'NdisMGetDeviceProperty','NdisMMapIoSpace','T70WwZeroDeviceMemory','NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2',
     'NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2')),
 'dsdt_ssdt_memory32fixed_resources_do_not_conflict':(
     memory_ranges_disjoint and usb_handoff is not None and
     usb_handoff[1]-usb_handoff[0]==0x40 and
     ccci_handoff is not None and ccci_handoff[1]-ccci_handoff[0]==0x40 and
     'Memory32Fixed (ReadOnly, 0x43F7F180, 0x40)' in asl and
     'Memory32Fixed (ReadOnly, 0x43F7F100, 0x40)' in
         (acpi_dir/'DSDT.asl').read_text()),
 'firmware_reserves_md1_handoff':(
     'Memory32Fixed (ReadOnly, 0x43F7F180, 0x40)' in asl and
     'T70_CCCI_MEMORY_HANDOFF_ADDRESS' in driver and
     'PublishCcciMemoryHandoff (TRUE)' in memory_map and
     'T70CcciMemoryHandoffIsValid' in driver),
 'firmware_discovers_and_reserves_lk_md1':(
     'ParseCcciMdMemory' in memory_map and
     'RANGE_OWNER_MODEM_MEMORY' in memory_map and
     'mCcciTagReader (Base, InfoBytes' in memory_map and
     'ArmMmuEnabled ()' in memory_map and
     'LoadExistingCcciHandoff' in memory_map),
 'unsupported_device_caps_do_not_claim_modem_ready':re.search(
     r'case OID_WWAN_DEVICE_CAPS:.*?return NDIS_STATUS_NOT_SUPPORTED;',driver,re.S) is not None,
  "packet_filter_set_query_state_matches":(
     "T70WwPacketFilterStore(Value, &PacketFilter)" in driver and
     "Adapter->PacketFilter = (ULONG)PacketFilter;" in driver and
     "*Current = Requested;" in (here/"NdisCallbackCore.h").read_text()),
 'driver_model_not_advertised_until_complete':all(
     re.search(r'case '+oid+r':.*?return NDIS_STATUS_NOT_SUPPORTED;',driver,re.S)
     is not None for oid in ('OID_WWAN_DRIVER_CAPS','OID_WWAN_DEVICE_CAPS')),
}
failed=[name for name,passed in checks.items() if not passed]
if failed: raise SystemExit('MtkWwan package checks failed: '+', '.join(failed))
print('MtkWwan package checks passed: '+', '.join(checks))
