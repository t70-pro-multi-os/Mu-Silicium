// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../MediaTekTest.h"
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#include <Library/MtkUfsCore.h>
#include "../../../../../Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsReadOnlyBlockIoDxe/MtkUfsReadOnlyBlockIoDxe.c"

EFI_GUID gEfiScsiIoProtocolGuid = { 0x101U, 0U, 0U, { 0U } };
EFI_GUID gEfiDevicePathProtocolGuid = { 0x102U, 0U, 0U, { 0U } };
EFI_GUID gEfiBlockIoProtocolGuid = { 0x103U, 0U, 0U, { 0U } };
EFI_GUID gEfiDriverBindingProtocolGuid = { 0x104U, 0U, 0U, { 0U } };

static EFI_SCSI_IO_PROTOCOL       MockScsiIo;
static EFI_DEVICE_PATH_PROTOCOL   *MockDevicePath;
static EFI_BLOCK_IO_PROTOCOL      GenericBlockIo;
static EFI_BLOCK_IO_PROTOCOL      *MockGenericBlockIo;
static EFI_BLOCK_IO_PROTOCOL      *PublishedBlockIo;
static EFI_DRIVER_BINDING_PROTOCOL *PublishedDriverBinding;
static T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS *PublishedDiagnostics;
static EFI_HANDLE                 MockController = (EFI_HANDLE)(UINTN)0x1010U;
static EFI_STATUS                 MockOpenStatus = EFI_SUCCESS;
static EFI_STATUS                 MockCloseStatus = EFI_SUCCESS;
static BOOLEAN                    ScsiClaimed;
static EFI_STATUS                 MockBlockInstallStatus = EFI_SUCCESS;
static EFI_STATUS                 MockBlockUninstallStatus = EFI_SUCCESS;
static EFI_STATUS                 MockScsiStatus = EFI_SUCCESS;
static EFI_STATUS                 MockDriverBindingInstallStatus = EFI_SUCCESS;
static EFI_STATUS                 MockDiagnosticsUninstallStatus = EFI_SUCCESS;
static UINTN                      DiagnosticsUninstallCalls;
static UINT32                     MockBlockSize = 4096U;
static UINT32                     MockLastBlock = 31240191U;
static UINT64                     MockLastBlock16 = 31240191U;
static UINTN                      CapacityFault;
static BOOLEAN                    FaultCapacity16;
static UINTN                      OpenCalls;
static UINTN                      CloseCalls;
static UINTN                      BlockInstallCalls;
static UINTN                      BlockUninstallCalls;
static UINTN                      PageAllocations;
static UINTN                      PageFrees;
static UINTN                      ScsiCommands;
static UINTN                      ReadCapacityCommands;
static UINTN                      ReadDataCommands;
static UINT64                     LastReadLba;
static UINT32                     LastReadBlocks;
static UINTN                      FailReadCommand;
static UINTN                      ShortReadCommand;
static EFI_STATUS                 MockFreeStatus = EFI_SUCCESS;
static EFI_PHYSICAL_ADDRESS       FailedFreeMemory;
static UINTN                      FailedFreePages, FreeAttempts;
static UINT64                     ReadDelayNs;
static UINT64                     ReadLbaTrace[128];
static UINT32                     ReadSizeTrace[128];
static UINT64                     ReadTimeoutTrace[128];
static UINTN                      ReadTraceCount;
static UINTN                      MaxAllocationBytes;
static UINT64                     CounterFrequency = 1000000000ULL;

static UINT16
NodeLength (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *Node
  )
{
  return (UINT16)(Node->Length[0] | ((UINT16)Node->Length[1] << 8));
}

UINTN
EFIAPI
DevicePathNodeLength (
  IN CONST VOID  *Node
  )
{
  return NodeLength ((CONST EFI_DEVICE_PATH_PROTOCOL *)Node);
}

EFI_DEVICE_PATH_PROTOCOL *
EFIAPI
NextDevicePathNode (
  IN CONST VOID  *Node
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Current;

  Current = (CONST EFI_DEVICE_PATH_PROTOCOL *)Node;
  return (EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)Node + NodeLength (Current));
}

BOOLEAN
EFIAPI
IsDevicePathEnd (
  IN CONST VOID  *Node
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Current;

  Current = (CONST EFI_DEVICE_PATH_PROTOCOL *)Node;
  return (BOOLEAN)(Current->Type == END_DEVICE_PATH_TYPE);
}

BOOLEAN
EFIAPI
CompareGuid (
  IN CONST EFI_GUID  *Guid1,
  IN CONST EFI_GUID  *Guid2
  )
{
  return (BOOLEAN)(memcmp (Guid1, Guid2, sizeof (*Guid1)) == 0);
}

UINT64
EFIAPI
GetPerformanceCounterProperties (
  OUT UINT64  *StartValue,
  OUT UINT64  *EndValue
  )
{
  if (StartValue != NULL) {
    *StartValue = 0;
  }

  if (EndValue != NULL) {
    *EndValue = MAX_UINT64;
  }

  return CounterFrequency;
}

