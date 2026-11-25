// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Protocol/MtkGpio.h>
#include <Library/GpioImplLib.h>
#ifdef TEST_MT6768
#define TEST_LAST_PIN 179U
#else
#define TEST_LAST_PIN 187U
#endif
EFI_GUID gMediaTekGpioProtocolGuid;
static BOOLEAN FailLeaseAllocation;
VOID *EFIAPI AllocateZeroPool(UINTN Size) {
  return FailLeaseAllocation?NULL:calloc(1,Size);
}
static EFI_STATUS EFIAPI RejectInstall(EFI_HANDLE *Handle,...) {
  (VOID)Handle;Installs++;return EFI_DEVICE_ERROR;
}
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/GpioDxe/GpioDxe.c"
#ifdef TEST_MT6768
#include "../../../../Silicon/MediaTek/MT6768Pkg/Library/GpioImplLib/GpioImplLib.c"
#else
#include "../../../../Silicon/MediaTek/MT6789Pkg/Library/GpioImplLib/GpioImplLib.c"
#endif
static UINT32 Registers[0x1000/4];
static UINT32 IoCfgRegisters[0x1000/4];
static unsigned Reads, Writes;
static UINTN WriteAddresses[2048];
static unsigned WriteAddressCount;
static UINTN DropOnceAddress;
static UINTN DropAfterFirstAddress;
static unsigned DropAfterFirstWrites;
static EFI_TPL CurrentTpl=TPL_APPLICATION;
static BOOLEAN ReenterOnWrite;
static EFI_STATUS ReentryStatus;
static UINT32 ReentryPin;

EFI_STATUS EFIAPI EfiAcquireLockOrFail(EFI_LOCK *Lock) {
  CHECK(Lock!=NULL && Lock->Lock!=EfiLockUninitialized);
  if(Lock->Lock==EfiLockAcquired) return EFI_ACCESS_DENIED;
  CHECK(CurrentTpl<=Lock->Tpl);
  Lock->OwnerTpl=CurrentTpl;
  CurrentTpl=Lock->Tpl;
  Lock->Lock=EfiLockAcquired;
  return EFI_SUCCESS;
}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *Lock) {
  CHECK(Lock!=NULL && Lock->Lock==EfiLockAcquired);
  CurrentTpl=Lock->OwnerTpl;
  Lock->Lock=EfiLockReleased;
}

