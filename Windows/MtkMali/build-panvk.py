#!/usr/bin/env python3
"""Build the pinned PanVK tree for native AArch64 Linux and run an offline ISA check."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import struct
import shutil
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

PINNED_MESA_COMMIT = "58f2c55854bd48b18df756edb11f187af9cd2fde"
HERE = Path(__file__).resolve().parent
SILICIUM = HERE.parents[1]
PROJECT = SILICIUM.parent
MESA = PROJECT / "mesa"
BUILD = MESA / "build-t70pro-panvk"
REPORT = PROJECT / "analysis" / "panvk-build.json"


# Missing from the Android-only PanVK subset; original Mesa C sources are unchanged.
WAYLAND_MESA_COMMIT = "0c11a9cfb35595e404913691241a2a2a0f0a8f03"
WAYLAND_MESA_SOURCES = {
    'include/vulkan/vulkan_wayland.h': '31778a15a99e3b311d172d8e34f77be9d468aa252d81515dd7bda6eed8ec3f0a',
    "src/loader/loader_wayland_helper.c": "a03fab990c9d96e2fa82e7157338b260ac6ec4a67454c3bd40291a000b961e16",
    "src/loader/loader_wayland_helper.h": "84d28496a93221984351a05be20b7d3205adb8a3c1051577b90f5ba73becdbc0",
    "src/loader/meson.build": "01d98829e36ccfd6c206b43f4e562fba3f66d01c15f03a2db563783fc3319989",
}
WAYLAND_PROTOCOLS = [
    "staging/fifo/fifo-v1.xml",
    "staging/commit-timing/commit-timing-v1.xml",
    "unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml",
    "stable/presentation-time/presentation-time.xml",
    "staging/tearing-control/tearing-control-v1.xml",
    "staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml",
    "staging/color-management/color-management-v1.xml",
]

MESON_OPTIONS = [
    "-Dplatforms=wayland",
    "-Dgallium-drivers=",
    "-Dvulkan-drivers=panfrost",
    "-Dbuild-tests=false",
    "-Dtools=",
    "-Dprecomp-compiler=enabled",
    "-Dinstall-precomp-compiler=false",
]

SPIRV_ASM = """OpCapability Addresses
OpCapability Kernel
OpMemoryModel Physical64 OpenCL
OpEntryPoint Kernel %kernel "kernel"
OpExecutionMode %kernel LocalSize 1 1 1
OpName %kernel "kernel"
OpName %dst "dst"
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%ptrCrossUint = OpTypePointer CrossWorkgroup %uint
%fn = OpTypeFunction %void %ptrCrossUint
%c40 = OpConstant %uint 40
%c2 = OpConstant %uint 2
%kernel = OpFunction %void None %fn
%dst = OpFunctionParameter %ptrCrossUint
%entry = OpLabel
%sum = OpIAdd %uint %c40 %c2
OpStore %dst %sum
OpReturn
OpFunctionEnd
"""


def run(
    argv: list[str], *, capture: bool = False, cwd: Path | None = None
) -> subprocess.CompletedProcess[str]:
    print("+ " + shlex.join(argv), flush=True)
    return subprocess.run(
        argv,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        cwd=cwd,
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()



def inspect_linux_icd(path: Path, *, require_wayland: bool = False) -> dict:
    """Check the actual linked ELF, without loading a GPU driver or Linux process."""
    data = path.read_bytes()
    if data[:7] != b"\x7fELF\x02\x01\x01":
        raise RuntimeError("ICD is not a little-endian ELF64 file")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    if header[1:3] != (3, 183):
        raise RuntimeError("ICD must be an AArch64 shared object")
    phoff, shoff = header[5:7]
    phsize, phnum, shsize, shnum = header[9:13]
    if phsize != 56 or shsize != 64:
        raise RuntimeError("Unexpected ELF program/section header size")
    for i in range(phnum):
        ph = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize)
        if ph[2] + ph[5] > len(data) or ph[5] > ph[6]:
            raise RuntimeError("Truncated ELF segment")
        if ph[0] == 1 and ph[1] & 3 == 3:
            raise RuntimeError("ICD has a writable executable load segment")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize)
                for i in range(shnum)]
    def string(table: bytes, offset: int) -> str:
        return table[offset:].split(b"\0", 1)[0].decode("ascii")
    exports, defined, imports, needed, soname = set(), set(), set(), [], None
    for section in sections:
        _, kind, _, _, offset, size, link, _, _, stride = section
        if offset + size > len(data) and kind != 8:
            raise RuntimeError("Truncated ELF section")
        if kind not in (2, 6, 11):
            continue
        strings = sections[link]
        table = data[strings[4]:strings[4] + strings[5]]
        if kind in (2, 11):
            for i in range(offset, offset + size, stride):
                name, info, other, index, _, _ = struct.unpack_from("<IBBHQQ", data, i)
                symbol = string(table, name)
                if index:
                    defined.add(symbol)
                elif kind == 11:
                    imports.add(symbol)
                if kind == 11 and index and info >> 4 in (1, 2) and other & 3 == 0:
                    exports.add(symbol)
        else:
            for i in range(offset, offset + size, stride):
                tag, value = struct.unpack_from("<qQ", data, i)
                if tag == 1:
                    needed.append(string(table, value))
                elif tag == 14:
                    soname = string(table, value)
    required = {"vk_icdGetInstanceProcAddr",
                "vk_icdGetPhysicalDeviceProcAddr",
                "vk_icdNegotiateLoaderICDInterfaceVersion"}
    if not required <= exports or soname != "libvulkan_panfrost.so":
        raise RuntimeError("ICD loader exports or SONAME are missing")
    wayland_symbols = {
        "wsi_CreateWaylandSurfaceKHR", "wsi_GetPhysicalDeviceWaylandPresentationSupportKHR",
        "wsi_wl_init_wsi", "loader_wayland_dispatch",
        "zwp_linux_dmabuf_v1_interface", "wp_linux_drm_syncobj_manager_v1_interface",
        "wp_presentation_interface", "wp_fifo_manager_v1_interface",
        "wp_commit_timing_manager_v1_interface", "wp_tearing_control_manager_v1_interface",
        "wp_color_manager_v1_interface",
    }
    # Wayland 1.24 performs prepare/read inside its timeout dispatcher.
    wayland_imports = {"wl_proxy_marshal_flags", "wl_display_dispatch_queue_pending",
                       "wl_display_dispatch_queue_timeout", "wl_display_create_queue_with_name",
                       "wl_display_flush"}
    wayland = ("libwayland-client.so.0" in needed and wayland_symbols <= defined
               and wayland_imports <= imports)
    if require_wayland and not wayland:
        raise RuntimeError("Linked ICD lacks actual Wayland WSI/protocols/client-library calls")
    return {"format": "ELF64 AArch64 shared object", "soname": soname,
            "Wayland_WSI_linked": wayland,
            "Wayland_defined_symbols": sorted(wayland_symbols & defined),
            "Wayland_client_imports": sorted(wayland_imports & imports),
            "loader_exports": sorted(required), "needed": needed,
            "writable_executable_load_segments": False}


def qualified_mesa_changes() -> tuple[list[str], dict, dict | None]:
    """Accept only sources covered by the existing ABI/compiler executions."""
    abi = json.loads((PROJECT / "analysis/panvk-kbase-abi-verification.json").read_text())
    allowed = dict(WAYLAND_MESA_SOURCES)
    for name, digest in WAYLAND_MESA_SOURCES.items():
        if sha256(MESA / name) != digest:
            raise RuntimeError("Restored Wayland source changed: " + name)
    for name, digest in abi["source_sha256"].items():
        if name.startswith("mesa/"):
            if sha256(PROJECT / name) != digest:
                raise RuntimeError("Tested source changed: " + name)
            allowed[name.removeprefix("mesa/")] = digest
    dirty = run(["git", "-C", str(MESA), "diff", "--name-only", "HEAD"],
                capture=True).stdout.splitlines()
    shader_qualification = None
    shader_compat = {"meson.build", "meson.options", "src/meson.build",
                     "src/panfrost/compiler/pan_nir.h",
                     "src/panfrost/compiler/bifrost/compiler.h",
                     "src/panfrost/compiler/midgard/midgard_compile.c"}
    if any(name in shader_compat for name in dirty):
        native_path = PROJECT / "analysis/mali-shader-verification.json"
        windows_path = PROJECT / "analysis/mali-shader-windows.json"
        native, windows = (json.loads(p.read_text()) for p in (native_path, windows_path))
        if (native["status"] != "passed" or windows["status"] != "passed" or
            native["native_shader_cases"] < 14 or native["native_layout_assertions"] != 300 or
            windows["ARM64_layout_test"]["assertions"] != 300 or
            native["source_sha256"] != windows["source_sha256"]):
            raise RuntimeError("Current native and ARM64 shader qualifications required")
        for name, digest in native["source_sha256"].items():
            if sha256(PROJECT / name) != digest:
                raise RuntimeError("Qualified shader source changed: " + name)
        for name in shader_compat:
            digest = native["provenance"]["mesa_tracked_change_sha256"].get(name)
            if not digest or digest != sha256(MESA / name) or digest != windows["provenance"]["mesa_tracked_change_sha256"].get(name):
                raise RuntimeError("Untested compiler portability change: " + name)
            allowed[name] = digest
        shader_qualification = {"native_report_sha256": sha256(native_path),
                                "Windows_report_sha256": sha256(windows_path)}
    if any(name not in allowed for name in dirty):
        raise RuntimeError("Tracked changes outside tested PanVK ABI/compiler portability sources")
    return dirty, allowed, shader_qualification



def prepare_wayland(root: Path) -> tuple[list[Path], dict]:
    """Run the upstream scanner and execute an independent XML-to-C metadata check."""
    scanner = PROJECT / "tools/wayland/build/src/wayland-scanner"
    if not scanner.is_file():
        raise RuntimeError("Build the upstream Wayland 1.24.0 scanner in tools/wayland first")
    if tool_version(str(scanner), ["--version"]) != "wayland-scanner 1.24.0":
        raise RuntimeError("The scanner must match the builder Wayland 1.24.0 library")
    output = BUILD / "src/loader"
    check_dir = BUILD / "t70-wayland-protocol-check"
    output.mkdir(parents=True, exist_ok=True)
    check_dir.mkdir(parents=True, exist_ok=True)
    xmls = [root / "usr/share/wayland-protocols" / name for name in WAYLAND_PROTOCOLS]
    core = root / "usr/share/wayland/wayland.xml"
    if not all(path.is_file() for path in [core, *xmls]):
        raise RuntimeError("Required builder Wayland protocol XML is missing")
    generated, sources = {}, []
    for xml in xmls:
        for mode, suffix in [("private-code", "-protocol.c"), ("client-header", "-client-protocol.h")]:
            destination = output / (xml.stem + suffix)
            run([str(scanner), "--strict", mode, str(xml), str(destination)])
            generated[str(destination)] = sha256(destination)
            if mode == "private-code":
                sources.append(destination)
    core_c = check_dir / "core-protocol.c"
    run([str(scanner), "--strict", "public-code", str(core), str(core_c)])
    malformed = subprocess.run([str(scanner), "--strict", "private-code"],
                               input='<protocol name="broken"><interface',
                               capture_output=True, text=True)
    if malformed.returncode == 0:
        raise RuntimeError("Scanner accepted malformed protocol XML")
    documents = [ET.parse(path).getroot() for path in [core, *xmls]]
    interfaces = [interface for doc in documents for interface in doc.findall("interface")]
    lines = ["#include <wayland-util.h>", "#include <stdlib.h>", "#include <stdio.h>",
             "#include <string.h>", "static unsigned checks;",
             '#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\\n", #x); abort(); } checks++; } while (0)']
    for interface in interfaces:
        name = interface.attrib["name"]
        if not re.fullmatch(r"[a-z_][a-z0-9_]*", name):
            raise RuntimeError("Unexpected protocol interface identifier")
        lines.append(f"extern const struct wl_interface {name}_interface;")
    lines += ["int main(void) {", "CHECK(sizeof(void *) == 8);",
              "CHECK(sizeof(struct wl_interface) == 40);", "CHECK(sizeof(struct wl_message) == 24);"]
    types = {"int": "i", "uint": "u", "fixed": "f", "string": "s", "object": "o",
             "new_id": "n", "array": "a", "fd": "h"}
    messages = 0
    for interface in interfaces:
        variable = interface.attrib["name"] + "_interface"
        lines += [f'CHECK(strcmp({variable}.name, {json.dumps(interface.attrib["name"])}) == 0);',
                  f'CHECK({variable}.version == {int(interface.attrib["version"])});']
        for tag, count, member in [("request", "method_count", "methods"), ("event", "event_count", "events")]:
            elements = interface.findall(tag)
            lines.append(f"CHECK({variable}.{count} == {len(elements)});")
            for index, message in enumerate(elements):
                messages += 1
                field = f"{variable}.{member}[{index}]"
                signature = str(message.attrib.get("since", "1"))
                if signature == "1":
                    signature = ""
                arguments = []
                for arg in message.findall("arg"):
                    if arg.attrib["type"] == "new_id" and "interface" not in arg.attrib:
                        signature += "su"
                        arguments += [None, None]
                    if arg.attrib.get("allow-null") == "true":
                        signature += "?"
                    signature += types[arg.attrib["type"]]
                    arguments.append(arg.attrib.get("interface"))
                lines += [f'CHECK(strcmp({field}.name, {json.dumps(message.attrib["name"])}) == 0);',
                          f'CHECK(strcmp({field}.signature, {json.dumps(signature)}) == 0);']
                for position, name in enumerate(arguments):
                    expected = "NULL" if name is None else "&" + name + "_interface"
                    lines.append(f"CHECK({field}.types[{position}] == {expected});")
    lines += ['printf("Wayland protocol metadata: %u checks passed\\n", checks);', 'return 0;', '}']
    fixture = check_dir / "protocol-check.c"
    fixture.write_text("\n".join(lines) + "\n")
    executable = check_dir / "protocol-check"
    command = ["/usr/bin/clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-idirafter", str(root / "usr/include"), str(fixture), str(core_c),
               *(str(path) for path in sources), "-o", str(executable)]
    run(command)
    result = run([str(executable)], capture=True).stdout.strip()
    print(result, flush=True)
    match = re.fullmatch(r"Wayland protocol metadata: (\d+) checks passed", result)
    if match is None:
        raise RuntimeError("Protocol metadata checker did not report success")
    return sources, {
        "status": "passed", "scanner": str(scanner), "scanner_sha256": sha256(scanner),
        "scanner_version": "1.24.0", "protocol_count": len(xmls), "interfaces": len(interfaces),
        "messages": messages, "metadata_assertions": int(match[1]), "malformed_XML_rejected": True,
        "XML_source_sha256": {str(path): sha256(path) for path in [core, *xmls]},
        "generated_source_sha256": generated,
        "original_Mesa_helper_commit": WAYLAND_MESA_COMMIT,
        "Mesa_helper_source_sha256": WAYLAND_MESA_SOURCES,
        "native_ASan_UBSan_test": {"path": str(executable), "bytes": executable.stat().st_size,
                                  "sha256": sha256(executable), "fixture_sha256": sha256(fixture)},
        "compositor_runtime_executed": False, "device_runtime_executed": False,
    }


def macos_cross_icd(jobs: int) -> int:
    """Reuse this build's generated files and relink with its Linux system libraries."""
    if platform.system() != "Darwin":
        raise RuntimeError("--macos-cross-icd requires the macOS mounted builder tree")
    lock = Path("/Users/you/.tmp/build.lock")
    if not lock.is_file() or json.loads(lock.read_text()).get("pid") != os.getppid():
        raise RuntimeError("Run this build through Resources/Scripts/with-build-lock")
    if shutil.disk_usage(PROJECT).free < 40 * 1024**3:
        raise RuntimeError("Less than 40 GiB free; refusing to rebuild")
    original = json.loads(REPORT.read_text())
    vm_mesa = original["mesa_tree"]
    root = PROJECT.parents[2]
    if vm_mesa != "/home/builder/teclast-t70pro/mesa" or not (root / "usr/include").is_dir():
        raise RuntimeError("Expected the existing builder mount and configured Mesa tree")
    commit = run(["git", "-C", str(MESA), "rev-parse", "HEAD"], capture=True).stdout.strip()
    if commit != PINNED_MESA_COMMIT:
        raise RuntimeError("Pinned Mesa commit mismatch")
    dirty, allowed, shader_qualification = qualified_mesa_changes()
    old_icd = BUILD / "src/panfrost/vulkan/libvulkan_panfrost.so"
    if sha256(old_icd) != original["artifacts"]["panvk_icd"]["sha256"]:
        raise RuntimeError("Retained ICD does not match its build record")
    zig = Path(os.environ.get("T70_MALI_ZIG", "/private/tmp/t70-mali-python/ziglang/zig"))
    if not zig.is_file():
        raise RuntimeError("Existing Zig compiler not found")
    vm_project = str(Path(vm_mesa).parent)
    def mapped(value: str) -> str:
        return value.replace(vm_project, str(PROJECT)).replace("/usr/", str(root / "usr") + "/")
    def file_for(value: str) -> Path:
        value = mapped(value)
        return Path(value) if Path(value).is_absolute() else BUILD / value
    edges, current = {}, None
    for line in (BUILD / "build.ninja").read_text().splitlines():
        if line.startswith("build "):
            output, rest = line[6:].split(": ", 1)
            tokens = rest.split()
            current = {"out": [x for x in output.split() if x != "|"],
                       "rule": tokens[0],
                       "in": [x for x in tokens[1:] if x not in ("|", "||")]}
            for name in current["out"]:
                edges[name] = current
        elif line.startswith(" ") and current is not None and "=" in line:
            key, value = line.strip().split("=", 1)
            current[key.strip()] = value.strip()
    # Keep the native Meson directory intact; extend only this cross-build graph.
    protocol_sources, wayland_qualification = prepare_wayland(root)
    wsi_object = "src/vulkan/wsi/libvulkan_wsi.a.p/wsi_common.c.o"
    wsi_archive = edges["src/vulkan/wsi/libvulkan_wsi.a"]
    extra_sources = [MESA / "src/vulkan/wsi/wsi_common_wayland.c",
                     MESA / "src/loader/loader_wayland_helper.c", *protocol_sources]
    for source in extra_sources:
        relative = os.path.relpath(source, BUILD)
        output = "src/vulkan/wsi/libvulkan_wsi.a.p/t70-wayland-" + source.name + ".o"
        edges[output] = {"out": [output], "in": [relative], "rule": "c_COMPILER",
                         "ARGS": edges[wsi_object]["ARGS"]}
        wsi_archive["in"].append(output)
    target = str(old_icd.relative_to(BUILD))
    seen, compile_edges, archive_edges, inputs, generated = set(), [], [], set(), set()
    def visit(name: str) -> None:
        if name in seen:
            return
        seen.add(name)
        edge = edges.get(name)
        if edge is None:
            path = file_for(name)
            if not path.is_file():
                raise RuntimeError("Required build input missing: " + name)
            inputs.add(path)
            return
        if edge["rule"].startswith("CUSTOM_COMMAND"):
            for output in edge["out"]:
                path = file_for(output)
                if not path.is_file():
                    raise RuntimeError("Generated source missing; native regeneration required: " + output)
                generated.add(path)
            return
        if edge["rule"] not in ("c_COMPILER", "cpp_COMPILER", "STATIC_LINKER", "cpp_LINKER"):
            raise RuntimeError("Unsupported configured Ninja rule: " + edge["rule"])
        for dependency in edge["in"]:
            visit(dependency)
        if edge["rule"] in ("c_COMPILER", "cpp_COMPILER"):
            compile_edges.append(edge)
        elif edge["rule"] == "STATIC_LINKER":
            archive_edges.append(edge)
    visit(target)
    # A generated-source rebuild is deliberately not attempted with Linux generators on macOS.
    # Check every relevant local header as well as every source and generated input, so cache
    # reuse cannot silently retain an object after an included header has changed.
    for base in (MESA / "src", MESA / "include", BUILD / "src", BUILD / "include"):
        for pattern in ("*.h", "*.hpp", "*.inc"):
            inputs.update(base.rglob(pattern))
    inputs.update(generated)
    inputs.add(BUILD / "build.ninja")
    inputs.update(MESA / name for name in WAYLAND_MESA_SOURCES)
    inputs.update(Path(name) for name in wayland_qualification["XML_source_sha256"])
    inputs.add(Path(wayland_qualification["scanner"]))
    def input_snapshot() -> dict:
        return {str(path): sha256(path) for path in sorted(inputs) if path.is_file()}
    snapshot = input_snapshot()
    snapshot_hash = hashlib.sha256(json.dumps(snapshot, sort_keys=True).encode()).hexdigest()
    state_file = BUILD / "t70-macos-cross-state.json"
    state = json.loads(state_file.read_text()) if state_file.exists() else {}
    cc = Path(os.environ.get("T70_PANVK_CC", "/usr/bin/clang"))
    compiler_hash = sha256(cc)
    skipped = {"-mtls-dialect=desc", "-flifetime-dse=1", "-DHAVE___BUILTIN_ADD_OVERFLOW_P",
               "-DHAVE___BUILTIN_SUB_OVERFLOW_P", "-DHAVE___BUILTIN_MUL_OVERFLOW_P",
               "-DHAVE_FUNC_ATTRIBUTE_OPTIMIZE",
               "-DHAVE___BUILTIN_AARCH64_GET_FPCR", "-DHAVE___BUILTIN_AARCH64_SET_FPCR"}
    base = [str(cc), "--target=aarch64-linux-gnu", "-Wno-unknown-warning-option",
            "--sysroot=" + str(root)]
    cpp = ["-nostdinc++", "-isystem", str(root / "usr/include/c++/15"),
           "-isystem", str(root / "usr/include/aarch64-linux-gnu/c++/15")]
    system = ["-isystem", str(root / "usr/include/aarch64-linux-gnu"),
              "-isystem", str(root / "usr/include")]
    commands = []
    def compile_one(edge: dict) -> tuple:
        output = edge["out"][0]
        args = [mapped(arg) for arg in shlex.split(edge["ARGS"]) if arg not in skipped]
        extra = cpp if edge["rule"] == "cpp_COMPILER" else []
        command = [*base, *extra, *system, *args, "-DVK_USE_PLATFORM_WAYLAND_KHR",
                   "-DWL_HIDE_DEPRECATED", "-DHAVE_WL_DISPATCH_QUEUE_TIMEOUT",
                   "-DHAVE_WL_CREATE_QUEUE_WITH_NAME", "-Isrc/loader", "-I../src/loader",
                   "-c", mapped(edge["in"][0]), "-o", output]
        digest = hashlib.sha256(json.dumps([compiler_hash, snapshot_hash, command]).encode()).hexdigest()
        old = state.get(output)
        path = BUILD / output
        if old and old["command_sha256"] == digest and path.is_file() and sha256(path) == old["sha256"]:
            return output, old, command, True
        path.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(command, cwd=BUILD, capture_output=True, text=True)
        if result.returncode:
            path.unlink(missing_ok=True)
            raise RuntimeError(output + "\n" + result.stderr[-5000:])
        return output, {"command_sha256": digest, "sha256": sha256(path)}, command, False
    print(f"Rebuilding {len(compile_edges)} objects and {len(archive_edges)} archives; "
          f"reusing {len(generated)} existing generated inputs", flush=True)
    rebuilt = reused = 0
    try:
        # Check newly enabled WSI sources before the full ICD object graph.
        new_edges = [edge for edge in compile_edges if "/t70-wayland-" in edge["out"][0]]
        for edge in new_edges:
            output, record, command, cached = compile_one(edge)
            state[output] = record
            commands.append(command)
            rebuilt += not cached
            reused += cached
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(compile_one, edge) for edge in compile_edges if edge not in new_edges]
            for future in as_completed(futures):
                try:
                    output, record, command, cached = future.result()
                except Exception:
                    for pending in futures:
                        pending.cancel()
                    raise
                state[output] = record
                commands.append(command)
                rebuilt += not cached
                reused += cached
                if (rebuilt + reused) % 50 == 0:
                    state_file.write_text(json.dumps(state, sort_keys=True) + "\n")
                    print(f"Objects {rebuilt + reused}/{len(compile_edges)} "
                          f"(rebuilt {rebuilt}, reused {reused})", flush=True)
    finally:
        state_file.write_text(json.dumps(state, sort_keys=True) + "\n")
    def execute(command: list[str]) -> None:
        result = subprocess.run(command, cwd=BUILD, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError("Linux archive/link failed:\n" + result.stderr[-8000:])
    for edge in archive_edges:
        output = BUILD / edge["out"][0]
        output.unlink(missing_ok=True)
        command = [str(zig), "ar", *shlex.split(edge["LINK_ARGS"]),
                   edge["out"][0], *edge["in"]]
        execute(command)
        commands.append(command)
    edge = edges[target]
    pending = old_icd.with_suffix(".so.pending")
    gcc = root / "usr/lib/gcc/aarch64-linux-gnu/15"
    lib = root / "usr/lib/aarch64-linux-gnu"
    command = [str(zig), "ld.lld", "-m", "aarch64linux", "--eh-frame-hdr",
               "--hash-style=gnu", "-z", "noexecstack", "--sysroot=" + str(root),
               "-L" + str(lib), "-L" + str(gcc), "-o", str(pending),
               str(lib / "crti.o"), str(gcc / "crtbeginS.o")]
    # The graph includes implicit library dependencies. Explicit object inputs precede them.
    command.extend(name for name in edge["in"] if name.endswith(".o"))
    for arg in shlex.split(edge["LINK_ARGS"]):
        arg = mapped(arg)
        if arg.startswith("-Wl,"):
            command.extend(arg[4:].split(","))
        elif arg == "-lm":
            command.append(str(lib / "libm.so.6"))
        elif arg not in ("-fPIC", "-pthread"):
            command.append(arg)
    command.append(str(lib / "libwayland-client.so"))
    command.extend([str(gcc / "libstdc++.so"), str(lib / "libm.so.6"),
                    str(lib / "libgcc_s.so.1"), str(gcc / "libgcc.a"),
                    str(lib / "libc.so.6"), str(lib / "libc_nonshared.a"),
                    str(lib / "ld-linux-aarch64.so.1"),
                    str(gcc / "crtendS.o"), str(lib / "crtn.o")])
    runtime_inputs = {name: sha256(Path(name)) for name in command
                      if Path(name).is_absolute() and Path(name).is_file()
                      and str(root / "usr/lib") in name}
    try:
        execute(command)
        commands.append(command)
        elf = inspect_linux_icd(pending, require_wayland=True)
        if any(sha256(Path(name)) != digest for name, digest in runtime_inputs.items()):
            raise RuntimeError("A Linux link runtime input changed during the build")
        if input_snapshot() != snapshot:
            raise RuntimeError("A build input changed while the build lock was held")
        new_artifact = {"path": original["artifacts"]["panvk_icd"]["path"],
                        "bytes": pending.stat().st_size, "sha256": sha256(pending)}
        pending.replace(old_icd)
    finally:
        pending.unlink(missing_ok=True)
    removed_files = removed_bytes = 0
    for output in [*(edge["out"][0] for edge in compile_edges),
                   *(edge["out"][0] for edge in archive_edges)]:
        path = BUILD / output
        if path.is_file():
            removed_bytes += path.stat().st_size
            path.unlink()
            removed_files += 1
    state_file.unlink(missing_ok=True)
    now = datetime.now(timezone.utc).isoformat()
    original["macos_cross_relink"] = {
        "status": "passed", "at": now, "host_system": platform.system(),
        "target": "aarch64-linux-gnu", "sysroot": str(root),
        "compiler_version": tool_version(str(cc), ["--version"]),
        "linker_version": tool_version(str(zig), ["ld.lld", "--version"]),
        "linker_sha256": sha256(zig),
        "compiler_sha256": compiler_hash, "mesa_commit": commit,
        "omitted_GCC_only_flags": sorted(skipped),
        "FPCR_access": "existing Mesa AArch64 MRS/MSR fallback, not a stub",
        "tracked_source_changes": dirty, "tested_source_sha256": allowed,
        "compiler_portability_qualification": shader_qualification,
        "source_and_generated_input_sha256": snapshot,
        "input_snapshot_sha256": snapshot_hash,
        "objects_compiled": rebuilt, "objects_reused": reused,
        "archives_linked": len(archive_edges), "build_jobs": jobs,
        "commands_sha256": hashlib.sha256(json.dumps(sorted(commands), sort_keys=True).encode()).hexdigest(),
        "ELF_verification": elf, "previous_icd": original["artifacts"]["panvk_icd"],
        "link_runtime_input_sha256": runtime_inputs,
        "objects_in_ICD": len(compile_edges),
        "intermediates_removed_files": removed_files, "intermediates_removed_bytes": removed_bytes,
        "generated_sources_reused": len(generated), "shader_compiler_rerun": False,
        "Wayland_protocol_qualification": wayland_qualification,
        "effective_platforms": ["wayland"], "DRM_KMS_WSI_retained": True,
        "native_Meson_configuration_unchanged": True,
        "Wayland_WSI_added_objects": len(extra_sources),
        "device_runtime_executed": False, "Vulkan_runtime_executed": False,
        "scope": "ICD cross-rebuilt from the tested current JM ABI sources; existing generated sources "
                 "and actual builder Linux system libraries reused; actual upstream Wayland scanner "
                 "generated and independently verified protocol data; Linux Wayland WSI linked"}
    original["window_system_build"] = {"Wayland": True, "DRM_KMS": True,
        "Wayland_compositor_runtime_executed": False, "device_runtime_executed": False}
    original["effective_options"] = {**original["options"], "platforms": ["wayland"],
        "window_system": "Wayland and DRM KMS display (macOS cross build)"}
    original["artifacts"]["panvk_icd"] = new_artifact
    original["artifacts_match_current_sources"] = True
    original["current_source_artifact_status"] = "Current tested PanVK sources cross-rebuilt and linked; ELF verified offline"
    original["post_build_source_changes"]["current_icd_relinked"] = True
    original["post_build_source_changes"]["current_icd_relinked_at"] = now
    original["build_script_sha256"] = sha256(Path(__file__))
    REPORT.write_text(json.dumps(original, indent=2, sort_keys=True) + "\n")
    print("Current-source Linux ICD relinked and ELF verified; GPU execution remains untested.", flush=True)
    return 0

def tool_version(command: str, args: list[str]) -> str:
    result = run([command, *args], capture=True)
    return (result.stdout + result.stderr).strip().splitlines()[0]


def artifact_for(targets: list[dict], name: str) -> Path:
    matches = [target for target in targets if target.get("name") == name]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one Meson target named {name!r}, found {len(matches)}")
    files = matches[0].get("filename", [])
    candidates = [Path(file) for file in files]
    candidates = [path if path.is_absolute() else BUILD / path for path in candidates]
    candidates = [path for path in candidates if path.is_file()]
    if len(candidates) != 1:
        raise RuntimeError(f"Could not resolve output for {name!r}: {files!r}")
    return candidates[0]


def compile_smoke(compiler: Path, disassembler: Path) -> dict:
    smoke_dir = BUILD / "t70-g57-offline-smoke"
    smoke_dir.mkdir(parents=True, exist_ok=True)
    asm = smoke_dir / "store_constant.spvasm"
    spirv = smoke_dir / "store_constant.spv"
    out_h = smoke_dir / "store_constant.h"
    out_c = smoke_dir / "store_constant.c"
    binary = smoke_dir / "store_constant-g57-arch9.bin"
    asm.write_text(SPIRV_ASM, encoding="ascii")

    spirv_as = os.environ.get("SPIRV_AS", "spirv-as")
    spirv_val = os.environ.get("SPIRV_VAL", "spirv-val")
    run([spirv_as, "--target-env", "opencl2.2", str(asm), "-o", str(spirv)])
    run([spirv_val, "--target-env", "opencl2.2", str(spirv)])
    run([str(compiler), "t70_store_constant", "9", str(spirv), str(out_h), str(out_c)])

    generated = out_c.read_text(encoding="utf-8")
    match = re.search(
        r"const uint32_t\s+([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\]\s*=\s*\{(.*?)\};",
        generated,
        flags=re.DOTALL,
    )
    if not match:
        raise RuntimeError("panfrost_compile did not emit a C shader blob")
    words = [int(word, 16) for word in re.findall(r"0x([0-9a-fA-F]{8})", match.group(3))]
    if len(words) != int(match.group(2)):
        raise RuntimeError("Generated C blob word count does not match its declared array size")

    probe_c = smoke_dir / "blob_abi_probe.c"
    probe = smoke_dir / "blob_abi_probe"
    probe_c.write_text(
        """#include <stddef.h>
#include <stdio.h>
#include "panfrost/compiler/bifrost/bifrost_compile.h"
int main(void) {
   printf("%zu %zu\\n",
          sizeof(struct bifrost_precompiled_kernel_info),
          offsetof(struct bifrost_precompiled_kernel_info, binary_size));
   return 0;
}
""",
        encoding="ascii",
    )
    compile_commands = json.loads((BUILD / "compile_commands.json").read_text())
    source_entry = next(
        entry for entry in compile_commands if entry["file"].endswith("src/panfrost/clc/pan_compile.c")
    )
    original_command = shlex.split(source_entry["command"])
    probe_command = []
    skip_next = False
    for arg in original_command:
        if skip_next:
            skip_next = False
            continue
        if arg in {"-MD", "-MMD"}:
            continue
        if arg in {"-MF", "-MQ", "-MT", "-o"}:
            skip_next = True
            continue
        if arg == "-c" or arg.endswith("src/panfrost/clc/pan_compile.c"):
            continue
        probe_command.append(arg)
    probe_command.extend([str(probe_c), "-o", str(probe)])
    run(probe_command, cwd=Path(source_entry["directory"]))
    info = run([str(probe)], capture=True).stdout.strip().split()
    if len(info) != 2 or sys.byteorder != "little":
        raise RuntimeError(f"Unexpected compiler ABI probe output: {info!r}")
    info_size, binary_size_offset = map(int, info)
    if info_size % 4 or binary_size_offset % 4:
        raise RuntimeError("Precompiled kernel metadata is not 32-bit aligned")
    size_index = binary_size_offset // 4
    payload_index = info_size // 4
    if size_index >= len(words) or payload_index >= len(words):
        raise RuntimeError("Precompiled kernel metadata is truncated")
    payload_size = words[size_index]
    payload_capacity = (len(words) - payload_index) * 4
    if not payload_size or payload_size > payload_capacity:
        raise RuntimeError(
            f"Invalid G57 code length {payload_size}; maximum encoded payload is {payload_capacity}"
        )

    payload = b"".join(word.to_bytes(4, "little") for word in words[payload_index:])
    binary.write_bytes(payload[:payload_size])
    disassembly = run(
        [str(disassembler), "--id=37011", "--verbose", "disassemble", str(binary)],
        capture=True,
    ).stdout
    if not disassembly.strip():
        raise RuntimeError("Valhall disassembler produced no output for the compiled shader")

    print("G57 arch-9 offline disassembly:")
    print(disassembly.rstrip(), flush=True)
    return {
        "status": "passed",
        "spirv_environment": "OpenCL 2.2",
        "spirv_sha256": sha256(spirv),
        "input_assembly": "one-work-item OpenCL kernel: out[0] = 40 + 2",
        "compiler_arch": 9,
        "gpu_disassembler_name": "G57 9.3",
        "selected_gpu_model_id": "0x90930000",
        "selection_basis": "G57 9.3 Mesa model target; runtime device ID not observed",
        "runtime_gpu_id_observed": False,
        "kernel_payload_bytes": payload_size,
        "kernel_payload_sha256": sha256(binary),
        "generated_c_sha256": sha256(out_c),
        "disassembly": disassembly,
        "device_runtime_executed": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=6, help="build parallelism (1..6)")
    parser.add_argument("--skip-build", action="store_true", help="run only the offline compiler check")
    parser.add_argument("--macos-cross-icd", action="store_true",
                        help="relink the tested current ICD using the mounted builder Linux inputs")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 6:
        parser.error("--jobs must be between 1 and 6")
    if platform.machine().lower() not in {"aarch64", "arm64"}:
        raise RuntimeError(f"Expected native AArch64 host, got {platform.machine()}")
    if not MESA.is_dir():
        raise RuntimeError(f"Pinned Mesa tree not found: {MESA}")

    if args.macos_cross_icd:
        if args.skip_build:
            parser.error("--macos-cross-icd cannot be combined with --skip-build")
        return macos_cross_icd(args.jobs)

    commit = run(["git", "-C", str(MESA), "rev-parse", "HEAD"], capture=True).stdout.strip()
    if commit != PINNED_MESA_COMMIT:
        raise RuntimeError(f"Mesa commit mismatch: expected {PINNED_MESA_COMMIT}, got {commit}")
    if platform.system() != "Linux":
        raise RuntimeError("The configured build and generators require builder Linux")
    lock = Path("/mnt/mac/Users/you/.tmp/build.lock")
    if not lock.is_file() or json.loads(lock.read_text()).get("pid") != os.getppid():
        raise RuntimeError("Run this build through Resources/Scripts/with-build-lock")
    if shutil.disk_usage(PROJECT).free < 40 * 1024**3:
        raise RuntimeError("Less than 40 GiB free; refusing to rebuild")
    dirty, allowed, shader_qualification = qualified_mesa_changes()
    previous = json.loads(REPORT.read_text()) if REPORT.exists() else {}
    source_snapshot = {
        str(path.relative_to(MESA)): sha256(path)
        for base in (MESA / "src", MESA / "include")
        for path in base.rglob("*")
        if path.is_file() and path.suffix in {".c", ".cpp", ".h", ".hpp", ".inc", ".py", ".xml"}
    }

    options = {
        "platforms": ["wayland"],
        "gallium_drivers": [],
        "vulkan_drivers": ["panfrost"],
        "build_tests": False,
        "tools_option": [],
        "precomp_compiler": "enabled",
        "install_precomp_compiler": False,
        "native_target": "aarch64-linux-gnu",
        "window_system": "Wayland and DRM KMS display",
    }
    started = datetime.now(timezone.utc).isoformat()
    phase = "setup"
    try:
        setup_options = [
            "--buildtype=release",
            "--wrap-mode=nodownload",
            *MESON_OPTIONS,
        ]
        if args.skip_build:
            if not (BUILD / "meson-private" / "coredata.dat").is_file():
                raise RuntimeError(f"No configured build directory at {BUILD}")
        elif (BUILD / "meson-private" / "coredata.dat").is_file():
            run(["meson", "configure", str(BUILD), *MESON_OPTIONS])
        else:
            run(["meson", "setup", str(BUILD), str(MESA), *setup_options])

        targets = json.loads(
            run(["meson", "introspect", str(BUILD), "--targets"], capture=True).stdout
        )
        target_names = {target.get("name") for target in targets}
        wanted = {"vulkan_panfrost", "panfrost_compile", "bifrost_compiler"}
        missing = wanted - target_names
        if missing:
            raise RuntimeError(f"Meson did not define required build targets: {sorted(missing)}")

        if not args.skip_build:
            phase = "build"
            run(
                [
                    "meson",
                    "compile",
                    "-C",
                    str(BUILD),
                    "--jobs",
                    str(args.jobs),
                    "vulkan_panfrost",
                    "panfrost_compile",
                    "bifrost_compiler",
                ]
            )

        phase = "offline_shader_compile"
        compiler = artifact_for(targets, "panfrost_compile")
        disassembler = artifact_for(targets, "bifrost_compiler")
        panvk = artifact_for(targets, "vulkan_panfrost")
        shader = compile_smoke(compiler, disassembler)
        report = {
            "schema": 1,
            "status": "passed",
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            "started_utc": started,
            "mesa_commit": commit,
            "mesa_tree": str(MESA),
            "mesa_tracked_source_changes_before_build": bool(dirty),
            "tracked_source_changes": dirty,
            "tested_source_sha256": allowed,
            "compiler_portability_qualification": shader_qualification,
            "source_snapshot_sha256": hashlib.sha256(json.dumps(source_snapshot, sort_keys=True).encode()).hexdigest(),
            "previous_icd": previous.get("artifacts", {}).get("panvk_icd"),
            "previous_configuration": previous.get("options"),
            "host": {
                "system": platform.system(),
                "release": platform.release(),
                "machine": platform.machine(),
                "cc_target": run(["cc", "-dumpmachine"], capture=True).stdout.strip(),
                "meson_version": tool_version("meson", ["--version"]),
                "ninja_version": tool_version("ninja", ["--version"]),
                "libdrm_version": tool_version("pkg-config", ["--modversion", "libdrm"]),
            },
            "build_directory": str(BUILD),
            "options": options,
            "build_jobs": args.jobs,
            "artifacts": {
                "panvk_icd": {"path": str(panvk), "bytes": panvk.stat().st_size, "sha256": sha256(panvk)},
                "panfrost_compile": {
                    "path": str(compiler),
                    "bytes": compiler.stat().st_size,
                    "sha256": sha256(compiler),
                },
                "bifrost_compiler": {
                    "path": str(disassembler),
                    "bytes": disassembler.stat().st_size,
                    "sha256": sha256(disassembler),
                },
            },
            "offline_shader_check": shader,
            "device_runtime_executed": False,
            "hardware_usable": False,
            "limitations": [
                "The shader check exercises the Mesa arch-9 Valhall compiler and disassembler only.",
                "It does not establish compatibility with the stock T70Pro kbase kernel ABI.",
                "No Mali device node was opened and no PanVK Vulkan runtime was launched.",
                "A successful native build does not establish boot, display, memory-management, or application support.",
            ],
        }
        for name, digest in source_snapshot.items():
            if sha256(MESA / name) != digest:
                raise RuntimeError("A Mesa source changed while the build lock was held: " + name)
        configured = json.loads((BUILD / "meson-info/intro-buildoptions.json").read_text())
        actual_platforms = next(x["value"] for x in configured if x["name"] == "platforms")
        if actual_platforms != ["wayland"]:
            raise RuntimeError("The retained build does not enable the requested Wayland platform")
        report["ELF_verification"] = inspect_linux_icd(panvk)
        if "libwayland-client.so.0" not in report["ELF_verification"]["needed"]:
            raise RuntimeError("The linked ICD has no Wayland client dependency")
        report["artifacts_match_current_sources"] = True
        report["window_system_build"] = {"Wayland": True, "DRM_KMS": True,
                                         "X11": False, "display_executed": False}
        removed_files = removed_bytes = 0
        for path in BUILD.rglob("*"):
            if path.is_file() and path.suffix in {".o", ".a"}:
                removed_bytes += path.stat().st_size
                path.unlink()
                removed_files += 1
        report["intermediate_cleanup"] = {"removed_files": removed_files,
                                           "removed_bytes": removed_bytes}
        REPORT.parent.mkdir(parents=True, exist_ok=True)
        REPORT.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Wrote {REPORT}")
        return 0
    except Exception as error:
        failure = {
            "schema": 1,
            "status": "failed",
            "phase": phase,
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            "mesa_commit": commit,
            "build_directory": str(BUILD),
            "error": str(error),
            "device_runtime_executed": False,
        }
        failure_path = REPORT.with_name("panvk-build-failure.json")
        failure_path.parent.mkdir(parents=True, exist_ok=True)
        failure_path.write_text(json.dumps(failure, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        raise


if __name__ == "__main__":
    sys.exit(main())
