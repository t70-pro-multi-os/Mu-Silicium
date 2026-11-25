/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include "T70UfsControllerProvider.h"
#include "T70UfsBlockRequest.h"

static_assert((MTK_UFS_DATA_BYTES % MTK_UFS_BLOCK_BYTES) == 0U,
              "UFS private DMA window must contain whole media blocks");

extern "C" {
volatile uint32_t native_t70_ufs_block_ready = 0;
volatile uint32_t native_t70_ufs_block_read_requests = 0;
volatile uint32_t native_t70_ufs_block_read_failures = 0;
volatile uint32_t native_t70_ufs_block_write_requests = 0;
volatile uint32_t native_t70_ufs_block_write_refusals = 0;
volatile uint32_t native_t70_ufs_block_dma_quarantined = 0;
volatile uint64_t native_t70_ufs_block_last_logical_block = 0;
volatile uint32_t native_t70_ufs_block_size = 0;
}

class NativeT70UfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeT70UfsBlock)

  T70UfsControllerProvider *controller_ = nullptr; /* Retained, non-owning transport access. */
  IOLock *gate_ = nullptr;
  uint8_t *staging_ = nullptr;                    /* CPU buffer; never programmed into UFS DMA. */
  uint64_t lastLogicalBlock_ = 0;
  uint32_t blockBytes_ = 0;
  bool providerOpen_ = false;
  bool providerQuiesced_ = false;
  bool online_ = false;

#if defined(T70_UFS_BLOCK_METHOD_TEST)
  friend struct NativeT70UfsBlockMethodHarness;
#endif

  bool quiesceProvider();
  void releaseProvider(bool quiesce);
  void releaseLocalStorage();
  static IOReturn mapReadStatus(T70_UFS_READ_STATUS status);

public:
  IOService *probe(IOService *, SInt32 *) override;
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;

  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("MediaTek"); }
  char *getProductString() override { return const_cast<char *>("T70 UFS LU2 read-only"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override {
    return const_cast<char *>("READ(16), private provider DMA, read-only");
  }
  IOReturn reportBlockSize(UInt64 *) override;
  IOReturn reportEjectability(bool *) override;
  IOReturn reportMaxValidBlock(UInt64 *) override;
  IOReturn reportMediaState(bool *, bool *) override;
  IOReturn reportRemovability(bool *) override;
  IOReturn reportWriteProtection(bool *) override;
  IOReturn doSynchronizeCache() override;
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override;
  IOReturn getWriteCacheState(bool *) override;
  IOReturn setWriteCacheState(bool) override;
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
                            IOStorageAttributes *, IOStorageCompletion *) override;
};

OSDefineMetaClassAndStructors(NativeT70UfsBlock, IOBlockStorageDevice)

IOService *NativeT70UfsBlock::probe(IOService *provider, SInt32 *score)
{
  T70UfsControllerProvider *controller =
    OSDynamicCast(T70UfsControllerProvider, provider);
  if (!controller || !controller->isReadyForReadOnlyLu2()) {
    return nullptr;
  }
  return IOBlockStorageDevice::probe(provider, score);
}

bool NativeT70UfsBlock::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider)) {
    return false;
  }

  controller_ = OSDynamicCast(T70UfsControllerProvider, provider);
  if (!controller_) {
    IOBlockStorageDevice::stop(provider);
    return false;
  }

  controller_->retain();
  if (!controller_->open(this)) {
    controller_->release();
    controller_ = nullptr;
    IOBlockStorageDevice::stop(provider);
    return false;
  }
  providerOpen_ = true;

  if (!controller_->isReadyForReadOnlyLu2()) {
    releaseProvider(true);
    IOBlockStorageDevice::stop(provider);
    return false;
  }

  lastLogicalBlock_ = controller_->lastLogicalBlock();
  blockBytes_ = controller_->blockSize();
  uint64_t oneBlockBytes = 0;
  if (lastLogicalBlock_ == UINT64_MAX ||
      T70UfsBlockPlanRequest(true, lastLogicalBlock_, blockBytes_, 0, 1,
                            MTK_UFS_DATA_BYTES, &oneBlockBytes) !=
        T70UfsBlockRequestSuccess ||
      oneBlockBytes != blockBytes_) {
    releaseProvider(true);
    IOBlockStorageDevice::stop(provider);
    return false;
  }

  gate_ = IOLockAlloc();
  staging_ = static_cast<uint8_t *>(IOMalloc(MTK_UFS_DATA_BYTES));
  if (!gate_ || !staging_) {
    releaseLocalStorage();
    releaseProvider(true);
    IOBlockStorageDevice::stop(provider);
    return false;
  }

  online_ = true;
  native_t70_ufs_block_last_logical_block = lastLogicalBlock_;
  native_t70_ufs_block_size = blockBytes_;
  native_t70_ufs_block_ready = 1;
  registerService();
  return true;
}

