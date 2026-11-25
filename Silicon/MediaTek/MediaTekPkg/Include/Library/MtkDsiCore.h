/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_DSI_CORE_H
#define MTK_DSI_CORE_H

#define MTK_DSI_CORE_REVISION 3U

/* Keep this register core freestanding and OS-neutral. */
typedef unsigned char MTK_DSI_U8;
typedef unsigned int MTK_DSI_U32;
typedef char MTK_DSI_TYPES_MUST_MATCH[
  (sizeof (MTK_DSI_U8) == 1U && sizeof (MTK_DSI_U32) == 4U) ? 1 : -1
  ];

#define MTK_DSI_LK_MODULE_ID       0x10U
#define MTK_DSI_LK_PANEL_TYPE      2U
#define MTK_DSI_LK_WIDTH           2240U
#define MTK_DSI_LK_HEIGHT          1400U
#define MTK_DSI_LK_RATE_FIELD      733U
#define MTK_DSI_GET_PARAMS_SIZE    0x758U
#define MTK_DSI_GET_PARAMS_FRAGMENT_OFFSET 0x170U
#define MTK_DSI_LK_DELAY_MICROSECONDS 1000U

typedef enum {
  MtkDsiSuccess = 0,
  MtkDsiInvalid,
  MtkDsiBusy,
  MtkDsiWrongState,
  MtkDsiIoError,
  MtkDsiDelayError,
  MtkDsiQuarantined,
  MtkDsiTimeout
} MTK_DSI_STATUS;

typedef enum {
  MtkDsiBankDsi0 = 0,
  MtkDsiBankMipiTx
} MTK_DSI_BANK;

/* Callback offsets are relative to DSI0 physical base 0x14000000 and MIPI
 * TX physical base 0x11f60000. Thus DSI port offset 0x13000 is the DSI bank
 * address 0x14013000; the gate registers remain DSI-bank offsets 0x108/0x1a8. */

typedef MTK_DSI_STATUS
(*MTK_DSI_READ32)(
  void *Context,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 *Value
  );

typedef MTK_DSI_STATUS
(*MTK_DSI_WRITE32)(
  void *Context,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 Value
  );

/* LK's 0x6c0b4 delay leaf reads the counter converted with the 1,000,000
 * ticks/second fixed-point scale at 0xCBD88. The separate millisecond value
 * is stored at 0xCBD78 and is not used by this delay leaf. */
typedef MTK_DSI_STATUS
(*MTK_DSI_DELAY_MICROSECONDS)(
  void *Context,
  MTK_DSI_U32 Microseconds
  );

typedef struct {
  void *Context;
  MTK_DSI_READ32 Read32;
  MTK_DSI_WRITE32 Write32;
  MTK_DSI_DELAY_MICROSECONDS DelayMicroseconds;
} MTK_DSI_CORE_IO;

/* The DSI_config stage creates this context shadow before operation 7. These
 * fields are the source context word and destination DSI port offset pairs
 * emitted by the original 0x46a30 copy routine:
 *
 *   context+0x014 -> DSI+0x008   context+0x020 -> DSI+0x014
 *   context+0x024 -> DSI+0x018   context+0x028 -> DSI+0x01c
 *   context+0x02c -> DSI+0x020   context+0x030 -> DSI+0x024
 *   context+0x034 -> DSI+0x028   context+0x038 -> DSI+0x02c
 *   context+0x05c -> DSI+0x050   context+0x060 -> DSI+0x054
 *   context+0x064 -> DSI+0x058   context+0x068 -> DSI+0x05c
 *   context+0x070 -> DSI+0x064   context+0x09c -> DSI+0x090
 *   context+0x11c -> DSI+0x110   context+0x120 -> DSI+0x114
 *   context+0x124 -> DSI+0x118   context+0x128 -> DSI+0x11c
 *   context+0x1ac -> DSI+0x200
 *
 * Values must come from the configured context, not from live MMIO or a
 * captured trace. The original masks Reg18 with ~0x3c. */
typedef struct {
  MTK_DSI_U32 Reg08;
  MTK_DSI_U32 Reg14;
  MTK_DSI_U32 Reg18;
  MTK_DSI_U32 Reg1C;
  MTK_DSI_U32 Reg20;
  MTK_DSI_U32 Reg24;
  MTK_DSI_U32 Reg28;
  MTK_DSI_U32 Reg2C;
  MTK_DSI_U32 Reg50;
  MTK_DSI_U32 Reg54;
  MTK_DSI_U32 Reg58;
  MTK_DSI_U32 Reg5C;
  MTK_DSI_U32 Reg64;
  MTK_DSI_U32 Reg90;
  MTK_DSI_U32 Reg110;
  MTK_DSI_U32 Reg114;
  MTK_DSI_U32 Reg118;
  MTK_DSI_U32 Reg11C;
  MTK_DSI_U32 Reg200;
} MTK_DSI_PORT_SHADOW;

/* Raw get_params output plus the actual DSI_config arguments. GetParams is the
 * complete 0x758-byte buffer returned by LK get_params; it is not a captured
 * MMIO trace. ConfigWidth/Height are DSI_config's +0x910/+0x914 values. */
/* Input values from these six DSI port registers before DSI_config writes its
 * derived configuration. They are not the completed context shadow. Read them
 * only after the caller proves DSI power, clock, and valid MMIO mapping. */
