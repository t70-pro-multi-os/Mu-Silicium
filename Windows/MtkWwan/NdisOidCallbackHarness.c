/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "NdisCallbackCore.h"
typedef uint32_t ULONG, NDIS_OID;
typedef uint16_t USHORT;
typedef uint8_t UCHAR;
typedef int32_t NDIS_STATUS;
typedef void VOID, *PVOID;
typedef uint32_t NDIS_MEDIUM, NDIS_PHYSICAL_MEDIUM, NDIS_HARDWARE_STATUS, NDIS_MEDIA_STATE;
typedef struct { ULONG Held; } NDIS_SPIN_LOCK;
typedef struct {
  struct { PVOID InformationBuffer; ULONG InformationBufferLength, BytesWritten, BytesNeeded; } QUERY_INFORMATION;
  struct { PVOID InformationBuffer; ULONG InformationBufferLength, BytesRead, BytesNeeded; } SET_INFORMATION;
} NDIS_OID_DATA;
typedef struct { NDIS_OID_DATA DATA; } NDIS_OID_REQUEST, *PNDIS_OID_REQUEST;
typedef struct { NDIS_SPIN_LOCK OidLock; ULONG PacketFilter, Lookahead; } T70_WWAN_ADAPTER;
#define NDIS_STATUS_SUCCESS ((NDIS_STATUS)0)
#define NDIS_STATUS_NOT_SUPPORTED ((NDIS_STATUS)-1)
#define NDIS_STATUS_BUFFER_TOO_SHORT ((NDIS_STATUS)-2)
#define NDIS_STATUS_INVALID_LENGTH ((NDIS_STATUS)-3)
#define NDIS_STATUS_INVALID_DATA ((NDIS_STATUS)-4)
#define OID_GEN_SUPPORTED_LIST 1U
#define OID_GEN_HARDWARE_STATUS 2U
#define OID_GEN_MEDIA_SUPPORTED 3U
#define OID_GEN_MEDIA_IN_USE 4U
#define OID_GEN_MAXIMUM_LOOKAHEAD 5U
#define OID_GEN_MAXIMUM_FRAME_SIZE 6U
#define OID_GEN_LINK_SPEED 7U
#define OID_GEN_TRANSMIT_BUFFER_SPACE 8U
#define OID_GEN_RECEIVE_BUFFER_SPACE 9U
#define OID_GEN_TRANSMIT_BLOCK_SIZE 10U
#define OID_GEN_RECEIVE_BLOCK_SIZE 11U
#define OID_GEN_VENDOR_ID 12U
#define OID_GEN_VENDOR_DESCRIPTION 13U
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010eU
#define OID_GEN_CURRENT_LOOKAHEAD 0x0001010fU
#define OID_GEN_MAXIMUM_TOTAL_SIZE 14U
#define OID_GEN_MAC_OPTIONS 15U
#define OID_GEN_MEDIA_CONNECT_STATUS 16U
#define OID_GEN_PHYSICAL_MEDIUM 17U
#define OID_GEN_DRIVER_VERSION 18U
#define OID_WWAN_DRIVER_CAPS 0x0e010101U
#define OID_WWAN_DEVICE_CAPS 0x0e010102U
#define NdisMediumWirelessWan 4U
#define NdisPhysicalMediumWirelessWan 8U
#define NdisHardwareStatusNotReady 5U
#define NdisMediaStateDisconnected 0U
#define NDIS_MAC_OPTION_NO_LOOPBACK 8U
#define NDIS_MINIPORT_MAJOR_VERSION 6U
#define NDIS_MINIPORT_MINOR_VERSION 30U
#define T70_WWAN_MTU 1500U
#define RtlCopyMemory(D,S,N) memcpy((D),(S),(N))
static const NDIS_OID gSupportedOids[]={OID_GEN_CURRENT_PACKET_FILTER};
static const char gVendorDescription[]="T70 callback harness";
static unsigned LockDepth,LockAcquires,LockReleases,Checks,Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure=__LINE__; } while (0)
static VOID NdisAcquireSpinLock(NDIS_SPIN_LOCK *Lock){CHECK(Lock&&!Lock->Held&&!LockDepth);Lock->Held=1;LockDepth++;LockAcquires++;}
static VOID NdisReleaseSpinLock(NDIS_SPIN_LOCK *Lock){CHECK(Lock&&Lock->Held&&LockDepth==1U);Lock->Held=0;LockDepth--;LockReleases++;}
/* PRODUCTION_CALLBACKS */
int main(void){
 T70_WWAN_ADAPTER Adapter={0};NDIS_OID_REQUEST Set={0},Query={0};ULONG Requested,Returned=0xfeedU;
 Requested=T70_WWAN_PACKET_FILTER_MULTICAST|T70_WWAN_PACKET_FILTER_BROADCAST;
 Set.DATA.SET_INFORMATION.InformationBuffer=&Requested;Set.DATA.SET_INFORMATION.InformationBufferLength=sizeof(Requested);
 CHECK(WwSetOid(&Adapter,&Set,OID_GEN_CURRENT_PACKET_FILTER)==NDIS_STATUS_SUCCESS);
 CHECK(Adapter.PacketFilter==Requested&&Set.DATA.SET_INFORMATION.BytesRead==sizeof(Requested));
 Query.DATA.QUERY_INFORMATION.InformationBuffer=&Returned;Query.DATA.QUERY_INFORMATION.InformationBufferLength=sizeof(Returned);
 CHECK(WwQueryOid(&Adapter,&Query,OID_GEN_CURRENT_PACKET_FILTER)==NDIS_STATUS_SUCCESS);
 CHECK(Returned==Requested&&Query.DATA.QUERY_INFORMATION.BytesWritten==sizeof(Returned));
 Requested=0x10U;Set.DATA.SET_INFORMATION.BytesRead=0U;
 CHECK(WwSetOid(&Adapter,&Set,OID_GEN_CURRENT_PACKET_FILTER)==NDIS_STATUS_NOT_SUPPORTED);
 CHECK(Adapter.PacketFilter==(T70_WWAN_PACKET_FILTER_MULTICAST|T70_WWAN_PACKET_FILTER_BROADCAST)&&Set.DATA.SET_INFORMATION.BytesRead==0U);
 Requested=0U;
 CHECK(WwSetOid(&Adapter,&Set,OID_GEN_CURRENT_PACKET_FILTER)==NDIS_STATUS_SUCCESS&&Adapter.PacketFilter==0U);
 Requested=0U;CHECK(WwSetOid(&Adapter,&Set,OID_GEN_CURRENT_LOOKAHEAD)==NDIS_STATUS_SUCCESS&&Adapter.Lookahead==0U);
 Requested=1U;CHECK(WwSetOid(&Adapter,&Set,OID_GEN_CURRENT_LOOKAHEAD)==NDIS_STATUS_INVALID_DATA&&Adapter.Lookahead==0U);
 Set.DATA.SET_INFORMATION.InformationBufferLength=sizeof(ULONG)-1U;
 CHECK(WwSetOid(&Adapter,&Set,OID_GEN_CURRENT_PACKET_FILTER)==NDIS_STATUS_INVALID_LENGTH&&Set.DATA.SET_INFORMATION.BytesNeeded==sizeof(ULONG));
 Set.DATA.SET_INFORMATION.InformationBuffer=NULL;Set.DATA.SET_INFORMATION.InformationBufferLength=sizeof(ULONG);
 CHECK(WwSetOid(&Adapter,&Set,OID_GEN_CURRENT_PACKET_FILTER)==NDIS_STATUS_INVALID_LENGTH);
 CHECK(WwSetOid(&Adapter,&Set,0xffffffffU)==NDIS_STATUS_NOT_SUPPORTED);
 CHECK(WwQueryOid(&Adapter,&Query,OID_WWAN_DEVICE_CAPS)==NDIS_STATUS_NOT_SUPPORTED);
 CHECK(LockDepth==0U&&LockAcquires==LockReleases);
 if(Failure){fprintf(stderr,"NDIS OID production callback failed at line %u after %u checks\n",Failure,Checks);return (int)Failure;}
 printf("NDIS WwSetOid/WwQueryOid production callbacks passed: %u checks\n",Checks);return 0;
}
