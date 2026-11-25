// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_HUSB311_TYPEC_CORE_H
#define MTK_HUSB311_TYPEC_CORE_H

typedef unsigned char      MTK_HUSB311_U8;
typedef unsigned short     MTK_HUSB311_U16;
typedef unsigned int       MTK_HUSB311_U32;
typedef unsigned long long MTK_HUSB311_TOKEN;

typedef enum {
  MtkHusb311TypecSuccess = 0,
  MtkHusb311TypecInvalid,
  MtkHusb311TypecNotReady,
  MtkHusb311TypecIoError,
  MtkHusb311TypecConflict,
  MtkHusb311TypecTimeout,
  MtkHusb311TypecBusy,
  MtkHusb311TypecStaleToken,
  MtkHusb311TypecQuarantined,
  MtkHusb311TypecPending
} MTK_HUSB311_TYPEC_STATUS;

typedef struct {
  /* Fresh board state; MUSB session is the raw DEVCTL register value. */
  MTK_HUSB311_U8 PowerBusLeaseHeld;
  MTK_HUSB311_U8 TcpcInitialized;
  MTK_HUSB311_U8 CcDetectionEnabled;
  MTK_HUSB311_U8 ScBoostEnabled;
  MTK_HUSB311_U8 HusbVbusSafe0V;
  MTK_HUSB311_U8 MusbDevCtl;
  MTK_HUSB311_U8 MusbControllerIdle;
  /* An independent board mux already owns the controller role. */
  MTK_HUSB311_U8 IndependentControllerRole;
} MTK_HUSB311_TYPEC_GUARDS;

typedef MTK_HUSB311_TYPEC_STATUS (*MTK_HUSB311_TYPEC_READ_REGISTERS)(
  void *Context,
  MTK_HUSB311_U8 FirstRegister,
  MTK_HUSB311_U8 Length,
  MTK_HUSB311_U8 *Bytes
  );

/* ROLE/RX/HEADER use masked updates. VENDOR_ALERT accepts only mask=value=02:
 * write the literal W1C mask, never a read/modify/write of the alert byte. */
typedef MTK_HUSB311_TYPEC_STATUS (*MTK_HUSB311_TYPEC_UPDATE_REGISTER)(
  void *Context,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 Mask,
  MTK_HUSB311_U8 Value
  );

typedef MTK_HUSB311_TYPEC_STATUS (*MTK_HUSB311_TYPEC_DELAY_MS)(
  void *Context,
  MTK_HUSB311_U32 Milliseconds
  );

typedef MTK_HUSB311_TYPEC_STATUS (*MTK_HUSB311_TYPEC_READ_GUARDS)(
  void *Context,
  MTK_HUSB311_TYPEC_GUARDS *Guards
  );

typedef struct {
  void *Context;
  MTK_HUSB311_TYPEC_READ_REGISTERS ReadRegisters;
  MTK_HUSB311_TYPEC_UPDATE_REGISTER UpdateRegister;
  MTK_HUSB311_TYPEC_DELAY_MS DelayMs;
  MTK_HUSB311_TYPEC_READ_GUARDS ReadGuards;
} MTK_HUSB311_TYPEC_IO;

typedef enum {
  MtkHusb311TypecStateUninitialized = 0,
  MtkHusb311TypecStateIdle,
  MtkHusb311TypecStatePreparing,
  MtkHusb311TypecStateOwned,
  MtkHusb311TypecStateCleaning,
  MtkHusb311TypecStateQuarantined
} MTK_HUSB311_TYPEC_STATE;

/* Read observations only; the first failed acquire is retained through cleanup.
 * Step: 1 guards, 2 identity, 3 voltage, 4 control, 5 fault, 6 alert,
 * 7 saved fields, 8 RX stop, 9 CC open, 10 quiet, 11 Rp, 12 attach,
 * 13 header, 14 final validation. Register is the rejected observation when
 * identified, otherwise the last attempted read. Length zero means no read.
 * GuardFlags records seven fresh conditions; FF means the callback failed. */
typedef struct {
  MTK_HUSB311_U8 Step;
  MTK_HUSB311_U8 Register;
  MTK_HUSB311_U8 Length;
  MTK_HUSB311_U8 GuardFlags;
  MTK_HUSB311_U16 Value;
  MTK_HUSB311_U8 OpenAttempts;
  MTK_HUSB311_U8 OpenWriteStatus;
  MTK_HUSB311_TYPEC_STATUS ReadStatus;
  MTK_HUSB311_TYPEC_STATUS Status;
} MTK_HUSB311_TYPEC_DIAGNOSTIC;

