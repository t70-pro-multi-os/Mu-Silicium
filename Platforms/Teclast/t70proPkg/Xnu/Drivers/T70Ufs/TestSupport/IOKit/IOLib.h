/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_TEST_IO_LIB_H_
#define T70_UFS_TEST_IO_LIB_H_

#include <IOKit/IOService.h>
#include <cstdlib>
#include <cstring>
#include <mutex>

using IOLock = std::mutex;
static inline IOLock *IOLockAlloc() { return new IOLock(); }
static inline void IOLockFree(IOLock *lock) { delete lock; }
static inline void IOLockLock(IOLock *lock) { lock->lock(); }
static inline void IOLockUnlock(IOLock *lock) { lock->unlock(); }
static inline void *IOMalloc(size_t bytes) { return std::malloc(bytes); }
static inline void IOFree(void *buffer, size_t) { std::free(buffer); }

#endif
