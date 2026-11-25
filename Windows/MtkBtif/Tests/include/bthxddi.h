#ifndef BTIF_TEST_BTHXDDI_H
#define BTIF_TEST_BTHXDDI_H
#include <ntddk.h>
#define BTHX_DDI_VERSION_1 1U
typedef enum { HciPacketCommand=1, HciPacketAclData=2, HciPacketEvent=4 } BTHX_HCI_PACKET_TYPE;
#define IOCTL_BTHX_GET_VERSION 0x100U
#define IOCTL_BTHX_SET_VERSION 0x101U
#define IOCTL_BTHX_QUERY_CAPABILITIES 0x102U
#define IOCTL_BTHX_WRITE_HCI 0x103U
#define IOCTL_BTHX_READ_HCI 0x104U
typedef struct { ULONG Version; } BTHX_VERSION;
typedef enum { ScoSupportNone=0, ScoSupportHCI=1, ScoSupportHCIBypass=2 } BTHX_SCO_SUPPORT;
typedef struct {
    ULONG MaxAclTransferInSize;
    BTHX_SCO_SUPPORT ScoSupport;
    ULONG MaxScoChannels;
    BOOLEAN IsDeviceIdleCapable;
    BOOLEAN IsDeviceWakeCapable;
} BTHX_CAPABILITIES;
#pragma pack(push,1)
typedef struct {
    ULONG DataLen;
    UCHAR Type;
    UCHAR Data[1];
} BTHX_HCI_READ_WRITE_CONTEXT, *PBTHX_HCI_READ_WRITE_CONTEXT;
#pragma pack(pop)
#endif
