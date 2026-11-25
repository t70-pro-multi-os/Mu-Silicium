#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile and execute production MediaTek C with fake MMIO in the builder VM."""
from pathlib import Path
import json
import hashlib
import subprocess
import tempfile
import sys
import argparse
import platform
import runpy
import time

Darwin = sys.platform == "darwin"
Compiler = "clang" if Darwin else "gcc"

package = Path(__file__).resolve().parent
repo = package.parents[2]
includes = ["Mu_Basecore/MdePkg/Include", "Mu_Basecore/MdePkg/Include/AArch64",
            "Mu_Basecore/ArmPkg/Include", "Mu_Basecore/MdeModulePkg/Include",
            "Mu_Basecore/EmbeddedPkg/Include", "Mu_Basecore/CryptoPkg/Include",
            "Common/Mu/MsCorePkg/Include", "Silicon/Silicium/SiliciumPkg/Include", "Silicon/MediaTek/MediaTekPkg/Include"]
ufs_passthru_support = [repo / ("Mu_Basecore/MdePkg/Library/BaseLib/" + name)
                         for name in ("LinkedList.c", "Math64.c", "RShiftU64.c", "MultU64x32.c")]
ufs_handoff_support = [repo / ("Mu_Basecore/MdePkg/Library/BaseLib/" + name)
                       for name in ("LinkedList.c", "Math64.c", "DivU64x32.c", "MultU64x32.c")]
sd_dxe_support = [repo / "Mu_Basecore/MdePkg/Library/UefiLib/UefiLib.c"] + [
    repo / ("Mu_Basecore/MdePkg/Library/BaseLib/" + name)
    for name in ("LinkedList.c", "Math64.c", "DivU64x32.c", "MultU64x32.c", "String.c")]
usb_bus_support_dir = repo / "Mu_Basecore/MdeModulePkg/Bus/Usb/UsbBusDxe"
usb_bus_support = [usb_bus_support_dir / name for name in
                   ("UsbBus.c", "UsbUtility.c", "UsbHub.c", "UsbEnumer.c", "UsbDesc.c")]
usb_keyboard_dir = repo / "Mu_Basecore/MdeModulePkg/Bus/Usb/UsbKbDxe"
usb_keyboard_support = [usb_keyboard_dir / name for name in ("KeyBoard.c", "EfiKey.c")] + [
    repo / "Mu_Basecore/MdePkg/Library/UefiUsbLib" / name
    for name in ("UsbDxeLib.c", "Hid.c")] + [
    repo / "Mu_Basecore/MdePkg/Library/BaseLib/LinkedList.c"]
usb_storage_dir = repo / "Mu_Basecore/MdeModulePkg/Bus/Usb/UsbMassStorageDxe"
usb_storage_support = [usb_storage_dir / name for name in ("UsbMassBot.c", "UsbMassBoot.c")]
tests = ("WatchdogModePreservationTest", "UnavailableRtcTest", "DeviceBootManagerTest", "KeypadDeviceTest", "KeypadDxeTest", "GpioTest", "Gpio6768Test", "PmicWrapperTest", "PmicTest", "ClockTest", "MsdcPlatformTest", "MsdcPlatform6768Test", "MsdcTest", "SdDxeMediaTest", "RuntimeMemoryTest", "I2cCoreTest", "I2cDxeTest", "I2cTransportTest", "HimaxCoreTest", "HimaxDxeTest", "MtkStpCoreTest", "MtkBtifCoreTest", "MtkVcn28CoreTest", "MtkGnssCoreTest", "StockPowerCoreTest", "MtkGaugeCoreTest", "MtkThermalCoreTest", "MtkSc8989xOtgCoreTest", "MtkHusb311TypecCoreTest", "StockPowerReadOnlyDxeTest", "BootStatusDxeTest", "LinuxBootTest", "LinuxLidTest", "I2cPlatformTest", "GpioPlatformTest", "MusbHostCoreTest", "MusbHostDxeTest", "MusbKeyboardPathTest", "MusbStoragePathTest", "MtkMusbPlatformCoreTest", "MusbHandoffDxeTest", "MtkUsbHubCoreTest", "MtkUsbPhyCoreTest", "MtkUsbPhyPlatformTest", "MtkUfsCoreTest", "MtkUfsTransportTest", "MtkUfsPlatformTest", "MtkUfsPowerCoreTest", "MtkPmicBrokerTest", "DisplayCoreTest", "MtkPanelCoreTest", "MtkDsiCoreTest", "MtkDsiDxeTest", "FramebufferGopDxeTest", "MtkUfsDxeTest", "MtkUfsHandoffDxeTest", "UfsPassThruTest")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--tests", nargs="+", choices=tests, help="run only selected programs; do not replace the full-suite report")
parser.add_argument("--output", type=Path, help="write this selected run to its purpose-specific analysis report")
parser.add_argument("--display-lk", action="store_true", help="compare DisplayCoreTest with exact LK OVL ARM64 output; requires Unicorn")
parser.add_argument("--macos-low32-x86", action="store_true",
                    help="run MtkUfsDxeTest as macOS x86_64 with low PAGEZERO under existing Rosetta; retain the production 32-bit DMA ceiling")
