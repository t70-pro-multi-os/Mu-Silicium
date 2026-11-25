/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_TEST_IO_SERVICE_H_
#define T70_UFS_TEST_IO_SERVICE_H_

#include <cstddef>
#include <cstdint>

using UInt8 = uint8_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using SInt32 = int32_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOReturn = int32_t;

constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnUnsupported = -1;
constexpr IOReturn kIOReturnNotWritable = -2;
constexpr IOReturn kIOReturnBadArgument = -3;
constexpr IOReturn kIOReturnNotReady = -4;
constexpr IOReturn kIOReturnBusy = -5;
constexpr IOReturn kIOReturnIOError = -6;
constexpr IOReturn kIOReturnNoMedia = -7;
constexpr unsigned kIODirectionIn = 1;
constexpr unsigned kIODirectionOut = 2;

class OSObject {
public:
  virtual ~OSObject() = default;
  void retain() {}
  void release() {}
};

class IOService : public OSObject {
protected:
  virtual bool handleOpen(IOService *, IOOptionBits, void *) { return true; }
  virtual void handleClose(IOService *, IOOptionBits) {}
  virtual bool handleIsOpen(const IOService *) const { return true; }
public:
  virtual IOService *probe(IOService *provider, SInt32 *) { return provider; }
  virtual bool start(IOService *) { return true; }
  virtual void stop(IOService *) {}
  virtual void free() {}
  bool open(IOService *client, IOOptionBits options = 0, void *arg = nullptr) {
    return handleOpen(client, options, arg);
  }
  void close(IOService *client, IOOptionBits options = 0) {
    handleClose(client, options);
  }
  void registerService() {}
};

#define OSDeclareDefaultStructors(name) public: name() = default; virtual ~name() = default;
#define OSDefineMetaClassAndStructors(name, superclass)
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)

#endif
