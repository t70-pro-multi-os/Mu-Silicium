// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_GNSS_RECEIVER_H
#define MTK_GNSS_RECEIVER_H
#include <Library/MtkGnssCore.h>
/* Private Windows child-PDO protocol, not a firmware port or an Android ABI.
 * The lower conninfra/receiver owner must implement these before this UMDF
 * consumer can start. No root device or synthetic receiver is supplied.
 * QUERY never powers RF. COMMAND enable grants permission; only START starts.
 * Successful START/MODIFY returns a new, nonzero, strictly increasing provider lease within Generation.
 * POLL is nonblocking (NO_MORE_ENTRIES when empty), and returns one atomic fix
 * captured under that lease, not cached values from another fix/session.
 * STOP/disable/RESET confirms engine/RF quiescence before success. The provider
 * also stops on front-end/process removal, even if no STOP can be delivered.
 * Failure/timeout may be uncertain; the consumer quarantines until reset.
 */
#define MTK_RX_VERSION 1U
#define MTK_RX_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN,0x900,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define MTK_RX_COMMAND CTL_CODE(FILE_DEVICE_UNKNOWN,0x901,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define MTK_RX_START CTL_CODE(FILE_DEVICE_UNKNOWN,0x902,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define MTK_RX_MODIFY CTL_CODE(FILE_DEVICE_UNKNOWN,0x903,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define MTK_RX_STOP CTL_CODE(FILE_DEVICE_UNKNOWN,0x904,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define MTK_RX_POLL CTL_CODE(FILE_DEVICE_UNKNOWN,0x905,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define MTK_RX_MANDATORY_COMMANDS 7U /* location enable, engine reset, aid clear */
#define MTK_RX_UTC 1U
#define MTK_RX_MSL 2U
#define MTK_RX_HDOP 4U
#define MTK_RX_HORIZONTAL_95 8U
#define MTK_RX_SATELLITES 16U
#define MTK_RX_FINAL 32U
#define MTK_RX_MAX_AGE_MS 5000U
#define MTK_RX_FUTURE_MS 1000U
#define MTK_RX_NAN UINT64_C(0x7ff8000000000000)
typedef struct {
  ULONG Size,Version,Commands,Format;
  ULONGLONG Generation;
  GNSS_CHIPSETINFO Chipset;
} MTK_RX_INFO;
typedef struct {
  ULONGLONG Lease;
  GNSS_FIXSESSION_PARAM Session;
} MTK_RX_MODIFY_PARAM;
typedef struct {
  ULONG Size,Version;
  ULONGLONG Generation,Lease,Sequence;
  ULONG Flags,SatelliteCount;
  MTK_GNSS_LOCATION Location;
  uint64_t MslAltitudeBits,HdopBits,HorizontalAccuracy95Bits;
  MTK_GNSS_SATELLITE Satellites[GNSS_MAXSATELLITE];
} MTK_RX_FIX;
/* Source Android accuracy is 68% confidence; never label it 95%. Only the
 * separately established HORIZONTAL_95 value may populate Windows accuracy.
 * Satellite identities are (Android constellation << 16) | SVID, an explicit
 * unique "other identification" allowed by GNSS_SATELLITEINFO. Multiple bands
 * of one satellite coalesce with used-in-fix OR and the strongest C/N0.
 */
C_ASSERT(sizeof(MTK_RX_FIX)==2736);
#endif
