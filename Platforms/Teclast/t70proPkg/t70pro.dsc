# SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME = t70pro
  PLATFORM_GUID = A16813B1-6789-4B71-90DF-D68D15D18156
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x00010005
!ifndef T70_BUILD_OUTPUT_DIRECTORY
  DEFINE T70_BUILD_OUTPUT_DIRECTORY = Build/t70proPkg
!endif
  OUTPUT_DIRECTORY = $(T70_BUILD_OUTPUT_DIRECTORY)
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = RELEASE
  SKUID_IDENTIFIER = DEFAULT
  FLASH_DEFINITION = t70proPkg/t70pro.fdf
  DEFINE ENABLE_SECUREBOOT = 0
  DEFINE USE_PHYSICAL_TIMER = 1
  DEFINE USE_CUSTOM_DISPLAY_DRIVER = 1
  DEFINE FD_BASE = 0x44000000
  DEFINE FD_SIZE = 0x00400000
  DEFINE FD_BLOCKS = 0x400

!include SiliciumPkg/SiliciumPkg.dsc.inc

[BuildOptions]
  *_CLANGPDB_AARCH64_CC_FLAGS = -DT70PRO_RAM_DIAGNOSTICS
  *_CLANGPDB_AARCH64_CC_FLAGS = -march=armv8.2-a+crypto+rcpc
  # Firmware and XNU module builds share these objects on both hosts.
  # Keep one COFF format for the linker and final-PE CodeView verification.
  *_CLANGPDB_AARCH64_CC_FLAGS = -fno-lto

[PcdsFixedAtBuild]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x43F80000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x80000
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0C000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C040000
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x202039383736544D
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x6789
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiTableStorageFile|{0xAB,0xE1,0x48,0xDC,0x92,0xED,0xB3,0x4C,0xA1,0x2F,0x81,0x49,0x61,0x91,0x87,0xEF}

  # Internal LU2 BlockIo is read-only; board dependencies are included in FV.
  gMediaTekPkgTokenSpaceGuid.PcdStorageIsEMMC|FALSE
  gMediaTekPkgTokenSpaceGuid.PcdMsdcHostMask|2
  gMediaTekPkgTokenSpaceGuid.PcdMsdcReadOnly|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSdDxeReadOnly|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSdDxeSynchronousPassThru|TRUE
  gMediaTekPkgTokenSpaceGuid.PcdMsdcMaxClockHz|25000000
  gMediaTekPkgTokenSpaceGuid.PcdSdDetectPin|7
  gMediaTekPkgTokenSpaceGuid.PcdSdDetectActiveLow|FALSE
  gMediaTekPkgTokenSpaceGuid.PcdSdVmchEint|1
  gMediaTekPkgTokenSpaceGuid.PcdMt6358GaugeSnapshot|TRUE

[LibraryClasses]
  # No qualified MT6358 RTC provider: never report uptime as wall-clock time.
  RealTimeClockLib|t70proPkg/Library/UnavailableRealTimeClockLib/UnavailableRealTimeClockLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  PrmContextBufferLib|PrmPkg/Library/DxePrmContextBufferLib/DxePrmContextBufferLib.inf
  PrmModuleDiscoveryLib|PrmPkg/Library/DxePrmModuleDiscoveryLib/DxePrmModuleDiscoveryLib.inf
  PrmPeCoffLib|PrmPkg/Library/DxePrmPeCoffLib/DxePrmPeCoffLib.inf
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  DeviceBootManagerLib|t70proPkg/Library/DeviceBootManagerLib/DeviceBootManagerLib.inf
  KeypadDeviceLib|t70proPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf
  MtkDisplayCoreLib|MediaTekPkg/Library/FramebufferCoreLib/FramebufferCoreLib.inf
  MusbHostCoreLib|MediaTekPkg/Library/MusbHostCoreLib/MusbHostCoreLib.inf
  MtkUsbPhyPlatformLib|t70proPkg/Library/MtkUsbPhyPlatformLib/MtkUsbPhyPlatformLib.inf
  MtkUfsCoreLib|MediaTekPkg/Library/UfsCoreLib/MtkUfsCoreLib.inf
  MtkUfsPowerCoreLib|MediaTekPkg/Library/UfsPowerLib/MtkUfsPowerCoreLib.inf
  StockPowerCoreLib|MediaTekPkg/Library/StockPowerCoreLib/StockPowerCoreLib.inf
  HimaxCoreLib|MediaTekPkg/Library/HimaxCoreLib/HimaxCoreLib.inf
  MtkI2cCoreLib|MediaTekPkg/Library/MtkI2cCoreLib/MtkI2cCoreLib.inf
  GpioImplLib|MT6789Pkg/Library/GpioImplLib/GpioImplLib.inf
  ClockImplLib|MT6789Pkg/Library/ClockImplLib/ClockImplLib.inf
  PmicWrapperImplLib|MT6789Pkg/Library/PmicWrapperImplLib/PmicWrapperImplLib.inf
  MsdcImplLib|MT6789Pkg/Library/MsdcImplLib/MsdcImplLib.inf
  MemoryMapLib|t70proPkg/Library/MemoryMapLib/MemoryMapLib.inf
  PlatformSecLib|t70proPkg/Library/PlatformSecLib/PlatformSecLib.inf