typedef struct {
  /* Initialize once after zero-initialization; repeat calls are rejected. */
  MTK_HUSB311_U32 Signature;
  MTK_HUSB311_TYPEC_IO Io;
  MTK_HUSB311_TYPEC_STATE State;
  MTK_HUSB311_U8 OriginalRoleControl;
  MTK_HUSB311_U8 OriginalRxDetect;
  MTK_HUSB311_U8 OriginalMessageHeader;
  MTK_HUSB311_U8 ExpectedRoleControl;
  MTK_HUSB311_U8 ExpectedRxDetect;
  MTK_HUSB311_U8 ExpectedMessageHeader;
  MTK_HUSB311_U8 RoleOwnedMask;
  MTK_HUSB311_U8 OwnedFields;
  MTK_HUSB311_U8 AttachedCcPin;
  MTK_HUSB311_U8 ConflictActive;
  MTK_HUSB311_U8 ConflictRegister;
  MTK_HUSB311_U8 ConflictMask;
  MTK_HUSB311_U8 ConflictExpected;
  MTK_HUSB311_U8 ConflictTarget;
  MTK_HUSB311_U8 ConflictField;
  MTK_HUSB311_U8 ConflictRoleMask;
  MTK_HUSB311_U8 ConflictWriteAttempted;
  MTK_HUSB311_U8 ConflictRestoring;
  MTK_HUSB311_TYPEC_STATUS LastError;
  MTK_HUSB311_U16 TxResultBaseline;
  MTK_HUSB311_U16 ProbeElapsedMs;
  MTK_HUSB311_U16 ProbeStableMs;
  MTK_HUSB311_U8 ProbeCcPin;
  MTK_HUSB311_U8 ProbeWaitingForAttach;
  MTK_HUSB311_TYPEC_STATUS ProbeFailure;
  MTK_HUSB311_TYPEC_STATUS CleanupFirstError;
  MTK_HUSB311_U16 CleanupTxResult;
  MTK_HUSB311_U16 CleanupElapsedMs;
  MTK_HUSB311_U16 CleanupQuietMs;
  MTK_HUSB311_TOKEN TokenCounter;
  MTK_HUSB311_TOKEN ActiveToken;
  MTK_HUSB311_TYPEC_DIAGNOSTIC Diagnostic;
  MTK_HUSB311_TYPEC_DIAGNOSTIC FirstFailure;
} MTK_HUSB311_TYPEC_CORE;

#define MTK_HUSB311_TYPEC_REG_VENDOR_ID       0x00U
#define MTK_HUSB311_TYPEC_REG_DEVICE_ID       0x02U
#define MTK_HUSB311_TYPEC_REG_ALERT           0x10U
#define MTK_HUSB311_TYPEC_REG_TCPC_CONTROL    0x19U
#define MTK_HUSB311_TYPEC_REG_ROLE_CONTROL    0x1AU
#define MTK_HUSB311_TYPEC_REG_POWER_CONTROL   0x1CU
#define MTK_HUSB311_TYPEC_REG_CC_STATUS       0x1DU
#define MTK_HUSB311_TYPEC_REG_POWER_STATUS    0x1EU
#define MTK_HUSB311_TYPEC_REG_FAULT_STATUS    0x1FU
#define MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER  0x2EU
#define MTK_HUSB311_TYPEC_REG_RX_DETECT       0x2FU
#define MTK_HUSB311_TYPEC_REG_VENDOR_STATUS  0x97U

#define MTK_HUSB311_TYPEC_ALERT_ALLOWED_MASK  0x0003U
#define MTK_HUSB311_TYPEC_ALERT_TX_RESULT_MASK 0x0070U
#define MTK_HUSB311_TYPEC_REG_VENDOR_ALERT    0x98U
#define MTK_HUSB311_TYPEC_ALERT_VBUS_80_MASK  0x02U
#define MTK_HUSB311_TYPEC_TCPC_BIST_MASK      0x02U
#define MTK_HUSB311_TYPEC_POWER_VCONN_MASK    0x01U
#define MTK_HUSB311_TYPEC_ROLE_DRP_MASK       0x40U
#define MTK_HUSB311_TYPEC_ROLE_CC_MASK        0x0FU
#define MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK  0x30U
#define MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK  0x80U
#define MTK_HUSB311_TYPEC_ROLE_OPEN_FIELDS    0x0FU
#define MTK_HUSB311_TYPEC_ROLE_SOURCE_RP_DFT  0x05U
#define MTK_HUSB311_TYPEC_HEADER_ROLE_MASK    0x09U
#define MTK_HUSB311_TYPEC_HEADER_DFP_SOURCE   0x09U
#define MTK_HUSB311_TYPEC_MUSB_SESSION_MASK  0x01U
#define MTK_HUSB311_TYPEC_CC_OPEN             0U
#define MTK_HUSB311_TYPEC_CC_RD               2U
#define MTK_HUSB311_TYPEC_CC_PIN_NONE         0U
#define MTK_HUSB311_TYPEC_CC_PIN_1            1U
#define MTK_HUSB311_TYPEC_CC_PIN_2            2U
#define MTK_HUSB311_TYPEC_TOKEN_FIELD_RX      0x01U
#define MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE    0x02U
#define MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER  0x04U

#define MTK_HUSB311_TYPEC_ERROR_RECOVERY_MS   25U
#define MTK_HUSB311_TYPEC_ATTACH_DEBOUNCE_MS  200U
#define MTK_HUSB311_TYPEC_ATTACH_TIMEOUT_MS   5000U
#define MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS    5U
#define MTK_HUSB311_TYPEC_TX_QUIESCENCE_MS    100U
#define MTK_HUSB311_TYPEC_TX_QUIESCENCE_TIMEOUT_MS 1000U

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecInitialize(
  MTK_HUSB311_TYPEC_CORE *Core,
  const MTK_HUSB311_TYPEC_IO *Io
  );

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecAcquireSource(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN *Token
  );

// A bounded open-CC probe for periodic discovery. Connected partners retain
// normal debounce and readiness checks; clean timeout restores owned state.
MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecProbeSource(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN *Token
  );

// Advance an open-CC probe once per sample period while VBUS is off.
// Pending includes rollback. Drop the bus lease between samples, and poll
// until the probe returns its original failure or an ownership error.
MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecBeginSourceProbe(MTK_HUSB311_TYPEC_CORE *Core);

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecPollSourceProbe(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN *Token
  );

// Cancel a pending probe or release an owned source. From Pending, call
// again after one sample period with a new bus lease.
MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecStepCleanup(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  );

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecCancelSourceProbe(MTK_HUSB311_TYPEC_CORE *Core);

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecValidateSource(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  );

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecReleaseSource(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  );

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecRetryCleanup(
  MTK_HUSB311_TYPEC_CORE *Core
  );

int
MtkHusb311TypecTokenActive(
  const MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  );

#endif
