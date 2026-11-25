// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Mali-G57 MC2 power for the Teclast T70 Pro (MT6789).
//
// Every boot this driver clears the GPU record and, at ReadyToBoot, reads the
// GPU power state the preloader left (SPM, MFGPLL, clock mux, PMIC rails).
// Built with T70_GPU_POWER_ON it then powers the GPU the way the T70's own
// stock mtk_gpufreq_mt6789.ko (V1.08, sha256 62806856...) does in
// __gpufreq_power_control(POWER_ON): VGPU then VSRAM_GPU, MTCMOS MFG0 to MFG3
// with the MFG1 bus protection released, MFG_PLL_SEL to MFGPLL (checked in
// CLK_CFG_4), the BG3D gate, the MFG_TOP clock controls and the GPU
// timestamp. It stops at the first step that does not complete. The GPU stays
// at one operating point; the Windows driver owns the GPU core from then on.
//
// The stock module was disassembled and compared with MediaTek's MT6789
// sources (gpufreq_mt6789.[ch], clk-mt6789.c, clk-mt6789-mfgcfg.c,
// clk-pll.c, clk-mux.c, mt6358-regulator.c) and the T70 stock device tree.
// Where they differ the T70 module wins: it enables VGPU before VSRAM_GPU,
// writes no infra way enable, and its 390 MHz point is 675 mV, not 625 mV.
// ACPI shows GPU0 only when the record says the GPU answered with a Mali-G57.
//
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/EventGroup.h>
#include <Protocol/MtkPmic.h>

#ifndef T70_GPU_POWER_ON
#define T70_GPU_POWER_ON 0
#endif

// Record in the board handoff page, read by DSDT GPU0._STA.
#define REC                 0x43F7F500UL
#define REC_BYTES           0x60U
#define REC_MAGIC           0x47303754U // "T70G"
#define REC_VERSION         1U
#define RES_SURVEY          0U          // read only; nothing was changed
#define RES_POWERED         1U          // powered and GPU_ID verified
#define RES_FAILED          0x80U       // | step that did not complete

// Stock device tree: /soc/gpufreq reg and /soc/mali@13000000.
#define TOPCKGEN            0x10000000UL
#define INFRACFG_AO         0x10001000UL
#define SPM                 0x10006000UL
#define APMIXED             0x1000C000UL
#define MFG_TOP             0x13FBF000UL
#define GPU                 0x13000000UL
#define GPU_BYTES           0x4000U

// gpufreq_mt6789.h
#define PWR_STATUS          (SPM + 0x16C)
#define PWR_STATUS_2ND      (SPM + 0x170)
#define MFG_PWR_CON(n)      (SPM + 0x308 + 4U * (n))
#define PWR_RST_B           BIT0
#define PWR_ISO             BIT1
#define PWR_ON              BIT2
#define PWR_ON_2ND          BIT3
#define PWR_CLK_DIS         BIT4
#define SRAM_PDN            BIT8
#define SRAM_PDN_ACK        BIT12
#define MFG_PWR_STA(n)      (BIT2 << (n))
#define MFG_ALL_STA         (BIT2 | BIT3 | BIT4 | BIT5)

#define PROTECTEN_SET       (INFRACFG_AO + 0x2A0)
#define PROTECTEN_CLR       (INFRACFG_AO + 0x2A4)
#define PROTECTSTA1         (INFRACFG_AO + 0x228)
#define PROTECTEN_1_SET     (INFRACFG_AO + 0x2A8)
#define PROTECTEN_1_CLR     (INFRACFG_AO + 0x2AC)
#define PROTECTSTA1_1       (INFRACFG_AO + 0x258)
#define PROTECTEN_2_SET     (INFRACFG_AO + 0x714)
#define PROTECTEN_2_CLR     (INFRACFG_AO + 0x718)
#define PROTECTSTA1_2       (INFRACFG_AO + 0x724)
#define MFG1_PROT_STEP1_0   BIT21
#define MFG1_PROT_STEP1_1   (BIT5 | BIT6)
#define MFG1_PROT_STEP2_0   (BIT21 | BIT22)
#define MFG1_PROT_STEP2_1   BIT7

