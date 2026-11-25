/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Host mock declaration only; not resource-hub formatting or Windows ABI. */
#ifndef T70_TEST_RESHUB_H
#define T70_TEST_RESHUB_H
#include <wdf.h>
#define RESOURCE_HUB_PATH_SIZE 128
#define IOCTL_RH_QUERY_CONNECTION_PROPERTIES 0x100
#define RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION 1
#define RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION 1
#define SERIAL_BUS_DESCRIPTOR 0x8e
#define SERIAL_BUS_DESCRIPTOR_REVISION 1
#define SERIAL_BUS_DESCRIPTOR_REVISION_V2 2
#define SERIAL_BUS_I2C_DESCRIPTOR_TYPE 1
enum {ConnectionIdType};
typedef struct {
 ULONG Version,QueryType;
 union {PHYSICAL_ADDRESS ConnectionId;} u;
} RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER;
typedef struct {
 ULONG Version,PropertiesLength;
 UCHAR ConnectionProperties[1];
} RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER;
#pragma pack(push,1)
typedef struct {
 UCHAR Tag;
 USHORT Length;
 UCHAR RevisionId,ResourceSourceIndex,SerialBusType,GeneralFlags;
 USHORT TypeSpecificFlags;
 UCHAR TypeSpecificRevisionId;
 USHORT TypeDataLength;
} PNP_SERIAL_BUS_DESCRIPTOR;
#pragma pack(pop)
#include <string.h>
static inline size_t TestConnectionDescriptor(void *Out, uint16_t Address) {
 const char Source[] = "\\_SB.I2T1";
 const ULONG Speed = 400000;
 const size_t Prefix = offsetof(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, ConnectionProperties);
 const size_t TypeBytes = sizeof(Speed) + sizeof(Address);
 PNP_SERIAL_BUS_DESCRIPTOR Descriptor = {
  SERIAL_BUS_DESCRIPTOR, sizeof(Descriptor) - 3 + TypeBytes + sizeof(Source),
  SERIAL_BUS_DESCRIPTOR_REVISION_V2, 0, SERIAL_BUS_I2C_DESCRIPTOR_TYPE, 2, 0,
  SERIAL_BUS_DESCRIPTOR_REVISION, TypeBytes
 };
 RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Reply = {
  RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION,
  sizeof(Descriptor) + TypeBytes + sizeof(Source), {0}
 };
 UCHAR *Bytes = Out;
 memcpy(Bytes, &Reply, Prefix);
 Bytes += Prefix;
 memcpy(Bytes, &Descriptor, sizeof(Descriptor));
 Bytes += sizeof(Descriptor);
 memcpy(Bytes, &Speed, sizeof(Speed));
 memcpy(Bytes + sizeof(Speed), &Address, sizeof(Address));
 memcpy(Bytes + TypeBytes, Source, sizeof(Source));
 return Prefix + Reply.PropertiesLength;
}
NTSTATUS RESOURCE_HUB_CREATE_PATH_FROM_ID(UNICODE_STRING *,ULONG,ULONG);
#endif
