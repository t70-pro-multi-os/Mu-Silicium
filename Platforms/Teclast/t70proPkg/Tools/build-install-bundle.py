#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Assemble the boot-only hardware-trial bundle; never accesses USB."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text())


def assemble(boot, selector_report, selector_tests, lk_evidence, kit, shell, out):
    if out.exists() or out.is_symlink():
        raise ValueError("Bundle output already exists; files preserved")
    image = read(boot.with_suffix(".json"))
    selector, tests = read(selector_report), read(selector_tests)
    lk = read(lk_evidence)
    if (lk["lk_sha256"] != selector["original_lk_sha256"] or
            lk["selection_mode"] != selector["boot_mode_tag"]["recovery_mode"] or
            not lk.get("selection_mode_uses_boot_kernel") or
            not lk.get("all_passed") or not lk.get("requires_valid_AB_boot_control") or
            not lk.get("original_LK_decision_and_suffix_getter_executed") or
            lk["evidence_report_sha256"] != sha(lk_evidence.with_suffix(".md"))):
        raise ValueError("Exact original-LK evidence for the selected mode using the boot kernel is required")
    if selector["boot_mode_tag"]["uefi_mode"] == 99 and not any(
            case["lk_boot_mode"] == 99 and case["load_vfy_boot_argument"] == 0
            for case in lk.get("cases", [])):
        raise ValueError("Exact original-LK evidence for continuing from Fastboot is required")
    factory_mode = selector["boot_mode_tag"].get("factory_mode")
    if factory_mode is not None and not any(
            case["lk_boot_mode"] == factory_mode and case["load_vfy_boot_argument"] == 0
            for case in lk.get("cases", [])):
        raise ValueError("Exact original-LK evidence for the factory mode is required")
    if (image.get("bytes") != 67108864 or image.get("sha256") != sha(boot) or
            not image.get("avb_footer_verified") or image.get("oem_signed") or
            not image.get("requires_unlocked_bootloader")):
        raise ValueError("Expected a verified, unsigned full boot image")
    if not selector.get("selection_transport") or not selector.get("stock_kernel_preserved"):
        raise ValueError("A usable selection transport and original Android kernel are required")
    if (not tests.get("compiled_selector_executed") or not tests.get("all_passed") or
            not tests.get("stock_kernel_copy_matches_original_sha256")):
        raise ValueError("Compiled selector and original-kernel checks are required")
    if (image["kernel_image_sha256"] != selector["image_sha256"] or
            tests["tested_artifact_sha256"] != selector["image_sha256"]):
        raise ValueError("The packaged image, selector and execution report do not match")
    spec = importlib.util.spec_from_file_location("efi_media", HERE/"prepare-uefi-media.py")
    media = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(media)
    media.check_efi(shell.read_bytes())
    plan = read(kit/"plan.json")
    if (image["stock_boot_sha256"] != plan["partitions"]["boot_a"]["sha256"] or
            selector["stock_boot_sha256"] != plan["partitions"]["boot_a"]["sha256"]):
        raise ValueError("The packaged stock baseline differs from the recovery original")
    installer_checks = read(selector_tests.parent/"boot-installer-tests.json")
    avb_checks = read(selector_tests.parent/"boot-package-tests.json")
    lz4_checks = read(selector_tests.parent/"lk-lz4-tests.json")
    if (not installer_checks["regular_file_write_and_readback_verified"] or
            installer_checks["installer_source_sha256"] != sha(HERE/"boot-install.py") or
            avb_checks["image_sha256"] != image["sha256"]):
        raise ValueError("Installer or AVB checks do not match the final image")
    if (not lz4_checks.get("all_passed") or
            lz4_checks["tested_artifact_sha256"] != selector["image_sha256"] or
            lz4_checks["lk_sha256"] != selector["original_lk_sha256"] or
            not lz4_checks.get("actual_lk_memory_copy_executed")):
        raise ValueError("Original-LK LZ4 execution must match the final capsule")
    props = {p["key"]: p["value"] for p in image["metadata"]["descriptors"] if p["tag"] == 0}
    fingerprint = props["com.android.build.boot.fingerprint"]
    out.mkdir(parents=True)
    shutil.copyfile(boot, out/"boot.img")
    if sha(out/"boot.img") != image["sha256"]:
        raise ValueError("Boot image changed while assembling the bundle")
    shutil.copyfile(HERE/"boot-install.py", out/"install.py")
    shutil.copyfile(boot.with_suffix(".json"), out/"boot-metadata.json")
    shutil.copyfile(selector_report, out/"selector.json")
    shutil.copyfile(selector_tests, out/"selector-tests.json")
    checks = out/"checks"
    checks.mkdir()
    for name in ("boot-installer-tests.json", "boot-package-tests.json", "avb-chain.json",
                 "lk-lz4-tests.json", "bundle-safety-tests.json"):
        shutil.copyfile(selector_tests.parent/name, checks/name)
    shutil.copyfile(lk_evidence, checks/"lk-evidence.json")
    shutil.copyfile(lk_evidence.with_suffix(".md"), checks/"lk-evidence.md")
    efi = out/"esp/EFI/BOOT/BOOTAA64.EFI"
    efi.parent.mkdir(parents=True)
    shutil.copyfile(shell, efi)
    launcher = out/"install.sh"
    launcher.write_text('#!/bin/sh\nset -eu\ncd -- "$(dirname -- "$0")"\n'
                        'if [ "$#" -eq 0 ]; then set -- plan; fi\n'
                        'exec /opt/homebrew/bin/python3 -B install.py "$@"\n')
    launcher.chmod(0o755)
    target = {k: plan["partitions"]["boot_a"][k] for k in ("name", "parttype", "offset_bytes", "size_bytes")}
    manifest = {
        "schema": 1, "model": "Teclast T70 Pro G100 / MT6789 G3D1",
        "installation_class": "hardware_trial", "preparation_complete": True,
        "ready_for_hardware_trial": True, "production_installation_verified": False,
        "hardware_boot_verified": False, "android_return_verified": False,
        "target_os": None, "contains_os": False, "contains_efi_shell": True,
        "selection_transport": selector["selection_transport"],
        "selected_lk_mode": selector["boot_mode_tag"]["uefi_mode"],
        "selection_instructions": selector["selection_instructions"],
        "target": target, "active_slot_required": "_a", "payload": "boot.img",
        "original_boot_sha256": plan["partitions"]["boot_a"]["sha256"],
        "android_fingerprint": fingerprint,
        "guard_sha256": {n: plan["partitions"][n]["sha256"] for n in ("lk_a", "init_boot_a", "vendor_boot_a", "dtbo_a", "vbmeta_a")},
        "recovery_files": {n: sha(kit/n) for n in ("restore.py", "plan.json", "source-chunks.json", "kit-files.json")},
        "original_source_duplicated": False, "device_access_during_preparation": False,
        "remaining_hardware_checks": [
            "manufacturer DA boot_a write and exact readback",
            "stock LK accepts the unsigned modified boot image while unlocked",
            "larger compressed capsule is loaded and decompressed within the reserved kernel range",
            "physical Recovery Mode starts stock Android Recovery",
            "default Android boot and explicit UEFI selection",
            "physical display/input/storage and target-OS drivers",
            "original boot restoration and Android daily-use checks"],
        "offline_checks": {"compiled_selector": tests["count"],
                           "regular_file_install_restore": installer_checks["count"],
                           "avb_packaging": avb_checks["count"],
                           "original_LK_AB_entry": lk["count"],
                           "original_LK_lz4": lz4_checks["count"]},
        "source_sha256": sha(Path(__file__)),
    }
    note = out/"설치안내.txt"
    note.write_text(
        "T70 Pro G100 — Android 보존 UEFI 시험 설치\n\n"
        "이 묶음에는 공통 UEFI와 ARM64 EFI Shell이 들어 있습니다. 새 OS는 포함하지 않습니다.\n"
        "실기기 첫 부팅·쓰기·원복은 미확인입니다. 시험 설치 결과와 완성된 OS 설치는 구분합니다.\n\n"
        "변경 대상: 현재 A 슬롯의 boot_a 한 곳(64 MiB).\n"
        "현재 슬롯·언락·순정 버전·기기·GPT·LK/init_boot/vendor_boot/dtbo/vbmeta가 기준과 다르면 중지합니다.\n"
        "misc의 A/B 정보·검사값·A 우선도·정상 부팅 표시와 비어 있는 부팅 명령도 확인합니다.\n"
        "LK, init_boot, vbmeta, 슬롯, GPT, userdata, 모뎀 고유 영역은 그대로 둡니다.\n"
        "복구 원본과 제조사 DA는 기존 recovery-kit을 사용하며 이 묶음에 중복 복사하지 않았습니다.\n\n"
        "Mac 터미널에서 이 폴더로 이동한 뒤 실행합니다.\n"
        "  ./install.sh plan\n"
        "  ./install.sh doctor\n"
        "계획과 모듈 검사에는 USB 접근이 없습니다.\n\n"
        "설치: Android 정상 부팅·USB 디버깅 허용 상태에서\n"
        "  ./install.sh install --execute --reboot\n"
        "연결 대기 문구가 나오면 태블릿 전원을 끄고 USB를 연결합니다.\n"
        "기기에 쓴 각 블록을 다시 읽고 마지막에 boot 전체 해시를 검사합니다.\n"
        "오류가 나면 같은 연결에서 순정 boot 자동 복구를 시도합니다. 연결이 끊겼으면 아래 명령을 사용합니다.\n\n"
        "복구: Android가 부팅하지 않아도 실행할 수 있습니다.\n"
        "  ./install.sh restore --execute --reboot\n"
        "전원을 끈 태블릿을 USB에 연결합니다. 제조사 preloader가 남아 있어야 합니다.\n"
        "이 복구는 boot_a만 원본으로 되씁니다. userdata나 잠금 상태는 바꾸지 않습니다.\n\n"
        "부팅 선택: " + selector["selection_instructions"] + "\n"
        "기동 정보가 맞지 않거나 지원하지 않는 진단 모드에서는 정지합니다.\n"
        "부팅 전환용으로 misc·슬롯을 쓰거나 adb reboot recovery를 사용하지 않습니다.\n\n"
        "외장 매체: FAT32 EFI System Partition에 esp/ 안의 EFI 폴더를 복사합니다.\n"
        "현재 BOOTAA64.EFI는 EFI Shell이며 Linux·Windows 설치 프로그램이 아닙니다.\n"
        "OS 종류·드라이버를 확인하기 전에는 내장 저장장치 초기화나 파티션 변경을 하지 않습니다.\n\n"
        "첫 시험은 기본 Android 부팅, UEFI 진입, 원본 boot 복구 순으로 확인합니다.\n"
        "복구 후 화면·터치·파일/앱·Wi-Fi·SIM 인식·충전 상태를 확인해야 시험이 끝납니다.\n"
        "야간 시험이라면 취침 전 복구 시작 시각과 기상 전 완료 시각을 먼저 정합니다.\n"
        "자동 재시작 요청만으로 Android 정상 부팅·무인 복구가 입증되지는 않습니다.\n"
        "리락은 실행하지 마세요. OEM 서명 이미지가 아니며 수정된 boot가 남은 상태의 리락은 지원하지 않습니다.\n"
    )
    manifest["files"] = {str(p.relative_to(out)): {"bytes": p.stat().st_size, "sha256": sha(p)}
                         for p in sorted(out.rglob("*")) if p.is_file()}
    manifest_path = out/"manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+"\n")
    (out/"SHA256SUMS").write_text("".join(sha(p)+"  "+str(p.relative_to(out))+"\n"
                                         for p in sorted(out.rglob("*")) if p.is_file()))
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("boot", "selector-report", "selector-tests", "lk-evidence", "kit", "shell", "out"):
        p.add_argument("--"+name, type=Path, required=True)
    a = p.parse_args()
    result = assemble(a.boot, a.selector_report, a.selector_tests, a.lk_evidence, a.kit, a.shell, a.out)
    print(json.dumps({k: result[k] for k in ("preparation_complete", "ready_for_hardware_trial", "hardware_boot_verified", "target_os")}))


if __name__ == "__main__":
    main()
