/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_UFS_POWER_CORE_H_
#define MTK_UFS_POWER_CORE_H_

#include <stdint.h>
#include <Library/MtkUfsPower.h>

#define MTK_UFS_POWER_CORE_SIGNATURE  0x55504643U
#define MTK_UFS_POWER_GATE_COUNT       5U
#define MTK_UFS_POWER_ROOT_COUNT       10U

typedef enum {
  MtkUfsPowerBankHci = 0,
  MtkUfsPowerBankInfra,
  MtkUfsPowerBankTop,
  MtkUfsPowerBankPwrap
} MTK_UFS_POWER_BANK;

typedef enum {
  MtkUfsPowerIoOk = 0,
  MtkUfsPowerIoBusy,
  MtkUfsPowerIoFailed
} MTK_UFS_POWER_IO_STATUS;

/* Phase is public so a host harness can assert that repeated calls advance the
 * same transaction instead of reissuing an accepted SMC, UIC or WACS command. */
typedef enum {
  MtkUfsPowerPhaseIdle = 0,
  MtkUfsPowerPhaseQueryVemcEnable,
  MtkUfsPowerPhaseQueryVemcVoltage,
  MtkUfsPowerPhaseQueryHardware,
  MtkUfsPowerPhasePrepareCheck,
  MtkUfsPowerPhasePrepareHibernate,
  MtkUfsPowerPhasePrepareHibernateProbe,
  MtkUfsPowerPhasePrepareHibernateWait,
  MtkUfsPowerPhasePreparePowerdown,
  MtkUfsPowerPhaseReleaseRoots,
  MtkUfsPowerPhaseReleaseVemc,
  MtkUfsPowerPhaseReleaseRefSmcPre,
  MtkUfsPowerPhaseReleaseRefDelayPre,
  MtkUfsPowerPhaseReleaseRefWrite,
  MtkUfsPowerPhaseReleaseRefAck,
  MtkUfsPowerPhaseReleaseRefSmcPost,
  MtkUfsPowerPhaseReleaseGateRead,
  MtkUfsPowerPhaseReleaseGateWrite,
  MtkUfsPowerPhaseReleaseGateVerify,
  MtkUfsPowerPhaseRestoreRoots,
  MtkUfsPowerPhaseRestoreVemc,
  MtkUfsPowerPhaseRestoreGateRead,
  MtkUfsPowerPhaseRestoreGateWrite,
  MtkUfsPowerPhaseRestoreGateVerify,
  MtkUfsPowerPhaseRestoreResetAssert,
  MtkUfsPowerPhaseRestoreResetRelease,
  MtkUfsPowerPhaseRestoreRefSmcPre,
  MtkUfsPowerPhaseRestoreRefWrite,
  MtkUfsPowerPhaseRestoreRefAck,
  MtkUfsPowerPhaseRestoreRefDelayPost,
  MtkUfsPowerPhaseRestoreRefSmcPost,
  MtkUfsPowerPhaseRestorePreHce,
  MtkUfsPowerPhaseRestoreHce,
  MtkUfsPowerPhaseRestorePostHce,
  MtkUfsPowerPhaseRestorePreLink,
  MtkUfsPowerPhaseRestoreLinkStartup,
  MtkUfsPowerPhaseRestorePostLink,
  MtkUfsPowerPhaseFault,
  MtkUfsPowerPhaseRestoreRefOffCheck,
  MtkUfsPowerPhaseRestoreRefOffSmcPre,
  MtkUfsPowerPhaseRestoreRefOffDelayPre,
  MtkUfsPowerPhaseRestoreRefOffWrite,
  MtkUfsPowerPhaseRestoreRefOffAck,
  MtkUfsPowerPhaseRestoreRefOffSmcPost,
  MtkUfsPowerPhaseRestoreRefOnCheck
} MTK_UFS_POWER_PHASE;

typedef struct {
  void *Context;
  int (*TryLock)(void *Context);
  void (*Unlock)(void *Context);
  MTK_UFS_POWER_IO_STATUS (*Read32)(void *Context, MTK_UFS_POWER_BANK Bank, uint32_t Offset, uint32_t *Value);
  MTK_UFS_POWER_IO_STATUS (*Write32)(void *Context, MTK_UFS_POWER_BANK Bank, uint32_t Offset, uint32_t Value);
  /* Command/value/stage are fixed by the core; callers cannot select an FID. */
  MTK_UFS_POWER_IO_STATUS (*Smc)(void *Context, uint32_t Command, uint32_t Value, uint32_t Stage, int64_t *RawResult);
  uint64_t (*NowUs)(void *Context);
} MTK_UFS_POWER_CORE_IO;

/* Contains scalar progress only. IO callbacks, runtime addresses and Boot
 * Services pointers are supplied afresh to every Handle call. */
typedef struct {
  uint32_t Signature;
  uint32_t State;
  uint32_t Phase;
  uint32_t Operation;
  uint32_t RootIndex;
  uint32_t GateIndex;
  uint32_t GateOwnedMask;
  uint32_t GateObservedMask;
  uint32_t IpVersion;
  uint32_t WacsStage;
  uint32_t WacsAddress;
  uint32_t WacsValue;
  uint32_t WacsPollCount;
  uint32_t UicPending;
  uint32_t UicCommand;
  uint32_t UicArg1;
  uint32_t UicArg2;
  uint32_t UicArg3;
  uint32_t UicInputArg3;
  uint32_t UicResultArg3;
  uint32_t UicPollCount;
  uint32_t PhasePollCount;
  uint32_t SmcPending;
  uint32_t SmcCommand;
  uint32_t SmcValue;
  uint32_t SmcStage;
  uint32_t ResetIndex;
  uint32_t RestoreOriginState;
  uint32_t QueryStopProof;
  uint32_t QueryVemcGood;
  uint32_t LastError;
  uint64_t Generation;
  uint64_t TransactionGeneration;
  uint64_t WacsStartedUs;
  uint64_t UicStartedUs;
  uint64_t PhaseStartedUs;
  uint64_t NextPollUs;
} MTK_UFS_POWER_CORE;

void MtkUfsPowerCoreInitialize(MTK_UFS_POWER_CORE *Core);
MTK_UFS_POWER_RESULT MtkUfsPowerCoreHandle(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  MTK_UFS_POWER_REQUEST *Request
  );

#endif
