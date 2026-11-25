#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the built T70Pro ACPI resources against a supplied stock device tree."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import sys
import runpy

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--python-dtb", action="store_true", help="use the existing bounded supplier scanner instead of fdtget")
    parser.add_argument("--acpica-bin", type=Path, help="use ACPICA tools from this directory")
    args = parser.parse_args()
    acpiexec = str((args.acpica_bin / "acpiexec").resolve()) if args.acpica_bin else "acpiexec"
    iasl = str((args.acpica_bin / "iasl").resolve()) if args.acpica_bin else "iasl"
    if args.acpica_bin:
        for tool in (acpiexec, iasl):
            if not Path(tool).is_file():
                parser.error("ACPICA executable is missing: " + tool)
    dt_reader = runpy.run_path(str(Path(__file__).resolve().parent/"Linux/platform-modules.py")) if args.python_dtb else None
    dt_nodes = dt_reader["tree"](args.dtb.read_bytes())[0] if dt_reader else None

    def cells(node, prop):
        if dt_reader:
            return dt_reader["cells"](dt_nodes[node]["properties"][prop])
        result = subprocess.check_output(
            ["fdtget", "-t", "x", str(args.dtb), node, prop], text=True)
        return [int(x, 16) for x in result.split()]

    def raw_property(node, prop):
        if dt_reader:
            return dt_nodes[node]["properties"][prop]
        result = subprocess.check_output(
            ["fdtget", "-t", "bx", str(args.dtb), node, prop], text=True)
        return bytes(int(value, 16) for value in result.split())

    def children(node):
        if dt_reader:
            return [p.rsplit("/",1)[-1] for p,v in dt_nodes.items() if v["parent"] == node]
        return subprocess.check_output(
            ["fdtget", "-l", str(args.dtb), node], text=True).splitlines()

    def irq(node, index=0):
        values = cells(node, "interrupts")
        kind, number, flags, unused = values[index*4:index*4+4]
        assert kind in (0, 1) and flags == 4 and unused == 0
        return number + (32 if kind == 0 else 16)

    tables = {}
    paths = {}
    ssdts = {}
    required = {"APIC", "GTDT", "FACP", "PPTT", "DSDT"}
    for p in sorted(args.build_dir.rglob("*")):
        if p.suffix not in (".acpi", ".aml"):
            continue
        data = p.read_bytes()
        assert len(data) >= 36, p
        signature = data[:4].decode("ascii")
        table_id = data[16:24].decode("ascii").rstrip("\x00 ") if signature == "SSDT" else signature
        assert table_id not in tables, ("Duplicate table", table_id)
        if signature == "SSDT":
            assert table_id and table_id not in required, (table_id, p)
            ssdts[table_id] = str(p)
        else:
            assert signature in required, (signature, p)
        assert struct.unpack_from("<I", data, 4)[0] == len(data), p
        assert sum(data) % 256 == 0, ("Invalid checksum", p)
        tables[table_id], paths[table_id] = data, p
    assert required <= tables.keys(), tables.keys()

    cpu_nodes = [n for n in children("/cpus") if n.startswith("cpu@")]
    affinities = [cells("/cpus/"+n, "reg")[0] for n in cpu_nodes]
    assert len(affinities) == 8 and len(set(affinities)) == 8
    cpu_phandles = [cells("/cpus/"+n, "phandle")[0] for n in cpu_nodes]
    cpu_uid = {phandle: uid for uid, phandle in enumerate(cpu_phandles)}

    madt = tables["APIC"]
    assert struct.unpack_from("<II", madt, 36) == (0, 0)
    entries = []
    pos = 44
    while pos < len(madt):
        kind, size = struct.unpack_from("<BB", madt, pos)
        assert size >= 2 and pos+size <= len(madt)
        entries.append((kind, madt[pos:pos+size]))
        pos += size
    gicc = [data for kind, data in entries if kind == 11]
    assert len(gicc) == len(affinities)
    efficiency_classes = {b"arm,cortex-a55": 0, b"arm,cortex-a76": 1}
    for uid, (data, affinity) in enumerate(zip(gicc, affinities)):
        assert struct.unpack_from("<III", data, 4) == (0, uid, 1)
        assert struct.unpack_from("<I", data, 20)[0] == irq("/soc/pmu")
        assert struct.unpack_from("<4Q", data, 24) == (0, 0, 0, 0)
        assert struct.unpack_from("<I", data, 56)[0] == irq("/interrupt-controller")
        assert struct.unpack_from("<Q", data, 60)[0] == 0
        assert struct.unpack_from("<Q", data, 68)[0] == affinity
        core = raw_property("/cpus/" + cpu_nodes[uid], "compatible").split(b"\x00", 1)[0]
        assert data[76] == efficiency_classes[core], ("GICC efficiency class", uid, core, data[76])
    gic_reg = cells("/interrupt-controller", "reg")
    gicd = [data for kind, data in entries if kind == 12]
    gicr = [data for kind, data in entries if kind == 14]
    assert len(gicd) == len(gicr) == 1
    assert struct.unpack_from("<Q", gicd[0], 8)[0] == (gic_reg[0]<<32 | gic_reg[1])
    assert gicd[0][20] == 3
    assert struct.unpack_from("<QI", gicr[0], 4) == (
        gic_reg[4]<<32 | gic_reg[5], gic_reg[6]<<32 | gic_reg[7])

    gtdt = tables["GTDT"]
    assert struct.unpack_from("<Q", gtdt, 36)[0] == 0xffffffffffffffff
    for i in range(4):
        assert struct.unpack_from("<II", gtdt, 48+i*8) == (irq("/timer", i), 0)
    assert struct.unpack_from("<Q", gtdt, 80)[0] == 0xffffffffffffffff
    assert struct.unpack_from("<II", gtdt, 88) == (0, 0)

    fadt = tables["FACP"]
    method = (dt_reader["strings"](dt_nodes["/psci"]["properties"]["method"])[0] if dt_reader else
        subprocess.check_output(["fdtget", "-t", "s", str(args.dtb), "/psci", "method"], text=True).strip())
    assert method == "smc"
    assert struct.unpack_from("<I", fadt, 112)[0] == 1<<20
    assert struct.unpack_from("<H", fadt, 129)[0] == 1
    assert fadt[45] == 8 and fadt[131] == 6
    assert fadt[116:129] == bytes(13), "No unverified reset register"

    pptt = tables["PPTT"]
    nodes = {}
    pos = 36
    while pos < len(pptt):
        kind, size = struct.unpack_from("<BB", pptt, pos)
        assert kind == 0 and size == 20 and pos+size <= len(pptt)
        nodes[pos] = struct.unpack_from("<4I", pptt, pos+4)
        pos += size
    assert len(nodes) == 11
    packages = [p for p, v in nodes.items() if v[0] & 1]
    assert len(packages) == 1 and nodes[packages[0]][1] == 0
    clusters = [p for p, v in nodes.items() if v[1] == packages[0]]
    assert len(clusters) == 2
    performance_controller = "/soc/performance-controller@0011bc00"
    compatible = raw_property(performance_controller, "compatible").split(b"\x00")
    assert b"mediatek,cpufreq-hw" in compatible
    assert cells(performance_controller, "#performance-domain-cells") == [1]
    performance_phandle, = cells(performance_controller, "phandle")
    performance_registers = cells(performance_controller, "reg")
    assert performance_registers == [0, 0x0011bc10, 0, 0x120, 0, 0x0011bd30, 0, 0x120]
    performance_names = raw_property(performance_controller, "reg-names").split(b"\x00")[:-1]
    assert performance_names == [b"performance-domain0", b"performance-domain1"]
    performance_domains = [cells("/cpus/"+n, "performance-domains") for n in cpu_nodes]
    topology = []
    for i, cluster in enumerate(clusters):
        parent, actual = cluster, []
        for pos, (flags, owner, uid, resources) in nodes.items():
            assert resources == 0
            if owner == parent:
                assert flags & 10 == 10
                actual.append(uid)
        expected = [
            cpu_uid[cells("/cpus/cpu-map/cluster%d/%s" % (i, n), "cpu")[0]]
            for n in children("/cpus/cpu-map/cluster%d" % i)]
        assert actual == expected
        assert all(performance_domains[uid] == [performance_phandle, i] for uid in expected)
        topology.append(actual)

    commands = ";".join("evaluate \\_SB.CPU%d._UID" % i for i in range(8))
    result = subprocess.run([acpiexec, "-b", commands, str(paths["DSDT"]), *ssdts.values()],
                            capture_output=True, text=True, check=True)
    output = result.stdout + result.stderr
    integers = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", output)]
    assert integers == list(range(8)), output
    assert "AE_ERROR" not in output and "AE_NOT_FOUND" not in output, output
    idle_root = "/cpus/idle-states"
    assert raw_property(idle_root, "entry-method") == b"arm,psci\0"
    idle_nodes = {
        cells(idle_root + "/" + name, "phandle")[0]: idle_root + "/" + name
        for name in children(idle_root)
    }
    commands = ";".join(r"evaluate \_SB.CPU%d._LPI" % uid for uid in range(len(cpu_nodes)))
    output = subprocess.check_output(
        [acpiexec, "-b", commands, str(paths["DSDT"])], text=True, stderr=subprocess.STDOUT
    )
    assert not re.search(r"\bAE_[A-Z_]+\b", output), output
    packages = re.split(r"^Evaluating \\_SB\.CPU[0-7]\._LPI$", output, flags=re.M)[1:]
    assert len(packages) == len(cpu_nodes), output
    no_counter = bytes.fromhex("82 0C 00") + bytes(12) + b"\x79\0"
    cpu_lpi = []
    for uid, (cpu, package) in enumerate(zip(cpu_nodes, packages)):
        core_idle = idle_nodes[cells("/cpus/" + cpu, "cpu-idle-states")[0]]
        assert raw_property(core_idle, "local-timer-stop") == b""
        psci_state, = cells(core_idle, "arm,psci-suspend-param")
        assert (psci_state & ~0xFFFF) == 0x10000, (cpu, core_idle, psci_state)
        residency, = cells(core_idle, "min-residency-us")
        entry_latency, = cells(core_idle, "entry-latency-us")
        exit_latency, = cells(core_idle, "exit-latency-us")
        wake_latency = entry_latency + exit_latency
        assert re.findall(r"\[Package\] Contains (\d+) Elements:", package) == ["5", "10", "10"], package
        values = [int(x, 16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", package)]
        assert values == [0, 0, 2, 1, 1, 1, 0, 0, 0, residency, wake_latency, 1, 1, 0, 0], package
        buffers = re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)", package, re.M)
        ffh = b"\x82\x0c\x00\x7f\x20\x00\x03"
        wfi = ffh + struct.pack("<Q", 0xFFFFFFFF) + b"\x79\0"
        core_off = ffh + struct.pack("<Q", psci_state) + b"\x79\0"
        assert bytes.fromhex("".join(buffers)) == (wfi + no_counter * 2 + core_off + no_counter * 2), package
        cpu_lpi.append({"uid": uid, "psci_state": hex(psci_state), "min_residency_us": residency,
                        "wake_latency_us": wake_latency, "core_context_lost": True})

    # Exercise the actual guarded AML using simulated SystemMemory only.
    # TSET belongs to a test-only SSDT, never included in the device firmware.
    i2c_cases = []
    fixture = Path(__file__).resolve().parent / "Tests/I2cAcpiTest.asl"
    with tempfile.TemporaryDirectory(prefix="acpi-i2c-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        stem = str(Path(temp) / "I2cTest")
        subprocess.run([iasl, "-p", stem, str(fixture)], capture_output=True, text=True, check=True)
        cases = [(0,0,0,0,0,[0,0,0]), (1,136000000,136000000,1,136000000,[15,0,0]),
                 (2,136000000,136000000,1,136000000,[0,15,0]), (4,136000000,136000000,1,136000000,[0,0,15]),
                 (7,136000000,136000000,1,136000000,[15,15,15]), (7,136000000,136000000,0,136000000,[0,0,0]),
                 (7,999999,1000000001,1,999999,[0,0,0]), (7,1000000,1000000000,1,1000000001,[15,15,0]),
                 (0,136000000,136000000,1,136000000,[0,0,0])]
        for mask, clock0, clock6, revision, clock1, expected in cases:
            command = ("evaluate \\_SB.TSET %d %d %d %d;evaluate \\_SB.TLIV 19 %d;"
                       "evaluate \\_SB.I2T0._STA;evaluate \\_SB.I2C6._STA;evaluate \\_SB.I2T1._STA") % (
                           mask, clock0, clock6, revision, clock1)
            r = subprocess.run([acpiexec, "-b", command, str(paths["DSDT"]), stem+".aml"], capture_output=True, text=True, check=True)
            out = r.stdout+r.stderr
            values = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", out)]
            assert values == expected and "AE_ERROR" not in out and "AE_NOT_FOUND" not in out, (mask,expected,out)
            i2c_cases.append({"ready_mask": mask, "clocks": [clock0,clock6,clock1], "revision": revision, "sta": values})
    # The real runtime AML reads simulated registers; fixture writes are never
    # shipped. Boot handoff remains required but its old frequency is not reused.
    i2c_retained_cases = []
    i2c_restore_cases = []
    fixture = Path(__file__).resolve().parent / "Tests/I2cAcpiTest.asl"
    fields = "IAXI IMUX IFG0 IFG2 IWG0 IWG6 IPM0 IPM6 IPIN IST0 IST6 IDM0 IDM6 MPC0 MPC1 MPWR UPC0 UPC1 UPWR ICL1 IST1 IDM1".split()
    valid = [1,0x200,0,0,0,0,0x111100,0x11000000,0xc03c,0,0,0,0,
             0xff800001,0x150000,1,0xff800001,0x180000,1,136000000,0,0]
    cases = [("UNIVPLL_div20",{},[124800000]*3)]
    cases += [("fixed26",{"IMUX":0},[26000000]*3),
              ("MAINPLL_div32",{"IMUX":0x100},[68250000]*3),
              ("UNIVPLL_postdiv2",{"UPC1":0x1180000},[62400000]*3),
              ("MAINPLL_postdiv2",{"IMUX":0x100,"MPC1":0x1150000},[34125000]*3),
              ("fixed_no_PLL",{"IAXI":0,"IMUX":0,"MPC0":0,"UPC0":0},[26000000]*3),
              ("UNIVPLL_no_MAINPLL",{"IAXI":0,"MPC0":0},[124800000]*3),
              ("MAINPLL_no_UNIVPLL",{"IMUX":0x100,"UPC0":0},[68250000]*3),
              ("unrelated_gate_pin_bits",{"IFG0":1,"IFG2":1,"IWG0":4,"IWG6":1,"IPM0":0xf0111100,"IPM6":0x110000ff},[124800000]*3)]
    cases += [("AXI_parent_%d"%v,{"IAXI":v},[124800000]*3) for v in range(6)]
    denied = [
        ("AXI_unqualified_OSC","IAXI",6),("AXI_invalid","IAXI",7),
        ("I2C_mux_gate","IMUX",0x8200),("I2C_parent_invalid","IMUX",0x300),
        ("I2C_pseudo_gate","IFG0",0x800),("APDMA_pseudo_gate","IFG0",0x1000),
        ("APDMA_gate","IFG2",0x80000000),
        ("MAINPLL_enable","MPC0",0xff800000),("MAINPLL_reset","MPC0",0xff000001),
        ("MAINPLL_divider","MPC0",0xfe800001),("MAINPLL_power","MPWR",0),
        ("MAINPLL_isolation","MPWR",3),("MAINPLL_PCW","MPC1",0),
        ("UNIVPLL_enable","UPC0",0xff800000),("UNIVPLL_reset","UPC0",0xff000001),
        ("UNIVPLL_divider","UPC0",0xfe800001),("UNIVPLL_power","UPWR",0),
        ("UNIVPLL_isolation","UPWR",3),("UNIVPLL_PCW","UPC1",0),
        ("UNIVPLL_VCO_low","UPC1",0x80000),("UNIVPLL_VCO_high","UPC1",0x3fffff),
        ("UNIVPLL_rate_low","UPC1",0x7180000)
    ]
    cases += [(name,{field:value},[0,0,0]) for name,field,value in denied]
    for ix,bus,gate,gate_bit,mode,mode_value,start,dma,pins in [
            (0,0,"IWG0",1,"IPM0",0x110000,"IST0","IDM0",[4,8]),
            (1,6,"IWG6",4,"IPM6",0,"IST6","IDM6",[0x4000,0x8000]),
            (2,1,"IWG0",2,"IPM0",0x1100,"IST1","IDM1",[16,32])]:
        for name,field,value in [
                ("wrapper",gate,gate_bit),("pinmux",mode,mode_value),
                ("controller_active",start,1),("DMA_active",dma,1),
                ("SDA_low","IPIN",0xc03c&~pins[0]),("SCL_low","IPIN",0xc03c&~pins[1])]:
            expected=[124800000]*3;expected[ix]=0
            cases.append(("bus%d_%s"%(bus,name),{field:value},expected))
    with tempfile.TemporaryDirectory(prefix="acpi-i2c-retained-", dir=None if sys.platform=="darwin" else args.build_dir.parent) as temp:
        stem=str(Path(temp)/"I2cLive")
        subprocess.run([iasl,"-p",stem,str(fixture)],capture_output=True,text=True,check=True)
        for name,changes,expected in cases:
            state=valid.copy()
            for field,value in changes.items(): state[fields.index(field)]=value
            command="evaluate \\_SB.TSET 7 136000000 136000000 1;"
            command+=";".join("evaluate \\_SB.TLIV %d 0x%x"%(i,value) for i,value in enumerate(state))
            command+=";evaluate \\_SB.I2T0.IRCK;evaluate \\_SB.I2C6.IRCK;evaluate \\_SB.I2T1.IRCK;evaluate \\_SB.ILCK 3"
            r=subprocess.run([acpiexec,"-b",command,str(paths["DSDT"]),stem+".aml"],capture_output=True,text=True,check=True)
            out=r.stdout+r.stderr
            values=[int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]
            assert values==expected+[0] and not re.search(r"\bAE_[A-Z_]+\b",out),(name,expected,out)
            i2c_retained_cases.append({"case":name,"changes":changes,"source_hz":values[:3],"invalid_bus":values[3]})
        # A revoked boot qualification cannot be replaced with runtime clocks.
        for mask,revision,expected in [(0,1,[0,0,0]),(1,1,[124800000,0,0]),(2,1,[0,124800000,0]),
                                       (4,1,[0,0,124800000]),(7,0,[0,0,0]),(7,2,[0,0,0])]:
            command="evaluate \\_SB.TSET %d 136000000 136000000 %d;"%(mask,revision)
            command+=";".join("evaluate \\_SB.TLIV %d 0x%x"%(i,value) for i,value in enumerate(valid))
            command+=";evaluate \\_SB.I2T0.IRCK;evaluate \\_SB.I2C6.IRCK;evaluate \\_SB.I2T1.IRCK"
            r=subprocess.run([acpiexec,"-b",command,str(paths["DSDT"]),stem+".aml"],capture_output=True,text=True,check=True)
            out=r.stdout+r.stderr
            values=[int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]
            assert values==expected and not re.search(r"\bAE_[A-Z_]+\b",out),(mask,revision,out)
            i2c_retained_cases.append({"case":"boot_qualification","mask":mask,"revision":revision,"source_hz":values})
        # ACPICA keeps the clear-alias write in its own storage. A gate that
        # does not reflect the write must keep the controller unavailable.
        for index, (bus, gate, bit, dma, pin, start) in enumerate((
                ("I2T0", "IWG0", 1, "IDM0", 4, "IST0"),
                ("I2C6", "IWG6", 4, "IDM6", 0x4000, "IST6"),
                ("I2T1", "IWG0", 2, "IDM1", 16, "IST1"))):
            wrote = [0, bit] if index == 1 else [bit, 0]
            gated = {gate: bit | 8}
            for name, mask, revision, changes, expected in [
                    ("already_live", 7, 1, {}, [124800000, 0, 0]),
                    ("enable_not_reflected", 7, 1, gated, [0] + wrote),
                    ("no_boot_handoff", 7 ^ (1 << index), 1, gated, [0, 0, 0]),
                    ("bad_revision", 7, 2, gated, [0, 0, 0]),
                    ("parent_clock_gated", 7, 1, gated | {"IFG0": 0x800}, [0, 0, 0]),
                    ("apdma_clock_gated", 7, 1, gated | {"IFG2": 0x80000000}, [0, 0, 0]),
                    ("pin_low", 7, 1, gated | {"IPIN": 0xc03c & ~pin}, [0, 0, 0]),
                    ("dma_busy", 7, 1, gated | {dma: 1}, [0, 0, 0]),
                    ("unreadable_wrapper", 7, 1, {gate: 0xffffffff}, [0, 0, 0]),
                    ("controller_busy", 7, 1, {start: 1}, [0, 0, 0])]:
                state = valid.copy()
                for field, value in changes.items():
                    state[fields.index(field)] = value
                command = "evaluate \\_SB.TSET %d 136000000 136000000 %d;" % (mask, revision)
                command += ";".join("evaluate \\_SB.TLIV %d 0x%x" % (i, value) for i, value in enumerate(state + [0, 0]))
                command += ";evaluate \\_SB.%s.IRRS;evaluate \\_SB.TWRP 0;evaluate \\_SB.TWRP 1" % bus
                r = subprocess.run([acpiexec, "-b", command, str(paths["DSDT"]), stem + ".aml"],
                                   capture_output=True, text=True, check=True)
                out = r.stdout + r.stderr
                values = [int(x, 16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", out)]
                assert values == expected and not re.search(r"\bAE_[A-Z_]+\b", out), (bus, name, expected, out)
                i2c_restore_cases.append({"bus": bus, "case": name, "source_hz": values[0],
                                          "wrapper_clear_values": values[1:]})
    gpio_cases = []
    fixture = Path(__file__).resolve().parent / "Tests/GpioAcpiTest.asl"
    with tempfile.TemporaryDirectory(prefix="acpi-gpio-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        stem = str(Path(temp) / "GpioTest")
        subprocess.run([iasl, "-p", stem, str(fixture)], capture_output=True, text=True, check=True)
        for ready, pins, irqs, revision, expected in [
            (0,0,0,0,[0,0]), (1,188,126,1,[1,15]), (0,188,126,1,[0,0]),
            (2,188,126,1,[0,0]), (1,187,126,1,[0,0]), (1,188,125,1,[0,0]),
            (1,188,126,0,[0,0]), (1,188,126,2,[0,0])]:
            command = "evaluate \\_SB.GSET 0x%x 0x%x 0x%x 0x%x;evaluate \\_SB.GPO0.GRDY;evaluate \\_SB.GPO0._STA" % (ready,pins,irqs,revision)
            r = subprocess.run([acpiexec, "-b", command, str(paths["DSDT"]), stem+".aml"], capture_output=True, text=True, check=True)
            out = r.stdout+r.stderr
            values = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", out)]
            assert values == expected and "AE_ERROR" not in out and "AE_NOT_FOUND" not in out, (ready,pins,irqs,revision,out)
            gpio_cases.append({"ready":ready,"pins":pins,"interrupt_pins":irqs,"revision":revision,"results":values})
    hall_cases = []
    hall_node = "/soc/odm/hall_para"
    assert raw_property(hall_node, "hall_name") == b"MH248\0"
    assert cells(hall_node, "hall_far_gpvalue") == [0]
    assert cells(hall_node, "hall_int_port") == cells("/soc/pinctrl", "phandle")+[0,1]
    assert raw_property(hall_node, "wakeup-source") == b""
    with tempfile.TemporaryDirectory(prefix="acpi-hall-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        overlays=[]
        for name in ("Gpio", "Hall"):
            stem=str(Path(temp)/name)
            subprocess.run([iasl,"-p",stem,str(Path(__file__).resolve().parent/("Tests/"+name+"AcpiTest.asl"))],capture_output=True,text=True,check=True)
            overlays.append(stem+".aml")
        # Exact-board handoff and controller guards, handler connect/disconnect,
        # and both real input polarities run in AcpiExec virtual memory only.
        valid=[1,0,0,1,1,188,126,1]
        wires=[valid]
        for index,value in [(0,0),(0,2),(1,1),(2,1),(3,0),(3,2),
                            (4,0),(4,2),(5,187),(6,125),(7,0),(7,2)]:
            wire=valid.copy();wire[index]=value;wires.append(wire)
        for wire in wires:
            for connected,raw in [(1,0),(1,1),(0,1)]:
                ready=wire==valid
                command=("evaluate \\_SB.GPO0._REG 8 0;"
                         "evaluate \\_SB.GSET %d %d %d %d;"
                         "evaluate \\_SB.LSET %d %d %d %d;"
                         "evaluate \\_SB.HRAW %d;evaluate \\_SB.GPO0._REG 8 %d;"
                         "evaluate \\_SB.GPO0.HLRD;evaluate \\_SB.LID0._STA;"
                         "evaluate \\_SB.LID0._LID;evaluate \\_SB.GPO0._AEI;"
                         "evaluate \\_SB.GPO0._EVT 0;evaluate \\_SB.GPO0._EVT 1") % (*wire[4:],*wire[:4],raw,connected)
                r=subprocess.run([acpiexec,"-b",command,str(paths["DSDT"]),*overlays],capture_output=True,text=True,check=True)
                out=r.stdout+r.stderr
                values=[int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]
                expected=[int(ready),15 if ready else 0,0 if ready and connected and raw else 1]
                assert values==expected and not re.search(r"\bAE_[A-Z_]+\b",out),(wire,connected,raw,out)
                notifications=re.findall(r"Received a Device Notify on \[LID0\].*Value 0x80",out)
                assert len(notifications)==(2 if ready and connected else 0),(wire,connected,out)
                rows=re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
                inline=re.findall(r"\[Buffer\] Length [0-9A-F]+ =\s*(?:0000:\s*)?((?:[0-9A-F]{2} ?)+)",out)
                resource=bytes.fromhex("".join(rows or inline))
                if ready:
                    assert len(resource)==37 and resource[:9]==bytes.fromhex("8c2000010001001500"),resource.hex()
                    assert struct.unpack_from("<H",resource,12)[0]==1000
                    pin_offset=struct.unpack_from("<H",resource,14)[0]
                    source_offset=struct.unpack_from("<H",resource,17)[0]
                    assert struct.unpack_from("<H",resource,pin_offset)[0]==0
                    assert resource[source_offset:]==b"\\_SB.GPO0\0\x79\0"
                else:
                    assert resource==b"\x79\0",resource.hex()
                hall_cases.append({"handoff":wire[:4],"gpio_handoff":wire[4:],
                    "handler_connected":connected,"raw_gpio":raw,"results":values,
                    "lid_notifications":len(notifications),"aei_bytes":resource.hex()})
        hall_polarity_cases=[]
        for uuid_ok,revision,function in [(1,0,0),(1,0,1),(1,1,0),(1,1,1),(1,0,2),(0,0,0),(0,0,1)]:
            out=subprocess.check_output([acpiexec,"-b","evaluate \\_SB.LDSM %d %d %d"%(uuid_ok,revision,function),str(paths["DSDT"]),*overlays],text=True,stderr=subprocess.STDOUT)
            assert not re.search(r"\bAE_[A-Z_]+\b",out),out
            if uuid_ok and function==1:
                assert "[Package] Contains 1 Elements" in out and re.findall(r"\[Integer\] = ([0-9A-F]+)",out)==["0000000000000000"],out
                returned="high-asserted GPIO0"
            else:
                expected="03" if uuid_ok and function==0 else "00"
                assert re.findall(r"\[Buffer\] Length 01 =\s*(?:0000:\s*)?([0-9A-F]{2})",out)==[expected],out
                returned=expected
            hall_polarity_cases.append({"uuid_match":uuid_ok,"revision":revision,"function":function,"result":returned})
        command=("evaluate \\_SB.GPO0._REG 8 0;evaluate \\_SB.GPO0._REG 3 1;"
                 "evaluate \\_SB.GPO0.HREG;evaluate \\_SB.LID0._DEP")
        out=subprocess.check_output([acpiexec,"-b",command,str(paths["DSDT"]),*overlays],text=True,stderr=subprocess.STDOUT)
        assert not re.search(r"\bAE_[A-Z_]+\b",out),out
        assert [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]==[0],out
        assert re.findall(r"Name (\w+) Device",out)==["GPO0"],out
        assert "Received a Device Notify on [LID0]" not in out,out
    charger = "/soc/i2c@1101a000/sc8989x@6A"
    assert cells(charger, "reg") == [0x6a]
    assert cells(charger, "sc,intr-gpio") == cells("/soc/pinctrl", "phandle") + [12,2]
    power_cases = []
    audio_cases = []
    connectivity_acpi = {}
    with tempfile.TemporaryDirectory(prefix="acpi-power-audio-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        overlays = []
        for name in ("Gpio", "I2c", "Hall", "Power", "Thermal"):
            stem = str(Path(temp)/name)
            subprocess.run([iasl,"-p",stem,str(Path(__file__).resolve().parent/("Tests/"+name+"AcpiTest.asl"))],capture_output=True,text=True,check=True)
            overlays.append(stem+".aml")
        aml = [str(paths["DSDT"]), *overlays]
        aml.extend(ssdts.values())

        def acpi(command):
            out = subprocess.check_output([acpiexec,"-b",command,*aml],text=True,stderr=subprocess.STDOUT)
            assert not re.search(r"\bAE_[A-Z_]+\b",out),out
            return out

        def integer_results(out):
            return [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]

        def resource_bytes(out):
            rows = re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
            inline = re.findall(r"\[Buffer\] Length [0-9A-F]+ =\s*(?:0000:\s*)?((?:[0-9A-F]{2} ?)+)",out)
            return bytes.fromhex("".join(rows or inline))

        hvfs = "/cpuhvfs@00114400"
        cpu_usram, cpu_csram = (cells(hvfs, "reg")[i:i+4] for i in (0, 4))
        assert raw_property(hvfs, "reg-names").split(b"\x00")[:2] == [b"USRAM", b"CSRAM"]
        assert cpu_usram == [0, 0x00114400, 0, 0xc00]
        assert cpu_csram == [0, 0x0011bc00, 0, 0x1400]
        # The stock driver falls back to the two domain registers without
        # the per-CPU control-group property.
        for name in cpu_nodes:
            cpu_node = "/cpus/" + name
            properties = (dt_nodes[cpu_node]["properties"] if dt_reader else
                          subprocess.check_output(["fdtget", "-p", str(args.dtb), cpu_node],
                                                  text=True).splitlines())
            assert "control-group" not in properties, cpu_node
        cpu_freq_resources = [(1, performance_registers[i+1], performance_registers[i+3]) for i in (0, 4)]
        cpu_freq_resources.extend([(0, cpu_usram[1]+0xbfc, 4), (0, cpu_csram[1]+0x4cc, 4)])
        assert integer_results(acpi(r"evaluate \_SB.CPFM._STA")) == [15]
        assert re.findall(r'\[String\].*"(HANA[0-9A-F]+)"', acpi(r"evaluate \_SB.CPFM._HID")) == ["HANA6799"]
        cpu_freq_crs = resource_bytes(acpi(r"evaluate \_SB.CPFM._CRS"))
        assert cpu_freq_crs == b"".join(struct.pack("<BHBII", 0x86, 9, rw, base, length)
                                       for rw, base, length in cpu_freq_resources)+b"\x79\x00"
        for Uid in range(8):
            CpuPath = "\\_SB.CPU%d" % Uid
            assert integer_results(acpi("evaluate " + CpuPath + "._UID")) == [Uid]
            assert re.findall(r'Name (\w+) Device', acpi("evaluate " + CpuPath + "._DEP")) == ["CPFM"]

        # AttribByte defines a status and a single data byte; the second
        # buffer byte is reserved. _PSR's true result must be integer 1.
        commands = [r"evaluate \_SB.ACAD.SBRD (00 00 24)",r"evaluate \_SB.ACAD.SBRD (01 00 24)",
                    r"evaluate \_SB.ACAD.SBRD (00 00)",r"evaluate \_SB.ACAD.SBRD 1"]
        expected = [0x24,0x100,0x100,0x100]
        for source in range(8):
            for power_good in (0,4):
                commands.append(r"evaluate \_SB.ACAD.ISON 0x%x" % (source*32+power_good))
                expected.append(int(bool(power_good) and 0 < source < 7))
        commands.append(r"evaluate \_SB.ACAD.ISON 0x100")
        assert integer_results(acpi(";".join(commands))) == expected + [0]

        adc = "/soc/auxadc@11001000"
        fuse = "/soc/efuse@11c10000"
        clock = "/syscon@10001000"
        adc_reg = cells(adc, "reg")
        gate_address = cells(clock, "reg")[1] + 0x94
        fuse_offset = cells(adc, "mediatek,cali-efuse-reg-offset")[0]
        fuse_address = cells(fuse, "reg")[1] + fuse_offset
        assert cells(adc, "nvmem") == cells(fuse, "phandle")
        assert cells(adc, "clocks") == [cells(clock, "phandle")[0], 23]
        assert [cells(adc, "mediatek,cali-"+field+"-bit")[0] for field in ("en", "ge", "oe")] == [20, 10, 0]
        assert cells("/thermal-ntc1", "io-channels") == [cells(adc, "phandle")[0], 0]
        assert cells("/thermal-ntc2", "io-channels") == [cells(adc, "phandle")[0], 1]
        assert cells("/thermal-ntc1", "temperature-lookup-table") == cells("/thermal-ntc2", "temperature-lookup-table")
        ntc = acpi(r"evaluate \_SB.NTC0._HID;evaluate \_SB.NTC0._PR0")
        assert '"HANA6794"' in ntc and "AXCK" in ntc, ntc
        lvts = "/soc/lvts@1100B000"
        lvts_reg = cells(lvts, "reg")
        lvts_fuses = [fuse + "/data1", fuse + "/data2"]
        assert cells(lvts, "clocks") == [cells(clock, "phandle")[0], 4]
        assert cells(lvts, "nvmem-cells") == [cells(node, "phandle")[0] for node in lvts_fuses]
        lvts_calibration = [cells(node, "reg") for node in lvts_fuses]
        assert [length for _, length in lvts_calibration] == [0x2c, 8]
        for index, zone in enumerate(("soc_max", "cpu_little1", "cpu_little2", "cpu_little3",
                                      "cpu_little4", "cpu_big1", "cpu_big2", "cpu_big3", "cpu_big4",
                                      "gpu2", "gpu1", "soc1", "soc2", "md")):
            assert cells("/thermal-zones/"+zone, "thermal-sensors") == [cells(lvts, "phandle")[0], index]
        soc_trip = "/thermal-zones/soc_max/trips/soc_max_crit@0"
        assert raw_property(soc_trip, "type") == b"critical\x00"
        trip_millicelsius, = cells(soc_trip, "temperature")
        critical = (trip_millicelsius + 273150 + 50) // 100
        dependency = acpi(r"evaluate \_TZ.SOC0._DEP")
        assert re.findall(r"Name (\w+) Device", dependency) == ["NTC0"], dependency
        assert integer_results(acpi(r"evaluate \_TZ.SOC0._CRT")) == [critical]
        thermal_uuid = "(CD 99 D3 14 27 7A 18 4B 8F B4 7C B7 B9 F4 E5 00)"
        thermal_dsm = r"evaluate \_TZ.SOC0._DSM "
        assert resource_bytes(acpi(thermal_dsm + thermal_uuid + " 0 0 [0]")) == b"\x05"
        sensor = acpi(thermal_dsm + thermal_uuid + " 0 2 [0]")
        assert re.findall(r'\[String\].*("[^"]+")', sensor) == [json.dumps(r"\_SB.NTC0")], sensor
        for query in (thermal_uuid+" 1 2 [0]", thermal_uuid+" 0 1 [0]",
                      thermal_uuid+" 0 3 [0]", "(00) 0 2 [0]", "0 0 2 [0]"):
            assert resource_bytes(acpi(thermal_dsm+query)) == b"\x00", query
        ntc_resources = [(1, adc_reg[1], adc_reg[3]), (0, gate_address, 4), (0, fuse_address, 4),
                         (0, lvts_reg[1], lvts_reg[3]), (0, cells(clock, "reg")[1]+0x90, 4)]
        efuse_base = cells(fuse, "reg")[1]
        ntc_resources.extend((0, efuse_base + offset, length) for offset, length in lvts_calibration)
        descriptor = b"".join(struct.pack("<BHBII", 0x86, 9, rw, base, length)
                              for rw, base, length in ntc_resources) + b"\x79\x00"
        assert resource_bytes(acpi(r"evaluate \_SB.NTC0._CRS")) == descriptor
        commands = [r"evaluate \_SB.NSET 0x800", r"evaluate \_SB.AXCK._STA",
                    r"evaluate \_SB.NSET 0xc00", r"evaluate \_SB.AXCK._STA",
                    r"evaluate \_SB.AXCK._ON", r"evaluate \_SB.NGET",
                    r"evaluate \_SB.AXCK._OFF", r"evaluate \_SB.NGET"]
        ntc_gate_checks = integer_results(acpi(";".join(commands)))
        assert ntc_gate_checks == [1, 0, 0, 0x400, 0x400, 0x400], ntc_gate_checks
        for status, written in ((0, 0), (0x200, 0), (0x600, 0x400), (0xffffffff, 0)):
            steps = (rf"evaluate \_SB.VTST {status:#x};evaluate \_SB.NTC0.LVON;"
                     r"evaluate \_SB.VTRD")
            # The emulator keeps set/clear aliases and live status independent.
            assert integer_results(acpi(steps)) == [status, written], steps

        hall_irq = bytes.fromhex("8c2000010001001500000000e80317000019002300000000005c5f53422e47504f3000")
        charger_irq = bytes.fromhex("8c200001000100030000000000001700001900230000000c005c5f53422e47504f3000")
        for charger_ready,bus,gpio,hall,handler in [
            (0,1,1,1,1),(1,0,1,1,1),(1,1,0,1,1),
            (1,1,1,0,0),(1,1,1,0,1),(1,1,1,1,1),(2,1,1,0,1)]:
            ready = charger_ready == 1 and bus and gpio
            command = (r"evaluate \_SB.ACAD._REG 9 0;"
                       r"evaluate \_SB.ASET %d;evaluate \_SB.TSET %d 0 136000000 1;"
                       r"evaluate \_SB.GSET %d 188 126 1;evaluate \_SB.LSET %d 0 0 1;"
                       r"evaluate \_SB.ACAD._REG 9 %d;evaluate \_SB.ACAD._STA;"
                       r"evaluate \_SB.GPO0._AEI;evaluate \_SB.GPO0._EVT 12;"
                       r"evaluate \_SB.GPO0._EVT 13;evaluate \_SB.ACAD._REG 9 0;"
                       r"evaluate \_SB.GPO0._EVT 12;evaluate \_SB.ACAD._PSR;"
                       r"evaluate \_SB.BAT0.ACST") % (
                           charger_ready,2*bus,gpio,hall,handler)
            out = acpi(command)
            assert integer_results(out)==[15 if ready else 0,0,2],out
            assert out.count("Received a Device Notify on [ACAD]")==(2 if ready and handler else 0),out
            resource = resource_bytes(out)
            assert resource==(hall_irq if hall and gpio else b"")+(charger_irq if ready else b"")+b"\x79\x00",out
            power_cases.append({"handoff_ready":charger_ready,"i2c6":bus,"gpio":gpio,"hall":hall,
                                "handler":handler,"acpi_ready":bool(ready),"gpio_event_resource":resource.hex()})

        # Compare the installed enumeration resources with the stock DT in
        # order. The firmware advertises these devices as present, not ready.
        for table, name, hid, node, compatible in (
            ("T70BTIF", "BTI0", "HANA6795", "/soc/btif@1100c000", b"mediatek,btif"),
            ("T70WCON", "WCN0", "HANA6796", "/soc/wifi@18000000", b"mediatek,wifi"),
            ("T70CELL", "DPM0", "HANA6797", "/soc/dpmaif@10014000", b"mediatek,dpmaif"),
            ("T70CELL", "CCF0", "HANA6798", "/soc/ccifdriver@10209000", b"mediatek,ccci_ccif"),
        ):
            if table not in ssdts:
                continue
            assert raw_property(node, "compatible") == compatible + b"\x00"
            registers = cells(node, "reg")
            interrupts = cells(node, "interrupts")
            assert registers and len(registers) % 4 == 0 and interrupts and len(interrupts) % 4 == 0
            assert not any(registers[0::4] + registers[2::4])
            windows = list(zip(registers[1::4], registers[3::4]))
            extra_vectors = []
            if name == "WCN0":
                # BTCVSD control/SRAM windows live inside the shared WCN aperture.
                # Enumerate the distinct SCO GSI without claiming overlapping MMIO.
                sco = "/mtk-btcvsd-snd@18050000"
                assert raw_property(sco, "compatible") == b"mediatek,mtk-btcvsd-snd\x00"
                sco_regs = cells(sco, "reg")
                assert sco_regs == [0, 0x18050000, 0, 0x1000,
                                    0, 0x18080000, 0, 0x10000]
                sco_windows = list(zip(sco_regs[1::4], sco_regs[3::4]))
                assert all(any(base <= start and start + size <= base + length
                               for base, length in windows)
                           for start, size in sco_windows)
                assert len(cells(sco, "interrupts")) == 4 and irq(sco) == 439
                extra_vectors.append(irq(sco))
                # Stock consys uses only the keyed CONN reset field at RGU+0x18.
                rgu = cells("/soc/consys@18002000", "reg")[4:8]
                assert rgu == cells("/soc/watchdog@10007000", "reg")
                assert len(rgu) == 4 and rgu[0] == rgu[2] == 0 and rgu[3] >= 0x1c
                windows.append((rgu[1] + 0x18, 4))
            vectors = extra_vectors + [irq(node, i) for i in range(len(interrupts) // 4)]
            prefix = r"evaluate \_SB." + name
            out = acpi(";".join(prefix + "." + field for field in ("_HID", "_UID", "_STA", "_CRS")))
            assert re.findall(r'\[String\].* = "([^"]+)"', out) == [hid], out
            assert integer_results(out) == [0, 15 if name in ("DPM0", "CCF0") else 7], out
            memory = b"".join(b"\x86\x09\x00\x01" + struct.pack("<II", *window)
                              for window in windows)
            handoff = b""
            if name == "DPM0":
                # The fixed CCCI handoff follows the DT MMIO windows and IRQ in _CRS.
                handoff = b"\x86\x09\x00\x00" + struct.pack("<II", 0x43F7F180, 0x40)
            irq_descriptors = b"".join(b"\x89\x06\x00\x01\x01" + struct.pack("<I", vector)
                                       for vector in vectors)
            assert resource_bytes(out) == memory + irq_descriptors + handoff + b"\x79\x00", out
            connectivity_acpi[name] = {"hid": hid, "sta": 15 if name in ("DPM0", "CCF0") else 7, "registers": windows, "gsiv": vectors}

        if "T70AUDIO" in paths:
            afe = "/soc/mt6789-afe-pcm@11210000"
            assert cells(afe,"reg")==[0,0x11210000,0,0x2000] and irq(afe)==266
            i2c1 = "/soc/i2c@11e01000"
            assert cells(i2c1,"clock-frequency")==[400000]
            for channel in range(2):
                amp = i2c1+"/aw882xx@%x"%(0x34+channel)
                assert cells(amp,"reg")==[0x34+channel] and cells(amp,"sound-channel")==[channel]
            afe_desc = bytes.fromhex("86090001000021110020000089060001010a010000")
            amp_prefix = bytes.fromhex("8e1900020001020000010600801a0600")
            amp_desc = b"".join(amp_prefix+struct.pack("<H",address)+b"\\_SB.I2T1\0" for address in (0x34,0x35))
            for mask,rate,revision in [(0,136000000,1),(4,0,1),(4,136000000,0),(4,136000000,1)]:
                ready = mask==4 and rate!=0 and revision==1
                out = acpi((r"evaluate \_SB.TSET %d 0 0 %d;evaluate \_SB.TLIV 19 %d;"
                            r"evaluate \_SB.AFE0._CRS;evaluate \_SB.AFE0._DEP")%(mask,revision,rate))
                assert re.findall(r"Name (\w+) Device",out)==(["I2T1"] if ready else []),out
                assert resource_bytes(out)==afe_desc+(amp_desc if ready else b"")+b"\x79\x00",out
                audio_cases.append({"i2c1_ready_mask":mask,"i2c1_source_hz":rate,
                                    "revision":revision,"amplifier_resources":ready})

    touch_cases = []
    touch_resume_cases = []
    with tempfile.TemporaryDirectory(prefix="acpi-touch-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        overlays = []
        for name in ("I2c", "Gpio", "Himax"):
            stem = str(Path(temp) / name)
            subprocess.run([iasl, "-p", stem, str(Path(__file__).resolve().parent / ("Tests/"+name+"AcpiTest.asl"))], capture_output=True, text=True, check=True)
            overlays.append(stem+".aml")
        for ready, address, pin, revision, bus, gpio, expected in [
            (0,0,0,0,0,0,[0,0]), (1,0x48,9,1,1,1,[1,15]),
            (0,0x48,9,1,1,1,[0,0]), (2,0x48,9,1,1,1,[0,0]),
            (1,0x49,9,1,1,1,[0,0]), (1,0x48,10,1,1,1,[0,0]),
            (1,0x48,9,0,1,1,[0,0]), (1,0x48,9,2,1,1,[0,0]),
            (1,0x48,9,1,0,1,[0,0]), (1,0x48,9,1,2,1,[0,0]),
            (1,0x48,9,1,1,0,[0,0])]:
            command = ("evaluate \\_SB.TSET %d 136000000 136000000 1;"
                       "evaluate \\_SB.GSET %d 188 126 1;"
                       "evaluate \\_SB.HSET %d %d %d %d;"
                       "evaluate \\_SB.HXT0.HMXR;evaluate \\_SB.HXT0._STA") % (bus,gpio,ready,address,pin,revision)
            r = subprocess.run([acpiexec, "-b", command, str(paths["DSDT"]), *overlays], capture_output=True, text=True, check=True)
            out = r.stdout+r.stderr
            values = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", out)]
            assert values == expected and "AE_ERROR" not in out and "AE_NOT_FOUND" not in out, (command,expected,out)
            touch_cases.append({"handoff":[ready,address,pin,revision],"parents":[bus,gpio],"results":values})
        for fault, token, ready in [(n, 1, 1) for n in range(9)] + [
                (0,0,1), (0,0xFFFFFF,1), (0,0x1000000,1), (0,1,0)]:
            expected = (token << 8) | 1 if not fault and 0 < token <= 0xFFFFFF and ready else 0
            command = (r"evaluate \_SB.TSET 1 136000000 136000000 1;"
                       r"evaluate \_SB.GSET 1 188 126 1;"
                       r"evaluate \_SB.HSET %d 0x48 9 1;"
                       r"evaluate \_SB.HRTS %d;"
                       r"evaluate \_SB.HXT0.HMRD 0x%x") % (ready, fault, token)
            r = subprocess.run([acpiexec, "-b", command, str(paths["DSDT"]), *overlays],
                               capture_output=True, text=True, check=True)
            out = r.stdout+r.stderr
            values = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", out)]
            assert values == [expected] and "AE_ERROR" not in out and "AE_NOT_FOUND" not in out, (command,expected,out)
            touch_resume_cases.append({"pad_fault":fault,"token":token,"handoff_ready":ready,"result":values[0]})
        # Decode the actual AML resource buffer, not just the ASL source text.
        r = subprocess.run([acpiexec, "-b", "evaluate \\_SB.HXT0._CRS;evaluate \\_SB.HXT0._DEP", str(paths["DSDT"])], capture_output=True, text=True, check=True)
        out = r.stdout+r.stderr
        assert "AE_ERROR" not in out and "AE_NOT_FOUND" not in out, out
        rows = re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
        resource = bytes.fromhex("".join(rows))
        touch_node = "/soc/i2c@11e00000/himax_ts@48"
        assert cells(touch_node,"reg") == [0x48]
        assert cells(touch_node,"interrupts") == [9,2,9,0]
        assert cells(touch_node,"himax,irq-gpio")[1:] == [9,0]
        assert cells(touch_node,"himax,rst-gpio")[1:] == [152,0]
        serial_length = struct.unpack_from("<H",resource,1)[0]+3
        serial = resource[:serial_length]
        assert serial[0] == 0x8e and serial[3:12] == bytes([2,0,1,2,0,0,1,6,0]), serial.hex()
        assert struct.unpack_from("<IH",serial,12) == (400000,0x48)
        assert serial[18:] == b"\\_SB.I2T0\0"
        gpio_desc = resource[serial_length:]
        assert gpio_desc[0] == 0x8c and gpio_desc[3:9] == bytes([1,0,1,0,3,0]), gpio_desc.hex()
        pin_offset = struct.unpack_from("<H",gpio_desc,14)[0]
        source_offset = struct.unpack_from("<H",gpio_desc,17)[0]
        assert struct.unpack_from("<H",gpio_desc,pin_offset)[0] == 9
        assert gpio_desc[source_offset:source_offset+10] == b"\\_SB.GPO0\0"
        assert "[Package] Contains 2 Elements" in out, out
        assert re.findall(r"Name (\w+) Device",out) == ["I2T0","GPO0"], out
        assert b"HANA8313\0" in tables["DSDT"]
        inf = Path(__file__).resolve().parents[3] / "Windows/HimaxTouch/HimaxTouch.inf"
        assert r"ACPI\HANA8313" in inf.read_text()
    # MSDC1 is not SDHCI. Enumerate the actual board resources for the future
    # custom host driver; no PNP0D40/inbox-driver or OS data-path qualification.
    sd_node = "/soc/mmc@11240000"
    sd_regs = cells(sd_node, "reg")
    sd_cd = cells(sd_node, "cd-gpios")
    assert sd_regs == [0,0x11240000,0,0x1000,0,0x11ef0000,0,0x1000]
    assert sd_cd == cells("/soc/pinctrl", "phandle")+[7,0]
    assert cells(sd_node, "host-index") == [1]
    sd_cases = []
    with tempfile.TemporaryDirectory(prefix="acpi-sd-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        stem = str(Path(temp)/"GpioTest")
        subprocess.run([iasl,"-p",stem,str(Path(__file__).resolve().parent/"Tests/GpioAcpiTest.asl")],capture_output=True,text=True,check=True)
        for ready,pins,irqs,revision,expected in [
                (1,188,126,1,15),(0,188,126,1,0),(2,188,126,1,0),
                (1,187,126,1,0),(1,188,125,1,0),(1,188,126,0,0),(1,188,126,2,0)]:
            command = "evaluate \\_SB.GSET %d %d %d %d;evaluate \\_SB.SDC1._STA" % (ready,pins,irqs,revision)
            out = subprocess.check_output([acpiexec,"-b",command,str(paths["DSDT"]),stem+".aml"],text=True,stderr=subprocess.STDOUT)
            assert not re.search(r"\bAE_[A-Z_]+\b",out),out
            values = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]
            assert values == [expected],out
            sd_cases.append({"gpio_handoff":[ready,pins,irqs,revision],"sta":values[0]})
    out = subprocess.check_output([acpiexec,"-b","evaluate \\_SB.SDC1._CRS",str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
    assert not re.search(r"\bAE_[A-Z_]+\b",out),out
    rows = re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
    sd_resource = bytes.fromhex("".join(rows))
    sd_prefix = b"\x86\x09\x00\x01" + struct.pack("<II", sd_regs[1], sd_regs[3])
    sd_prefix += b"\x89\x06\x00\x01\x01" + struct.pack("<I", irq(sd_node))
    assert sd_resource.startswith(sd_prefix), sd_resource.hex()
    offset = len(sd_prefix)
    # The slot exposes an exclusive both-edge IRQ followed by the GPIO input.
    # In the interrupt descriptor, 0x05 encodes edge-triggered and active-both.
    for kind, flags in ((0, 0x05), (1, 0x01)):
        size = struct.unpack_from("<H", sd_resource, offset + 1)[0] + 3
        gpio = sd_resource[offset:offset + size]
        assert gpio[:1] == b"\x8c" and gpio[3:10] == bytes([1, kind, 1, 0, flags, 0, 0]), gpio.hex()
        assert struct.unpack_from("<H", gpio, 12)[0] == 0, gpio.hex()
        pin_offset = struct.unpack_from("<H", gpio, 14)[0]
        source_offset = struct.unpack_from("<H", gpio, 17)[0]
        assert source_offset - pin_offset == 2 and struct.unpack_from("<H", gpio, pin_offset)[0] == sd_cd[1], gpio.hex()
        assert gpio[source_offset:] == b"\\_SB.GPO0\0", gpio.hex()
        offset += size
    sd_top = b"\x86\x09\x00\x01" + struct.pack("<II", sd_regs[5], sd_regs[7])
    assert sd_resource[offset:] == sd_top + b"\x79\x00", sd_resource.hex()
    out = subprocess.check_output([acpiexec,"-b","evaluate \\_SB.SDC1._DEP;evaluate \\_SB.SDC1._DSD;evaluate \\_SB.SDC1._HID;evaluate \\_SB.SDC1._UID;evaluate \\_SB.SDC1._CCA;evaluate \\_SB.SDC1._RMV",str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
    assert not re.search(r"\bAE_[A-Z_]+\b",out),out
    # cd-gpios selects the input descriptor at GPIO resource index one.
    # The final zero preserves the stock active-high presence level.
    assert re.findall(r"Name (\w+) Device",out) == ["GPO0","SDC1"],out
    assert all('"'+name+'"' in out for name in ("bus-width","max-frequency","cd-gpios","HANA6792")),out
    sd_values = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]
    assert sd_values == [*cells(sd_node,"bus-width"),*cells(sd_node,"max-frequency"),1,0,sd_cd[2],1,0,0],out
    sd_acpi = {"node":sd_node,"device":"\\_SB.SDC1","hid":"HANA6792",
        "registers":sd_regs,"gsiv":irq(sd_node),"card_detect_gpio":sd_cd[1:],
        "card_detect_gpio_interrupt":{"pin":sd_cd[1],"trigger":"both-edge","wake":False},
        "resource_bytes":sd_resource.hex(),"gpio_dependency_cases":sd_cases,
        "properties_evaluated":True,"sta_scope":"fixed controller presence with valid GPIO provider, not media readiness",
        "SDHCI_compatibility_claimed":False,"Windows_host_driver_implemented":False,
        "power_clock_resume_implemented":False,"physical_execution":False}

    ufs_cases = []
    with tempfile.TemporaryDirectory(prefix="acpi-ufs-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        stem = str(Path(temp) / "UfsTest")
        subprocess.run([iasl, "-p", stem, str(Path(__file__).resolve().parent / "Tests/UfsAcpiTest.asl")], capture_output=True, text=True, check=True)
        for ready, address, lun, revision, expected in [
            (0,0,0,0,[0,0]), (1,0x11270000,2,1,[1,15]),
            (0,0x11270000,2,1,[0,0]), (2,0x11270000,2,1,[0,0]),
            (1,0x11271000,2,1,[0,0]), (1,0x11270000,0,1,[0,0]),
            (1,0x11270000,2,0,[0,0]), (1,0x11270000,2,2,[0,0])]:
            command = "evaluate \\_SB.USET %d %d %d %d;evaluate \\_SB.UFS0.URDY;evaluate \\_SB.UFS0._STA" % (ready,address,lun,revision)
            r = subprocess.run([acpiexec, "-b", command, str(paths["DSDT"]), stem+".aml"], capture_output=True, text=True, check=True)
            out = r.stdout+r.stderr
            values = [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", out)]
            assert values == expected and "AE_ERROR" not in out and "AE_NOT_FOUND" not in out, (command,out)
            ufs_cases.append({"handoff":[ready,address,lun,revision],"results":values})
        r = subprocess.run([acpiexec, "-b", "evaluate \\_SB.UFS0._CRS;evaluate \\_SB.UFS0._CCA", str(paths["DSDT"])], capture_output=True, text=True, check=True)
        out = r.stdout+r.stderr
        assert "AE_ERROR" not in out and "AE_NOT_FOUND" not in out, out
        rows = re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
        ufs_resource = bytes.fromhex("".join(rows))
        ufs_regs = cells("/soc/ufshci@11270000","reg")
        assert ufs_regs == [0,0x11270000,0,0x2300]
        assert ufs_resource[:12] == b"\x86\x09\x00\x01"+struct.pack("<II",ufs_regs[1],ufs_regs[3])
        assert ufs_resource[12:24] == b"\x86\x09\x00\x00"+struct.pack("<II",0x43f7f0b0,16)
        assert ufs_resource[24:33] == b"\x89\x06\x00\x01\x01"+struct.pack("<I",irq("/soc/ufshci@11270000"))
        assert re.findall(r"\[Integer\] = ([0-9A-F]+)",out) == ["0000000000000000"]
        assert b"HANA678B\0" in tables["DSDT"]
        inf = Path(__file__).resolve().parents[3] / "Windows/MtkUfs/MtkUfs.inf"
        assert r"ACPI\HANA678B" in inf.read_text()
    display_cases = []
    with tempfile.TemporaryDirectory(prefix="acpi-display-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        stem = str(Path(temp)/"DisplayTest")
        subprocess.run([iasl,"-p",stem,str(Path(__file__).resolve().parent/"Tests/DisplayAcpiTest.asl")],capture_output=True,text=True,check=True)
        baseline=[1,1,0x80000000,0,0xc00000,0,1400,2240,5600,1,0,0]
        for changes in [[],[(0,0)],[(0,2)],[(1,0)],[(2,0x80000001)],[(2,0)],[(3,16)],
                        [(4,0xbf67ff)],[(4,0x4000001)],[(5,1)],[(6,0)],[(6,4097)],[(7,0)],
                        [(7,4097)],[(8,5599)],[(8,65540)],[(9,2)],[(10,1)],[(11,1)],
                        [(2,0xfffff000),(3,15)]]:
            wire=baseline.copy()
            command="evaluate \\_SB.DINI;"
            for index,value in changes:
                wire[index]=value
                command+="evaluate \\_SB.DSET %d 0x%x;" % (index,value)
            command+="evaluate \\_SB.DISP.DRDY;evaluate \\_SB.DISP._STA;evaluate \\_SB.DISP.DFBI"
            r=subprocess.run([acpiexec,"-b",command,str(paths["DSDT"]),stem+".aml"],capture_output=True,text=True,check=True)
            out=r.stdout+r.stderr
            values=[int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]
            expected=[1,15] if not changes else [0,0]
            assert values==expected and "AE_ERROR" not in out and "AE_NOT_FOUND" not in out,(changes,out)
            rows=re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
            returned=bytes.fromhex("".join(rows))
            wire[0]=expected[0];wire[10]=wire[11]=0
            assert returned==struct.pack("<12I",*wire),(changes,returned.hex(),wire,out)
            display_cases.append({"changes":changes,"ready_and_sta":values,"dfbi":returned.hex()})
        assert b"HANA678C\0" in tables["DSDT"]
        assert b"\x86\x09\x00\x00"+struct.pack("<II",0x43f7f0c0,48) in tables["DSDT"]
        inf=Path(__file__).resolve().parents[3]/"Windows/MtkDisplay/MtkDisplay.inf"
        assert r"ACPI\HANA678C" in inf.read_text()
    musb_cases = []
    musb_gate_cases = []
    with tempfile.TemporaryDirectory(prefix="acpi-musb-", dir=None if sys.platform == "darwin" else args.build_dir.parent) as temp:
        stem=str(Path(temp)/"MusbTest")
        subprocess.run([iasl,"-p",stem,str(Path(__file__).resolve().parent/"Tests/MusbAcpiTest.asl")],capture_output=True,text=True,check=True)
        valid_changes=[[],[(12,0x101)],[(12,0x103)],[(12,0x303)],[(12,0x503)],[(13,31)],
                       [(1,2),(15,1)],[(1,2),(2,0xef),(15,2)],[(1,2),(2,0x7f),(15,3)]]
        mutations=valid_changes+[[(i,0)] for i in range(12)]+[[(12,0)],[(12,0x102)],[(12,0x703)],[(12,0x110)],[(13,32)],[(14,0)],[(15,1)]]
        mutations += [[(0,2)],[(1,2)],[(2,0xff)]]
        mutations += [[(2,0x7f ^ (1<<i))] for i in range(7)]
        mutations += [[(1,2),(2,0x7f ^ (1<<i)),(15,1)] for i in range(7)]
        mutations += [[(1,2),(2,0xef ^ (1<<i)),(15,2)] for i in (0,1,2,3,5,6,7)]
        mutations += [[(1,2),(15,2)],[(1,2),(2,0xef),(15,1)],
                      [(1,2),(2,0x6f),(15,2)],[(1,2),(2,0xff),(15,2)],
                      [(1,2),(2,0x1ef),(15,1)],[(1,2),(2,0xef),(15,3)],
                      [(1,2),(2,0x6f),(15,3)],
                      [(1,3),(2,0x7f),(15,1)],
                      [(1,1),(2,0xef),(15,2)],[(1,1),(15,1)],[(1,1),(2,0x6f)]]
        for changes in mutations:
            command="evaluate \\_SB.MINI;"
            for index,value in changes: command+="evaluate \\_SB.MSET %d 0x%x;"%(index,value)
            command+="evaluate \\_SB.USB0.MHRD;evaluate \\_SB.USB0._STA"
            result=subprocess.run([acpiexec,"-b",command,str(paths["DSDT"]),stem+".aml"],capture_output=True,text=True,check=True)
            out=result.stdout+result.stderr
            values=[int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]
            expected=[1,15] if changes in valid_changes else [0,0]
            assert values==expected and "AE_ERROR" not in out and "AE_NOT_FOUND" not in out,(changes,out)
            musb_cases.append({"changes":changes,"ready_and_sta":values})
        # MCLK returns an opaque identity of the exact retained rate settings.
        clock = 0x1000007e
        live = [0x01000000, 0, 0x980000, 5, 0xff800001, 0x180000, 1]
        for changes, expected in [
            ({}, clock), ({0: 0}, 0x7d), ({0: 0x02000000}, clock + 1), ({0: 0x03000000}, clock + 2),
            ({2: 0x97ffe1}, 0x10000002), ({2: 0x98001f}, 0x100000fa),
            ({5: 0x180001}, clock + 0x100), ({5: 0x1180000}, clock + 0x20000000),
            ({0: 0x81000000}, 0), ({1: 2}, 0), ({2: 0x970000}, 0), ({2: 0x1980000}, 0),
            ({2: 0x180000}, 0),
            ({3: 1}, 0), ({3: 4}, 0), ({3: 7}, 0), ({4: 0xff800000}, 0), ({5: 0}, 0), ({6: 3}, 0),
        ]:
            regs = [changes.get(i, value) for i, value in enumerate(live)]
            command = "evaluate \\_SB.MLIV " + " ".join(hex(value) for value in regs)
            command += ";evaluate \\_SB.USB0.MCLK"
            out = subprocess.check_output([acpiexec, "-b", command, str(paths["DSDT"]), stem+".aml"],
                                          text=True, stderr=subprocess.STDOUT)
            values = [int(x, 16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", out)]
            assert values == [expected] and not re.search(r"\bAE_[A-Z_]+\b", out), (changes, expected, out)
            musb_cases.append({"live_clock_registers": regs, "clock_identity": values[0]})
        # Register writes in the production AML, including the complete PLL
        # sequence. AcpiExec does not latch W1C alias writes into status.
        top_clear = [(0x100000a8, 0x80000000)]
        bus_clear = [(0x100010a8, 2)]
        pll_on = [(0x1000c3cc, 3), (0x1000c3cc, 1), (0x1000c3cc, 5), (0x1000c3c4, 0x980000)]
        program = [(0x1000c3c4, 0x10180000), (0x1000c3c8, 0x80340012)]
        pll_off = {2: 0x180000, 3: 2}
        blank = {0: 0x81000000, 1: 2, 2: 0x10000000, 3: 0x102}
        blank_on = [(0x1000c3cc, 0x103), (0x1000c3cc, 0x101), (0x1000c3cc, 0x105),
                    (0x1000c3c4, 0x10980000)]
        for name, changes, identity, expected, writes in [
            ("blank_isolated_pll", blank, clock, 0, program + blank_on + top_clear),
            ("blank_at_lowest_pcw", blank, 0x10000002, 0,
             [(0x1000c3c4, 0x1017ffe1), program[1], *blank_on[:3],
              (0x1000c3c4, 0x1097ffe1), *top_clear]),
            ("blank_at_highest_pcw", blank, 0x100000fa, 0,
             [(0x1000c3c4, 0x1018001f), program[1], *blank_on[:3],
              (0x1000c3c4, 0x1098001f), *top_clear]),
            ("blank_with_fixed_parent", blank | {0: 0x80000000}, 0x7d, 0, program + blank_on + top_clear),
            ("blank_with_top_open", blank | {0: 0x01000000}, clock, 0, []),
            ("blank_with_bus_open", blank | {1: 0}, clock, 0, []),
            ("blank_with_output_on", blank | {3: 5}, clock, 0, []),
            ("blank_without_isolation", blank | {3: 0}, clock, 0, []),
            ("blank_with_reset_released", blank | {2: 0x10800000}, clock, 0, []),
            ("blank_with_pdiv", blank | {2: 0x11000000}, clock, 0, []),
            ("blank_with_selector_drift", blank | {0: 0x82000000}, clock, 0, []),
            ("blank_with_parent_pcw_drift", blank | {5: 0x180001}, clock, 0, []),
            ("blank_with_parent_divider_drift", blank | {5: 0x1180000}, clock, 0, []),
            ("blank_with_parent_off", blank | {4: 0}, clock, 0, []),
            ("blank_with_unencodable_pcw", blank, 0x100000fe, 0, []),
            ("blank_with_identity_overflow", blank, 0x11000007e, 0, []),
            ("retained", {}, clock, clock, []),
            ("powered_pll_in_reset", {2: 0x180000}, clock, 0, []),
            ("powered_pll_in_reset_with_gates_off", {0: 0x81000000, 1: 2, 2: 0x180000}, clock, 0, []),
            ("bus_gate", {1: 2}, clock, 0, bus_clear),
            ("bus_gate_unrelated_bits", {1: 0xffffffff}, clock, 0, bus_clear),
            ("fixed_parent", {0: 0, 1: 2}, 0x7d, 0, bus_clear),
            ("alternate_parent_2", {0: 0x02000000, 1: 2}, clock + 1, 0, bus_clear),
            ("alternate_parent_3", {0: 0x03000000, 1: 2}, clock + 2, 0, bus_clear),
            ("zero_identity", {1: 2}, 0, 0, []),
            ("different_identity", {1: 2}, clock + 1, 0, []),
            ("top_gate", {0: 0x81000000, 1: 2}, clock, 0, top_clear),
            ("usb_pll_off", pll_off, clock, clock, pll_on),
            ("usb_pll_and_bus_off", pll_off | {1: 2}, clock, 0, pll_on + bus_clear),
            ("all_usb_clocks_off", pll_off | {0: 0x81000000, 1: 2}, clock, 0, pll_on + top_clear),
            ("pll_unrelated_bits", {2: 0x10180000, 3: 0x102}, clock, clock,
             [(0x1000c3cc, 0x103), (0x1000c3cc, 0x101), (0x1000c3cc, 0x105), (0x1000c3c4, 0x10980000)]),
            ("pll_state_incomplete", {1: 2, 3: 1}, clock, 0, []),
            ("pll_state_isolated_with_output", {1: 2, 3: 7}, clock, 0, []),
            ("pll_reset_state_unexpected", {1: 2, 2: 0x980000, 3: 2}, clock, 0, []),
            ("usb_pll_unprogrammed", {1: 2, 2: 0, 3: 2}, clock, 0, []),
            ("usb_pll_pcw_low", {1: 2, 2: 0x17ffe0}, clock, 0, []),
            ("usb_pll_pcw_high", {1: 2, 2: 0x180020}, clock, 0, []),
            ("usb_pll_pcw_drift_same_rounded_rate", {1: 2, 2: 0x180001, 3: 2}, clock, 0, []),
            ("shared_parent_pcw_drift_same_rounded_rate", pll_off | {1: 2, 5: 0x180001}, clock, 0, []),
            ("shared_parent_divider_changed", pll_off | {1: 2, 5: 0x1180000}, clock, 0, []),
            ("shared_parent_off", pll_off | {1: 2, 4: 0}, clock, 0, []),
            ("shared_parent_isolated", pll_off | {1: 2, 6: 3}, clock, 0, []),
        ]:
            regs = [changes.get(i, value) for i, value in enumerate(live)]
            command = "evaluate \\_SB.MLIV " + " ".join(hex(value) for value in regs)
            command += f";evaluate \\_SB.MSTC 0x340012;evaluate \\_SB.USB0.MGON {identity:#x}"
            result = subprocess.run([acpiexec, "-vr", "-b", command, str(paths["DSDT"]), stem+".aml"],
                                    capture_output=True, text=True, check=True)
            trace = (result.stdout + result.stderr).split(r"Evaluating \_SB.USB0.MGON", 1)[1]
            values = [int(x, 16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)", trace)]
            observed = [(int(address, 16), int(value, 16)) for value, address in
                        re.findall(r"SystemMemory Write:\s+Val ([0-9A-F]+) Addr ([0-9A-F]+) BitWidth 20", trace)]
            assert values == [expected] and observed == writes and not re.search(r"\bAE_[A-Z_]+\b", trace), (
                name, identity, expected, writes, trace)
            musb_gate_cases.append({"case": name, "clock_registers": regs, "expected_identity": identity,
                                    "result": values[0], "writes": observed})
        out=subprocess.check_output([acpiexec,"-b","evaluate \\_SB.USB0._CRS;evaluate \\_SB.USB0._CCA",str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
        assert "AE_ERROR" not in out and "AE_NOT_FOUND" not in out,out
        rows=re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
        musb_resource=bytes.fromhex("".join(rows))
        musb_regs=cells("/soc/usb0@11200000","reg")
        assert musb_regs==[0,0x11200000,0,0x10000,0,0x11f40000,0,0x10000]
        assert musb_resource[:12]==b"\x86\x09\x00\x01"+struct.pack("<II",musb_regs[1],musb_regs[3])
        assert musb_resource[12:24]==b"\x86\x09\x00\x00"+struct.pack("<II",0x43f7f100,64)
        phy_child = "/soc/usb-phy@11f40000/usb-phy@11f40000"
        phy_regs = cells(phy_child, "reg")
        assert phy_regs == [0, musb_regs[5], 0, 0x700] and phy_regs[3] < musb_regs[7]
        assert musb_resource[24:36] == b"\x86\x09\x00\x01" + struct.pack("<II", phy_regs[1], phy_regs[3])
        assert musb_resource[36:45]==b"\x89\x06\x00\x01\x01"+struct.pack("<I",irq("/soc/usb0@11200000"))
        assert b"HANA678D\0" in tables["DSDT"]
        assert r"ACPI\HANA678D" in (Path(__file__).resolve().parents[3]/"Windows/MtkUsb/MtkUsb.inf").read_text()
    # Stock matrix controller rising-edge IRQ is a single ordinary extended
    # interrupt resource. Its flags must not claim system wake. PMIC TOPSTATUS
    # remains a separate shared-broker path, with no per-key GPIO fabrication.
    keypad_node="/soc/kp@10010000"
    keypad_regs=cells(keypad_node,"reg")
    keypad_irq=cells(keypad_node,"interrupts")
    keypad_map=cells(keypad_node,"mediatek,hw-init-map")
    assert keypad_regs==[0,0x10010000,0,0x1000]
    assert keypad_irq==[0,106,1,0]  # stock GIC SPI106, rising edge
    assert len(keypad_map)==72 and keypad_map[:2]==[115,114] and not any(keypad_map[2:])
    assert cells(keypad_node,"mediatek,key-debounce-ms")==[1024]
    assert cells(keypad_node,"mediatek,use-extend-type")==[0]
    power_node="/soc/pwrap@10026000/mt6366/mt6366keys/power"
    assert cells(power_node,"linux,keycodes")==[116]
    assert raw_property(power_node,"wakeup-source")==b""
    out=subprocess.check_output([acpiexec,"-b",
        "evaluate \\_SB.KPD0._CRS;evaluate \\_SB.KPD0._STA;evaluate \\_SB.KPD0._HID",
        str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
    assert not re.search(r"\bAE_[A-Z_]+\b",out),out
    rows=re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
    inline=re.findall(r"\[Buffer\] Length [0-9A-F]+ =\s*(?:0000:\s*)?((?:[0-9A-F]{2} ?)+)",out)
    keypad_resource=bytes.fromhex("".join(rows or inline))
    keypad_gsiv=keypad_irq[1]+32
    assert keypad_gsiv==138
    # Extended interrupt flags: consumer + edge, active-high, exclusive,
    # no wake capability. Compare the complete AML resource bytes.
    expected_keypad=(b"\x86\x09\x00\x00"+struct.pack("<II",keypad_regs[1],keypad_regs[3])+
        b"\x89\x06\x00\x03\x01"+struct.pack("<I",keypad_gsiv)+b"\x79\x00")
    assert keypad_resource==expected_keypad,(keypad_resource.hex(),expected_keypad.hex())
    assert [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]==[15],out
    assert '"HANA6790"' in out,out
    out=subprocess.check_output([acpiexec,"-b",
        "evaluate \\_SB.BTN0._HID;evaluate \\_SB.BTN0._CID;evaluate \\_SB.BTN0._STA",
        str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
    assert not re.search(r"\bAE_[A-Z_]+\b",out),out
    assert '"HANA6791"' in out and '"PNP0C40"' in out,out
    assert [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]==[15],out
    # Cover wake is described by the wake-capable GPIO resource in GPO0._AEI.
    absent=("BTN0._CRS","BTN0._PRW","KPD0._PRW","LID0._PRW")
    for name in absent:
        out=subprocess.check_output([acpiexec,"-b","evaluate \\_SB."+name,
            str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
        assert "AE_NOT_FOUND" in out and "[Buffer]" not in out and "[Package]" not in out,(name,out)
        assert not set(re.findall(r"\bAE_[A-Z_]+\b",out))-{"AE_NOT_FOUND"},out
    physical_buttons={
        "provider":"\\_SB.KPD0","provider_HID":"HANA6790",
        "inbox":"\\_SB.BTN0","inbox_HID":"HANA6791","inbox_CID":"PNP0C40",
        "stock_keypad_node":keypad_node,"registers":keypad_regs,"stock_gsiv":keypad_gsiv,
        "IRQ_resource_assigned":True,"IRQ_wake_capability_flag":False,
        "debounce_register_offset":24,"debounce_low14_value":1024,"map_entries_not_register_value":72,
        "interrupt":"stock GIC SPI106, rising edge, GSIV138, exclusive ordinary interrupt; no wake flag",
        "volume_map":{"status_word0_bit0":115,"status_word0_bit1":114,"active_low":True},
        "power":{"node":power_node,"linux_keycode":116,"TOPSTATUS":0x28,
            "active_low_bit":1,"read_path":"existing shared PMIC PRM broker; no second PWRAP mapping"},
        "resource_bytes":keypad_resource.hex(),
        "inbox_GPIO_resources_created":False,"PMIC_wake_route_claimed":False,
        "absent_resources_evaluated":list(absent),"STA_is_presence_only":True,
        "Windows_or_physical_execution":False}
    # Evaluate the compiled GPU AML, and compare the complete returned resource
    # template to stock DTB and the binder's raw GSIV contract. _STA denotes
    # device presence; it never qualifies power, cache, DMA or installed drivers.
    mali_node="/soc/mali@13000000"
    mali_names=raw_property(mali_node,"interrupt-names").rstrip(b"\0").decode().split("\0")
    assert mali_names==["GPU","MMU","JOB","EVENT","PWR"]
    mali_regs=cells(mali_node,"reg");mali_gsiv=[irq(mali_node,i) for i in range(3)]
    assert mali_regs==[0,0x13000000,0,0x4000] and mali_gsiv==[428,429,430]
    out=subprocess.check_output([acpiexec,"-b","evaluate \\_SB.GPU0._CRS;evaluate \\_SB.GPU0._CCA;evaluate \\_SB.GPU0._STA",str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
    assert "AE_ERROR" not in out and "AE_NOT_FOUND" not in out,out
    rows=re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
    mali_resource=bytes.fromhex("".join(rows))
    expected_mali=b"\x86\x09\x00\x01"+struct.pack("<II",mali_regs[1],mali_regs[3])
    expected_mali+=b"".join(b"\x89\x06\x00\x01\x01"+struct.pack("<I",n) for n in mali_gsiv)+b"\x79\x00"
    assert mali_resource==expected_mali,(mali_resource.hex(),expected_mali.hex())
    assert [int(x,16) for x in re.findall(r"\[Integer\] = ([0-9A-F]+)",out)]==[0,15],out
    assert b"HANA678E\0" in tables["DSDT"]
    # Compare every broker resource to the stock tree, not the new ASL text.
    owner_nodes=[("/soc/gpufreq",1),("/soc/gpufreq",5),("/soc/gpufreq",0),
                 ("/soc/gpufreq",8),("/soc/mcusys_pll1u_top@1000c000",0)]
    owner_regs=[cells(n,"reg")[i*4:i*4+4] for n,i in owner_nodes]
    assert owner_regs==[[0,b,0,4096] for b in (0x10006000,0x10001000,0x13fbf000,0x10000000,0x1000c000)]
    pwrap_regs=cells("/soc/pwrap@10026000","reg")
    assert pwrap_regs==[0,0x10026000,0,4096]
    out=subprocess.check_output([acpiexec,"-b","evaluate \\_SB.MFG0._CRS;evaluate \\_SB.MFG0._STA;evaluate \\_SB.GPU0._DEP",str(paths["DSDT"])],text=True,stderr=subprocess.STDOUT)
    assert "AE_ERROR" not in out and "AE_NOT_FOUND" not in out,out
    rows=re.findall(r"^\s*[0-9A-F]{4}: ((?:[0-9A-F]{2} )+)",out,re.M)
    owner_resource=bytes.fromhex("".join(rows))
    assert owner_resource==b"".join(b"\x86\x09\x00\x01"+struct.pack("<II",r[1],r[3]) for r in owner_regs)+b"\x79\x00"
    assert re.findall(r"\[Integer\] = ([0-9A-F]+)",out)==["000000000000000F"],out
    assert "[Package] Contains 1 Elements" in out and re.search(r"Name MFG0 Device",out),out
    assert b"HANA678F\0" in tables["DSDT"]
    inf=(Path(__file__).resolve().parents[3]/"Windows/MtkMali/MtkMaliSoc.inf").read_text()
    assert r"ACPI\HANA678F" in inf and "AddService=MtkMaliBoard,2,Service" in inf
    gpio_regs = cells("/soc/pinctrl","reg")
    eint_regs = cells("/soc/apirq@1000b000","reg")
    for reg in [gpio_regs[i:i+4] for i in range(0,len(gpio_regs),4)]+[eint_regs]:
        assert reg[0] == reg[2] == 0
        assert b"\x86\x09\x00\x01"+struct.pack("<II",reg[1],reg[3]) in tables["DSDT"]
    gpio_gsiv = irq("/soc/apirq@1000b000")
    assert cells("/soc/pinctrl","gpio-ranges")[1:] == [0,0,188]
    assert tables["DSDT"].count(b"\x89\x06\x00\x09\x01"+struct.pack("<I",gpio_gsiv)) == 6
    resources = []
    for node in ("/soc/i2c@11e00000", "/soc/i2c@1101a000", "/soc/i2c@11e01000"):
        reg = cells(node,"reg")
        assert reg[0] == reg[2] == reg[4] == reg[6] == 0
        for address, length in ((reg[1],reg[3]),(reg[5],reg[7])):
            # Memory32Fixed, read/write, exactly the DTB controller/APDMA tuple.
            assert b"\x86\x09\x00\x01"+struct.pack("<II",address,length) in tables["DSDT"]
        gsiv = irq(node)
        assert b"\x89\x06\x00\x01\x01"+struct.pack("<I",gsiv) in tables["DSDT"]
        resources.append({"node":node,"registers":reg,"gsiv":gsiv})
    print(json.dumps({
        "status": "pass", "cpus": len(affinities),
        "mpidr": [hex(x) for x in affinities], "cluster_uids": topology,
        "cpu_efficiency_classes": [data[76] for data in gicc],
        "stock_cpu_performance_domains": {
            "controller": performance_controller,
            "by_uid": [spec[1] for spec in performance_domains],
            "registers": [
                {"name": name.decode(), "base": hex(performance_registers[i*4+1]),
                 "size": performance_registers[i*4+3]} for i, name in enumerate(performance_names)
            ]},
        "cpu_frequency_acpi": {"hid": "HANA6799", "resources": [
            {"writable": bool(rw), "base": hex(base), "size": size}
            for rw, base, size in cpu_freq_resources]},
        "connectivity_acpi": connectivity_acpi,
        "timer_gsiv": [irq("/timer", i) for i in range(4)],
        "psci": method, "dsdt_uids_evaluated": integers,
        "cpu_lpi": cpu_lpi,
        "display_readiness_cases":display_cases,
        "musb_readiness_cases":musb_cases,"musb_gate_resume_cases":musb_gate_cases,
        "musb_resource_bytes":musb_resource.hex(),
        "physical_buttons":physical_buttons,
        "mali_resources_match_dtb":{"node":mali_node,"registers":mali_regs,"irq_names":mali_names[:3],"gsiv":mali_gsiv,"unbound_irq_names":mali_names[3:],"sta_is_presence_only":True,"shared_resource_provider_installed":False,"physical_execution":False},
        "mali_resource_bytes":mali_resource.hex(),
        "mali_shared_owner":{"device":"\\_SB.MFG0","hid":"HANA678F","registers":owner_regs,"resource_bytes":owner_resource.hex(),"GPU_DEP_evaluated":True,"PWRAP_in_OS_resources":False,"PWRAP_runtime_broker_registers":pwrap_regs,"device_qualification_installed":False,"physical_execution":False},
        "sd_acpi":sd_acpi,
        "ntc_acpi": {"hid": "HANA6794", "resources": ntc_resources, "gate_script": ntc_gate_checks,
                     "soc_critical_deci_kelvin": critical, "soc_zone_sensor": "NTC0"},
        "ufs_readiness_cases":ufs_cases,"ufs_resource_bytes":ufs_resource.hex(),
        "touch_readiness_cases": touch_cases, "touch_resume_cases": touch_resume_cases,
        "touch_resource_bytes":resource.hex(),
        "charger_acpi_cases": power_cases, "speaker_i2c_resource_cases": audio_cases,
        "dtb_reader": "bounded Python stock-module parser" if dt_reader else "fdtget",
        "magnetic_cover_cases":hall_cases,"magnetic_cover_polarity_cases":hall_polarity_cases,"magnetic_cover_scope":"MH248 GPIO0; virtual GPIO/AML, no physical wake test",
        "gpio_readiness_cases": gpio_cases, "gpio_resources_match_dtb": {"gpio":gpio_regs,"eint":eint_regs,"shared_gsiv":gpio_gsiv,"logical_banks":6},
        "i2c_readiness_cases": i2c_cases, "i2c_resources_match_dtb": resources,
        "i2c_retained_readiness_cases": i2c_retained_cases,
        "i2c_wrapper_restore_cases": i2c_restore_cases,
        "i2c_retained_scope": "Actual AML with simulated memory. IRCK reads live state; IRRS restores only a dedicated wrapper gate under retained parents. No physical resume proof.",
        "tables": {s: {"path": str(paths[s]), "bytes": len(b),
                       "sha256": hashlib.sha256(b).hexdigest()}
                   for s,b in tables.items()}
    }, indent=2))

if __name__ == "__main__":
    main()
