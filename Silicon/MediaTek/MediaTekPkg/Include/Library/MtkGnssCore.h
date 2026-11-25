// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_GNSS_CORE_H
#define MTK_GNSS_CORE_H
#include <stdint.h>
/* Inner receiver framing recovered from this board's libmnl.so, not STP or
 * NMEA. The stock primary input ring is 512 bytes. Keep its whole decoded
 * frame within that size; do not inherit its wrapping/overrun behavior. */
#define MTK_GNSS_FRAME_MAX 512U
#define MTK_GNSS_PAYLOAD_MAX (MTK_GNSS_FRAME_MAX-10U)
#define MTK_GNSS_WIRE_MAX (2U*(MTK_GNSS_FRAME_MAX-4U)+4U)
typedef enum {
  MtkGnssOk, MtkGnssInvalid, MtkGnssNeedMore, MtkGnssFrameReady,
  MtkGnssMalformed, MtkGnssFaulted, MtkGnssCapacity, MtkGnssStale,
  MtkGnssUnrelated
} MTK_GNSS_STATUS;
typedef struct {
  unsigned State,Used,Expected,Initialized,Faulted;
  unsigned char Buffer[MTK_GNSS_FRAME_MAX];
} MTK_GNSS_DECODER;
typedef struct {
  unsigned Type,Flags,Length;
  const unsigned char *Data,*Raw;
  unsigned RawLength;
} MTK_GNSS_FRAME;
/* Software only. Reset only after the owner has drained/restarted the stream
 * and invalidated old STP queue callbacks. Init wipes any previous frame. */
MTK_GNSS_STATUS MtkGnssDecoderInit(MTK_GNSS_DECODER *Decoder);
/* Consume at most one complete, length/checksum-verified frame. Skip initial
 * noise; preserve split escapes, markers and data across calls/STP boundaries.
 * Frame is unchanged except on FrameReady; its pointers expire on the next
 * Receive/Init. Malformed/overflow input wipes and latches until owner recovery.
 * Type and the high four length bits (Flags) remain opaque. Serialize all calls;
 * arguments must not alias Decoder, each other, or its published frame data.
 * This boundary does not run the position engine or publish a location. */
MTK_GNSS_STATUS MtkGnssReceive(MTK_GNSS_DECODER *Decoder,
  const unsigned char *Data,unsigned Length,unsigned *Consumed,MTK_GNSS_FRAME *Frame);
/* Exact stock descriptor/escaping/checksum for a caller-owned command type.
 * No type is interpreted and no command is automatically sent. Length 0..502;
 * NULL input is allowed only for zero length. Disjoint input/output required.
 * Capacity failure writes no prefix; Written is zero on every failure. */
MTK_GNSS_STATUS MtkGnssEncode(unsigned Type,const unsigned char *Data,
  unsigned Length,unsigned char *Output,unsigned Capacity,unsigned *Written);
/* Separate MCU/offload port framing from libmnl's pack_data/nmea_recv path.
 * The owner must establish the endpoint/mode; these are not STP GPS bytes. */
#define MTK_GNSS_OFFLOAD_PART_MAX 1532U
#define MTK_GNSS_OFFLOAD_MESSAGE_MAX 65535U
typedef struct {
  unsigned char *Buffer;
  unsigned Capacity,Used,Total,Next,Epoch,Faulted;
} MTK_GNSS_OFFLOAD;
typedef struct {
  const unsigned char *Data;
  unsigned Length;
} MTK_GNSS_MESSAGE;
/* First init only, with a disjoint owner buffer of 1..65535 bytes whose lifetime
 * covers all calls. Use Reset thereafter; Init may reuse epoch 1 only after all
 * old callbacks are destroyed. Each port needs its own state. Serialize calls. */
MTK_GNSS_STATUS MtkGnssOffloadInit(MTK_GNSS_OFFLOAD *State,
  unsigned char *Buffer,unsigned Capacity);
/* Owner recovery after cancel/drain, timeout, suspend, reset or port close.
 * Wipes bytes and invalidates old completions; epoch wrap is terminal. */
MTK_GNSS_STATUS MtkGnssOffloadReset(MTK_GNSS_OFFLOAD *State,unsigned *Epoch);
/* One complete endpoint fragment per call: LE16 length including 4-byte header,
 * U8 total and U8 index (1-based), then 0..1532 data bytes. Enforce actual length,
 * consistent total, ordered exactly-once fragments and bounded assembled size.
 * Never auto-detect a port from payload contents. On the established NMEA port,
 * FrameReady bytes are the stock output boundary, not a validated position.
 * Message changes only on FrameReady; data expires on the next current-epoch
 * Receive/Reset. Stale callbacks neither read input nor mutate the current state.
 * Malformed/capacity input wipes and latches until Reset. Inputs, outputs, state
 * and owner buffer must be disjoint. No allocation, I/O or callback occurs here. */
