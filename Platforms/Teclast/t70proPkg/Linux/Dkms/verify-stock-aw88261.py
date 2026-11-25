#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Cross-check the exact T70 amplifier profile and DT against Linux v7.0 selection."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ACF_DIGEST = "4bb530416e53d20bdb4e42fd63a77e04c7d1aa26ced0c24404752bf967385bd5"
PROVIDER = "/soc/i2c@11e01000"
ACF_NAME = "aw88261_acf.bin"


def require(ok, message):
    if not ok:
        raise ValueError(message)


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = crc >> 1 ^ (0x8C if crc & 1 else 0)
    return crc


def verify(dtb, acf):
    require(len(acf) == 3856 and hashlib.sha256(acf).hexdigest() == ACF_DIGEST,
            "ACF is not the verified T70 stock file")
    require(struct.unpack_from("<I", acf)[0] == 0x0A15F908
            and struct.unpack_from("<4I", acf, 52) == (768, 12, 80, 1), "ACF header")
    read = runpy.run_path(str(HERE.parent / "platform-modules.py"))
    nodes, _ = read["tree"](dtb)
    cells = read["cells"]
    links = {}
    amp_handles = []
    for idx, address in enumerate((0x34, 0x35)):
        p = nodes[f"{PROVIDER}/aw882xx@{address:x}"]["properties"]
        require(p["compatible"] == b"awinic,aw882xx_smartpa\0"
                and p["status"] == b"okay\0" and cells(p["reg"]) == [address]
                and cells(p["#sound-dai-cells"]) == [0]
                and cells(p["sound-channel"]) == [idx], "unexpected T70 amp DT")
        links[idx] = address
        handle, = cells(p["phandle"])
        require(handle > 0 and handle < 0xffffffff
                and sum(n["properties"].get("phandle") == p["phandle"]
                        for n in nodes.values()) == 1, "invalid stock amp phandle")
        amp_handles.append(handle)

    bus = nodes[PROVIDER]["properties"]
    card = nodes["/sound"]["properties"]
    speaker = nodes["/sound/mediatek,speaker-codec"]["properties"]
    require(cells(speaker["sound-dai"]) == amp_handles,
            "stock speaker DAI does not resolve to both amplifier nodes")
    afe = nodes["/soc/mt6789-afe-pcm@11210000"]["properties"]
    require(bus["compatible"] == b"mediatek,mt6983-i2c\0"
            and cells(bus["reg"]) == [0, 0x11e01000, 0, 0x1000, 0, 0x10217100, 0, 0x80]
            and cells(bus["interrupts"]) == [0, 145, 4, 0],
            "unexpected stock amplifier bus")
    require(card["compatible"] == b"mediatek,mt6789-mt6366-sound\0"
            and card["mediatek,platform"] == afe["phandle"]
            and cells(card["mediatek,spk-i2s"]) == [3, 0]
            and cells(card["mtk_spk_i2s_out"]) == [3]
            and cells(card["mtk_spk_i2s_in"]) == [0]
            and afe["compatible"] == b"mediatek,mt6789-sound\0"
            and cells(afe["reg"]) == [0, 0x11210000, 0, 0x2000]
            and cells(afe["interrupts"]) == [0, 234, 4, 0],
            "unexpected stock speaker DAI or AFE route")

    pmic = "/soc/pwrap@10026000/mt6366"
    analog = nodes[pmic + "/mt6358codec"]["properties"]
    vaud = nodes[pmic + "/mt6358regulator/ldo-vaud28"]["properties"]
    require(analog["compatible"] == b"mediatek,mt6366-sound\0"
            and analog["reg-vaud28-supply"] == vaud["phandle"]
            and cells(vaud["regulator-min-microvolt"]) == [2800000]
            and cells(vaud["regulator-max-microvolt"]) == [2800000],
            "unexpected stock MT6366 analog codec supply")

    profile = {}
    start = 80 + 12 * 64
    for i in range(12):
        offset = 80 + i * 64
        entry_type, = struct.unpack_from("<I", acf, offset)
        chan, bus, addr, prof, typ, length, off, crc = struct.unpack_from(
            "<4H4I", acf, offset + 20)
        require(entry_type == 2 and bus == addr == 0xffff and chan < 4
                and acf[offset+4:offset+20].rstrip(b"\0") == b"aw88261",
                "unexpected ACF selector")
        require(off == start and off + length <= len(acf)
                and crc8(acf[off:off+length]) == crc, "ACF payload bounds/CRC")
        start = off + length
        # Linux version field 1 uses the 64-byte legacy descriptor and selects
        # default entries by channel after excluding monitor sections.
        if typ != 10 and chan in links:
            require(typ == 4, "unexpected non-register scene")
            profile.setdefault(chan, []).append(prof)
    require(start == len(acf) and profile == {0: [0, 10], 1: [0, 10]},
            "unexpected default scene mapping")
    return {"stock_acf_sha256": ACF_DIGEST, "descriptor_bytes": 64,
            "linux_v7_default_register_profiles": profile,
            "stock_dt_channel_address": {idx: hex(addr) for idx, addr in links.items()},
            "linux_dt_requires": {"compatible": "awinic,aw88261",
                                  "channel_property": "awinic,audio-channel"},
            "linux_v7_requested_acf_name": ACF_NAME,
            "stock_card": {"speaker_output_i2s": 3, "speaker_input_i2s": 0,
                           "afe_mmio": "0x11210000", "afe_gic_spi": 234,
                           "codec_bus_mmio": "0x11e01000", "codec_bus_gic_spi": 145},
            "separate_analog_codec": {"compatible": "mediatek,mt6366-sound",
                                      "stock_supply_name": "reg-vaud28-supply",
                                      "voltage_uv": 2800000},
            "scope": "offline envelope and selector check; not ASoC binding or playback"}


