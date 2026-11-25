/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NdisCallbackCore.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

int main(void)
{
  uint8_t Storage[80], Output[67], DeviceMemory[67];
  for (size_t I = 0U; I < sizeof(Storage); ++I) Storage[I] = (uint8_t)(I ^ 0xA5U);
  memset(Output, 0, sizeof(Output));

  CHECK(T70WwPacketFilterValid(0U));
  CHECK(T70WwPacketFilterValid(T70_WWAN_PACKET_FILTER_SUPPORTED));
  CHECK(T70WwPacketFilterValid(T70_WWAN_PACKET_FILTER_DIRECTED | T70_WWAN_PACKET_FILTER_BROADCAST));
  CHECK(!T70WwPacketFilterValid(0x10U));
  uint32_t PacketFilter = 0U;
  CHECK(T70WwPacketFilterStore(T70_WWAN_PACKET_FILTER_MULTICAST |
      T70_WWAN_PACKET_FILTER_BROADCAST, &PacketFilter));
  CHECK(PacketFilter == (T70_WWAN_PACKET_FILTER_MULTICAST |
      T70_WWAN_PACKET_FILTER_BROADCAST));
  CHECK(!T70WwPacketFilterStore(0x10U, &PacketFilter));
  CHECK(PacketFilter == (T70_WWAN_PACKET_FILTER_MULTICAST |
      T70_WWAN_PACKET_FILTER_BROADCAST));
  CHECK(!T70WwPacketFilterStore(0U, NULL));
  CHECK(T70WwSendCompleteFlags(0U, 0x80U) == 0U);
  CHECK(T70WwSendCompleteFlags(1U, 0x80U) == 0x80U);
  CHECK(T70WwSendCompleteFlags(0U, 0x40U) == 0U);
  T70WwCopyFromDeviceMemory(Output + 1U, Storage + 3U, 64U);
  CHECK(Output[0] == 0U && Output[65] == 0U && Output[66] == 0U);
  CHECK(memcmp(Output + 1U, Storage + 3U, 64U) == 0);
  memset(DeviceMemory,0xa5,sizeof(DeviceMemory));
  T70WwZeroDeviceMemory(DeviceMemory+1U,64U);
  CHECK(DeviceMemory[0]==0xa5U&&DeviceMemory[65]==0xa5U&&DeviceMemory[66]==0xa5U);
  for(size_t I=1U;I<65U;I++)CHECK(DeviceMemory[I]==0U);

  if (Failure != 0U) {
    fprintf(stderr, "NdisCallbackCoreTest failed at line %u after %u checks\n",
            Failure, Checks);
    return (int)Failure;
  }
  printf("T70 NDIS callback flag and unaligned device-copy checks passed: %u\n",
         Checks);
  return 0;
}
