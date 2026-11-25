/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_PMIC_BROKER_H_
#define MTK_PMIC_BROKER_H_
#include <stdint.h>

#define MTK_PMIC_BROKER_VERSION 1U
#define MTK_PMIC_BROKER_VERSION_KEY_IRQ 2U
#define MTK_PMIC_BROKER_VERSION_SD_POWER 3U
#define MTK_PMIC_BROKER_VERSION_CONN_RAILS 4U
#define MTK_PMIC_BROKER_VERSION_GAUGE 5U
#define MTK_PMIC_BROKER_SIGNATURE 0x504D4231U
#define MTK_PMIC_BROKER_CAP_KEY_IRQ_LEASE (1U << 0)
#define MTK_PMIC_BROKER_CAP_CAPTURED_KEY_ACK (1U << 1)
#define MTK_PMIC_BROKER_CAP_SD_POWER_LEASE (1U << 2)
#define MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE (1U << 3)
#define MTK_PMIC_BROKER_CAP_GAUGE_LEASE (1U << 4)
#define MTK_PMIC_PRM_HANDLER_GUID \
  {0x9f58ca73,0x6bc4,0x45b8,{0xb6,0x9c,0xf3,0x3c,0x62,0x61,0xd0,0x81}}

typedef enum {
  MtkPmicBrokerQuery=0, MtkPmicBrokerBeginRead, MtkPmicBrokerBeginWrite,
  MtkPmicBrokerStep, MtkPmicBrokerAck, MtkPmicBrokerBeginKeySnapshot,
  MtkPmicBrokerAcquireKeyLease=6, MtkPmicBrokerReleaseKeyLease,
  MtkPmicBrokerAckKeySnapshot,
  MtkPmicBrokerAcquireSdPower, MtkPmicBrokerReleaseSdPower,
  MtkPmicBrokerAcquireConnRails, MtkPmicBrokerReleaseConnRails,
  MtkPmicBrokerBeginConnRead, MtkPmicBrokerBeginConnWrite,
  MtkPmicBrokerAcquireGaugeLease, MtkPmicBrokerReleaseGaugeLease,
  MtkPmicBrokerBeginGaugeRead, MtkPmicBrokerBeginGaugeWrite
} MTK_PMIC_BROKER_OPERATION;
typedef enum {
  MtkPmicBrokerStateIdle=0, MtkPmicBrokerStatePending,
  MtkPmicBrokerStateDone, MtkPmicBrokerStateFault
} MTK_PMIC_BROKER_STATE;
typedef enum {
  MtkPmicBrokerResultSuccess=0, MtkPmicBrokerResultInProgress,
  MtkPmicBrokerResultInvalid, MtkPmicBrokerResultBusy,
  MtkPmicBrokerResultWrongState, MtkPmicBrokerResultIoError,
  MtkPmicBrokerResultTimeout, MtkPmicBrokerResultStaleGeneration,
  MtkPmicBrokerResultExhausted, MtkPmicBrokerResultSiblingPending
} MTK_PMIC_BROKER_RESULT;

/* Fixed 64-byte kernel/PRM wire. Version 1 and operations 0..5 retain their
 * original meaning. Version 1 KeySnapshot is read-only and never creates or
 * overwrites a retained IRQ capture. Version 2 Query returns capability bits
 * in Flags and performs no MMIO. Version 2 AcquireKeyLease uses generation,
 * address and value zero; its completed job generation is the lease token.
 * Version 2 KeySnapshot uses that lease token as input generation and zero
 * address/value; its returned job generation is the snapshot token. Pending
 * snapshot returns address 0x91c/value zero; Done returns group-status in
 * Value[31:16] plus TOPSTATUS in Value[15:0]. Version 2 AckKeySnapshot uses
 * the retained snapshot token as input generation and zero address/value;
 * accepted jobs return a fresh job generation for Step/metadata Ack. If home
 * or another sibling status bit is pending, it returns Done/SiblingPending,
 * preserves the packed captured value and retains the snapshot for a later
 * fresh check after metadata Ack. It issues no W1C in that case. A successful
 * op8 writes only the captured 0x91c power bits 0/2 and retires the retained
 * capture only after metadata Ack. Version 2 ReleaseKeyLease uses the lease
 * token as input generation and zero address/value; it restores only bits the
 * broker enabled and preserves current unrelated bits. All input
 * flags/reserved/state/result fields are zero. Firmware ResultBusy means the
 * input was not executed. Completed release and captured-ack retries are
 * cached without MMIO. Generation exhaustion is fail-closed; a fault has no
 * reset/forget/force-clear operation. A same-bit PMIC edge can coalesce with
 * the captured W1C while that bit is set; this wire cannot promise edge counts.
 * Version 3 supports Query, AcquireSdPower, ReleaseSdPower, Step and Ack.
 * Acquire takes zero generation, address and value. After job completion and
 * metadata Ack, its generation is the lease token. Release takes that token
 * and zero address/value. Both operations return the job generation for
 * Step and Ack. Finish power-up settling before starting the host; quiesce
 * the slot and gate its card clock before releasing power.
 * Version 4 adds a lease for the MT6789 CONN power fields. Acquire uses zero
 * generation/address/value and returns Done with the lease generation. Read
 * and Write use the lease generation, an allowed address, and zero for reads
 * or the complete 16-bit register image for writes. VS2 voter SET/CLR accept
 * only the CONN vote. Complete job Step/Ack with the returned job generation.
 * Release uses the lease generation and zero address/value. Once all changed
 * fields have been observed restored, it returns Idle with the lease generation
 * and can be retried without MMIO. Return CONN and GPIO to their saved state
 * before releasing this lease. v4 Query advertises only the CONN capability. */
