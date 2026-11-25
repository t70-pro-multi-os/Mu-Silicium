// SPDX-License-Identifier: BSD-2-Clause-Patent
// Freestanding versions of the BaseLib routines used by the read-only display handoff.
#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

VOID *EFIAPI
ZeroMem (VOID *Buffer, UINTN Length)
{
  UINT8 *Bytes = Buffer;
  for (UINTN Index = 0; Index < Length; ++Index) {
    Bytes[Index] = 0;
  }
  return Buffer;
}

VOID *EFIAPI
CopyMem (VOID *Destination, CONST VOID *Source, UINTN Length)
{
  UINT8 *Out = Destination;
  CONST UINT8 *In = Source;

  if ((UINTN)Out > (UINTN)In && (UINTN)Out - (UINTN)In < Length) {
    while (Length != 0) {
      --Length;
      Out[Length] = In[Length];
    }
  } else {
    for (UINTN Index = 0; Index < Length; ++Index) {
      Out[Index] = In[Index];
    }
  }
  return Destination;
}

INTN EFIAPI
CompareMem (CONST VOID *Buffer1, CONST VOID *Buffer2, UINTN Length)
{
  CONST UINT8 *First = Buffer1;
  CONST UINT8 *Second = Buffer2;
  for (UINTN Index = 0; Index < Length; ++Index) {
    if (First[Index] != Second[Index]) {
      return (INTN)First[Index] - Second[Index];
    }
  }
  return 0;
}

VOID *EFIAPI
ScanMem8 (CONST VOID *Buffer, UINTN Length, UINT8 Value)
{
  CONST UINT8 *Bytes = Buffer;
  for (UINTN Index = 0; Index < Length; ++Index) {
    if (Bytes[Index] == Value) {
      return (VOID *)(Bytes + Index);
    }
  }
  return NULL;
}

UINTN EFIAPI
AsciiStrLen (CONST CHAR8 *String)
{
  UINTN Length = 0;
  while (String[Length] != '\0') {
    ++Length;
  }
  return Length;
}
