// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MUSB_HOST_CORE_H
#define MUSB_HOST_CORE_H

#include <stdint.h>
#include <stddef.h>

#define MUSB_HOST_CORE_REVISION 1U
#define MUSB_HOST_MAX_ENDPOINTS 16U
#define MUSB_HOST_PORT_COUNT 1U
#define MUSB_HOST_PIO_ENDPOINT 1U
#define MUSB_HOST_EP0_FIFO_BYTES 64U
#define MUSB_HOST_DATA_FIFO_BYTES 512U
#define MUSB_HOST_T70_FIFO_PROFILE 1U
#define MUSB_HOST_CONNECT_DISCONNECT_MASK 0x30U
#define MUSB_HOST_RESUME_MASK 0x02U
#define MUSB_HOST_CONTROLLER_MIN_BYTES 0x500U
#define MUSB_HOST_PHY_MIN_BYTES 0x700U
#define MUSB_HOST_GSIV 160U

#define MUSB_READY_MMIO_MAPPED       (1U << 0)
#define MUSB_READY_CLOCK_ENABLED     (1U << 1)
#define MUSB_READY_PHY_READY         (1U << 2)
#define MUSB_READY_HOST_ROLE_ACTIVE  (1U << 3)
#define MUSB_READY_VBUS_SOURCE       (1U << 4)
#define MUSB_READY_FIFO_PROFILE      (1U << 5)
#define MUSB_READY_PORT_EVENTS       (1U << 6)
#define MUSB_READY_REQUIRED          0x7FU
#define MUSB_READY_ID_LOW_HUB        (1U << 7)
#define MUSB_READY_HOST_FIRST_REQUIRED (MUSB_READY_REQUIRED & ~MUSB_READY_VBUS_SOURCE)
#define MUSB_READY_ID_LOW_REQUIRED   \
  ((MUSB_READY_REQUIRED & ~MUSB_READY_VBUS_SOURCE) | MUSB_READY_ID_LOW_HUB)

#define MUSB_HOST_STATE_HALTED       0U
#define MUSB_HOST_STATE_OPERATIONAL  1U

#define MUSB_PORT_CONNECTION         0x0001U
#define MUSB_PORT_ENABLE             0x0002U
#define MUSB_PORT_SUSPEND            0x0004U
#define MUSB_PORT_OVERCURRENT        0x0008U
#define MUSB_PORT_RESET              0x0010U
#define MUSB_PORT_POWER              0x0100U
#define MUSB_PORT_LOW_SPEED          0x0200U
#define MUSB_PORT_HIGH_SPEED         0x0400U

#define MUSB_PORT_CHANGE_CONNECTION  0x0001U
#define MUSB_PORT_CHANGE_ENABLE      0x0002U
#define MUSB_PORT_CHANGE_SUSPEND     0x0004U
#define MUSB_PORT_CHANGE_OVERCURRENT 0x0008U
#define MUSB_PORT_CHANGE_RESET       0x0010U

#define MUSB_PORT_FEATURE_ENABLE             1U
#define MUSB_PORT_FEATURE_SUSPEND            2U
#define MUSB_PORT_FEATURE_RESET              4U
#define MUSB_PORT_FEATURE_POWER              8U
#define MUSB_PORT_FEATURE_CHANGE_CONNECTION 16U
#define MUSB_PORT_FEATURE_CHANGE_ENABLE     17U
#define MUSB_PORT_FEATURE_CHANGE_SUSPEND    18U
#define MUSB_PORT_FEATURE_CHANGE_OVERCURRENT 19U
#define MUSB_PORT_FEATURE_CHANGE_RESET      20U

#define MUSB_SPEED_FULL 0U
#define MUSB_SPEED_LOW  1U
#define MUSB_SPEED_HIGH 2U

#define MUSB_TRANSFER_BULK      2U
#define MUSB_TRANSFER_INTERRUPT 3U
// Host-side mode for one software-scheduled interrupt IN poll.
#define MUSB_TRANSFER_INTERRUPT_POLL 4U

typedef enum {
  MusbOk = 0,
  MusbInvalid,
  MusbUnsupported,
  MusbNotReady,
  MusbBusy,
  MusbTimeout,
  MusbStall,
  MusbProtocolError,
  MusbBufferOverflow,
  MusbDisconnected,
  MusbIoError,
  MusbCancelled
} MUSB_STATUS;

typedef int (*MUSB_READ8)(void *Context, uint32_t Offset, uint8_t *Value);
typedef int (*MUSB_READ16)(void *Context, uint32_t Offset, uint16_t *Value);
typedef int (*MUSB_READ32)(void *Context, uint32_t Offset, uint32_t *Value);
typedef int (*MUSB_WRITE8)(void *Context, uint32_t Offset, uint8_t Value);
typedef int (*MUSB_WRITE16)(void *Context, uint32_t Offset, uint16_t Value);
typedef int (*MUSB_WRITE32)(void *Context, uint32_t Offset, uint32_t Value);
typedef uint64_t (*MUSB_NOW_US)(void *Context);
typedef void (*MUSB_DELAY_US)(void *Context, uint32_t Microseconds);
typedef int (*MUSB_TRY_LOCK)(void *Context);
typedef void (*MUSB_UNLOCK)(void *Context);
typedef int (*MUSB_CANCELLED)(void *Context);

