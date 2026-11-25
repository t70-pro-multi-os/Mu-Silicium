#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check CPU PEP service lifetime and early PPM registration policy."""
from pathlib import Path
import re

here=Path(__file__).resolve().parent
inf=(here/'MtkCpu.inf').read_text()
driver=(here/'Driver.c').read_text()
service=inf.split('[Service]',1)[1].split('[',1)[0]
checks={
    'boot_start_pep': 'StartType=0' in service and
        'LoadOrderGroup=System Bus Extender' in service,
    'irreversible_pep_registration_pins_image':
        re.search(r'PoFxRegisterPluginEx\(&PepInformation.*?Driver->DriverUnload\s*=\s*NULL',
                  driver,re.S) is not None,
    'capability_query_before_mmio_domain_read':
        driver.index('if (Notification == PEP_NOTIFY_PPM_QUERY_CAPABILITIES)') <
        driver.index('if (!CpuPpmBuildDomainsLocked(CpuPpmDevice, Domains))'),
    'performance_query_has_bootstrap_range_before_prepare':
        'Notification == PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES &&' in driver and
        'CpuPpmDevice == NULL' in driver and
        driver.index('CpuPpmDevice == NULL') <
        driver.index('if (!CpuPpmBuildDomainsLocked(CpuPpmDevice, Domains))'),
    'kernel_callbacks_retained_for_late_constraints_refresh':
        'CpuPpmKernelInformation = KernelInformation;' in driver and
        'CpuPpmKernelInformation.RequestWorker(KernelHandle)' in driver and
        'CpuPpmPerfConstraintChange' in driver,
    'ppm_requests_use_bounded_selector':
        'T70CpuPpmSelect(&Domains[Domain], Minimum,' in driver and
        'Request->DesiredPerformance, Request->MaximumPerformance' in driver,
}
failed=[name for name,passed in checks.items() if not passed]
if failed: raise SystemExit('MtkCpu package checks failed: '+', '.join(failed))
print('MtkCpu package checks passed: '+', '.join(checks))
