// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T70UfsControllerProvider.h"
#include "T70UfsHost.h"
#include "T70UfsDeviceTree.h"
#include "T70UfsMemory.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>

extern "C" {
extern uint64_t gDramBase, gDramSize;
void *t70_ufs_quarantine_state = nullptr;
volatile uint32_t t70_ufs_ready = 0;
volatile uint32_t t70_ufs_probe_attempts = 0;
volatile uint32_t t70_ufs_probe_refusals = 0;
volatile uint32_t t70_ufs_start_attempts = 0;
volatile uint32_t t70_ufs_mmio_accesses = 0;
volatile uint32_t t70_ufs_dma_quarantines = 0;
}
namespace {
constexpr uint64_t handoffPhysical = 0x43f7f000;
constexpr uint64_t controllerPhysical = 0x11270000;
constexpr uint64_t controllerBytes = 0x2300;
constexpr uint64_t handoffMagic = 0x0000000148303754;
volatile uint32_t ownerClaimed = 0;

bool requestedOnActualBoard()
{
  int requested = 0;
  if (!PE_parse_boot_argn("t70-ufs", &requested, sizeof(requested)) || requested != 1) return false;
  IORegistryEntry *root = IORegistryEntry::fromPath("/", gIODTPlane);
  if (!root) return false;
  OSData *compatible = OSDynamicCast(OSData, root->getProperty("compatible"));
  const char expected[] = "mediatek,MT6789";
  bool matches = compatible && compatible->getLength() == sizeof(expected) &&
                 memcmp(compatible->getBytesNoCopy(), expected, sizeof(expected)) == 0;
  root->release();
  return matches;
}

IOMemoryMap *mapRange(uint64_t physical, uint64_t bytes, bool readOnly)
{
  IODeviceMemory *memory = IODeviceMemory::withRange(physical, bytes);
  if (!memory) return nullptr;
  IOOptionBits options = kIOMapInhibitCache;
  if (readOnly) options |= kIOMapReadOnly;
  IOMemoryMap *mapping = memory->map(options);
  memory->release();
  if (mapping && mapping->getLength() < bytes) {
    mapping->release();
    return nullptr;
  }
  return mapping;
}
}

struct T70UfsControllerState {
  T70_UFS_HOST host;
  IOLock *gate;
  IOService *client;
  IOMemoryMap *handoffMap;
  IOMemoryMap *fdtMap;
  IOMemoryMap *controllerMap;
  IOBufferMemoryDescriptor *dma;
  bool dmaPrepared;
  bool ownsClaim;
  bool quarantineRecorded;
};

namespace {
uint32_t readMapped(IOMemoryMap *mapping, uint32_t offset)
{
  uint32_t value = *reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress() + offset);
  __asm__ volatile("dmb oshld" ::: "memory");
  return value;
}
uint64_t readMapped64(IOMemoryMap *mapping, uint32_t offset)
{
  uint64_t low = readMapped(mapping, offset);
  return low | (static_cast<uint64_t>(readMapped(mapping, offset + 4)) << 32);
}
T70_UFS_HANDOFF_SNAPSHOT snapshot(T70UfsControllerState *s)
{
  T70_UFS_HANDOFF_SNAPSHOT result = {};
  result.HandoffAddress = handoffPhysical;
  result.HandoffReady = readMapped(s->handoffMap, 0xb0);
  result.Magic = readMapped64(s->handoffMap, 0);
  result.FdtAddress = readMapped64(s->handoffMap, 0x48);
  result.FdtSize = readMapped64(s->handoffMap, 0x50);
  result.FdtStatus = readMapped64(s->handoffMap, 0x58);
  result.HandoffMmioBase = readMapped(s->handoffMap, 0xb4);
  result.HandoffLun = readMapped(s->handoffMap, 0xb8);
  result.HandoffFormat = readMapped(s->handoffMap, 0xbc);
  if (readMapped(s->handoffMap, 0xb0) != result.HandoffReady) result.HandoffReady = 0;
  return result;
}
bool validSnapshot(const T70_UFS_HANDOFF_SNAPSHOT &proof)
{
  return proof.Magic == handoffMagic && proof.FdtStatus == 0 && proof.HandoffReady == 1 &&
         proof.HandoffMmioBase == controllerPhysical && proof.HandoffLun == MTK_UFS_USER_LUN &&
         proof.HandoffFormat == 1 && proof.FdtAddress >= 0x40000000 && !(proof.FdtAddress & 0xfff) &&
         proof.FdtSize >= 40 && proof.FdtSize <= 0x100000 &&
         proof.FdtAddress <= UINT64_MAX - proof.FdtSize &&
         T70UfsRangeOutsideOwnedRam(proof.FdtAddress, (proof.FdtSize + 0xfff) & ~UINT64_C(0xfff),
                                    gDramBase, gDramSize) &&
         T70UfsRangeOutsideOwnedRam(proof.FdtAddress, proof.FdtSize, handoffPhysical, 0x1000);
}
uint32_t readController(void *context, uint32_t offset)
{
  auto *s = static_cast<T70UfsControllerState *>(context);
  if (!s->controllerMap || (offset & 3) || offset > controllerBytes - 4) return UINT32_MAX;
  __atomic_fetch_add(&t70_ufs_mmio_accesses, 1U, __ATOMIC_RELAXED);
  return readMapped(s->controllerMap, offset);
}
void writeController(void *context, uint32_t offset, uint32_t value)
{
  auto *s = static_cast<T70UfsControllerState *>(context);
  if (!s->controllerMap || (offset & 3) || offset > controllerBytes - 4) return;
  __atomic_fetch_add(&t70_ufs_mmio_accesses, 1U, __ATOMIC_RELAXED);
  *reinterpret_cast<volatile uint32_t *>(s->controllerMap->getVirtualAddress() + offset) = value;
  __asm__ volatile("dsb oshst" ::: "memory");
}
void fenceController(void *)
{
  __asm__ volatile("dsb osh" ::: "memory");
}
void stallController(void *, uint32_t microseconds)
{
  IODelay(microseconds);
}
}

