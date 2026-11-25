/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T70RamTestCore.h"
#include <limits.h>

#define T70_RAM_TEST_PATTERN_COUNT  6U
#define T70_RAM_TEST_CHUNK_WORDS    16384U
#define T70_RAM_TEST_ADDRESS_SALT   UINT64_C(0x9e3779b97f4a7c15)

int
T70RamTestPrepareToAllocate (
  T70_RAM_TEST_LEASE *Lease,
  T70_RAM_TEST_RELEASE_PAGES Release,
  void *Context
  )
{
  if (Lease == NULL) return 0;
  if (Lease->Pages == 0) return Lease->Address == 0;
  if (Release == NULL || !Release (Context, Lease->Address, Lease->Pages)) return 0;
  Lease->Address = 0;
  Lease->Pages = 0;
  return 1;
}

static uint64_t
ExpectedValue (volatile uint64_t *Buffer, size_t Index, unsigned Pattern)
{
  uint64_t AddressValue = (uint64_t)(uintptr_t)&Buffer[Index] ^ T70_RAM_TEST_ADDRESS_SALT;
  uint64_t WalkingBit = UINT64_C(1) << (Index & 63U);

  switch (Pattern) {
    case 0: return 0;
    case 1: return UINT64_MAX;
    case 2: return AddressValue;
    case 3: return ~AddressValue;
    case 4: return WalkingBit;
    default: return ~WalkingBit;
  }
}

static int
ReportProgress (
  T70_RAM_TEST_PROGRESS Progress,
  void *Context,
  unsigned Pattern,
  int Verify,
  size_t Completed,
  size_t Total
  )
{
  return Progress != NULL && Progress (Context, Pattern, Verify, Completed, Total);
}

T70_RAM_TEST_STATUS
T70RamTestRun (
  volatile uint64_t *Buffer,
  size_t Words,
  T70_RAM_TEST_PROGRESS Progress,
  T70_RAM_TEST_CACHE_FLUSH Flush,
  void *Context,
  T70_RAM_TEST_RESULT *Result
  )
{
  size_t Bytes;
  unsigned Pattern;

  if (Result == NULL) return T70RamTestInvalid;
  *Result = (T70_RAM_TEST_RESULT){ 0 };
  if (Buffer == NULL || Progress == NULL || Flush == NULL || Words == 0 ||
      ((uintptr_t)Buffer & (sizeof (*Buffer) - 1U)) != 0 ||
      Words > SIZE_MAX / sizeof (*Buffer) ||
      Words > SIZE_MAX / (2U * T70_RAM_TEST_PATTERN_COUNT)) {
    return T70RamTestInvalid;
  }

  Bytes = Words * sizeof (*Buffer);
  if ((uintptr_t)Buffer > UINTPTR_MAX - Bytes) return T70RamTestInvalid;
  Result->TotalOperations = Words * (2U * T70_RAM_TEST_PATTERN_COUNT);

  for (Pattern = 0; Pattern < T70_RAM_TEST_PATTERN_COUNT; Pattern++) {
    size_t Start;
    for (Start = 0; Start < Words; ) {
      size_t End = Words - Start > T70_RAM_TEST_CHUNK_WORDS ?
                   Start + T70_RAM_TEST_CHUNK_WORDS : Words;
      size_t Index;
      for (Index = Start; Index < End; Index++)
        Buffer[Index] = ExpectedValue (Buffer, Index, Pattern);
      Result->CompletedOperations += End - Start;
      Start = End;
      if (ReportProgress (Progress, Context, Pattern, 0,
                          Result->CompletedOperations, Result->TotalOperations)) {
        Flush (Context, Buffer, Bytes);
        return T70RamTestCancelled;
      }
    }

    Flush (Context, Buffer, Bytes);

    for (Start = 0; Start < Words; ) {
      size_t End = Words - Start > T70_RAM_TEST_CHUNK_WORDS ?
                   Start + T70_RAM_TEST_CHUNK_WORDS : Words;
      size_t Index;
      for (Index = Start; Index < End; Index++) {
        uint64_t Expected = ExpectedValue (Buffer, Index, Pattern);
        uint64_t Actual = Buffer[Index];
        if (Actual != Expected) {
          Result->ErrorAddress = (uintptr_t)&Buffer[Index];
          Result->Expected = Expected;
          Result->Actual = Actual;
          Result->Pattern = Pattern;
          Result->Verify = 1;
          Result->ErrorCount = 1;
          return T70RamTestMismatch;
        }
      }
      Result->CompletedOperations += End - Start;
      Start = End;
      if (ReportProgress (Progress, Context, Pattern, 1,
                          Result->CompletedOperations, Result->TotalOperations))
        return T70RamTestCancelled;
    }
  }

  return T70RamTestPass;
}
