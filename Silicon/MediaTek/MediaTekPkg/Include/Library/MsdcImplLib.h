#ifndef _MSDC_IMPL_LIB_H_
#define _MSDC_IMPL_LIB_H_

typedef struct {
  UINT32 HostMask;
  BOOLEAN ReadOnly;
  UINT32 MaxClockHz;
  BOOLEAN UseTop;
  UINT32 MsdcPadTuneReg;
  UINT32 TuningStep[2];
  BOOLEAN AsyncFifo;
  BOOLEAN BusyCheck;
  BOOLEAN StopClkFix;
  BOOLEAN EnhanceRx;
} MSDC_PLATFORM_INFO;

EFI_STATUS
GetSourceClockRate (
  UINT32 Index,
  UINTN *Hz
  );

EFI_STATUS
SourceClockControl (
  UINT32 Index,
  BOOLEAN Enable
  );

EFI_STATUS
ClockControl (
  UINT32 Index,
  BOOLEAN Enable
  );

EFI_STATUS
PowerControl (
  UINT32 Index,
  BOOLEAN Enable
  );

EFI_STATUS
InitGpio (
  UINT32 Index
  );

EFI_STATUS
GetCardPresent (UINT32 Index, BOOLEAN *Present);

extern MSDC_PLATFORM_INFO gPlatformInfo;

#endif /* _MSDC_IMPL_LIB_H_ */