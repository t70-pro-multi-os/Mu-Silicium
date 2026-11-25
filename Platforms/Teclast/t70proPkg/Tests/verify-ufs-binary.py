#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare shipped UFS UniPro and power-callback instructions with the production C core.

ufs_mtk_link_startup_notify is executed. Its external DME calls are
replaced with a controlled attribute store, and its two kernel canary reads
use a fixture. Optional factory TEE checks execute only reset and unsupported
UFS handler branches, with modeled MMIO and return context; they never execute
key-management commands or the EL3 dispatcher. Suspend/resume execute only the
selected module callbacks; generic Hibern8 UIC and platform rail operations
remain mocked or outside the module. No module is loaded and no physical
device address is accessible.
"""
from pathlib import Path
import argparse
import ctypes as C
import hashlib
import json
import random
import struct
import subprocess
import sys
import tempfile
from unicorn import (Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE,
    UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE)
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
    UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6,
    UC_ARM64_REG_X7, UC_ARM64_REG_X8, UC_ARM64_REG_SP,
    UC_ARM64_REG_LR, UC_ARM64_REG_PC)

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
BINARY = REPO.parent / "analysis/stock-binaries/vendor_boot_a_0/ufs-mediatek-mod.ko"
EXPECTED_SHA = "5e34b9ff94bd5b032a24ee50ad3c3564b79571b4dccdbc257558fb9e3fb90952"
FUNCTION = "ufs_mtk_link_startup_notify"
CORE = REPO / "Silicon/MediaTek/MediaTekPkg/Library/UfsCoreLib/MtkUfsPlatform.c"
ATTRIBUTES = (0xD0A8, 0x155E, 0xD0A6, 0xD09E, 0xD0A1)

# These are .text-relative ELF symbol locations and CALL26 relocation sites
# from the hash-pinned stock module. The runner fails closed if the artifact's
# callback or any external branch moves or changes its target.
EXPECTED_CALLBACK_FUNCTIONS = (
    {"label": "ufs_mtk_suspend", "symbol": "ufs_mtk_suspend", "offset": 0x3B20, "size": 0x214,
     "calls": {0x3BA0: "ufshcd_dme_set_attr", 0x3BB0: "ufs_mtk_eh_unipro_set_lpm",
               0x3BCC: "ufshcd_dme_set_attr", 0x3BE0: "ufs_mtk_eh_unipro_set_lpm",
               0x3BFC: "ufs_mtk_auto_hibern8_disable", 0x3C50: "__arm_smccc_smc",
               0x3CB0: "cpu_latency_qos_update_request", 0x3D24: "__arm_smccc_smc",
               0x3D2C: "__stack_chk_fail"}},
    {"label": "ufs_mtk_resume", "symbol": "ufs_mtk_resume", "offset": 0x3D38, "size": 0x4B0,
     "calls": {0x3DDC: "__arm_smccc_smc", 0x3E00: "regulator_set_mode",
               0x3E30: "__arm_smccc_smc", 0x3E48: "ufshcd_link_recovery",
               0x3ECC: "cpu_latency_qos_update_request", 0x3EF0: "ufshcd_hba_enable",
               0x3F10: "ufshcd_dme_set_attr", 0x3F24: "ufs_mtk_eh_unipro_set_lpm",
               0x3FD4: "_dev_err", 0x3FE0: "ufshcd_uic_hibern8_exit",
               0x4000: "ktime_get", 0x401C: "ktime_get", 0x40F4: "usleep_range_state",
               0x4110: "_dev_warn", 0x4150: "ufshcd_update_evt_hist",
               0x416C: "ufshcd_update_evt_hist", 0x4180: "ufshcd_make_hba_operational",
               0x41B0: "ufshcd_mcq_make_queues_operational", 0x41BC: "ufshcd_mcq_config_mac",
               0x41E0: "__stack_chk_fail"}},
    {"label": "ufs_mtk_mphy_power_on", "symbol": "ufs_mtk_mphy_power_on", "offset": 0x4E54, "size": 0x1A0,
     "calls": {0x4EC8: "phy_power_off", 0x4F04: "__arm_smccc_smc",
               0x4F0C: "regulator_disable", 0x4F1C: "regulator_enable",
               0x4F34: "usleep_range_state", 0x4F64: "__arm_smccc_smc",
               0x4F6C: "phy_power_on", 0x4FE0: "_dev_info", 0x4FEC: "__stack_chk_fail"}},
    {"label": "ufs_mtk_wait_idle_state", "symbol": "ufs_mtk_wait_idle_state", "offset": 0x0EEC, "size": 0x1B0,
     "calls": {0x0F18: "ktime_get_mono_fast_ns", 0x0F24: "__const_udelay",
               0x0F2C: "ktime_get_mono_fast_ns", 0x0FA0: "__const_udelay",
               0x0FC4: "__const_udelay", 0x0FD0: "ktime_get_mono_fast_ns",
               0x108C: "_dev_info"}},
    {"label": "readl_13e0", "symbol": "readl", "offset": 0x13E0, "size": 0x80,
     "calls": {0x1414: "log_read_mmio", 0x1444: "log_post_read_mmio"}},
    {"label": "writel_0e70", "symbol": "writel", "offset": 0x0E70, "size": 0x7C,
     "calls": {0x0EB0: "log_write_mmio", 0x0ED4: "log_post_write_mmio"}},
    {"label": "writel_74b4", "symbol": "writel", "offset": 0x74B4, "size": 0x7C,
     "calls": {0x74F4: "log_write_mmio", 0x7518: "log_post_write_mmio"}},
    {"label": "readl_8434", "symbol": "readl", "offset": 0x8434, "size": 0x80,
     "calls": {0x8468: "log_read_mmio", 0x8498: "log_post_read_mmio"}},
    {"label": "writel_85c8", "symbol": "writel", "offset": 0x85C8, "size": 0x7C,
     "calls": {0x8608: "log_write_mmio", 0x862C: "log_post_write_mmio"}},
)

BRIDGE = r'''
#include <Library/MtkUfsPlatform.h>
#include <string.h>
typedef struct { uint32_t Attr[65536], Trace[64 * 3], Count; } FIXTURE;
static MTK_UFS_PLATFORM_STATUS Read(void *C, MTK_UFS_MMIO_BANK B, uint32_t O, uint32_t *V) {
  (void)C; if (B != MtkUfsControllerBank || O != 0x30) return MtkUfsPlatformInvalid;
  *V = 9; return MtkUfsPlatformOk;
}
static MTK_UFS_PLATFORM_STATUS Write(void *C, MTK_UFS_MMIO_BANK B, uint32_t O, uint32_t V) {
  (void)C; (void)B; (void)O; (void)V; return MtkUfsPlatformInvalid;
}
static void Delay(void *C, uint32_t U) { (void)C; (void)U; }
static MTK_UFS_PLATFORM_STATUS Smc(void *C, const uint64_t A[8], int64_t *R) {
  (void)C; (void)A; (void)R; return MtkUfsPlatformInvalid;
}
static MTK_UFS_PLATFORM_STATUS Hibernate(void *C, int Enter) {
  (void)C; (void)Enter; return MtkUfsPlatformInvalid;
}
static MTK_UFS_PLATFORM_STATUS Dme(void *C, int G, uint16_t A, uint32_t *V) {
  FIXTURE *F = C;
  if (F->Count >= 64) return MtkUfsPlatformInvalid;
  if (G) *V = F->Attr[A]; else F->Attr[A] = *V;
  F->Trace[F->Count * 3] = (uint32_t)G;
  F->Trace[F->Count * 3 + 1] = A;
  F->Trace[F->Count * 3 + 2] = *V;
  F->Count++;
  return MtkUfsPlatformOk;
}
int Run(uint32_t Version, int Post, const uint32_t *Initial, uint32_t *Output, uint32_t *Trace) {
  const uint16_t Attr[] = { 0xD0A8, 0x155E, 0xD0A6, 0xD09E, 0xD0A1 };
  const MTK_UFS_PLATFORM_CONFIG Config = { 0x2300, 0x1000, 0, 0, 32,
    { {0x130,0x134,0x8000}, {0x140,0x144,0x80}, {0x150,0x154,0x200000} }, 0 };
  MTK_UFS_PLATFORM P;
  FIXTURE F;
  MTK_UFS_PLATFORM_IO Io = { .Context=&F, .Read32=Read, .Write32=Write,
    .DelayUs=Delay, .Smc=Smc, .Dme=Dme, .Hibernate=Hibernate };
  unsigned I;
  memset(&F, 0, sizeof(F));
  for (I = 0; I < 5; I++) F.Attr[Attr[I]] = Initial[I];
  if (MtkUfsPlatformInitialize(&P, &Io, &Config)) return -1;
  P.IpVersion = Version; P.RefClock = MtkUfsRefClockOn;
  P.State = Post ? MtkUfsPlatformLinkPrepared : MtkUfsPlatformHostEnabled;
  if (Post ? MtkUfsPlatformPostLink(&P) : MtkUfsPlatformPreLink(&P)) return -2;
  for (I = 0; I < 5; I++) Output[I] = F.Attr[Attr[I]];
  memcpy(Trace, F.Trace, F.Count * 3 * sizeof(uint32_t));
  return F.Count;
}

typedef struct {
  uint32_t Reg[0x2300 / 4], Attr[65536];
  int64_t OffResult, OnResult;
  int FailOffTransport, FailOnTransport;
  unsigned SmcCalls, SramOffCalls, SramOnCalls;
} POLICY_FIXTURE;
static MTK_UFS_PLATFORM_STATUS PolicyRead(void *C, MTK_UFS_MMIO_BANK B, uint32_t O, uint32_t *V) {
  POLICY_FIXTURE *F = C;
  if (B != MtkUfsControllerBank || (O & 3U) || O >= sizeof(F->Reg)) return MtkUfsPlatformInvalid;
  *V = F->Reg[O / 4]; return MtkUfsPlatformOk;
}
static MTK_UFS_PLATFORM_STATUS PolicyWrite(void *C, MTK_UFS_MMIO_BANK B, uint32_t O, uint32_t V) {
  POLICY_FIXTURE *F = C;
  if (B != MtkUfsControllerBank || (O & 3U) || O >= sizeof(F->Reg)) return MtkUfsPlatformInvalid;
  F->Reg[O / 4] = O == 0x144U ? ((V & 1U) ? 3U : 0U) : V;
  return MtkUfsPlatformOk;
}
static MTK_UFS_PLATFORM_STATUS PolicySmc(void *C, const uint64_t A[8], int64_t *R) {
  POLICY_FIXTURE *F = C;
  if (A[0] != MTK_UFS_SMC_FUNCTION || A[4] != 0 || A[5] || A[6] || A[7]) return MtkUfsPlatformInvalid;
  F->SmcCalls++;
  if (A[1] == 8U) { *R = 0; return MtkUfsPlatformOk; }
  if (A[1] != 32U || (A[2] != 0U && A[2] != 1U) || A[3] != 0U) return MtkUfsPlatformInvalid;
  if (A[2] == 0U) { F->SramOffCalls++; *R = F->OffResult; return F->FailOffTransport ? MtkUfsPlatformIoError : MtkUfsPlatformOk; }
  F->SramOnCalls++; *R = F->OnResult; return F->FailOnTransport ? MtkUfsPlatformIoError : MtkUfsPlatformOk;
}
static MTK_UFS_PLATFORM_STATUS PolicyDme(void *C, int Get, uint16_t A, uint32_t *V) {
  POLICY_FIXTURE *F = C;
  if (Get) *V = F->Attr[A]; else F->Attr[A] = *V;
  return MtkUfsPlatformOk;
}
static MTK_UFS_PLATFORM_STATUS PolicyHibernate(void *C, int Enter) {
  POLICY_FIXTURE *F = C;
  if (Enter != 0 && Enter != 1) return MtkUfsPlatformInvalid;
  F->Reg[0x22C8 / 4] = ((Enter ? 3U : 2U) << 28) | 1U;
  return MtkUfsPlatformOk;
}
static void PolicyDelay(void *C, uint32_t U) { (void)C; (void)U; }
int RunSramPolicy(uint32_t Allow, int64_t OffResult, int64_t OnResult,
                  int FailOffTransport, int FailOnTransport, int DoResume,
                  int *SuspendStatus, int *ResumeStatus, int *FinalState,
                  int *SramResume, unsigned *SmcCalls, unsigned *SramOffCalls,
                  unsigned *SramOnCalls) {
  const MTK_UFS_PLATFORM_CONFIG Config = { 0x2300, 0x1000, 0, 0, 32,
    { {0x130,0x134,0x8000}, {0x140,0x144,0x80}, {0x150,0x154,0x200000} }, Allow };
  MTK_UFS_PLATFORM P;
  POLICY_FIXTURE F;
  MTK_UFS_PLATFORM_IO Io = { .Context=&F, .Read32=PolicyRead, .Write32=PolicyWrite,
    .DelayUs=PolicyDelay, .Smc=PolicySmc, .Dme=PolicyDme, .Hibernate=PolicyHibernate };
  memset(&F, 0, sizeof(F));
  F.OffResult = OffResult; F.OnResult = OnResult;
  F.FailOffTransport = FailOffTransport; F.FailOnTransport = FailOnTransport;
  F.Reg[0x34 / 4] = 1U;
  F.Reg[0x22C8 / 4] = (2U << 28) | 1U;
  if (MtkUfsPlatformInitialize(&P, &Io, &Config) != MtkUfsPlatformOk) return -1;
  P.State = MtkUfsPlatformLinkReady; P.RefClock = MtkUfsRefClockOn;
  *SuspendStatus = MtkUfsPlatformSuspend(&P);
  *ResumeStatus = -1;
  if (DoResume && *SuspendStatus == MtkUfsPlatformOk) *ResumeStatus = MtkUfsPlatformResume(&P);
  *FinalState = P.State; *SramResume = P.SramResume;
  *SmcCalls = F.SmcCalls; *SramOffCalls = F.SramOffCalls; *SramOnCalls = F.SramOnCalls;
  return 0;
}
'''

def verify_secure_handlers(archive):
    # Read only this public firmware member into memory; never extract a copy
    # or execute its key-management commands. The two dispatch records identify
    # the distinct kernel/bootloader UFS handlers used by this exact firmware.
    raw = subprocess.check_output(["bsdtar", "-xOf", str(archive), "Firmware/tee.img"], timeout=60)
    expected = "e1000e08cfb8d7bd077839306ba8597e930554df6765423333ba4ab626f86182"
    if len(raw) != 863200 or hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError("T70 factory secure firmware identity mismatch")
    base = 0x48200000
    records = ((0x2C7C8, 0x82000276, 0xC2000276, "MTK_SIP_KERNEL_UFS_CONTROL", 0x4821EC80),
               (0x2C7E8, 0x8200040D, 0xC200040D, "MTK_SIP_BL_UFS_CONTROL", 0x4821EDFC))
    for at, fid32, fid64, name, target in records:
        a, b, name_ptr, _, handler = struct.unpack_from("<IIQQQ", raw, at)
        name_at = name_ptr - base + 0x200
        if ((a, b, handler) != (fid32, fid64, target) or not 0 <= name_at < len(raw) or
                raw[name_at:name_at + len(name) + 1] != name.encode() + b"\0"):
            raise ValueError("Secure UFS dispatch record mismatch")
    reset, stack, sentinel = 0x10003448, 0x60000000, 0x70000000
    fixtures = []
    for initial in (0, 0xFFFFFFFF, 0xA5A5A5A5, 0x5A5A5A5A):
        for high in (0, 1):
            fixtures.append(("kernel-reset", 0x4821EC80, 2, high, 0, initial, 0,
                             (initial & ~8) | (high << 3), True))
    for initial in (0, 0xFFFFFFFF):
        for on in (0, 1):
            for stage in (0, 1):
                fixtures.append(("kernel-refclock-notification", 0x4821EC80, 8, on, stage,
                                 initial, -1, initial, False))
            fixtures.append(("kernel-SRAM-power", 0x4821EC80, 32, on, 0, initial, -1, initial, False))
            fixtures.append(("bootloader-command-2", 0x4821EDFC, 2, on, 0, initial, -3, initial, False))
    for command in (0, 1, 4, 64, 128, 256, 512):
        fixtures.append(("kernel-other-unsupported", 0x4821EC80, command, 0, 0, 0, -1, 0, False))
    cases = []
    for name, target, command, value, stage, initial, result, final, touches_reset in fixtures:
        machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        code = raw[0x200:]
        machine.mem_map(base, (len(code) + 4095) & ~4095)
        machine.mem_write(base, code)
        machine.mem_map(reset & ~4095, 4096)
        machine.mem_write(reset, struct.pack("<I", initial))
        machine.mem_map(stack, 0x10000)
        machine.mem_map(sentinel, 4096)
        return_context = stack + 0x1000
        context_before = bytes([0xA5]) * 32
        machine.mem_write(return_context, context_before)
        memory = []
        def access(uc, kind, address, size, data, _):
            if address != reset or size != 4:
                raise RuntimeError("Secure handler accessed an unexpected MMIO register")
            memory.append(("write" if kind == UC_MEM_WRITE else "read", address, size, data))
        def guard(uc, address, size, _):
            if address == sentinel:
                uc.emu_stop()
            elif not target <= address < (0x4821EDFC if target == 0x4821EC80 else 0x4821EF4C):
                raise RuntimeError("Secure handler escaped the selected no-external-call branch")
        machine.hook_add(UC_HOOK_CODE, guard)
        machine.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access,
                         begin=reset & ~4095, end=(reset & ~4095) + 4095)
        machine.reg_write(UC_ARM64_REG_SP, stack + 0xF000)
        machine.reg_write(UC_ARM64_REG_LR, sentinel)
        machine.reg_write(UC_ARM64_REG_X0, command)
        machine.reg_write(UC_ARM64_REG_X1, value)
        machine.reg_write(UC_ARM64_REG_X2, stage)
        machine.reg_write(UC_ARM64_REG_X5, return_context)
        try:
            machine.emu_start(target, sentinel + 4, count=1000)
        except Exception as error:
            raise RuntimeError((name, command, value, stage,
                hex(machine.reg_read(UC_ARM64_REG_PC)), hex(machine.reg_read(UC_ARM64_REG_SP)))) from error
        observed = machine.reg_read(UC_ARM64_REG_X0)
        if observed & (1 << 63):
            observed -= 1 << 64
        writes = [(a, width, data) for kind, a, width, data in memory if kind == "write"]
        context_after = bytearray(context_before)
        context_write = target == 0x4821EDFC and value == 0
        if context_write:
            struct.pack_into("<q", context_after, 16, -3)

        if (machine.reg_read(UC_ARM64_REG_PC) != sentinel or observed != result or
                struct.unpack("<I", machine.mem_read(reset, 4))[0] != final or
                writes != ([(reset, 4, final)] if touches_reset else []) or
                len(memory) != (2 if touches_reset else 0) or
                machine.mem_read(return_context, 32) != context_after):
            raise AssertionError((name, command, value, stage, observed, memory))
        cases.append({"case": name, "command": command, "value": value, "stage": stage,
                      "return": observed, "reset_writes": len(writes), "error_return_context_written": context_write})
    return {"status": "passed", "firmware_member": "Firmware/tee.img", "bytes": len(raw),
            "sha256": expected, "original_archive": str(archive),
            "scope": "Original ARM64 secure handler branches at their exact dispatch targets in Unicorn; modeled reset register/return context/stack, no external calls or key-management commands; full SMC dispatcher/EL3/device not executed",
            "dispatch_records_verified": len(records), "cases": cases,
            "kernel_reset_command": 2, "kernel_reset_register": hex(reset), "kernel_reset_bit": 3,
            "kernel_refclock_notification_result": -1, "kernel_SRAM_power_result": -1,
            "bootloader_command_2_result": -3, "input_copies_created": 0}


def verify_suspend_resume(text, callback_ranges, callback_calls):
    """Run the hash-pinned module callbacks with MMIO and kernel calls mocked."""
    code, mmio, hba, host = 0x10000000, 0x20000000, 0x30000000, 0x30002000
    stack, sentinel, canary = 0x40000000, 0x50000000, 0x60000000
    mphy = callback_ranges["ufs_mtk_mphy_power_on"]
    callbacks = {
        "suspend": callback_ranges["ufs_mtk_suspend"],
        "resume": callback_ranges["ufs_mtk_resume"],
    }
    call_sites = {}
    for label, sites in callback_calls.items():
        if label in ("ufs_mtk_suspend", "ufs_mtk_resume", "ufs_mtk_mphy_power_on",
                     "ufs_mtk_wait_idle_state", "readl_13e0", "writel_0e70",
                     "readl_8434", "writel_74b4", "writel_85c8"):
            for place, name in sites.items():
                if place in call_sites:
                    raise ValueError("Duplicate stock external-call relocation")
                call_sites[place] = name
    all_ranges = [(start, start + length, label)
                  for label, (start, length) in callback_ranges.items()]
    stack_canary = 0x1234567812345678

    def run_one(which, smc_result):
        machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        machine.mem_map(code, (len(text) + 4095) & ~4095)
        machine.mem_write(code, text)
        machine.mem_map(mmio, 0x10000)
        machine.mem_map(hba, 0x10000)
        machine.mem_map(stack, 0x10000)
        machine.mem_map(sentinel, 0x1000)
        machine.mem_map(canary, 0x1000)
        machine.mem_write(hba, struct.pack("<Q", mmio))
        machine.mem_write(hba + 0x54, struct.pack("<I", 2))  # stock Hibern8 enum
        machine.mem_write(hba + 0xB8, struct.pack("<Q", host))
        machine.mem_write(hba + 0x1F0, struct.pack("<I", 1))  # retained HCE path
        machine.mem_write(hba + 0xA48, struct.pack("<Q", 0))
        machine.mem_write(hba + 0xA84, struct.pack("<I", 0))
        machine.mem_write(hba + 0xBB8, b"\0")
        machine.mem_write(hba + 0x131D, b"\0")  # no MCQ reconfiguration
        machine.mem_write(host, struct.pack("<Q", 0))  # NULL M-PHY
        machine.mem_write(host + 0x4A, b"\x03")  # known UniPro version
        machine.mem_write(host + 0x70, struct.pack("<I", 0x10450100))
        machine.mem_write(host + 0x74, struct.pack("<I", 0))  # stock host ID
        machine.mem_write(host + 0x1588, b"\0")
        machine.mem_write(mmio + 0x34, struct.pack("<I", 1))
        machine.mem_write(mmio + 0x140, struct.pack("<I", 0xA5A50000))
        machine.mem_write(mmio + 0x14C, struct.pack("<I", 0x02000000))
        machine.mem_write(canary + 0x710, struct.pack("<Q", stack_canary))

        events, external_calls, smc_calls, mphy_calls = [], [], [], []
        mphy_mmio_writes, result_buffer_reads = [], []
        phy_power_calls, hibern8_exit_calls = [], 0
        fake_time = [1_000_000_000]

        def code_hook(uc, address, _size, _):
            nonlocal active_result_buffer, hibern8_exit_calls
            if address == sentinel:
                uc.emu_stop()
                return
            place = address - code
            if not any(start <= place < end for start, end, _label in all_ranges):
                raise RuntimeError("Stock callback escaped validated symbol ranges: " + hex(address))
            instruction = struct.unpack("<I", uc.mem_read(address, 4))[0]
            if instruction == 0xD5384108:  # mrs x8, SP_EL0; controlled canary source
                uc.reg_write(UC_ARM64_REG_X8, canary)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
                return
            if place == mphy[0]:
                enabled = uc.reg_read(UC_ARM64_REG_X1) & 1
                mphy_calls.append(enabled)
                events.append(("mphy", enabled))
            name = call_sites.get(place)
            if name is None:
                return
            external_calls.append(name)
            if name == "__stack_chk_fail":
                raise RuntimeError("Stock callback reached stack-canary failure")
            if name in ("phy_power_on", "phy_power_off", "regulator_enable", "regulator_disable"):
                phy_power_calls.append(name)
                raise RuntimeError("NULL M-PHY path unexpectedly touched PHY/rail API: " + name)
            if name == "__arm_smccc_smc":
                args = [uc.reg_read(reg) for reg in (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                    UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
                    UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7)]
                expected_value = 0 if which == "suspend" else 1
                if args != [0xC2000276, 32, expected_value, 0, 0, 0, 0, 0]:
                    raise RuntimeError(("Unexpected stock cmd32 arguments", which, args))
                sp = uc.reg_read(UC_ARM64_REG_SP)
                result_ptr = struct.unpack("<Q", uc.mem_read(sp, 8))[0]
                if not stack <= result_ptr <= stack + 0x10000 - 32:
                    raise RuntimeError("cmd32 result buffer is outside the mock stack")
                raw_result = smc_result & 0xFFFFFFFFFFFFFFFF
                uc.mem_write(result_ptr, struct.pack("<QQQQ", raw_result, 0, 0, 0))
                smc_calls.append({"command": 32, "value": expected_value, "result": smc_result,
                                  "result_buffer": result_ptr - stack})
                events.append(("cmd32", expected_value))
                active_result_buffer = (result_ptr, result_ptr + 32)
                uc.reg_write(UC_ARM64_REG_X0, 0)
            elif name in ("ufshcd_dme_set_attr", "ufs_mtk_eh_unipro_set_lpm",
                          "ufs_mtk_auto_hibern8_disable", "ufshcd_uic_hibern8_exit",
                          "ufshcd_hba_enable", "ufshcd_make_hba_operational",
                          "ufshcd_mcq_make_queues_operational", "ufshcd_mcq_config_mac"):
                if name in ("ufshcd_dme_set_attr", "ufshcd_uic_hibern8_exit",
                            "ufshcd_hba_enable", "ufshcd_make_hba_operational") and uc.reg_read(UC_ARM64_REG_X0) != hba:
                    raise RuntimeError(("Wrong HBA argument to bounded kernel mock", name))
                if name == "ufshcd_uic_hibern8_exit":
                    hibern8_exit_calls += 1
                    events.append(("uic_hibern8_exit_mock",))
                    # Model only the externally supplied exit result for the
                    # module's subsequent status poll; this does not run UIC.
                    uc.mem_write(mmio + 0x14C, struct.pack("<I", 0x02000000))
                uc.reg_write(UC_ARM64_REG_X0, 0)
            elif name in ("ktime_get", "ktime_get_mono_fast_ns"):
                fake_time[0] += 1_000_000
                uc.reg_write(UC_ARM64_REG_X0, fake_time[0])
            elif name in ("__const_udelay", "usleep_range_state", "cpu_latency_qos_update_request",
                          "ufshcd_update_evt_hist", "log_read_mmio", "log_post_read_mmio",
                          "log_write_mmio", "log_post_write_mmio", "_dev_info"):
                uc.reg_write(UC_ARM64_REG_X0, 0)
            elif name in ("ufshcd_link_recovery", "_dev_err", "_dev_warn", "regulator_set_mode"):
                raise RuntimeError(("Unexpected recovery/error path external call", which, name))
            else:
                raise RuntimeError(("Unbounded stock external call", which, name, hex(address)))
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

        def mmio_write_hook(uc, _kind, address, size, value, _):
            place = uc.reg_read(UC_ARM64_REG_PC) - code
            if mphy[0] <= place < mphy[0] + mphy[1]:
                mphy_mmio_writes.append((address, size, value))

        active_result_buffer = None
        def result_read_hook(_uc, _kind, address, size, _value, _):
            if active_result_buffer is not None:
                begin, end = active_result_buffer
                if address < end and address + size > begin:
                    result_buffer_reads.append((address, size))

        machine.hook_add(UC_HOOK_CODE, code_hook)
        machine.hook_add(UC_HOOK_MEM_WRITE, mmio_write_hook, begin=mmio, end=mmio + 0xFFFF)
        machine.hook_add(UC_HOOK_MEM_READ, result_read_hook, begin=stack, end=stack + 0xFFFF)
        machine.reg_write(UC_ARM64_REG_SP, stack + 0xF000)
        machine.reg_write(UC_ARM64_REG_LR, sentinel)
        machine.reg_write(UC_ARM64_REG_X0, hba)
        machine.reg_write(UC_ARM64_REG_X1, 0)
        machine.reg_write(UC_ARM64_REG_X2, 1 if which == "suspend" else 0)
        start, _length = callbacks[which]
        try:
            machine.emu_start(code + start, sentinel + 4, count=100000)
        except Exception as error:
            raise RuntimeError((which, smc_result, "stock callback fixture failed",
                hex(machine.reg_read(UC_ARM64_REG_PC)))) from error

        returned = machine.reg_read(UC_ARM64_REG_X0)
        if returned & (1 << 63):
            returned -= 1 << 64
        final_link_state = struct.unpack("<I", machine.mem_read(hba + 0x54, 4))[0]
        expected_link_state = 2 if which == "suspend" else 1
        if (machine.reg_read(UC_ARM64_REG_PC) != sentinel or returned != 0 or
                final_link_state != expected_link_state or len(smc_calls) != 1 or
                len(mphy_calls) != 1 or phy_power_calls or mphy_mmio_writes or result_buffer_reads):
            raise AssertionError((which, smc_result, returned, final_link_state,
                                  smc_calls, mphy_calls, phy_power_calls, mphy_mmio_writes,
                                  result_buffer_reads, external_calls))
        mphy_event = ("mphy", 0 if which == "suspend" else 1)
        cmd_event = ("cmd32", 0 if which == "suspend" else 1)
        if mphy_event not in events or cmd_event not in events:
            raise AssertionError((which, events))
        if which == "suspend" and events.index(mphy_event) >= events.index(cmd_event):
            raise AssertionError(("Suspend must power down M-PHY before cmd32 off", events))
        if which == "resume" and events.index(cmd_event) >= events.index(mphy_event):
            raise AssertionError(("Resume must send cmd32 on before M-PHY power on", events))
        if (which == "resume" and hibern8_exit_calls != 1) or (which == "suspend" and hibern8_exit_calls != 0):
            raise AssertionError((which, "unexpected Hibern8 exit callback count", hibern8_exit_calls))
        return {"callback": which, "initial_hba_link_state": 2,
                "final_hba_link_state": final_link_state, "mphy_pointer": "NULL",
                "mphy_helper_calls": mphy_calls, "cmd32": smc_calls[0],
                "stock_return": returned, "cmd32_result_read_by_module": False,
                "mocked_external_calls": external_calls,
                "generic_hibern8_exit_mock_calls": hibern8_exit_calls,
                "phy_or_rail_calls": phy_power_calls,
                "mphy_mmio_writes": mphy_mmio_writes,
                "cmd32_result_buffer_reads": result_buffer_reads}

    cases = []
    for result in (-1, -3):
        for callback in ("suspend", "resume"):
            cases.append(run_one(callback, result))
    return {"status": "passed", "cases": cases,
            "scope": "Hash-pinned original ARM64 ufs_mtk_suspend/ufs_mtk_resume and NULL-M-PHY helper paths in Unicorn. Fake MMIO and bounded external-call mocks only; Hibern8 UIC implementation, generic UFS clock framework, analog rails, firmware/TEE, and physical hardware were not executed.",
            "assertions": ["HBA link state starts in Hibern8 (2); resume reaches active (1)",
                           "cmd32 off follows M-PHY-off helper; cmd32 on precedes M-PHY-on helper",
                           "cmd32 result buffer is set to -1 or -3 and not inspected by stock callback",
                           "NULL M-PHY performs no phy_power_* or regulator_* operation"]}


def verify_edk2_sram_policy(core):
    """Exercise the production C core's stricter retention policy in a fixture."""
    function = core.RunSramPolicy
    function.argtypes = [C.c_uint32, C.c_int64, C.c_int64, C.c_int, C.c_int, C.c_int,
                         C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_int),
                         C.POINTER(C.c_int), C.POINTER(C.c_uint), C.POINTER(C.c_uint),
                         C.POINTER(C.c_uint)]
    function.restype = C.c_int
    fixtures = (
        {"name": "exact-minus-one-retained-off-and-on", "args": (1, -1, -1, 0, 0, 1),
         "expect": (0, 0, 4, 0, 1, 1)},
        {"name": "exact-minus-one-rejected-with-default-policy", "args": (0, -1, 0, 0, 0, 0),
         "expect": (3, -1, 4, 0, 1, 0)},
        {"name": "ordinary-off-error-with-successful-recovery-on", "args": (1, -3, 0, 0, 0, 0),
         "expect": (3, -1, 4, 0, 1, 1)},
        {"name": "ordinary-off-and-on-errors-preserve-fault", "args": (1, -3, -3, 0, 0, 0),
         "expect": (3, -1, 6, 1, 1, 1)},
        {"name": "ordinary-on-error-after-retained-suspend", "args": (1, -1, -3, 0, 0, 1),
         "expect": (0, 3, 6, 2, 1, 1)},
        {"name": "completed-minus-one-is-not-accepted-on-transport-error", "args": (1, -1, -1, 1, 0, 0),
         "expect": (3, -1, 6, 1, 1, 1)},
        {"name": "on-transport-error-after-retained-suspend", "args": (1, -1, -1, 0, 1, 1),
         "expect": (0, 3, 6, 2, 1, 1)},
    )
    results = []
    for fixture in fixtures:
        suspend_status, resume_status, state = C.c_int(), C.c_int(), C.c_int()
        sram_resume, smc_calls, off_calls, on_calls = C.c_int(), C.c_uint(), C.c_uint(), C.c_uint()
        status = function(*fixture["args"], C.byref(suspend_status), C.byref(resume_status),
                          C.byref(state), C.byref(sram_resume), C.byref(smc_calls),
                          C.byref(off_calls), C.byref(on_calls))
        if status != 0:
            raise RuntimeError((fixture["name"], "native retention fixture setup failed", status))
        observed = (suspend_status.value, resume_status.value, state.value,
                    sram_resume.value, off_calls.value, on_calls.value)
        if observed != fixture["expect"]:
            raise AssertionError((fixture["name"], observed, fixture["expect"], smc_calls.value))
        results.append({"case": fixture["name"], "status": "passed",
                        "suspend_status": observed[0], "resume_status": observed[1],
                        "final_state": observed[2], "sram_resume": observed[3],
                        "cmd32_off_calls": observed[4], "cmd32_on_calls": observed[5],
                        "smc_calls_including_refclock_cmd8": smc_calls.value})
    return {"status": "passed", "cases": results,
            "scope": "Production MtkUfsPlatform.c in a compiled native fixture with fake MMIO/SMC/DME/Hibern8; not the stock ARM64 module or device.",
            "policy": "Only callback-completed exact -1 may use explicit AllowSramRetention; transport failures and ordinary nonzero results stay errors.",
            "bridge_config_trailing_allow_sram_retention": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tee-firmware", type=Path, help="original factory RAR; stream only Firmware/tee.img")
    args = parser.parse_args()
    secure_handlers = verify_secure_handlers(args.tee_firmware) if args.tee_firmware else None
    raw = BINARY.read_bytes()
    if hashlib.sha256(raw).hexdigest() != EXPECTED_SHA:
        raise ValueError("Stock UFS module identity mismatch")
    if len(raw) < 64 or raw[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", raw, 16) != (1, 183):
        raise ValueError("Expected AArch64 relocatable ELF64 little endian")
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    width, count, names_index = struct.unpack_from("<HHH", raw, 58)
    if width != 64 or not count or shoff + width * count > len(raw):
        raise ValueError("ELF section table exceeds module")
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * width) for i in range(count)]
    def contents(index):
        if not 0 <= index < count:
            raise ValueError("Section index outside module")
        section = sections[index]
        if section[4] + section[5] > len(raw):
            raise ValueError("Section contents outside module")
        return raw[section[4]:section[4] + section[5]]
    if not 0 <= names_index < count:
        raise ValueError("Section-name table index outside ELF")
    section_names = contents(names_index)
    text_sections = [i for i, section in enumerate(sections)
                     if section[0] < len(section_names) and
                     section_names[section[0]:].split(b"\0", 1)[0] == b".text"]
    if len(text_sections) != 1:
        raise ValueError("Expected exactly one .text section")
    symbols, function_definitions = {}, {}
    for index, section in enumerate(sections):
        if section[1] != 2:  # SHT_SYMTAB
            continue
        if section[9] != 24 or section[5] % 24:
            raise ValueError("Malformed symbol table")
        names = contents(section[6])
        entries = []
        data = contents(index)
        for at in range(0, len(data), 24):
            name_offset, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, at)
            if name_offset >= len(names):
                raise ValueError("Symbol name outside string table")
            name = names[name_offset:].split(b"\0", 1)[0].decode()
            entries.append(name)
            if name and (info & 0x0F) == 2 and shndx != 0:  # STT_FUNC definition
                function_definitions.setdefault(name, []).append((shndx, value, size))
        symbols[index] = entries
    function_matches = function_definitions.get(FUNCTION, [])
    if len(function_matches) != 1:
        raise ValueError("Target function missing")
    text_index, offset, length = function_matches[0]
    if text_index != text_sections[0]:
        raise ValueError("Target callback is not in .text")
    text = contents(text_index)
    if not length or offset + length > len(text):
        raise ValueError("Target function outside section")
    call_relocations = {}
    for index, section in enumerate(sections):
        if section[1] != 4 or section[7] != text_index:  # SHT_RELA
            continue
        if section[9] != 24 or section[5] % 24:
            raise ValueError("Malformed relocation table")
        data = contents(index)
        table = symbols.get(section[6])
        if table is None:
            raise ValueError("Relocation references a missing symbol table")
        for at in range(0, len(data), 24):
            place, info, addend = struct.unpack_from("<QQq", data, at)
            if info & 0xFFFFFFFF == 283:  # R_AARCH64_CALL26
                if addend != 0:
                    raise ValueError("Unexpected call addend")
                symbol_index = info >> 32
                if symbol_index >= len(table) or place in call_relocations:
                    raise ValueError("Malformed or duplicate CALL26 relocation")
                call_relocations[place] = table[symbol_index]
    calls = {place: name for place, name in call_relocations.items()
             if offset <= place < offset + length}
    allowed = {"ufshcd_dme_get_attr", "ufshcd_dme_set_attr", "ufs_mtk_eh_unipro_set_lpm", "__stack_chk_fail"}
    if set(calls.values()) != allowed:
        raise ValueError("Unexpected function external calls: " + repr(calls))

    callback_ranges_by_label, callback_calls = {}, {}
    expected_by_symbol = {}
    for spec in EXPECTED_CALLBACK_FUNCTIONS:
        expected_by_symbol.setdefault(spec["symbol"], []).append((text_index, spec["offset"], spec["size"]))
    for symbol, expected in expected_by_symbol.items():
        actual = function_definitions.get(symbol, [])
        if sorted(actual) != sorted(expected):
            raise ValueError(("Stock callback/helper symbol range mismatch", symbol,
                              sorted(actual), sorted(expected)))
    for spec in EXPECTED_CALLBACK_FUNCTIONS:
        start, size = spec["offset"], spec["size"]
        if start + size > len(text):
            raise ValueError("Stock callback/helper range exceeds .text: " + spec["label"])
        actual_calls = {place: name for place, name in call_relocations.items()
                        if start <= place < start + size}
        if actual_calls != spec["calls"]:
            raise ValueError(("Stock callback/helper CALL26 map mismatch", spec["label"],
                              {hex(k): v for k, v in actual_calls.items()},
                              {hex(k): v for k, v in spec["calls"].items()}))
        callback_ranges_by_label[spec["label"]] = (start, size)
        callback_calls[spec["label"]] = actual_calls
    callback_ranges = dict(callback_ranges_by_label)
    for name in ("ufs_mtk_suspend", "ufs_mtk_resume", "ufs_mtk_mphy_power_on",
                 "ufs_mtk_wait_idle_state"):
        callback_ranges[name] = callback_ranges_by_label[name]
    stock_power_callbacks = verify_suspend_resume(text, callback_ranges, callback_calls)
    code, hba, host, stack, sentinel, canary = 0x10000000, 0x30000000, 0x30002000, 0x40000000, 0x50000000, 0x60000000
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    machine.mem_map(code, (len(text) + 4095) & ~4095)
    machine.mem_write(code, text)
    machine.mem_map(hba, 0x10000)
    machine.mem_map(stack, 0x10000)
    machine.mem_map(sentinel, 0x1000)
    machine.mem_map(canary, 0x1000)
    machine.mem_write(hba + 0xB8, struct.pack("<Q", host))
    machine.mem_write(host + 0x4A, bytes([3]))  # already known UniPro version
    machine.mem_write(canary + 0x710, struct.pack("<Q", 0x1234567812345678))
    store, trace = {}, []
    def hook(uc, address, size, _):
        if address == sentinel:
            uc.emu_stop()
            return
        place = address - code
        if not offset <= place < offset + length:
            raise RuntimeError("Execution escaped selected stock function: " + hex(address))
        word = struct.unpack("<I", uc.mem_read(address, 4))[0]
        if word == 0xD5384108:  # mrs x8, SP_EL0: fixture only
            uc.reg_write(UC_ARM64_REG_X8, canary)
            uc.reg_write(UC_ARM64_REG_PC, address + 4)
            return
        if place not in calls:
            return
        name = calls[place]
        if name not in ("ufshcd_dme_get_attr", "ufshcd_dme_set_attr"):
            raise RuntimeError("Unexpected stock error branch: " + name)
        if uc.reg_read(UC_ARM64_REG_X0) != hba:
            raise RuntimeError("Wrong HBA argument")
        selector = uc.reg_read(UC_ARM64_REG_X1)
        attr = selector >> 16
        if selector & 0xFFFF or attr not in ATTRIBUTES:
            raise RuntimeError("Unexpected attribute selector: " + hex(selector))
        get = name == "ufshcd_dme_get_attr"
        if get:
            if uc.reg_read(UC_ARM64_REG_X3) != 0:
                raise RuntimeError("Unexpected peer DME request")
            value = store[attr]
            uc.mem_write(uc.reg_read(UC_ARM64_REG_X2), struct.pack("<I", value))
        else:
            if uc.reg_read(UC_ARM64_REG_X2) != 0 or uc.reg_read(UC_ARM64_REG_X4) != 0:
                raise RuntimeError("Unexpected DME set flags")
            value = uc.reg_read(UC_ARM64_REG_X3) & 0xFFFFFFFF
            store[attr] = value
        trace.append((int(get), attr, value))
        uc.reg_write(UC_ARM64_REG_X0, 0)
        uc.reg_write(UC_ARM64_REG_PC, address + 4)
    machine.hook_add(UC_HOOK_CODE, hook)
    cases = 0
    versions = (0x10380000, 0x20160706, 0x10440000, 0x10450000, 0x104500FF, 0x10450100, 0x10460000, 0x10480000)
    with tempfile.TemporaryDirectory(prefix="ufs-binary-", dir=None if sys.platform == "darwin" else REPO / "Build") as tmp:
        bridge, library = Path(tmp) / "bridge.c", Path(tmp) / "core.so"
        bridge.write_text(BRIDGE)
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-shared", "-fPIC",
            "-I" + str(REPO / "Silicon/MediaTek/MediaTekPkg/Include"), str(CORE), str(bridge), "-o", str(library)], check=True)
        core = C.CDLL(str(library))
        core.Run.argtypes = [C.c_uint32, C.c_int, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
        core.Run.restype = C.c_int
        edk2_sram_policy = verify_edk2_sram_policy(core)
        rng = random.Random(0x6789)
        for version in versions:
            for post in (0, 1):
                for _ in range(32):
                    initial = [rng.getrandbits(32) for attr in ATTRIBUTES]
                    store.clear(); store.update(zip(ATTRIBUTES, initial)); trace.clear()
                    machine.mem_write(host + 0x70, struct.pack("<I", version))
                    machine.reg_write(UC_ARM64_REG_SP, stack + 0xF000)
                    machine.reg_write(UC_ARM64_REG_LR, sentinel)
                    machine.reg_write(UC_ARM64_REG_X0, hba)
                    machine.reg_write(UC_ARM64_REG_X1, post)
                    machine.emu_start(code + offset, sentinel + 4, count=10000)
                    if machine.reg_read(UC_ARM64_REG_PC) != sentinel or machine.reg_read(UC_ARM64_REG_X0) != 0:
                        raise RuntimeError("Stock function did not return success to sentinel")
                    output, native_trace = (C.c_uint32 * 5)(), (C.c_uint32 * 192)()
                    count = core.Run(version, post, (C.c_uint32 * 5)(*initial), output, native_trace)
                    native = [tuple(native_trace[i * 3:i * 3 + 3]) for i in range(count)]
                    if count < 0 or native != trace or list(output) != [store[a] for a in ATTRIBUTES]:
                        raise AssertionError((hex(version), post, native, trace, list(output), store))
                    cases += 1
    result = {"scope": "shipped ARM64 instructions in Unicorn vs production native C; successful UniPro pre/post link setup with controlled DME attributes; no device",
        "module": str(BINARY.relative_to(REPO.parent)), "module_sha256": EXPECTED_SHA,
        "function": FUNCTION, "symbol_offset": hex(offset), "symbol_size": length,
        "call_sites_trapped": {hex(k): v for k, v in calls.items()}, "canary": "two SP_EL0 reads redirected to a fixture at execution time; original module bytes unchanged",
        "cases": cases, "versions": [hex(v) for v in versions], "status": "passed",
        "stock_callback_elf_ranges": {
            spec["label"]: {"section": ".text", "offset": hex(spec["offset"]), "size": spec["size"]}
            for spec in EXPECTED_CALLBACK_FUNCTIONS},
        "stock_callback_call_relocations": {
            label: {hex(place): name for place, name in sites.items()}
            for label, sites in callback_calls.items()},
        "stock_suspend_resume": stock_power_callbacks,
        "edk2_sram_retention_policy": edk2_sram_policy,
        "production_source_sha256": hashlib.sha256(CORE.read_bytes()).hexdigest(),
        "header_sha256": hashlib.sha256((REPO / "Silicon/MediaTek/MediaTekPkg/Include/Library/MtkUfsPlatform.h").read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "secure_handlers": secure_handlers}
    (REPO.parent / "analysis/ufs-binary-tests.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Stock UFS UniPro binary / production C: {cases} cases passed; no device")
    print(f"Stock UFS suspend/resume callbacks: {len(stock_power_callbacks['cases'])} cases passed; generic UIC/rails not executed")
    print(f"Production C SRAM retention policy: {len(edk2_sram_policy['cases'])} controlled cases passed")
    if secure_handlers:
        print(f"Stock UFS secure handlers: {len(secure_handlers['cases'])} cases passed; no EL3/device execution")

if __name__ == "__main__":
    main()