MTK_GNSS_STATUS MtkGnssOffloadReceive(MTK_GNSS_OFFLOAD *State,unsigned Epoch,
  const unsigned char *Data,unsigned Length,MTK_GNSS_MESSAGE *Message);
/* Decoded stock Android consumer records, NOT receiver/STP wire messages.
 * IEEE binary64/binary32 fields are carried as integer bits: consumers copy the
 * bits into their OS values. No floating-point instruction/state is required.
 * Location validity bits 0..7: lat/lon, WGS84 ellipsoid altitude, speed, bearing,
 * horizontal/vertical/speed/bearing accuracy (68% confidence). Units: degrees,
 * metres, metres/sec. Elapsed validity bits 0/1: timestamp/uncertainty in ns.
 * These decoders are stateless and make no freshness, fix or RF assertion. */
#define MTK_GNSS_LOCATION_RECORD_BYTES 104U
#define MTK_GNSS_SV_RECORD_BYTES 48U
#define MTK_GNSS_MCU_POSITION_BYTES 3168U
typedef struct {
  uint32_t Flags,Reserved;
  uint64_t LatitudeBits,LongitudeBits,AltitudeBits,SpeedBits,BearingBits;
  uint64_t HorizontalAccuracyBits,VerticalAccuracyBits;
  uint64_t SpeedAccuracyBits,BearingAccuracyBits;
  int64_t UtcMilliseconds;
  uint32_t ElapsedFlags,Reserved2;
  uint64_t ElapsedTimestampNs,ElapsedUncertaintyBits;
} MTK_GNSS_LOCATION;
typedef struct {
  int32_t Svid,Constellation;
  uint32_t Cn0Bits,BasebandCn0Bits,ElevationBits,AzimuthBits;
  uint64_t CarrierFrequencyHz;
  uint32_t Flags,Reserved;
} MTK_GNSS_SATELLITE;
/* Exactly 104 bytes from the verified GpsLocation_ext consumer boundary.
 * Ignore opaque size/padding words; validate flags and each PRESENT value.
 * Unavailable fields are zero, including unavailable elapsed timing. UTC is
 * preserved as signed milliseconds; epoch conversion/freshness is owner policy.
 * Reject unknown flags, nonfinite/range errors and uncertainty without timestamp.
 * Output is unchanged on failure. All inputs/outputs must be disjoint. */
MTK_GNSS_STATUS MtkGnssDecodeLocation(const unsigned char *Data,unsigned Length,
  MTK_GNSS_LOCATION *Location);
/* GnssSvStatus_ext: 16-byte header (count signed LE32 at +8), 48-byte entries.
 * Header/entry size words remain opaque, as in the verified stock consumer.
 * Bound count by ACTUAL supplied bytes and caller capacity; a fixed-size stock
 * array may have unused trailing entries. Validate every selected entry before
 * writing any output. Count/output are unchanged on failure. Zero count accepts
 * NULL Satellites. Repeated SVIDs on distinct bands are retained in input order.
 * Floats are bits; carrier Hz uses the stock truncation towards zero. */
MTK_GNSS_STATUS MtkGnssDecodeSvStatus(const unsigned char *Data,unsigned Length,
  MTK_GNSS_SATELLITE *Satellites,unsigned Capacity,unsigned *Count);
/* Established MCU-control endpoint, AFTER OffloadReceive FrameReady. Kind 2
 * carries LE16 kind/length, opaque LE16, then a 3168-byte position record.
 * Other kinds return Unrelated. No record fields are guessed here: this is NOT
 * the 104-byte Android consumer record and must never enter DecodeLocation.
 * Position is a view with the input message's lifetime; outputs change only on
 * Ok. The owner must reject stale epochs before calling and serialize use. */
MTK_GNSS_STATUS MtkGnssMcuPosition(const MTK_GNSS_MESSAGE *Message,
  MTK_GNSS_MESSAGE *Position,unsigned *Opaque);
/* Complete verified NMEA OUTPUT sentences only; never raw receiver input.
 * A local 256-byte bound allows high-precision output beyond classic 82 bytes.
 * RMC/GGA parser is stateless: it never borrows date/altitude from older fixes. */
