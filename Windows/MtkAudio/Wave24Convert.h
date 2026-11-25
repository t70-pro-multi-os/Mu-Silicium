// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVE24_CONVERT_H
#define T70_WAVE24_CONVERT_H
#include <stddef.h>
#include <stdint.h>
/* Convert interleaved little-endian Windows PCM 24-valid-in-32 samples into
 * ALSA S24_LE four-byte containers. Channels must be 1 or 2; complete frames
 * only. Low source padding byte must be zero. Destination high byte is sign
 * extension, while the low three bytes retain the exact signed 24-bit value.
 * Disjoint source/destination only (including in-place rejection). Capacity is
 * writable ordinary CPU memory, not MMIO. Caller owns immutable source and
 * exclusive destination for both validation and conversion passes; pointer
 * range checks do not establish allocation validity. No allocation, hardware,
 * cache synchronization, format discovery or prepared snapshot is performed.
 * Return 1 on conversion, 0 on invalid input with destination unchanged.
 * Zero bytes accepts NULL buffers and writes nothing, but still checks channel
 * count. No output beyond Bytes is touched. */
int T70Wave24ToS24Le(const void *Source, size_t Bytes, uint32_t Channels,
                    void *Destination, size_t Capacity);
#endif
