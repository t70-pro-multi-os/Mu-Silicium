# SPDX-License-Identifier: BSD-2-Clause-Patent
# Compile the actual board ACPI tables without a guessed RAM layout or firmware FD.
[Defines]
  PLATFORM_NAME = T70ProAcpi
  PLATFORM_GUID = 93BB07EC-1CF2-4A5A-806F-27E4C72C39D3
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x00010005
  OUTPUT_DIRECTORY = Build/t70proPkg
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = DEBUG|RELEASE
  SKUID_IDENTIFIER = DEFAULT

[LibraryClasses]
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf

[PcdsFixedAtBuild]
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0C000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C040000
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x202039383736544D
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x6789
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorId|0x4C495350
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorRevision|1

[Components]
  t70pro/AcpiTables.inf