#define MFGPLL_CON0         (APMIXED + 0x268)   // [0] PLL enable
#define MFGPLL_CON1         (APMIXED + 0x26C)   // [31] PCW change, [26:24] posdiv, [21:0] PCW
#define MFGPLL_CON3         (APMIXED + 0x274)   // [0] power on, [1] isolation
#define PLL_PWR_ON          BIT0
#define PLL_ISO_EN          BIT1
#define PLL_EN              BIT0
#define PCW_CHG             BIT31

// clk-mt6789.c: CLK_CFG_4 holds mfg_ref_sel [17:16] gate 23, mfg_pll_sel [18].
#define CLK_CFG_UPDATE      (TOPCKGEN + 0x04)
#define CLK_CFG_4           (TOPCKGEN + 0x50)
#define CLK_CFG_4_SET       (TOPCKGEN + 0x54)
#define CLK_CFG_4_CLR       (TOPCKGEN + 0x58)
#define MFG_REF_PDN         BIT23
#define MFG_REF_UPD         BIT18
#define MFG_PLL_SEL_MFGPLL  BIT18

// clk-mt6789-mfgcfg.c and __gpufreq_external_cg_control.
#define MFG_CG_STA          (MFG_TOP + 0x0)
#define MFG_CG_CLR          (MFG_TOP + 0x8)
#define MFG_CG_BG3D         BIT0
#define MFG_ASYNC_CON       (MFG_TOP + 0x20)
#define MFG_ASYNC_CON_1     (MFG_TOP + 0x24)
#define MFG_GLOBAL_CON      (MFG_TOP + 0xB0)
#define MFG_QCHANNEL_CON    (MFG_TOP + 0xB4)
#define MFG_I2M_PROTECTOR   (MFG_TOP + 0xF60)
#define MFG_TIMESTAMP       (MFG_TOP + 0x130)   // [0] enable, [1] SoC timer

// Operating point: opp44 of the T70 stock module's g_default_gpu, 390 MHz,
// VGPU 675 mV, VSRAM_GPU 750 mV, posdiv 4 (the T70 device tree agrees).
#define GPU_KHZ             390000U
#define GPU_POSDIV          2U      // divide by 4: 1560 MHz VCO
#define VGPU_UV             675000U
#define VSRAM_UV            750000U
#define VGPU_NAME           "buck_vgpu"
#define VSRAM_NAME          "ldo_vsram_gpu"

#define POLL_US             10U
#define POLL_TRIES          500U    // 5 ms, the stock driver's limit
#define MUX_TRIES           6U      // the stock module retries mfg_pll_sel

STATIC EFI_EVENT mReadyToBoot;
STATIC BOOLEAN   mDone;

STATIC VOID Rec (UINTN Offset, UINT32 Value) { MmioWrite32 (REC + Offset, Value); }

STATIC EFI_STATUS
MapMmio (EFI_PHYSICAL_ADDRESS Base, UINT64 Bytes)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS Status = gDS->GetMemorySpaceDescriptor (Base, &D);
  if (EFI_ERROR (Status)) return Status;
  if (D.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, Bytes, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) return Status;
  } else if (D.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
    return EFI_UNSUPPORTED;
  }
  return gDS->SetMemorySpaceAttributes (Base, Bytes, EFI_MEMORY_UC);
}

// The PMIC methods refuse a reentrant call with EFI_NOT_READY; retry briefly.
STATIC EFI_STATUS
Rail (MTK_PMIC_PROTOCOL *Pmic, CONST CHAR8 *Name, BOOLEAN *On, UINT32 *Uv)
{
  EFI_STATUS Status = EFI_NOT_READY;
  for (UINTN Try = 0; Try < 10U && Status == EFI_NOT_READY; ++Try) {
    Status = Pmic->RegulatorIsEnabled (Name, On);
    if (!EFI_ERROR (Status)) Status = Pmic->RegulatorGetVoltage (Name, Uv);
    if (Status == EFI_NOT_READY) MicroSecondDelay (100);
  }
  return Status;
}

// Packs a rail sample: [31] enabled, [30] read ok, [23:0] microvolts.
STATIC UINT32
RailWord (MTK_PMIC_PROTOCOL *Pmic, CONST CHAR8 *Name)
{
  BOOLEAN On = FALSE;
  UINT32 Uv = 0;
  if (Pmic == NULL || EFI_ERROR (Rail (Pmic, Name, &On, &Uv))) return 0;
  return BIT30 | (On ? BIT31 : 0) | (Uv & 0xFFFFFFU);
}

