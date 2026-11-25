#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Describe the actual stock Wi-Fi and Bluetooth drivers without loading them."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess

HERE = Path(__file__).resolve().parent
FDT = runpy.run_path(str(HERE.parent / "platform-modules.py"))
DT = {"wlan": "/soc/wifi@18000000", "btif": "/soc/btif@1100c000",
      "voice": "/mtk-btcvsd-snd@18050000"}
MODULES = {
    "wlan_drv_gen4m_6789": "50e3b842dfb944e7d34fe786afa96dc091f8ab0d719159d8d30bf17476a452c3",
    "bt_drv_connac1x": "f64d70ae7dd1351d7370683fb69f76860db0bb81abe431aca3804b744b86b73c",
    "wmt_drv": "d395f5249bdeb2b5f6134f70c64e156bd682ba895a6da2d4c2544836a27b91e3",
    "btif_drv": "1570949e96c7d9495be844a05f50925088b60bfbd9054d40867f7c4885233bbc",
}


# Stock kalGetFwFlavorByPlat reads WMTCHIN_ADIE, not the AP/SoC ID.
ADIE_FLAVORS = {0x6631: "1d", 0x6635: "1e"}
SOC22_FIRMWARE = {
    "1d": {
        "wlan-executable": "2654c3c4ffd7f32fc46a96a85a229d06d4a13210e8056980ba3576f3f63fed7f",
        "bt-rom-patch": "5168bdb4c9c3421015a4455dec55c95c702e527c4015c7f43735cc498d94072f",
        "mcu-rom-patch": "2e735583762086d5bc91eefe048419e624ec6b3fcfa55c4bf75250a9ca80982f",
        "wifi-rom-patch": "8f1ca05e8f00465d06939886554ea83b03e88c3296453fd5b3bc9b08dbdcd233",
    },
    "1e": {
        "wlan-executable": "2bd73bd2c2bb338c5238074b848f045644fa3929859ecbde53b8066febe53848",
        "bt-rom-patch": "4372d89d31e60c9a069b0dd22c4959ab73a31b8113eb6484a1a2802f134c4624",
        "mcu-rom-patch": "4c1a4b1fc1a8650d72c2d4f9ac53159e48d67d65bad8bcafdb4d0c7d357e82c0",
        "wifi-rom-patch": "cd0c46df3bfd5b6d07aa4d741859170fdb2a19c017e6ad0248ec7e082bc739c8",
    },
}


def require(ok, why):
    if not ok:
        raise ValueError(why)


def verify_firmware(directory, adie=None):
    groups = {}
    for flavor, files in SOC22_FIRMWARE.items():
        groups[flavor] = {}
        for role, digest in files.items():
            if role == "wlan-executable":
                name = f"WIFI_RAM_CODE_soc2_2_{flavor}_1.bin"
            else:
                target = role.removesuffix("-rom-patch")
                name = f"soc2_2_ram_{target}_{flavor}_1_hdr.bin"
            path = directory / name
            require(hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                    f"not the exact stock {flavor} {role}: {name}")
            groups[flavor][role] = {"name": name, "sha256": digest}
    require(adie is None or adie in ADIE_FLAVORS,
            "unknown connectivity A-die ID: no stock firmware family selected")
    return {"available_flavors": groups,
            "selected_flavor": ADIE_FLAVORS.get(adie)}


