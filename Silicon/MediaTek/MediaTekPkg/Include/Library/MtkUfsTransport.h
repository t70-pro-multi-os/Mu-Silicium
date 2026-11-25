/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_UFS_TRANSPORT_H_
#define MTK_UFS_TRANSPORT_H_
#include <stddef.h>
#include <stdint.h>
/* UFSHCI legacy single-slot transport. The caller serializes all functions.
 * DMA memory must be contiguous, uncached, and owned until Stop succeeds.
 * No caller data pointer is ever programmed into the controller. */
#define MTK_UFS_DMA_BYTES       0x21000U
#define MTK_UFS_DATA_OFFSET     0x1000U
#define MTK_UFS_DATA_BYTES      0x20000U
#define MTK_UFS_COMMAND_OFFSET  0x0800U
#define MTK_UFS_RESPONSE_OFFSET 0x0820U
#define MTK_UFS_PRD_OFFSET      0x0860U
#define MTK_UFS_ERROR_INTERRUPTS 0x00031886U
#define MTK_UFS_INTERRUPTS       (MTK_UFS_ERROR_INTERRUPTS | 1U)
#define MTK_UFS_USER_LUN         2U
#define MTK_UFS_DEVICE_WLUN      0xD0U
#define MTK_UFS_USER_BYTES       127959826432ULL
#define MTK_UFS_BLOCK_BYTES      4096U

typedef enum {
  MtkUfsIoSuccess = 0, MtkUfsIoPending, MtkUfsIoBusy, MtkUfsIoInvalid,
  MtkUfsIoNotReady, MtkUfsIoError, MtkUfsIoUnsafeDma
} MTK_UFS_IO_STATUS;
typedef struct {
  void *Context;
  uint32_t (*Read32)(void *Context, uint32_t Offset);
  void (*Write32)(void *Context, uint32_t Offset, uint32_t Value);
  void (*Fence)(void *Context);
  void (*StallUs)(void *Context, uint32_t Microseconds);
} MTK_UFS_TRANSPORT_IO;
typedef struct {
  MTK_UFS_TRANSPORT_IO Io;
  volatile uint8_t *Dma;
  uint64_t Physical;
  uint32_t ExpectedBytes;
  uint8_t NextTag, ActiveTag, ActiveLun;
  uint8_t Attached, Active, Nop, Faulted;
  uint8_t QueryOpcode;
  uint8_t UicPending, Hibernate8CommandDone;
  uint8_t StopInProgress, StopPhase;
} MTK_UFS_TRANSPORT;
typedef struct {
  uint32_t Transferred;
  uint8_t ScsiStatus;
  uint8_t SenseLength;
  uint8_t Sense[30];
  uint8_t QueryFlagValid, QueryFlagValue;
} MTK_UFS_IO_RESULT;
/* Performs read-only validation first; failure before ownership makes no writes.
 * Requires firmware to leave the link enabled but both lists stopped, interrupt
 * masked, and old DMA bases zero. Only UFSHCI 2.0/2.1 legacy mode is accepted. */
MTK_UFS_IO_STATUS MtkUfsTransportAttach(MTK_UFS_TRANSPORT *T,
  const MTK_UFS_TRANSPORT_IO *Io, volatile void *Dma, size_t DmaBytes,
  uint64_t Physical);
MTK_UFS_IO_STATUS MtkUfsTransportNop(MTK_UFS_TRANSPORT *T);
/* Read-only Windows enumeration policy. Does not broaden the UEFI BlockIo list.
 * Unknown, vendor, security, firmware, power, write and UNMAP commands fail. */
int MtkUfsTransportWriteCdb(const uint8_t *Cdb, size_t CdbLength, uint32_t Bytes);
int MtkUfsTransportReadCdb(const uint8_t *Cdb, size_t CdbLength, uint32_t Bytes);
MTK_UFS_IO_STATUS MtkUfsTransportScsi(MTK_UFS_TRANSPORT *T,
  const uint8_t *Cdb, size_t CdbLength, uint32_t Bytes);
/* Explicit write/flush entry; the read-only UEFI adapter never calls this.
 * WRITE(6/10/12/16) or non-IMMED SYNCHRONIZE CACHE(10/16), only LU2.
 * Copies data into private common DMA before ringing the doorbell. */
MTK_UFS_IO_STATUS MtkUfsTransportWriteScsi(MTK_UFS_TRANSPORT *T,
  const uint8_t *Cdb, size_t CdbLength, uint32_t Bytes, const void *Data);
MTK_UFS_IO_STATUS MtkUfsTransportPoll(MTK_UFS_TRANSPORT *T, MTK_UFS_IO_RESULT *Result);
/* Narrow device-management commands; never exposed through the LU2 CDB list.
 * READ/CLEAR bAutoBkopsEn only, no arbitrary flag/attribute/descriptor writes.
 * Poll returns QueryFlagValid/Value after a complete, matching query response. */
MTK_UFS_IO_STATUS MtkUfsTransportReadAutoBkops(MTK_UFS_TRANSPORT *T);
MTK_UFS_IO_STATUS MtkUfsTransportDisableAutoBkops(MTK_UFS_TRANSPORT *T);
/* Fixed power commands to the UFS device well-known LUN, not LU2 passthru. */
MTK_UFS_IO_STATUS MtkUfsTransportPowerActive(MTK_UFS_TRANSPORT *T);
MTK_UFS_IO_STATUS MtkUfsTransportPowerDown(MTK_UFS_TRANSPORT *T);
/* Nonblocking T70 shutdown transition; caller polls link completion and probe. */
MTK_UFS_IO_STATUS MtkUfsTransportBeginHibernate8(MTK_UFS_TRANSPORT *T);
MTK_UFS_IO_STATUS MtkUfsTransportPollHibernate8(MTK_UFS_TRANSPORT *T);
/* Nonblocking stop state machine for interrupt/DPC driven callers. */
MTK_UFS_IO_STATUS MtkUfsTransportBeginStop(MTK_UFS_TRANSPORT *T);
MTK_UFS_IO_STATUS MtkUfsTransportPollStop(MTK_UFS_TRANSPORT *T);
/* Fatal/time-out/remove path. Masks interrupts, stops lists, disables HCE and
 * verifies completion. On failure DMA memory remains quarantined, never reused. */
MTK_UFS_IO_STATUS MtkUfsTransportStop(MTK_UFS_TRANSPORT *T);
/* Idle PnP stop: detach DMA lists while preserving the already-running link.
 * Restart may attach again only if all inherited hardware gates still hold. */
MTK_UFS_IO_STATUS MtkUfsTransportDetach(MTK_UFS_TRANSPORT *T);
#endif