static EFI_STATUS
EFIAPI
MockGetDeviceType (
  IN  EFI_SCSI_IO_PROTOCOL  *This,
  OUT UINT8                 *DeviceType
  )
{
  (VOID)This;
  if (DeviceType == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceType = EFI_SCSI_IO_TYPE_DISK;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockGetDeviceLocation (
  IN     EFI_SCSI_IO_PROTOCOL  *This,
  IN OUT UINT8                 **Target,
  OUT    UINT64                *Lun
  )
{
  (VOID)This;
  if ((Target == NULL) || (*Target == NULL) || (Lun == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (*Target, TARGET_MAX_BYTES);
  *Lun = 2ULL << 8;
  return EFI_SUCCESS;
}

static UINT32
GetBigEndian32 (
  IN CONST UINT8  *Bytes
  )
{
  return ((UINT32)Bytes[0] << 24) |
         ((UINT32)Bytes[1] << 16) |
         ((UINT32)Bytes[2] << 8) |
         (UINT32)Bytes[3];
}

static EFI_STATUS
EFIAPI
MockExecuteScsiCommand (
  IN     EFI_SCSI_IO_PROTOCOL             *This,
  IN OUT EFI_SCSI_IO_SCSI_REQUEST_PACKET  *Packet,
  IN     EFI_EVENT                        Event OPTIONAL
  )
{
  UINT8   *Cdb;
  UINT8   *Data;
  UINT32  RequestedLength;
  UINT32  Index;

  (VOID)This;
  (VOID)Event;
  if ((Packet == NULL) || (Packet->Cdb == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Cdb = (UINT8 *)Packet->Cdb;
  ScsiCommands++;
  if ((Cdb[0] == 0x25U) && (Packet->CdbLength == 10U)) {
    ReadCapacityCommands++;
  } else if ((Cdb[0] == 0x9EU) && (Packet->CdbLength == 16U)) {
    ReadCapacityCommands++;
  } else if ((Cdb[0] == 0x28U) && (Packet->CdbLength == 10U)) {
    ReadDataCommands++;
    if (FailReadCommand == ReadDataCommands) return EFI_DEVICE_ERROR;
  }

  if (MockScsiStatus != EFI_SUCCESS) {
    return MockScsiStatus;
  }

  if ((Packet->DataDirection != EFI_SCSI_IO_DATA_DIRECTION_READ) ||
      (Packet->InDataBuffer == NULL))
  {
    return EFI_DEVICE_ERROR;
  }

  Data = (UINT8 *)Packet->InDataBuffer;
  RequestedLength = Packet->InTransferLength;
  Packet->HostAdapterStatus = EFI_SCSI_IO_STATUS_HOST_ADAPTER_OK;
  Packet->TargetStatus = EFI_SCSI_IO_STATUS_TARGET_GOOD;

  if ((Cdb[0] == 0x25U) && (Packet->CdbLength == 10U)) {
    if (RequestedLength != 8U) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Data[0] = (UINT8)(MockLastBlock >> 24);
    Data[1] = (UINT8)(MockLastBlock >> 16);
    Data[2] = (UINT8)(MockLastBlock >> 8);
    Data[3] = (UINT8)MockLastBlock;
    Data[4] = (UINT8)(MockBlockSize >> 24);
    Data[5] = (UINT8)(MockBlockSize >> 16);
    Data[6] = (UINT8)(MockBlockSize >> 8);
    Data[7] = (UINT8)MockBlockSize;
  } else if ((Cdb[0] == 0x9EU) && (Packet->CdbLength == 16U)) {
    CHECK (RequestedLength == 32U && Cdb[1] == 0x10U && GetBigEndian32 (&Cdb[10]) == 32U);
    memset (Data, 0, RequestedLength);
    for (Index = 0; Index < 8; Index++) Data[Index] = (UINT8)(MockLastBlock16 >> (56U - 8U * Index));
    for (Index = 0; Index < 4; Index++) Data[8U + Index] = (UINT8)(MockBlockSize >> (24U - 8U * Index));
  } else if ((Cdb[0] == 0x28U) && (Packet->CdbLength == 10U)) {
    UINT64  Lba;
    UINT32  Blocks;

    Lba = ((UINT64)Cdb[2] << 24) |
          ((UINT64)Cdb[3] << 16) |
          ((UINT64)Cdb[4] << 8) |
          (UINT64)Cdb[5];
    Blocks = ((UINT32)Cdb[7] << 8) | (UINT32)Cdb[8];
    if (((UINT64)Blocks * 4096U != RequestedLength) || (Blocks == 0U)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    CHECK(RequestedLength <= MTK_UFS_MAX_COMMAND_BYTES);
    if (ReadTraceCount < 128) {
      ReadLbaTrace[ReadTraceCount] = Lba;
      ReadSizeTrace[ReadTraceCount] = RequestedLength;
      ReadTimeoutTrace[ReadTraceCount++] = Packet->Timeout;
    }
    TimeNs += ReadDelayNs;
    LastReadLba = Lba;
    LastReadBlocks = Blocks;
    for (Index = 0; Index < RequestedLength; Index++) {
      Data[Index] = (UINT8)(((Lba * 4096ULL + Index) * 31ULL + 7ULL) & 0xFFU);
    }
  } else {
    return EFI_ACCESS_DENIED;
  }

  Packet->InTransferLength = ((Cdb[0] == 0x28U) &&
                             (ShortReadCommand == ReadDataCommands)) ?
                            RequestedLength - 1U : RequestedLength;
  Packet->SenseDataLength = 0U;
  if ((Cdb[0] == (FaultCapacity16 ? 0x9EU : 0x25U)) && CapacityFault) {
    if (CapacityFault == 1) Packet->InTransferLength--;
    if (CapacityFault == 2) Packet->InTransferLength++;
    if (CapacityFault == 3) Packet->HostAdapterStatus = 1;
    if (CapacityFault == 4) Packet->TargetStatus = 2;
    if (CapacityFault == 5) return EFI_TIMEOUT;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockHandleProtocol (
  IN  EFI_HANDLE  Handle,
  IN  EFI_GUID    *Protocol,
  OUT VOID        **Interface
  )
{
  (VOID)Handle;
  if ((Protocol == NULL) || (Interface == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Interface = NULL;
  if (CompareGuid (Protocol, &gEfiScsiIoProtocolGuid)) {
    *Interface = &MockScsiIo;
    return EFI_SUCCESS;
  }

  if (CompareGuid (Protocol, &gEfiDevicePathProtocolGuid)) {
    *Interface = MockDevicePath;
    return EFI_SUCCESS;
  }

  if (CompareGuid (Protocol, &gEfiBlockIoProtocolGuid)) {
    if (MockGenericBlockIo != NULL) {
      *Interface = MockGenericBlockIo;
      return EFI_SUCCESS;
    }

    if (PublishedBlockIo != NULL) {
      *Interface = PublishedBlockIo;
      return EFI_SUCCESS;
    }
  }

  return EFI_UNSUPPORTED;
}

static EFI_STATUS
EFIAPI
MockOpenProtocol (
  IN     EFI_HANDLE  Handle,
  IN     EFI_GUID    *Protocol,
  OUT    VOID        **Interface OPTIONAL,
  IN     EFI_HANDLE  AgentHandle,
  IN     EFI_HANDLE  ControllerHandle,
  IN     UINT32      Attributes
  )
{
  (VOID)Handle;
  (VOID)AgentHandle;
  (VOID)ControllerHandle;
  OpenCalls++;
  CHECK (CompareGuid (Protocol, &gEfiScsiIoProtocolGuid));
  CHECK (Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (MockOpenStatus)) {
    return MockOpenStatus;
  }

  if (ScsiClaimed) return EFI_ALREADY_STARTED;
  ScsiClaimed = TRUE;
  if (Interface != NULL) {
    *Interface = &MockScsiIo;
  }

  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockCloseProtocol (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN EFI_HANDLE  AgentHandle,
  IN EFI_HANDLE  ControllerHandle
  )
{
  (VOID)Handle;
  (VOID)AgentHandle;
  (VOID)ControllerHandle;
  CloseCalls++;
  CHECK (CompareGuid (Protocol, &gEfiScsiIoProtocolGuid));
  CHECK (ScsiClaimed);
  if (!EFI_ERROR (MockCloseStatus)) ScsiClaimed = FALSE;
  return MockCloseStatus;
}

static EFI_STATUS
EFIAPI
MockInstallProtocolInterface (
  IN OUT EFI_HANDLE       *Handle,
  IN     EFI_GUID         *Protocol,
  IN     EFI_INTERFACE_TYPE InterfaceType,
  IN     VOID             *Interface
  )
{
  CHECK (Handle != NULL);
  CHECK (InterfaceType == EFI_NATIVE_INTERFACE);
  if (CompareGuid (Protocol, &gEfiDriverBindingProtocolGuid)) {
    if (EFI_ERROR (MockDriverBindingInstallStatus))
      return MockDriverBindingInstallStatus;
    PublishedDriverBinding = (EFI_DRIVER_BINDING_PROTOCOL *)Interface;
    return EFI_SUCCESS;
  }

  if (CompareGuid (Protocol, &mDiagnosticsGuid)) {
    PublishedDiagnostics = (T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS *)Interface;
    return EFI_SUCCESS;
  }

  CHECK (CompareGuid (Protocol, &gEfiBlockIoProtocolGuid));
  BlockInstallCalls++;
  if (EFI_ERROR (MockBlockInstallStatus)) {
    return MockBlockInstallStatus;
  }

  PublishedBlockIo = (EFI_BLOCK_IO_PROTOCOL *)Interface;
  // Protocol notifications can consume the interface before Install returns.
  CHECK (PublishedBlockIo->ReadBlocks (PublishedBlockIo,
           PublishedBlockIo->Media->MediaId, 0, 0, NULL) == EFI_SUCCESS);
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockUninstallProtocolInterface (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN VOID        *Interface
  )
{
  (VOID)Handle;
  if (CompareGuid (Protocol, &mDiagnosticsGuid)) {
    CHECK (PublishedDiagnostics == (T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS *)Interface);
    DiagnosticsUninstallCalls++;
    if (EFI_ERROR (MockDiagnosticsUninstallStatus))
      return MockDiagnosticsUninstallStatus;
    PublishedDiagnostics = NULL;
    return EFI_SUCCESS;
  }
  CHECK (CompareGuid (Protocol, &gEfiBlockIoProtocolGuid));
  CHECK (PublishedBlockIo == (EFI_BLOCK_IO_PROTOCOL *)Interface);
  BlockUninstallCalls++;
  if (EFI_ERROR (MockBlockUninstallStatus)) {
    return MockBlockUninstallStatus;
  }

  PublishedBlockIo = NULL;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockAllocatePages (
  IN EFI_ALLOCATE_TYPE  Type,
  IN EFI_MEMORY_TYPE    MemoryType,
  IN UINTN              Pages,
  IN OUT EFI_PHYSICAL_ADDRESS  *Memory
  )
{
  UINTN  Bytes;
  VOID   *Allocation;

  CHECK (Type == AllocateMaxAddress);
  CHECK (MemoryType == EfiBootServicesData);
  CHECK (Pages != 0U);
  CHECK (Memory != NULL);
  Bytes = EFI_PAGES_TO_SIZE (Pages);
  if (Bytes > MaxAllocationBytes) MaxAllocationBytes = Bytes;
#if defined(MAP_FIXED_NOREPLACE) && !defined(__APPLE__)
  {
    static CONST UINTN  Candidates[] = {
      0x60000000U, 0x70000000U, 0x50000000U,
      0x40000000U, 0x30000000U, 0x20000000U, 0x10000000U
    };
    UINTN  Index;

    Allocation = MAP_FAILED;
    for (Index = 0; Index < sizeof (Candidates) / sizeof (Candidates[0]); Index++) {
      Allocation = mmap (
                     (VOID *)(UINTN)Candidates[Index],
                     Bytes,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                     -1,
                     0
                     );
      if (Allocation != MAP_FAILED) {
        break;
      }
    }
  }
#else
  Allocation = mmap ((VOID *)(UINTN)0x40000000U, Bytes,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
  if ((Allocation == MAP_FAILED) ||
      (((UINT64)(UINTN)Allocation + Bytes - 1U) > *Memory))
  {
    if (Allocation != MAP_FAILED) {
      munmap (Allocation, Bytes);
    }

    return EFI_OUT_OF_RESOURCES;
  }

  *Memory = (EFI_PHYSICAL_ADDRESS)(UINTN)Allocation;
  PageAllocations++;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockFreePages (
  IN EFI_PHYSICAL_ADDRESS  Memory,
  IN UINTN                 Pages
  )
{
  CHECK (Pages != 0U);
  FreeAttempts++;
  if (EFI_ERROR (MockFreeStatus)) {
    CHECK (FailedFreePages == 0U);
    FailedFreeMemory = Memory;
    FailedFreePages = Pages;
    return MockFreeStatus; // Retain the mapping: failure is not a successful free.
  }
  PageFrees++;
  return (munmap ((VOID *)(UINTN)Memory, EFI_PAGES_TO_SIZE (Pages)) == 0) ?
         EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static VOID
SetDevicePath (
  VOID
  )
{
  static UINT8  Storage[sizeof (VENDOR_DEVICE_PATH) +
                        sizeof (UFS_DEVICE_PATH) +
                        sizeof (EFI_DEVICE_PATH_PROTOCOL)];
  VENDOR_DEVICE_PATH        *Vendor;
  UFS_DEVICE_PATH           *Scsi;
  EFI_DEVICE_PATH_PROTOCOL  *End;

  ZeroMem (Storage, sizeof (Storage));
  Vendor = (VENDOR_DEVICE_PATH *)Storage;
  Vendor->Header.Type = HARDWARE_DEVICE_PATH;
  Vendor->Header.SubType = HW_VENDOR_DP;
  Vendor->Header.Length[0] = (UINT8)sizeof (*Vendor);
  Vendor->Header.Length[1] = (UINT8)(sizeof (*Vendor) >> 8);
  CopyMem (&Vendor->Guid, &mMtkUfsSeedGuid, sizeof (Vendor->Guid));

  Scsi = (UFS_DEVICE_PATH *)(Storage + sizeof (*Vendor));
  Scsi->Header.Type = MESSAGING_DEVICE_PATH;
  Scsi->Header.SubType = MSG_UFS_DP;
  Scsi->Header.Length[0] = (UINT8)sizeof (*Scsi);
  Scsi->Header.Length[1] = (UINT8)(sizeof (*Scsi) >> 8);
  Scsi->Pun = 0U;
  Scsi->Lun = 2U;

  End = (EFI_DEVICE_PATH_PROTOCOL *)(Storage + sizeof (*Vendor) + sizeof (*Scsi));
  End->Type = END_DEVICE_PATH_TYPE;
  End->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  End->Length[0] = (UINT8)sizeof (*End);
  End->Length[1] = 0U;
  MockDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Storage;
}

static VOID TestChildDevicePaths (VOID)
{
  UINT8 Path[sizeof(VENDOR_DEVICE_PATH)+sizeof(SCSI_DEVICE_PATH)+END_DEVICE_PATH_LENGTH];
  UFS_DEVICE_PATH *Ufs;
  SCSI_DEVICE_PATH *Scsi;
  EFI_DEVICE_PATH_PROTOCOL *End;
  UINTN I;
  SetDevicePath ();
  CHECK (MtkUfsHasLu2DevicePath (MockDevicePath));
  Ufs = (UFS_DEVICE_PATH *)((UINT8 *)MockDevicePath + sizeof(VENDOR_DEVICE_PATH));
  for (I=0; I<256; I++) {
    Ufs->Lun = I;
    CHECK (MtkUfsHasLu2DevicePath (MockDevicePath) == (I==2));
  }
  Ufs->Lun = 2;
  Ufs->Pun = 1; CHECK (!MtkUfsHasLu2DevicePath (MockDevicePath)); Ufs->Pun = 0;
  Ufs->Header.Length[0]--; CHECK (!MtkUfsHasLu2DevicePath (MockDevicePath)); Ufs->Header.Length[0]++;
  End = NextDevicePathNode (Ufs);
  End->SubType = END_INSTANCE_DEVICE_PATH_SUBTYPE; CHECK (!MtkUfsHasLu2DevicePath (MockDevicePath));
  End->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  ((VENDOR_DEVICE_PATH *)MockDevicePath)->Guid.Data1 ^= 1;
  CHECK (!MtkUfsHasLu2DevicePath (MockDevicePath));
  SetDevicePath ();
  ZeroMem (Path, sizeof(Path)); CopyMem (Path, MockDevicePath, sizeof(VENDOR_DEVICE_PATH));
  Scsi = (SCSI_DEVICE_PATH *)(Path+sizeof(VENDOR_DEVICE_PATH));
  Scsi->Header.Type = MESSAGING_DEVICE_PATH; Scsi->Header.SubType = MSG_SCSI_DP;
  Scsi->Header.Length[0] = sizeof(*Scsi); Scsi->Lun = 2;
  End = (EFI_DEVICE_PATH_PROTOCOL *)(Scsi+1);
  End->Type = END_DEVICE_PATH_TYPE; End->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  End->Length[0] = sizeof(*End);
  CHECK (MtkUfsHasLu2DevicePath ((EFI_DEVICE_PATH_PROTOCOL *)Path));
  Scsi->Pun = 1; CHECK (!MtkUfsHasLu2DevicePath ((EFI_DEVICE_PATH_PROTOCOL *)Path));
  CHECK (!MtkUfsHasLu2DevicePath (NULL));
}

static VOID
TestSuccessfulStartReadWriteAndStop (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       BeforeCommands;
  UINTN       BeforeReadDataCommands;
  UINTN       BeforeCloseCalls;
  UINT8       *Buffer;
  UINTN       Index;
  UINTN       BufferSize;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;

  MockBlockSize = 4096U;
  MockLastBlock = 31240191U;
  MockBlockInstallStatus = EFI_SUCCESS;
  MockBlockUninstallStatus = EFI_SUCCESS;
  MockScsiStatus = EFI_SUCCESS;
  Status = mDriverBinding.Start (&mDriverBinding, MockController, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (PublishedBlockIo != NULL);
  CHECK (PublishedBlockIo->Media != NULL);
  CHECK (PublishedBlockIo->Media->ReadOnly);
  CHECK (PublishedBlockIo->Media->BlockSize == 4096U);
  CHECK (PublishedBlockIo->Media->LastBlock == MockLastBlock);
  CHECK (mDevice.Started);
  CHECK (PageAllocations == PageFrees);

  BlockIo = PublishedBlockIo;
  BufferSize = 65U * 4096U;
  Buffer = (UINT8 *)malloc (BufferSize);
  CHECK (Buffer != NULL);
  // Capacity's LastBlock is inclusive; the next LBA must issue no command.
  CHECK (BlockIo->ReadBlocks (BlockIo, 1, MockLastBlock, 4096U, Buffer) == EFI_SUCCESS);
  CHECK (LastReadLba == MockLastBlock && LastReadBlocks == 1);
  BeforeReadDataCommands = ReadDataCommands;
  CHECK (BlockIo->ReadBlocks (BlockIo, 1, (UINT64)MockLastBlock + 1U, 4096U, Buffer) == EFI_INVALID_PARAMETER);
  CHECK (ReadDataCommands == BeforeReadDataCommands);
  memset (Buffer, 0xA5, BufferSize);
  BeforeReadDataCommands = ReadDataCommands;
  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                               0x100U, BufferSize, Buffer);
  CHECK (Status == EFI_SUCCESS);
  CHECK (ReadDataCommands == BeforeReadDataCommands + 2U);
  CHECK (LastReadLba == 0x100U + 64U);
  CHECK (LastReadBlocks == 1U);
  for (Index = 0; Index < BufferSize; Index++) {
    UINT64  AbsoluteByte;

    AbsoluteByte = ((UINT64)0x100U * 4096ULL) + Index;
    CHECK (Buffer[Index] == (UINT8)((AbsoluteByte * 31ULL + 7ULL) & 0xFFU));
  }
  // Whole contiguous FAT file >16 MiB must stream through the same 256 KiB
  // bounce buffer. Never allocate a DMA buffer proportional to caller length.
  free(Buffer);
  BufferSize = 0x1001000U;
  Buffer = malloc(BufferSize); CHECK(Buffer != NULL);
  memset(Buffer, 0xA5, BufferSize);
  ReadTraceCount = 0; MaxAllocationBytes = 0;
  CHECK(BlockIo->ReadBlocks(BlockIo, 1, 0x100, BufferSize, Buffer) == EFI_SUCCESS);
  CHECK(ReadTraceCount == 65 && MaxAllocationBytes == 0x40000U);
  for (Index=0; Index<65; ++Index) {
    CHECK(ReadLbaTrace[Index] == 0x100U + Index*64U);
    CHECK(ReadSizeTrace[Index] == (Index==64 ? 4096U : 0x40000U));
  }
  for (Index=0; Index<BufferSize; ++Index)
    CHECK(Buffer[Index] == (UINT8)(((((UINT64)0x100*4096)+Index)*31+7)&255));

  memset(Buffer, 0xA5, BufferSize);
  BeforeReadDataCommands = ReadDataCommands;
  FailReadCommand = ReadDataCommands + 3; ReadTraceCount = 0;
  CHECK(BlockIo->ReadBlocks(BlockIo, 1, 0x100, BufferSize, Buffer) == EFI_DEVICE_ERROR);
  CHECK(ReadDataCommands == BeforeReadDataCommands+3 && ReadTraceCount==2);
  for(Index=0x80000; Index<BufferSize; ++Index) CHECK(Buffer[Index]==0xA5);
  FailReadCommand = 0;

  // Four successful 30 s chunks exhaust the request-wide 120 s budget;
  // a fifth must not be issued and its destination must remain unchanged.
  memset(Buffer,0xA5,BufferSize); TimeNs=0; ReadDelayNs=30000000000ULL;
  BeforeReadDataCommands=ReadDataCommands; ReadTraceCount=0;
  CHECK(BlockIo->ReadBlocks(BlockIo,1,0x100,BufferSize,Buffer)==EFI_TIMEOUT);
  CHECK(ReadDataCommands==BeforeReadDataCommands+4 && ReadTraceCount==4);
  for(Index=0; Index<4; ++Index) CHECK(ReadTimeoutTrace[Index]==300000000ULL);
  for(Index=0x100000; Index<BufferSize; ++Index) CHECK(Buffer[Index]==0xA5);
  ReadDelayNs=0; TimeNs=0;
  CHECK(PageAllocations==PageFrees);

  // Residual budget shrinks the fifth command to 20 s; the sixth is never
  // issued even if the provider returns the fifth success late (25 s).
  // Repeat across a full-width counter wrap with identical results.
  for (UINTN Wrap = 0; Wrap < 2; Wrap++) {
    memset (Buffer, 0xA5, 6U * 0x40000U);
    TimeNs = Wrap ? MAX_UINT64 - 50000000000ULL : 0;
    ReadDelayNs = 25000000000ULL;
    BeforeReadDataCommands = ReadDataCommands;
    ReadTraceCount = 0;
    CHECK (BlockIo->ReadBlocks (BlockIo, 1, 0x100, 6U * 0x40000U, Buffer) == EFI_TIMEOUT);
    CHECK (ReadDataCommands == BeforeReadDataCommands + 5U && ReadTraceCount == 5U);
    for (Index = 0; Index < 5; Index++)
      CHECK (ReadTimeoutTrace[Index] == (Index == 4 ? 200000000ULL : 300000000ULL));
    for (Index = 5U * 0x40000U; Index < 6U * 0x40000U; Index++) CHECK (Buffer[Index] == 0xA5);
    CHECK (PageAllocations == PageFrees);
  }
  ReadDelayNs = 0; TimeNs = 0;
  CounterFrequency = 0;
  BeforeReadDataCommands = ReadDataCommands;
  CHECK (BlockIo->ReadBlocks (BlockIo, 1, 0, 4096, Buffer) == EFI_DEVICE_ERROR);
  CHECK (ReadDataCommands == BeforeReadDataCommands && PageAllocations == PageFrees);
  CounterFrequency = 1000000000ULL;

  BeforeCommands = ScsiCommands;
  CHECK (BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId,
                               0U, 4096U, Buffer) == EFI_WRITE_PROTECTED);
  CHECK (ScsiCommands == BeforeCommands);
  CHECK (BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId + 1U,
                               0U, 4096U, Buffer) == EFI_MEDIA_CHANGED);
  CHECK (ScsiCommands == BeforeCommands);

  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId + 1U,
                              0U, 4096U, Buffer) == EFI_MEDIA_CHANGED);
  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                              0U, 1U, Buffer) == EFI_BAD_BUFFER_SIZE);
  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                              MockLastBlock, 8192U, Buffer) == EFI_INVALID_PARAMETER);
  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                              0U, 4096U, NULL) == EFI_INVALID_PARAMETER);
  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                              0U, 0U, NULL) == EFI_SUCCESS);
  BeforeCommands = ScsiCommands;
  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                              0U, MAX_UINTN & ~(UINTN)4095U, Buffer) == EFI_INVALID_PARAMETER);
  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                              0U, 4096U, (VOID *)(MAX_UINTN - 2047U)) == EFI_INVALID_PARAMETER);
  CHECK (ScsiCommands == BeforeCommands);

  memset (Buffer, 0x5A, 4096U);
  MockScsiStatus = EFI_DEVICE_ERROR;
  BeforeReadDataCommands = ReadDataCommands;
  CHECK (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                              0x200U, 4096U, Buffer) == EFI_DEVICE_ERROR);
  CHECK (ReadDataCommands == BeforeReadDataCommands + 1U);
  for (Index = 0; Index < 4096U; Index++) {
    CHECK (Buffer[Index] == 0x5AU);
  }

  MockScsiStatus = EFI_SUCCESS;
  // A successful transport with a short byte count must not copy that chunk,
  // advance to another command, or leak its bounce allocation.
  memset (Buffer, 0x5A, 65U * 4096U);
  BeforeReadDataCommands = ReadDataCommands;
  ShortReadCommand = ReadDataCommands + 1U;
  CHECK (BlockIo->ReadBlocks (BlockIo, 1, 0x200U, 65U * 4096U, Buffer) == EFI_DEVICE_ERROR);
  CHECK (ReadDataCommands == BeforeReadDataCommands + 1U);
  for (Index = 0; Index < 65U * 4096U; Index++) CHECK (Buffer[Index] == 0x5A);
  CHECK (PageAllocations == PageFrees);
  ShortReadCommand = 0;

  // If cleanup fails, propagate it after successful I/O but preserve a prior
  // transfer error. The failed mapping remains owned, not silently freed.
  for (UINTN Failure = 0; Failure < 2; Failure++) {
    UINTN BeforeFreeAttempts = FreeAttempts;
    memset (Buffer, 0x5A, 4096U);
    MockScsiStatus = Failure ? EFI_TIMEOUT : EFI_SUCCESS;
    MockFreeStatus = EFI_DEVICE_ERROR;
    CHECK (BlockIo->ReadBlocks (BlockIo, 1, 0x200U, 4096U, Buffer) ==
           (Failure ? EFI_TIMEOUT : EFI_DEVICE_ERROR));
    CHECK (FreeAttempts == BeforeFreeAttempts + 1U);
    CHECK (FailedFreePages == 1U && PageAllocations == PageFrees + 1U);
    for (Index = 0; Index < 4096U; Index++) {
      UINT8 Expected = Failure ? 0x5A :
        (UINT8)((((UINT64)0x200U * 4096U + Index) * 31U + 7U) & 0xFFU);
      CHECK (Buffer[Index] == Expected);
    }
    // Fixture-only recovery; production did not free this failed allocation.
    MockFreeStatus = EFI_SUCCESS;
    CHECK (MockFreePages (FailedFreeMemory, FailedFreePages) == EFI_SUCCESS);
    FailedFreeMemory = 0;
    FailedFreePages = 0;
  }
  MockScsiStatus = EFI_SUCCESS;
  CHECK (PageAllocations == PageFrees);
  free (Buffer);
  Status = mDriverBinding.Stop (&mDriverBinding, MockController, 1U, NULL);
  CHECK (Status == EFI_INVALID_PARAMETER);
  CHECK (PublishedBlockIo == BlockIo);
  MockBlockUninstallStatus = EFI_ACCESS_DENIED;
  BeforeCloseCalls = CloseCalls;
  Status = mDriverBinding.Stop (&mDriverBinding, MockController, 0U, NULL);
  CHECK (Status == EFI_ACCESS_DENIED);
  CHECK (PublishedBlockIo == BlockIo);
  CHECK (CloseCalls == BeforeCloseCalls);
  MockBlockUninstallStatus = EFI_SUCCESS;
  MockCloseStatus = EFI_DEVICE_ERROR;
  Status = mDriverBinding.Stop (&mDriverBinding, MockController, 0U, NULL);
  CHECK (Status == EFI_DEVICE_ERROR && PublishedBlockIo == NULL && mDevice.Started);
  UINTN BeforeRetryUninstalls = BlockUninstallCalls;
  CHECK (mDriverBinding.Start (&mDriverBinding, MockController, NULL) == EFI_UNSUPPORTED);
  MockCloseStatus = EFI_SUCCESS;
  Status = mDriverBinding.Stop (&mDriverBinding, MockController, 0U, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (BlockUninstallCalls == BeforeRetryUninstalls);
  CHECK (PublishedBlockIo == NULL);
  CHECK (!mDevice.Started && (mDevice.Signature == 0U));
  CHECK (CloseCalls == BeforeCloseCalls + 2U);
}

static void TestFailedStartCleanup (void) {
  for (UINTN Failure = 0; Failure < 2; Failure++) {
    MockScsiStatus = Failure ? EFI_SUCCESS : EFI_TIMEOUT;
    MockBlockInstallStatus = Failure ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
    MockCloseStatus = EFI_DEVICE_ERROR;
    CHECK (mDriverBinding.Start (&mDriverBinding, MockController, NULL) ==
           (Failure ? EFI_OUT_OF_RESOURCES : EFI_TIMEOUT));
    CHECK (ScsiClaimed && mDevice.Started && mDevice.BlockIoUninstalled);
    CHECK (PublishedBlockIo == NULL && PageAllocations == PageFrees);
    UINTN OpensBefore = OpenCalls, UninstallsBefore = BlockUninstallCalls;
    CHECK (mDriverBinding.Start (&mDriverBinding, MockController, NULL) == EFI_UNSUPPORTED);
    CHECK (OpenCalls == OpensBefore);
    CHECK (mDriverBinding.Stop (&mDriverBinding, MockController, 0, NULL) == EFI_DEVICE_ERROR);
    CHECK (ScsiClaimed && mDevice.Started && BlockUninstallCalls == UninstallsBefore);
    MockCloseStatus = EFI_SUCCESS;
    CHECK (mDriverBinding.Stop (&mDriverBinding, MockController, 0, NULL) == EFI_SUCCESS);
    CHECK (!ScsiClaimed && !mDevice.Started && mDevice.Signature == 0);
    CHECK (BlockUninstallCalls == UninstallsBefore);
    MockScsiStatus = MockBlockInstallStatus = EFI_SUCCESS;
    CHECK (mDriverBinding.Start (&mDriverBinding, MockController, NULL) == EFI_SUCCESS);
    CHECK (ScsiClaimed && PublishedBlockIo && !mDevice.BlockIoUninstalled);
    CHECK (mDriverBinding.Stop (&mDriverBinding, MockController, 0, NULL) == EFI_SUCCESS);
    CHECK (!ScsiClaimed && !PublishedBlockIo && PageAllocations == PageFrees);
  }
}

static void TestCapacityResponses (void) {
  for (UINTN Use16 = 0; Use16 < 2; Use16++) {
    for (UINTN Case = 0; Case < 12; Case++) {
      MTK_UFS_BLOCK_DEVICE Device = {0};
      Device.ScsiIo = &MockScsiIo;
      MockBlockSize = Case == 6 ? 0 : Case == 7 ? 512 : Case == 8 ? 8192 : 4096;
      UINT64 Last = Case == 9 ? 31240190ULL : Case == 10 ? 31240192ULL :
                    Case == 11 ? (Use16 ? MAX_UINT64 : 0xFFFFFFFEU) : 31240191ULL;
      MockLastBlock = Use16 ? MAX_UINT32 : (UINT32)Last;
      MockLastBlock16 = Last;
      FaultCapacity16 = (BOOLEAN)Use16;
      CapacityFault = Case <= 5 ? Case : 0;
      UINTN Before = ReadCapacityCommands;
      EFI_STATUS Status = MtkUfsReadCapacity (&Device, NULL);
      if (Case == 0) {
        CHECK (Status == EFI_SUCCESS && Device.Media.MediaPresent);
        CHECK (Device.Media.LastBlock == 31240191ULL && Device.Media.BlockSize == 4096);
        CHECK (Device.Media.MediaId == 1 && Device.Media.ReadOnly && !Device.Media.RemovableMedia);
      } else {
        CHECK (Status == (Case == 5 ? EFI_TIMEOUT : Case <= 6 ? EFI_DEVICE_ERROR : EFI_UNSUPPORTED));
        CHECK (!Device.Media.MediaPresent && Device.Media.BlockSize == 0 && Device.Media.LastBlock == 0);
      }
      CHECK (ReadCapacityCommands == Before + (Use16 && Case != 6 ? 2U : 1U));
      CHECK (PageAllocations == PageFrees && PublishedBlockIo == NULL);
    }
  }
  CapacityFault = 0; FaultCapacity16 = FALSE;
  MockLastBlock = 31240191U; MockLastBlock16 = 31240191U; MockBlockSize = 4096U;
}

static void TestTimeoutArithmetic (void) {
  // Exhaustive one-wrap arithmetic over a bounded up/down counter range.
  for (UINT64 Start = 10; Start <= 109; Start++) {
    for (UINT64 Now = 10; Now <= 109; Now++) {
      CHECK (MtkUfsElapsedTicks (Start, Now, 10, 109) == (Now + 100 - Start) % 100);
      CHECK (MtkUfsElapsedTicks (Start, Now, 109, 10) == (Start + 100 - Now) % 100);
    }
  }
  CHECK (MtkUfsElapsedTicks (MAX_UINT64 - 5, 3, 0, MAX_UINT64) == 9);
  CHECK (MtkUfsElapsedTicks (3, MAX_UINT64 - 5, MAX_UINT64, 0) == 9);
  UINT64 Timeout = 123;
  CHECK (MtkUfsGetRemainingTimeout (0, 0, MAX_UINT64, NULL) == EFI_INVALID_PARAMETER);
  TimeNs = 0;
  CHECK (MtkUfsGetRemainingTimeout (0, 0, MAX_UINT64, &Timeout) == EFI_SUCCESS && Timeout == 300000000);
  TimeNs = MTK_UFS_MAX_TOTAL_READ_NS - 100;
  CHECK (MtkUfsGetRemainingTimeout (0, 0, MAX_UINT64, &Timeout) == EFI_SUCCESS && Timeout == 1);
  for (UINT64 Excess = 0; Excess <= 100; Excess++) {
    Timeout = 123;
    TimeNs = MTK_UFS_MAX_TOTAL_READ_NS - 99 + Excess;
    CHECK (MtkUfsGetRemainingTimeout (0, 0, MAX_UINT64, &Timeout) == EFI_TIMEOUT && Timeout == 123);
  }
  TimeNs = MAX_UINT64;
  CHECK (MtkUfsGetRemainingTimeout (0, 0, MAX_UINT64, &Timeout) == EFI_TIMEOUT);
  TimeNs = 0;
}

int
main (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       InitialOpenCalls;
  UINTN       InitialCloseCalls;
  UINTN       InitialInstallCalls;
  UINTN       InitialUninstallCalls;
  UINTN       Index;
  TestChildDevicePaths ();
  SetDevicePath ();
  MockScsiIo.GetDeviceType = MockGetDeviceType;
  MockScsiIo.GetDeviceLocation = MockGetDeviceLocation;
  MockScsiIo.ExecuteScsiCommand = MockExecuteScsiCommand;
  MockScsiIo.IoAlign = 0U;
  Services.HandleProtocol = MockHandleProtocol;
  Services.OpenProtocol = MockOpenProtocol;
  Services.CloseProtocol = MockCloseProtocol;
  TestTimeoutArithmetic ();
  Services.InstallProtocolInterface = MockInstallProtocolInterface;
  Services.UninstallProtocolInterface = MockUninstallProtocolInterface;
  Services.AllocatePages = MockAllocatePages;
  Services.FreePages = MockFreePages;
  TestCapacityResponses ();

  Status = MtkUfsReadOnlyBlockIoEntryPoint (
             (EFI_HANDLE)(UINTN)0x2020U,
             NULL
             );
  CHECK (Status == EFI_SUCCESS);
  CHECK (PublishedDriverBinding == &mDriverBinding);
  CHECK (PublishedDiagnostics == &mDiagnostics);
  CHECK (PublishedDiagnostics->Version == T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_VERSION);
  CHECK (PublishedDiagnostics->Count == 0U);
  CHECK (PublishedDiagnostics->Dropped == 0U);
  {
    VENDOR_DEVICE_PATH *Vendor = (VENDOR_DEVICE_PATH *)MockDevicePath;
    Vendor->Guid.Data1 ^= 1U;
    for (Index = 0; Index < 16U; Index++) {
      EFI_HANDLE Unrelated = (EFI_HANDLE)(UINTN)(0x3000U + Index);
      CHECK (mDriverBinding.Supported (&mDriverBinding, Unrelated, NULL) == EFI_UNSUPPORTED);
    }
    Vendor->Guid.Data1 ^= 1U;
    CHECK (PublishedDiagnostics->Count == 0U);
    CHECK (PublishedDiagnostics->Dropped == 0U);
  }
  {
    UFS_DEVICE_PATH *UfsNode = (UFS_DEVICE_PATH *)NextDevicePathNode (MockDevicePath);
    UfsNode->Lun = 1U;
    CHECK (mDriverBinding.Supported (&mDriverBinding, MockController, NULL) == EFI_UNSUPPORTED);
    CHECK (PublishedDiagnostics->Count == 1U);
    CHECK (PublishedDiagnostics->Records[0].Controller == MockController);
    CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_SUPPORTED_SEEN) != 0U);
    CHECK (PublishedDiagnostics->Records[0].SupportedStage == T70UfsDiagStageLuPath);
    CHECK (PublishedDiagnostics->Records[0].SupportedStatus == EFI_UNSUPPORTED);
    CHECK (PublishedDiagnostics->Records[0].StageStatus == EFI_UNSUPPORTED);
    UfsNode->Lun = 2U;
  }
  CHECK (mDriverBinding.Supported (&mDriverBinding, MockController, NULL) ==
         EFI_SUCCESS);
  CHECK (PublishedDiagnostics->Count == 1U);
  CHECK (PublishedDiagnostics->Records[0].Controller == MockController);
  CHECK (PublishedDiagnostics->Records[0].SupportedStage == T70UfsDiagStageSupported);
  CHECK (PublishedDiagnostics->Records[0].SupportedStatus == EFI_SUCCESS);
  CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_LUN_VALID) != 0U);
  CHECK (PublishedDiagnostics->Records[0].Lun == 2U);

  MockGenericBlockIo = &GenericBlockIo;
  InitialOpenCalls = OpenCalls;
  CHECK (mDriverBinding.Supported (&mDriverBinding, MockController, NULL) ==
         EFI_ACCESS_DENIED);
  CHECK (mDriverBinding.Start (&mDriverBinding, MockController, NULL) ==
         EFI_ACCESS_DENIED);
  CHECK (OpenCalls == InitialOpenCalls);
  MockGenericBlockIo = NULL;
  InitialOpenCalls = OpenCalls;
  InitialCloseCalls = CloseCalls;
  MockScsiStatus = EFI_DEVICE_ERROR;
  Status = mDriverBinding.Start (&mDriverBinding, MockController, NULL);
  CHECK (Status == EFI_DEVICE_ERROR);
  CHECK (OpenCalls == InitialOpenCalls + 1U);
  CHECK (CloseCalls == InitialCloseCalls + 1U);
  CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_START_SEEN) != 0U);
  CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_OPEN_ATTEMPTED) != 0U);
  CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_CAPACITY10_ATTEMPTED) != 0U);
  CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_INSTALL_ATTEMPTED) == 0U);
  CHECK (PublishedDiagnostics->Records[0].StartStage == T70UfsDiagStageCapacity10);
  CHECK (PublishedDiagnostics->Records[0].OpenStatus == EFI_SUCCESS);
  CHECK (PublishedDiagnostics->Records[0].Capacity10Status == EFI_DEVICE_ERROR);
  CHECK (PublishedDiagnostics->Records[0].StartStatus == EFI_DEVICE_ERROR);
  CHECK (mDriverBinding.Supported (&mDriverBinding, MockController, NULL) == EFI_SUCCESS);
  CHECK (PublishedDiagnostics->Records[0].StartStage == T70UfsDiagStageCapacity10);
  CHECK (PublishedDiagnostics->Records[0].StageStatus == EFI_DEVICE_ERROR);
  MockScsiStatus = EFI_SUCCESS;
  MockOpenStatus = EFI_DEVICE_ERROR;
  Status = mDriverBinding.Start (&mDriverBinding, MockController, NULL);
  CHECK (Status == EFI_DEVICE_ERROR);
  CHECK (PublishedDiagnostics->Records[0].StartStage == T70UfsDiagStageOpenScsi);
  CHECK (PublishedDiagnostics->Records[0].OpenStatus == EFI_DEVICE_ERROR);
  CHECK (PublishedDiagnostics->Records[0].Capacity10Status == EFI_NOT_READY);
  CHECK (PublishedDiagnostics->Records[0].Capacity16Status == EFI_NOT_READY);
  CHECK (PublishedDiagnostics->Records[0].InstallStatus == EFI_NOT_READY);
  CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_CAPACITY10_ATTEMPTED) == 0U);
  CHECK ((PublishedDiagnostics->Records[0].Flags & T70_UFS_DIAG_INSTALL_ATTEMPTED) == 0U);
  CHECK (PublishedDiagnostics->Records[0].BlockSize == 0U);
  CHECK (PublishedDiagnostics->Records[0].LastBlock == 0U);
  CHECK (PublishedDiagnostics->Records[0].Lun == 2U);
  MockOpenStatus = EFI_SUCCESS;
  InitialOpenCalls = OpenCalls;
  InitialCloseCalls = CloseCalls;
  MockBlockSize = 512U;
  Status = mDriverBinding.Start (&mDriverBinding, MockController, NULL);
  CHECK (Status == EFI_UNSUPPORTED);
  CHECK (OpenCalls == InitialOpenCalls + 1U);
  CHECK (CloseCalls == InitialCloseCalls + 1U);
  CHECK (!mDevice.Started && (mDevice.Signature == 0U));
  CHECK (PublishedBlockIo == NULL);
  CHECK (PageAllocations == PageFrees);
  CHECK (PublishedDiagnostics->Records[0].StartStage == T70UfsDiagStageGeometry);
  CHECK (PublishedDiagnostics->Records[0].BlockSize == 512U);
  InitialOpenCalls = OpenCalls;
  InitialCloseCalls = CloseCalls;
  InitialInstallCalls = BlockInstallCalls;
  MockBlockSize = 4096U;
  MockBlockInstallStatus = EFI_OUT_OF_RESOURCES;
  CHECK (mDriverBinding.Start (&mDriverBinding, MockController, NULL) ==
         EFI_OUT_OF_RESOURCES);
  CHECK (OpenCalls == InitialOpenCalls + 1U);
  CHECK (CloseCalls == InitialCloseCalls + 1U);
  CHECK (BlockInstallCalls == InitialInstallCalls + 1U);
  CHECK (PublishedBlockIo == NULL);
  CHECK (!mDevice.Started && (mDevice.Signature == 0U));
  CHECK (PageAllocations == PageFrees);
  CHECK (PublishedDiagnostics->Records[0].StartStage == T70UfsDiagStageInstallBlockIo);
  CHECK (PublishedDiagnostics->Records[0].InstallStatus == EFI_OUT_OF_RESOURCES);
  MockBlockInstallStatus = EFI_SUCCESS;
  InitialOpenCalls = OpenCalls;
  InitialCloseCalls = CloseCalls;
  InitialUninstallCalls = BlockUninstallCalls;
  TestSuccessfulStartReadWriteAndStop ();
  CHECK (PublishedDiagnostics->Records[0].StartStage == T70UfsDiagStageReady);
  CHECK (PublishedDiagnostics->Records[0].StartStatus == EFI_SUCCESS);
  CHECK (PublishedDiagnostics->Records[0].InstallStatus == EFI_SUCCESS);
  CHECK (PublishedDiagnostics->Records[0].BlockSize == 4096U);
  CHECK (PublishedDiagnostics->Records[0].LastBlock == 31240191U);
  CHECK (OpenCalls == InitialOpenCalls + 1U);
  CHECK (CloseCalls == InitialCloseCalls + 2U);
  CHECK (BlockUninstallCalls == InitialUninstallCalls + 2U);
  CHECK (PageAllocations == PageFrees);
  CHECK (ScsiCommands == ReadCapacityCommands + ReadDataCommands);

  // Complete stop resets publication state so a subsequent bind can publish
  // and remove a fresh interface normally.
  CHECK (mDriverBinding.Start (&mDriverBinding, MockController, NULL) == EFI_SUCCESS);
  CHECK (PublishedBlockIo != NULL && !mDevice.BlockIoUninstalled);
  CHECK (mDriverBinding.Stop (&mDriverBinding, MockController, 0, NULL) == EFI_SUCCESS);
  CHECK (PublishedBlockIo == NULL && !mDevice.Started && PageAllocations == PageFrees);

  TestFailedStartCleanup ();

  PublishedDriverBinding = NULL;
  MockDriverBindingInstallStatus = EFI_DEVICE_ERROR;
  MockDiagnosticsUninstallStatus = EFI_SUCCESS;
  Status = MtkUfsReadOnlyBlockIoEntryPoint ((EFI_HANDLE)(UINTN)0x3030U, NULL);
  CHECK (Status == EFI_DEVICE_ERROR);
  CHECK (PublishedDriverBinding == NULL && PublishedDiagnostics == NULL);
  CHECK (mDiagnostics.DriverBindingInstallStatus == EFI_DEVICE_ERROR);
  CHECK (mDiagnostics.DiagnosticsUninstallStatus == EFI_SUCCESS);

  MockDiagnosticsUninstallStatus = EFI_ACCESS_DENIED;
  Status = MtkUfsReadOnlyBlockIoEntryPoint ((EFI_HANDLE)(UINTN)0x4040U, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (PublishedDriverBinding == NULL && PublishedDiagnostics == &mDiagnostics);
  CHECK (mDiagnostics.DriverBindingInstallStatus == EFI_DEVICE_ERROR);
  CHECK (mDiagnostics.DiagnosticsUninstallStatus == EFI_ACCESS_DENIED);
  CHECK (DiagnosticsUninstallCalls == 2U);

  printf ("UFS BlockIo DXE: %u checks passed (actual UFS child path, LU2 rejection matrix, read-only SCSI and Boot Services mocks)\n", Checks);
  return 0;
}
