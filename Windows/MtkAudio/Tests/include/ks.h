// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_TEST_KS_H
#define T70_TEST_KS_H
#include <stdint.h>
typedef struct {uint32_t Data1;uint16_t Data2,Data3;uint8_t Data4[8];} GUID;
typedef struct {uint32_t FormatSize,Flags,SampleSize,Reserved;GUID MajorFormat,SubFormat,Specifier;} KSDATAFORMAT,*PKSDATAFORMAT;
#endif
