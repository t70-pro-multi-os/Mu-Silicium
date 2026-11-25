/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define T70_UFS_BLOCK_METHOD_TEST 1
#include "NativeT70UfsBlock.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr unsigned kMaxReads = 4;
struct ProviderFixture {
  T70_UFS_READ_STATUS readStatus = T70UfsReadDmaOwnershipUncertain;
  T70_UFS_READ_STATUS readStatuses[kMaxReads]{};
  unsigned readStatusCount = 0;
  bool stopSucceeded = false;
  unsigned readCalls = 0;
  unsigned stopCalls = 0;
  uint64_t readLbas[kMaxReads]{};
  uint64_t readCounts[kMaxReads]{};
  size_t readBufferBytes[kMaxReads]{};
} gProviderFixture;

struct CompletionFixture {
  bool called = false;
  IOReturn result = 0;
  IOByteCount bytes = UINT64_MAX;
};

void complete(void *target, void *, IOReturn result, IOByteCount bytes)
{
  auto *fixture = static_cast<CompletionFixture *>(target);
  fixture->called = true;
  fixture->result = result;
  fixture->bytes = bytes;
}

void check(bool condition, const char *label)
{
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
  }
}

void resetProvider(T70_UFS_READ_STATUS status)
{
  gProviderFixture = {};
  gProviderFixture.readStatus = status;
}

void resetObservers()
{
  native_t70_ufs_block_ready = 0;
  native_t70_ufs_block_read_requests = 0;
  native_t70_ufs_block_read_failures = 0;
  native_t70_ufs_block_write_requests = 0;
  native_t70_ufs_block_write_refusals = 0;
  native_t70_ufs_block_dma_quarantined = 0;
}

bool matchesBlockPattern(const IOMemoryDescriptor &descriptor,
                         uint64_t byteOffset, uint64_t firstLba,
                         uint64_t blockCount)
{
  const uint64_t byteCount = blockCount * MTK_UFS_BLOCK_BYTES;
  if (byteOffset > descriptor.bytes.size() ||
      byteCount > descriptor.bytes.size() - byteOffset) {
    return false;
  }
  for (uint64_t block = 0; block < blockCount; ++block) {
    const uint8_t expected = static_cast<uint8_t>((firstLba + block) & 0xffU);
    const uint64_t start = byteOffset + block * MTK_UFS_BLOCK_BYTES;
    for (uint64_t byte = 0; byte < MTK_UFS_BLOCK_BYTES; ++byte) {
      if (descriptor.bytes[static_cast<size_t>(start + byte)] != expected) {
        return false;
      }
    }
  }
  return true;
}

bool bytesEqual(const IOMemoryDescriptor &descriptor, uint64_t byteOffset,
                uint64_t byteCount, uint8_t value)
{
  if (byteOffset > descriptor.bytes.size() ||
      byteCount > descriptor.bytes.size() - byteOffset) {
    return false;
  }
  for (uint64_t byte = 0; byte < byteCount; ++byte) {
    if (descriptor.bytes[static_cast<size_t>(byteOffset + byte)] != value) {
      return false;
    }
  }
  return true;
}
}

struct NativeT70UfsBlockMethodHarness {
  static void prepare(NativeT70UfsBlock &device, T70UfsControllerProvider *provider,
                      uint64_t lastLba, uint32_t blockBytes, bool online)
  {
    device.controller_ = provider;
    device.providerOpen_ = true;
    device.providerQuiesced_ = false;
    device.lastLogicalBlock_ = lastLba;
    device.blockBytes_ = blockBytes;
    device.online_ = online;
    device.gate_ = IOLockAlloc();
    device.staging_ = static_cast<uint8_t *>(IOMalloc(MTK_UFS_DATA_BYTES));
    check(device.gate_ != nullptr && device.staging_ != nullptr,
          "test device allocations succeed");
  }
  static bool online(const NativeT70UfsBlock &device) { return device.online_; }
  static void cleanup(NativeT70UfsBlock &device)
  {
    IOLockFree(device.gate_);
    device.gate_ = nullptr;
    IOFree(device.staging_, MTK_UFS_DATA_BYTES);
    device.staging_ = nullptr;
  }
};

