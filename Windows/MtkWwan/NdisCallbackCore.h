/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MTK_WWAN_NDIS_CALLBACK_CORE_H
#define T70_MTK_WWAN_NDIS_CALLBACK_CORE_H

#include <stddef.h>
#include <stdint.h>

#define T70_WWAN_PACKET_FILTER_DIRECTED 0x00000001U
#define T70_WWAN_PACKET_FILTER_MULTICAST 0x00000002U
#define T70_WWAN_PACKET_FILTER_ALL_MULTICAST 0x00000004U
#define T70_WWAN_PACKET_FILTER_BROADCAST 0x00000008U
#define T70_WWAN_PACKET_FILTER_SUPPORTED (T70_WWAN_PACKET_FILTER_DIRECTED | \
  T70_WWAN_PACKET_FILTER_MULTICAST | T70_WWAN_PACKET_FILTER_ALL_MULTICAST | \
  T70_WWAN_PACKET_FILTER_BROADCAST)

static inline int
T70WwPacketFilterValid(uint32_t Filter)
{
  return (Filter & ~T70_WWAN_PACKET_FILTER_SUPPORTED) == 0U;
}

static inline int
T70WwPacketFilterStore(uint32_t Requested, uint32_t *Current)
{
  if (Current == NULL || !T70WwPacketFilterValid(Requested)) {
    return 0;
  }
  *Current = Requested;
  return 1;
}

static inline uint32_t
T70WwSendCompleteFlags(uint32_t AtDispatchLevel, uint32_t DispatchFlag)
{
  return AtDispatchLevel != 0U ? DispatchFlag : 0U;
}

static inline void
T70WwZeroDeviceMemory(volatile uint8_t *Destination, size_t Length)
{
  for (size_t Index = 0U; Index < Length; ++Index) {
    Destination[Index] = 0U;
  }
}

static inline void
T70WwCopyFromDeviceMemory(void *Destination,
                          const volatile uint8_t *Source,
                          size_t Length)
{
  uint8_t *Output = (uint8_t *)Destination;
  for (size_t Index = 0U; Index < Length; ++Index) {
    Output[Index] = Source[Index];
  }
}

#endif
