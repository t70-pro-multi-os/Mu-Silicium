/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_MFG_H
#define T70_MALI_MFG_H
#include "MaliJm.h"
enum {MaliMfgSpm=0,MaliMfgInfraAo=1};
#define MALI_MFG_DOMAINS 4u
#define MALI_MFG_WAIT_POLLS 2000u
typedef struct {
    void *Context;
    uint32_t (*Read32)(void *,uint32_t Region,uint32_t Offset);
    void (*Write32)(void *,uint32_t Region,uint32_t Offset,uint32_t Value);
    void (*Barrier)(void *);
    void (*DelayUs)(void *,uint32_t);
    /* Parent proves exclusive ownership, mapped/powered SPM/infra and stable
       rails. Domain 2/3 ON also requires the reference mux already enabled.
       OFF requires the GPU idle, its bus drained and board clocks sequenced
       exactly around these domain transitions. No permissive fallback. */
    int (*Authorized)(void *,uint32_t Domain,uint32_t On);
} MALI_MFG_IO;
typedef struct {MALI_MFG_IO Io;uint32_t Initialized,Poisoned;} MALI_MFG;
/* Caller serializes all accesses, PASSIVE on Windows. These are the exact
   MT6789 stock SPM domains and MFG1 bus-protection transitions, not a PMIC,
   PLL/mux/DVFS, full board-power driver, address-ownership or IRQ proof. */
MALI_RESULT MaliMfgInitialize(MALI_MFG *,const MALI_MFG_IO *);
MALI_RESULT MaliMfgSetDomain(MALI_MFG *,uint32_t Domain,uint32_t On);
/* Checks all four domains, SRAM/control state and bus-protection release.
   Parent must separately prove live clocks/voltage and exclusive GPU ownership
   before using this as one part of MaliJm's PowerReady contract. */
MALI_RESULT MaliMfgAllDomainsReady(MALI_MFG *);
/* Stock gpufreq clock sequence. The provider implements actual clock prepare,
   enable and parent operations; this is not a Windows clock-provider shim. */
enum {MaliMfgTop=2,MaliMfgTopckgen=3};
enum {MaliMfgMuxClock=0,MaliMfgBg3dClock=1};
enum {MaliMfgMainParent=0,MaliMfgSubParent=1};
enum {MaliMfgMuxPrepared=1,MaliMfgMuxEnabled=2,MaliMfgMainSelected=4,
      MaliMfgBgPrepared=8,MaliMfgBgEnabled=16,MaliMfgConfigured=32,
      MaliMfgClocksOn=63};
typedef struct {
    MALI_MFG_IO Registers;
    int (*Prepare)(void *,uint32_t Clock);
    int (*Enable)(void *,uint32_t Clock);
    int (*Disable)(void *,uint32_t Clock);
    int (*Unprepare)(void *,uint32_t Clock);
    int (*SetParent)(void *,uint32_t Parent);
} MALI_MFG_CLOCK_IO;
typedef struct {MALI_MFG_CLOCK_IO Io;uint32_t Initialized,Poisoned,Stages;} MALI_MFG_CLOCK;
/* Fresh zeroed object only. Never reinitialize a live/poisoned object to discard
   retained clock ownership. Caller serializes, at PASSIVE on Windows.
   Registers.Authorized(...,3,On) must prove stable rails, all SPM domains,
   reference clock, exclusive mapped MFG/TOPCKGEN access; OFF also requires
   GPU idle and bus drain. No rail/PLL/DVFS policy is inferred by this API. */
MALI_RESULT MaliMfgClockInitialize(MALI_MFG_CLOCK *,const MALI_MFG_CLOCK_IO *);
MALI_RESULT MaliMfgClockSet(MALI_MFG_CLOCK *,uint32_t On);
/* Partial clock/configuration proof only, never a full board PowerReady. */
MALI_RESULT MaliMfgClockReady(MALI_MFG_CLOCK *);
/* Complete stock ON/OFF sequencing over the implemented domain/clock cores.
   These are provider operations, not a replacement PMIC/PLL driver. Rail 0/1
   retain the exact g_pmic[] order; no voltage identity or DVFS value is guessed.
   Every provider callback returns zero on success, including release operations.
   All callbacks are serialized at PASSIVE. Failed operations retain the object
   and their resources until a separately proven board recovery. */
