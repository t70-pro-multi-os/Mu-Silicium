/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
/* No CRT: volatile loops keep these definitions from lowering into themselves. */
void *memset(void *P, int V, size_t N) {
    volatile unsigned char *D=P;
    while(N--) *D++=(unsigned char)V;
    return P;
}
void *memcpy(void *P, const void *Q, size_t N) {
    volatile unsigned char *D=P;
    const volatile unsigned char *S=Q;
    while(N--) *D++=*S++;
    return P;
}
int memcmp(const void *P, const void *Q, size_t N) {
    const volatile unsigned char *A=P,*B=Q;
    while(N--) {
        int Difference=(int)*A++-(int)*B++;
        if(Difference)return Difference;
    }
    return 0;
}
