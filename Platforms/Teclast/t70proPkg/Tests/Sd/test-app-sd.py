"""Run DeviceCheck on the QEMU SDHC card with a USB keyboard fixture."""
from pathlib import Path
import re, sys, runpy

q = Path(__file__).resolve().parents[1] / "Qemu"
sys.path.insert(0, str(q))
import firmware_probe as f

original = f.exercise

def exercise(r, m, b, o, mode, report):
    assert mode == "sd", "run with --sdhc --usb keyboard"
    fixture = report.get("media_fixture", {})
    assert fixture.get("card_type") == "SDHC" and fixture.get("bytes") == 4 * 1024**3, fixture
    assert any("usb-kbd,id=kbd" in arg for arg in report["command"]), "QEMU USB keyboard fixture missing"

    # Keep the general firmware checks, but leave SD-specific assertions to
    # this application-level test.
    original(r, m, b, o, "none", report)
    p = f.Probe(r, m, b, o)
    p.refresh()
    p.idle()
    # Check actual guest driver writes against the stock MSDC1 pad map, through
    # GpioDxe and the MT6789 implementation, rather than writing a ready state.
    pads = {}
    for name, address, mask, expected in (
        ("cmd_dat_input", 0x11ea0080, 0x7c, 0x7c),
        ("drive_code3", 0x11ea0030, 0x07fffe00, 0x036db600),
        ("clock_pulldown", 0x11ea00b0, 0x3f, 0x01),
        ("cmd_dat_pullup_r0", 0x11ea00e0, 0x3f, 0x3e),
        ("clock_pulldown_r1", 0x11ea00f0, 0x3f, 0x01),
        ("clock_mux1", 0x10005380, 0x70000000, 0x10000000),
        ("cmd_dat_mux1", 0x10005390, 0x00077777, 0x00011111),
    ):
        live = int.from_bytes(r.memory(address, 4), "little")
        assert live & mask == expected, (name, hex(live), hex(mask), hex(expected))
        pads[name] = dict(address=hex(address), live=hex(live), mask=hex(mask), expected=hex(expected))
    report["msdc1_pad_readback"] = pads
    report["usb_fixture"] = p.prepare_usb(report["usb_power_path_fixture"])
    r.resume_for(2)
    p.idle()
    p.call("DxeCore", "CoreConnectController",
           int(report["usb_fixture"]["handle"], 16), 0, 0, 1)
    r.resume_for(2)
    p.idle()

    # Select the embedded app. No DeviceCheck C/reconnect key is sent before
    # this first scan; its SD host and FAT partition must connect on entry.
    for _ in range(6):
        p.key("down")
    p.framebuffer("sd-menu-selected")
    selected = p.ui_hash
    p.key("ret")
    p.refresh()
    assert "T70DeviceCheck" in p.images, "embedded EFI diagnostic did not launch"

    def log(name):
        count = p.read64(p.address("T70DeviceCheck", "mDeviceCheckLogChars"))
        assert 0 < count < 32768, count
        text = r.memory(p.address("T70DeviceCheck", "mDeviceCheckLog"), count * 2).decode("utf-16le")
        (o / name).write_text(text)
        return text.replace("\r\n", "\n")

    def ready_report(text, connecting=True):
        assert re.search(r"^SD host1 detect=present/Success samples=[1-9][0-9]* path=Success$", text, re.M), text
        assert re.search(r"^SD driver=[^/]+/Success init=ready/Success ready=1 busy=0 attempts=[1-9][0-9]* fail=0$", text, re.M), text
        for required in (
            "SD/MMC host protocols: 1",
            "media-path=Success",
            "SD SUMMARY blocks=1 filesystems=1",
            "LBA0 read=Success",
        ):
            assert required in text, (required, text)
        if connecting:
            assert re.search(r"^SD host connect H[^:]+: Success$", text, re.M), text
            # ConnectController returns NOT_FOUND when no new driver starts;
            # a pre-existing FAT root and BOOTAA64 open below prove access.
            assert any("partition/filesystem connect: " + status in text
                       for status in ("Success", "Not Found")), text
        assert re.search(r"^Block H[^\n]*bus=SD part=0$", text, re.M), text
        assert re.search(r"^ present=1 readonly=1 block=512 ", text, re.M), text
        assert re.search(r"^FS H.*bus=SD root=Success\r?\n BOOTAA64.EFI open: Success$", text, re.M), text

    initial = log("sd-device-check-first-entry.txt")
    ready_report(initial)

    # A refresh may update cached status but must not call ConnectController.
    refresh_latency = p.key("r")
    assert refresh_latency is not None, "R did not return to the refreshed report"
    refreshed = log("sd-device-check-refresh.txt")
    ready_report(refreshed, connecting=False)
    assert "SD host connect H" not in refreshed, "R reconnected the SD host"
    assert "partition/filesystem connect:" not in refreshed, "R reconnected the SD filesystem"

    # Idle must leave both framebuffer contents and dirty-page count unchanged.
    before = p.framebuffer("sd-app-idle-before")
    p.dirty_pages()
    r.resume_for(1)
    idle_dirty = p.dirty_pages()
    after = p.framebuffer("sd-app-idle-after")
    assert idle_dirty == 0, f"idle diagnostic rewrote {idle_dirty} framebuffer pages"
    assert after == before, "idle framebuffer changed"

    p.key("esc")
    p.framebuffer("sd-menu-returned")
    assert p.ui_hash == selected, "ESC failed to restore the selected menu"
    p.key("ret")
    p.refresh()
    reopened = log("sd-device-check-reopened.txt")
    ready_report(reopened)
    p.key("esc")

    report["device_check_sd"] = dict(
        first_entry_auto_connect=True,
        detect_present_success=True,
        identify_ready_success=True,
        sd_blockio_filesystem=True,
        boot_file_open=True,
        refresh_without_reconnect=True,
        refresh_key_latency_ms=round(refresh_latency, 2),
        idle_dirty_pages=idle_dirty,
        idle_framebuffer_unchanged=True,
        ESC_return_and_reopen=True,
        usb_keyboard=True,
    )

f.exercise = exercise
runpy.run_path(str(q / "verify-qemu.py"), run_name="__main__")