enum {MaliMfgPowerRail0=1,MaliMfgPowerRail1=2,MaliMfgPowerDomain0=4,
      MaliMfgPowerDomain1=8,MaliMfgPowerRefPrepared=16,MaliMfgPowerRefEnabled=32,
      MaliMfgPowerDomain2=64,MaliMfgPowerDomain3=128,MaliMfgPowerClocks=256,
      MaliMfgPowerDcm=512,MaliMfgPowerOn=1023};
typedef struct {
    MALI_MFG_IO Domains;
    MALI_MFG_CLOCK_IO Clocks;
    /* Same Context and register accessors for both cores; authorization remains
       separate because domain 3 ON precedes the full-domain clock proof. */
    int (*Authorized)(void *,uint32_t On);
    int (*RailSet)(void *,uint32_t Rail,uint32_t On);
    int (*ReferencePrepare)(void *);
    int (*ReferenceEnable)(void *);
    int (*ReferenceDisable)(void *);
    int (*ReferenceUnprepare)(void *);
} MALI_MFG_POWER_IO;
typedef struct {
    MALI_MFG_POWER_IO Io;
    MALI_MFG Domains;
    MALI_MFG_CLOCK Clocks;
    uint32_t Initialized,Poisoned,Stages,References;
} MALI_MFG_POWER;
/* Fresh zeroed object. Parent authorization proves exclusive mapped ownership
   and validated rails/reference providers; final OFF additionally requires no
   pending GPU work. Internal bus-idle polling is bounded. */
MALI_RESULT MaliMfgPowerInitialize(MALI_MFG_POWER *,const MALI_MFG_POWER_IO *);
MALI_RESULT MaliMfgPowerReference(MALI_MFG_POWER *,uint32_t On);
/* Partial power-sequence proof. GPU address/coherency/IRQ ownership and actual
   PMIC/PLL/provider implementation remain separate board requirements. */
MALI_RESULT MaliMfgPowerSequenceReady(MALI_MFG_POWER *);
/* One exclusive PMIC transport is selected: the raw WACS2 port below, or a
   status-bearing transaction provider such as the shared PRM broker. Mixed or
   partial transports are rejected; no fallback crosses ownership domains. */
#define MALI_MFG_PMIC_WAIT_POLLS 1000u
typedef struct {
    void *Context;
    /* Raw WACS2 registers are valid only when both broker fields are NULL. */
    uint32_t (*Read32)(void *,uint32_t Offset);
    void (*Write32)(void *,uint32_t Offset,uint32_t Value);
    void (*Barrier)(void *);
    void (*DelayUs)(void *,uint32_t);
    int (*Authorized)(void *);
    /* Set these together only for a real shared transaction provider. */
    MALI_RESULT (*BrokerRead16)(void *,uint32_t Address,uint16_t *Value);
    MALI_RESULT (*BrokerWrite16)(void *,uint32_t Address,uint16_t Value);
} MALI_MFG_PMIC_IO;
typedef struct {MALI_MFG_PMIC_IO Io;uint32_t Initialized,Poisoned;} MALI_MFG_PMIC;
MALI_RESULT MaliMfgPmicInitialize(MALI_MFG_PMIC *,const MALI_MFG_PMIC_IO *);
/* Even 16-bit PMIC addresses only; output commits after response acknowledgement.
   A timeout poisons the transport. Reinitializing cannot discard an outstanding
   command. Writes wait for completion, unlike the stock queued-write helper. */
MALI_RESULT MaliMfgPmicRead16(MALI_MFG_PMIC *,uint32_t Address,uint16_t *Value);
MALI_RESULT MaliMfgPmicWrite16(MALI_MFG_PMIC *,uint32_t Address,uint32_t Value);
enum {MaliMfgVgpuRail=0,MaliMfgVsramGpuRail=1};
typedef struct {
    MALI_MFG_PMIC *Pmic;
    void *Context;
    /* Proves exclusive regulator ownership and live VSYS/always-on VS2 supply.
       ON additionally proves that the programmed selector is safe for the GPU
       OPP/PLL to be used; OFF proves the full GPU has been quiesced. */
    int (*Authorized)(void *,uint32_t Rail,uint32_t On);
} MALI_MFG_RAIL_IO;
typedef struct {
    MALI_MFG_RAIL_IO Io;
    uint32_t Initialized,Poisoned,Acquired,Owned,Microvolts[2],OriginalMicrovolts[2];
} MALI_MFG_RAILS;
/* The exact stock gpufreq order is VGPU then VSRAM_GPU; release is reversed.
   Existing rails are borrowed and never disabled. RailSet does not change a
   voltage selector, mode, VS2 or shared parent. Release refuses to drop either
   rail while any acquired borrowed rail has an unrestored original voltage.
   Voltage transitions use the
   separately authorized pair API below; OPP selection remains a board policy. */
