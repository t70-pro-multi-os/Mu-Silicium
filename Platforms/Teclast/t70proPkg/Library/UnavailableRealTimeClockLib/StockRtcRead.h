// SPDX-License-Identifier: BSD-2-Clause-Patent
// Boot transport. The selected library retains its historical path.
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/FdtLib.h>
#include <Library/TimerLib.h>
#include <Protocol/MtkPmicWrapper.h>

static EFI_BOOT_SERVICES *mRtcBoot;
static EFI_EVENT mRtcExitEvent;
static BOOLEAN mRtcEnded, mRtcReading, mRtcWriteFault, mRtcFdtValid;
static VOID EFIAPI StockRtcExit(EFI_EVENT Event, VOID *Context) {
  (VOID)Event; (VOID)Context; mRtcEnded=TRUE; mRtcBoot=NULL; mRtcFdtValid=FALSE;
}
static BOOLEAN RtcString(const VOID *Fdt, INT32 Node, const CHAR8 *Key,
                         const CHAR8 *Value, UINTN Size) {
  INT32 Length; const VOID *P=FdtGetProp(Fdt,Node,Key,&Length);
  return P && Length==(INT32)Size && CompareMem(P,Value,Size)==0;
}
static BOOLEAN RtcEnabled(const VOID *Fdt, INT32 Node) {
  INT32 Length; const VOID *P=FdtGetProp(Fdt,Node,"status",&Length);
  return (!P && Length==-FDT_ERR_NOTFOUND) ||
    RtcString(Fdt,Node,"status","okay",5) || RtcString(Fdt,Node,"status","ok",3);
}
static BOOLEAN RtcTree(const VOID *Fdt) {
  INT32 Soc,Bus,Pmic,Rtc,Length; const VOID *Reg; UINT32 Bytes;
  static const UINT8 ExpectedReg[]={0,0,0,0,0x10,2,0x60,0,0,0,0,0,0,0,0x10,0};
  if (!Fdt || FdtCheckHeader(Fdt)) return FALSE;
  Bytes=FdtTotalSize(Fdt);
  if (Bytes>0x200000 || FdtCheckFull(Fdt,Bytes)) return FALSE;
  Soc=FdtPathOffset(Fdt,"/soc"); Bus=FdtPathOffset(Fdt,"/soc/pwrap@10026000");
  Pmic=FdtPathOffset(Fdt,"/soc/pwrap@10026000/mt6366");
  Rtc=FdtPathOffset(Fdt,"/soc/pwrap@10026000/mt6366/mt6358rtc");
  if (Soc<0 || Bus<0 || Pmic<0 || Rtc<0 || !RtcEnabled(Fdt,0) ||
      !RtcEnabled(Fdt,Soc) || !RtcEnabled(Fdt,Bus) || !RtcEnabled(Fdt,Pmic) || !RtcEnabled(Fdt,Rtc) ||
      !RtcString(Fdt,Bus,"compatible","mediatek,mt6789-pwrap",22) ||
      !RtcString(Fdt,Pmic,"compatible","mediatek,mt6366",16) ||
      !RtcString(Fdt,Rtc,"compatible","mediatek,mt6358-rtc",20)) return FALSE;
  Reg=FdtGetProp(Fdt,Bus,"reg",&Length);
  return Reg && Length==sizeof(ExpectedReg) && CompareMem(Reg,ExpectedReg,sizeof(ExpectedReg))==0;
}
static VOID EFIAPI StockRtcFdtChanged(EFI_EVENT Event, VOID *Context) {
  EFI_SYSTEM_TABLE *St=Context;
  (VOID)Event;
  if (mRtcEnded) return;
  mRtcFdtValid=FALSE;
  for (UINTN I=0;I<St->NumberOfTableEntries;I++) {
    if (CompareGuid(&St->ConfigurationTable[I].VendorGuid,&gFdtTableGuid)) {
      mRtcFdtValid=RtcTree(St->ConfigurationTable[I].VendorTable);
      return;
    }
  }
}

