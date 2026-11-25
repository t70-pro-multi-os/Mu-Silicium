// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdResponse.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
static unsigned Checks;
#define CHECK(x) do { Checks++; assert(x); } while(0)
int main(void)
{
    const uint32_t Addresses[] = {0,1,0x7fff,0x8000,0xffff};
    for (unsigned a=0;a<sizeof(Addresses)/sizeof(*Addresses);a++) {
        for (uint32_t low=0;low<0x10000;low++) {
            uint16_t out=0xa55a;
            int accepted=Addresses[a]!=0 && ((low>>13)&7)==0;
            T70_SD_RESPONSE_RESULT result=T70SdDecodeRelativeAddress((Addresses[a]<<16)|low,&out);
            CHECK(result==(accepted?T70_SD_RESPONSE_OK:T70_SD_RESPONSE_REJECTED));
            CHECK(out==(accepted?Addresses[a]:0xa55a));
        }
    }
    for (uint32_t address=1;address<0x10000;address++) {
        uint16_t out=0;
        CHECK(T70SdDecodeRelativeAddress(address<<16,&out)==T70_SD_RESPONSE_OK);
        CHECK(out==address);
    }
    CHECK(T70SdDecodeRelativeAddress(0x12340000,NULL)==T70_SD_RESPONSE_INVALID_ARGUMENT);
    CHECK(T70SdDecodeRelativeAddress(UINT32_MAX,NULL)==T70_SD_RESPONSE_INVALID_ARGUMENT);
    for (uint32_t low=0;low<0x1000;low++) {
        CHECK(T70SdCheckCmd8(low)==(low==0x1aa?T70_SD_RESPONSE_OK:T70_SD_RESPONSE_REJECTED));
        CHECK(T70SdCheckCmd8(low|(1u<<22))==T70_SD_RESPONSE_REJECTED);
    }
    for (unsigned bit=0;bit<32;bit++) {
        CHECK(T70SdCheckCmd8(0x1aa^(1u<<bit))==
            (bit<12||bit==22?T70_SD_RESPONSE_REJECTED:T70_SD_RESPONSE_OK));
        CHECK(T70SdCheckAppCommand((1u<<5)^(1u<<bit))==
            (bit==5||bit==22?T70_SD_RESPONSE_REJECTED:T70_SD_RESPONSE_OK));
    }
    for (unsigned app=0;app<2;app++)for(unsigned illegal=0;illegal<2;illegal++) {
        CHECK(T70SdCheckAppCommand((app<<5)|(illegal<<22))==
            (app&&!illegal?T70_SD_RESPONSE_OK:T70_SD_RESPONSE_REJECTED));
    }
    CHECK(T70SdCheckCmd8(UINT32_MAX)==T70_SD_RESPONSE_REJECTED);
    CHECK(T70SdCheckAppCommand(UINT32_MAX)==T70_SD_RESPONSE_REJECTED);
    printf("SD response core: %u checks passed; no transport/hardware exercised\n",Checks);
    return 0;
}
