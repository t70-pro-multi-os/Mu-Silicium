// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../MediaTekTest.h"
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Pi/PiDxeCis.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/MtkDisplayCore.h>
#include "LkDisplayFixture.h"
#include "../../../../../Silicon/MediaTek/MediaTekPkg/Drivers/FramebufferGopDxe/FramebufferGopDxe.c"

#define TEST_FDT_CAPACITY 16384
#define TEST_FB_WIDTH 64
#define TEST_FB_HEIGHT 32
#define TEST_FB_SIZE (TEST_FB_WIDTH * TEST_FB_HEIGHT * 4)

EFI_GUID gFdtTableGuid = {1};
EFI_GUID gEfiEventExitBootServicesGuid = {2};
EFI_GUID gEfiGraphicsOutputProtocolGuid = {3};
EFI_GUID gEfiDevicePathProtocolGuid = {4};
EFI_GUID gEfiEndOfDxeEventGroupGuid = {5};
EFI_GUID gEfiEventReadyToBootGuid = {6};

static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS = &DxeServices;
static UINT8 FdtBuffer[TEST_FDT_CAPACITY];
static UINT8 FrameBuffer[TEST_FB_SIZE] __attribute__((aligned(4096)));
static EFI_GRAPHICS_OUTPUT_PROTOCOL *InstalledGop;
static EFI_DEVICE_PATH_PROTOCOL *InstalledDevicePath;
static EFI_HANDLE InstalledHandle;
static EFI_EVENT_NOTIFY EventNotify;
static VOID *EventContext;
static volatile VOID *SuccessContext;
static VOID *RetainedFailureContext;
static VOID *Freed[16];
static UINTN FreeCount;
static UINTN AllocationCount;
static UINTN InstallProtocolCount;
static EFI_STATUS InstallStatus;
static EFI_STATUS CloseStatus;
static EFI_STATUS CreateEventStatus;
static EFI_STATUS MemoryMapStatus;
static EFI_STATUS GcdStatus;
static EFI_STATUS SetAttributesStatus;
static UINT64 GcdAttributes;
static UINTN SetAttributesCount;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *QueriedInfo;

static EFI_STATUS RuntimeStatus;
static UINTN RuntimeHandoffBad, DriverEventCalls, DriverEventFailMask;
static BOOLEAN ReenterStart, DrawDuringInstall;
static UINT64 SeedEarlyMapStatus,SeedEarlyMapMeta;
static UINT64 SeedOverlap[6];
static EFI_EVENT_NOTIFY DriverNotifies[4];
static CONST EFI_GUID *DriverGroups[4];
UINT64 EFIAPI MmioRead64(UINTN Address) {
  if(Address>=0x43F7F080 && Address<=0x43F7F0A8 && (Address&7)==0)
    return SeedOverlap[(Address-0x43F7F080)/8];
  switch (Address) {
    case 0x43f7f060:return SeedEarlyMapStatus;
    case 0x43f7f068:return SeedEarlyMapMeta;
    case 0x43f7f000:return RuntimeHandoffBad==1?0:0x0000000148303754ULL;
    case 0x43f7f048:return (UINT64)(UINTN)FdtBuffer+(RuntimeHandoffBad==2?8:0);
    case 0x43f7f050:return RuntimeHandoffBad==4?0x200001:
      RuntimeHandoffBad==5?4:FdtTotalSize(FdtBuffer);
    case 0x43f7f058:return RuntimeHandoffBad==3?EFI_DEVICE_ERROR:EFI_SUCCESS;
    default:CHECK(FALSE); return 0;
  }
}
static LK_DISPLAY_FIXTURE *LkIo;
UINT32 EFIAPI MmioRead32(UINTN Address) {
  UINT32 Value=0; CHECK(LkIo!=NULL); CHECK(LkFixtureRead(LkIo,Address,&Value)==EFI_SUCCESS); return Value;
}
static UINT8 OsHandoff[48];
static UINT8 GopProgress[32];
static UINT8 LkSnapshot[72];
static UINTN GopTraceFlushes;
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
static UINTN OsCacheCleans, OsPublications;
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value) {
  if (Address >= 0x43F7F800 && Address+sizeof(Value) <= 0x43F7F848) {
    memcpy(LkSnapshot+(Address-0x43F7F800),&Value,sizeof(Value)); return Value;
  }
  if (Address >= 0x43F7F060 && Address+sizeof(Value) <= 0x43F7F080) {
    memcpy(GopProgress+(Address-0x43F7F060),&Value,sizeof(Value)); return Value;
  }
  CHECK(Address >= 0x43F7F0C0 && Address <= 0x43F7F0EC && !(Address & 3));
  if (Address == 0x43F7F0C0 && Value == 1) {
    UINT32 Revision; memcpy(&Revision,OsHandoff+4,4);
    CHECK(Revision == 1); OsPublications++;
  }
  memcpy(OsHandoff+(Address-0x43F7F0C0),&Value,4); return Value;
}
UINT64 EFIAPI MmioWrite64(UINTN Address, UINT64 Value) {
  if (Address >= 0x43F7F800 && Address+sizeof(Value) <= 0x43F7F848) {
    memcpy(LkSnapshot+(Address-0x43F7F800),&Value,sizeof(Value)); return Value;
  }
  if (Address >= 0x43F7F060 && Address+sizeof(Value) <= 0x43F7F080) {
    memcpy(GopProgress+(Address-0x43F7F060),&Value,sizeof(Value)); return Value;
  }
  CHECK(Address >= 0x43F7F0C0 && Address <= 0x43F7F0E8 && !(Address & 7));
  memcpy(OsHandoff+(Address-0x43F7F0C0),&Value,8); return Value;
}
VOID *EFIAPI WriteBackDataCacheRange(VOID *Address, UINTN Length) {
  if ((UINTN)Address == 0x43F7F060) { CHECK(Length==32); GopTraceFlushes++; return Address; }
  if ((UINTN)Address == 0x43F7F800) { CHECK(Length==8 || Length==72); return Address; }
  CHECK((UINTN)Address == 0x43F7F0C0 && Length == 48); OsCacheCleans++; return Address;
}

