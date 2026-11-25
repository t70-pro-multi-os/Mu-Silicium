# RAM-only GOP screenshot capture

`CaptureBmp(Bs, Gop, &Buffer, &Size)` captures the visible screen **at call time**
using GOP EfiBltVideoToBltBuffer. It does not reuse the DeviceCheck pre-app
screen-restoration frame. No console drawing, storage access or automatic export.

When the console has no GOP, the helper can use a unique usable GOP, or
multiple handles that demonstrably describe the same framebuffer and mode.
It refuses competing displays; the fallback never chooses an arbitrary screen.
The preferred explicit console GOP is unchanged. This selection is capture-only,
not a console switch, and does not change the caller's saved screen or mode.

Success returns an owned BootServicesData BMP buffer; caller must FreePool it.
Failure returns NULL/0. Use fresh output variables and replace an existing
screenshot only after success; this helper does not manage the previous buffer.
Capture before drawing an export dialog if the intended screenshot is the
diagnostic page. UI hotkey and manual-export integration are not implemented here.
Dirty DeviceCheck source/INF remain untouched.

Encoding: Windows BMP 14-byte file header + 40-byte BITMAPINFOHEADER, 24-bit BGR,
positive height/bottom-up rows, zero 4-byte row padding, BI_RGB/no compression.
GOP handles framebuffer format conversion, including PixelBltOnly; this module
never reads framebuffer memory or assumes PixelsPerScanLine equals width.
Bounds: dimensions 1..4096 each, at most 128 MiB combined capture/BMP allocation.
It rejects mode geometry changes observed during capture, but does not guarantee
an atomic display snapshot if other code draws concurrently.

Native mocks verify all four row-padding cases, header/size, BGR ordering,
vertical orientation, allocation and BLT failure cleanup, changed mode and bounds.
No real screenshot/device test performed. This is a reusable component, not a
standalone EFI app or a claim that a screenshot can currently be exported.

    cc -fshort-wchar -Wall -Wextra -Werror -I"$MDE" -I"$MDE/AArch64" -I. CaptureBmp.c ../../Tests/DeviceCheck/CaptureBmpTest.c -o /tmp/t70-capture-test
    /tmp/t70-capture-test

MDE is the existing MdePkg/Include path. No third-party libraries are needed.

Canonical source moved from app commit ab235e7. DeviceCheck C/INF and
UI/export integration are not changed by this ownership transfer.
