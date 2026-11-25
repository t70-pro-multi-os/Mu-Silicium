#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
source = pathlib.Path(__file__).resolve().parents[1] / "LteInspect.c"
source_text = source.read_text()
forbidden_calls = ("ioctl", "open", "system", "popen", "fork", "execve", "execl", "execv")
for name in forbidden_calls:
    import re
    assert re.search(r"\b" + name + r"\s*\(", source_text) is None, name
assert source_text.count("dev/ccci_monitor") == 1

def put(root, rel, data=""):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data)

def run(root):
    return subprocess.run([str(binary), "--root", str(root)], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=3, check=True).stdout

with tempfile.TemporaryDirectory(prefix="t70-lte-fixture-") as tmp:
    root = pathlib.Path(tmp)
    for name in ("ccci_md_all", "ccci_ccif"):
        (root / "sys/module" / name).mkdir(parents=True)
    put(root, "proc/421/comm", "ccci_mdinit\n")
    put(root, "proc/422/comm", "rild\n")
    put(root, "sys/class/net/ccmni0/operstate", "up\n")
    put(root, "sys/class/net/ccmni0/carrier", "1\n")
    (root / "dev").mkdir()
    os.mkfifo(root / "dev/ccci_monitor")  # Any accidental open would hang until timeout.
    out = run(root)
    assert "CCCI core modules: 2/5 present" in out, out
    assert "CCCI monitor endpoint: present (stat only; never opened)" in out, out
    assert "ccci_mdinit process: present" in out, out
    assert "rild process: present" in out, out
    assert "CCMNI netdevs: 1 registered; 1 link-up, 0 link-down, 0 unknown" in out, out
    assert "CCCI modem FSM state: UNKNOWN" in out, out
    assert "LTE data session / SIM registration: NOT TESTED" in out, out

with tempfile.TemporaryDirectory(prefix="t70-lte-link-state-") as tmp:
    root = pathlib.Path(tmp)
    for iface, state, carrier in (
        ("ccmni0", "down", None),
        ("ccmni1", "lowerlayerdown", "1"),
        ("ccmni2", None, "0"),
        ("ccmni3", "up", None),
        ("ccmni4", "dormant", "1"),
        ("ccmni5", "up", "1"),
    ):
        if state is not None:
            put(root, f"sys/class/net/{iface}/operstate", state + "\n")
        if carrier is not None:
            put(root, f"sys/class/net/{iface}/carrier", carrier + "\n")
    out = run(root)
    assert "CCMNI netdevs: 6 registered; 1 link-up, 3 link-down, 2 unknown" in out, out
    assert "LTE data session / SIM registration: NOT TESTED" in out, out

with tempfile.TemporaryDirectory(prefix="t70-lte-complete-value-") as tmp:
    root = pathlib.Path(tmp)
    put(root, "proc/421/comm", "ccci_mdinit\nother\n")
    put(root, "proc/422/comm", "rild helper\n")
    put(root, "sys/class/net/ccmni0/operstate", "up extra\n")
    put(root, "sys/class/net/ccmni0/carrier", "1\n")
    out = run(root)
    assert "ccci_mdinit process: absent" in out, out
    assert "rild process: absent" in out, out
    assert "CCMNI netdevs: 1 registered; 0 link-up, 0 link-down, 1 unknown" in out, out

with tempfile.TemporaryDirectory(prefix="t70-lte-empty-") as tmp:
    out = run(pathlib.Path(tmp))
    assert "CCCI core modules: 0/5 present" in out, out
    assert "CCCI monitor endpoint: absent" in out, out
    assert "CCMNI netdevs: 0 registered" in out, out
    assert "LTE data session / SIM registration: NOT TESTED" in out, out


with tempfile.TemporaryDirectory(prefix="t70-lte-stock-fsm-") as tmp:
    root = pathlib.Path(tmp)
    rel = "sys/kernel/ccci/boot"
    for data, expected in (
        ("md1:4\n", "CCCI modem FSM state: 4 (raw stock sysfs value)"),
        ("md1:0\n", "CCCI modem FSM state: 0 (raw stock sysfs value)"),
        ("md1:n/a\n", "CCCI modem FSM state: UNKNOWN"),
        ("md1:4\nmd2:6\n", "CCCI modem FSM state: UNKNOWN"),
        ("md1:4 junk\n", "CCCI modem FSM state: UNKNOWN"),
        ("md1:+4\n", "CCCI modem FSM state: UNKNOWN"),
        ("md1:2147483648\n", "CCCI modem FSM state: UNKNOWN"),
        ("md1:" + "4"*64 + "\n", "CCCI modem FSM state: UNKNOWN"),
    ):
        put(root, rel, data)
        out = run(root)
        assert expected in out, (data, out)
        assert "LTE data session / SIM registration: NOT TESTED" in out, out
    boot = root / rel
    boot.unlink()
    (root / "fake").write_text("md1:4\n")
    boot.symlink_to(root / "fake")
    assert "CCCI modem FSM state: UNKNOWN" in run(root)
    boot.unlink()
    os.mkfifo(boot)
    assert "CCCI modem FSM state: UNKNOWN" in run(root)

print("PASS: read-only inventory fixtures; monitor FIFO stayed unopened")