bool NativeT70UfsBlock::quiesceProvider()
{
  if (!controller_ || !providerOpen_ || providerQuiesced_) {
    return true;
  }
  providerQuiesced_ = controller_->quiesceDmaForStop();
  if (!providerQuiesced_) {
    native_t70_ufs_block_dma_quarantined = 1;
  }
  return providerQuiesced_;
}

void NativeT70UfsBlock::releaseProvider(bool quiesce)
{
  if (!controller_) {
    return;
  }
  if (providerOpen_) {
    if (quiesce) {
      (void)quiesceProvider();
    }
    controller_->close(this);
    providerOpen_ = false;
  }
  controller_->release();
  controller_ = nullptr;
  providerQuiesced_ = false;
}

void NativeT70UfsBlock::releaseLocalStorage()
{
  if (staging_) {
    IOFree(staging_, MTK_UFS_DATA_BYTES);
    staging_ = nullptr;
  }
  if (gate_) {
    IOLockFree(gate_);
    gate_ = nullptr;
  }
}

IOReturn NativeT70UfsBlock::mapReadStatus(T70_UFS_READ_STATUS status)
{
  switch (status) {
    case T70UfsReadSuccess:
      return kIOReturnSuccess;
    case T70UfsReadBusy:
      return kIOReturnBusy;
    case T70UfsReadNotReady:
    case T70UfsReadDmaOwnershipUncertain:
      return kIOReturnNotReady;
    case T70UfsReadDeviceError:
      return kIOReturnIOError;
    case T70UfsReadInvalidArgument:
    case T70UfsReadGeometryMismatch:
    case T70UfsReadOutOfRange:
    case T70UfsReadTooLarge:
    case T70UfsReadBufferTooSmall:
    default:
      return kIOReturnBadArgument;
  }
}

IOReturn NativeT70UfsBlock::reportBlockSize(UInt64 *value)
{
  if (!value) return kIOReturnBadArgument;
  if (!controller_ || blockBytes_ != MTK_UFS_BLOCK_BYTES) return kIOReturnNotReady;
  *value = blockBytes_;
  return kIOReturnSuccess;
}

IOReturn NativeT70UfsBlock::reportEjectability(bool *value)
{
  if (!value) return kIOReturnBadArgument;
  *value = false;
  return kIOReturnSuccess;
}

IOReturn NativeT70UfsBlock::reportMaxValidBlock(UInt64 *value)
{
  if (!value) return kIOReturnBadArgument;
  if (!controller_ || lastLogicalBlock_ == UINT64_MAX) return kIOReturnNotReady;
  *value = lastLogicalBlock_;
  return kIOReturnSuccess;
}

IOReturn NativeT70UfsBlock::reportMediaState(bool *present, bool *changed)
{
  if (!present) return kIOReturnBadArgument;
  *present = online_;
  if (changed) *changed = false;
  return kIOReturnSuccess;
}

IOReturn NativeT70UfsBlock::reportRemovability(bool *value)
{
  if (!value) return kIOReturnBadArgument;
  *value = false;
  return kIOReturnSuccess;
}

IOReturn NativeT70UfsBlock::reportWriteProtection(bool *value)
{
  if (!value) return kIOReturnBadArgument;
  *value = true;
  return kIOReturnSuccess;
}

