// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_TEST_KSMEDIA_H
#define T70_TEST_KSMEDIA_H
#include <ks.h>
#define STATIC_KSDATAFORMAT_TYPE_AUDIO 0x73647561,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}
#define STATIC_KSDATAFORMAT_SUBTYPE_PCM 1,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}
#define STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX 0x05589f81,0xc356,0x11ce,{0xbf,1,0,0xaa,0,0x55,0x59,0x5a}
/* Shape mock only; official header assertions are separately compiled. */
#pragma pack(push,1)
typedef struct {uint8_t Bytes[18];} WAVEFORMATEX;
typedef struct {KSDATAFORMAT DataFormat;WAVEFORMATEX WaveFormatEx;} KSDATAFORMAT_WAVEFORMATEX;
#pragma pack(pop)
typedef struct {uint8_t Bytes[40];} WAVEFORMATEXTENSIBLE;
typedef struct {KSDATAFORMAT DataFormat;WAVEFORMATEXTENSIBLE WaveFormatExt;} KSDATAFORMAT_WAVEFORMATEXTENSIBLE;
#endif