#define MTK_GNSS_NMEA_MAX 256U
#define MTK_GNSS_NMEA_UTC 1U
#define MTK_GNSS_NMEA_MSL 2U
#define MTK_GNSS_NMEA_GEOID 4U
#define MTK_GNSS_NMEA_HDOP 8U
#define MTK_GNSS_NMEA_LEAP_SECOND 16U
#define MTK_GNSS_NMEA_TIME_OF_DAY 32U
typedef struct {
  MTK_GNSS_LOCATION Location;
  unsigned Kind,AuxFlags,TimeOfDayMilliseconds,Quality,SatellitesUsed;
  unsigned Year,Month,Day,Mode;
  uint64_t HdopBits,MslAltitudeBits,GeoidSeparationBits;
  uint64_t TimeOfDayNanoseconds;
  int64_t LatitudeNanominutes,LongitudeNanominutes;
  unsigned LatitudeStepNanominutes,LongitudeStepNanominutes;
} MTK_GNSS_NMEA;
/* Kind 1=RMC, 2=GGA. Mandatory XOR checksum and CRLF; decimal precision <=9.
 * Century is an explicit trusted owner policy (1900..9900, multiple of 100)
 * for RMC's two-digit year, not a guessed pivot. GGA has no absolute date/UTC.
 * Only UTC-flagged reports can supply an absolute Location.UtcMilliseconds.
 * A 23:59:60 timestamp is normalized to next POSIX midnight and additionally
 * marked LEAP_SECOND; owners must retain this ambiguity when comparing fixes.
 * Invalid RMC status V/mode N or GGA quality 0 returns a report with no position
 * flags. GGA's ellipsoid altitude requires BOTH MSL and geoid metres; HDOP is
 * preserved as dimensionless and is never invented into metre accuracy.
 * Other checksum-valid sentences return Unrelated. No partial output on error.
 * Disjoint, stable input/output required. The stream API below can handle line
 * assembly, epochs and pairing. Owner supplies endpoint and absolute fix age. */
MTK_GNSS_STATUS MtkGnssNmeaParse(const unsigned char *Data,unsigned Length,
  unsigned Century,MTK_GNSS_NMEA *Report);
/* One explicitly established NMEA output stream per state. No allocation/I/O.
 * Report precision metadata keeps different sub-ms fixes and differing decimal
 * coordinate resolutions distinct; it does not describe positioning accuracy. */
typedef struct {
  unsigned Century,PairWindowMs,Epoch,Faulted,Used,Initialized,ClockValid;
  unsigned HaveRmc,HaveGga,HaveChronology;
  uint64_t LastSeenMs,LineStartMs,RmcAtMs,GgaAtMs;
  int64_t LastRmcDay;
  uint64_t LastRmcTimeNs;
  MTK_GNSS_NMEA Rmc,Gga;
  unsigned char Line[MTK_GNSS_NMEA_MAX];
} MTK_GNSS_NMEA_STREAM;
/* First init only. PairWindowMs is an explicit arrival/partial-line budget of
 * 1..60000 ms; Century follows NmeaParse. State is private to a serialized owner.
 * Re-init may reuse epoch 1 only after ALL old completions are destroyed. */
MTK_GNSS_STATUS MtkGnssNmeaStreamInit(MTK_GNSS_NMEA_STREAM *State,
  unsigned Century,unsigned PairWindowMs);
/* After cancel/drain, timeout, suspend or close: wipe line, both cached records
 * and chronological watermarks; invalidate old callbacks. Allows an explicit
 * century update. Epoch UINT32_MAX is terminal and leaves Epoch output intact. */
MTK_GNSS_STATUS MtkGnssNmeaStreamReset(MTK_GNSS_NMEA_STREAM *State,
  unsigned Century,unsigned *Epoch);
/* At most 256 bytes and one complete sentence per call, with exact Consumed.
 * Honor partial consumption for joined lines. Skip empty CR/LF bytes only.
 * Unknown checksum-valid sentences return Unrelated without changing Report.
 * Kind 3 reports merge one RMC/GGA pair, ONLY at identical nanosecond UTC time
 * of day, overlapping coordinate half-last-digit intervals, and within the
 * owner arrival budget. UTC/date/speed/bearing stay from RMC; altitude, HDOP,
 * quality and SV count come from GGA. The pair is then wiped, not reused.
 * Void/quality-zero clears both caches. Different times/positions stay separate.
 * A valid RMC's calendar day/time cannot move backwards within an epoch.
 * NowMs is an owner monotonic arrival clock, not an absolute fix-age assertion.
 * NULL/zero input ticks expiry. Expired caches are wiped; partial-line timeout,
 * clock regression, malformed/overlong lines wipe and latch until owner Reset.
 * Stale epochs cannot read input, change state or Report (Consumed becomes 0).
 * All inputs/outputs/state must be disjoint; aliased arguments return Invalid
 * before any writes. Report changes only on FrameReady. No OS location publish,
 * automatic command, physical receiver/RF action or endpoint guessing occurs. */
MTK_GNSS_STATUS MtkGnssNmeaStreamReceive(MTK_GNSS_NMEA_STREAM *State,
  unsigned Epoch,uint64_t NowMs,const unsigned char *Data,unsigned Length,
  unsigned *Consumed,MTK_GNSS_NMEA *Report);
#endif
