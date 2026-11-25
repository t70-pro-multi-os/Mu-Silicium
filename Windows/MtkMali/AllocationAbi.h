/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_ALLOCATION_ABI_H
#define T70_MALI_ALLOCATION_ABI_H
#include <stdint.h>
#define MALI_ALLOCATION_VERSION 1u
#define MALI_ALLOCATION_BUFFER 1u
#define MALI_ALLOCATION_LINEAR_2D 2u
#define MALI_ALLOCATION_CPU_VISIBLE 1u
#define MALI_ALLOCATION_SHARE_BACKING 2u
#define MALI_ALLOCATION_BATCH 256u
#define MALI_ALLOCATION_LIMIT 4096u
#define MALI_ALLOCATION_MAX_BYTES (UINT64_C(1)<<30)
/* Untrusted UMD wire data. No native pointers or handles in this ABI.
   LINEAR_2D Size is Pitch*Height or that value rounded up to 4 KiB.
   Explicit page padding is required when binding the full GPU page range;
   no bytes beyond the declared Size are implicitly made accessible. */
typedef struct {
    uint32_t Version, Bytes, Kind, Flags;
    uint64_t Size;
    uint32_t Width, Height, Pitch, Format;
} MALI_ALLOCATION_DESC;
#endif
