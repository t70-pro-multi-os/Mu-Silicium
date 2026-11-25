/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_TEST_IO_BLOCK_STORAGE_DEVICE_H_
#define T70_UFS_TEST_IO_BLOCK_STORAGE_DEVICE_H_

#include <IOKit/IOMemoryDescriptor.h>

class IOBlockStorageDevice : public IOService {
public:
  virtual IOReturn doEjectMedia() = 0;
  virtual IOReturn doFormatMedia(UInt64) = 0;
  virtual UInt32 doGetFormatCapacities(UInt64 *, UInt32) const = 0;
  virtual char *getVendorString() = 0;
  virtual char *getProductString() = 0;
  virtual char *getRevisionString() = 0;
  virtual char *getAdditionalDeviceInfoString() = 0;
  virtual IOReturn reportBlockSize(UInt64 *) = 0;
  virtual IOReturn reportEjectability(bool *) = 0;
  virtual IOReturn reportMaxValidBlock(UInt64 *) = 0;
  virtual IOReturn reportMediaState(bool *, bool *) = 0;
  virtual IOReturn reportRemovability(bool *) = 0;
  virtual IOReturn reportWriteProtection(bool *) = 0;
  virtual IOReturn doSynchronizeCache() = 0;
  virtual IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) = 0;
  virtual IOReturn getWriteCacheState(bool *) = 0;
  virtual IOReturn setWriteCacheState(bool) = 0;
  virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
                                    IOStorageAttributes *, IOStorageCompletion *) = 0;
};

#endif
