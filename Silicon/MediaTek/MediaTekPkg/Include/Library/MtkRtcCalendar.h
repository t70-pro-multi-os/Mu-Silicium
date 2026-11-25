// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_RTC_CALENDAR_H
#define MTK_RTC_CALENDAR_H

#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>

#define MTK_RTC_BBPU       0x588U
#define MTK_RTC_SECONDS    0x592U
#define MTK_RTC_TRIGGER    0x5C2U
#define MTK_RTC_BUSY       0x40U
#define MTK_RTC_FIELDS     7U

typedef struct {
  VOID       *Context;
  EFI_STATUS (*Read)(VOID *, UINT16, UINT16 *);
  EFI_STATUS (*Write)(VOID *, UINT16, UINT16);
} MTK_RTC_IO;

static const UINT8 mMtkRtcMonthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};

static inline BOOLEAN MtkRtcLeapYear(UINT16 Year) {
  return Year%4==0 && (Year%100!=0 || Year%400==0);
}

static inline UINT8 MtkRtcDaysInMonth(UINT16 Year,UINT8 Month) {
  return mMtkRtcMonthDays[Month-1] + (Month==2 && MtkRtcLeapYear(Year));
}

// MT6366 uses the MT6358 calendar layout in rtc-mt6397.
static inline EFI_STATUS MtkRtcReadCalendar(const MTK_RTC_IO *Io,EFI_TIME *Time) {
  EFI_TIME T;
  EFI_STATUS Status;
  UINT16 Values[MTK_RTC_FIELDS],Seconds;
  UINTN Attempt,Index;

  for (Attempt=0;Attempt<3;Attempt++) {
    for (Index=0;Index<MTK_RTC_FIELDS;Index++) {
      Status=Io->Read(Io->Context,(UINT16)(MTK_RTC_SECONDS+2*Index),&Values[Index]);
      if (EFI_ERROR(Status)) return Status;
    }
    Status=Io->Read(Io->Context,MTK_RTC_SECONDS,&Seconds);
    if (EFI_ERROR(Status)) return Status;
    if ((Seconds&63)==(Values[0]&63)) break;
  }
  if (Attempt==3) return EFI_NOT_READY;

  ZeroMem(&T,sizeof(T));
  T.Second=Values[0]&63; T.Minute=Values[1]&63;
  T.Hour=Values[2]&31; T.Day=Values[3]&31;
  T.Month=Values[5]&15; T.Year=1968+(Values[6]&127);
  if (T.Second>59 || T.Minute>59 || T.Hour>23 || !T.Month ||
      T.Month>12 || !T.Day || T.Day>MtkRtcDaysInMonth(T.Year,T.Month)) return EFI_DEVICE_ERROR;
  // RealTimeClockRuntimeDxe keeps timezone and daylight in its own variable.
  T.TimeZone=Time->TimeZone; T.Daylight=Time->Daylight; *Time=T;
  return EFI_SUCCESS;
}

static inline EFI_STATUS MtkRtcEncodeCalendar(const EFI_TIME *Time,UINT16 Values[MTK_RTC_FIELDS]) {
  UINT32 PriorYears,Ordinal;
  UINTN Index;
  if (!Time || Time->Year<1968 || Time->Year>2095 || !Time->Month || Time->Month>12 ||
      !Time->Day || Time->Day>MtkRtcDaysInMonth(Time->Year,Time->Month) ||
      Time->Hour>23 || Time->Minute>59 || Time->Second>59 ||
      Time->Nanosecond>999999999 || (Time->Daylight&~3U) ||
      (Time->TimeZone!=EFI_UNSPECIFIED_TIMEZONE && (Time->TimeZone<-1440 || Time->TimeZone>1440))) {
    return EFI_INVALID_PARAMETER;
  }
  Values[0]=Time->Second; Values[1]=Time->Minute; Values[2]=Time->Hour; Values[3]=Time->Day;
  // 0001-01-01 was Monday. Stock uses tm_wday + 1 (Sunday=1).
  PriorYears=Time->Year-1;
  Ordinal=365*PriorYears+PriorYears/4-PriorYears/100+PriorYears/400+Time->Day;
  for (Index=1;Index<Time->Month;Index++) Ordinal+=MtkRtcDaysInMonth(Time->Year,(UINT8)Index);
  Values[4]=(UINT16)(Ordinal%7+1); Values[5]=Time->Month; Values[6]=(UINT16)(Time->Year-1968);
  return EFI_SUCCESS;
}

static inline EFI_STATUS MtkRtcWriteCalendar(const MTK_RTC_IO *Io,
  const UINT16 Values[MTK_RTC_FIELDS],BOOLEAN *WriteFault) {
  EFI_STATUS Status;
  UINT16 Busy;
  UINTN Index;
  UINT64 Start,Now,Low,High,Delta;

  Status=Io->Read(Io->Context,MTK_RTC_BBPU,&Busy);
  if (EFI_ERROR(Status)) return Status;
  if (Busy&MTK_RTC_BUSY) return EFI_NOT_READY;
  // A fault after sending a shadow write or trigger may have affected RTC.
  // Block subsequent calendar accesses until ownership starts afresh.
  *WriteFault=TRUE;
  for (Index=0;Index<MTK_RTC_FIELDS;Index++) {
    Status=Io->Write(Io->Context,(UINT16)(MTK_RTC_SECONDS+2*Index),Values[Index]);
    if (EFI_ERROR(Status)) return Status;
  }
  Status=Io->Write(Io->Context,MTK_RTC_TRIGGER,1);
  if (EFI_ERROR(Status)) return Status;
  (VOID)GetPerformanceCounterProperties(&Low,&High);
  Start=GetPerformanceCounter();
  for (Index=0;Index<=1000;Index++) {
    Status=Io->Read(Io->Context,MTK_RTC_BBPU,&Busy);
    if (EFI_ERROR(Status)) return Status;
    if (!(Busy&MTK_RTC_BUSY)) { *WriteFault=FALSE; return EFI_SUCCESS; }
    Now=GetPerformanceCounter();
    Delta=High>Low?Now-Start:Start-Now;
    if (GetTimeInNanoSecond(Delta)>=1000000000ULL || Index==1000) break;
    MicroSecondDelay(1000);
  }
  return EFI_TIMEOUT;
}

#endif
