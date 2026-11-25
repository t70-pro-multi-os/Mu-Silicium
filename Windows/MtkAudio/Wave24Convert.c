// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Wave24Convert.h"
int T70Wave24ToS24Le(const void *Source,size_t Bytes,uint32_t Channels,
                    void *Destination,size_t Capacity)
{
  const unsigned char *S = Source;
  unsigned char *D = Destination;
  uintptr_t A = (uintptr_t)Source, B = (uintptr_t)Destination;
  size_t I;
  if ((Channels != 1 && Channels != 2) || Bytes % (4 * Channels) ||
      Bytes > Capacity) return 0;
  if (!Bytes) return 1;
  if (!Source || !Destination || Bytes > UINTPTR_MAX - A ||
      Capacity > UINTPTR_MAX - B) return 0;
  if (A < B + Bytes && B < A + Bytes) return 0;
  /* Validate the entire input first: a malformed final sample must not leave
   * partially converted output. All accesses are byte-wise and endian-neutral. */
  for (I = 0; I < Bytes; I += 4)
    if (S[I] != 0) return 0;
  for (I = 0; I < Bytes; I += 4) {
    D[I] = S[I + 1];
    D[I + 1] = S[I + 2];
    D[I + 2] = S[I + 3];
    D[I + 3] = (S[I + 3] & 0x80) ? 0xff : 0;
  }
  return 1;
}