static UINT32 ToBe32(UINT32 Value) {
  return ((Value & 0xFF) << 24) | ((Value & 0xFF00) << 8) |
         ((Value & 0xFF0000) >> 8) | ((Value & 0xFF000000) >> 24);
}

static void SetCell(VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 Value) {
  UINT32 Wire = ToBe32(Value);
  CHECK(FdtSetProp(Fdt, Node, Name, &Wire, sizeof(Wire)) == 0);
}

static void SetPair(VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT64 Base, UINT64 Size) {
  UINT32 Wire[4] = { ToBe32((UINT32)(Base >> 32)), ToBe32((UINT32)Base),
                     ToBe32((UINT32)(Size >> 32)), ToBe32((UINT32)Size) };
  CHECK(FdtSetProp(Fdt, Node, Name, Wire, sizeof(Wire)) == 0);
}

static VOID *MakeFdt(VOID) {
  INT32 Chosen, Fb, Reserved, ReservedFb;
  UINT64 Base = (UINT64)(UINTN)FrameBuffer;
  CHECK(FdtCreateEmptyTree(FdtBuffer, sizeof(FdtBuffer)) == 0);
  SetCell(FdtBuffer, 0, "#address-cells", 2);
  SetCell(FdtBuffer, 0, "#size-cells", 2);
  Chosen = FdtAddSubnode(FdtBuffer, 0, "chosen"); CHECK(Chosen >= 0);
  SetCell(FdtBuffer, Chosen, "#address-cells", 2);
  SetCell(FdtBuffer, Chosen, "#size-cells", 2);
  Fb = FdtAddSubnode(FdtBuffer, Chosen, "framebuffer@0"); CHECK(Fb >= 0);
  CHECK(FdtSetPropString(FdtBuffer, Fb, "compatible", "simple-framebuffer") == 0);
  CHECK(FdtSetPropString(FdtBuffer, Fb, "format", "x8r8g8b8") == 0);
  SetCell(FdtBuffer, Fb, "width", TEST_FB_WIDTH);
  SetCell(FdtBuffer, Fb, "height", TEST_FB_HEIGHT);
  SetCell(FdtBuffer, Fb, "stride", TEST_FB_WIDTH * 4);
  SetPair(FdtBuffer, Fb, "reg", Base, TEST_FB_SIZE);
  SetCell(FdtBuffer, Fb, "memory-region", 7);
  Reserved = FdtAddSubnode(FdtBuffer, 0, "reserved-memory"); CHECK(Reserved >= 0);
  SetCell(FdtBuffer, Reserved, "#address-cells", 2);
  SetCell(FdtBuffer, Reserved, "#size-cells", 2);
  CHECK(FdtSetPropEmpty(FdtBuffer, Reserved, "ranges") == 0);
  ReservedFb = FdtAddSubnode(FdtBuffer, Reserved, "fb@0"); CHECK(ReservedFb >= 0);
  SetPair(FdtBuffer, ReservedFb, "reg", Base, TEST_FB_SIZE);
  SetCell(FdtBuffer, ReservedFb, "phandle", 7);
  CHECK(FdtSetPropEmpty(FdtBuffer, ReservedFb, "no-map") == 0);
  CHECK(FdtPack(FdtBuffer) == 0);
  return FdtBuffer;
}