typedef struct {
  MTK_DSI_U32 Reg08;
  MTK_DSI_U32 Reg14;
  MTK_DSI_U32 Reg18;
  MTK_DSI_U32 Reg1C;
  MTK_DSI_U32 Reg64;
  MTK_DSI_U32 Reg200;
} MTK_DSI_PORT_INITIAL;

typedef struct {
  const MTK_DSI_U8 *GetParams;
  MTK_DSI_U32 GetParamsSize;
  MTK_DSI_U32 ModuleId;
  MTK_DSI_U32 Mode;
  MTK_DSI_U32 ConfigWidth;
  MTK_DSI_U32 ConfigHeight;
  MTK_DSI_PORT_INITIAL Initial;
} MTK_DSI_OP7_PROFILE;

typedef enum {
  MtkDsiStateUninitialized = 0,
  MtkDsiStateIdle,
  MtkDsiStateRunningOp7,
  MtkDsiStateOp7Complete,
  MtkDsiStateQuarantined,
  MtkDsiStateStarting,
  MtkDsiStateStreaming,
  MtkDsiStateSuspending,
  MtkDsiStateSuspended,
  MtkDsiStateResuming
} MTK_DSI_CORE_STATE;

typedef struct {
  MTK_DSI_CORE_IO Io;
  MTK_DSI_U32 Signature;
  MTK_DSI_CORE_STATE State;
  MTK_DSI_STATUS LastStatus;
  MTK_DSI_BANK CurrentBank;
  MTK_DSI_U32 CurrentOffset;
  MTK_DSI_U32 CurrentValue;
  MTK_DSI_U32 ReadCalls;
  MTK_DSI_U32 WriteAttempts;
  MTK_DSI_U32 WritesAccepted;
  MTK_DSI_U32 DelayAttempts;
  MTK_DSI_U32 NextWriteIndex;
  MTK_DSI_U8 Busy;
  MTK_DSI_U8 SideEffectsPossible;
  MTK_DSI_U8 Reserved[2];
  MTK_DSI_PORT_SHADOW SavedPort;
} MTK_DSI_CORE;

/* Caller must zero-initialize Core, map both documented MMIO banks, hold the
 * board's exclusive DSI/MIPI ownership lease, ensure the DSI host is stopped,
 * and serialize the whole call. Busy rejects reentry; it is not a CPU lock.
 * Port shadow must come from the successful along_cyx DSI_config stage. */
MTK_DSI_STATUS
MtkDsiCoreInitialize(
  MTK_DSI_CORE *Core,
  const MTK_DSI_CORE_IO *Io
  );

/* Recomputes the along_cyx port shadow from get_params bytes, DSI_config
 * module/mode/geometry, and the six initial MMIO words. This is a pure
 * operation: output is unchanged on every invalid or unsupported input. The
 * caller must have obtained Initial only after DSI power, clock, and MMIO
 * mapping are independently verified. This does not start DSI or prove PHY
 * readiness. */
MTK_DSI_STATUS
MtkDsiCoreBuildAlongCyxConfigShadow(
  const MTK_DSI_OP7_PROFILE *Profile,
  const MTK_DSI_PORT_INITIAL *Initial,
  MTK_DSI_PORT_SHADOW *Output
  );

/* Runs only the original along_cyx DSI operation 7 for the exact supported
 * get_params/config profile after recomputing its port shadow. Read/modify/write preserves unowned bits. Any
 * ambiguous write or failure after a possible write quarantines the core;
 * the caller must retain ownership and must not replay/reset blindly. Success
 * records that register sequence callbacks completed. It does not prove PHY
 * power, PLL lock, link readiness, or visible pixels. */
MTK_DSI_STATUS
MtkDsiCoreRunAlongCyxOp7(
  MTK_DSI_CORE *Core,
  const MTK_DSI_OP7_PROFILE *Profile
  );

/* Reapply along_cyx DSI_config TXRX setup, then video operations 3 and 4. Only an
 * op7-complete core is accepted. The caller must have prepared the panel,
 * programmed the display route/plane, and retained all domain/clock leases.
 * Completion records register writes, not a frame or link acknowledgement. */
MTK_DSI_STATUS
MtkDsiCoreStartVideo (
  MTK_DSI_CORE *Core
  );

/* Stop video (operation 5), wait for operation 9's busy bit to clear, then
 * capture the 19-word context and execute operation 8's D-PHY shutdown.
 * MaxPolls is 1..1000; busy retries use the original 1000-us delay callback.
 * The bounded wait replaces LK's unbounded caller wait. No shutdown/reset is
 * issued on timeout. Call before panel unprepare and before releasing clocks.
 * This does not turn off the SoC display power domain or the panel's rails. */
MTK_DSI_STATUS
MtkDsiCoreSuspend (
  MTK_DSI_CORE *Core,
  MTK_DSI_U32 MaxPolls
  );

/* Resume operation 7 from the saved context, including LK's subsequent-use
 * rate word and reset sequence. It leaves video stopped (Op7Complete).
 * The caller must restore domain/clock/panel preparation before this call,
 * then program the route/plane and call StartVideo before panel enable.
 * A quarantined core cannot resume and must retain its ownership leases. */
MTK_DSI_STATUS
MtkDsiCoreResume (
  MTK_DSI_CORE *Core
  );

#endif
