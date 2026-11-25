#!/usr/bin/env python3
"""Synthetic checks for the T70 connectivity handoff helper; no host devices used."""
from __future__ import annotations

import os
import json
import hashlib
import signal
import sys
import time
import socket
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "connectivity-services.sh"


class ConnectivityServiceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="t70-connectivity-")
        self.root = Path(self.temp.name)
        for name in ("sys", "dev", "run", "bin", "etc"):
            (self.root / name).mkdir()
        self.env = os.environ.copy()
        self.env["TECLAST_CONNECTIVITY_TEST_ROOT"] = str(self.root)
        self.env["PATH"] = str(self.root / "bin") + os.pathsep + self.env.get("PATH", "")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def install_fake(self, name: str, source: str) -> Path:
        path = self.root / "bin" / name
        path.write_text(source, encoding="utf-8")
        path.chmod(0o755)
        return path

    def make_wifi(self) -> None:
        (self.root / "sys/module/wlan_drv_gen4m_6789").mkdir(parents=True)
        (self.root / "sys/class/ieee80211/phy0").mkdir(parents=True)
        (self.root / "sys/class/net/wlan0/wireless").mkdir(parents=True)

    def make_socket(self, relative: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.bind(str(path))
        finally:
            sock.close()

    def install_wmt_control(self, ready_passes: int = 20) -> Path:
        count = self.root / "wmt-ready-count"
        calls = self.root / "wmt-calls"
        self.env["TECLAST_TEST_WMT_READY_COUNT"] = str(count)
        self.env["TECLAST_TEST_WMT_READY_PASSES"] = str(ready_passes)
        self.env["TECLAST_TEST_WMT_CALLS"] = str(calls)
        return self.install_fake("teclast-wmt-control", """#!/bin/sh
case "$1" in
  --ready)
    n=$(cat "$TECLAST_TEST_WMT_READY_COUNT" 2>/dev/null || echo 0)
    n=$((n + 1)); echo "$n" > "$TECLAST_TEST_WMT_READY_COUNT"
    [ "$n" -le "$TECLAST_TEST_WMT_READY_PASSES" ];;
  --wifi-on|--wifi-off)
    echo "$1" >> "$TECLAST_TEST_WMT_CALLS"
    exit 0;;
  *) exit 64;;
esac
""")

    def run_helper(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(HELPER), *args],
            env=self.env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_cfg80211_phy_link_identifies_netdev_without_wireless_extensions(self) -> None:
        self.make_wifi()
        netdev = self.root / "sys/class/net/wlan0"
        (netdev / "wireless").rmdir()
        (netdev / "phy80211").symlink_to("../../ieee80211/phy0")
        self.install_fake("wpa_supplicant", "#!/bin/sh\nexit 0\n")
        probe = self.run_helper("probe")
        self.assertEqual(probe.returncode, 0, probe.stderr)
        self.assertIn("wifi.netdev=present\n", probe.stdout)
        self.assertIn("wifi.service_handoff=eligible-not-tested\n", probe.stdout)

    def test_phy_link_must_identify_a_registered_wiphy(self) -> None:
        self.make_wifi()
        netdev = self.root / "sys/class/net/wlan0"
        (netdev / "wireless").rmdir()
        for remote in ("../../ieee80211/phy9", ".."):
            with self.subTest(remote=remote):
                (netdev / "phy80211").symlink_to(remote)
                probe = self.run_helper("probe")
                self.assertEqual(probe.returncode, 0, probe.stderr)
                self.assertIn("wifi.netdev=missing\n", probe.stdout)
                (netdev / "phy80211").unlink()

    def test_probe_keeps_wifi_socket_separate_from_wmt_control(self) -> None:
        socket_path = self.root / "run/wpa_supplicant/wlan0"
        for ready in (False, True):
            self.install_wmt_control(ready_passes=20 if ready else 0)
            for present in (False, True):
                with self.subTest(wmt_ready=ready, socket_present=present):
                    if present:
                        self.make_socket("run/wpa_supplicant/wlan0")
                    result = self.run_helper("probe")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    fields = dict(line.split("=", 1) for line in result.stdout.splitlines())
                    self.assertEqual(fields["wmt.ready"], "yes" if ready else "no")
                    self.assertEqual(fields["wifi.control_socket"],
                                     "present" if present else "not-observed")
                    self.assertFalse(Path(self.env["TECLAST_TEST_WMT_CALLS"]).exists())
                    if present:
                        socket_path.unlink()

    def test_wifi_power_requires_modules_and_preserves_control_failure(self) -> None:
        self.assertEqual(self.run_helper("wifi-power-on").returncode, 69)
        self.make_wifi()
        for name in ("wmt_drv", "wmt_chrdev_wifi"):
            (self.root / "sys/module" / name).mkdir(parents=True)
        (self.root / "dev/wmtWifi").symlink_to("/dev/null")
        control = self.install_wmt_control()
        control.write_text('#!/bin/sh\n[ "$1" = --ready ] && exit 0\n[ "$1" = --wifi-on ] && exit 37\nexit 9\n', encoding="utf-8")
        self.assertEqual(self.run_helper("wifi-power-on").returncode, 37)
        control.write_text('#!/bin/sh\n[ "$1" = --ready ] && exit 0\n[ "$1" = --wifi-on ] || exit 9\n', encoding="utf-8")
        self.assertIn("driver_interfaces=registered", self.run_helper("wifi-power-on").stdout)
        control.write_text('#!/bin/sh\n[ "$1" = --wifi-off ] || exit 9\n', encoding="utf-8")
        self.assertEqual(self.run_helper("wifi-power-off").returncode, 0)
        self.assertEqual(self.run_helper("wifi-power-on", "extra").returncode, 64)

    def test_wifi_power_does_not_succeed_if_wmt_stops_before_confirmation(self) -> None:
        self.make_wifi()
        for name in ("wmt_drv", "wmt_chrdev_wifi"):
            (self.root / "sys/module" / name).mkdir(parents=True)
        (self.root / "dev/wmtWifi").symlink_to("/dev/null")
        self.install_wmt_control(ready_passes=2)
        result = self.run_helper("wifi-power-on")
        self.assertEqual(result.returncode, 69, result.stderr)
        self.assertNotIn("driver_interfaces=registered", result.stdout)
        self.assertIn("stopped before Wi-Fi registration", result.stderr)
        calls = Path(self.env["TECLAST_TEST_WMT_CALLS"])
        self.assertEqual(calls.read_text().splitlines(), ["--wifi-on"])

    def test_wifi_power_requires_live_wmt_before_power_request(self) -> None:
        self.make_wifi()
        for name in ("wmt_drv", "wmt_chrdev_wifi"):
            (self.root / "sys/module" / name).mkdir(parents=True)
        (self.root / "dev/wmtWifi").symlink_to("/dev/null")
        self.install_wmt_control(ready_passes=0)
        result = self.run_helper("wifi-power-on")
        self.assertEqual(result.returncode, 69, result.stderr)
        self.assertIn("run wmt-run first", result.stderr)
        calls = Path(self.env["TECLAST_TEST_WMT_CALLS"])
        self.assertFalse(calls.exists())

    def test_bluez_requires_hci_and_dbus_then_preserves_exit(self) -> None:
        self.assertEqual(self.run_helper("bt-service-run").returncode, 69)
        (self.root / "sys/class/bluetooth/hci0").mkdir(parents=True)
        self.assertIn("D-Bus", self.run_helper("bt-service-run").stderr)
        self.make_socket("run/dbus/system_bus_socket")
        self.install_fake("bluetoothd", '#!/bin/sh\n[ "$#" = 1 ] && [ "$1" = --nodetach ] || exit 9\nexit 29\n')
        self.assertEqual(self.run_helper("bt-service-run").returncode, 29)

    def test_dbus_foreground_and_runtime_id_generation(self) -> None:
        (self.root / "etc/teclast").mkdir()
        (self.root / "etc/teclast/dbus-system.conf").write_text("synthetic-only")
        self.install_fake("dbus-uuidgen", '#!/bin/sh\n[ "$1" = "--ensure=$TECLAST_CONNECTIVITY_TEST_ROOT/run/dbus/machine-id" ] || exit 9\n')
        self.install_fake("dbus-daemon", '#!/bin/sh\n[ "$1" = --nofork ] && [ "$2" = --nopidfile ] && [ "$3" = --nosyslog ] || exit 9\nexit 27\n')
        self.assertEqual(self.run_helper("dbus-run").returncode, 27)
        self.make_socket("run/dbus/system_bus_socket")
        self.assertEqual(self.run_helper("dbus-run").returncode, 73)

    def test_wmt_requires_module_and_control_nodes(self) -> None:
        result = self.run_helper("wmt-run")
        self.assertEqual(result.returncode, 69)
        self.assertIn("WMT module", result.stderr)
        (self.root / "sys/module/wmt_drv").mkdir(parents=True)
        self.assertIn("detector node", self.run_helper("wmt-run").stderr)

    def test_wmt_service_preserves_exit_status(self) -> None:
        (self.root / "sys/module/wmt_drv").mkdir(parents=True)
        (self.root / "dev/wmtdetect").symlink_to("/dev/null")
        self.assertFalse((self.root / "dev/stpwmt").exists())
        self.install_fake("teclast-wmt-control", '#!/bin/sh\n[ "$#" = 1 ] && [ "$1" = --run ] || exit 99\nexit 31\n')
        result = self.run_helper("wmt-run")
        self.assertEqual(result.returncode, 31, result.stderr)
        self.assertIn("power=driver-managed", result.stdout)
        self.assertEqual(self.run_helper("wmt-run", "unexpected").returncode, 64)

    def test_bt_requires_kernel_transport_and_HCI_modules(self) -> None:
        result = self.run_helper("bt-run")
        self.assertEqual(result.returncode, 69)
        self.assertIn("wmt_drv", result.stderr)
        for module in ("wmt_drv", "btif_drv", "bt_drv_connac1x", "bluetooth", "hci_uart"):
            (self.root / "sys/module" / module).mkdir(parents=True)
        result = self.run_helper("bt-run")
        self.assertEqual(result.returncode, 69)
        self.assertIn("transport is absent", result.stderr)

    def test_bt_explicit_bridge_preserves_exit_status(self) -> None:
        for module in ("wmt_drv", "btif_drv", "bt_drv_connac1x", "bluetooth", "hci_uart"):
            (self.root / "sys/module" / module).mkdir(parents=True)
        # The fake service never opens this node or any host device.
        (self.root / "dev/stpbt").symlink_to("/dev/null")
        self.install_wmt_control()
        self.install_fake("teclast-bt-hci", '#!/bin/sh\n[ "$#" = 1 ] && [ "$1" = --run ] || exit 99\nexit 37\n')
        result = self.run_helper("bt-run")
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertIn("power_owner=transport-open-close", result.stdout)
        self.assertEqual(self.run_helper("bt-run", "unexpected").returncode, 64)

    def test_bt_bridge_is_not_started_without_live_wmt(self) -> None:
        for module in ("wmt_drv", "btif_drv", "bt_drv_connac1x", "bluetooth", "hci_uart"):
            (self.root / "sys/module" / module).mkdir(parents=True)
        (self.root / "dev/stpbt").symlink_to("/dev/null")
        self.install_wmt_control(ready_passes=0)
        marker = self.root / "bridge-started"
        self.install_fake("teclast-bt-hci", f'#!/bin/sh\ntouch "{marker}"\nexit 0\n')
        result = self.run_helper("bt-run")
        self.assertEqual(result.returncode, 69, result.stderr)
        self.assertIn("run wmt-run first", result.stderr)
        self.assertFalse(marker.exists())

    def test_probe_separates_driver_service_and_connection(self) -> None:
        self.make_wifi()
        self.make_socket("run/wpa_supplicant/wlan0")
        self.install_fake("wpa_supplicant", "#!/bin/sh\nexit 0\n")
        result = self.run_helper("probe")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("wifi.module=loaded", result.stdout)
        self.assertIn("wifi.wiphy=present", result.stdout)
        self.assertIn("wifi.netdev=present", result.stdout)
        self.assertIn("wifi.wpa_supplicant=available", result.stdout)
        self.assertIn("wifi.control_socket=present", result.stdout)
        self.assertIn("wifi.service_handoff=eligible-not-tested", result.stdout)
        self.assertIn("wifi.connection=not-queried", result.stdout)
        self.assertIn("wifi.ip_configuration=not-managed", result.stdout)
        self.assertIn("bt.service_handoff=blocked", result.stdout)
        self.assertIn("gnss.protocol=unverified", result.stdout)

    def test_wifi_run_uses_nl80211_foreground_and_preserves_exit_code(self) -> None:
        self.make_wifi()
        config = self.root / "wifi.conf"
        config.write_text("synthetic test only\n", encoding="utf-8")
        capture = self.root / "argv.txt"
        self.env["TECLAST_TEST_CAPTURE"] = str(capture)
        self.install_fake(
            "wpa_supplicant",
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$TECLAST_TEST_CAPTURE\"\nexit 23\n",
        )
        result = self.run_helper("wifi-run", "wlan0", str(config))
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn("wifi.service=delegating backend=nl80211", result.stdout)
        args = capture.read_text(encoding="utf-8").splitlines()
        self.assertEqual(args[:6], ["-D", "nl80211", "-i", "wlan0", "-c", str(config)])
        self.assertIn("-O", args)
        self.assertNotIn("-C", args)
        self.assertNotIn("-B", args)

    def test_wifi_run_fails_closed_when_driver_or_config_is_missing(self) -> None:
        config = self.root / "wifi.conf"
        config.write_text("synthetic test only\n", encoding="utf-8")
        capture = self.root / "not-launched"
        self.env["TECLAST_TEST_CAPTURE"] = str(capture)
        self.install_fake(
            "wpa_supplicant",
            "#!/bin/sh\nprintf launched > \"$TECLAST_TEST_CAPTURE\"\nexit 0\n",
        )
        result = self.run_helper("wifi-run", "wlan0", str(config))
        self.assertEqual(result.returncode, 69)
        self.assertIn("stock Wi-Fi module is not loaded", result.stderr)
        self.assertFalse(capture.exists())

    def test_wifi_status_reports_only_supplicant_state(self) -> None:
        self.make_wifi()
        control = self.root / "run/wpa_supplicant/wlan0"
        self.make_socket(str(control.relative_to(self.root)))
        self.install_fake(
            "wpa_cli",
            "#!/bin/sh\nprintf '%s\\n' 'bssid=02:00:00:00:00:01' 'ssid=synthetic-private-name' 'wpa_state=COMPLETED' 'ip_address=192.0.2.4'\n",
        )
        result = self.run_helper("wifi-status", "wlan0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("wifi.connection=COMPLETED", result.stdout)
        self.assertIn("wifi.ip_configuration=not-managed", result.stdout)
        self.assertNotIn("synthetic-private-name", result.stdout)
        self.assertNotIn("02:00:00:00:00:01", result.stdout)
        self.assertNotIn("192.0.2.4", result.stdout)

    def test_invalid_interface_names_do_not_execute_services(self) -> None:
        for iface in ("", ".", "..", "bad/name", "wlan0;false", "abcdefghijklmnop"):
            with self.subTest(iface=iface):
                result = self.run_helper("wifi-status", iface)
                self.assertEqual(result.returncode, 64)
                self.assertIn("invalid interface name", result.stderr)

    def test_wifi_status_preserves_authenticating_and_disabled(self) -> None:
        self.make_socket("run/wpa_supplicant/wlan0")
        for state in ("AUTHENTICATING", "INTERFACE_DISABLED"):
            with self.subTest(state=state):
                self.install_fake("wpa_cli", f"#!/bin/sh\nprintf 'wpa_state={state}\\n'\n")
                result = self.run_helper("wifi-status", "wlan0")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), [
                    "wifi.connection=" + state, "wifi.ip_configuration=not-managed",
                ])

    def test_wifi_status_propagates_control_client_failure(self) -> None:
        self.make_socket("run/wpa_supplicant/wlan0")
        self.install_fake(
            "wpa_cli",
            "#!/bin/sh\necho synthetic-control-error >&2\nexit 17\n",
        )
        result = self.run_helper("wifi-status", "wlan0")
        self.assertEqual(result.returncode, 17)
        self.assertIn("synthetic-control-error", result.stderr)
        self.assertNotIn("ssid=", result.stdout)
        self.install_fake("wpa_cli", "#!/bin/sh\nprintf 'wpa_state=UNKNOWN_FUTURE_STATE\\n'\n")
        unknown = self.run_helper("wifi-status", "wlan0")
        self.assertEqual(unknown.returncode, 3)
        self.assertIn("wifi.ip_configuration=unknown", unknown.stdout)

    def test_bluetooth_and_gnss_require_runtime_interfaces(self) -> None:
        (self.root / "sys/module/bluetooth").mkdir(parents=True)
        (self.root / "sys/class/bluetooth/hci0").mkdir(parents=True)
        (self.root / "sys/module/gnss").mkdir(parents=True)
        (self.root / "sys/class/gnss/gnss0").mkdir(parents=True)
        self.make_socket("run/dbus/system_bus_socket")
        self.install_fake("bluetoothd", "#!/bin/sh\nexit 0\n")
        result = self.run_helper("probe")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("bt.hci=present", result.stdout)
        self.assertIn("bt.dbus_socket=present", result.stdout)
        self.assertIn("bt.service_handoff=eligible-not-tested", result.stdout)
        self.assertIn("bt.power=not-set", result.stdout)
        self.assertIn("gnss.class_device=registered", result.stdout)
        self.assertIn("gnss.char_device=missing", result.stdout)
        self.assertIn("gnss.protocol=unverified", result.stdout)
        self.assertIn("gnss.data=not-queried", result.stdout)


    def make_dhcp(self, events=None, *, client_exit=0, hold=False):
        self.make_wifi()
        self.make_socket("run/wpa_supplicant/wlan0")
        (self.root / "etc/resolv.conf").symlink_to(self.root / "run/teclast-connectivity/resolv.conf")
        self.install_fake("wpa_cli", "#!/bin/sh\nprintf 'wpa_state=COMPLETED\\n'\n")
        self.network = self.root / "network.json"
        self.foreign = {"addresses": [["wlan0", "192.0.2.99/24"]], "routes": [["foreign-route"]]}
        self.network.write_text(json.dumps(self.foreign))
        self.env.update(
            TECLAST_TEST_NETWORK=str(self.network),
            TECLAST_TEST_EVENTS=json.dumps(events if events is not None else [self.lease()]),
            TECLAST_TEST_CLIENT_EXIT=str(client_exit),
            TECLAST_TEST_HOLD="1" if hold else "0",
        )
        self.install_fake("ip", "#!" + sys.executable + "\n" + r'''
import json, os, sys
from pathlib import Path
p = Path(os.environ["TECLAST_TEST_NETWORK"])
a = sys.argv[1:]
assert a[:2] == ["-f", "inet"], a
kind, op = a[2:4]
assert kind in ("addr", "route") and op in ("add", "del"), a
with (p.parent / "ip-calls.jsonl").open("a") as log: log.write(json.dumps(a) + "\n")
if os.environ.get("TECLAST_TEST_IP_FAIL") == kind + "-" + op:
    print("RTNETLINK answers: Operation not permitted", file=sys.stderr); sys.exit(11)
state = json.loads(p.read_text())
key = "addresses" if kind == "addr" else "routes"
item = [a[6], a[4]] if kind == "addr" else a[4:]
if op == "add":
    if item in state[key]:
        print("RTNETLINK answers: File exists", file=sys.stderr); sys.exit(2)
    state[key].append(item)
else:
    if item not in state[key]:
        print("RTNETLINK answers: " + ("Cannot assign requested address" if kind == "addr" else "No such process"), file=sys.stderr); sys.exit(2)
    state[key].remove(item)
pending = p.with_suffix(".pending")
pending.write_text(json.dumps(state)); os.replace(pending, p)
''')
        self.install_fake("udhcpc", "#!" + sys.executable + "\n" + r'''
import json, os, signal, subprocess, sys, time
from pathlib import Path
args = sys.argv[1:]
root = Path(os.environ["TECLAST_CONNECTIVITY_TEST_ROOT"])
(root / "dhcp-argv.json").write_text(json.dumps(args))
helper = args[args.index("-s") + 1]
iface = args[args.index("-i") + 1]
for event in json.loads(os.environ["TECLAST_TEST_EVENTS"]):
    env = os.environ.copy()
    env.update(interface=iface)
    env.update(event.get("options", {}))
    result = subprocess.run([helper, event["event"]], env=env, capture_output=True, text=True)
    network = json.loads(Path(env["TECLAST_TEST_NETWORK"]).read_text())
    resolver = root / "run/teclast-connectivity/resolv.conf"
    record = {"event": event["event"], "code": result.returncode,
              "network": network, "dns": resolver.read_text() if resolver.exists() else "",
              "stderr": result.stderr}
    with (root / "events.jsonl").open("a") as out: out.write(json.dumps(record) + "\n")
(root / "client-ready").touch()
(root / ("client-ready-" + iface)).touch()
if os.environ["TECLAST_TEST_HOLD"] == "1":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    while True: time.sleep(0.02)
sys.exit(int(os.environ["TECLAST_TEST_CLIENT_EXIT"]))
''')

    @staticmethod
    def lease(event="bound", **options):
        return {"event": event, "options": {"ip": "192.0.2.4", "subnet": "255.255.255.0",
                "router": "192.0.2.1", "dns": "192.0.2.53 198.51.100.53", **options}}

    def records(self):
        return [json.loads(line) for line in (self.root / "events.jsonl").read_text().splitlines()]

    def calls(self):
        p = self.root / "ip-calls.jsonl"
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def assert_clean(self):
        self.assertEqual(json.loads(self.network.read_text()), self.foreign)
        self.assertFalse((self.root / "run/teclast-connectivity/lease-wlan0").exists())
        resolver = self.root / "run/teclast-connectivity/resolv.conf"
        self.assertEqual(resolver.read_text(), "")

    def test_dhcp_bound_renew_changed_lease_and_shutdown_preserve_foreign_config(self):
        self.make_dhcp([self.lease(), self.lease("renew", dns="203.0.113.53"),
                        self.lease("renew", ip="198.51.100.4", router="198.51.100.1")])
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 0, result.stderr)
        records = self.records()
        self.assertEqual([row["code"] for row in records], [0, 0, 0])
        self.assertEqual(records[0]["network"], records[1]["network"])
        self.assertEqual(records[1]["dns"], "nameserver 203.0.113.53\n")
        self.assertIn(["wlan0", "198.51.100.4/24"], records[2]["network"]["addresses"])
        self.assertEqual(sum(a[2:4] == ["addr", "add"] for a in self.calls()), 2)
        args = json.loads((self.root / "dhcp-argv.json").read_text())
        self.assertEqual(args[:4], ["-f", "-R", "-i", "wlan0"])
        self.assertNotIn("-q", args); self.assertNotIn("-b", args)
        self.assert_clean()

    def test_dhcp_classless_routes_override_router_and_support_onlink(self):
        self.make_dhcp([self.lease(staticroutes="203.0.113.0/24 0.0.0.0 0.0.0.0/0 192.0.2.2")])
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 0, result.stderr)
        routes = self.records()[0]["network"]["routes"]
        self.assertIn(["203.0.113.0/24", "dev", "wlan0", "proto", "16", "metric", "600"], routes)
        self.assertIn(["0.0.0.0/0", "via", "192.0.2.2", "dev", "wlan0", "proto", "16", "metric", "601"], routes)
        self.assertFalse(any("192.0.2.1" in row for row in routes))
        self.assert_clean()

    def test_dhcp_empty_classless_option_suppresses_default_route(self):
        self.make_dhcp([self.lease(staticroutes="")])
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.records()[0]["network"]["routes"], self.foreign["routes"])
        self.assert_clean()

    def test_dhcp_deconfig_nak_and_leasefail_remove_only_owned_state(self):
        for event in ("deconfig", "nak", "leasefail"):
            with self.subTest(event=event):
                if hasattr(self, "network"):
                    # Each iteration uses the same reusable synthetic fixture.
                    import shutil
                    shutil.rmtree(self.root / "sys/module")
                    shutil.rmtree(self.root / "sys/class")
                    (self.root / "run/wpa_supplicant/wlan0").unlink()
                    (self.root / "etc/resolv.conf").unlink()
                    (self.root / "events.jsonl").unlink()
                self.make_dhcp([self.lease(), {"event": event}])
                result = self.run_helper("wifi-dhcp-run", "wlan0")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.records()[-1]["network"], self.foreign)
                self.assertEqual(self.records()[-1]["dns"], "")
                self.assert_clean()

    def test_invalid_dhcp_options_are_rejected_before_any_ip_call(self):
        invalid = [
            {"ip": "192.0.2.999"}, {"ip": "127.0.0.1"}, {"ip": "192.0.2.0"},
            {"ip": "192.0.2.255"}, {"ip": "192.000.2.4"}, {"ip": "1.2.3.4;false"},
            {"subnet": "255.0.255.0"}, {"dns": "192.0.2.53\nnameserver 203.0.113.1"},
            {"dns": "$(touch never)"}, {"router": "--help"},
            {"router": "192.0.2.4"}, {"staticroutes": "203.0.113.1/24 192.0.2.1"},
            {"staticroutes": "0.0.0.0/33 192.0.2.1"}, {"staticroutes": "0.0.0.0/0"},
            {"staticroutes": "0.0.0.0/0 224.0.0.1"}, {"dns": " ".join(["192.0.2.53"] * 9)},
        ]
        self.make_dhcp([self.lease(**row) for row in invalid])
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 74, result.stderr)
        self.assertEqual(len(self.records()), len(invalid))
        self.assertTrue(all(row["code"] == 74 for row in self.records()), self.records())
        self.assertEqual(self.calls(), [])
        self.assert_clean()

    def test_invalid_renew_stops_client_and_removes_owned_lease(self):
        # A BusyBox-like client ignores the hook result and would remain active
        # with the rejected lease; the service must not wait for its expiry.
        self.make_dhcp([self.lease(), self.lease("renew", dns="bad")], hold=True)
        proc = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan0"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            _, err = proc.communicate(timeout=3)
            self.assertEqual(proc.returncode, 74, err)
            self.assertEqual(sum(a[2:4] == ["addr", "del"] for a in self.calls()), 1)
            self.assert_clean()
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.communicate(timeout=10)

    def test_invalid_renew_reports_failed_cleanup_and_stops_client(self):
        self.make_dhcp([self.lease(), self.lease("renew", dns="bad")], hold=True)
        self.env["TECLAST_TEST_IP_FAIL"] = "route-del"
        proc = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan0"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            _, err = proc.communicate(timeout=3)
            self.assertEqual(proc.returncode, 74, err)
            lease = self.root / "run/teclast-connectivity/lease-wlan0"
            self.assertEqual((lease / "state").read_text(), "cleanup-failed\n")
            self.assertTrue((lease / "address").exists())
            self.assertTrue((lease / "route.0").exists())
            self.assertFalse(any(a[2:4] == ["addr", "del"] for a in self.calls()))
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.communicate(timeout=10)

    def test_route_add_failure_unwinds_successful_address_only(self):
        self.make_dhcp()
        self.env["TECLAST_TEST_IP_FAIL"] = "route-add"
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 74, result.stderr)
        self.assertEqual(self.records()[0]["code"], 74)
        self.assertEqual(self.records()[0]["network"], self.foreign)
        self.assertFalse(any(a[2:4] == ["route", "del"] for a in self.calls()))
        self.assert_clean()

    def test_foreign_address_collision_is_not_deleted(self):
        self.make_dhcp([self.lease(ip="192.0.2.99")])
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 74, result.stderr)
        self.assertFalse(any(a[2:4] == ["addr", "del"] for a in self.calls()))
        self.assert_clean()

    def test_cleanup_failure_retains_owned_address_routes_and_excludes_restart(self):
        self.make_dhcp()
        self.env["TECLAST_TEST_IP_FAIL"] = "route-del"
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 74, result.stderr)
        lease = self.root / "run/teclast-connectivity/lease-wlan0"
        self.assertEqual((lease / "state").read_text(), "cleanup-failed\n")
        self.assertTrue((lease / "address").exists())
        self.assertTrue((lease / "route.0").exists())
        self.assertFalse(any(a[2:4] == ["addr", "del"] for a in self.calls()))
        second = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(second.returncode, 73, second.stderr)
        self.assertTrue((lease / "address").exists())

    def test_dhcp_requires_association_resolver_delegation_and_exclusive_directory(self):
        self.make_dhcp()
        self.install_fake("wpa_cli", "#!/bin/sh\nprintf 'wpa_state=SCANNING\\n'\n")
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 69)
        self.install_fake("wpa_cli", "#!/bin/sh\nprintf 'wpa_state=COMPLETED\\n'\n")
        (self.root / "etc/resolv.conf").unlink()
        (self.root / "etc/resolv.conf").write_text("foreign resolver\n")
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 73)
        self.assertFalse((self.root / "dhcp-argv.json").exists())
        self.assertEqual((self.root / "etc/resolv.conf").read_text(), "foreign resolver\n")
        self.assertEqual(self.calls(), [])

    def test_unsolicited_dhcp_event_never_invokes_ip(self):
        self.make_dhcp()
        self.env.update(interface="wlan0", TECLAST_DHCP_IFACE="wlan0")
        result = self.run_helper("bound")
        self.assertEqual(result.returncode, 73)
        self.assertEqual(self.calls(), [])

    def test_term_stops_owned_child_cleans_lease_and_preserves_exit_status(self):
        self.make_dhcp(hold=True)
        proc = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan0"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while not (self.root / "client-ready").exists() and proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue((self.root / "client-ready").exists())
            status = self.run_helper("wifi-status", "wlan0")
            self.assertIn("wifi.ip_configuration=lease-published", status.stdout)
            self.assertIn("wifi.ip_configuration=service-state-present", self.run_helper("probe").stdout)
            self.assertNotIn("192.0.2.", status.stdout)
            proc.send_signal(signal.SIGTERM)
            stdout, stderr = proc.communicate(timeout=10)
            self.assertEqual(proc.returncode, 143, stderr)
            self.assert_clean()
        finally:
            if proc.poll() is None:
                proc.kill(); proc.communicate()

    def test_dhcp_client_failure_exit_code_is_preserved_after_cleanup(self):
        self.make_dhcp(client_exit=23)
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assert_clean()


    def test_multiple_interfaces_publish_dns_and_release_independently(self):
        self.make_dhcp(hold=True)
        first = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan0"], env=self.env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        second = None
        try:
            deadline = time.monotonic() + 10
            while not (self.root / "client-ready-wlan0").exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue((self.root / "client-ready-wlan0").exists())
            (self.root / "sys/class/net/wlan1/wireless").mkdir(parents=True)
            self.make_socket("run/wpa_supplicant/wlan1")
            env = dict(self.env, TECLAST_TEST_EVENTS=json.dumps([self.lease(ip="198.51.100.4", router="198.51.100.1", dns="203.0.113.53")]))
            second = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan1"], env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 10
            while not (self.root / "client-ready-wlan1").exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue((self.root / "client-ready-wlan1").exists())
            resolver = self.root / "run/teclast-connectivity/resolv.conf"
            self.assertIn("nameserver 192.0.2.53\n", resolver.read_text())
            self.assertIn("nameserver 203.0.113.53\n", resolver.read_text())
            first.terminate(); _, err = first.communicate(timeout=10)
            self.assertEqual(first.returncode, 143, err)
            self.assertEqual(resolver.read_text(), "nameserver 203.0.113.53\n")
            self.assertEqual(resolver.stat().st_mode & 0o777, 0o644)
            self.assertIn(["wlan1", "198.51.100.4/24"], json.loads(self.network.read_text())["addresses"])
            second.terminate(); _, err = second.communicate(timeout=10)
            self.assertEqual(second.returncode, 143, err)
            self.assert_clean()
        finally:
            for proc in (first, second):
                if proc is not None and proc.poll() is None:
                    proc.kill(); proc.communicate()

    def test_running_client_rejects_duplicate_and_keeps_original_lease(self):
        self.make_dhcp(hold=True)
        proc = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan0"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while not (self.root / "client-ready").exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue((self.root / "client-ready").exists())
            before = self.network.read_text()
            duplicate = self.run_helper("wifi-dhcp-run", "wlan0")
            self.assertEqual(duplicate.returncode, 73, duplicate.stderr)
            self.assertIsNone(proc.poll())
            self.assertEqual(self.network.read_text(), before)
            proc.terminate(); proc.communicate(timeout=10)
            self.assert_clean()
        finally:
            if proc.poll() is None:
                proc.kill(); proc.communicate()

    def test_address_cleanup_failure_is_retained_and_not_reported_stopped(self):
        self.make_dhcp()
        self.env["TECLAST_TEST_IP_FAIL"] = "addr-del"
        result = self.run_helper("wifi-dhcp-run", "wlan0")
        self.assertEqual(result.returncode, 74, result.stderr)
        lease = self.root / "run/teclast-connectivity/lease-wlan0"
        self.assertEqual((lease / "state").read_text(), "cleanup-failed\n")
        self.assertTrue((lease / "address").exists())
        self.assertFalse((lease / "route.0").exists())
        self.assertIn(["wlan0", "192.0.2.4/24"], json.loads(self.network.read_text())["addresses"])


    def test_dns_fragment_cannot_disappear_during_another_interface_publication(self):
        self.make_dhcp(hold=True)
        first = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan0"], env=self.env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        second = hook = None
        try:
            def ready(name):
                deadline = time.monotonic() + 10
                while not (self.root / name).exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue((self.root / name).exists(), name)
            ready("client-ready-wlan0")
            (self.root / "sys/class/net/wlan1/wireless").mkdir(parents=True)
            self.make_socket("run/wpa_supplicant/wlan1")
            env = dict(self.env, TECLAST_TEST_EVENTS=json.dumps([self.lease(ip="198.51.100.4", router="198.51.100.1", dns="203.0.113.53")]))
            second = subprocess.Popen([str(HELPER), "wifi-dhcp-run", "wlan1"], env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            ready("client-ready-wlan1")
            self.install_fake("cat", "#!" + sys.executable + "\n" + r'''
import os, sys, time
from pathlib import Path
root = Path(os.environ["TECLAST_CONNECTIVITY_TEST_ROOT"])
for name in sys.argv[1:]:
    if name.endswith("/lease-wlan1/resolv.conf"):
        (root / "reading-dns").touch()
        deadline = time.monotonic() + 10
        while not (root / "release-dns").exists() and time.monotonic() < deadline: time.sleep(0.01)
    try: sys.stdout.buffer.write(Path(name).read_bytes())
    except OSError: sys.exit(1)
''')
            lease = self.root / "run/teclast-connectivity/lease-wlan0"
            event_env = dict(self.env, interface="wlan0", TECLAST_DHCP_IFACE="wlan0",
                             TECLAST_DHCP_DIR=str(lease), TECLAST_DHCP_OWNER=(lease / "owner").read_text().strip(),
                             **self.lease("renew", dns="192.0.2.54")["options"])
            hook = subprocess.Popen([str(HELPER), "renew"], env=event_env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            ready("reading-dns")
            second.terminate()
            deadline = time.monotonic() + 0.7
            while ["wlan1", "198.51.100.4/24"] in json.loads(self.network.read_text())["addresses"] and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertNotIn(["wlan1", "198.51.100.4/24"], json.loads(self.network.read_text())["addresses"])
            self.assertTrue((self.root / "run/teclast-connectivity/lease-wlan1/resolv.conf").exists())
            (self.root / "release-dns").touch()
            _, err = hook.communicate(timeout=10)
            self.assertEqual(hook.returncode, 0, err)
            _, err = second.communicate(timeout=10)
            self.assertEqual(second.returncode, 143, err)
            self.assertEqual((self.root / "run/teclast-connectivity/resolv.conf").read_text(), "nameserver 192.0.2.54\n")
            self.assertIn(["wlan0", "192.0.2.4/24"], json.loads(self.network.read_text())["addresses"])
            first.terminate(); first.communicate(timeout=10)
            self.assert_clean()
        finally:
            (self.root / "release-dns").touch()
            for proc in (hook, second, first):
                if proc is not None and proc.poll() is None:
                    proc.kill(); proc.communicate()


if __name__ == "__main__":
    program = unittest.main(exit=False)
    result = program.result
    report = {
        "status": "passed" if result.wasSuccessful() else "failed",
        "tests": result.testsRun,
        "command": "python3 -B Platforms/Teclast/t70proPkg/Linux/Tests/test_connectivity_services.py",
        "exit_code": 0 if result.wasSuccessful() else 1,
        "scope": "Actual connectivity shell; synthetic sysfs, UNIX sockets, fake wpa/udhcpc/ip executables and real temporary address/route/DNS state writes; no host network or tablet access",
        "source_sha256": {str(path.relative_to(ROOT.parents[3])): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in (HELPER, Path(__file__))},
        "integration": "Packaged helper and BusyBox applets; explicit activation only",
        "not_bundled": ["gpsd"],
        "device_execution": False,
    }
    (ROOT.parents[4] / "analysis/linux-connectivity-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    sys.exit(0 if result.wasSuccessful() else 1)
