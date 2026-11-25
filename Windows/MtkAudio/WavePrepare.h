// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVE_PREPARE_H
#define T70_WAVE_PREPARE_H
#include <stddef.h>
#include "AfePrepare.h"
typedef enum {T70WaveInvalid,T70WaveNeeds24BitNormalization,T70WavePrepared}
 T70_WAVE_PREPARE_RESULT;
/* Strict little-endian WAVEFORMATEXTENSIBLE byte descriptor, exactly 40 bytes
 * with cbSize=22 and integer PCM GUID. Not a RIFF file or native struct cast.
 * Mono/front-center or stereo/front-left+right only; these are input layout
 * policies, not evidence of T70 speaker routing. S16/16 prepares existing DL1.
 * Valid 24/32 is recognized but returns Needs24BitNormalization without changing
 * Out: Windows left-aligned 24-bit samples must NOT be blindly copied as the
 * existing S24_LE format. No conversion is performed. S32/float/packed24 and
 * unknown extra extensions are rejected. Windows supports more than this subset.
 * Block align and byte rate must be exact; geometry and rates use existing
 * prepare validation. All non-Prepared results preserve Out. Caller provides
 * valid immutable bytes and disjoint writable Out; no allocation/hardware I/O.
 * Prepared is only numeric frontend metadata, never playback compatibility. */
T70_WAVE_PREPARE_RESULT T70WaveExtensiblePrepare(const void *Descriptor,size_t Bytes,
 uint64_t DmaBase,uint64_t BufferBytes,uint64_t PeriodBytes,T70_AFE_PREPARED *Out);
#endif
