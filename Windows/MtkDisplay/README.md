# T70Pro Windows display-only driver

`MtkDisplay.sys` is a software display-only miniport for an inherited BGRX8
scanout. It is based on Microsoft's MIT-licensed KMDOD DDI/mode-management
code, pinned in `upstream.json`. All sources and outputs stay in builder.

The driver binds only to `ACPI\HANA678C`. Start validates the exact `_HID`
and a bounded 48-byte `DFBI` buffer twice, across
`DxgkCbAcquirePostDisplayOwnership`. Base, pitch, size, geometry and byte order
must agree with the POST owner. No panel, DSI, GPU, clock, PHY or secure-monitor
register address is guessed. GOP publishes this information only at EBS after
live DT/reservation, owned mode and uncached memory-attribute checks. The GOP
producer is included in the FV and retries after runtime-FDT publication.
It only publishes validated live geometry at EBS; stock LK video tags alone
provide no pitch/format, so they do not enable this ACPI device.

Implemented paths:

- Reserve/map the validated POST framebuffer as UC, blank it before source
  visibility, maintain black output while hidden, and release the mapping on stop.
- One internal target at the inherited native resolution; native and transposed
  32-bit source modes. Validate pinned modes and transformations before changing
  the active mapping. Unsupported modes preserve the existing framebuffer.
- Synchronous software present with 0/90/180/270 degree counter-clockwise rotation,
  scroll destinations, dirty rectangles, a full repaint after visibility/mode
  changes, and bounded work for overlapping rectangles. Snapshot and validate
  every rectangle before any write; probe/lock/map the complete pitched source
  and unwind allocations/MDLs on failures.
- A passive-level per-device mutex serializes level-zero presents with mode,
  visibility and lifecycle updates. Crash-display callbacks remain nonpaged and
  never wait for that mutex.
- Child/status/capability, software-pointer fallback, crash screen, black POST
  ownership handback, and repeated stop. No fake interrupts, threads or VSync
  timings. No async completion is advertised.

Valid D1/D2/D3 notifications succeed while blanking and unmapping the inherited
buffer; all present/crash/visibility writes are gated. D0 reacquires POST ownership
and validates fresh ACPI/layout data before mapping again, including adapter and
monitor ordering. A failed reacquisition stays unmapped and reports the actual
status. Native geometry changes are rejected until mode rebuilding is supported.
These callbacks do not switch panel/DSI power or prove recovery from power loss.
Panel/DSI cold start, backlight control and Mali WDDM rendering remain absent.

Build: `../.venv/bin/python Windows/build.py --display` from the repository root
(use `/home/builder/teclast-t70pro/.venv/bin/python` for this workspace).
Tests: the same command with `--test`; the runner acquires the shared build lock.
The portable copy engine is run with ASan/UBSan. Actual ARM64 production DOD
callbacks run under Unicorn with kernel/Dxgk/ACPI/MDL mocks, including mode
creation, commit, four rotations, bad ACPI/layouts, pool/MDL/map failures,
crash output, and lifecycle cleanup. Golden pixel matrices anchor rotation to
the documented Windows direction. Exception dispatch itself is not emulated;
Windows SEH and real concurrent scheduling require Windows validation.

The package is unsigned. Signing, InfVerif/HLK, Windows loading, actual scanout,
performance, real IRQs, panel state and all device execution remain untested.
No installation or tablet state change is part of these offline commands.

Sources:
- https://github.com/microsoft/Windows-driver-samples/tree/47e616d951b7a5aa379d7727d394d9ddc08ebd3b/video/KMDOD
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_presentdisplayonly
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ne-d3dkmdt-_d3dkmdt_vidpn_present_path_rotation
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkcb_acquire_post_display_ownership

## Direct3D 11 and 12 software execution

`DirectX.cxx` builds `Windows/Build/T70DirectX.exe`, an ARM64 Windows application.
It explicitly selects Microsoft WARP (CPU) for D3D11 and D3D12. It compiles HLSL
vertex/pixel shaders, draws a full-screen triangle into a 64x64 RGBA target and
checks every staging/readback pixel. D3D12 records real resource transitions and
copy commands and waits for a queue fence before mapping. A timeout, device
removal or unconfirmed completion prevents reuse/release of submitted resources
until process teardown. Win32 failures with an empty last-error code remain errors.

```sh
# builder, repository root; existing SDK/toolchain only
../.venv/bin/python Windows/MtkDisplay/build-directx.py
# Windows ARM64, after the platform actually boots and provides a display
T70DirectX.exe
T70DirectX.exe --show11
T70DirectX.exe --show12
```

The optional window paths present a flip-discard double-buffered DXGI swap chain
for ten seconds, or until closed. No argument runs both offscreen draw/readback
checks. Exit 0 means both checks succeeded in that execution; 1 means failure;
64 means invalid arguments. DLL availability and feature creation errors are
returned unchanged. D3D12 WARP availability depends on the Windows image; no
Windows optional component is installed by the build or app.

The build verifies real ARM64 PE imports, including the SDK's ordinal import for
D3D12CreateDevice. The production pixel validator passes 6,188 ASan/UBSan checks;
the actual ARM64 app runs through 57 success/failure scenarios with explicit
Win32/DXGI/D3D/COM mocks. Those mocks validate API ordering, barriers, fence-before-
map, presentation, cleanup and in-flight resource retention. They do not execute
HLSL, WARP, a Windows runtime or the Mali GPU. Reports are
`Windows/Build/directx-{verification,tests}.json`.

This supplies an explicit software DirectX consumer and a future runtime check.
It does not turn the display-only driver into a Mali render driver or make every
application select WARP. Mali acceleration still requires a WDDM render miniport
and matching D3D user driver (scheduling, GPU MMU/contexts, shader compiler,
resource management, synchronization and power/reset). The stock Linux kbase
module and Android EGL/Vulkan binaries use different kernel/runtime interfaces.

Direct3D references:
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-a-basic-direct3d-12-component
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization
