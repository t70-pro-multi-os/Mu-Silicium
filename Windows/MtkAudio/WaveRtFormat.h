// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_FORMAT_H
#define T70_WAVERT_FORMAT_H
#include "WavePrepare.h"
/* Consume a retained, immutable KS render format after a real owner supplies
 * DMA geometry. NOT NewStream itself (NewStream precedes buffer allocation).
 * No-attribute packed KS descriptor: 82-byte PCM WAVEFORMATEX or 104-byte
 * WAVEFORMATEXTENSIBLE. Plain PCM accepts only S16 mono/stereo; extensible
 * retains its existing validation. SampleSize may be zero or frame size.
 * PCM cbSize is ignored as implicitly zero; no appended bytes are consumed.
 * Other standard formats remain outside this direct-buffer subset. 24/32 conversion is deliberately not advertised as a WaveRT path.
 * Caller guarantees readable Bytes and disjoint aligned writable Out, with
 * serialized lifetime; passing validates metadata, not MDL/cache/DMA ownership,
 * stream creation, endpoint support, pin assignment or hardware readiness.
 * Failure preserves Out, including alias/range failure. No retained pointers. */
T70_WAVE_PREPARE_RESULT T70WaveRtPrepareBufferFormat(const void *KsFormat,size_t Bytes,
 uint64_t DmaBase,uint64_t BufferBytes,uint64_t PeriodBytes,T70_AFE_PREPARED *Out);
#endif