#if T70_GPU_POWER_ON
STATIC EFI_STATUS
RailSet (MTK_PMIC_PROTOCOL *Pmic, CONST CHAR8 *Name, UINT32 Uv)
{
  EFI_STATUS Status = EFI_NOT_READY;
  for (UINTN Try = 0; Try < 10U && Status == EFI_NOT_READY; ++Try) {
    Status = Pmic->RegulatorSetVoltage (Name, Uv);
    if (Status == EFI_NOT_READY) MicroSecondDelay (100);
  }
  return Status;
}

STATIC EFI_STATUS
RailOn (MTK_PMIC_PROTOCOL *Pmic, CONST CHAR8 *Name)
{
  EFI_STATUS Status = EFI_NOT_READY;
  for (UINTN Try = 0; Try < 10U && Status == EFI_NOT_READY; ++Try) {
    Status = Pmic->RegulatorSetEnable (Name, TRUE);
    if (Status == EFI_NOT_READY) MicroSecondDelay (100);
  }
  return Status;
}

STATIC BOOLEAN
Wait (UINTN Address, UINT32 Mask, UINT32 Want)
{
  for (UINTN Try = 0; Try < POLL_TRIES; ++Try) {
    if ((MmioRead32 (Address) & Mask) == Want) return TRUE;
    MicroSecondDelay (POLL_US);
  }
  return (MmioRead32 (Address) & Mask) == Want;
}

STATIC BOOLEAN
WaitBoth (UINT32 Mask, UINT32 Want)
{
  for (UINTN Try = 0; Try < POLL_TRIES; ++Try) {
    if ((MmioRead32 (PWR_STATUS) & Mask) == Want &&
        (MmioRead32 (PWR_STATUS_2ND) & Mask) == Want) return TRUE;
    MicroSecondDelay (POLL_US);
  }
  return FALSE;
}

// __gpufreq_mfgN_control(POWER_ON) without the stock driver's ignored timeouts.
STATIC BOOLEAN
MfgOn (UINTN N)
{
  UINTN Con = MFG_PWR_CON (N);
  MmioOr32 (Con, PWR_ON);
  MmioOr32 (Con, PWR_ON_2ND);
  if (!WaitBoth (MFG_PWR_STA (N), MFG_PWR_STA (N))) return FALSE;
  MicroSecondDelay (100);
  MmioAnd32 (Con, ~(UINT32)PWR_CLK_DIS);
  MmioAnd32 (Con, ~(UINT32)PWR_ISO);
  MmioOr32 (Con, PWR_RST_B);
  MmioAnd32 (Con, ~(UINT32)SRAM_PDN);
  return Wait (Con, SRAM_PDN_ACK, 0);
}

STATIC UINT32
MfgPllCon1 (VOID)
{
  // Freq = PCW * 26 MHz / 2^14 / 2^posdiv; VCO 1560 MHz for 390 MHz.
  UINT32 Pcw = (UINT32)(((UINT64)GPU_KHZ * (1U << GPU_POSDIV) << 14) / 26000U);
  return (GPU_POSDIV << 24) | (Pcw & 0x3FFFFFU);
}