typedef struct {
  uint32_t Size;
  uint16_t Version,Operation;
  uint32_t Flags,State;
  uint64_t Generation;
  uint32_t Result,Address,Value,Reserved0;
  uint64_t Reserved[3];
} MTK_PMIC_BROKER_REQUEST;

typedef enum {
  MtkPmicBrokerIoOk=0, MtkPmicBrokerIoBusy, MtkPmicBrokerIoFailed
} MTK_PMIC_BROKER_IO_STATUS;
typedef struct {
  void *Context;
  MTK_PMIC_BROKER_IO_STATUS (*Read32)(void *,uint32_t Offset,uint32_t *Value);
  MTK_PMIC_BROKER_IO_STATUS (*Write32)(void *,uint32_t Offset,uint32_t Value);
  uint64_t (*NowUs)(void *);
} MTK_PMIC_BROKER_IO;

#define MTK_PMIC_CONN_REG_COUNT 12U

/* Scalar journal only. Firmware supplies converted MMIO callbacks each call.
 * One common firmware CAS lock encloses both this core and the UFS power core.
 * OtherOwned is the UFS WACS journal, including its unknown/faulted responses.
 * This arbitrates these two software clients; secure-world exclusion and live
 * parent supply/clock ownership still require separate platform qualification. */
typedef struct {
  uint32_t Signature,State,Phase,Operation,Address,RequestedValue,Value;
  uint32_t Mask,ReadValue,PollCount,FinalResult,WriteCallbackActive;
  uint32_t SnapshotStage,SnapshotGroupStatus;
  uint64_t Generation,LastAckGeneration,StartedUs;
  uint32_t ProtocolVersion,RetainKeySnapshot;
  uint32_t KeyLeaseState,KeyLeaseBaselineBits,KeyLeaseOwnedMask;
  uint32_t KeySnapshotValid,KeySnapshotStatus,KeySnapshotTopStatus;
  uint32_t KeySnapshotAckState,KeySnapshotAckMask,KeyAckWriteMayHaveEffect;
  uint32_t KeySnapshotSiblingStatus,LastKeyAckValue;
  uint64_t KeyLeaseToken,KeySnapshotToken,KeySnapshotLeaseToken;
  uint64_t LastReleasedLeaseToken,LastReleasedJobGeneration;
  uint64_t LastAckedSnapshotToken,LastKeyAckJobGeneration;
  uint64_t SdPowerToken,SdReleasedToken,SdReleaseGeneration;
  uint32_t SdPowerFault,SdStage,SdBaseline[5];
  uint64_t ConnLeaseToken,ConnReleasedToken;
  uint32_t ConnCaptured,ConnDirty;
  uint64_t GaugeLeaseToken,GaugeReleasedToken;
  uint32_t GaugeWriteData;
  uint16_t ConnBaseline[MTK_PMIC_CONN_REG_COUNT];
} MTK_PMIC_BROKER;

void MtkPmicBrokerInitialize(MTK_PMIC_BROKER *);
int MtkPmicBrokerBlocksUfs(const MTK_PMIC_BROKER *);
MTK_PMIC_BROKER_RESULT MtkPmicBrokerHandle(
  MTK_PMIC_BROKER *,const MTK_PMIC_BROKER_IO *,int OtherOwned,
  MTK_PMIC_BROKER_REQUEST *);
/* Runtime PRM uses this as the final WACS write gate. It is deliberately
 * operation/state specific; no raw PMIC register write interface is exposed. */
int MtkPmicBrokerWacsWriteAllowed(const MTK_PMIC_BROKER *,uint32_t Offset,uint32_t Value);
int MtkPmicBrokerConnAddressAllowed(uint32_t Address);
#endif