MALI_RESULT MaliMfgRailsInitialize(MALI_MFG_RAILS *,const MALI_MFG_RAIL_IO *);
MALI_RESULT MaliMfgRailSet(MALI_MFG_RAILS *,uint32_t Rail,uint32_t On);
MALI_RESULT MaliMfgRailsReady(MALI_MFG_RAILS *);
/* Before any power-off transition, the board may use this in its OFF
   authorization. Every borrowed rail must have its original selector/DA/status
   restored while both rails are still held. No automatic voltage change or
   guessed rollback. Busy retains ownership and allows an approved restore. */
MALI_RESULT MaliMfgRailsReleaseReady(MALI_MFG_RAILS *);
/* All target AND transient rail pairs must be approved by the board's live
   OPP policy. The exclusive regulator/PWRAP lease stays held for the whole
   operation. Approving the final pair alone cannot authorize the first write. */
typedef struct {
    void *Context;
    int (*Authorized)(void *,uint32_t VgpuUv,uint32_t VsramUv);
} MALI_MFG_VOLTAGE_IO;
/* Both rails must already be acquired and stable. Same-direction VGPU/VSRAM
   steps only; a board springboard policy splits larger/unsafe steps. Increasing
   writes VSRAM first, decreasing writes VGPU first, then the exact stock delay.
   A failed pair never rolls back or discards ownership. Saved voltages commit
   only after both selector/DA/status checks. Borrowed rails also require the
   exclusive voltage lease; their original OPP must be restored before handoff. */
MALI_RESULT MaliMfgRailsVoltage(MALI_MFG_RAILS *,uint32_t VgpuUv,uint32_t VsramUv,
                               const MALI_MFG_VOLTAGE_IO *);
/* Actual MT6789 clock-register provider. The parent grants an exclusive lease
   for the selected GPU clock and proves its mapped/powered resources, stable
   26MHz root and voltage sufficient for the unchanged programmed PLL setting.
   OFF additionally proves GPU idle. MAINPLL is shared and is only inspected. */
enum {MaliMfgApmixed=4};
enum {MaliMfgHwReference=0,MaliMfgHwPll=1,MaliMfgHwBg3d=2,MaliMfgHwParent=3};
typedef struct {
    void *Context;
    uint32_t (*Read32)(void *,uint32_t Region,uint32_t Offset);
    void (*Write32)(void *,uint32_t Region,uint32_t Offset,uint32_t Value);
    void (*Barrier)(void *);
    void (*DelayUs)(void *,uint32_t);
    int (*Authorized)(void *,uint32_t Clock,uint32_t On);
} MALI_MFG_HW_CLOCK_IO;
typedef struct {
    MALI_MFG_HW_CLOCK_IO Io;
    uint32_t Initialized,Poisoned,Held,PllSetting,RefSetting;
} MALI_MFG_HW_CLOCK;
/* Decodes the exact stock 26MHz,22-bit PCW/14 fractional-bit,3-bit post-divider
   calculation with upward rounding. Pure conversion, not voltage/OPP approval. */
MALI_RESULT MaliMfgPllDecodeRate(uint32_t Setting,uint64_t *Hz);
/* Pure exact MFG descriptor conversion: nearest PCW, smallest 0..4 divider
   reaching 1.5GHz VCO, 3.8GHz ceiling. ActualHz includes decoder rounding and
   can exceed RequestedHz. Invalid requests leave both outputs untouched. */
MALI_RESULT MaliMfgPllEncodeRate(uint64_t RequestedHz,uint32_t *Setting,uint64_t *ActualHz);
typedef struct {
    void *Context;
    int (*Authorized)(void *,uint64_t OldHz,uint64_t RequestedHz,uint64_t ActualHz);
} MALI_MFG_RATE_IO;
/* Program only with reference held, BG3D gated, mux on Sub and MFGPLL off and
   isolated. This deliberately avoids live PLL changes/FHCTL and requires a board
   lease approving the requested AND rounded rate at the current rail voltage.
   Uses stock PCW/postdivider/change-strobe writes and 20us settle. On failure
   retain reference ownership and poison; never continue into GPU clock enable. */
