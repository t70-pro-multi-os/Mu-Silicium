// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Framebuffer.h"
#include <string.h>
static int Layout(uint32_t w, uint32_t h, uint32_t p, size_t n) {
    return w && h && w <= 4096 && h <= 4096 && p >= w * 4 && !(p & 3) &&
        p <= 65536 && (uint64_t)p*h <= n && n <= MTK_FB_MAX_BYTES;
}
int MtkFbValidateHandoff(const MTK_FB_HANDOFF *h) {
    return h && sizeof(*h) == 48 && h->Ready == 1 && h->Revision == 1 &&
        h->Base >= 0x40000000ULL && !(h->Base & 4095) &&
        h->Base < (1ULL << 36) && h->RegionSize <= (1ULL << 36)-h->Base &&
        h->RegionSize <= MTK_FB_MAX_BYTES &&
        !(h->Base < 0x43f80000ULL && h->Base+h->RegionSize > 0x43f7f000ULL) &&
        h->Format == 1 && !h->Reserved &&
        Layout(h->Width,h->Height,h->Pitch,(size_t)h->RegionSize);
}
static int Rect(const MTK_FB_RECT *r, uint32_t w, uint32_t h) {
    return r->Left >= 0 && r->Top >= 0 && r->Right >= r->Left &&
        r->Bottom >= r->Top && (uint32_t)r->Right <= w && (uint32_t)r->Bottom <= h;
}
int MtkFbValidatePresent(uint32_t dw, uint32_t dh, uint32_t dp, size_t dn,
    uint32_t sw, uint32_t sh, uint32_t sp, size_t sn, unsigned rotation,
    const MTK_FB_MOVE *moves, uint32_t nm, const MTK_FB_RECT *rects, uint32_t nr) {
    uint32_t i;
    if (!Layout(dw,dh,dp,dn) || !Layout(sw,sh,sp,sn) || rotation > 3 ||
        ((rotation & 1) ? (sw != dh || sh != dw) : (sw != dw || sh != dh)) ||
        nm > MTK_FB_MAX_RECTS || nr > MTK_FB_MAX_RECTS-nm ||
        (nm && !moves) || (nr && !rects)) return 0;
    for (i=0;i<nm;i++) {
        const MTK_FB_MOVE *m = &moves[i];
        if (!Rect(&m->Dest,sw,sh) || m->X < 0 || m->Y < 0 ||
            (uint64_t)(uint32_t)m->X + (uint32_t)(m->Dest.Right-m->Dest.Left) > sw ||
            (uint64_t)(uint32_t)m->Y + (uint32_t)(m->Dest.Bottom-m->Dest.Top) > sh) return 0;
    }
    for (i=0;i<nr;i++) if (!Rect(&rects[i],sw,sh)) return 0;
    return 1;
}
static int Separate(const void *d, size_t dn, const void *s, size_t sn) {
    uintptr_t a=(uintptr_t)d,b=(uintptr_t)s;
    return a && b && dn <= UINTPTR_MAX-a && sn <= UINTPTR_MAX-b &&
        (a+dn <= b || b+sn <= a);
}
static void Copy(uint8_t *d, uint32_t dw, uint32_t dh, uint32_t dp,
    const uint8_t *s, uint32_t sp, unsigned rot, const MTK_FB_RECT *r) {
    int32_t x,y;
    for (y=r->Top;y<r->Bottom;y++) {
        if (!rot) {
            memcpy(d+(size_t)y*dp+(size_t)r->Left*4,
                   s+(size_t)y*sp+(size_t)r->Left*4,(size_t)(r->Right-r->Left)*4);
            continue;
        }
        for (x=r->Left;x<r->Right;x++) {
            uint32_t dx,dy;
            if (rot==1) { dx=(uint32_t)y; dy=dh-1-(uint32_t)x; }
            else if (rot==2) { dx=dw-1-(uint32_t)x; dy=dh-1-(uint32_t)y; }
            else { dx=dw-1-(uint32_t)y; dy=(uint32_t)x; }
            memcpy(d+(size_t)dy*dp+(size_t)dx*4,s+(size_t)y*sp+(size_t)x*4,4);
        }
    }
}
int MtkFbPresent(void *dst, uint32_t dw, uint32_t dh, uint32_t dp, size_t dn,
    const void *src, uint32_t sw, uint32_t sh, uint32_t sp, size_t sn,
    unsigned rotation, const MTK_FB_MOVE *moves, uint32_t nm,
    const MTK_FB_RECT *rects, uint32_t nr) {
    uint32_t i;
    uint64_t work = 0;
    if (!MtkFbValidatePresent(dw,dh,dp,dn,sw,sh,sp,sn,rotation,moves,nm,rects,nr) ||
        !Separate(dst,dn,src,sn)) return 0;
    // Bound overlapping updates to one full surface worth of work.
    for (i=0;i<nm;i++) work += (uint64_t)(moves[i].Dest.Right-moves[i].Dest.Left)*(moves[i].Dest.Bottom-moves[i].Dest.Top);
    for (i=0;i<nr;i++) work += (uint64_t)(rects[i].Right-rects[i].Left)*(rects[i].Bottom-rects[i].Top);
    if (work >= (uint64_t)sw*sh) {
        MTK_FB_RECT full = {0,0,(int32_t)sw,(int32_t)sh};
        Copy(dst,dw,dh,dp,src,sp,rotation,&full); return 1;
    }
    /* Dxgk's source image contains the final pixels at every move destination. */
    for (i=0;i<nm;i++) Copy(dst,dw,dh,dp,src,sp,rotation,&moves[i].Dest);
    for (i=0;i<nr;i++) Copy(dst,dw,dh,dp,src,sp,rotation,&rects[i]);
    return 1;
}
int MtkFbWrite(void *dst, uint32_t dw, uint32_t dh, uint32_t dp, size_t dn,
    const void *src, uint32_t sw, uint32_t sh, uint32_t sp, size_t sn,
    int32_t x, int32_t y) {
    uint32_t i;
    if (!Layout(dw,dh,dp,dn) || !Layout(sw,sh,sp,sn) || !Separate(dst,dn,src,sn) ||
        x<0 || y<0 || (uint64_t)(uint32_t)x+sw>dw || (uint64_t)(uint32_t)y+sh>dh) return 0;
    for (i=0;i<sh;i++) memcpy((uint8_t*)dst+(size_t)(y+i)*dp+(size_t)x*4,
        (const uint8_t*)src+(size_t)i*sp,(size_t)sw*4);
    return 1;
}
