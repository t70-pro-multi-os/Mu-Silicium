// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_WINDOWS_FRAMEBUFFER_H
#define MTK_WINDOWS_FRAMEBUFFER_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MTK_FB_MAX_BYTES (64U * 1024U * 1024U)
#define MTK_FB_MAX_RECTS 4096U
/* ACPI DFBI wire layout, little endian, exactly 48 bytes. */
typedef struct {
    uint32_t Ready, Revision;
    uint64_t Base, RegionSize;
    uint32_t Width, Height, Pitch, Format;
    uint64_t Reserved;
} MTK_FB_HANDOFF;
typedef struct { int32_t Left, Top, Right, Bottom; } MTK_FB_RECT;
typedef struct { int32_t X, Y; MTK_FB_RECT Dest; } MTK_FB_MOVE;
int MtkFbValidateHandoff(const MTK_FB_HANDOFF *h);
/* Rotation 0..3 means 0,90,180,270 degrees counter-clockwise (D3DKMDT). */
int MtkFbValidatePresent(uint32_t dw, uint32_t dh, uint32_t dp, size_t dn,
    uint32_t sw, uint32_t sh, uint32_t sp, size_t sn, unsigned rotation,
    const MTK_FB_MOVE *moves, uint32_t nm, const MTK_FB_RECT *rects, uint32_t nr);
int MtkFbPresent(void *dst, uint32_t dw, uint32_t dh, uint32_t dp, size_t dn,
    const void *src, uint32_t sw, uint32_t sh, uint32_t sp, size_t sn,
    unsigned rotation, const MTK_FB_MOVE *moves, uint32_t nm,
    const MTK_FB_RECT *rects, uint32_t nr);
int MtkFbWrite(void *dst, uint32_t dw, uint32_t dh, uint32_t dp, size_t dn,
    const void *src, uint32_t sw, uint32_t sh, uint32_t sp, size_t sn,
    int32_t x, int32_t y);
#ifdef __cplusplus
}
#endif
#endif
