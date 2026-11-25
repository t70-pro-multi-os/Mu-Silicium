// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Framebuffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static unsigned char d[65536],s[65536],before[65536];
int main(void) {
    MTK_FB_HANDOFF h={1,1,0x80000000,1400*4*2240,1400,2240,5600,1,0}, bad;
    CHECK(MtkFbValidateHandoff(&h));
    for (unsigned i=0;i<12;i++) { bad=h;
        switch(i) { case 0:bad.Ready=0;break;case 1:bad.Revision=2;break;
        case 2:bad.Base=0x80000001;break;case 3:bad.RegionSize--;break;
        case 4:bad.Width=0;break;case 5:bad.Height=4097;break;
        case 6:bad.Pitch=5599;break;case 7:bad.Format=2;break;
        case 8:bad.Reserved=1;break;case 9:bad.Base=0xffffff000ULL;break;
        case 10:bad.RegionSize=MTK_FB_MAX_BYTES+1;break;case 11:bad.Base=0x1000;break; }
        CHECK(!MtkFbValidateHandoff(&bad));
    }
    for (unsigned w=1;w<=17;w++) for (unsigned hgt=1;hgt<=13;hgt++) for(unsigned r=0;r<4;r++) {
        unsigned sw=(r&1)?hgt:w,sh=(r&1)?w:hgt,dp=w*4+8,sp=sw*4+12;
        memset(d,0xa5,sizeof(d));memset(s,0x57,sizeof(s));
        for(unsigned y=0;y<sh;y++)for(unsigned x=0;x<sw;x++) {
            unsigned p=0xff000000|y*256|x; memcpy(s+y*sp+x*4,&p,4);
        }
        MTK_FB_RECT rect={0,0,(int)sw,(int)sh};
        CHECK(MtkFbPresent(d,w,hgt,dp,dp*hgt,s,sw,sh,sp,sp*sh,r,NULL,0,&rect,1));
        for(unsigned y=0;y<sh;y++)for(unsigned x=0;x<sw;x++) {
            unsigned dx=r==1?y:r==2?w-1-x:r==3?w-1-y:x;
            unsigned dy=r==1?hgt-1-x:r==2?hgt-1-y:r==3?x:y;
            CHECK(!memcmp(d+dy*dp+dx*4,s+y*sp+x*4,4));
        }
        for(unsigned y=0;y<hgt;y++) for(unsigned x=w*4;x<dp;x++) CHECK(d[y*dp+x]==0xa5);
        CHECK(d[dp*hgt]==0xa5);
        memcpy(before,d,sizeof(d));
        MTK_FB_RECT rects[2]={rect,{-1,0,0,0}};
        CHECK(!MtkFbPresent(d,w,hgt,dp,dp*hgt,s,sw,sh,sp,sp*sh,r,NULL,0,rects,2));
        CHECK(!memcmp(before,d,sizeof(d)));
        CHECK(!MtkFbPresent(d,w,hgt,dp,dp*hgt,d,sw,sh,sp,sp*sh,r,NULL,0,&rect,1));
        CHECK(!MtkFbPresent(d,w,hgt,dp,dp*hgt,s,sw,sh,sp,sp*sh-1,r,NULL,0,&rect,1));
        CHECK(!MtkFbPresent(d,w,hgt,dp,dp*hgt,s,sw,sh,sp,sp*sh,4,NULL,0,&rect,1));
        CHECK(!MtkFbValidatePresent(w,hgt,dp,dp*hgt,sw,sh,sp,sp*sh,r,NULL,1,NULL,0));
        CHECK(!MtkFbValidatePresent(w,hgt,dp,dp*hgt,sw,sh,sp,sp*sh,r,NULL,0,rects,4097));
        if(sw>1 && sh>1) {
            MTK_FB_MOVE move={0,0,{1,1,(int)sw,(int)sh}};
            memset(d,0xa5,sizeof(d));
            CHECK(MtkFbPresent(d,w,hgt,dp,dp*hgt,s,sw,sh,sp,sp*sh,r,&move,1,NULL,0));
            memcpy(before,d,sizeof(d));move.X=(int)sw;
            CHECK(!MtkFbPresent(d,w,hgt,dp,dp*hgt,s,sw,sh,sp,sp*sh,r,&move,1,NULL,0));
            CHECK(!memcmp(before,d,sizeof(d)));
        }
    }
    // Golden matrices anchor direction to the D3DKMDT counter-clockwise API.
    { const uint32_t image[6]={1,2,3,4,5,6}; uint32_t output[6]={0};
      const uint32_t ccw[6]={2,4,6,1,3,5},cw[6]={5,3,1,6,4,2};
      MTK_FB_RECT rect={0,0,2,3};
      CHECK(MtkFbPresent(output,3,2,12,24,image,2,3,8,24,1,NULL,0,&rect,1));
      CHECK(!memcmp(output,ccw,24));
      CHECK(MtkFbPresent(output,3,2,12,24,image,2,3,8,24,3,NULL,0,&rect,1));
      CHECK(!memcmp(output,cw,24));
    }
    { MTK_FB_RECT many[MTK_FB_MAX_RECTS];
      for(unsigned i=0;i<MTK_FB_MAX_RECTS;i++) many[i]=(MTK_FB_RECT){0,0,16,8};
      memset(d,0xa5,sizeof(d));memset(s,0x12,sizeof(s));
      CHECK(MtkFbPresent(d,16,8,80,640,s,16,8,80,640,0,NULL,0,many,MTK_FB_MAX_RECTS));
      CHECK(d[0]==0x12 && d[639]==0xa5 && d[640]==0xa5);
      memcpy(before,d,sizeof(d));
      CHECK(!MtkFbPresent(d,16,8,80,640,(void*)(UINTPTR_MAX-3),16,8,80,640,0,NULL,0,many,1));
      CHECK(!memcmp(before,d,sizeof(d)));
    }
    memset(d,0xa5,sizeof(d));memset(s,0x34,sizeof(s));
    CHECK(MtkFbWrite(d,10,8,48,384,s,3,2,20,40,7,6));
    CHECK(!memcmp(d+6*48+7*4,s,12) && !memcmp(d+7*48+7*4,s+20,12));
    memcpy(before,d,sizeof(d));
    CHECK(!MtkFbWrite(d,10,8,48,384,s,3,2,20,40,8,6));
    CHECK(!MtkFbWrite(d,10,8,48,384,s,3,2,20,40,-1,6));
    CHECK(!memcmp(before,d,sizeof(d)));
    printf("MtkDisplay framebuffer: %u checks passed\n",checks);return 0;
}