bool T70UfsControllerProvider::handleOpen(IOService *, IOOptionBits, void *) { return true; }
void T70UfsControllerProvider::handleClose(IOService *, IOOptionBits) {}
bool T70UfsControllerProvider::handleIsOpen(const IOService *) const { return true; }
IOService *T70UfsControllerProvider::probe(IOService *, SInt32 *) { return nullptr; }
bool T70UfsControllerProvider::start(IOService *) { return false; }
void T70UfsControllerProvider::stop(IOService *) {}
void T70UfsControllerProvider::free() {}
bool T70UfsControllerProvider::isReadyForReadOnlyLu2() const { return true; }
uint64_t T70UfsControllerProvider::lastLogicalBlock() const
{
  return (MTK_UFS_USER_BYTES / MTK_UFS_BLOCK_BYTES) - 1;
}
uint32_t T70UfsControllerProvider::blockSize() const { return MTK_UFS_BLOCK_BYTES; }
T70_UFS_READ_STATUS T70UfsControllerProvider::readBlocks(
  uint64_t lba, uint64_t count, void *buffer, size_t bufferBytes)
{
  const unsigned call = gProviderFixture.readCalls++;
  if (call < kMaxReads) {
    gProviderFixture.readLbas[call] = lba;
    gProviderFixture.readCounts[call] = count;
    gProviderFixture.readBufferBytes[call] = bufferBytes;
  }
  const T70_UFS_READ_STATUS status =
    call < gProviderFixture.readStatusCount
      ? gProviderFixture.readStatuses[call]
      : gProviderFixture.readStatus;
  if (status == T70UfsReadSuccess && buffer != nullptr) {
    auto *output = static_cast<uint8_t *>(buffer);
    for (size_t byte = 0; byte < bufferBytes; ++byte) {
      output[byte] = static_cast<uint8_t>(
        (lba + byte / MTK_UFS_BLOCK_BYTES) & 0xffU);
    }
  }
  return status;
}
bool T70UfsControllerProvider::quiesceDmaForStop()
{
  ++gProviderFixture.stopCalls;
  return gProviderFixture.stopSucceeded;
}

