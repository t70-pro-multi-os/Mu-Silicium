#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
if [[ ${1:-} != --locked ]]; then
  exec "$repo/Resources/Scripts/with-build-lock" T70Pro "Build T70Pro resources" -- bash "$0" --locked "$@"
fi
shift
target=${1:-acpi}
mode=${2:-DEBUG}
module_args=()
host_args=()
xnu_args=()
xnu_os_args=()
xnu_stage=XnuStage
case "$target" in
  acpi) platform=Acpi.dsc ;;
  firmware|firmware-trial) platform=t70pro.dsc; mode=RELEASE ;;
  module)
    platform=t70pro.dsc; mode=RELEASE
    [[ ${2:-} == MediaTekPkg/Drivers/*.inf || ${2:-} == t70proPkg/Drivers/*.inf ||
       ${2:-} == PrmPkg/PrmConfigDxe/PrmConfigDxe.inf ||
       ${2:-} == PrmPkg/PrmLoaderDxe/PrmLoaderDxe.inf ]] || { echo "Provide a MediaTek, T70Pro or configured PRM driver module" >&2; exit 2; }
    module_args=(-m "$2") ;;
  devicecheck) platform=t70pro.dsc; mode=RELEASE; module_args=(-m t70proPkg/Applications/DeviceCheck/T70DeviceCheck.inf) ;;
  linuxboot) platform=t70pro.dsc; mode=RELEASE; module_args=(-m t70proPkg/Applications/LinuxBoot/LinuxBoot.inf) ;;
  xnuboot|xnuboot-macos) platform=t70pro.dsc; mode=RELEASE; xnu_args=(-D T70_XNU_BOOT=TRUE); module_args=(-m t70proPkg/Applications/XnuBoot/XnuBoot.inf) ;;
  xnuqemu) platform=t70pro.dsc; mode=RELEASE; xnu_args=(-D T70_XNU_BOOT=TRUE -D T70_XNU_QEMU_TEST=TRUE); module_args=(-m t70proPkg/Tests/QemuXnu/QemuXnu.inf) ;;
  *) echo "Usage: $0 [acpi [DEBUG|RELEASE]|firmware|firmware-trial|devicecheck|linuxboot|xnuboot|xnuboot-macos|xnuqemu|module MediaTekPkg/Drivers/name/name.inf]" >&2; exit 2 ;;
esac
# Keep the normal tree by default; isolated firmware verification may select a
# separate build-output leaf without cloning sources or overwriting old FVs.
build_output=${T70_BUILD_OUTPUT:-Build/t70proPkg}
output_args=()
if [[ -n ${T70_BUILD_OUTPUT:-} ]]; then
  [[ $target == firmware || $target == firmware-trial ]] || { echo "T70_BUILD_OUTPUT is supported only for firmware targets" >&2; exit 2; }
  [[ $build_output =~ ^Build/[A-Za-z0-9][A-Za-z0-9._-]*$ && ! -L $repo/$build_output ]] || { echo "Use a non-symlink Build/name output directory" >&2; exit 2; }
  output_args=(-D "T70_BUILD_OUTPUT_DIRECTORY=$build_output")
fi
if [[ $target == xnuboot-macos ]]; then
  xnu_os_args=(--os macos)
  xnu_stage=XnuStage-macos
fi
[[ $mode == DEBUG || $mode == RELEASE ]] || { echo "Use DEBUG or RELEASE" >&2; exit 2; }
export WORKSPACE="$repo"
export EDK_TOOLS_PATH="$repo/Mu_Basecore/BaseTools"
export PYTHON_COMMAND="${PYTHON_COMMAND:-$(command -v python3)}"
export PACKAGES_PATH="$repo/Mu_Basecore:$repo/Common/Mu:$repo/Common/Mu_OEM_Sample:$repo/Silicon/Silicium:$repo/Silicon/MediaTek:$repo/Platforms/Teclast:$repo/Silicium-ACPI:$repo/Silicium-ACPI/Platforms/Teclast:$repo/Silicium-ACPI/Silicon/MediaTek"
if [[ $target == xnuboot || $target == xnuboot-macos || $target == xnuqemu ]]; then
  "$PYTHON_COMMAND" "$repo/Platforms/Teclast/t70proPkg/Xnu/prepare-sources.py" --check
  export PACKAGES_PATH="$PACKAGES_PATH:$repo/../xnu-sources/UniXNU/package:$repo/../xnu-sources/DarwinBoot"
fi
native_bin="$EDK_TOOLS_PATH/Source/C/bin"
if [[ $(uname -s) == Darwin ]]; then
  native_bin="$repo/../tools/native-macos"
  # Metadata targets resolve the whole DSC/FDF graph but do not invoke a
  # compiler/linker. Do not demand Zig or rewrite tool wrappers for them.
  if [[ ${BUILD_ACTION:-all} != genc && ${BUILD_ACTION:-all} != genmake ]]; then
    [[ -x ${T70_EDK_ZIG:-} ]] || { echo "Set T70_EDK_ZIG to the existing Zig executable" >&2; exit 1; }
    [[ -x $native_bin/GenFw ]] || { echo "Build Mac BaseTools from the existing source first" >&2; exit 1; }
    # Separate host executables; the source and AARCH64 output tree remain one.
    for tool in lld-link llvm-lib ld.lld; do
      case "$tool" in llvm-lib) zig_tool=lib ;; lld-link) zig_tool='lld-link /threads:1' ;; ld.lld) zig_tool='ld.lld --threads=1' ;; esac
      { printf '%s\n' '#!/usr/bin/env bash'; printf 'exec "${T70_EDK_ZIG:?}" %s "$@"\n' "$zig_tool"; } > "$native_bin/$tool"
      chmod +x "$native_bin/$tool"
    done
    ln -sfn "$(command -v clang)" "$native_bin/clang"
    ln -sfn "$(command -v gmake)" "$native_bin/make"
  fi
  host_args=(-D T70_NO_LTO=TRUE)
  export CLANG_BIN="$native_bin/"
  export CLANG_HOST_BIN="$native_bin/"
  export IASL_PREFIX="$repo/../tools/acpica/generate/unix/bin/"
else
  export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
fi
export CONF_PATH="$repo/Conf"
export PATH="$native_bin:$PATH"
[[ -x $native_bin/GenFw ]] || { echo "Build native BaseTools first" >&2; exit 1; }
cd "$repo"
# BuildEnv uses optional variables that are unset on a fresh Linux environment.
mkdir -p "$CONF_PATH"
set +u
export RECONFIG=FALSE
source "$EDK_TOOLS_PATH/BuildEnv"
set -u
# BuildEnv prepends generic wrappers which select Linux executables on a shared tree.
export PATH="$native_bin:$PATH"
# The Teclast boot logo (Logo.bmp) is not kept in Git; without it the
# firmware shows the Silicium logo instead.
logo="$repo/Platforms/Teclast/t70proPkg/Drivers/T70PlatformLogoDxe/Logo.bmp"
[ -f "$logo" ] || cp "$repo/Silicon/Silicium/SiliciumPkg/Include/Resources/BootLogo.bmp" "$logo"
# T70_GPU_POWER_ON=1 builds GpuPowerDxe that powers the Mali at ReadyToBoot.
[ "${T70_GPU_POWER_ON:-}" = 1 ] && host_args+=(-D T70_GPU_POWER_ON=TRUE)
build -p "Platforms/Teclast/t70proPkg/$platform" -a AARCH64 -t CLANGPDB -b "$mode" -n 6 ${host_args[@]+"${host_args[@]}"} ${xnu_args[@]+"${xnu_args[@]}"} ${module_args[@]+"${module_args[@]}"} ${output_args[@]+"${output_args[@]}"} "${BUILD_ACTION:-all}"
if [[ $target == xnuboot || $target == xnuboot-macos || $target == xnuqemu ]]; then
  build -p "Platforms/Teclast/t70proPkg/$platform" -a AARCH64 -t CLANGPDB -b "$mode" -n 6 ${host_args[@]+"${host_args[@]}"} ${xnu_args[@]+"${xnu_args[@]}"} -m DarwinBootPkg/Driver/DarwinBoot.inf "${BUILD_ACTION:-all}"
  [[ ${BUILD_ACTION:-all} == all && $target != xnuqemu ]] || exit 0
  output="$repo/$build_output/${mode}_CLANGPDB"
  stage="$output/$xnu_stage/EFI/Teclast"
  mkdir -p "$stage"
  cp "$output/AARCH64/T70XnuBoot.efi" "$stage/T70XnuBoot.efi"
  cp "$output/AARCH64/DarwinBoot.efi" "$stage/DarwinBoot.efi"
  cp "$repo/Platforms/Teclast/t70proPkg/Xnu/sources.json" "$output/$xnu_stage/sources.json"
  "$PYTHON_COMMAND" "$repo/Platforms/Teclast/t70proPkg/Xnu/build-kernel.py" --locked ${xnu_os_args[@]+"${xnu_os_args[@]}"}
  "$PYTHON_COMMAND" "$repo/Platforms/Teclast/t70proPkg/Xnu/verify.py" --locked ${xnu_os_args[@]+"${xnu_os_args[@]}"}
  "$PYTHON_COMMAND" "$repo/Platforms/Teclast/t70proPkg/Xnu/verify-kernel.py" --locked ${xnu_os_args[@]+"${xnu_os_args[@]}"}
  echo "XNU stage verified: $output/$xnu_stage (EFI, kernel and runtime allocation ADT)"
  exit 0
fi

[[ ${BUILD_ACTION:-all} == all ]] || exit 0
[[ $target != linuxboot && $target != module && $target != devicecheck ]] || exit 0

# GenFw emits the C data section, including a trailing reference pointer.
# Publish precisely Header.Length bytes and finalize the ACPI checksum.
"$PYTHON_COMMAND" - "$repo/$build_output/${mode}_CLANGPDB" <<'FINALIZE'
from pathlib import Path
import struct
import sys
root = Path(sys.argv[1])
assert root.is_dir(), root
files = list(root.rglob("*.acpi"))
assert len(files) == 4, files
for p in files:
    data = bytearray(p.read_bytes())
    size = struct.unpack_from("<I", data, 4)[0]
    assert 36 <= size <= len(data), p
    data = data[:size]
    data[9] = 0
    data[9] = (-sum(data)) & 255
    if data != p.read_bytes():
        p.write_bytes(data)
FINALIZE

if [[ $target == firmware || $target == firmware-trial ]]; then
  output="$repo/$build_output/RELEASE_CLANGPDB"
  fd="$output/FV/SILICIUM_UEFI.fd"
  [[ $("$PYTHON_COMMAND" -c 'import os,sys;print(os.stat(sys.argv[1]).st_size)' "$fd") == 4194304 ]] || { echo "Unexpected FD size" >&2; exit 1; }
  stem=T70Pro-UEFI
  trial=0
  if [[ $target == firmware-trial ]]; then
    stem=T70Pro-UEFI-Trial
    trial=1
  fi
  clang --target=aarch64-none-elf -c "$repo/Platforms/Teclast/t70proPkg/BootShim.S" \
    -DT70_RAM_TRIAL="$trial" -DFD_PATH="\"$fd\"" -o "$output/BootShim.o"
  ld.lld --image-base=0 -Ttext=0 --entry=_Head --oformat=binary "$output/BootShim.o" \
    -o "$output/$stem.Image"
  rm "$output/BootShim.o"
  # Match the stock kernel's legacy LZ4 stream and Android boot header v4.
  compressed="$output/$stem.Image.lz4"
  trap 'rm -f "$compressed"' EXIT
  lz4 -l -f "$output/$stem.Image" "$compressed"
  "$PYTHON_COMMAND" "$repo/Resources/Scripts/mkbootimg.py" --header_version 4 \
    --kernel "$compressed" --output "$output/$stem-boot.img"
fi
