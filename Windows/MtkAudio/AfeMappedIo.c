// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeMappedIo.h"
typedef char T70UlongMustBe32Bits[sizeof(ULONG)==4?1:-1];
static volatile ULONG *Address(const T70_AFE_MAPPED_IO *M,uint32_t Offset) {
 uintptr_t Base;
 if(!M || M->PhysicalBase!=UINT64_C(0x11210000) || M->Length!=0x2000 ||
    !M->VirtualBase)return NULL;
 Base=(uintptr_t)M->VirtualBase;
 if((Base&3) || (Offset&3) || Base>UINTPTR_MAX-M->Length ||
    Offset>M->Length-sizeof(ULONG))return NULL;
 return (volatile ULONG *)(Base+Offset);
}
int32_t T70AfeMappedRead32(void *Context,uint32_t Offset,uint32_t *Value) {
 const T70_AFE_MAPPED_IO *M=Context;
 volatile ULONG *Register=Address(M,Offset);
 uintptr_t V=(uintptr_t)Value,C=(uintptr_t)Context;
 if(!Register || !Value || (V&3) || V>UINTPTR_MAX-sizeof(*Value) ||
    C>UINTPTR_MAX-sizeof(*M))return STATUS_INVALID_PARAMETER;
 uintptr_t B=(uintptr_t)M->VirtualBase;
 if((V<B+M->Length && B<V+sizeof(*Value)) ||
    (V<C+sizeof(*M) && C<V+sizeof(*Value)))return STATUS_INVALID_PARAMETER;
 uint32_t Result=(uint32_t)READ_REGISTER_ULONG(Register);
 *Value=Result;return 0;
}
int32_t T70AfeMappedWrite32(void *Context,uint32_t Offset,uint32_t Value) {
 volatile ULONG *Register=Address(Context,Offset);
 if(!Register)return STATUS_INVALID_PARAMETER;
 WRITE_REGISTER_ULONG(Register,(ULONG)Value);return 0;
}