int main()
{
  const uint64_t last = (MTK_UFS_USER_BYTES / MTK_UFS_BLOCK_BYTES) - 1;
  T70UfsControllerProvider provider;
  NativeT70UfsBlock device;
  CompletionFixture completionState;
  IOStorageCompletion completion{&completionState, nullptr, complete};

  resetObservers();
  resetProvider(T70UfsReadDeviceError);
  NativeT70UfsBlockMethodHarness::prepare(device, &provider, last,
                                           MTK_UFS_BLOCK_BYTES, false);
  IOMemoryDescriptor writeBuffer(MTK_UFS_BLOCK_BYTES, kIODirectionOut);
  const IOReturn writeResult = device.doAsyncReadWrite(
    &writeBuffer, 0, 1, nullptr, &completion);
  check(writeResult == kIOReturnNotWritable, "write request is refused immediately");
  check(!completionState.called && gProviderFixture.readCalls == 0,
        "write refusal does not call provider or completion");
  check(native_t70_ufs_block_write_requests == 1 &&
        native_t70_ufs_block_write_refusals == 1, "write refusal counters advance");
  NativeT70UfsBlockMethodHarness::cleanup(device);

  resetObservers();
  resetProvider(T70UfsReadDeviceError);
  completionState = {};
  NativeT70UfsBlockMethodHarness::prepare(device, &provider, last,
                                           MTK_UFS_BLOCK_BYTES, false);
  IOMemoryDescriptor rangeBuffer(MTK_UFS_BLOCK_BYTES, kIODirectionIn);
  const IOReturn rangeResult = device.doAsyncReadWrite(
    &rangeBuffer, last + 1, 1, nullptr, &completion);
  check(rangeResult == kIOReturnBadArgument, "past-end LBA is rejected");
  check(gProviderFixture.readCalls == 0, "range rejection does not reach provider");
  NativeT70UfsBlockMethodHarness::cleanup(device);

  resetObservers();
  resetProvider(T70UfsReadDeviceError);
  completionState = {};
  NativeT70UfsBlockMethodHarness::prepare(device, &provider, last,
                                           MTK_UFS_BLOCK_BYTES, false);
  IOMemoryDescriptor shortBuffer(MTK_UFS_BLOCK_BYTES - 1, kIODirectionIn);
  const IOReturn shortResult = device.doAsyncReadWrite(
    &shortBuffer, 0, 1, nullptr, &completion);
  check(shortResult == kIOReturnBadArgument, "short descriptor is rejected");
  check(gProviderFixture.readCalls == 0, "short descriptor does not reach provider");
  NativeT70UfsBlockMethodHarness::cleanup(device);

  resetObservers();
  resetProvider(T70UfsReadSuccess);
  completionState = {};
  NativeT70UfsBlockMethodHarness::prepare(device, &provider, last,
                                           MTK_UFS_BLOCK_BYTES, true);
  IOMemoryDescriptor normalBuffer(MTK_UFS_BLOCK_BYTES, kIODirectionIn);
  std::memset(normalBuffer.bytes.data(), 0xa5, normalBuffer.bytes.size());
  const uint64_t normalLba = 10;
  const IOReturn normalMethodResult = device.doAsyncReadWrite(
    &normalBuffer, normalLba, 1, nullptr, &completion);
  check(normalMethodResult == kIOReturnSuccess && completionState.called,
        "normal read submits and completes");
  check(completionState.result == kIOReturnSuccess &&
        completionState.bytes == MTK_UFS_BLOCK_BYTES,
        "normal read reports the full block");
  check(gProviderFixture.readCalls == 1 &&
        gProviderFixture.readLbas[0] == normalLba &&
        gProviderFixture.readCounts[0] == 1 &&
        gProviderFixture.readBufferBytes[0] == MTK_UFS_BLOCK_BYTES,
        "normal read sends exact LBA and length to provider");
  check(matchesBlockPattern(normalBuffer, 0, normalLba, 1),
        "normal read copies provider data to caller");
  check(native_t70_ufs_block_read_failures == 0,
        "normal read does not increment failures");
  NativeT70UfsBlockMethodHarness::cleanup(device);

  resetObservers();
  resetProvider(T70UfsReadSuccess);
  completionState = {};
  NativeT70UfsBlockMethodHarness::prepare(device, &provider, last,
                                           MTK_UFS_BLOCK_BYTES, true);
  const uint64_t chunkBlocks = MTK_UFS_DATA_BYTES / MTK_UFS_BLOCK_BYTES;
  const uint64_t splitBlocks = chunkBlocks + 1;
  const uint64_t splitBytes = splitBlocks * MTK_UFS_BLOCK_BYTES;
  const uint64_t splitLba = 100;
  IOMemoryDescriptor splitBuffer(splitBytes, kIODirectionIn);
  std::memset(splitBuffer.bytes.data(), 0xa5, splitBuffer.bytes.size());
  const IOReturn splitMethodResult = device.doAsyncReadWrite(
    &splitBuffer, splitLba, splitBlocks, nullptr, &completion);
  check(splitMethodResult == kIOReturnSuccess && completionState.called,
        "over-boundary read submits and completes");
  check(completionState.result == kIOReturnSuccess &&
        completionState.bytes == splitBytes,
        "over-boundary read reports all bytes");
  check(gProviderFixture.readCalls == 2 &&
        gProviderFixture.readLbas[0] == splitLba &&
        gProviderFixture.readCounts[0] == chunkBlocks &&
        gProviderFixture.readBufferBytes[0] == MTK_UFS_DATA_BYTES &&
        gProviderFixture.readLbas[1] == splitLba + chunkBlocks &&
        gProviderFixture.readCounts[1] == 1 &&
        gProviderFixture.readBufferBytes[1] == MTK_UFS_BLOCK_BYTES,
        "128 KiB limit splits into a full chunk and one block");
  check(matchesBlockPattern(splitBuffer, 0, splitLba, splitBlocks),
        "split read copies both chunks in order");
  check(native_t70_ufs_block_read_failures == 0,
        "split success does not increment failures");
  NativeT70UfsBlockMethodHarness::cleanup(device);

  resetObservers();
  resetProvider(T70UfsReadDeviceError);
  gProviderFixture.readStatusCount = 2;
  gProviderFixture.readStatuses[0] = T70UfsReadSuccess;
  gProviderFixture.readStatuses[1] = T70UfsReadDeviceError;
  completionState = {};
  NativeT70UfsBlockMethodHarness::prepare(device, &provider, last,
                                           MTK_UFS_BLOCK_BYTES, true);
  const uint64_t partialLba = 200;
  IOMemoryDescriptor partialBuffer(splitBytes, kIODirectionIn);
  std::memset(partialBuffer.bytes.data(), 0xa5, partialBuffer.bytes.size());
  const IOReturn partialMethodResult = device.doAsyncReadWrite(
    &partialBuffer, partialLba, splitBlocks, nullptr, &completion);
  check(partialMethodResult == kIOReturnSuccess && completionState.called,
        "later-chunk error completes the request");
  check(completionState.result == kIOReturnIOError &&
        completionState.bytes == MTK_UFS_DATA_BYTES,
        "later-chunk error reports exactly the completed first chunk");
  check(gProviderFixture.readCalls == 2 &&
        gProviderFixture.readLbas[0] == partialLba &&
        gProviderFixture.readCounts[0] == chunkBlocks &&
        gProviderFixture.readLbas[1] == partialLba + chunkBlocks &&
        gProviderFixture.readCounts[1] == 1,
        "later-chunk error occurs after the expected split");
  check(matchesBlockPattern(partialBuffer, 0, partialLba, chunkBlocks),
        "successful first chunk is copied");
  check(bytesEqual(partialBuffer, MTK_UFS_DATA_BYTES, MTK_UFS_BLOCK_BYTES, 0xa5),
        "failed second chunk is not copied to caller");
  check(native_t70_ufs_block_read_failures == 1 &&
        gProviderFixture.stopCalls == 0 &&
        NativeT70UfsBlockMethodHarness::online(device),
        "ordinary device error counts failure without DMA quarantine");
  NativeT70UfsBlockMethodHarness::cleanup(device);

  resetObservers();
  resetProvider(T70UfsReadDmaOwnershipUncertain);
  gProviderFixture.stopSucceeded = false;
  completionState = {};
  NativeT70UfsBlockMethodHarness::prepare(device, &provider, last,
                                           MTK_UFS_BLOCK_BYTES, true);
  IOMemoryDescriptor uncertainBuffer(MTK_UFS_BLOCK_BYTES, kIODirectionIn);
  std::memset(uncertainBuffer.bytes.data(), 0xa5, uncertainBuffer.bytes.size());
  const IOReturn uncertainMethodResult = device.doAsyncReadWrite(
    &uncertainBuffer, 0, 1, nullptr, &completion);
  check(uncertainMethodResult == kIOReturnSuccess && completionState.called,
        "uncertain DMA is reported through completion");
  check(completionState.result == kIOReturnNotReady && completionState.bytes == 0,
        "uncertain DMA completes with no exposed bytes");
  check(uncertainBuffer.bytes[0] == 0xa5,
        "uncertain DMA output was not copied to caller");
  check(gProviderFixture.readCalls == 1 && gProviderFixture.stopCalls == 1,
        "uncertain DMA requests provider quarantine");
  check(!NativeT70UfsBlockMethodHarness::online(device) &&
        native_t70_ufs_block_dma_quarantined == 1,
        "uncertain DMA offlines block and records quarantine");
  NativeT70UfsBlockMethodHarness::cleanup(device);

  std::puts("PASS: actual NativeT70UfsBlock method passes normal/split reads, partial completion, refusal, and DMA quarantine");
  return 0;
}