static EFI_STATUS EFIAPI TestAllocatePool(EFI_MEMORY_TYPE Type, UINTN Size, VOID **Buffer) {
  CHECK(Type == EfiBootServicesData && Buffer != NULL && Size != 0);
  if (ReenterStart) GopRetry(NULL,NULL);
  *Buffer = malloc(Size);
  if (*Buffer == NULL) return EFI_OUT_OF_RESOURCES;
  AllocationCount++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestFreePool(VOID *Buffer) {
  CHECK(Buffer != NULL);
  if (FreeCount < sizeof(Freed) / sizeof(Freed[0])) Freed[FreeCount++] = Buffer;
  free(Buffer);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestGetMemoryMap(UINTN *Size, EFI_MEMORY_DESCRIPTOR *Map,
  UINTN *Key, UINTN *DescriptorSize, UINT32 *Version) {
  EFI_PHYSICAL_ADDRESS Base = (EFI_PHYSICAL_ADDRESS)(UINTN)FrameBuffer;
  if (EFI_ERROR(MemoryMapStatus)) return MemoryMapStatus;
  if (*Size < sizeof(*Map)) { *Size = sizeof(*Map); return EFI_BUFFER_TOO_SMALL; }
  memset(Map, 0, sizeof(*Map));
  Map->Type = EfiReservedMemoryType;
  Map->PhysicalStart = Base;
  Map->NumberOfPages = TEST_FB_SIZE / EFI_PAGE_SIZE;
  *Size = sizeof(*Map); *Key = 1; *DescriptorSize = sizeof(*Map);
  *Version = EFI_MEMORY_DESCRIPTOR_VERSION;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestCreateEventEx(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify,
  CONST VOID *Context, CONST EFI_GUID *Group, EFI_EVENT *Event) {
  CHECK(Type == EVT_NOTIFY_SIGNAL && Notify != NULL);
  if (Context == NULL) {
    UINTN Slot=DriverEventCalls++;
    CHECK(Slot<4);
    CHECK(Tpl==(Slot==0?TPL_NOTIFY:TPL_CALLBACK));
    if(DriverEventFailMask & (1U<<Slot)) return EFI_OUT_OF_RESOURCES;
    DriverNotifies[Slot]=Notify; DriverGroups[Slot]=Group;
    *Event=(EFI_EVENT)(UINTN)(0x2100+Slot);
    return EFI_SUCCESS;
  }
  CHECK(Tpl == TPL_NOTIFY && Group == &gEfiEventExitBootServicesGuid);
  if (EFI_ERROR(CreateEventStatus)) return CreateEventStatus;
  EventNotify = Notify; EventContext = (VOID *)Context; *Event = (EFI_EVENT)(UINTN)0x1234;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestCloseEvent(EFI_EVENT Event) {
  CHECK(Event == (EFI_EVENT)(UINTN)0x1234);
  return CloseStatus;
}

static EFI_STATUS EFIAPI TestInstallMultiple(EFI_HANDLE *Handle, ...) {
  va_list Args;
  EFI_GUID *Guid;
  VOID *Interface;
  UINTN Seen = 0;
  CHECK(Handle != NULL);
  if (*Handle == NULL) *Handle = (EFI_HANDLE)(UINTN)0x5678;
  va_start(Args, Handle);
  while ((Guid = va_arg(Args, EFI_GUID *)) != NULL) {
    Interface = va_arg(Args, VOID *);
    CHECK(Interface != NULL);
    if (Guid == &gEfiGraphicsOutputProtocolGuid) InstalledGop = Interface;
    else if (Guid == &gEfiDevicePathProtocolGuid) InstalledDevicePath = Interface;
    else CHECK(FALSE);
    Seen++;
  }
  va_end(Args);
  InstallProtocolCount = Seen;
  InstalledHandle = *Handle;
  if (DrawDuringInstall && InstalledGop != NULL) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel = {1,2,3,0};
    UINT32 Phase; UINT64 Detail;
    CHECK(InstalledGop->SetMode(InstalledGop,0)==EFI_SUCCESS);
    CHECK(InstalledGop->Blt(InstalledGop,&Pixel,EfiBltBufferToVideo,0,0,0,0,1,1,0)==EFI_SUCCESS);
    memcpy(&Phase,GopProgress+8,4);memcpy(&Detail,GopProgress+24,8);
    CHECK(Phase==7 && (Detail>>63)==0 && mPublishedGop==NULL);
  }
  if (EFI_ERROR(InstallStatus)) { InstalledGop = NULL; InstalledDevicePath = NULL; }
  return InstallStatus;
}

static UINT64 GcdCapabilities;
static EFI_STATUS EFIAPI TestGetGcdDescriptor(EFI_PHYSICAL_ADDRESS Base,
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor) {
  if (EFI_ERROR(GcdStatus)) return GcdStatus;
  CHECK(Base == (EFI_PHYSICAL_ADDRESS)(UINTN)FrameBuffer);
  memset(Descriptor, 0, sizeof(*Descriptor));
  Descriptor->BaseAddress = Base;
  Descriptor->Length = TEST_FB_SIZE;
  Descriptor->GcdMemoryType = EfiGcdMemoryTypeSystemMemory;
  Descriptor->Capabilities = GcdCapabilities;
  Descriptor->Attributes = GcdAttributes;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestSetGcdAttributes(EFI_PHYSICAL_ADDRESS Base, UINT64 Length, UINT64 Attributes) {
  if (EFI_ERROR(SetAttributesStatus)) return SetAttributesStatus;
  CHECK(Base == (EFI_PHYSICAL_ADDRESS)(UINTN)FrameBuffer && Length == TEST_FB_SIZE);
  GcdAttributes = Attributes; SetAttributesCount++;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *Guid, VOID **Table) {
  CHECK(Guid == &gFdtTableGuid && Table != NULL);
  if (EFI_ERROR(RuntimeStatus)) return RuntimeStatus;
  *Table = LkIo != NULL ? FdtBuffer : MakeFdt();
  return EFI_SUCCESS;
}

static void ResetServices(void) {
  memset(&Services, 0, sizeof(Services));
  gBS = &Services;
  memset(&DxeServices, 0, sizeof(DxeServices));
  Services.AllocatePool = TestAllocatePool;
  Services.FreePool = TestFreePool;
  Services.GetMemoryMap = TestGetMemoryMap;
  Services.CreateEventEx = TestCreateEventEx;
  Services.CloseEvent = TestCloseEvent;
  Services.InstallMultipleProtocolInterfaces = TestInstallMultiple;
  DxeServices.GetMemorySpaceDescriptor = TestGetGcdDescriptor;
  DxeServices.SetMemorySpaceAttributes = TestSetGcdAttributes;
  InstalledGop = NULL; InstalledDevicePath = NULL; InstalledHandle = NULL;
  mPublishedGop=NULL; mDriverExitEvent=NULL;
  memset(mRetryEvents,0,sizeof(mRetryEvents));
  mDriverRegistered=mStartInProgress=mExitingBootServices=FALSE;
  mLastStartStatus=EFI_NOT_READY;
  mTracePhase=mTraceAttempts=0;mTraceDetail=0;
  mEarlyMapStatus=mEarlyMapMeta=SeedEarlyMapStatus=SeedEarlyMapMeta=0;
  memset(SeedOverlap,0,sizeof(SeedOverlap));
  memset(GopProgress,0,sizeof(GopProgress));GopTraceFlushes=0;
  memset(LkSnapshot,0xa5,sizeof(LkSnapshot));
  RuntimeHandoffBad=DriverEventCalls=DriverEventFailMask=0; ReenterStart=DrawDuringInstall=FALSE;
  memset(DriverNotifies,0,sizeof(DriverNotifies)); memset(DriverGroups,0,sizeof(DriverGroups));
  EventNotify = NULL; EventContext = NULL; InstallProtocolCount = 0;
  InstallStatus = EFI_SUCCESS; CloseStatus = EFI_SUCCESS;
  CreateEventStatus = EFI_SUCCESS; MemoryMapStatus = EFI_SUCCESS;
  GcdStatus = EFI_SUCCESS; SetAttributesStatus = EFI_SUCCESS;
  GcdAttributes = EFI_MEMORY_WB | EFI_MEMORY_XP; SetAttributesCount = 0;
  GcdCapabilities = EFI_MEMORY_UC | EFI_MEMORY_XP;
  QueriedInfo = NULL; RuntimeStatus = EFI_SUCCESS;
  memset(OsHandoff,0xa5,sizeof(OsHandoff)); OsCacheCleans = OsPublications = 0;
  FreeCount = 0; AllocationCount = 0; memset(Freed, 0, sizeof(Freed));
}

static BOOLEAN WasFreed(VOID *Pointer) {
  UINTN I;
  for (I = 0; I < FreeCount && I < sizeof(Freed) / sizeof(Freed[0]); ++I)
    if (Freed[I] == Pointer) return TRUE;
  return FALSE;
}

static void TestLkRuntimeAcquisition(void) {
  ResetServices(); LkIo=malloc(sizeof(*LkIo)); CHECK(LkIo);
  LkFixtureTree(FdtBuffer,sizeof(FdtBuffer)); LkFixtureInitialize(LkIo,0x14005000,0,FALSE,TRUE);
  MTK_DISPLAY_FRAMEBUFFER Fb;
  CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_SUCCESS && Fb.FrameBufferBase==0x90000000 && Fb.ProtectedSize==0x3000);
  CHECK(LkIo->Writes==0);
  UINT64 SnapshotMagic;UINT32 SnapshotWords[16];
  memcpy(&SnapshotMagic,LkSnapshot,8);memcpy(SnapshotWords,LkSnapshot+8,64);
  CHECK(SnapshotMagic==LK_SNAPSHOT_MAGIC && SnapshotWords[0]==0x14005000 && SnapshotWords[1]==0);
  CHECK(SnapshotWords[2+6]==0x21FF && SnapshotWords[2+10]==0x10000104);
  LkIo->Spm[0x16C/4]=0;
  CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_NOT_READY);
  CHECK(mTraceDetail==0x1000616C00000000ULL && LkIo->Writes==0);
  memcpy(&SnapshotMagic,LkSnapshot,8);CHECK(SnapshotMagic==0);
  LkIo->Spm[0x16C/4]=0x200000;
  *LkFixtureRegister(LkIo,0x14005044)=260;
  CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_UNSUPPORTED && LkIo->Writes==0);
  memcpy(&SnapshotMagic,LkSnapshot,8);memcpy(SnapshotWords,LkSnapshot+8,64);
  CHECK(SnapshotMagic==LK_DSI_SNAPSHOT_MAGIC && SnapshotWords[0]==0x14013000 && SnapshotWords[1]==0);
  CHECK(SnapshotWords[2+2]==0x300C0 && SnapshotWords[2+6]==32 && SnapshotWords[2+7]==0x00200040);
  CHECK(SnapshotWords[14]==0 && SnapshotWords[15]==0);
  LkFixtureInitialize(LkIo,0x14005000,0,FALSE,TRUE);
  *LkFixtureRegister(LkIo,0x1401301C)=0x200C0;
  CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_UNSUPPORTED && LkIo->Writes==0);
  memcpy(&SnapshotMagic,LkSnapshot,8);memcpy(SnapshotWords,LkSnapshot+8,64);
  CHECK(SnapshotMagic==LK_DSI_SNAPSHOT_MAGIC && SnapshotWords[2+2]==0x200C0);
  LkFixtureInitialize(LkIo,0x14005000,1,FALSE,TRUE);
  *LkFixtureRegister(LkIo,0x14005050)=0x31FF;
  CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_UNSUPPORTED);
  memcpy(SnapshotWords,LkSnapshot+8,64);
  CHECK(SnapshotWords[1]==1 && SnapshotWords[2+3]==2 && SnapshotWords[2+6]==0x31FF);
  CHECK(SnapshotWords[2+13]==0x90000000);
  free(LkIo); LkIo=NULL;
}
static void TestPhysicalRuntimeAcquisition(void) {
  ResetServices();LkIo=malloc(sizeof(*LkIo));CHECK(LkIo);
  LkFixturePhysical18Tree(FdtBuffer,sizeof(FdtBuffer));LkFixturePhysical18(LkIo);
  MTK_DISPLAY_FRAMEBUFFER Fb;
  CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_UNSUPPORTED); // SEC has not made pixels opaque yet.
  CHECK(MtkDisplayAcquireLkHandoff(FdtBuffer,sizeof(FdtBuffer),LkFixtureRead,LkFixtureWrite,LkIo,&Fb)==EFI_SUCCESS);
  CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_SUCCESS);
  CHECK(Fb.Width==2240 && Fb.Height==1400 && Fb.FrameBufferBase==0xFE648000);
  UINT32 Words[16];memcpy(Words,LkSnapshot+8,64);
  CHECK(Words[1]==1 && Words[2+9]==0x100000 && Words[2+10]==0x10002300 && Words[2+12]==0x5A0001);
  free(LkIo);LkIo=NULL;
}
static void TestFramebufferWriteCombining(void) {
  for(UINTN SupportsWc=0;SupportsWc<2U;++SupportsWc) {
    ResetServices();if(SupportsWc)GcdCapabilities|=EFI_MEMORY_WC;
    CHECK(TryStartGop()==EFI_SUCCESS);
    UINT64 Expected=(SupportsWc?EFI_MEMORY_WC:EFI_MEMORY_UC)|EFI_MEMORY_XP;
    printf("Framebuffer memory policy: WC_supported=%lu actual=%llx expected=%llx\n",
      (unsigned long)SupportsWc,(unsigned long long)GcdAttributes,(unsigned long long)Expected);fflush(stdout);
    CHECK(GcdAttributes==Expected);
    EventNotify(NULL,EventContext);CHECK(OsHandoff[0]==1);free(EventContext);
  }
  ResetServices();GcdCapabilities=EFI_MEMORY_WB|EFI_MEMORY_XP;
  CHECK(TryStartGop()==EFI_UNSUPPORTED && SetAttributesCount==0);
  ResetServices();GcdCapabilities|=EFI_MEMORY_WC;SetAttributesStatus=EFI_DEVICE_ERROR;
  CHECK(TryStartGop()==EFI_DEVICE_ERROR && InstalledGop==NULL);
  ResetServices();GcdCapabilities|=EFI_MEMORY_WC;
  CHECK(TryStartGop()==EFI_SUCCESS);GcdAttributes=EFI_MEMORY_UC|EFI_MEMORY_XP;
  EventNotify(NULL,EventContext);CHECK(OsHandoff[0]==0);free(EventContext);
}