def verify(dtb, moddir, firmware=None, adie=None):
    require(adie is None or firmware is not None, "--adie-id requires --stock-firmware-dir")
    tree, _ = FDT["tree"](dtb.read_bytes())
    dt = {key: tree[value]["properties"] for key, value in DT.items()}
    cells = FDT["cells"]
    require(dt["wlan"]["compatible"] == b"mediatek,wifi\0"
            and cells(dt["wlan"]["reg"]) == [0, 0x18000000, 0, 0x100000]
            and cells(dt["wlan"]["interrupts"]) == [0, 409, 4, 0]
            and cells(dt["wlan"]["emi-addr"]) == [0]
            and cells(dt["wlan"]["emi-size"]) == [0x600000],
            "unexpected Wi-Fi AXI or EMI description")
    require(dt["btif"]["compatible"] == b"mediatek,btif\0"
            and cells(dt["btif"]["reg"]) == [0, 0x1100c000, 0, 0x1000,
                                             0, 0x10217b00, 0, 0x80, 0, 0x10217b80, 0, 0x80]
            and cells(dt["btif"]["interrupts"]) == [0, 190, 4, 0, 0, 184, 4, 0, 0, 185, 4, 0],
            "unexpected BTIF registers or IRQs")
    infra = tree["/syscon@10001000"]["properties"]
    require(cells(dt["btif"]["clocks"]) == [*cells(infra["phandle"]), 18, *cells(infra["phandle"]), 61]
            and dt["btif"]["clock-names"] == b"btifc\0apdmac\0",
            "unexpected BTIF clock references")
    require(dt["voice"]["compatible"] == b"mediatek,mtk-btcvsd-snd\0"
            and cells(dt["voice"]["reg"])[1] == 0x18050000, "unexpected BT voice PCM node")

    metadata = {}
    for name, digest in MODULES.items():
        path = moddir / (name + ".ko")
        require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, "not the stock " + name)
        syms = subprocess.check_output(["readelf", "-Ws", path], text=True)
        imports, definitions = set(), set()
        for line in syms.splitlines():
            row = line.split()
            if len(row) == 8 and row[0].endswith(":"):
                (imports if row[6] == "UND" else definitions).add(row[7])
        metadata[name] = (imports, definitions)
    w, wdef = metadata["wlan_drv_gen4m_6789"]
    require({"mtk_axi_driver", "mtk_axi_probe", "mtk_axi_of_ids"} <= wdef
            and {"__platform_driver_register", "mtk_wcn_wmt_wlan_reg",
                 "cfg80211_register_netdevice"} <= w
            and not {"__sdio_register_driver", "__pci_register_driver"} & w,
            "stock Wi-Fi platform driver transport is not confirmed")
    require({"mt66xx_driver_data_soc2_1x1", "mt66xx_chip_info_soc2_1x1",
             "soc2_1x1ConstructFirmwarePrio", "apucSoc2_1x1FwName",
             "kalGetFwFlavorByPlat"} <= wdef
            and "request_firmware" in w
            and b"WIFI_RAM_CODE_soc2_2\0" in (moddir / "wlan_drv_gen4m_6789.ko").read_bytes(),
            "stock Wi-Fi chip implementation/firmware family is not confirmed")
    bt, _ = metadata["bt_drv_connac1x"]
    wmt, _ = metadata["wmt_drv"]
    _, btif = metadata["btif_drv"]
    require({"mtk_wcn_stp_send_data", "mtk_wcn_stp_receive_data", "mtk_wcn_wmt_func_on"} <= bt,
            "Bluetooth STP transport not confirmed")
    require({"cdev_init", "cdev_add", "mtk_wcn_stp_set_bluez"} <= bt
            and "hci_register_dev" not in bt,
            "unexpected stock Bluetooth interface type")
    require({"mtk_wcn_btif_open", "mtk_wcn_btif_write", "mtk_wcn_btif_rx_cb_register"} <= (wmt & btif),
            "WMT-to-BTIF backing edges not confirmed")
    result = {
        "wifi": {"node": DT["wlan"], "transport": "AXI platform", "irq_spi": 409,
                 "depends_on_vendor_wmt_and_cfg80211": True,
                 "chip_implementation": "SOC2_1X1",
                 "firmware_family": "WIFI_RAM_CODE_soc2_2"},
        "bluetooth": {"driver_transport": "vendor WMT/STP",
                      "stock_interface": "character device, not kernel HCI",
                      "wmt_has_btif_backend": True, "btif_node": DT["btif"],
                      "wmt_also_compiles_sdio_backend": "__sdio_register_driver" in wmt,
                      "bluetooth_voice_pcm_is_separate": True},
        "scope": "stock source/ELF/DT contract, not generic driver binding or active RF mode",
    }

    if firmware is not None:
        result["firmware"] = verify_firmware(firmware, adie)
    return result


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("stock_dtb", type=Path)
    p.add_argument("stock_module_dir", type=Path)
    p.add_argument("--stock-firmware-dir", type=Path)
    p.add_argument("--adie-id", type=lambda value: int(value, 0),
                   help="optional already observed WMT connectivity A-die ID (e.g. 0x6631)")
    a = p.parse_args()
    try:
        print(json.dumps(verify(a.stock_dtb, a.stock_module_dir, a.stock_firmware_dir, a.adie_id), indent=2))
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        p.exit(2, f"connectivity audit refused: {error}\n")