typedef struct {
  void *Context;
  uint64_t ControllerLength;
  MUSB_READ8 Read8;
  MUSB_READ16 Read16;
  MUSB_READ32 Read32;
  MUSB_WRITE8 Write8;
  MUSB_WRITE16 Write16;
  MUSB_WRITE32 Write32;
  MUSB_NOW_US NowUs;
  MUSB_DELAY_US DelayUs;
  MUSB_TRY_LOCK TryLock;
  MUSB_UNLOCK Unlock;
  /*
   * Optional nonblocking, level-triggered transfer-cancel query. Keep returning
   * nonzero until the current transfer is retired. NULL preserves old behavior.
   */
  MUSB_CANCELLED Cancelled;
} MUSB_HOST_IO;

typedef struct {
  uint32_t Revision;
  uint32_t Flags;
  uint64_t ControllerLength;
  uint64_t PhyLength;
  uint32_t InterruptVector;
  uint8_t EndpointCount;
  uint8_t PortCount;
  uint8_t FifoProfile;
  uint8_t InterruptMask;
} MUSB_HOST_CONFIG;

// NULL or {0,0,0,0} selects a directly attached device.  Otherwise the
// nearest high-speed hub translates this full/low-speed transaction.
// MultiTt is explicit because UEFI's translator record has no such field.
typedef struct {
  uint8_t HubAddress;
  uint8_t HubPort;
  uint8_t MultiTt;
  uint8_t Reserved;
} MUSB_HOST_ROUTE;

typedef struct {
  MUSB_HOST_IO Io;
  MUSB_HOST_CONFIG Config;
  uint16_t PortStatus;
  uint16_t PortChangeStatus;
  uint8_t State;
  uint8_t Initialized;
  uint8_t PortPowerLost;
} MUSB_HOST;

MUSB_STATUS
MusbHostInitialize(
  MUSB_HOST *Host,
  const MUSB_HOST_IO *Io,
  const MUSB_HOST_CONFIG *Config
  );

/* Import a quiescent previous-owner root port snapshot, before Operational. */
MUSB_STATUS
MusbHostAdoptPortState(MUSB_HOST *Host, uint16_t PortStatus, uint16_t ChangeStatus);

MUSB_STATUS
MusbHostSetState(
  MUSB_HOST *Host,
  uint8_t State
  );

/* Invalidate a halted controller's port after the platform observes VBUS
 * loss during resume. This only changes software state; no register I/O. */
MUSB_STATUS
MusbHostMarkVbusLost(MUSB_HOST *Host);

MUSB_STATUS
MusbHostGetPortStatus(
  MUSB_HOST *Host,
  uint8_t PortNumber,
  uint16_t *PortStatus,
  uint16_t *PortChangeStatus
  );

/* Complete live hardware resume for a suspended host port. Call from a
 * context in which the controller's DelayUs callback may wait 20+10 ms. */
MUSB_STATUS
MusbHostServiceRemoteWake(MUSB_HOST *Host);

MUSB_STATUS
MusbHostSetPortFeature(
  MUSB_HOST *Host,
  uint8_t PortNumber,
  uint8_t Feature
  );

MUSB_STATUS
MusbHostClearPortFeature(
  MUSB_HOST *Host,
  uint8_t PortNumber,
  uint8_t Feature
  );

MUSB_STATUS
MusbHostControlTransfer(
  MUSB_HOST *Host,
  uint8_t DeviceAddress,
  uint8_t DeviceSpeed,
  uint8_t MaximumPacketLength,
  const uint8_t SetupPacket[8],
  void *Data,
  uint32_t DataCapacity,
  uint32_t TimeoutMilliseconds,
  uint32_t *BytesTransferred
  );

MUSB_STATUS
MusbHostDataTransfer(
  MUSB_HOST *Host,
  uint8_t TransferType,
  uint8_t DeviceAddress,
  uint8_t EndpointAddress,
  uint8_t DeviceSpeed,
  uint16_t MaximumPacketLength,
  uint8_t Interval,
  void *Data,
  uint32_t DataLength,
  uint8_t *DataToggle,
  uint32_t TimeoutMilliseconds,
  uint32_t *BytesTransferred
  );

MUSB_STATUS
MusbHostControlTransferRouted(
  MUSB_HOST *Host,
  uint8_t DeviceAddress,
  uint8_t DeviceSpeed,
  uint8_t MaximumPacketLength,
  const uint8_t SetupPacket[8],
  void *Data,
  uint32_t DataCapacity,
  uint32_t TimeoutMilliseconds,
  const MUSB_HOST_ROUTE *Route,
  uint32_t *BytesTransferred
  );

MUSB_STATUS
MusbHostDataTransferRouted(
  MUSB_HOST *Host,
  uint8_t TransferType,
  uint8_t DeviceAddress,
  uint8_t EndpointAddress,
  uint8_t DeviceSpeed,
  uint16_t MaximumPacketLength,
  uint8_t Interval,
  void *Data,
  uint32_t DataLength,
  uint8_t *DataToggle,
  uint32_t TimeoutMilliseconds,
  const MUSB_HOST_ROUTE *Route,
  uint32_t *BytesTransferred
  );

#endif