static void TestInstalledGopAndDevicePath(void) {
  EFI_STATUS Status;
  UINTN SizeOfInfo;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel = { 1, 2, 3, 4 };
  MTK_GOP_DEVICE_PATH *Path;
  ResetServices();
  Status = TryStartGop();
  CHECK(Status == EFI_SUCCESS && InstalledGop != NULL && InstalledDevicePath != NULL);
  CHECK(InstalledHandle == (EFI_HANDLE)(UINTN)0x5678 && InstallProtocolCount == 2);
  Path = (MTK_GOP_DEVICE_PATH *)InstalledDevicePath;
  CHECK(Path->Vendor.Header.Type == HARDWARE_DEVICE_PATH && Path->Vendor.Header.SubType == HW_VENDOR_DP);
  CHECK(Path->End.Type == END_DEVICE_PATH_TYPE && Path->End.SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE);
  CHECK(Path->Vendor.Guid.Data1 == mMtkGopDevicePathGuid.Data1);
  CHECK(InstalledGop->Mode->MaxMode == 1 && InstalledGop->Mode->Mode == 0);
  CHECK(InstalledGop->QueryMode(InstalledGop, 0, &SizeOfInfo, &Info) == EFI_SUCCESS);
  CHECK(SizeOfInfo == sizeof(*Info) && Info->HorizontalResolution == TEST_FB_WIDTH && Info->VerticalResolution == TEST_FB_HEIGHT);
  CHECK(Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor && Info->PixelsPerScanLine == TEST_FB_WIDTH);
  CHECK(Services.FreePool(Info) == EFI_SUCCESS);
  CHECK(InstalledGop->QueryMode(InstalledGop, 1, &SizeOfInfo, &Info) == EFI_INVALID_PARAMETER);
  CHECK(InstalledGop->SetMode(InstalledGop, 0) == EFI_SUCCESS);
  CHECK(((EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)FrameBuffer)[0].Blue == 0);
  CHECK(InstalledGop->Blt(InstalledGop, &Pixel, EfiBltVideoFill, 0, 0, 4, 3, 2, 1, 0) == EFI_SUCCESS);
  CHECK(((EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)FrameBuffer)[3 * TEST_FB_WIDTH + 4].Red == 3);
  SuccessContext = EventContext;
  EventNotify((EFI_EVENT)(UINTN)0x1234, EventContext);
  CHECK(OsHandoff[0] == 1 && OsHandoff[4] == 1 && OsPublications == 1 && OsCacheCleans == 1);
  { UINT64 Base; UINT32 Width; memcpy(&Base,OsHandoff+8,8); memcpy(&Width,OsHandoff+24,4);
    CHECK(Base == (UINT64)(UINTN)FrameBuffer && Width == TEST_FB_WIDTH); }
  EventNotify((EFI_EVENT)(UINTN)0x1234, EventContext);
  CHECK(OsHandoff[0] == 0 && OsPublications == 1 && OsCacheCleans == 2);
  CHECK(InstalledGop->QueryMode(InstalledGop, 0, &SizeOfInfo, &Info) == EFI_DEVICE_ERROR);
}