OSDefineMetaClassAndStructors(T70UfsControllerProvider, IOService)

IOService *T70UfsControllerProvider::probe(IOService *provider, SInt32 *score)
{
  __atomic_fetch_add(&t70_ufs_probe_attempts, 1U, __ATOMIC_RELAXED);
  if (!requestedOnActualBoard()) {
    __atomic_fetch_add(&t70_ufs_probe_refusals, 1U, __ATOMIC_RELAXED);
    return nullptr;
  }
  return IOService::probe(provider, score);
}

bool T70UfsControllerProvider::start(IOService *provider)
{
  __atomic_fetch_add(&t70_ufs_start_attempts, 1U, __ATOMIC_RELAXED);
  if (!requestedOnActualBoard() || state || !IOService::start(provider)) return false;
  state = static_cast<T70UfsControllerState *>(IOMalloc(sizeof(*state)));
  if (!state) { IOService::stop(provider); return false; }
  bzero(state, sizeof(*state));
  T70UfsHostInitialize(&state->host);
  state->gate = IOLockAlloc();
  if (!state->gate) { IOService::stop(provider); return false; }
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&ownerClaimed, &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    IOService::stop(provider); return false;
  }
  state->ownsClaim = true;
  // UEFI reserves the marker and allocates its FDT copy as ACPI memory, outside this window.
  if (!T70UfsRangeOutsideOwnedRam(handoffPhysical, 0x1000, gDramBase, gDramSize)) {
    IOService::stop(provider); return false;
  }
  state->handoffMap = mapRange(handoffPhysical, 0x1000, true);
  if (!state->handoffMap) { IOService::stop(provider); return false; }
  auto proof = snapshot(state);
  if (!validSnapshot(proof)) { IOService::stop(provider); return false; }
  state->fdtMap = mapRange(proof.FdtAddress, proof.FdtSize, true);
  if (!state->fdtMap) { IOService::stop(provider); return false; }
  proof.FdtNodeStatus = T70UfsValidateDeviceTree(
    reinterpret_cast<const void *>(state->fdtMap->getVirtualAddress()), static_cast<size_t>(proof.FdtSize));
  if (proof.FdtNodeStatus != 1) { IOService::stop(provider); return false; }
  proof.FdtControllerBase = controllerPhysical;
  proof.FdtControllerSize = controllerBytes;
  auto finalProof = snapshot(state);
  if (!validSnapshot(finalProof) || finalProof.FdtAddress != proof.FdtAddress ||
      finalProof.FdtSize != proof.FdtSize) { IOService::stop(provider); return false; }
  state->controllerMap = mapRange(controllerPhysical, controllerBytes, false);
  if (!state->controllerMap) { IOService::stop(provider); return false; }
  // The stock UFS node does not promise coherent DMA; allocate an uncached private window.
  const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
    static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits>(kIOMemoryMapperNone) | static_cast<IOOptionBits>(kIOMapInhibitCache);
  state->dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options,
    MTK_UFS_DMA_BYTES, 0x00000000fffff000ULL);
  if (!state->dma) { IOService::stop(provider); return false; }
  if (state->dma->prepare(kIODirectionInOut) != kIOReturnSuccess) { IOService::stop(provider); return false; }
  state->dmaPrepared = true;
  IOByteCount length = 0;
  uint64_t physical = state->dma->getPhysicalSegment(0, &length);
  if (!physical || (physical & 0xfff) || length < MTK_UFS_DMA_BYTES ||
      physical > UINT64_C(0x100000000) - MTK_UFS_DMA_BYTES) { IOService::stop(provider); return false; }
  MTK_UFS_TRANSPORT_IO io = {state, readController, writeController, fenceController, stallController};
  if (T70UfsHostStart(&state->host, &proof, controllerPhysical, &io,
                     state->dma->getBytesNoCopy(), MTK_UFS_DMA_BYTES, physical) != T70UfsHostSuccess) {
    IOService::stop(provider); return false;
  }
  __atomic_store_n(&t70_ufs_ready, 1U, __ATOMIC_RELEASE);
  registerService();
  return true;
}