STATIC UINT32
PowerOn (MTK_PMIC_PROTOCOL *Pmic)
{
  BOOLEAN On;
  UINT32 Uv;
  UINT32 Pll;
  UINTN Try;

  // Step 2: voltages first, keeping VSRAM_GPU at or above VGPU in between
  // (the stock __gpufreq_volt_scale_gpu order).
  if (Pmic == NULL || EFI_ERROR (Rail (Pmic, VGPU_NAME, &On, &Uv))) return 2;
  if (VGPU_UV < Uv) {
    if (EFI_ERROR (RailSet (Pmic, VGPU_NAME, VGPU_UV)) ||
        EFI_ERROR (RailSet (Pmic, VSRAM_NAME, VSRAM_UV))) return 2;
  } else {
    if (EFI_ERROR (RailSet (Pmic, VSRAM_NAME, VSRAM_UV)) ||
        EFI_ERROR (RailSet (Pmic, VGPU_NAME, VGPU_UV))) return 2;
  }

  // Step 3: VGPU then VSRAM_GPU on, the T70 module's order, then settle.
  if (EFI_ERROR (RailOn (Pmic, VGPU_NAME)) || EFI_ERROR (RailOn (Pmic, VSRAM_NAME))) return 3;
  MicroSecondDelay (1000);
  if (EFI_ERROR (Rail (Pmic, VGPU_NAME, &On, &Uv)) || !On || Uv != VGPU_UV) return 3;
  if (EFI_ERROR (Rail (Pmic, VSRAM_NAME, &On, &Uv)) || !On || Uv != VSRAM_UV) return 3;

  // Steps 4 to 8: MFG0, MFG1 and its bus protection, the mfg_ref_sel gate
  // (clk_ref_mux), MFG2, MFG3.
  if (!MfgOn (0)) return 4;
  if (!MfgOn (1)) return 5;
  MmioWrite32 (PROTECTEN_2_CLR, MFG1_PROT_STEP2_1);
  MmioWrite32 (PROTECTEN_CLR, MFG1_PROT_STEP2_0);
  MmioWrite32 (PROTECTEN_2_CLR, MFG1_PROT_STEP1_1);
  MmioWrite32 (PROTECTEN_1_CLR, MFG1_PROT_STEP1_0);
  MmioWrite32 (CLK_CFG_4_CLR, MFG_REF_PDN);
  MmioWrite32 (CLK_CFG_UPDATE, MFG_REF_UPD);
  if (!MfgOn (2)) return 6;
  if (!MfgOn (3)) return 7;
  if ((MmioRead32 (PWR_STATUS) & MFG_ALL_STA) != MFG_ALL_STA ||
      (MmioRead32 (PWR_STATUS_2ND) & MFG_ALL_STA) != MFG_ALL_STA) return 8;

  // Step 9: MFGPLL at 390 MHz. Linux enables it when mfg_pll_sel takes it as
  // parent; change the rate only while mfg_pll_sel still points at mfg_ref.
  if ((MmioRead32 (CLK_CFG_4) & MFG_PLL_SEL_MFGPLL) != 0) {
    MmioWrite32 (CLK_CFG_4_CLR, MFG_PLL_SEL_MFGPLL);
  }
  Pll = MfgPllCon1 ();
  if ((MmioRead32 (MFGPLL_CON0) & PLL_EN) == 0) {
    MmioWrite32 (MFGPLL_CON1, PCW_CHG | Pll);            // mtk_pll_set_rate_regs
    MmioOr32 (MFGPLL_CON3, PLL_PWR_ON);                  // mtk_pll_prepare
    MicroSecondDelay (1);
    MmioAnd32 (MFGPLL_CON3, ~(UINT32)PLL_ISO_EN);
    MicroSecondDelay (1);
    MmioOr32 (MFGPLL_CON0, PLL_EN);
    MicroSecondDelay (20);
  } else if ((MmioRead32 (MFGPLL_CON1) & 0x073FFFFFU) != Pll) {
    MmioWrite32 (MFGPLL_CON1, PCW_CHG | Pll);
    MicroSecondDelay (20);
  }
  if ((MmioRead32 (MFGPLL_CON0) & PLL_EN) == 0 ||
      (MmioRead32 (MFGPLL_CON1) & 0x073FFFFFU) != Pll) return 9;

  // Step 10: __gpufreq_clock_control(POWER_ON) of the T70 module:
  // mfg_pll_sel to MFGPLL, retried until CLK_CFG_4[18] shows it, then the
  // BG3D gate and __gpufreq_external_cg_control.
  for (Try = 0; Try < MUX_TRIES && (MmioRead32 (CLK_CFG_4) & MFG_PLL_SEL_MFGPLL) == 0; ++Try) {
    MmioWrite32 (CLK_CFG_4_CLR, MFG_PLL_SEL_MFGPLL);
    MmioWrite32 (CLK_CFG_4_SET, MFG_PLL_SEL_MFGPLL);
    MicroSecondDelay (10);
  }
  if ((MmioRead32 (CLK_CFG_4) & MFG_PLL_SEL_MFGPLL) == 0) return 10;
  MmioWrite32 (MFG_CG_CLR, MFG_CG_BG3D);
  MmioOr32 (MFG_ASYNC_CON, BIT22 | BIT23 | BIT24 | BIT25);
  MmioAnd32 (MFG_GLOBAL_CON, ~(UINT32)(BIT9 | BIT10));
  MmioOr32 (MFG_QCHANNEL_CON, BIT4);
  MmioAndThenOr32 (MFG_GLOBAL_CON, ~(UINT32)BIT8, BIT19);
  MmioOr32 (MFG_ASYNC_CON_1, BIT0);
  MmioOr32 (MFG_I2M_PROTECTOR, BIT0);
  if ((MmioRead32 (MFG_CG_STA) & MFG_CG_BG3D) != 0 ||
      (MmioRead32 (CLK_CFG_4) & MFG_REF_PDN) != 0) return 10;

  // Step 11 is the GPU_ID check in the caller. The T70 module then enables
  // the GPU timestamp from the SoC timer.
  MmioWrite32 (MFG_TIMESTAMP, 0x3U);
  return 0;
}
#endif

