// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_VCN28_CORE_H
#define MTK_VCN28_CORE_H
/* MT6358 VCN28 policy for the T70Pro effective DT: fixed 2.8V, always-on,
 * 270us enable delay. ADIE 0x6635 bypasses this rail in shipped WMT code.
 * The RF owner supplies a runtime-probed ADIE ID; zero/0xffff are not IDs.
 * This is not conninfra boot, RF calibration or a positioning engine. */
#define MTK_VCN28_FM 1U
#define MTK_VCN28_GPS 2U
#define MTK_VCN28_ENABLE 0x1d88U
#define MTK_VCN28_OP_ENABLE 0x1d8aU
#define MTK_VCN28_OP_CONFIG 0x1d90U
#define MTK_VCN28_STATUS 0x1d96U
#define MTK_VCN28_ENABLE_DELAY_US 270U
#define MTK_VCN28_WAIT_POLLS 100U
typedef enum {
  MtkVcn28Ok, MtkVcn28Invalid, MtkVcn28Busy, MtkVcn28Offline,
  MtkVcn28IoError, MtkVcn28Timeout, MtkVcn28Uncertain
} MTK_VCN28_STATUS_CODE;
typedef struct {
  void *Context;
  /* Bounded, serialized PMIC accesses, through the owner's existing PWRAP or
   * regmap provider. A failed write may already have reached the PMIC.
   * Read publishes Value only on success. No callback may reenter this core. */
  MTK_VCN28_STATUS_CODE (*Read16)(void *Context,unsigned Address,unsigned *Value);
  MTK_VCN28_STATUS_CODE (*Write16)(void *Context,unsigned Address,unsigned Value);
  MTK_VCN28_STATUS_CODE (*DelayUs)(void *Context,unsigned Microseconds);
  /* Exclusive PMIC mode/enable ownership, valid mappings/clocks and a stable
   * runtime ADIE identity. Rechecked on every operation; no implicit power-on. */
  unsigned (*Authorized)(void *Context);
} MTK_VCN28_IO;
typedef struct {
  MTK_VCN28_IO Io;
  unsigned Initialized,AdieChipId,Users,Captured,Faulted,DelayPending;
  unsigned SavedEnable,SavedOpEnable,SavedOpConfig;
} MTK_VCN28;
/* Zero-initialized software object only. Init performs no IO. One shared RF
 * arbiter owns both bits; repeated acquire of one bit is idempotent, not a new
 * reference. The arbiter must also account for firmware auto-GPS/FM states and
 * other rail users before granting Authorized. Do not pass a guessed ADIE ID. */
MTK_VCN28_STATUS_CODE MtkVcn28Initialize(MTK_VCN28 *Rail,
  const MTK_VCN28_IO *Io,unsigned AdieChipId);
/* Acquire before GPS/FM pre-control and function-on. Release only after a
 * confirmed function-off, quiesced data/firmware work and released LNA usage.
 * An uncertain WMT result must retain this bit. Other users keep the rail and
 * mode lease. The last release restores inherited mode/select; the DT's
 * always-on rail stays enabled, including when acquired from an off state.
 * Errors retain snapshot/user ownership. New acquisition is blocked after an
 * error, but the serialized owner may retry an idempotent release after fixing
 * its PMIC transport. Never discard/reinitialize a live or failed object. */
MTK_VCN28_STATUS_CODE MtkVcn28Set(MTK_VCN28 *Rail,unsigned User,unsigned Enable);
#endif
