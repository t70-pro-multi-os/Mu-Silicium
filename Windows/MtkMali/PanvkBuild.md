# PanVK build and stock JM ABI checks

This build uses the single Mesa checkout pinned at
`58f2c55854bd48b18df756edb11f187af9cd2fde`. The retained native Meson configuration
has `platforms=[]` and DRM KMS display WSI. The current macOS cross-built ICD also
includes actual Wayland WSI, using this same build directory and builder Linux
libraries. Its effective platforms and the unchanged native configuration are
recorded separately. The native build script requests Wayland too; native Linux
execution is still pending because builder remains `starting`. Neither path installs files.

The native build and offline shader check use this command. Both build paths
accept only tracked changes matching the existing ABI/compiler test reports:

```sh
orb -m builder -w /home/builder/teclast-t70pro/Mu-Silicium \
  Resources/Scripts/with-build-lock T70Pro 'Build native AArch64 PanVK' -- \
  python3 Windows/MtkMali/build-panvk.py --jobs 6
```

The script stores build products under
`/home/builder/teclast-t70pro/mesa/build-t70pro-panvk` and records artifact
hashes and the compiler result in
`/home/builder/teclast-t70pro/analysis/panvk-build.json`. It assembles and
validates a small OpenCL 2.2 SPIR-V kernel, compiles it for Panfrost
architecture 9, extracts the emitted Valhall code from the generated Mesa
precompiled-kernel blob, and disassembles that code using the G57 model
selector. This is an offline compiler test; it does not open a Mali device
node.

The output establishes that this Mesa revision builds on the builder and that
its arch-9 compiler emits disassemblable Valhall code. It does not prove that
the PanVK kbase backend matches the stock T70Pro kernel ABI, that a Vulkan
runtime can initialize the tablet, or that graphics applications work. No
driver files are installed and no device runtime is launched.


## Stock JM ABI checks after the native build

`test-kbase-abi.py` tests the actual shared `kbase_jm_abi.h` used by Mesa's
`kbase_kmod.c`. Run it with the existing Unicorn Python environment and
`T70_MALI_ZIG` pointing to the existing Zig executable. It acquires the same
build lock itself:

```sh
python3 Windows/MtkMali/test-kbase-abi.py
```

The probe now requires a fresh context and exactly one successful completion
with the expected atom number and both user-data words. Submit errors, EOF,
malformed/duplicate/unrelated/fault events and exhausted bounded retries fail
device creation. There is no assumed layout on failure. Constructor failure
keeps fd ownership with its caller, and the tracking mapping is released on
both failure and device destruction.

Native ASan/UBSan and ARM64 test-PE execution each pass 10,007 checks of the
production packing/probe core with mocked submission and event I/O. Selected
original T70Pro ARM64 instructions compare 512 atom normalizations, 8,704
semantic-field loads, 792 entry stride/count guards and 1,024 completion
records, plus 128 pending-cookie release cases. They establish that this stock parser accepts a 64-byte v2 atom or a
72-byte seq_nr-first atom; the dependency-only 64-byte probe selects v2.
Copy/object-size checks, locks and wakeups are explicit mocks. Full kernel
submission, nonzero renderpass/frame extensions, external-resource ownership
and real GPU execution remain outside this check.

The allocator now chooses direct VA versus mmap-cookie handling from returned
flags, including executable allocations falling back to SAME_VA when the exec
zone is unavailable. Shader permission and backing lifetime are separate. Failed
anchor mappings release their pending handle; import failure and oversized or
wrapped sizes are checked before publishing a BO. The actual original backend
is included in a freestanding AArch64 Linux test ELF, retaining its real
allocate/map/free/import/export and shared DMA-heap allocation code. It passes
364 checks with mocked ioctl, fd/open/close/dup, CPU mappings, the public import
lock boundary, allocator, registry growth and logging. This includes both VA zones,
executable fallback, second CPU views, coherence flags, allocation failures,
pending-handle cleanup, malformed import sizes, registry failure, independent
CLOEXEC exports after caller close, heap/import/export failures and cleanup.

The actual complete `kbase_kmod.c` translation unit also cross-compiles to an
AArch64 Linux ELF object using the existing generated Mesa configuration and
libdrm header. The changed Vulkan memory allocator and buffer/image memory
requirements callers also cross-compile as three complete AArch64 Linux objects.
This check uses existing host tools and does not start the VM.
All temporary objects are deleted; the ARM64 test PE, Linux test ELF and source/artifact
hashes are recorded in `analysis/panvk-kbase-abi-verification.json`.

## Current-source macOS cross rebuild

