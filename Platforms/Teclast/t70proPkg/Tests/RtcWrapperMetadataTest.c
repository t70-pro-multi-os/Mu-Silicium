// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the shared production wrapper, not a replacement implementation.
#include "MediaTekTest.h"
EFI_GUID gEfiCallerIdGuid={0},gEfiRealTimeClockArchProtocolGuid={0};
#include "../../../../Mu_Basecore/EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.c"
#include "../../../../Mu_Basecore/EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClock.c"
EFI_BOOT_SERVICES *gBS;
static EFI_STATUS ProviderStatus,VariableStatus;
static unsigned ProviderCalls,VariableCalls;
static NON_VOLATILE_TIME_SETTINGS Persisted;
EFI_STATUS EFIAPI LibGetTime(EFI_TIME *T,EFI_TIME_CAPABILITIES *C) {
  T->Year=2026;T->Month=11;T->Day=19;T->Hour=1;T->Minute=2;T->Second=3;T->Nanosecond=0;
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI LibSetTime(EFI_TIME *T) { ProviderCalls++;return ProviderStatus; }
EFI_STATUS EFIAPI LibGetWakeupTime(BOOLEAN *E,BOOLEAN *P,EFI_TIME *T) { return EFI_UNSUPPORTED; }
EFI_STATUS EFIAPI LibSetWakeupTime(BOOLEAN E,EFI_TIME *T) { return EFI_UNSUPPORTED; }
EFI_STATUS EFIAPI LibRtcInitialize(EFI_HANDLE H,EFI_SYSTEM_TABLE *S) { return EFI_SUCCESS; }
EFI_STATUS EFIAPI EfiGetVariable(CHAR16 *N,EFI_GUID *G,UINT32 *A,UINTN *S,VOID *D) { return EFI_NOT_FOUND; }
EFI_STATUS EFIAPI EfiSetVariable(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN S,VOID *D) {
  VariableCalls++;CHECK(S==sizeof(Persisted));
  if(!EFI_ERROR(VariableStatus))memcpy(&Persisted,D,S);
  return VariableStatus;
}
static void Readback(INT16 Zone,UINT8 Daylight) {
  EFI_TIME T;memset(&T,0xa5,sizeof(T));
  CHECK(GetTime(&T,NULL)==EFI_SUCCESS);
  CHECK(T.TimeZone==Zone&&T.Daylight==Daylight&&T.Year==2026&&T.Month==11&&T.Day==19);
}
int main(void) {
  EFI_TIME T={2026,11,19,1,2,3,0,0,120,(EFI_TIME_ADJUST_DAYLIGHT|EFI_TIME_IN_DAYLIGHT),0},Before=T;
  EFI_STATUS Failures[]={EFI_UNSUPPORTED,EFI_DEVICE_ERROR,EFI_NOT_READY,EFI_TIMEOUT};unsigned I,N;
  mTimeSettings.TimeZone=60;mTimeSettings.Daylight=EFI_TIME_ADJUST_DAYLIGHT;Persisted=mTimeSettings;
  for(I=0;I<sizeof(Failures)/sizeof(Failures[0]);I++) {
    ProviderStatus=Failures[I];CHECK(SetTime(&T)==Failures[I]);
    CHECK(VariableCalls==0);Readback(60,EFI_TIME_ADJUST_DAYLIGHT);
    CHECK(!memcmp(&T,&Before,sizeof(T)));
  }
  N=ProviderCalls;T.Day=0;
  CHECK(SetTime(&T)==EFI_INVALID_PARAMETER&&ProviderCalls==N&&VariableCalls==0);
  CHECK(SetTime(NULL)==EFI_INVALID_PARAMETER&&ProviderCalls==N);Readback(60,EFI_TIME_ADJUST_DAYLIGHT);T=Before;
  ProviderStatus=EFI_SUCCESS;VariableStatus=EFI_WRITE_PROTECTED;N=ProviderCalls;
  CHECK(SetTime(&T)==EFI_DEVICE_ERROR&&VariableCalls==1&&ProviderCalls==N+1);Readback(60,EFI_TIME_ADJUST_DAYLIGHT);
  CHECK(Persisted.TimeZone==60&&Persisted.Daylight==EFI_TIME_ADJUST_DAYLIGHT);
  VariableStatus=EFI_UNSUPPORTED;CHECK(SetTime(&T)==EFI_DEVICE_ERROR&&VariableCalls==2);
  Readback(60,EFI_TIME_ADJUST_DAYLIGHT);
  CHECK(Persisted.TimeZone==60&&Persisted.Daylight==EFI_TIME_ADJUST_DAYLIGHT);
  VariableStatus=EFI_SUCCESS;CHECK(SetTime(&T)==EFI_SUCCESS&&VariableCalls==3);
  Readback(120,(EFI_TIME_ADJUST_DAYLIGHT|EFI_TIME_IN_DAYLIGHT));
  CHECK(Persisted.TimeZone==120&&Persisted.Daylight==(EFI_TIME_ADJUST_DAYLIGHT|EFI_TIME_IN_DAYLIGHT));
  VariableStatus=EFI_UNSUPPORTED;
  CHECK(SetTime(&T)==EFI_SUCCESS&&VariableCalls==3);Readback(120,(EFI_TIME_ADJUST_DAYLIGHT|EFI_TIME_IN_DAYLIGHT));
  VariableStatus=EFI_SUCCESS;T.TimeZone=EFI_UNSPECIFIED_TIMEZONE;T.Daylight=0;
  CHECK(SetTime(&T)==EFI_SUCCESS&&VariableCalls==4);Readback(EFI_UNSPECIFIED_TIMEZONE,0);
  printf("RTC wrapper metadata: %u checks passed\n",Checks);return 0;
}
