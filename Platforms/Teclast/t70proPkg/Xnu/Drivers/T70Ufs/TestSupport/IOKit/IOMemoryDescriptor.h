/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_TEST_IO_MEMORY_DESCRIPTOR_H_
#define T70_UFS_TEST_IO_MEMORY_DESCRIPTOR_H_

#include <IOKit/IOService.h>
#include <algorithm>
#include <cstring>
#include <vector>

class IOMemoryDescriptor : public OSObject {
  uint64_t length_;
  unsigned direction_;
public:
  std::vector<uint8_t> bytes;
  IOMemoryDescriptor(uint64_t length, unsigned direction)
    : length_(length), direction_(direction), bytes(static_cast<size_t>(length), 0) {}
  uint64_t getLength() const { return length_; }
  unsigned getDirection() const { return direction_; }
  uint64_t writeBytes(uint64_t offset, const void *source, uint64_t count) {
    if (!source || offset > length_ || count > length_ - offset) return 0;
    std::memcpy(bytes.data() + offset, source, static_cast<size_t>(count));
    return count;
  }
};

struct IOStorageAttributes {};
struct IOStorageCompletion {
  void *target = nullptr;
  void *parameter = nullptr;
  void (*action)(void *, void *, IOReturn, IOByteCount) = nullptr;
};

#endif
