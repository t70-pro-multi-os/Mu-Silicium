/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_RAM_TEST_CORE_H_
#define T70_RAM_TEST_CORE_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
  T70RamTestPass,
  T70RamTestMismatch,
  T70RamTestCancelled,
  T70RamTestInvalid
} T70_RAM_TEST_STATUS;

typedef struct {
  uintptr_t Address;
  size_t Pages;
} T70_RAM_TEST_LEASE;

typedef int (*T70_RAM_TEST_PROGRESS)(
  void *Context,
  unsigned Pattern,
  int Verify,
  size_t CompletedOperations,
  size_t TotalOperations
  );

typedef void (*T70_RAM_TEST_CACHE_FLUSH)(
  void *Context,
  volatile uint64_t *Buffer,
  size_t Bytes
  );

typedef int (*T70_RAM_TEST_RELEASE_PAGES)(
  void *Context,
  uintptr_t Address,
  size_t Pages
  );

typedef struct {
  uintptr_t ErrorAddress;
  uint64_t Expected;
  uint64_t Actual;
  size_t CompletedOperations;
  size_t TotalOperations;
  unsigned Pattern;
  unsigned ErrorCount;
  int Verify;
} T70_RAM_TEST_RESULT;

T70_RAM_TEST_STATUS
T70RamTestRun (
  volatile uint64_t *Buffer,
  size_t Words,
  T70_RAM_TEST_PROGRESS Progress,
  T70_RAM_TEST_CACHE_FLUSH Flush,
  void *Context,
  T70_RAM_TEST_RESULT *Result
  );

int
T70RamTestPrepareToAllocate (
  T70_RAM_TEST_LEASE *Lease,
  T70_RAM_TEST_RELEASE_PAGES Release,
  void *Context
  );

#endif