static void TestInstallFailureCloseOutcomes(void) {
  VOID *Context;
  ResetServices();
  InstallStatus = EFI_OUT_OF_RESOURCES;
  CloseStatus = EFI_SUCCESS;
  CHECK(TryStartGop() == EFI_OUT_OF_RESOURCES);
  Context = EventContext;
  CHECK(Context != NULL && WasFreed(Context));
  CHECK(GcdAttributes == (EFI_MEMORY_WB | EFI_MEMORY_XP) && SetAttributesCount == 2);

  ResetServices();
  InstallStatus = EFI_OUT_OF_RESOURCES;
  CloseStatus = EFI_DEVICE_ERROR;
  CHECK(TryStartGop() == EFI_SUCCESS);
  CHECK(InstalledGop == NULL && InstalledDevicePath == NULL);
  RetainedFailureContext = EventContext;
  CHECK(RetainedFailureContext != NULL && !WasFreed(RetainedFailureContext));
  CHECK(GcdAttributes == (EFI_MEMORY_WB | EFI_MEMORY_XP) && SetAttributesCount == 2);
  EventNotify((EFI_EVENT)(UINTN)0x1234, RetainedFailureContext);
  CHECK(((MTK_GOP_CONTEXT *)RetainedFailureContext)->BootServicesActive == FALSE);
}

static void TestEarlierFailureCleanup(void) {
  ResetServices();
  CreateEventStatus = EFI_OUT_OF_RESOURCES;
  CHECK(TryStartGop() == EFI_OUT_OF_RESOURCES);
  CHECK(EventContext == NULL);
  CHECK(SetAttributesCount == 2 && GcdAttributes == (EFI_MEMORY_WB | EFI_MEMORY_XP));
  CHECK(AllocationCount >= FreeCount);
}

static void TestOsHandoffFailures(void) {
  for (UINTN Bad=0;Bad<10;Bad++) {
    ResetServices(); CHECK(TryStartGop() == EFI_SUCCESS);
    MTK_GOP_CONTEXT *Context = EventContext;
    switch (Bad) {
      case 0: Context->Info.HorizontalResolution++; break;
      case 1: Context->Mode.FrameBufferBase++; break;
      case 2: GcdAttributes = EFI_MEMORY_WB | EFI_MEMORY_XP; break;
      case 3: GcdStatus = EFI_DEVICE_ERROR; break;
      case 4: Context->Handle = NULL; break;
      case 5: Context->AttributesChanged = FALSE; break;
      case 6: Context->BootServicesActive = FALSE; break;
      case 7: Context->Mode.MaxMode = 2; break;
      case 8: RuntimeStatus = EFI_NOT_FOUND; break;
      case 9: GcdAttributes |= EFI_MEMORY_WP; break;
    }
    EventNotify((EFI_EVENT)(UINTN)0x1234,Context);
    CHECK(OsHandoff[0] == 0 && OsCacheCleans == 1 && OsPublications == 0);
    free(Context);
  }
}


