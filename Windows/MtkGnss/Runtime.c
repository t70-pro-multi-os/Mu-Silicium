// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stddef.h>
/* The existing SDK has no vcruntime import library. Use OS routines for the
 * compiler-generated copies/fills; keep the DLL independent of a private CRT. */
__declspec(dllimport) void __stdcall RtlMoveMemory(void *,const void *,size_t);
__declspec(dllimport) void __stdcall RtlFillMemory(void *,size_t,unsigned char);
void *memcpy(void *To,const void *From,size_t Bytes) {
  RtlMoveMemory(To,From,Bytes);
  return To;
}
void *memset(void *To,int Value,size_t Bytes) {
  RtlFillMemory(To,Bytes,(unsigned char)Value);
  return To;
}
