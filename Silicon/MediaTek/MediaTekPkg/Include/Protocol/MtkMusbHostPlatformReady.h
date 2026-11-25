// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_MUSB_HOST_PLATFORM_READY_H
#define MTK_MUSB_HOST_PLATFORM_READY_H

#include <Uefi.h>

#define MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL_GUID \
  { 0x6c62f8ab, 0x4ac1, 0x4aa7, { 0x9d, 0x43, 0x48, 0xbc, 0x48, 0x12, 0x0d, 0x7e } }

#define MTK_MUSB_HOST_PLATFORM_READY_SIGNATURE SIGNATURE_32('M','U','S','B')
#define MTK_MUSB_HOST_PLATFORM_READY_LEGACY_REVISION 2U
#define MTK_MUSB_HOST_PLATFORM_READY_V3_REVISION 3U
#define MTK_MUSB_HOST_PLATFORM_READY_REVISION 4U

#define MTK_MUSB_READY_MMIO_MAPPED       (1U << 0)
#define MTK_MUSB_READY_CLOCK_ENABLED     (1U << 1)
#define MTK_MUSB_READY_PHY_READY         (1U << 2)
#define MTK_MUSB_READY_HOST_ROLE_ACTIVE  (1U << 3)
#define MTK_MUSB_READY_VBUS_SOURCE       (1U << 4)
#define MTK_MUSB_READY_FIFO_PROFILE      (1U << 5)
#define MTK_MUSB_READY_PORT_EVENTS       (1U << 6)
#define MTK_MUSB_READY_REQUIRED          0x7FU
#define MTK_MUSB_READY_ID_LOW_HUB        (1U << 7)
#define MTK_MUSB_READY_ID_LOW_REQUIRED   \
  ((MTK_MUSB_READY_REQUIRED & ~MTK_MUSB_READY_VBUS_SOURCE) | \
   MTK_MUSB_READY_ID_LOW_HUB)

#define MTK_MUSB_HOST_POWER_PATH_LEGACY_TYPEC 0U
#define MTK_MUSB_HOST_POWER_PATH_TYPEC_SOURCE 1U
#define MTK_MUSB_HOST_POWER_PATH_ID_LOW_HUB   2U
#define MTK_MUSB_HOST_POWER_PATH_TYPEC_ONLY   3U
#define MTK_MUSB_HOST_POWER_PATH_HOST_FIRST   4U
#define MTK_MUSB_READY_HOST_FIRST_REQUIRED (MTK_MUSB_READY_REQUIRED & ~MTK_MUSB_READY_VBUS_SOURCE)

#define MTK_MUSB_CONTROLLER_BASE 0x11200000ULL
#define MTK_MUSB_CONTROLLER_LENGTH 0x10000ULL
#define MTK_MUSB_PHY_BASE 0x11F40000ULL
#define MTK_MUSB_PHY_LENGTH 0x10000ULL
#define MTK_MUSB_GSIV 160U
#define MTK_MUSB_ENDPOINT_COUNT 16U
#define MTK_MUSB_ROOT_PORT_COUNT 1U
#define MTK_MUSB_FIFO_PROFILE_T70PRO_HOST_V1 1U
#define MTK_MUSB_CONNECT_DISCONNECT_MASK 0x30U

// Called at TPL_CALLBACK after DisconnectController has removed every child.
// Success retires the HCD only; platform power, PHY and pins remain owned by
// the producer until it removes this ready protocol and stops the controller.
typedef EFI_STATUS (EFIAPI *MTK_MUSB_RELEASE_HOST)(IN VOID *HostContext);

typedef struct {
  UINT32 Signature;
  UINT16 Revision;
  UINT16 Length;
  UINT32 Flags;
  EFI_PHYSICAL_ADDRESS ControllerBase;
  UINT64 ControllerLength;
  EFI_PHYSICAL_ADDRESS PhyBase;
  UINT64 PhyLength;
  UINT32 InterruptVector;
  UINT8 EndpointCount;
  UINT8 PortCount;
  UINT8 FifoProfile;
  UINT8 InterruptMask;
  // Revision 2 used this word as Reserved=0 (legacy Type-C source path).
  UINT32 HostPowerPath;
  // Written only by MusbHostDxe at TPL_NOTIFY while quiescing at EBS.
  // The platform consumer runs at TPL_CALLBACK and publishes this snapshot.
  UINT16 HostPortStatus;
  UINT16 HostPortChangeStatus;
  volatile UINT32 HostSnapshotValid;
  // Revision 4. Written by MusbHostDxe only while this full-length protocol
  // is installed. Cleared before its context is freed; failures retain both.
  MTK_MUSB_RELEASE_HOST ReleaseHost;
  VOID *HostContext;
} MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL;

#define MTK_MUSB_HOST_PLATFORM_READY_V3_LENGTH \
  OFFSET_OF(MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL, ReleaseHost)

extern EFI_GUID gMtkMusbHostPlatformReadyProtocolGuid;

#endif