bool T70UfsControllerProvider::isReadyForReadOnlyLu2() const
{
  if (!state || !state->gate) return false;
  IOLockLock(state->gate);
  bool ready = state->host.Ready && !state->host.DmaQuarantined;
  IOLockUnlock(state->gate);
  return ready;
}
uint64_t T70UfsControllerProvider::lastLogicalBlock() const
{
  if (!state || !state->gate) return 0;
  IOLockLock(state->gate);
  uint64_t value = state->host.Ready ? state->host.LastLogicalBlock : 0;
  IOLockUnlock(state->gate);
  return value;
}
uint32_t T70UfsControllerProvider::blockSize() const
{
  if (!state || !state->gate) return 0;
  IOLockLock(state->gate);
  uint32_t value = state->host.Ready ? state->host.BlockSize : 0;
  IOLockUnlock(state->gate);
  return value;
}
bool T70UfsControllerProvider::handleOpen(IOService *client, IOOptionBits, void *)
{
  if (!state || !state->gate || !client) return false;
  IOLockLock(state->gate);
  bool granted = state->host.Ready && !state->host.DmaQuarantined && !state->client;
  if (granted) state->client = client;
  IOLockUnlock(state->gate);
  return granted;
}
void T70UfsControllerProvider::handleClose(IOService *client, IOOptionBits)
{
  if (!state || !state->gate) return;
  IOLockLock(state->gate);
  if (state->client == client) state->client = nullptr;
  IOLockUnlock(state->gate);
}
bool T70UfsControllerProvider::handleIsOpen(const IOService *client) const
{
  if (!state || !state->gate) return false;
  IOLockLock(state->gate);
  bool open = state->client && (!client || state->client == client);
  IOLockUnlock(state->gate);
  return open;
}
T70_UFS_READ_STATUS T70UfsControllerProvider::readBlocks(uint64_t lba, uint64_t count, void *buffer, size_t bytes)
{
  if (!state || !state->gate) return T70UfsReadNotReady;
  IOLockLock(state->gate);
  T70_UFS_READ_STATUS result = T70UfsHostReadBlocks(&state->host, lba, count, buffer, bytes);
  if (!state->host.Ready) __atomic_store_n(&t70_ufs_ready, 0U, __ATOMIC_RELEASE);
  IOLockUnlock(state->gate);
  return result;
}
bool T70UfsControllerProvider::quiesceDmaForStop()
{
  if (!state) return true;
  if (state->ownsClaim) __atomic_store_n(&t70_ufs_ready, 0U, __ATOMIC_RELEASE);
  if (state->gate) IOLockLock(state->gate);
  T70UfsHostStop(&state->host);
  bool stopped = state->host.StopSucceeded && !state->host.DmaQuarantined;
  if (!stopped && !state->quarantineRecorded) {
    __atomic_fetch_add(&t70_ufs_dma_quarantines, 1U, __ATOMIC_RELAXED);
    state->quarantineRecorded = true;
  }
  if (stopped && state->ownsClaim) {
    __atomic_store_n(&ownerClaimed, 0U, __ATOMIC_RELEASE);
    state->ownsClaim = false;
  }
  if (state->gate) IOLockUnlock(state->gate);
  return stopped;
}
void T70UfsControllerProvider::stop(IOService *provider)
{
  quiesceDmaForStop();
  IOService::stop(provider);
}
void T70UfsControllerProvider::free()
{
  if (state && quiesceDmaForStop()) {
    if (state->dma) {
      if (state->dmaPrepared) state->dma->complete(kIODirectionInOut);
      state->dma->release();
    }
    if (state->controllerMap) state->controllerMap->release();
    if (state->fdtMap) state->fdtMap->release();
    if (state->handoffMap) state->handoffMap->release();
    if (state->gate) IOLockFree(state->gate);
    IOFree(state, sizeof(*state));
  } else if (state) {
    __atomic_store_n(&t70_ufs_quarantine_state, static_cast<void *>(state), __ATOMIC_RELEASE);
  }
  // shortcut: quarantined state has no reset recovery entry; reclaim only after a proven controller reset.
  state = nullptr;
  IOService::free();
}
