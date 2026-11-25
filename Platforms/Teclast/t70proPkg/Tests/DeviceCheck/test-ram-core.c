/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T70RamTestCore.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  volatile uint64_t *Buffer;
  unsigned Flushes;
  unsigned ProgressCalls;
  int InjectMismatch;
  int CancelDuringFill;
  int CancelAtVerify;
} TEST_CONTEXT;

typedef struct {
  unsigned Calls;
  int Succeeds;
} RELEASE_CONTEXT;

static int
ReleasePages (void *Opaque, uintptr_t Address, size_t Pages)
{
  RELEASE_CONTEXT *Context = Opaque;
  assert(Address == (uintptr_t)0x1234000 && Pages == 4096);
  Context->Calls++;
  return Context->Succeeds;
}

static void
Flush (void *Opaque, volatile uint64_t *Buffer, size_t Bytes)
{
  TEST_CONTEXT *Context = Opaque;
  (void)Buffer;
  (void)Bytes;
  Context->Flushes++;
  if (Context->InjectMismatch && Context->Flushes == 1) {
    Context->Buffer[0] ^= UINT64_C(1);
    Context->InjectMismatch = 0;
  }
}

static int
Progress (void *Opaque, unsigned Pattern, int Verify, size_t Completed, size_t Total)
{
  TEST_CONTEXT *Context = Opaque;
  Context->ProgressCalls++;
  assert(Completed <= Total);
  if (Context->CancelDuringFill && !Verify && Pattern == 0) return 1;
  return Context->CancelAtVerify && Verify && Pattern == 0;
}

int
main (void)
{
  const size_t Words = 19;
  const size_t LargeWords = 16384U + 3U;
  const uint64_t Sentinel = UINT64_C(0xa5a5a5a5a5a5a5a5);
  volatile uint64_t *Buffer = aligned_alloc(8, Words * sizeof(*Buffer));
  volatile uint64_t *LargeBuffer = aligned_alloc(8, LargeWords * sizeof(*LargeBuffer));
  T70_RAM_TEST_RESULT Result;
  TEST_CONTEXT Context;
  T70_RAM_TEST_STATUS Status;
  T70_RAM_TEST_LEASE Lease = { (uintptr_t)0x1234000, 4096 };
  RELEASE_CONTEXT Release = { 0, 0 };
  assert(Buffer != NULL);
  assert(LargeBuffer != NULL);

  memset(&Context, 0, sizeof(Context));
  Context.Buffer = Buffer;
  Status = T70RamTestRun(Buffer, Words, Progress, Flush, &Context, &Result);
  assert(Status == T70RamTestPass);
  assert(Result.TotalOperations == Words * 12);
  assert(Result.CompletedOperations == Result.TotalOperations);
  assert(Result.ErrorCount == 0 && Context.Flushes == 6);
  assert(Context.ProgressCalls > 0);

  memset(&Context, 0, sizeof(Context));
  Context.Buffer = Buffer;
  Context.InjectMismatch = 1;
  Status = T70RamTestRun(Buffer, Words, Progress, Flush, &Context, &Result);
  assert(Status == T70RamTestMismatch);
  assert(Result.ErrorCount == 1 && Result.ErrorAddress == (uintptr_t)&Buffer[0]);
  assert(Result.Pattern == 0 && Result.Verify == 1);
  assert(Result.Expected == 0 && Result.Actual == 1);

  memset((void *)LargeBuffer, 0xa5, LargeWords * sizeof(*LargeBuffer));
  memset(&Context, 0, sizeof(Context));
  Context.Buffer = LargeBuffer;
  Context.CancelDuringFill = 1;
  Status = T70RamTestRun(LargeBuffer, LargeWords, Progress, Flush, &Context, &Result);
  assert(Status == T70RamTestCancelled);
  assert(Result.CompletedOperations == 16384U && Context.Flushes == 1);
  assert(LargeBuffer[0] == 0 && LargeBuffer[16383] == 0);
  assert(LargeBuffer[16384] == Sentinel && LargeBuffer[LargeWords - 1] == Sentinel);

  memset(&Context, 0, sizeof(Context));
  Context.Buffer = LargeBuffer;
  Status = T70RamTestRun(LargeBuffer, LargeWords, Progress, Flush, &Context, &Result);
  assert(Status == T70RamTestPass);
  assert(Result.CompletedOperations == LargeWords * 12 && Context.Flushes == 6);
  assert(Context.ProgressCalls >= 24);

  memset(&Context, 0, sizeof(Context));
  Context.Buffer = Buffer;
  Context.CancelAtVerify = 1;
  Status = T70RamTestRun(Buffer, Words, Progress, Flush, &Context, &Result);
  assert(Status == T70RamTestCancelled);
  assert(Result.CompletedOperations > 0 && Result.CompletedOperations < Result.TotalOperations);
  assert(Context.Flushes == 1);

  memset(&Context, 0, sizeof(Context));
  assert(T70RamTestRun(Buffer, 0, Progress, Flush, &Context, &Result) == T70RamTestInvalid);
  assert(T70RamTestRun(Buffer, SIZE_MAX, Progress, Flush, &Context, &Result) == T70RamTestInvalid);
  assert(T70RamTestRun((volatile uint64_t *)((uintptr_t)Buffer + 1), Words, Progress, Flush,
                       &Context, &Result) == T70RamTestInvalid);
  assert(T70RamTestRun((volatile uint64_t *)(UINTPTR_MAX - 7), 1, Progress, Flush,
                       &Context, &Result) == T70RamTestInvalid);
  assert(T70RamTestRun(Buffer, Words, Progress, Flush, &Context, NULL) == T70RamTestInvalid);

  assert(!T70RamTestPrepareToAllocate(&Lease, ReleasePages, &Release));
  assert(Lease.Address == (uintptr_t)0x1234000 && Lease.Pages == 4096 && Release.Calls == 1);
  Release.Succeeds = 1;
  assert(T70RamTestPrepareToAllocate(&Lease, ReleasePages, &Release));
  assert(Lease.Address == 0 && Lease.Pages == 0 && Release.Calls == 2);
  assert(T70RamTestPrepareToAllocate(&Lease, NULL, NULL));

  free((void *)LargeBuffer);
  free((void *)Buffer);
  puts("RAM core: six-pattern pass across chunk/tail, mismatch, cancellation, invalid ranges, release retry");
  return 0;
}
