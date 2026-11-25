// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-only: execute the T70 runtime library; never access RTC/timer/MMIO.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../Library/UnavailableRealTimeClockLib/UnavailableRealTimeClockLib.c"
EFI_GUID gEfiRtPropertiesTableGuid = EFI_RT_PROPERTIES_TABLE_GUID;
EFI_GUID gFdtTableGuid={1},gEfiEventExitBootServicesGuid={2},gMediaTekPmicWrapperProtocolGuid={3};
EFI_GUID gEfiEventReadyToBootGuid={4},gEfiEventVirtualAddressChangeGuid={5},gEfiEventBeforeExitBootServicesGuid={7};
EFI_GUID gMtkPmicRtcRuntimeProtocolGuid={6};
EFI_DXE_SERVICES *gDS;
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) { return memcmp(A,B,N); }
VOID *EFIAPI ZeroMem(VOID *A,UINTN N) { return memset(A,0,N); }
BOOLEAN EFIAPI CompareGuid(CONST GUID *A, CONST GUID *B) { return memcmp(A,B,sizeof(*A)) == 0; }
UINTN EFIAPI MicroSecondDelay(UINTN N) { return N; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return 0; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 N) { return N; }
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *Start,UINT64 *End) { if(Start)*Start=0;if(End)*End=MAX_UINT64;return 1000000000; }
static unsigned Calls;
static EFI_STATUS Result;
static EFI_RT_PROPERTIES_TABLE *Published;
static EFI_STATUS EFIAPI Install(EFI_GUID *Guid, VOID *Table) {
  assert(CompareGuid(Guid,&gEfiRtPropertiesTableGuid));++Calls;
  if (!EFI_ERROR(Result)) Published=Table;
  return Result;
}
EFI_GUID gEfiCallerIdGuid = {0}; // Normally declared by EDK AutoGen.
// Execute the real runtime wrapper and its real date-validation helpers too.
#include "../../../../Mu_Basecore/EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.c"
#include "../../../../Mu_Basecore/EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClock.c"
EFI_GUID gEfiRealTimeClockArchProtocolGuid = {0};
EFI_BOOT_SERVICES *gBS;
static unsigned ProtocolCalls, VariableWrites;
EFI_STATUS EFIAPI EfiGetVariable(CHAR16 *Name, EFI_GUID *Guid, UINT32 *Attributes, UINTN *Size, VOID *Data) {
  (void)Name;(void)Guid;(void)Attributes;(void)Size;(void)Data;return EFI_NOT_FOUND;
}
EFI_STATUS EFIAPI EfiSetVariable(CHAR16 *Name, EFI_GUID *Guid, UINT32 Attributes, UINTN Size, VOID *Data) {
  (void)Name;(void)Guid;(void)Attributes;(void)Size;(void)Data;++VariableWrites;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InstallProtocol(EFI_HANDLE *Handle, ...) {
  (void)Handle;++ProtocolCalls;return EFI_SUCCESS;
}
static void WrapperContract(void) {
  EFI_BOOT_SERVICES Bs={0};EFI_RUNTIME_SERVICES Rt={0};EFI_SYSTEM_TABLE St={0};
  Bs.InstallConfigurationTable=Install;Bs.InstallMultipleProtocolInterfaces=InstallProtocol;
  St.BootServices=&Bs;St.RuntimeServices=&Rt;gBS=&Bs;
  Result=EFI_OUT_OF_RESOURCES;
  assert(InitializeRealTimeClock(NULL,&St)==EFI_OUT_OF_RESOURCES);
  assert(ProtocolCalls==0 && Rt.GetTime==NULL);
  Result=EFI_SUCCESS;
  assert(InitializeRealTimeClock(NULL,&St)==EFI_SUCCESS && ProtocolCalls==1);
  assert(Published->RuntimeServicesSupported==0x3ff0);
  assert(Rt.GetTime==GetTime && Rt.SetTime==SetTime);
  assert(Rt.GetWakeupTime==GetWakeupTime && Rt.SetWakeupTime==SetWakeupTime);
  EFI_TIME T,Expected;EFI_TIME_CAPABILITIES C,ExpectedC;
  memset(&T,0xa5,sizeof(T));Expected=T;memset(&C,0x5a,sizeof(C));ExpectedC=C;
  // Unlike the library, the real parent writes these two fields before calling
  // LibGetTime. A failed GetTime must not be described as output-unchanged.
  Expected.TimeZone=EFI_UNSPECIFIED_TIMEZONE;Expected.Daylight=0;
  assert(Rt.GetTime(&T,&C)==EFI_UNSUPPORTED);
  assert(!memcmp(&T,&Expected,sizeof(T)) && !memcmp(&C,&ExpectedC,sizeof(C)));
  assert(Rt.GetTime(NULL,&C)==EFI_INVALID_PARAMETER);
  BOOLEAN Enabled=1,Pending=1;
  memset(&T,0xa5,sizeof(T));
  assert(Rt.GetWakeupTime(&Enabled,&Pending,&T)==EFI_UNSUPPORTED);
  assert(!memcmp(&T,&Expected,sizeof(T)) && Enabled==1 && Pending==1);
  assert(Rt.GetWakeupTime(NULL,&Pending,&T)==EFI_INVALID_PARAMETER);
  assert(Rt.SetTime(NULL)==EFI_INVALID_PARAMETER);
  memset(&T,0,sizeof(T));T.Year=2026;T.Month=11;T.Day=18;T.TimeZone=0;
  assert(Rt.SetTime(&T)==EFI_UNSUPPORTED && VariableWrites==0);
  T.Day=0;assert(Rt.SetTime(&T)==EFI_INVALID_PARAMETER && VariableWrites==0);
  assert(Rt.SetWakeupTime(FALSE,NULL)==EFI_UNSUPPORTED);
  assert(Rt.SetWakeupTime(TRUE,&T)==EFI_UNSUPPORTED);
}
int main(void) {
  EFI_BOOT_SERVICES Bs={0}; EFI_SYSTEM_TABLE St={0}; EFI_CONFIGURATION_TABLE Ct={0};
  EFI_TIME Time, Before; EFI_TIME_CAPABILITIES Caps, OldCaps;
  memset(&Time,0xa5,sizeof(Time));Before=Time;memset(&Caps,0x5a,sizeof(Caps));OldCaps=Caps;
  BOOLEAN Enabled=1,Pending=1;
  assert(LibGetTime(&Time,&Caps)==EFI_UNSUPPORTED);
  assert(!memcmp(&Time,&Before,sizeof(Time)) && !memcmp(&Caps,&OldCaps,sizeof(Caps)));
  assert(LibGetTime(NULL,NULL)==EFI_UNSUPPORTED);
  assert(LibSetTime(&Time)==EFI_UNSUPPORTED && LibSetTime(NULL)==EFI_UNSUPPORTED);
  assert(LibGetWakeupTime(&Enabled,&Pending,&Time)==EFI_UNSUPPORTED && Enabled==1 && Pending==1);
  assert(!memcmp(&Time,&Before,sizeof(Time)));
  assert(LibGetWakeupTime(NULL,NULL,NULL)==EFI_UNSUPPORTED);
  assert(LibSetWakeupTime(0,NULL)==EFI_UNSUPPORTED && LibSetWakeupTime(1,&Time)==EFI_UNSUPPORTED);
  assert(LibRtcInitialize(NULL,NULL)==EFI_INVALID_PARAMETER);
  assert(LibRtcInitialize(NULL,&St)==EFI_INVALID_PARAMETER);
  St.BootServices=&Bs;Bs.InstallConfigurationTable=Install;
  assert(LibRtcInitialize(NULL,&St)==EFI_SUCCESS && Calls==1);
  assert(Published && Published->Version==1 && Published->Length==8);
  assert(Published->RuntimeServicesSupported==0x3ff0);
  EFI_RT_PROPERTIES_TABLE Previous={1,8,0x15bf};
  Ct.VendorGuid=gEfiRtPropertiesTableGuid;Ct.VendorTable=&Previous;
  St.NumberOfTableEntries=1;St.ConfigurationTable=&Ct;
  assert(LibRtcInitialize(NULL,&St)==EFI_SUCCESS && Published->RuntimeServicesSupported==0x15b0);
  assert(Previous.RuntimeServicesSupported==0x15bf);
  Previous.Length=7;unsigned N=Calls;
  assert(LibRtcInitialize(NULL,&St)==EFI_COMPROMISED_DATA && Calls==N);
  Previous.Length=8;Previous.Version=2;
  assert(LibRtcInitialize(NULL,&St)==EFI_COMPROMISED_DATA && Calls==N);
  Previous.Version=1;Result=EFI_OUT_OF_RESOURCES;
  assert(LibRtcInitialize(NULL,&St)==EFI_OUT_OF_RESOURCES);
  assert(Previous.RuntimeServicesSupported==0x15bf);
  St.ConfigurationTable=NULL;
  assert(LibRtcInitialize(NULL,&St)==EFI_INVALID_PARAMETER);
  WrapperContract();
  puts("T70 unavailable RTC library + actual runtime wrapper: unsupported services, library output preservation, wrapper timezone/daylight writes, property masks and publication failures passed (host only)");
  return 0;
}