static void TestDeferredDriverStartup(void) {
  ResetServices(); RuntimeStatus=EFI_NOT_FOUND;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS);
  CHECK(mDriverRegistered && DriverEventCalls==4 && InstalledGop==NULL);
  CHECK(mLastStartStatus==EFI_NOT_FOUND && AllocationCount==0);
  { UINT64 Magic,Status;UINT32 Phase;
    memcpy(&Magic,GopProgress,8);memcpy(&Phase,GopProgress+8,4);memcpy(&Status,GopProgress+16,8);
    CHECK(Magic==0x0000000147503754ULL && Phase==2 && Status==EFI_NOT_FOUND);
  }
  CHECK(DriverNotifies[0]==GopDriverExit && DriverGroups[0]==&gEfiEventExitBootServicesGuid);
  CHECK(DriverNotifies[1]==GopRetry && DriverGroups[1]==&gFdtTableGuid);
  CHECK(DriverGroups[2]==&gEfiEndOfDxeEventGroupGuid);
  CHECK(DriverGroups[3]==&gEfiEventReadyToBootGuid);
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_ALREADY_STARTED && DriverEventCalls==4);
  RuntimeStatus=EFI_SUCCESS; RuntimeHandoffBad=1;
  DriverNotifies[1](NULL,NULL);
  CHECK(InstalledGop==NULL && AllocationCount==0);
  RuntimeHandoffBad=0; ReenterStart=TRUE;
  DriverNotifies[2](NULL,NULL);
  CHECK(InstalledGop!=NULL && mPublishedGop!=NULL && mLastStartStatus==EFI_SUCCESS);
  UINTN Before=AllocationCount;
  DriverNotifies[3](NULL,NULL);
  CHECK(AllocationCount==Before);
  ReenterStart=FALSE;
  UINT8 BeforeHandoff[sizeof(OsHandoff)]; memcpy(BeforeHandoff,OsHandoff,sizeof(BeforeHandoff));
  DriverNotifies[0](NULL,NULL);
  CHECK(mExitingBootServices && memcmp(BeforeHandoff,OsHandoff,sizeof(BeforeHandoff))==0);
  EventNotify(NULL,EventContext);
  CHECK(OsHandoff[0]==1 && OsCacheCleans==1);
  // A later driver event must not erase a record owned by the installed GOP.
  DriverNotifies[0](NULL,NULL);
  CHECK(OsHandoff[0]==1 && OsCacheCleans==1);
  free(EventContext);

  ResetServices(); RuntimeStatus=EFI_NOT_FOUND;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS);
  DriverNotifies[0](NULL,NULL); RuntimeStatus=EFI_SUCCESS;
  DriverNotifies[2](NULL,NULL);
  CHECK(InstalledGop==NULL && AllocationCount==0 && OsHandoff[0]==0);
}

static void TestEbsNotificationOrder(void) {
  for (UINTN Invalid=0;Invalid<3;Invalid++) {
    for (UINTN GopFirst=0;GopFirst<2;GopFirst++) {
      ResetServices(); CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS);
      MTK_GOP_CONTEXT *Context=EventContext;
      CHECK(Context!=NULL && Context->Handle!=NULL);
      if (Invalid==1) GcdStatus=EFI_DEVICE_ERROR;
      if (Invalid==2) Context->Info.HorizontalResolution++;
      if (GopFirst) {
        EventNotify(NULL,Context);
        DriverNotifies[0](NULL,NULL);
      } else {
        DriverNotifies[0](NULL,NULL);
        EventNotify(NULL,Context);
      }
      CHECK(mExitingBootServices && OsHandoff[0]==(Invalid==0));
      CHECK(OsPublications==(Invalid==0) && OsCacheCleans==1);
      UINT8 Snapshot[sizeof(OsHandoff)]; memcpy(Snapshot,OsHandoff,sizeof(Snapshot));
      DriverNotifies[0](NULL,NULL);
      CHECK(memcmp(Snapshot,OsHandoff,sizeof(Snapshot))==0 && OsCacheCleans==1);
      EventNotify(NULL,Context);
      CHECK(OsHandoff[0]==0 && OsCacheCleans==2);
      DriverNotifies[0](NULL,NULL);
      CHECK(OsHandoff[0]==0 && OsCacheCleans==2);
      free(Context);
    }
  }
}

static void TestDriverRetryAndFailureLifetime(void) {
  ResetServices(); DriverEventFailMask=1;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_OUT_OF_RESOURCES);
  CHECK(!mDriverRegistered && AllocationCount==0 && DriverEventCalls==1);
  for (UINTN Slot=1;Slot<4;Slot++) {
    ResetServices(); RuntimeStatus=EFI_NOT_FOUND; DriverEventFailMask=1U<<Slot;
    CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS && mDriverRegistered);
    CHECK(InstalledGop==NULL && DriverEventCalls==4);
    RuntimeStatus=EFI_SUCCESS;
    DriverNotifies[Slot==1?2:1](NULL,NULL);
    CHECK(InstalledGop!=NULL);
    EventNotify(NULL,EventContext); free(EventContext);
  }
  ResetServices(); DriverEventFailMask=14;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS && InstalledGop!=NULL);
  EventNotify(NULL,EventContext); free(EventContext);

  ResetServices(); InstallStatus=EFI_OUT_OF_RESOURCES; CloseStatus=EFI_DEVICE_ERROR;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS && InstalledGop==NULL);
  CHECK(mPublishedGop!=NULL && mPublishedGop->Handle==NULL);
  UINTN Before=AllocationCount;
  GopRetry(NULL,NULL); CHECK(AllocationCount==Before);
  GopDriverExit(NULL,NULL); EventNotify(NULL,EventContext);
  CHECK(OsHandoff[0]==0); free(EventContext);
}