def verify_staging_overlay(stock, include_providers=False, include_card=False):
    include_providers = include_providers or include_card
    read = runpy.run_path(str(HERE.parent / "platform-modules.py"))
    def inventory(path):
        nodes, _ = read["tree"](path.read_bytes())
        return {name: node["properties"] for name, node in nodes.items()}

    expected = inventory(stock)
    for idx, address in enumerate((0x34, 0x35)):
        node = f"{PROVIDER}/aw882xx@{address:x}"
        expected[node] = {**expected[node], "compatible": b"awinic,aw88261\0",
                          "awinic,audio-channel": struct.pack(">I", idx),
                          "sound-name-prefix": f"T70 Amp {idx}\0".encode("ascii"),
                          "status": b"disabled\0"}
    overlays = ["Audio/t70pro-aw88261-staging.dtso"]
    if include_providers:
        bus = expected[PROVIDER]
        main = expected["/syscon@11E02000"]
        dma = expected["/syscon@10001000"]
        for node, compatible in ((main, b"mediatek,mt6789-imp_iic_wrap_w"),
                                 (dma, b"mediatek,mt6789-infracfg_ao")):
            phandle, = read["cells"](node["phandle"])
            require(node["compatible"].split(b"\0")[0] == compatible
                    and read["cells"](node["#clock-cells"]) == [1]
                    and 0 < phandle < 0xffffffff
                    and sum(p.get("phandle") == node["phandle"] for p in expected.values()) == 1,
                    "unexpected stock audio clock provider")
        require(bus["clocks"] == main["phandle"] + struct.pack(">I", 1)
                + dma["phandle"] + struct.pack(">I", 0x3d)
                and bus["clock-names"] == b"main\0dma\0"
                and read["cells"](bus["clock-frequency"]) == [400000]
                and read["cells"](bus["clock-div"]) == [1],
                "unexpected stock amplifier I2C clocks")
        for node, compatible in (
                (PROVIDER, b"teclast,t70pro-mt6983-i2c\0"),
                ("/soc/mt6789-afe-pcm@11210000", b"teclast,t70pro-mt6789-afe\0")):
            expected[node] = {**expected[node], "compatible": compatible, "status": b"disabled\0"}
        overlays.append("Audio/t70pro-audio-providers-staging.dtso")
    if include_card:
        labels = expected["/__symbols__"]
        paths = {
            "afe": "/soc/mt6789-afe-pcm@11210000",
            "aw882xx_smartpa_0": PROVIDER + "/aw882xx@34",
            "aw882xx_smartpa_1": PROVIDER + "/aw882xx@35",
        }
        for label, path in paths.items():
            require(labels[label] == (path + "\0").encode("ascii"),
                    f"stock speaker symbol does not match {path}")
        handle = {label: expected[path]["phandle"] for label, path in paths.items()}
        name = "/teclast-speaker-card"
        require(name not in expected, "stock already contains a T70 staging speaker node")
        expected[name] = {
            "compatible": b"teclast,t70pro-aw88261-speaker\0",
            "teclast,afe": handle["afe"],
            "teclast,amps": handle["aw882xx_smartpa_0"] + handle["aw882xx_smartpa_1"],
            "status": b"disabled\0",
        }
        overlays.append("Audio/t70pro-speaker-card-staging.dtso")
    with tempfile.TemporaryDirectory(prefix="t70-audio-staging-") as tempdir:
        temp = Path(tempdir)
        result = temp / "staged.dtb"
        files = []
        for source in overlays:
            overlay = temp / Path(source).with_suffix(".dtbo").name
            subprocess.run(["dtc", "-@", "-I", "dts", "-O", "dtb", "-o", overlay,
                            HERE / source], check=True)
            files.append(overlay)
        subprocess.run(["fdtoverlay", "-i", stock, "-o", result, *files], check=True)
        require(inventory(result) == expected,
                "overlay changed resources beyond the selected disabled audio nodes")



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_dtb", type=Path)
    parser.add_argument("stock_acf", type=Path)
    parser.add_argument("--check-staging", action="store_true")
    parser.add_argument("--check-providers", action="store_true")
    parser.add_argument("--check-card", action="store_true")
    args = parser.parse_args()
    try:
        report = verify(args.stock_dtb.read_bytes(), args.stock_acf.read_bytes())
        if args.check_staging or args.check_providers or args.check_card:
            verify_staging_overlay(args.stock_dtb, args.check_providers, args.check_card)
            report["staging_overlay"] = (
                "disabled card with exact AFE/amp phandles plus four disabled providers"
                if args.check_card else
                "disabled I2C1, AFE and two amps; resources unchanged"
                if args.check_providers else "exactly two expected disabled amp declarations")
        print(json.dumps(report, indent=2))
    except (OSError, KeyError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(2, f"AW88261 verification refused: {error}\n")
