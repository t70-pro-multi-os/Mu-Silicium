// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_PAIR_SERIAL_H
#define T70_AW_PAIR_SERIAL_H
#include "AwPairProfile.h"
#include "AwSerialDecode.h"
typedef struct {
 uint16_t Profile;
 T70_AW_SERIAL_FIELDS Channel[2];
} T70_AW_PAIR_SERIAL;
/* Immutable ACF for the entire call; valid aligned writable Out disjoint from
 * ACF. Both channel views are validated before decoding their selected profile.
 * Each of 06/07/08/13 must occur exactly once. Failure preserves all output.
 * Result owns copied values, not borrowed pointers. No I/O or live observations;
 * digital channel codes do not identify speaker orientation, AFE compatibility,
 * applied configuration, or playback readiness. Unexamined bits remain opaque.
 * Returns 1/0. Caller owns memory lifetime and serialization. */
int T70AwPairSerialObserve(const void *Acf,size_t Bytes,uint64_t Base,
 uint16_t Address0,uint16_t Address1,uint16_t Profile,T70_AW_PAIR_SERIAL *Out);
#endif
