#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
import runpy
import struct
from pathlib import Path
import unittest

F = runpy.run_path(str(Path(__file__).with_name("audit-storage-initramfs.py")))
RELEASE = "7.0.0-43-generic"


class TestAudit(unittest.TestCase):
    def setup_ufs(self):
        config = ["CONFIG_" + k + "=m" for k in F["CONFIG"]["ufs"] | F["COMMON_CONFIG"]]
        modules = {"host": "ufs_mediatek", "reset": "reset_ti_syscon",
                   "top clocks": "fixture_clk_top", "infra clocks": "fixture_clk_infra",
                   "PMIC transport": "fixture_pmic"}
        aliases = [f"alias of:N*T*C{c} {modules[r]}" for r, c in F["COMPAT"]["ufs"].items()]
        modules = {m: f"kernel/{m}.ko.zst" for m in (*modules.values(), "mt6397", "mt6358_regulator")}
        deps = [f"{v}:" for v in modules.values()]
        return config, modules, deps, aliases

    def run_audit(self, config, archive, deps, aliases, built=(), builtinfo=b"", release=RELEASE):
        return F["inspect"]("ufs", release, "\n".join(config),
                            "\n".join(f"usr/lib/modules/{RELEASE}/{f}" for f in archive),
                            "\n".join(deps), "\n".join(built), "\n".join(aliases), builtinfo)["issues"]

    def test_complete_mock(self):
        c,m,d,a=self.setup_ufs()
        self.assertEqual(self.run_audit(c,m.values(),d,a), [])

    def test_missing_separate_reset(self):
        c,m,d,a=self.setup_ufs()
        del m["reset_ti_syscon"]
        self.assertIn("reset: reset_ti_syscon: absent", self.run_audit(c,m.values(),d,a))

    def test_wrong_alias_does_not_count_as_transport(self):
        c,m,d,a=self.setup_ufs()
        a=[x.replace("mt6789-pwrap","mt6779-pwrap") for x in a]
        self.assertIn("PMIC transport: expected one matching OF driver, got 0",
                      self.run_audit(c,m.values(),d,a))

    def test_old_release_is_not_counted(self):
        c,m,d,a=self.setup_ufs()
        self.assertIn("host: ufs_mediatek: absent", self.run_audit(c,m.values(),d,a,release="6.12.38"))

    def test_module_dependency_path_checked(self):
        c,m,d,a=self.setup_ufs()
        d.remove(m["ufs_mediatek"]+":")
        d.append(m["ufs_mediatek"]+": custom/mt6397.ko.zst")
        self.assertIn("host: mt6397: dependencies refer to another archive path",
                      self.run_audit(c,m.values(),d,a))

    def test_dependency_cycle_rejected(self):
        c, m, d, a = self.setup_ufs()
        host, mfd = m["ufs_mediatek"], m["mt6397"]
        d.remove(host + ":")
        d.remove(mfd + ":")
        d.extend([f"{host}: {mfd}", f"{mfd}: {host}"])
        issues = self.run_audit(c, m.values(), d, a)
        self.assertIn("host: dependency cycle at ufs_mediatek", issues)

    def test_self_dependency_rejected(self):
        c, m, d, a = self.setup_ufs()
        host = m["ufs_mediatek"]
        d.remove(host + ":")
        d.append(f"{host}: {host}")
        self.assertIn("host: dependency cycle at ufs_mediatek",
                      self.run_audit(c, m.values(), d, a))

    def test_shared_dependency_is_not_cycle(self):
        c, m, d, a = self.setup_ufs()
        host, mfd, regulator = (m[name] for name in
                                ("ufs_mediatek", "mt6397", "mt6358_regulator"))
        common = m["fixture_clk_top"]
        for node in (host, mfd, regulator):
            d.remove(node + ":")
        d.extend([f"{host}: {mfd} {regulator} {common}",
                  f"{mfd}: {common}", f"{regulator}: {common}"])
        self.assertEqual(self.run_audit(c, m.values(), d, a), [])

    def test_builtin_metadata_needed(self):
        c,m,d,a=self.setup_ufs()
        c=[x.replace("CONFIG_RESET_TI_SYSCON=m","CONFIG_RESET_TI_SYSCON=y") for x in c]
        f=m.pop("reset_ti_syscon")
        a=[x for x in a if not x.endswith(" reset_ti_syscon")]
        self.assertIn("RESET_TI_SYSCON: built-in not corroborated by modules.builtin",
                      self.run_audit(c,m.values(),d,a))
        self.assertEqual(self.run_audit(c,m.values(),d,a,built=[f],
                             builtinfo=b"reset_ti_syscon.alias=of:N*T*Cti,syscon-reset\0"), [])

    def test_multiple_paths_refused(self):
        c,m,d,a=self.setup_ufs()
        self.assertIn("host: ufs_mediatek: ambiguous_archive_paths",
                      self.run_audit(c,[*m.values(),"other/ufs-mediatek.ko"],d,a))

    def test_missing_dependency_row_refused(self):
        c,m,d,a=self.setup_ufs()
        d.remove(m["ufs_mediatek"]+":")
        self.assertTrue(any("no exact modules.dep" in i for i in self.run_audit(c,m.values(),d,a)))


    def test_sd_archive_cannot_pass_without_effective_dt(self):
        modes = F["CONFIG"]["sd"] | F["COMMON_CONFIG"]
        config = "\n".join(f"CONFIG_{key}=m" for key in modes)
        providers = {role: "fixture_" + str(i) for i, role in enumerate(F["COMPAT"]["sd"])}
        aliases = "\n".join(f"alias of:N*T*C{compat} {providers[role]}"
                            for role, compat in F["COMPAT"]["sd"].items())
        modules = [f"kernel/{name}.ko" for name in [*modes.values(), *providers.values()]]
        archive = "\n".join(f"usr/lib/modules/{RELEASE}/{p}" for p in modules)
        deps = "\n".join(f"{p}:" for p in modules)
        args = ("sd", RELEASE, config, archive, deps, "", aliases, b"")
        self.assertEqual(F["inspect"](*args)["issues"],
                         ["SD effective DT not supplied: pass --effective-dtb"])
        self.assertEqual(F["inspect"](*args, (F["COMPAT"]["sd"]["pinctrl / GPIO7"], []))["issues"], [])

    def test_effective_sd_requires_actual_paris_resources(self):
        u32 = lambda n: struct.pack(">I", n)
        node = F["STOCK"]["SD"]
        stock = {"/": {"interrupt-parent": u32(4)},
                 "/soc/pinctrl": {"compatible": b"mediatek,mt6789-pinctrl\0",
                                 "phandle": u32(5), "mediatek,eint": u32(6),
                                 "reg": b"gpio banks", "reg-names": b"gpio\0"},
                 "/soc/eint": {"phandle": u32(6), "interrupts": b"spi244", "reg": b"eint bank"},
                 node: {"cd-gpios": struct.pack(">III", 5, 7, 0), "no-mmc": b""}}
        for key in ("reg", "interrupts", "clocks", "clock-names", "vmmc-supply", "vqmmc-supply",
                    "bus-width", "pinctrl-names", "pinctrl-0", "pinctrl-1", "pinctrl-2"):
            stock[node][key] = u32(1)
        dt = {path: dict(props) for path, props in stock.items()}
        dt[node].update(compatible=b"teclast,t70pro-mt6789-ext-sd\0", status=b"disabled\0")
        dt["/soc/pinctrl"].update(compatible=b"teclast,t70pro-mt6789-pinctrl\0",
                                  reg=b"gpio bankseint bank", **{"reg-names": b"gpio\0eint\0",
                                  "interrupts": b"spi244", "interrupt-parent": u32(4)})
        dt["/soc/eint"]["status"] = b"disabled\0"
        effective = F["effective_sd_contract"](stock, dt)
        self.assertEqual(effective, ("teclast,t70pro-mt6789-pinctrl", ["SD host: disabled in effective DT"]))
        report = F["inspect"]("sd", RELEASE, "", "", "", "", "", b"", effective)
        self.assertEqual(report["suppliers"]["pinctrl / GPIO7"]["compatible"], effective[0])
        self.assertIn("SD host: disabled in effective DT", report["issues"])
        dt[node]["status"] = b"okay\0"
        dt["/soc"] = {"status": b"disabled\0"}
        self.assertEqual(F["effective_sd_contract"](stock, dt)[1],
                         ["SD host: ancestor /soc disabled in effective DT",
                          "GPIO7 provider: ancestor /soc disabled in effective DT"])
        del dt["/soc"]
        dt["/soc/eint"]["status"] = b"okay\0"
        with self.assertRaisesRegex(ValueError, "stock EINT"):
            F["effective_sd_contract"](stock, dt)
        dt["/soc/eint"]["status"] = b"disabled\0"
        dt[node]["cd-gpios"] = struct.pack(">III", 5, 9, 0)
        with self.assertRaisesRegex(ValueError, "cd-gpios"):
            F["effective_sd_contract"](stock, dt)


if __name__ == "__main__":
    unittest.main()