UINT32 EFIAPI MmioRead32(UINTN address) {
  if(address>=0x200000 && address<0x201000 && (address&3)==0) {
    CHECK(mGpioLock.Lock==EfiLockAcquired && CurrentTpl==TPL_NOTIFY);
    Reads++;
    return IoCfgRegisters[(address-0x200000)/4];
  }
  CHECK(address>=0x100000 && address<0x101000 && (address&3)==0);
  UINTN offset=address-0x100000;
  CHECK((offset&15)==0); // SET/CLR/MWR aliases are not readable values.
  CHECK(mGpioLock.Lock==EfiLockAcquired && CurrentTpl==TPL_NOTIFY);
  Reads++;
  return Registers[offset/4];
}
UINT32 EFIAPI MmioWrite32(UINTN address,UINT32 value) {
  if(address>=0x200000 && address<0x201000 && (address&3)==0) {
    CHECK(mGpioLock.Lock==EfiLockAcquired && CurrentTpl==TPL_NOTIFY);
    if(WriteAddressCount<ARRAY_SIZE(WriteAddresses)) WriteAddresses[WriteAddressCount++]=address;
    Writes++;
    if(address==DropOnceAddress) { DropOnceAddress=0; return value; }
    if(address==DropAfterFirstAddress) {
      DropAfterFirstWrites++;
      if(DropAfterFirstWrites>1) return value;
    }
    IoCfgRegisters[(address-0x200000)/4]=value;
    return value;
  }
  CHECK(address>=0x100000 && address<0x101000 && (address&3)==0);
  CHECK(mGpioLock.Lock==EfiLockAcquired && CurrentTpl==TPL_NOTIFY);
  if(ReenterOnWrite) {
    ReenterOnWrite=FALSE;
    ReentryStatus=mGpio.SetState(ReentryPin,TRUE);
  }
  UINTN offset=address-0x100000, base=offset&~15U;
  if(WriteAddressCount<ARRAY_SIZE(WriteAddresses)) WriteAddresses[WriteAddressCount++]=address;
  Writes++;
  if ((offset&15)==4) Registers[base/4]|=value;
  else if ((offset&15)==8) Registers[base/4]&=~value;
  else if ((offset&15)==12) {
    CHECK(base>=0x300);
    for(unsigned shift=0;shift<32;shift+=4) {
      if(value&(8U<<shift)) {
        Registers[base/4]=(Registers[base/4]&~(7U<<shift))|(((value>>shift)&7U)<<shift);
      }
    }
  } else CHECK(0);
  return value;
}
int main(void) {
  BOOLEAN result=FALSE;
  BOOLEAN busy=FALSE;
  UINT32 leasePins[2]={152,155};
  UINT32 duplicatePins[2]={153,153};
  UINT32 overlapPins[2]={153,152};
  UINT32 invalidPins[2]={TEST_LAST_PIN,TEST_LAST_PIN+1};
  UINT32 allPins[TEST_LAST_PIN+1];
  UINT32 sdPins[6]={71,72,73,74,75,76};
  UINT64 token=0,otherToken=0,staleToken;
  BOOLEAN canRelease=TRUE;
  Services.InstallMultipleProtocolInterfaces=Install;
  LocateStatus=EFI_NOT_FOUND;
  CHECK(InitGpioDriver(NULL,NULL)==EFI_NOT_FOUND && Installs==0);
  LocateStatus=EFI_SUCCESS;
  FailLeaseAllocation=TRUE;
  CHECK(InitGpioDriver(NULL,NULL)==EFI_OUT_OF_RESOURCES && !mLeaseMemory && !mPinCount && Installs==0);
  FailLeaseAllocation=FALSE;
  Services.InstallMultipleProtocolInterfaces=RejectInstall;
  CHECK(InitGpioDriver(NULL,NULL)==EFI_DEVICE_ERROR && Installs==1 &&
        !mLeaseMemory && !mPinLeaseOwner && !mLeaseTokens && !mAcquirePinScratch &&
        !mLeaseSeen && !mLeaseSeenBytes && !mPinCount && !mGpioInstalled);
  Services.InstallMultipleProtocolInterfaces=Install;
  CHECK(InitGpioDriver(NULL,NULL)==EFI_SUCCESS && Installs==2);
  CHECK(mGpio.AcquirePins && mGpio.ReleasePins && mGpio.ValidateLease &&
        mGpio.SetDirWithLease && mGpio.SetStateWithLease &&
        mGpio.SetModeWithLease && mGpio.IsBusy &&
        mGpio.ConfigureMsdc1PadsWithLease);
  unsigned before=Reads+Writes;
  CHECK(mGpio.GetDir(0,NULL)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.GetState(0,NULL)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.GetDir(TEST_LAST_PIN+1,&result)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.GetState(TEST_LAST_PIN+1,&result)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.SetDir(TEST_LAST_PIN+1,TRUE)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.SetState(TEST_LAST_PIN+1,TRUE)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.SetMode(TEST_LAST_PIN+1,0)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.SetMode(0,8)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.SetMode(7,0xffffffffU)==EFI_INVALID_PARAMETER);
  CHECK(Reads+Writes==before);
  for(UINT32 pin=0;pin<=TEST_LAST_PIN;pin++) {
    UINT32 bank=(pin/32)*0x10, bit=1U<<(pin%32);
    Registers[bank/4]=0x55aa55aaU;
    UINT32 neighbors=Registers[bank/4]&~bit;
    CHECK(mGpio.SetDir(pin,TRUE)==EFI_SUCCESS);
    CHECK((Registers[bank/4]&~bit)==neighbors && (Registers[bank/4]&bit)!=0);
    CHECK(mGpio.GetDir(pin,&result)==EFI_SUCCESS && result==TRUE);
    CHECK(mGpio.SetDir(pin,FALSE)==EFI_SUCCESS);
    CHECK(Registers[bank/4]==neighbors);
    CHECK(mGpio.GetDir(pin,&result)==EFI_SUCCESS && result==FALSE);
    Registers[(0x100+bank)/4]=0x55aa55aaU;
    neighbors=Registers[(0x100+bank)/4]&~bit;
    CHECK(mGpio.SetState(pin,TRUE)==EFI_SUCCESS);
    CHECK(Registers[(0x100+bank)/4]==(neighbors|bit));
    CHECK(mGpio.SetState(pin,FALSE)==EFI_SUCCESS);
    CHECK(Registers[(0x100+bank)/4]==neighbors);
    for(unsigned output=0;output<2;output++) {
      CHECK(mGpio.SetDir(pin,output)==EFI_SUCCESS);
      Registers[(0x100+bank)/4]&=~bit;
      Registers[(0x200+bank)/4]|=bit;
      CHECK(mGpio.GetState(pin,&result)==EFI_SUCCESS && result==TRUE);
      Registers[(0x100+bank)/4]|=bit;
      Registers[(0x200+bank)/4]&=~bit;
      CHECK(mGpio.GetState(pin,&result)==EFI_SUCCESS && result==FALSE);
    }
    UINT32 modeReg=0x300+(pin/8)*0x10, shift=(pin%8)*4;
    for(UINT32 mode=0;mode<8;mode++) {
      Registers[modeReg/4]=0x76543210U;
      UINT32 original=Registers[modeReg/4];
      unsigned written=Writes;
      CHECK(mGpio.SetMode(pin,mode)==EFI_SUCCESS);
#ifdef TEST_MT6768
      CHECK(Writes==written+2);
#else
      CHECK(Writes==written+1);
#endif
      CHECK(Registers[modeReg/4]==((original&~(7U<<shift))|(mode<<shift)));
    }
  }
  CHECK(mGpio.AcquirePins(leasePins,2,&token)==EFI_SUCCESS && token!=0);
  CHECK(CurrentTpl==TPL_APPLICATION);
  CHECK(mGpio.IsBusy(152,&busy)==EFI_SUCCESS && busy);
  CHECK(mGpio.IsBusy(155,&busy)==EFI_SUCCESS && busy);
  CHECK(mGpio.IsBusy(153,&busy)==EFI_SUCCESS && !busy);
  CHECK(mGpio.ValidateLease(token,152)==EFI_SUCCESS);
  CHECK(mGpio.ValidateLease(token,155)==EFI_SUCCESS);
  CHECK(mGpio.ValidateLease(token,153)==EFI_ACCESS_DENIED);
  CHECK(mGpio.ValidateLease(0,152)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.SetStateWithLease(0,152,FALSE)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.IsBusy(152,NULL)==EFI_INVALID_PARAMETER);
  before=Writes;
  CHECK(mGpio.SetDir(152,TRUE)==EFI_ACCESS_DENIED);
  CHECK(mGpio.SetState(152,TRUE)==EFI_ACCESS_DENIED);
  CHECK(mGpio.SetMode(152,0)==EFI_ACCESS_DENIED && Writes==before);
  CHECK(mGpio.SetModeWithLease(token,152,0)==EFI_SUCCESS);
  CHECK(mGpio.SetDirWithLease(token,152,TRUE)==EFI_SUCCESS);
  CHECK(mGpio.SetStateWithLease(token,152,TRUE)==EFI_SUCCESS);
  CHECK(mGpio.SetStateWithLease(token,155,FALSE)==EFI_SUCCESS);
  before=Writes;
  CHECK(mGpio.SetDirWithLease(token,153,TRUE)==EFI_ACCESS_DENIED);
  CHECK(mGpio.SetStateWithLease(token,154,TRUE)==EFI_ACCESS_DENIED);
  CHECK(mGpio.SetModeWithLease(token,153,0)==EFI_ACCESS_DENIED && Writes==before);
  CHECK(mGpio.GetDir(152,&result)==EFI_SUCCESS && result);
  CHECK(mGpio.AcquirePins(overlapPins,2,&otherToken)==EFI_ACCESS_DENIED && otherToken==0);
  CHECK(mGpio.IsBusy(153,&busy)==EFI_SUCCESS && !busy);
  CHECK(mGpio.AcquirePins(duplicatePins,2,&otherToken)==EFI_INVALID_PARAMETER && otherToken==0);
  CHECK(mGpio.IsBusy(153,&busy)==EFI_SUCCESS && !busy);
  CHECK(mGpio.AcquirePins(invalidPins,2,&otherToken)==EFI_INVALID_PARAMETER && otherToken==0);
  CHECK(mGpio.IsBusy(TEST_LAST_PIN,&busy)==EFI_SUCCESS && !busy);
  CHECK(mGpio.AcquirePins(leasePins,0,&otherToken)==EFI_INVALID_PARAMETER && otherToken==0);
  CHECK(mGpio.AcquirePins(NULL,1,&otherToken)==EFI_INVALID_PARAMETER && otherToken==0);
  CHECK(mGpio.AcquirePins(leasePins,2,NULL)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.ReleasePins(token)==EFI_SUCCESS);
  staleToken=token;
  token=0;
  CHECK(mGpio.IsBusy(152,&busy)==EFI_SUCCESS && !busy);
  CHECK(mGpio.ValidateLease(staleToken,152)==EFI_INVALID_PARAMETER);
  before=Writes;
  CHECK(mGpio.SetStateWithLease(staleToken,152,FALSE)==EFI_INVALID_PARAMETER && Writes==before);
  CHECK(mGpio.ReleasePins(staleToken)==EFI_INVALID_PARAMETER);
  CHECK(mGpio.SetState(152,FALSE)==EFI_SUCCESS);
  CHECK(mGpio.AcquirePins(leasePins,2,&token)==EFI_SUCCESS && token!=0 && token!=staleToken);
  CHECK(mGpio.ReleasePins(token)==EFI_SUCCESS);

  // MSDC1 pad setup is one bounded lease operation. A lease missing pin 71
  // and unavailable platform mappings must be rejected before touching MMIO.
  CHECK(mGpio.AcquirePins(&sdPins[1],5,&token)==EFI_SUCCESS);
  before=Reads+Writes;
  canRelease=FALSE;
#ifdef TEST_MT6768
  CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_UNSUPPORTED && canRelease);