args = parser.parse_args()
if args.output and not args.tests:
    parser.error("--output requires a selected --tests run")
if args.output and args.output.resolve().parent != (repo.parent / "analysis").resolve():
    parser.error("--output must be a file in this project analysis directory")
if args.macos_low32_x86 and (not Darwin or (args.tests is not None and "MtkUfsDxeTest" not in args.tests)):
    parser.error("--macos-low32-x86 requires macOS and MtkUfsDxeTest")

if args.display_lk and args.tests and "DisplayCoreTest" not in args.tests:
    parser.error("--display-lk requires DisplayCoreTest")
def source_hashes():
    return {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
      for p in list((repo / "Silicon/MediaTek/MediaTekPkg/Drivers").rglob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsHandoffDxe").glob("*.h"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe").glob("*.h"))
      + list((package / "Library/MemoryMapLib").glob("*.c"))
      + list((package / "Library/UnavailableRealTimeClockLib").glob("*.[ch]"))
      + [repo / "Mu_Basecore/EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.c",
         repo / "Mu_Basecore/EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClock.c"]
      + list((package / "Library/DeviceBootManagerLib").glob("*.c"))
      + list((package / "Library/KeypadDeviceLib").glob("*.c"))
      + list((repo / "Silicon/Silicium/SiliciumPkg/Drivers/KeypadDxe").glob("*.c"))
      + list((repo / "Silicon/Silicium/SiliciumPkg/Drivers/KeypadDxe").glob("*.h"))
      + list((repo / "Silicon/Silicium/SiliciumPkg/Drivers/KeypadDeviceDxe").glob("*.c"))
      + [repo / "Silicon/Silicium/SiliciumPkg/Include/Library/KeypadDeviceLib.h",
         repo / "Silicon/Silicium/SiliciumPkg/Include/Protocol/EfiKeypadDevice.h",
         repo / "Silicon/Silicium/SiliciumPkg/Include/Configuration/BootDevices.h"]
      + list((package / "Library/MtkUsbPhyPlatformLib").glob("*.c"))
      + list((package / "Drivers").rglob("*.c"))
      + list((package / "Applications/LinuxBoot").glob("*.c"))
      + list((package / "Applications/LinuxBoot").glob("*.h"))
      + list((repo / "Silicon/MediaTek/MT6789Pkg/Library/GpioImplLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MT6768Pkg/Library/GpioImplLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MT6789Pkg/Library/ClockImplLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkI2cCoreLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/HimaxCoreLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/UfsCoreLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/UfsPowerLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib").glob("*.c"))
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Library/FramebufferCoreLib").glob("*.c"))
      + [repo / ("Silicon/MediaTek/" + chip + "Pkg/Library/MsdcImplLib/MsdcImplLib.c")
         for chip in ("MT6768", "MT6789")]
      + list((repo / "Silicon/MediaTek/MediaTekPkg/Include").rglob("*.h"))
      + [repo / "Mu_Basecore/MdeModulePkg/Bus/Sd/SdDxe" / name
         for name in ("SdDxe.c", "SdDxe.h", "SdBlockIo.c", "SdDiskInfo.c", "SdDxe.inf")]
      + list((repo / "Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsPassThruDxe").glob("*.c"))
      + list((repo / "Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsPassThruDxe").glob("*.h"))
      + [package / "Tests/verify-display-binary.py"]
      + list((package / "Tests").rglob("*.h"))
      + list((package / "Tests").rglob("*Test.c"))
      + ufs_passthru_support + ufs_handoff_support + sd_dxe_support + usb_bus_support
      + list(usb_bus_support_dir.glob("*.h"))
      + usb_keyboard_support + list(usb_keyboard_dir.glob("*.h"))
      + usb_storage_support + list(usb_storage_dir.glob("*.h"))
      + [repo / "Mu_Basecore/MdePkg/Library/UefiUsbLib/UefiUsbLibInternal.h"]
      + [repo / "Mu_Basecore/MdePkg/Library/UefiLib/UefiLibInternal.h"]
      + list((repo / "Mu_Basecore/MdePkg/Library/BaseFdtLib").rglob("*.c"))
      + list((repo / "Mu_Basecore/MdePkg/Library/BaseFdtLib").rglob("*.h"))
      + [repo / "Mu_Basecore/MdePkg/Library/BaseLib/BaseLibInternals.h",
         repo / "Mu_Basecore/MdeModulePkg/Include/Protocol/UfsHostControllerPlatform.h"]}