static void TestRuntimeCopyGate(void) {
  for(UINTN Bad=1;Bad<=5;Bad++) {
    ResetServices(); RuntimeHandoffBad=Bad;
    CHECK(EFI_ERROR(TryStartGop()));
    CHECK(InstalledGop==NULL && AllocationCount==0 && SetAttributesCount==0);
    CHECK(mTraceDetail==((Bad<=3?2ULL:3ULL)<<32));
  }
}

static void TestProgressFailures(void) {
  for (UINTN I=0;I<6;I++) {
    UINT64 Magic,Status;UINT32 Phase,Attempts;
    ResetServices();
    if(I==0)RuntimeStatus=EFI_NOT_FOUND;
    if(I==1)MemoryMapStatus=EFI_DEVICE_ERROR;
    if(I==2)GcdStatus=EFI_DEVICE_ERROR;
    if(I==3)CreateEventStatus=EFI_OUT_OF_RESOURCES;
    if(I==4)InstallStatus=EFI_OUT_OF_RESOURCES;
    GopRetry(NULL,NULL);
    memcpy(&Magic,GopProgress,8);memcpy(&Phase,GopProgress+8,4);
    memcpy(&Attempts,GopProgress+12,4);memcpy(&Status,GopProgress+16,8);
    CHECK(Magic==0x0000000147503754ULL && Attempts==1);
    CHECK((Phase==(UINT32[]){2,4,5,6,7,8}[I]));
    CHECK((Status==(EFI_STATUS[]){EFI_NOT_FOUND,EFI_DEVICE_ERROR,EFI_DEVICE_ERROR,
          EFI_OUT_OF_RESOURCES,EFI_OUT_OF_RESOURCES,EFI_SUCCESS}[I]));
    CHECK(GopTraceFlushes>=3);
    if(I==5) { EventNotify(NULL,EventContext);free(EventContext); }
  }
  ResetServices();RuntimeHandoffBad=1;GopRetry(NULL,NULL);
  CHECK(GopTraceFlushes==0);
}

static void TestEarlyMemoryMapProgress(void) {
  ResetServices();RuntimeStatus=EFI_NOT_FOUND;
  SeedEarlyMapStatus=EFI_NOT_READY;SeedEarlyMapMeta=0x4D4D048E00000000ULL;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS && InstalledGop==NULL);
  CHECK(mTraceDetail==0x4D048E0080000006ULL);
  SeedEarlyMapStatus=SeedEarlyMapMeta=0;
  GopRetry(NULL,NULL);CHECK(mTraceDetail==0x4D048E0080000006ULL);
  ResetServices();RuntimeStatus=EFI_NOT_FOUND;SeedEarlyMapStatus=EFI_SUCCESS;
  SeedEarlyMapMeta=0x4D4D0B830000000CULL;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS && InstalledGop==NULL);
  CHECK(mTraceDetail==0x4D0B830C00000000ULL);
  ResetServices();LkIo=malloc(sizeof(*LkIo));CHECK(LkIo);
  LkFixtureTree(FdtBuffer,sizeof(FdtBuffer));LkFixtureInitialize(LkIo,0x14005000,0,FALSE,FALSE);
  mEarlyMapMeta=0x4D4D068E00000000ULL;mEarlyMapStatus=EFI_ACCESS_DENIED;
  UINT64 Overlap[]={0x000000014F4D3754ULL,0x90000000,0x90020000,0x90001000,0x90003000,0xFFFFFFFF00000003ULL};
  memcpy(SeedOverlap,Overlap,sizeof(Overlap));
  MTK_DISPLAY_FRAMEBUFFER Fb;CHECK(ReadRuntimeFramebuffer(&Fb)==EFI_UNSUPPORTED);
  UINT64 Magic;memcpy(&Magic,LkSnapshot,8);CHECK(Magic==LK_OVERLAP_SNAPSHOT_MAGIC);
  CHECK(memcmp(LkSnapshot+16,Overlap+1,40)==0);
  UINT32 Tail[4];memcpy(Tail,LkSnapshot+56,16);CHECK((Tail[0]|Tail[1]|Tail[2]|Tail[3])==0);
  free(LkIo);LkIo=NULL;
  ResetServices();RuntimeStatus=EFI_NOT_FOUND;SeedEarlyMapMeta=0x4D4DFF0000000000ULL;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS && mEarlyMapMeta==0);
}

static UINT64 ActivityDetail(void) {
  UINT64 Detail;UINT32 Phase;
  memcpy(&Phase,GopProgress+8,4);memcpy(&Detail,GopProgress+24,8);
  CHECK(Phase==8 && (Detail>>63)==1);
  CHECK(((Detail>>32)&0xffff)==TEST_FB_WIDTH && (Detail&0xffff)==TEST_FB_HEIGHT);
  return Detail;
}

static void TestFirstModePreservesLkManufacturerLogo(void) {
  const UINTN Width=2240U,Height=1400U,Bytes=Width*Height*4U;
  VOID *Buffer=NULL;
  CHECK(posix_memalign(&Buffer,4096,Bytes)==0 && Buffer);
  UINT32 *Pixels=Buffer;
  for(UINTN Index=0;Index<Width*Height;Index++)Pixels[Index]=0x00112233U;
  Pixels[0]=0U;
  Pixels[650U*Width+750U]=0x00F76707U;
  Pixels[650U*Width+1120U]=0x00FC6505U;
  MTK_GOP_CONTEXT Context={0};
  Context.Signature=GOP_SIGNATURE;Context.BootServicesActive=TRUE;
  Context.FrameBuffer.FrameBufferBase=(UINTN)Buffer;
  Context.FrameBuffer.FrameBufferRegionSize=Bytes;
  Context.FrameBuffer.Width=Width;Context.FrameBuffer.Height=Height;
  Context.FrameBuffer.StrideBytes=Width*4U;
  Context.FrameBuffer.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;
  CHECK(MtkDisplayValidateLayout(&Context.FrameBuffer)==EFI_SUCCESS);
  Context.Mode.MaxMode=1;Context.Protocol.Mode=&Context.Mode;
  CHECK(GopSetMode(&Context.Protocol,0)==EFI_SUCCESS);
  CHECK(Pixels[100U*Width+100U]==0x00112233U);
  CHECK(Pixels[650U*Width+750U]==0x00F76707U);
  CHECK(Pixels[1038U*Width+750U]==0U && Pixels[(Height-1U)*Width+1120U]==0U);
  CHECK(Context.SuccessfulSetModeCalls==1U);
  CHECK(GopSetMode(&Context.Protocol,0)==EFI_SUCCESS);
  CHECK(Pixels[100U*Width+100U]==0U && Pixels[650U*Width+750U]==0U);
  free(Buffer);
}