STATIC VOID EFIAPI
OnReadyToBoot (EFI_EVENT Event, VOID *Context)
{
  MTK_PMIC_PROTOCOL *Pmic = NULL;
  UINT32 Result = RES_SURVEY;
  UINT32 Status;

  (VOID)Event;
  (VOID)Context;
  if (mDone) return;
  mDone = TRUE;

  if (EFI_ERROR (MapMmio (MFG_TOP, SIZE_4KB)) || EFI_ERROR (MapMmio (GPU, GPU_BYTES))) {
    Rec (0x08, RES_FAILED | 1U);
    return;
  }
  if (EFI_ERROR (gBS->LocateProtocol (&gMediaTekPmicProtocolGuid, NULL, (VOID **)&Pmic))) Pmic = NULL;

  // Survey: what the preloader left. MFG_TOP and the GPU are read only once
  // their power domains are on, as reading them unpowered can stall the bus.
  Status = MmioRead32 (PWR_STATUS);
  Rec (0x10, Status);
  Rec (0x14, MmioRead32 (PWR_STATUS_2ND));
  Rec (0x18, MmioRead32 (MFGPLL_CON0));
  Rec (0x1C, MmioRead32 (MFGPLL_CON1));
  Rec (0x20, MmioRead32 (MFGPLL_CON3));
  Rec (0x24, MmioRead32 (CLK_CFG_4));
  Rec (0x28, RailWord (Pmic, VGPU_NAME));
  Rec (0x2C, RailWord (Pmic, VSRAM_NAME));

#if T70_GPU_POWER_ON
  {
    UINT32 Failed = PowerOn (Pmic);
    Rec (0x0C, Failed);
    Status = MmioRead32 (PWR_STATUS);
    Rec (0x34, Status);
    Rec (0x38, MmioRead32 (MFGPLL_CON1));
    Rec (0x40, MmioRead32 (CLK_CFG_4));
    Rec (0x44, RailWord (Pmic, VGPU_NAME));
    Rec (0x48, RailWord (Pmic, VSRAM_NAME));
    if (Failed != 0) {
      Result = RES_FAILED | Failed;
    } else {
      UINT32 Id = MmioRead32 (GPU);
      Rec (0x3C, Id);
      Rec (0x4C, MmioRead32 (MFG_CG_STA));
      // Mali-G57 tNAx: arch 9.0, product 3 (Mesa PAN_PROD_ID(9, 0, 3)).
      /* arch 9.0, product 3 (Mali-G57); the MT6789 reads 0x9093xxxx. */
      Result = ((Id >> 16) & 0xFF0FU) == 0x9003U ? RES_POWERED : (RES_FAILED | 11U);
      if (Result != RES_POWERED) Rec (0x0C, 11U);
    }
  }
#endif
  Rec (0x08, Result);
  // ACPI reads the record through its own mapping.
  WriteBackDataCacheRange ((VOID *)REC, REC_BYTES);
  DEBUG ((DEBUG_INFO, "T70 GPU: result 0x%x PWR_STATUS 0x%x\n", Result, Status));
}

EFI_STATUS EFIAPI
GpuPowerDxeEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  (VOID)ImageHandle;
  (VOID)SystemTable;
  // No previous boot's record may survive into this one.
  for (UINTN Offset = 0; Offset < REC_BYTES; Offset += 4U) Rec (Offset, 0);
  Rec (0x00, REC_MAGIC);
  Rec (0x04, REC_VERSION | (T70_GPU_POWER_ON ? BIT8 : 0));
  Rec (0x08, RES_SURVEY);
  WriteBackDataCacheRange ((VOID *)REC, REC_BYTES);
  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL,
                             &gEfiEventReadyToBootGuid, &mReadyToBoot);
}