The retained `libvulkan_panfrost.so` now includes the tested JM ABI fixes and
the shared-memory allocation/export path and actual Wayland WSI below.
The complete ICD dependency graph was rebuilt: 956 AArch64 Linux objects and
62 archives, reusing 128 generated inputs from this same configured build.
Apple Clang uses the mounted builder Linux headers and GCC C++ headers;
the existing Zig AR/LLD tools link against the actual builder ELF libraries
and GNU startup objects. GCC-only feature flags are omitted, including the
FPCR builtin flags: Mesa's existing real AArch64 MRS/MSR fallback is compiled.

```sh
T70_PROJECT=/Users/you/OrbStack/builder/home/builder/teclast-t70pro
PYTHONDONTWRITEBYTECODE=1 \
T70_MALI_ZIG=/private/tmp/t70-mali-python/ziglang/zig \
python3 "$T70_PROJECT/Mu-Silicium/Resources/Scripts/with-build-lock" \
  T70Pro 'Cross rebuild current ARM64 Linux PanVK ICD' -- \
  python3 "$T70_PROJECT/Mu-Silicium/Windows/MtkMali/build-panvk.py" \
  --macos-cross-icd --jobs 6
```

The cross path requires the parent-owned build lock, the pinned Mesa commit,
the tested ABI source hashes and the recorded previous ICD hash. It rejects
other tracked changes and missing generated inputs. It records source/header,
generated-input and link-runtime fingerprints and checks that they did not
change during the build. Cache reuse requires matching command, input-snapshot
and actual object hashes. The new ELF must be an AArch64 shared object with
the expected SONAME, all three Vulkan ICD loader entrypoints, valid segments
and no writable executable load segment before it replaces the old ICD. It also
requires the actual Wayland surface/initialization/helper and seven protocol
symbols, the Linux client SONAME and five real Wayland 1.24 API imports.
Generated object/archive files and the resume state are deleted after success.

`analysis/panvk-build.json` preserves the original native compiler/ISA check
and records the separate current-source cross relink, actual ICD hash, ELF
dependencies and cleanup. The earlier standalone shader compiler binaries
are unchanged and their offline ISA check was not repeated. The upstream Wayland
scanner and an ASan/UBSan protocol metadata test execute on macOS. No Linux process,
Vulkan runtime, kernel ioctl, GPU job or display operation executes in this cross
build. Compositor/desktop packaging and actual display execution remain open.

### Wayland sources and protocol qualification

The Android-focused PanVK subset omitted `src/loader` and `vulkan_wayland.h`.
The original helper C/header and Vulkan header were restored from Mesa commit
`0c11a9cfb35595e404913691241a2a2a0f0a8f03`, keeping their license headers unchanged.
Only the loader Meson file is narrowed to the shipped Wayland helper; unused
DRI/GL loader targets are omitted. Their four exact source hashes are pinned
in the existing build script and included in its qualifications.

The official Wayland 1.24.0 release is built once as the native scanner in
`tools/wayland`, with libraries/tests/docs/DTD validation disabled. It uses Expat;
it is an actual upstream parser, not a replacement protocol generator. Release
SHA-256 is `82892487a01ad67b334eca83b54317a7c86a03a89cfadacfef5211f11a5d0536`.
The existing builder Wayland 1.24.0 client library and protocol XML are used directly.

The scanner emits client headers and private C protocol tables for fifo,
commit timing, linux-dmabuf, presentation time, tearing control, DRM syncobj
and color management. A separate XML parser creates expected metadata; a native
ASan/UBSan executable checks the actual generated C tables, including interface
versions, message names/signatures and every object argument type. It passes
1,006 checks across 46 interfaces and 228 messages, including the core protocol.
Malformed XML is separately rejected. This checks wire metadata and C ABI, not
compositor behavior or hardware support for every advertised protocol.

Nine added objects (WSI, helper and seven protocol tables) are linked into the
same ICD. All objects use `VK_USE_PLATFORM_WAYLAND_KHR`, keeping dispatch table
layouts consistent. The original native Ninja graph is preserved; cross-build
extensions are in memory and include actual source/generated/runtime hashes.
The current ICD is 23,962,048 bytes, SHA-256
`8861883092b41b57132dbbda25afe08ff9c361d1f9f2c5c82aa6fcfc03a98c3f`.
Its 956 objects and 62 archives were removed after qualification (1,018 files,
42,502,770 bytes); one current ICD, generator inputs and protocol test remain.


## Shared Linux GPU buffers

Vulkan exportable allocations on kbase now allocate backing through
`DMA_HEAP_IOCTL_ALLOC` on `/dev/dma_heap/system`, then import the dma-buf into
the GPU address space. An unavailable heap or failed allocation/import returns
an error. A private kbase allocation is never substituted for shared memory.
The stock module inventory includes `system_heap`; its actual node registration
and cross-device sharing have not been executed in the new Linux environment.