[Components]
  t70proPkg/Drivers/T70PlatformLogoDxe/T70PlatformLogoDxe.inf
!ifdef T70_DEVICE_CHECK
  t70proPkg/Applications/DeviceCheck/T70DeviceCheck.inf
!endif
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf {
    <LibraryClasses>
      CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  }
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
      FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  }
  ShellPkg/Application/Shell/Shell.inf {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiShellPkgTokenSpaceGuid.PcdShellProfileMask|0
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
  }
!ifdef T70_XNU_BOOT
  # Standalone modules only; no firmware auto-boot or persistent boot entry.
  t70proPkg/Applications/XnuBoot/XnuBoot.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
!ifdef T70_XNU_QEMU_TEST
  t70proPkg/Tests/QemuXnu/QemuXnu.inf
!endif
!endif
  # ARM64 runtime UFS supplier; Windows invokes the fixed PRM handler.
  PrmPkg/PrmConfigDxe/PrmConfigDxe.inf
  PrmPkg/PrmLoaderDxe/PrmLoaderDxe.inf
  MediaTekPkg/Drivers/MtkUfsPrm/MtkUfsPrm.inf
  # UFS transport and SCSI bus feed only the board read-only BlockIo adapter.
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  # Physical buttons feed the same UEFI console as the USB keyboard.
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  # Standard USB pointer driver; bus/keyboard are in SiliciumPkg.dsc.inc.
  MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf
  MediaTekPkg/Drivers/FramebufferGopDxe/FramebufferGopDxe.inf
  MediaTekPkg/Drivers/MusbHostDxe/MusbHostDxe.inf
  t70proPkg/Drivers/MusbHandoffDxe/MusbHandoffDxe.inf
  # Mali-G57 power record; T70_GPU_POWER_ON also powers the GPU at ReadyToBoot.
  t70proPkg/Drivers/GpuPowerDxe/GpuPowerDxe.inf {
    <BuildOptions>
!ifdef T70_GPU_POWER_ON
      *_*_*_CC_FLAGS = -DT70_GPU_POWER_ON=1
!endif
  }
  t70proPkg/Drivers/I2cPlatformDxe/I2cPlatformDxe.inf
  t70proPkg/Drivers/GpioPlatformDxe/GpioPlatformDxe.inf
  t70proPkg/Drivers/DsiPlatformDxe/DsiPlatformDxe.inf
  MediaTekPkg/Drivers/MtkUfsHandoffDxe/MtkUfsHandoffDxe.inf
  MediaTekPkg/Drivers/MtkUfsReadOnlyBlockIoDxe/MtkUfsReadOnlyBlockIoDxe.inf
  t70proPkg/Applications/LinuxBoot/LinuxBoot.inf {
    <LibraryClasses>
      BaseCryptLib|OpensslPkg/Library/BaseCryptLib/BaseCryptLib.inf
      OpensslLib|OpensslPkg/Library/OpensslLib/OpensslLibCrypto.inf
      IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
  t70pro/AcpiTables.inf
  t70proPkg/Drivers/BootStatusDxe/BootStatusDxe.inf
  MediaTekPkg/Drivers/HimaxTouchDxe/HimaxTouchDxe.inf
  MediaTekPkg/Drivers/StockPowerReadOnlyDxe/StockPowerReadOnlyDxe.inf
  # Board dependencies below are dispatched; internal storage stays read-only.
  MediaTekPkg/Drivers/GpioDxe/GpioDxe.inf
  MediaTekPkg/Drivers/ClockDxe/ClockDxe.inf
  MediaTekPkg/Drivers/PmicWrapperDxe/PmicWrapperDxe.inf
  MediaTekPkg/Drivers/MT6358PmicDxe/MT6358PmicDxe.inf
  MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.inf
  MediaTekPkg/Drivers/I2cDxe/I2cDxe.inf {
    <LibraryClasses>
      DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
      # DT-validated I2C APDMA routes are physical; no IOMMU producer exists.
      IoMmuLib|MdeModulePkg/Library/IoMmuLibNull/IoMmuLibNull.inf
  }