IOReturn NativeT70UfsBlock::doSynchronizeCache()
{
  return online_ ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn NativeT70UfsBlock::doSynchronize(UInt64, UInt64, IOOptionBits)
{
  return doSynchronizeCache();
}

IOReturn NativeT70UfsBlock::getWriteCacheState(bool *value)
{
  if (!value) return kIOReturnBadArgument;
  *value = false;
  return kIOReturnSuccess;
}

IOReturn NativeT70UfsBlock::setWriteCacheState(bool)
{
  return kIOReturnUnsupported;
}

IOReturn NativeT70UfsBlock::doAsyncReadWrite(
  IOMemoryDescriptor *buffer,
  UInt64 block,
  UInt64 count,
  IOStorageAttributes *,
  IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action) {
    return kIOReturnBadArgument;
  }

  uint64_t totalBytes = 0;
  const T70_UFS_BLOCK_REQUEST_STATUS plan = T70UfsBlockPlanRequest(
    buffer->getDirection() == kIODirectionIn,
    lastLogicalBlock_, blockBytes_, block, count,
    static_cast<size_t>(buffer->getLength()), &totalBytes);
  if (plan == T70UfsBlockRequestNotWritable) {
    __atomic_fetch_add(&native_t70_ufs_block_write_requests, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_t70_ufs_block_write_refusals, 1, __ATOMIC_RELAXED);
    return kIOReturnNotWritable;
  }
  if (plan != T70UfsBlockRequestSuccess) {
    return kIOReturnBadArgument;
  }

  if (!gate_ || !staging_) {
    return kIOReturnNotReady;
  }

  uint64_t transferred = 0;
  IOReturn result = kIOReturnNotReady;
  IOLockLock(gate_);
  if (online_ && controller_ && providerOpen_ &&
      controller_->isReadyForReadOnlyLu2()) {
    __atomic_fetch_add(&native_t70_ufs_block_read_requests, 1, __ATOMIC_RELAXED);
    result = kIOReturnSuccess;
    while (transferred < totalBytes) {
      const uint64_t remaining = totalBytes - transferred;
      const uint32_t chunkBytes = static_cast<uint32_t>(
        remaining > MTK_UFS_DATA_BYTES ? MTK_UFS_DATA_BYTES : remaining);
      const uint64_t chunkBlocks = chunkBytes / blockBytes_;
      T70_UFS_READ_STATUS readStatus = controller_->readBlocks(
        block + transferred / blockBytes_, chunkBlocks, staging_, chunkBytes);
      if (readStatus == T70UfsReadDmaOwnershipUncertain) {
        online_ = false;
        native_t70_ufs_block_ready = 0;
        (void)quiesceProvider();
        result = kIOReturnNotReady;
        break;
      }
      if (readStatus != T70UfsReadSuccess) {
        result = mapReadStatus(readStatus);
        break;
      }
      if (buffer->writeBytes(transferred, staging_, chunkBytes) != chunkBytes) {
        result = kIOReturnIOError;
        break;
      }
      transferred += chunkBytes;
    }
  }

  if (result != kIOReturnSuccess) {
    __atomic_fetch_add(&native_t70_ufs_block_read_failures, 1, __ATOMIC_RELAXED);
  }
  IOLockUnlock(gate_);
  completion->action(completion->target, completion->parameter, result, transferred);
  return kIOReturnSuccess;
}

void NativeT70UfsBlock::stop(IOService *provider)
{
  if (gate_) {
    IOLockLock(gate_);
  }
  online_ = false;
  native_t70_ufs_block_ready = 0;
  releaseProvider(true);
  if (gate_) {
    IOLockUnlock(gate_);
  }
  IOBlockStorageDevice::stop(provider);
}

void NativeT70UfsBlock::free()
{
  if (gate_) {
    IOLockLock(gate_);
  }
  online_ = false;
  native_t70_ufs_block_ready = 0;
  releaseProvider(true);
  if (gate_) {
    IOLockUnlock(gate_);
  }
  releaseLocalStorage();
  IOBlockStorageDevice::free();
}