MALI_RESULT MaliMfgHwClockProgramRate(MALI_MFG_HW_CLOCK *,uint64_t RequestedHz,
                                    const MALI_MFG_RATE_IO *);
MALI_RESULT MaliMfgHwClockInitialize(MALI_MFG_HW_CLOCK *,const MALI_MFG_HW_CLOCK_IO *);
/* Set adopts an authorized exclusive clock lease, including inherited state.
   Only acquired clocks may be released. No PLL frequency or reference selector
   is programmed; stale/invalid configuration and failed writes are rejected. */
MALI_RESULT MaliMfgHwClockSet(MALI_MFG_HW_CLOCK *,uint32_t Clock,uint32_t On);
/* Main selects/prepares MFGPLL, Sub selects the reference then releases MFGPLL.
   BG3D must be gated before reparenting. No nonexistent mux update is written. */
MALI_RESULT MaliMfgHwClockParent(MALI_MFG_HW_CLOCK *,uint32_t Parent);
MALI_RESULT MaliMfgHwClockReady(MALI_MFG_HW_CLOCK *);

/* One production composition of the real domain, WACS2/rail and hardware-clock
   cores. Registers covers SPM/infra/MFG/TOPCKGEN/APMIXED for this same GPU.
   The caller already holds the exclusive mapped-resource, PMIC bus/regulator
   and clock leases for the entire serialized PASSIVE call. Authorized proves
   those leases and, for OFF, stopped GPU/DMA/IRQs/worker and drained callbacks.
   OperatingPoint approves the actual decoded PLL rate and both live selector
   voltages using this device's qualified OPP and thermal/battery constraints.
   It is checked before the first rail enable and again before reporting ON.
   No default OPP, selector/rate programming or permissive policy is supplied. */
typedef struct {
    MALI_MFG_IO Registers;
    MALI_MFG_RAILS *Rails;
    MALI_MFG_HW_CLOCK *Clock;
    int (*Authorized)(void *,uint32_t On);
    int (*OperatingPoint)(void *,uint64_t Hz,uint32_t VgpuUv,uint32_t VsramUv);
} MALI_MFG_CONTROLLER_IO;
enum {MaliMfgControllerUnknown=0,MaliMfgControllerOn=1,MaliMfgControllerOff=2};
typedef struct {
    MALI_MFG_CONTROLLER_IO Io;
    MALI_MFG_POWER Power;
    uint32_t Initialized,Poisoned,State,Prepared,Enabled,ReferencePrepared,ReferenceEnabled;
    MALI_RESULT LastResult;
} MALI_MFG_CONTROLLER;
/* Fresh disjoint controller/config and initialized, unacquired rail/clock
   objects. All objects and immutable callbacks outlive the controller. Logical
   prepare/enable references share the already leased physical mux; reference
   gate, PLL, parent and BG3D operations always use the actual clock core.
   Set is idempotent, not a public reference counter. OFF from an unestablished
   initial state is refused. Borrowed rail restoration is preflighted BEFORE
   bus drain/clock/domain shutdown. Partial mutations preserve child journals
   and poison the controller; never zero/reinitialize to discard ownership.
   OS QUERY_INTERFACE/resource ownership and physical qualification are separate. */
MALI_RESULT MaliMfgControllerInitialize(MALI_MFG_CONTROLLER *,const MALI_MFG_CONTROLLER_IO *);
MALI_RESULT MaliMfgControllerSet(MALI_MFG_CONTROLLER *,uint32_t On);
/* OFF/unknown returns before MMIO. This is the hardware-sequence/OPP portion
   of board Ready, not an address/cache/IRQ association proof. */
MALI_RESULT MaliMfgControllerReady(MALI_MFG_CONTROLLER *);

/* The parent supplies this device's qualified, adjusted working table under
   an exclusive GPU-idle/regulator/clock lease. Default ROM tables are never
   silently installed as policy. Thermal/battery limits and AVS/aging/segment
   qualification remain parent duties. Fresh zeroed storage; all objects and
   the immutable policy outlive the serialized transition. */