Every import retains its own `F_DUPFD_CLOEXEC` descriptor. Each export duplicates
that retained descriptor independently, so the caller may close the input or a
previous export without invalidating the BO or other exports. Failure cleanup
closes only owned descriptors and releases pending imports; a successful BO
releases its retained descriptor at destruction. The two import ioctl shapes
are tried without a process-global mutable ABI selection.

Imported and heap-backed BOs are device-local only until exporter CPU-cache
maintenance is implemented. Vulkan external buffer/image memory requirements
exclude host-visible types, and allocation rejects an incompatible memory type
or backing smaller than the requested allocation. This policy also reaches
`vkGetMemoryFdPropertiesKHR` through the BO's `NO_MMAP` flag. Normal private
kbase allocations retain their existing host-map path.

The 364-check ARM64 test ELF exercises actual production allocation/import/
export code with explicit OS mocks. The full 947-object, 62-archive Linux ICD
rebuild and ELF link checks passed with these changes. DRM display WSI is
compiled; the earlier description that all WSI was disabled was incorrect.
Wayland WSI is now linked; compositor/desktop packaging, X11 and actual cross-device sharing remain open.

## Display synchronization

The existing common WSI path exports dma-buf acquire fences into Vulkan and
imports render-complete semaphore fences back into the displayed dma-buf. It
is reused rather than replaced by a second synchronization scheme. The kbase
provider now distinguishes POLLIN completion from POLLERR/POLLHUP/POLLNVAL,
retries interrupted polling, rejects invalid negative descriptors and retains
independent CLOEXEC duplicates. A failed transfer leaves the destination
unchanged. The real JM queue returns device loss if its semaphore wait fails,
before submitting GPU atoms. After WAIT_AVAILABLE, the JM queue separately waits
for imported compositor fences to actually signal. An available fence is not
necessarily complete. Internal submitted GPU signals retain their context ordering
and do not force an extra CPU wait. The test covers this distinction explicitly.

A Vulkan SYNC_FD value of -1 represents an already-completed signal. Common WSI
now skips both the kernel import and close for that value; previously an idle
present could submit fd -1 to the dma-buf ioctl. See the
[Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
and [dma-buf synchronization documentation](https://www.kernel.org/doc/html/latest/driver-api/dma-buf.html).

The production provider and common WSI bridge are executed together in an
ARM64 test ELF: 124 assertions cover pending/complete signals, error events,
interrupted polling, descriptor exhaustion, independent lifetimes, transfers,
merge/attach, WSI import failure cleanup and the real JM submit/event loop.
Both opaque event words must match the current atom owner. Atom reuse, stale
faults, duplicate events, detached fence atoms and out-of-order completions
cannot retire a later submission or release its buffers early. The pre-fix
production source failed the new completion/lifetime fixture. OS fd, read,
poll, clock, thread, allocator and ioctl services are explicit mocks. Complete WSI and arch-9 JM translation
units also compile. The current 947-object/62-archive Linux ICD is relinked
and ELF-checked with these changes; no GPU or display operation is executed.

## Submission failure and command-buffer lifetime

The actual JM queue now marks the Vulkan device lost when an asynchronous GPU
chunk faults, a chunk submission is refused, or synchronous debug submission
fails. Later queue submission is rejected on a lost device. Failed work does
not publish event SET/RESET operations; application fences are released only
after accepted chunks finish, with device loss already visible.

Every chunk and the submitting thread hold a reference to the same completion
record. Reverse-order completion and callbacks before submit returns cannot
release descriptors, refresh command storage or application fences early.
A refused chunk stops assembly, drains accepted chunks and then releases the
record and refresh lock. ORDER dependencies do not have to propagate a prior
chunk's fault for the application to observe it.

The new fixture includes the complete production JM queue source. Forty
ARM64 checks cover first/middle/final rejection, earlier-chunk faults,
reverse-order and immediate callbacks, allocation failure, refresh locking,
synchronous failure/success and rejection of submission after device loss.
All kmod, allocation, sync, cache-flush and logging boundaries are explicit
mocks. The pre-fix source failed at the fixture's device-loss assertion.
This checks queue control flow and ownership, not GPU scheduling or pixels.

Wayland build dependencies were installed in builder. Its local execution and
SSH channels subsequently timed out. A later `running` interval allowed a
simple Linux command, but the Wayland build request never created a build
marker or log output and was canceled. A builder-only normal stop succeeded;
its start request then timed out after 25 seconds with state `starting`. The native Wayland build is pending, while the current retained
DRM configuration and offline tests are verified. A native build failure will
write `analysis/panvk-build-failure.json` without replacing the previous success
record. Native sources are checked for changes while the build lock is held.