static VOID StockRtcEnable(EFI_SYSTEM_TABLE *St) {
  EFI_EVENT Changed=NULL,Exit=NULL;
  EFI_BOOT_SERVICES *Bs=St->BootServices;
  if (mRtcEnded || mRtcExitEvent || !Bs->CreateEventEx || !Bs->LocateProtocol ||
      !Bs->RaiseTPL || !Bs->RestoreTPL) return;
  if (EFI_ERROR(Bs->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,StockRtcExit,NULL,
                                 &gEfiEventExitBootServicesGuid,&Exit)) || !Exit) return;
  mRtcExitEvent=Exit;
  mRtcBoot=Bs;
  (VOID)Bs->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,StockRtcFdtChanged,St,
                          &gFdtTableGuid,&Changed);
  StockRtcFdtChanged(NULL,St);
}
static EFI_STATUS StockRtcReadRegister(VOID *Context,UINT16 Address,UINT16 *Value) {
  MTK_PMIC_WRAPPER_PROTOCOL *P=Context;
  EFI_STATUS Status;
  if (mRtcEnded) return EFI_UNSUPPORTED;
  Status=P->Read(Address,Value);
  return mRtcEnded?EFI_UNSUPPORTED:Status;
}

static EFI_STATUS StockRtcWriteRegister(VOID *Context,UINT16 Address,UINT16 Value) {
  MTK_PMIC_WRAPPER_PROTOCOL *P=Context;
  EFI_STATUS Status;
  if (mRtcEnded) return EFI_UNSUPPORTED;
  Status=P->Write(Address,Value);
  return mRtcEnded?EFI_UNSUPPORTED:Status;
}

static EFI_STATUS StockRtcAccess(EFI_TIME *Time,BOOLEAN SetTime,const UINT16 *Values) {
  EFI_BOOT_SERVICES *Bs=mRtcBoot;
  MTK_PMIC_WRAPPER_PROTOCOL *P=NULL;
  MTK_RTC_IO Io;
  EFI_TPL Old;
  EFI_STATUS Status;
  Old=Bs->RaiseTPL(TPL_HIGH_LEVEL); Bs->RestoreTPL(Old);
  if (mRtcEnded) return EFI_UNSUPPORTED;
  if (Old>TPL_CALLBACK || (SetTime && Old!=TPL_APPLICATION)) return EFI_NOT_READY;
  Old=Bs->RaiseTPL(TPL_NOTIFY);
  if (mRtcEnded || mRtcReading) { Bs->RestoreTPL(Old); return EFI_NOT_READY; }
  if (mRtcWriteFault) { Bs->RestoreTPL(Old); return EFI_DEVICE_ERROR; }
  mRtcReading=TRUE;
  Bs->RestoreTPL(Old);
  Status=Bs->LocateProtocol(&gMediaTekPmicWrapperProtocolGuid,NULL,(VOID **)&P);
  if (mRtcEnded) Status=EFI_UNSUPPORTED;
  else if (EFI_ERROR(Status) || !P || !P->Read || (SetTime && !P->Write)) Status=EFI_NOT_READY;
  else {
    Io=(MTK_RTC_IO){P,StockRtcReadRegister,StockRtcWriteRegister};
    Status=SetTime?MtkRtcWriteCalendar(&Io,Values,&mRtcWriteFault):MtkRtcReadCalendar(&Io,Time);
  }
  if (mRtcEnded) { mRtcReading=FALSE; return EFI_UNSUPPORTED; }
  Old=Bs->RaiseTPL(TPL_NOTIFY);
  mRtcReading=FALSE; Bs->RestoreTPL(Old);
  return Status;
}

static EFI_STATUS StockRtcGetTime(EFI_TIME *Time) {
  if (mRtcEnded || !mRtcBoot || !mRtcFdtValid) return EFI_UNSUPPORTED;
  if (!Time) return EFI_INVALID_PARAMETER;
  return StockRtcAccess(Time,FALSE,NULL);
}

static EFI_STATUS StockRtcSetTime(const EFI_TIME *Time) {
  EFI_STATUS Status;
  UINT16 Values[MTK_RTC_FIELDS];
  if (mRtcEnded || !mRtcBoot || !mRtcFdtValid) return EFI_UNSUPPORTED;
  Status=MtkRtcEncodeCalendar(Time,Values);
  if (EFI_ERROR(Status)) return Status;
  return StockRtcAccess(NULL,TRUE,Values);
}