#define MALI_MFG_OPP_MAX 64u
#define MALI_MFG_OPP_MAX_STEPS (MALI_MFG_OPP_MAX+1u)
#define MALI_MFG_RAIL_GAP_UV 200000u
#define MALI_MFG_OPP_SIGNATURE 0x4f505039u
typedef struct {uint64_t Hz;uint32_t VgpuUv,VsramUv;} MALI_MFG_OPP;
typedef struct {
    uint32_t Signature,Count;
    MALI_MFG_OPP Entries[MALI_MFG_OPP_MAX];
    uint64_t ActualHz[MALI_MFG_OPP_MAX];
    uint32_t Up[MALI_MFG_OPP_MAX],Down[MALI_MFG_OPP_MAX];
} MALI_MFG_OPP_POLICY;
enum {MaliMfgOppVoltage=1,MaliMfgOppRate=2};
typedef struct {uint32_t Kind,Index;uint64_t Hz;uint32_t VgpuUv,VsramUv;} MALI_MFG_OPP_STEP;
typedef struct {
    uint32_t Count,Initial,Target,Reserved;
    MALI_MFG_OPP_STEP Steps[MALI_MFG_OPP_MAX_STEPS];
} MALI_MFG_OPP_PLAN;
/* Exact stock 200mV springboards over a descending adjusted table. Rejects
   malformed/unsafe/unreachable tables and CPU aliases before writing policy.
   Planning is pure, bounded and leaves output untouched on failure. The
   original equal-voltage plateaus are preserved; no artificial voltage step. */
MALI_RESULT MaliMfgOppBuild(MALI_MFG_OPP_POLICY *,const MALI_MFG_OPP *,uint32_t Count);
/* Pure shipped MT6789 AVS/segment/aging calculation. Inputs are an immutable
   unadjusted 45-point table and three successfully supplied AVS words, including
   explicit zero words. The trusted platform owns acquiring/qualifying those
   inputs; this API does not read fuses, claim device ownership or approve OPPs.
   Never use the probe's test/fallback fuse constants as device calibration. */
#define MALI_MFG_STOCK_OPP_COUNT 45u
typedef struct {
    uint32_t Size,Version,Segment,Mcl50Load,Aging,PresentMask,AvsWords[3];
} MALI_MFG_CALIBRATION;
/* Builds the policy only after all conversions and validations succeed; inputs
   and output must be disjoint. Segment 1 or MCL50 removes the top seven points; only
   segment 2 without MCL50 keeps all 45. Aging is a one-time transformation
   of the unadjusted input, never a repeated mutation of an installed policy.
   Caller serializes the source and keeps per-device inputs out of logs. */
MALI_RESULT MaliMfgOppCalibrate(MALI_MFG_OPP_POLICY *,const MALI_MFG_OPP *,
    uint32_t Count,const MALI_MFG_CALIBRATION *);

MALI_RESULT MaliMfgOppPlan(const MALI_MFG_OPP_POLICY *,uint32_t Initial,uint32_t Target,MALI_MFG_OPP_PLAN *);
typedef struct {
    void *Context;
    /* Proves device-qualified table, current thermal/battery limits, whole-GPU
       idle and all shared resource leases for the complete transition. */
    int (*Authorized)(void *,uint32_t Initial,uint32_t Target);
    MALI_MFG_VOLTAGE_IO Voltage;
    MALI_MFG_RATE_IO Rate;
} MALI_MFG_DVFS_IO;
typedef struct {
    uint32_t Initialized,Poisoned,Current,Target,Completed;
    const MALI_MFG_OPP_POLICY *Policy;
    MALI_MFG_RAILS *Rails;
    MALI_MFG_HW_CLOCK *Clock;
    MALI_MFG_DVFS_IO Io;
    MALI_MFG_OPP_PLAN Plan;
} MALI_MFG_DVFS;
/* Real pair and stopped-PLL cores are used, not callbacks pretending to set
   hardware. Rate changes gate BG3D, move to Sub, stop/isolate MFGPLL, program,
   return to Main, and ungate. Every write/readback still requires its existing
   provider authorization. No queued GPU job may exist during this sequence.
   A partial/uncertain transition poisons the state and preserves rails/clocks,
   last complete step and target. Never zero/reinitialize it to discard fault. */
MALI_RESULT MaliMfgDvfsInitialize(MALI_MFG_DVFS *,const MALI_MFG_OPP_POLICY *,
    MALI_MFG_RAILS *,MALI_MFG_HW_CLOCK *,const MALI_MFG_DVFS_IO *,uint32_t Initial);
MALI_RESULT MaliMfgDvfsCommit(MALI_MFG_DVFS *,uint32_t Target);
#endif