static void TestSuccessfulDrawingProgress(void) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel={1,2,3,0},ReadBack;
  ResetServices();CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS);
  MTK_GOP_CONTEXT *Context=EventContext;
  UINT64 Detail=ActivityDetail();
  CHECK(((Detail>>16)&0xffff)==0 && ((Detail>>48)&0x7fff)==0);
  UINTN Flushes=GopTraceFlushes;
  CHECK(InstalledGop->SetMode(InstalledGop,1)==EFI_UNSUPPORTED);
  CHECK(InstalledGop->Blt(InstalledGop,&Pixel,EfiBltBufferToVideo,0,0,TEST_FB_WIDTH,0,1,1,0)==EFI_INVALID_PARAMETER);
  CHECK(GopTraceFlushes==Flushes && ActivityDetail()==Detail);
  CHECK(InstalledGop->SetMode(InstalledGop,0)==EFI_SUCCESS);
  CHECK(InstalledGop->Blt(InstalledGop,&Pixel,EfiBltBufferToVideo,0,0,4,3,1,1,0)==EFI_SUCCESS);
  Detail=ActivityDetail();
  CHECK(((Detail>>16)&0xffff)==1 && ((Detail>>48)&0x7fff)==1);
  CHECK(((EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)FrameBuffer)[3*TEST_FB_WIDTH+4].Red==3);
  Flushes=GopTraceFlushes;
  CHECK(InstalledGop->Blt(InstalledGop,&ReadBack,EfiBltVideoToBltBuffer,4,3,0,0,1,1,0)==EFI_SUCCESS);
  CHECK(ReadBack.Red==3);
  CHECK(InstalledGop->Blt(InstalledGop,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_SUCCESS);
  CHECK(GopTraceFlushes==Flushes && ActivityDetail()==Detail);
  // Calls during synchronous installation are not published as phase 8.
  mPublishedGop=NULL;
  CHECK(InstalledGop->SetMode(InstalledGop,0)==EFI_SUCCESS);
  CHECK(Context->SuccessfulSetModeCalls==2 && GopTraceFlushes==Flushes);
  mPublishedGop=Context;RecordGopProgress(EFI_SUCCESS);
  CHECK(((ActivityDetail()>>16)&0xffff)==2);
  Context->SuccessfulSetModeCalls=MAX_UINT16;
  Context->SuccessfulBufferToVideoCalls=0x7fff;
  CHECK(InstalledGop->SetMode(InstalledGop,0)==EFI_SUCCESS);
  CHECK(InstalledGop->Blt(InstalledGop,&Pixel,EfiBltBufferToVideo,0,0,0,0,1,1,0)==EFI_SUCCESS);
  Detail=ActivityDetail();
  CHECK(((Detail>>16)&0xffff)==MAX_UINT16 && ((Detail>>48)&0x7fff)==0x7fff);
  Context->BootServicesActive=FALSE;Flushes=GopTraceFlushes;
  CHECK(InstalledGop->SetMode(InstalledGop,0)==EFI_DEVICE_ERROR);
  CHECK(InstalledGop->Blt(InstalledGop,&Pixel,EfiBltBufferToVideo,0,0,0,0,1,1,0)==EFI_UNSUPPORTED);
  CHECK(GopTraceFlushes==Flushes && ActivityDetail()==Detail);
  free(Context);

  ResetServices();DrawDuringInstall=TRUE;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS);
  Detail=ActivityDetail();
  CHECK(((Detail>>16)&0xffff)==1 && ((Detail>>48)&0x7fff)==1);
  EventNotify(NULL,EventContext);free(EventContext);
  ResetServices();DrawDuringInstall=TRUE;InstallStatus=EFI_OUT_OF_RESOURCES;
  CHECK(FramebufferGopEntry(NULL,NULL)==EFI_SUCCESS && InstalledGop==NULL);
  UINT32 Phase;memcpy(&Phase,GopProgress+8,4);memcpy(&Detail,GopProgress+24,8);
  CHECK(Phase==7 && (Detail>>63)==0 && mPublishedGop==NULL);
}

int main(void) {
  TestLkRuntimeAcquisition();
  TestPhysicalRuntimeAcquisition();
  TestFramebufferWriteCombining();
  TestInstalledGopAndDevicePath();
  TestInstallFailureCloseOutcomes();
  TestEarlierFailureCleanup();
  TestOsHandoffFailures();
  TestDeferredDriverStartup();
  TestEbsNotificationOrder();
  TestDriverRetryAndFailureLifetime();
  TestRuntimeCopyGate();
  TestProgressFailures();
  TestEarlyMemoryMapProgress();
  TestFirstModePreservesLkManufacturerLogo();
  TestSuccessfulDrawingProgress();
  CHECK(SuccessContext != NULL && RetainedFailureContext != NULL);
  printf("Framebuffer GOP DXE: %u checks passed (same-handle GOP/device path, GOP calls, deferred FDT/EndOfDxe startup, reentry, runtime-copy gate, EBS and event lifetime)\n", Checks);
  return 0;
}