results = []
lk_ovl = None
frozen_source_sha256 = source_hashes()
frozen_runner_sha256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
frozen_stock_dtb_sha256 = hashlib.sha256((repo.parent/"analysis/stock-fdt/effective.dtb").read_bytes()).hexdigest()
try:
    with tempfile.TemporaryDirectory(prefix="mediatek-test-", dir=None if Darwin else repo / "Build") as temp:
        for name in (args.tests or tests):
            exe = Path(temp) / name
            cmd = [Compiler, "-std=gnu11", "-O1", "-g", "-fshort-wchar", "-DMDEPKG_NDEBUG",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-ffunction-sections", "-fdata-sections", "-Wl,-dead_strip" if Darwin else "-Wl,--gc-sections", "-include", "Base.h"]
            execution_arch = platform.machine()
            translated = bool(args.macos_low32_x86 and name == "MtkUfsDxeTest")
            if translated:
                # A native arm64 Mach-O cannot map this identity-DMA fixture
                # below 4 GiB. Keep the real DMA bound and use a separately
                # identified x86_64 test process; never change firmware limits.
                cmd += ["-arch", "x86_64", "-Wl,-pagezero_size,0x10000"]
                execution_arch = "x86_64"
            cmd += ["-I" + str(repo / inc) for inc in includes]
            source = {"Gpio6768Test":"GpioTest", "SdDxeMediaTest":"Sd/SdDxeMediaTest", "MsdcPlatform6768Test":"MsdcPlatformTest", "UfsPassThruTest":"Ufs/UfsPassThruTest", "MtkUfsCoreTest":"Ufs/MtkUfsCoreTest", "MtkUfsTransportTest":"Ufs/MtkUfsTransportTest", "MtkUfsPlatformTest":"Ufs/MtkUfsPlatformTest", "MtkUfsPowerCoreTest":"Ufs/MtkUfsPowerCoreTest", "MtkPmicBrokerTest":"Ufs/MtkPmicBrokerTest", "MtkUfsDxeTest":"Ufs/MtkUfsDxeTest", "MtkUfsHandoffDxeTest":"Ufs/MtkUfsHandoffDxeTest", "DisplayCoreTest":"Display/DisplayCoreTest", "MtkPanelCoreTest":"Display/MtkPanelCoreTest", "MtkDsiCoreTest":"Display/MtkDsiCoreTest", "MtkDsiDxeTest":"Display/MtkDsiDxeTest", "FramebufferGopDxeTest":"Display/FramebufferGopDxeTest"}.get(name,name)
            cmd += ["-I"+str(package/"Tests"), "-I"+str(repo/"Silicon/MediaTek/MediaTekPkg/Drivers/MusbHostDxe")]
            if name in ("MusbHostDxeTest", "MusbKeyboardPathTest", "MusbStoragePathTest"):
                cmd += ["-include", str(package / "Tests/UsbBusTestAutoGen.h"),
                        "-I" + str(usb_bus_support_dir)]
                support_sources = usb_bus_support
                if name == "MusbKeyboardPathTest":
                    cmd += ["-I" + str(usb_keyboard_dir),
                            "-D_PCD_GET_MODE_BOOL_PcdDisableDefaultKeyboardLayoutInUsbKbDriver=0",
                            "-D_PCD_GET_MODE_32_PcdUsbTransferTimeoutValue=3000U",
                            "-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=0U"]
                    support_sources = usb_bus_support + usb_keyboard_support
                if name == "MusbStoragePathTest":
                    cmd += ["-DT70PRO_RAM_DIAGNOSTICS", "-I" + str(usb_storage_dir),
                            "-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=0U"]
                    support_sources = usb_bus_support + usb_storage_support + [
                        repo / "Mu_Basecore/MdePkg/Library/BaseLib" / name
                        for name in ("Math64.c", "MultU64x32.c", "LinkedList.c")]
                    support_sources.append(
                        repo / "Mu_Basecore/MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLib.c")
                objects = []
                for support in support_sources:
                    obj = Path(temp) / (support.stem + ".o")
                    # Unused USB protocol globals retain the whole bus driver
                    # through ASan registration. Keep heap/stack/UB checks in
                    # these objects; the MUSB/test globals retain full ASan.
                    global_gc = (["-mllvm", "-asan-globals=0"] if Darwin else
                                 ["--param=asan-globals=0"])
                    subprocess.run(cmd + global_gc + ["-c", str(support), "-o", str(obj)],
                                   check=True, capture_output=True, text=True)
                    objects.append(str(obj))
                cmd += objects
            if name == "UfsPassThruTest":
                cmd += ["-I" + str(repo / "Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsPassThruDxe")]
                cmd += ["-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=0U"]
                cmd += [str(source_file) for source_file in ufs_passthru_support]
            if name == "KeypadDxeTest":
                cmd += [str(repo / "Mu_Basecore/MdePkg/Library/BaseLib/LinkedList.c"),
                        "-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=0U"]
            if name == "SdDxeMediaTest":
                cmd += [str(source_file) for source_file in sd_dxe_support]
                cmd += ["-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=0U",
                        "-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0U",
                        "-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0U"]
            if name == "MsdcPlatform6768Test":
                cmd += ["-DTEST_MT6768"]
            if name == "Gpio6768Test":
                cmd += ["-DTEST_MT6768"]
            arguments = []
            if name in ("MtkUfsDxeTest", "MtkUfsHandoffDxeTest"):
                cmd += ["-D_GNU_SOURCE", "-fno-strict-aliasing"]
            if name in ("I2cCoreTest", "I2cTransportTest"):
                cmd += [str(repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkI2cCoreLib/MtkI2cCore.c")]
            if name in ("HimaxCoreTest", "HimaxDxeTest", "GpioPlatformTest"):
                cmd += [str(repo / "Silicon/MediaTek/MediaTekPkg/Library/HimaxCoreLib/HimaxCore.c")]
            if name in ("MtkStpCoreTest", "MtkBtifCoreTest"):
                cmd += ["-DSTP_TEST_NATIVE", "-Wall", "-Wextra", "-Werror",
                        str(repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkStpCore.c")]
                if name == "MtkBtifCoreTest":
                    cmd += [str(repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkBtifCore.c")]
            if name == "MtkGnssCoreTest":
                cmd += ["-DSTP_TEST_NATIVE", "-Wall", "-Wextra", "-Werror",
                        str(repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkGnssCore.c"),
                        str(repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkStpCore.c")]
            if name == "MtkVcn28CoreTest":
                cmd += ["-DSTP_TEST_NATIVE", "-Wall", "-Wextra", "-Werror",
                        str(repo / "Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkVcn28Core.c")]
            if name in ("StockPowerCoreTest", "StockPowerReadOnlyDxeTest", "MtkSc8989xOtgCoreTest", "MtkHusb311TypecCoreTest", "MusbHandoffDxeTest"):
                cmd += [str(repo / "Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/StockPowerCore.c")]
            if name in ("MtkSc8989xOtgCoreTest", "MusbHandoffDxeTest"):
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkSc8989xOtgCore.c")]
            if name in ("MtkHusb311TypecCoreTest", "MusbHandoffDxeTest"):
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkHusb311TypecCore.c")]
            if name == "MtkThermalCoreTest":
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkThermalCore.c")]
            if name in ("MtkGaugeCoreTest", "PmicWrapperTest"):
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkGaugeCore.c")]
            if name == "MtkGaugeCoreTest":
                cmd += ["-Wall", "-Wextra", "-Werror"]
            if name == "MtkSc8989xOtgCoreTest":
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MtkMusbPlatformCore.c")]
            if name in ("MtkUsbPhyCoreTest", "MtkUsbPhyPlatformTest", "MusbHandoffDxeTest"):
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MtkUsbPhyCore.c")]
            if name == "MusbHandoffDxeTest":
                cmd += [str(package/"Library/MtkUsbPhyPlatformLib/MtkUsbPhyPlatform.c")]
            if name == "MtkUsbHubCoreTest":
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MtkUsbHubCore.c")]
            if name in ("MtkMusbPlatformCoreTest", "MusbHandoffDxeTest"):
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MtkMusbPlatformCore.c")]
            if name in ("MusbHostCoreTest", "MusbHostDxeTest", "MusbKeyboardPathTest", "MusbStoragePathTest"):
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MusbHostCore.c")]
            if name in ("MtkUfsCoreTest", "MtkUfsTransportTest", "MtkUfsDxeTest", "MtkUfsHandoffDxeTest"):
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Library/UfsCoreLib/MtkUfsCore.c")]
            if name == "MtkPmicBrokerTest":
                cmd += ["-Wall", "-Wextra", "-Werror",
                        str(repo/"Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsPrm/MtkPmicBroker.c")]
            if name == "MtkUfsPowerCoreTest":
                cmd += ["-Wall", "-Wextra", "-Werror",
                        str(repo/"Silicon/MediaTek/MediaTekPkg/Library/UfsPowerLib/MtkUfsPowerCore.c")]
            if name == "MtkUfsTransportTest":
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Library/UfsCoreLib/MtkUfsTransport.c")]
            if name == "MtkUfsHandoffDxeTest":
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsHandoffDxe/MtkUfsColdHost.c")]
                cmd += [str(source_file) for source_file in ufs_handoff_support]
                cmd += ["-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=0U"]
            if name in ("MtkUfsPlatformTest", "MtkUfsHandoffDxeTest"):
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Library/UfsCoreLib/MtkUfsPlatform.c")]
            if name in ("MtkDsiCoreTest", "MtkDsiDxeTest"):
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/FramebufferCoreLib/MtkDsiCore.c")]
            if name in ("MtkPanelCoreTest", "GpioPlatformTest"):
                cmd += ["-Wall", "-Wextra", "-Werror", str(repo/"Silicon/MediaTek/MediaTekPkg/Library/FramebufferCoreLib/MtkPanelCore.c")]
            if name in ("DisplayCoreTest", "FramebufferGopDxeTest", "RuntimeMemoryTest"):
                cmd += [str(repo/"Silicon/MediaTek/MediaTekPkg/Library/FramebufferCoreLib/MtkDisplayCore.c")]
            if name in ("UnavailableRtcTest", "MtkDsiDxeTest", "RuntimeMemoryTest", "BootStatusDxeTest", "LinuxLidTest", "I2cPlatformTest", "GpioPlatformTest", "DisplayCoreTest", "FramebufferGopDxeTest", "MtkUfsHandoffDxeTest", "MtkUsbPhyPlatformTest", "MusbHandoffDxeTest"):
                # Host LP64 uint64_t is unsigned long; UEFI UINT64 is unsigned
                # long long. Both are 64 bits; match firmware no-strict-aliasing.
                cmd += ["-fno-strict-aliasing"]
                if name != "MtkUfsHandoffDxeTest":
                    cmd += ["-Wno-error=incompatible-pointer-types"]
                fdt = repo / "Mu_Basecore/MdePkg/Library/BaseFdtLib"
                libfdt = fdt / "libfdt/libfdt"
                cmd += ["-idirafter", str(fdt), "-I" + str(libfdt), str(fdt / "FdtLib.c")]
                cmd += [str(libfdt / n) for n in ("fdt.c", "fdt_ro.c", "fdt_check.c", "fdt_addresses.c")]
                if name in ("DisplayCoreTest", "FramebufferGopDxeTest", "RuntimeMemoryTest", "GpioPlatformTest", "LinuxLidTest", "MtkUfsHandoffDxeTest", "MtkUsbPhyPlatformTest", "MusbHandoffDxeTest"):
                    cmd += [str(libfdt/n) for n in ("fdt_empty_tree.c", "fdt_rw.c", "fdt_sw.c")]
                if name in ("LinuxLidTest", "DisplayCoreTest"):
                    cmd += [str(libfdt / "fdt_wip.c")]
                if name == "BootStatusDxeTest":
                    cmd += [str(package / "Drivers/BootStatusDxe/BootStatusDxe.c")]
                fixture = Path(temp) / "runtime.dtb"
                dts = b"""/dts-v1/;
/memreserve/ 0x50000001 0x1001;
/ { #address-cells=<2>; #size-cells=<2>;
 memory@40000000 { device_type="memory"; reg=<0 0x40000000 0 0x80000000 1 0 1 0>; };
 reserved-memory { #address-cells=<2>; #size-cells=<2>; ranges;
   modem@60000000 { reg=<0 0x60000000 0 0x10000000>; no-map; };
   cma@a0000000 { reg=<0 0xa0000000 0 0x10000000>; reusable; };
   unused { status="disabled"; size=<0 0x100000>; };
 };
 modem { ccci,modem_info=[00 01 40 00 01 00 00 00 00 10 00 00 02 00 00 00]; };
};"""
                if name == "BootStatusDxeTest":
                    subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(fixture)],
                                   input=dts, check=True, capture_output=True)
                arguments = [str(repo.parent/"analysis/stock-fdt/effective.dtb") if name in ("MtkDsiDxeTest","I2cPlatformTest","LinuxLidTest","GpioPlatformTest","DisplayCoreTest", "MtkUfsHandoffDxeTest", "MtkUsbPhyPlatformTest", "MusbHandoffDxeTest") else str(fixture)]
                if name in ("UnavailableRtcTest", "FramebufferGopDxeTest", "RuntimeMemoryTest"): arguments = []
            if name == "MtkThermalCoreTest":
                arguments = [str(repo.parent/"analysis/stock-fdt/effective.dtb")]
            if name == "DisplayCoreTest" and args.display_lk:
                fixture = Path(temp) / "lk-ovl.bin"
                helper = runpy.run_path(str(package / "Tests/verify-display-binary.py"))
                lk_ovl = helper["write_lk_ovl_fixtures"](fixture)
                arguments.append(str(fixture))
            cmd += [str(package / "Tests" / (source + ".c")), "-o", str(exe)]
            compilation = subprocess.run(cmd, check=True, capture_output=True, text=True)
            started = time.monotonic()
            try:
                result = subprocess.run([str(exe)] + arguments, capture_output=True, text=True,
                                        timeout=30)
            except subprocess.TimeoutExpired as error:
                for part in (error.stdout, error.stderr):
                    if part:
                        print(part.decode(errors="replace") if isinstance(part, bytes) else part,
                              file=sys.stderr, flush=True)
                raise
            elapsed_seconds = time.monotonic() - started
            if result.returncode:
                print(result.stdout, result.stderr, file=sys.stderr, flush=True)
                result.check_returncode()
            output = "\n".join(part.strip() for part in (result.stdout, result.stderr) if part.strip())
            pll_traces = []
            cold_host_traces = []
            visible_lines = []
            for line in output.splitlines():
                if line.startswith("PLL_TRACE_JSON "):
                    if name != "ClockTest":
                        raise ValueError("PLL trace emitted by an unexpected program")
                    pll_traces.append(json.loads(line[len("PLL_TRACE_JSON "):]))
                elif line.startswith("MUSB_COLD_TRACE_JSON "):
                    if name != "MtkMusbPlatformCoreTest":
                        raise ValueError("Cold host trace emitted by an unexpected program")
                    cold_host_traces.append(json.loads(line[len("MUSB_COLD_TRACE_JSON "):]))
                else:
                    visible_lines.append(line)
            output = "\n".join(visible_lines)
            print(output, flush=True)
            item = {"test": name, "status": "passed", "output": output, "elapsed_seconds": elapsed_seconds,
                    "execution_architecture": execution_arch, "translated_x86_64_process": translated}
            compiler_output = "\n".join(part.strip() for part in (compilation.stdout, compilation.stderr) if part.strip())
            if compiler_output:
                item["compiler_output"] = compiler_output
            if name == "ClockTest":
                if len(pll_traces) != 14 or [row["id"] for row in pll_traces] != list(range(14)):
                    raise ValueError("Actual C PLL trace set is incomplete")
                item["pll_traces"] = pll_traces
            if name == "MtkMusbPlatformCoreTest":
                if len(cold_host_traces) != 1:
                    raise ValueError("Actual C cold host trace is missing or duplicated")
                trace = cold_host_traces[0]
                if len(trace.get("trial101_physical_response_replays", [])) != 12:
                    raise ValueError("Actual core physical POWER replay set is incomplete")
                item["cold_host_trace"] = trace
            results.append(item)
    final_source_sha256 = source_hashes()
    changed = sorted(name for name in set(frozen_source_sha256) | set(final_source_sha256)
                     if frozen_source_sha256.get(name) != final_source_sha256.get(name))
    if changed:
        raise RuntimeError("Test inputs changed during execution: " + ", ".join(changed))
    if hashlib.sha256(Path(__file__).read_bytes()).hexdigest() != frozen_runner_sha256:
        raise RuntimeError("Test runner changed during execution")
    if hashlib.sha256((repo.parent/"analysis/stock-fdt/effective.dtb").read_bytes()).hexdigest() != frozen_stock_dtb_sha256:
        raise RuntimeError("Stock DTB changed during execution")
    report_path = args.output or (repo.parent / ("analysis/mediatek-driver-selected-tests.json" if args.tests else "analysis/mediatek-driver-tests.json"))
    report_path.write_text(json.dumps({
        "scope": ("production C, host " + platform.system() + " " + platform.machine() +
                  ("; MtkUfsDxeTest is an explicitly requested x86_64 Rosetta process" if args.macos_low32_x86 else "") +
                  ", fake MMIO; no tablet execution"),
        "compiler": subprocess.check_output([Compiler, "--version"], text=True).splitlines()[0],
        "sanitizers": ["address", "undefined"], "results": results,
        "lk_ovl": lk_ovl,
        "runner_sha256": frozen_runner_sha256,
        "stock_dtb_sha256": frozen_stock_dtb_sha256,
        "source_sha256": frozen_source_sha256,
        "inputs_unchanged_during_execution": True}, indent=2) + "\n")
except subprocess.CalledProcessError as error:
    print(f"{Path(error.cmd[0]).name} exited with status {error.returncode}", file=sys.stderr)
    print(error.stdout or "", file=sys.stderr)
    print(error.stderr or "", file=sys.stderr)
    sys.exit(1)