#else
  CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_ACCESS_DENIED && canRelease);
#endif
  CHECK(Reads+Writes==before);
  CHECK(mGpio.ReleasePins(token)==EFI_SUCCESS);

  CHECK(mGpio.AcquirePins(sdPins,ARRAY_SIZE(sdPins),&token)==EFI_SUCCESS);
  before=Reads+Writes;
  canRelease=FALSE;
#ifdef TEST_MT6768
  CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_UNSUPPORTED && canRelease);
  CHECK(Reads+Writes==before);
#else
  CHECK(gPlatformInfo.ConfigureMsdc1Pads!=NULL && mIoCfgRtAvailable);
  {
    UINTN SavedPinCount=mPinCount;
    mPinCount=76U;
    CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_UNSUPPORTED && canRelease);
    CHECK(Reads+Writes==before);
    mPinCount=SavedPinCount;
  }
  {
    MTK_GPIO_CONFIGURE_MSDC1_PADS SavedConfigure=gPlatformInfo.ConfigureMsdc1Pads;
    gPlatformInfo.ConfigureMsdc1Pads=NULL;
    CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_UNSUPPORTED && canRelease);
    CHECK(Reads+Writes==before);
    gPlatformInfo.ConfigureMsdc1Pads=SavedConfigure;
  }
  mIoCfgRtAvailable=FALSE;
  CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_NOT_READY && canRelease);
  CHECK(Reads+Writes==before);
  mIoCfgRtAvailable=TRUE;

  {
    UINT32 OldIes,OldDrive,OldPupd,OldR0,OldR1,OldDir,OldMode71,OldMode72;
    UINT32 IesMask=0x7CU,DriveMask=0x07FFFE00U,BiasMask=0x3FU;
    UINT32 DirMask=0x1F00U,Mode71Mask=7U<<28,Mode72Mask=0x77777U;
    OldIes=IoCfgRegisters[0x80/4]=0xA5A55AA5U;
    OldDrive=IoCfgRegisters[0x30/4]=0x5AA5A55AU;
    OldPupd=IoCfgRegisters[0xB0/4]=0x123456A5U;
    OldR0=IoCfgRegisters[0xE0/4]=0x234567A5U;
    OldR1=IoCfgRegisters[0xF0/4]=0x345678A5U;
    OldDir=Registers[0x20/4]=0xFFFFFFFFU;
    OldMode71=Registers[0x380/4]=0x76543210U;
    OldMode72=Registers[0x390/4]=0x76543210U;
    WriteAddressCount=0;
    CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_SUCCESS && canRelease);
    CHECK((IoCfgRegisters[0x80/4]&IesMask)==IesMask &&
          (IoCfgRegisters[0x80/4]&~IesMask)==(OldIes&~IesMask) &&
          (IoCfgRegisters[0x80/4]&2U)==(OldIes&2U));
    CHECK((IoCfgRegisters[0x30/4]&DriveMask)==0x036DB600U &&
          (IoCfgRegisters[0x30/4]&~DriveMask)==(OldDrive&~DriveMask));
    CHECK((IoCfgRegisters[0xB0/4]&BiasMask)==1U &&
          (IoCfgRegisters[0xB0/4]&~BiasMask)==(OldPupd&~BiasMask));
    CHECK((IoCfgRegisters[0xE0/4]&BiasMask)==0x3EU &&
          (IoCfgRegisters[0xE0/4]&~BiasMask)==(OldR0&~BiasMask));
    CHECK((IoCfgRegisters[0xF0/4]&BiasMask)==1U &&
          (IoCfgRegisters[0xF0/4]&~BiasMask)==(OldR1&~BiasMask));
    CHECK((Registers[0x20/4]&DirMask)==0 &&
          (Registers[0x20/4]&~DirMask)==(OldDir&~DirMask));
    CHECK((Registers[0x380/4]&Mode71Mask)==(1U<<28) &&
          (Registers[0x380/4]&~Mode71Mask)==(OldMode71&~Mode71Mask));
    CHECK((Registers[0x390/4]&Mode72Mask)==0x11111U &&
          (Registers[0x390/4]&~Mode72Mask)==(OldMode72&~Mode72Mask));
    CHECK(WriteAddressCount>=2 && WriteAddresses[WriteAddressCount-2]==0x10038C &&
          WriteAddresses[WriteAddressCount-1]==0x10039C);

    // A dropped drive-register update is detected and rolls all touched bits
    // back. The lease is releasable only after every restored field verifies.
    IoCfgRegisters[0x80/4]=OldIes; IoCfgRegisters[0x30/4]=OldDrive;
    IoCfgRegisters[0xB0/4]=OldPupd; IoCfgRegisters[0xE0/4]=OldR0;
    IoCfgRegisters[0xF0/4]=OldR1; Registers[0x20/4]=OldDir;
    Registers[0x380/4]=OldMode71; Registers[0x390/4]=OldMode72;
    DropOnceAddress=0x200030;
    canRelease=FALSE;
    CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_DEVICE_ERROR && canRelease);
    CHECK((IoCfgRegisters[0x80/4]&IesMask)==(OldIes&IesMask));
    CHECK((IoCfgRegisters[0x30/4]&DriveMask)==(OldDrive&DriveMask));
    CHECK((IoCfgRegisters[0xB0/4]&BiasMask)==(OldPupd&BiasMask));
    CHECK((IoCfgRegisters[0xE0/4]&BiasMask)==(OldR0&BiasMask));
    CHECK((IoCfgRegisters[0xF0/4]&BiasMask)==(OldR1&BiasMask));
    CHECK((Registers[0x20/4]&DirMask)==(OldDir&DirMask));
    CHECK((Registers[0x380/4]&Mode71Mask)==(OldMode71&Mode71Mask));
    CHECK((Registers[0x390/4]&Mode72Mask)==(OldMode72&Mode72Mask));

    // If rollback itself fails, report that the lease must remain held.
    DropOnceAddress=0x200030;
    DropAfterFirstAddress=0x200080;
    DropAfterFirstWrites=0;
    canRelease=TRUE;
    CHECK(mGpio.ConfigureMsdc1PadsWithLease(token,&canRelease)==EFI_DEVICE_ERROR && !canRelease);
    CHECK(mGpio.IsBusy(71,&busy)==EFI_SUCCESS && busy);
    before=Writes;
    CHECK(mGpio.SetMode(71,0)==EFI_ACCESS_DENIED && Writes==before);
    DropAfterFirstAddress=0;
  }
