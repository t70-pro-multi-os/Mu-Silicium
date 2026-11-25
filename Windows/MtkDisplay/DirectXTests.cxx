// SPDX-License-Identifier: BSD-2-Clause-Patent
#define T70_DIRECTX_READBACK_TEST
#include "DirectX.cxx"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define CHECK(X) do {++Checks;assert(X);} while(0)
int main() {
    uint8_t Guard[16+64*272+16];memset(Guard,0xa5,sizeof(Guard));
    uint8_t *Data=Guard+16;
    for(unsigned Y=0;Y<64;Y++) for(unsigned X=0;X<64;X++) {
        uint8_t *V=Data+Y*272+X*4;V[0]=64;V[1]=128;V[2]=191;V[3]=255;
    }
    CHECK(DxPixelsValid(Data,272,64*272,64,64));
    CHECK(!DxPixelsValid(nullptr,272,64*272,64,64));
    CHECK(!DxPixelsValid(Data,272,64*272-1,64,64));
    CHECK(!DxPixelsValid(Data,252,64*272,64,64));
    CHECK(!DxPixelsValid(Data,273,64*273,64,64));
    CHECK(!DxPixelsValid(Data,272,64*272,0,64));
    CHECK(!DxPixelsValid(Data,272,64*272,64,0));
    CHECK(!DxPixelsValid(Data,272,64*272,8193,64));
    CHECK(!DxPixelsValid(Data,272,64*272,64,8193));
    CHECK(!DxPixelsValid(Data,272,128ULL*1024*1024+1,64,64));
    CHECK(!DxPixelsValid(Data,0xfffffffcU,UINT64_MAX,64,64));
    for(unsigned Channel=0;Channel<4;Channel++) for(unsigned Value=0;Value<256;Value++) {
        uint8_t *V=Data+63*272+63*4;uint8_t Old=V[Channel];V[Channel]=(uint8_t)Value;
        bool Valid=Channel==0?(Value>=63&&Value<=65):Channel==1?(Value>=127&&Value<=129):
            Channel==2?(Value>=190&&Value<=192):Value==255;
        CHECK(!!DxPixelsValid(Data,272,64*272,64,64)==Valid);V[Channel]=Old;
    }
    // Every pixel matters: a clear-only image or a missing triangle edge fails.
    for(unsigned Y=0;Y<64;Y++) for(unsigned X=0;X<64;X++) {
        uint8_t *V=Data+Y*272+X*4;V[0]=0;
        CHECK(!DxPixelsValid(Data,272,64*272,64,64));V[0]=64;
    }
    CHECK(DxPixelsValid(Data,272,64*272,64,64));
    for(unsigned I=0;I<16;I++) {CHECK(Guard[I]==0xa5);CHECK(Guard[sizeof(Guard)-1-I]==0xa5);}
    for(unsigned Y=0;Y<64;Y++) for(unsigned I=256;I<272;I++)CHECK(Data[Y*272+I]==0xa5);
    printf("DirectX production readback validator: %u checks passed (synthetic pixels; no D3D runtime)\n",Checks);
}