#endif
  CHECK(mGpio.ReleasePins(token)==EFI_SUCCESS);

  for(UINT32 pin=0;pin<ARRAY_SIZE(allPins);pin++) allPins[pin]=pin;
  CHECK(mGpio.AcquirePins(allPins,ARRAY_SIZE(allPins),&token)==EFI_SUCCESS);
  CHECK(mGpio.IsBusy(0,&busy)==EFI_SUCCESS && busy);
  CHECK(mGpio.IsBusy(TEST_LAST_PIN,&busy)==EFI_SUCCESS && busy);
  before=Writes;
  CHECK(mGpio.SetState(TEST_LAST_PIN,FALSE)==EFI_ACCESS_DENIED && Writes==before);
  CHECK(mGpio.ReleasePins(token)==EFI_SUCCESS);

  // A same-TPL callback cannot reenter a GPIO transaction while validation and
  // its MMIO write are in progress; it fails without another register write.
  ReenterOnWrite=TRUE;
  ReentryStatus=EFI_SUCCESS;
  ReentryPin=4;
  before=Writes;
  CHECK(mGpio.SetState(3,TRUE)==EFI_SUCCESS);
  CHECK(ReentryStatus==EFI_NOT_READY && Writes==before+1);

  // A caller already at TPL_NOTIFY can use the same-priority lock safely.
  CurrentTpl=TPL_NOTIFY;
  CHECK(mGpio.AcquirePins(leasePins,2,&token)==EFI_SUCCESS);
  CHECK(CurrentTpl==TPL_NOTIFY);
  CHECK(mGpio.ReleasePins(token)==EFI_SUCCESS && CurrentTpl==TPL_NOTIFY);
  CurrentTpl=TPL_APPLICATION;

  // Older controllers retain their SET/CLR mode interface.
  gPlatformInfo.ModeMaskWriteOffset=0;
  for(UINT32 mode=0;mode<8;mode++) {
    Registers[0x300/4]=0x76543217U;
    CHECK(mGpio.SetMode(0,mode)==EFI_SUCCESS);
    CHECK(Registers[0x300/4]==(0x76543210U|mode));
  }
#ifdef TEST_MT6768
  printf("GPIO MT6768: %u checks passed (legacy mux/leases preserved; MSDC1 pad API unsupported without writes)\n",Checks);
#else
  printf("GPIO MT6789: %u checks passed (MSDC1 map, field masks, mux-last, readback, rollback, lease quarantine)\n",Checks);
#endif
}
