// SPDX-License-Identifier: BSD-2-Clause-Patent
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <wdf.h>
#include <initguid.h>
#include <gnssdriver.h>
#include "Receiver.h"

EVT_WDF_DRIVER_DEVICE_ADD GnDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY GnD0Entry;
EVT_WDF_DEVICE_D0_EXIT GnD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL GnRemoved;
EVT_WDF_DEVICE_FILE_CREATE GnFileCreate;
EVT_WDF_FILE_CLEANUP GnFileCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GnIoctl;
EVT_WDF_TIMER GnPoll;

typedef struct {
  WDFDEVICE Device;
  WDFIOTARGET Receiver;
  WDFWAITLOCK Gate;
  WDFQUEUE Waiters;
  WDFTIMER Timer;
  WDFFILEOBJECT Owner;
  BOOLEAN Ready,Enabled,PlatformKnown,Active,Faulted,HaveEvent,FinalSeen;
  BOOLEAN DrainPending;
  NTSTATUS DrainStatus;
  ULONGLONG Lease,LastLease,Generation,Sequence,LastClock,EventClock;
  GNSS_FIXSESSION_PARAM Session;
  MTK_RX_INFO Info;
  MTK_RX_FIX Input;
  GNSS_EVENT_2 Event;
} GN_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GN_DEVICE,GnContext)
enum { GN_MAX_PENDING_FIX_REQUESTS = 8 };

static ULONGLONG GnNow(void) {
  FILETIME F; GetSystemTimeAsFileTime(&F);
  return ((ULONGLONG)F.dwHighDateTime<<32)|F.dwLowDateTime;
}
static void GnBits(double *P,uint64_t V) { RtlCopyMemory(P,&V,sizeof(V)); }
static unsigned GnFinite(uint64_t V) { return (V&UINT64_C(0x7ff0000000000000))!=UINT64_C(0x7ff0000000000000); }
static unsigned GnNonnegative(uint64_t V) { return GnFinite(V) && (!(V>>63) || !(V<<1)); }
static unsigned GnFloat(uint32_t V) { return (V&0x7f800000U)!=0x7f800000U; }
static double GnFloatDouble(uint32_t V) { float F; RtlCopyMemory(&F,&V,sizeof(F)); return (double)F; }
static BOOLEAN GnTimestamp(int64_t Ms,ULONGLONG *Stamp) {
  const ULONGLONG Offset=UINT64_C(116444736000000000); ULONGLONG Magnitude;
  if (Ms<0) {
    Magnitude=0U-(ULONGLONG)Ms;
    if (Magnitude>Offset/10000U) return FALSE;
    *Stamp=Offset-Magnitude*10000U;
  } else {
    if ((ULONGLONG)Ms>(UINT64_MAX-Offset)/10000U) return FALSE;
    *Stamp=Offset+(ULONGLONG)Ms*10000U;
  }
  return TRUE;
}
static BOOLEAN GnFresh(ULONGLONG Stamp,ULONGLONG Now) {
  return Stamp>Now?Stamp-Now<=MTK_RX_FUTURE_MS*UINT64_C(10000):Now-Stamp<=MTK_RX_MAX_AGE_MS*UINT64_C(10000);
}
/* Validate the entire provider record before touching the published event. */
static NTSTATUS GnMap(const MTK_RX_FIX *R,ULONGLONG Generation,ULONGLONG Lease,
    ULONGLONG Sequence,ULONG Id,ULONG Details,ULONGLONG Now,GNSS_EVENT_2 *E) {
  const MTK_GNSS_LOCATION *L=&R->Location; ULONGLONG Stamp;
  GNSS_FIXDATA_2 *F; GNSS_FIXDATA_ACCURACY_2 *A; ULONG I,J,Key;
  if (R->Size!=sizeof(*R) || R->Version!=MTK_RX_VERSION || (R->Flags&~63U) ||
      R->SatelliteCount>GNSS_MAXSATELLITE || (!R->Sequence) || (L->Flags&~255U) ||
      (L->Reserved || L->Reserved2) || (L->ElapsedFlags&~3U)) return STATUS_DATA_ERROR;
  if (R->Generation!=Generation || R->Lease!=Lease || R->Sequence<=Sequence) return STATUS_NO_MORE_ENTRIES;
  if (!(R->Flags&MTK_RX_UTC) || !(L->Flags&1U)) return STATUS_NO_MORE_ENTRIES;
  if (!GnTimestamp(L->UtcMilliseconds,&Stamp)) return STATUS_DATA_ERROR;
  if (!GnFresh(Stamp,Now)) return STATUS_NO_MORE_ENTRIES;
  if (!GnFinite(L->LatitudeBits) || (L->LatitudeBits&INT64_MAX)>UINT64_C(0x4056800000000000) ||
      !GnFinite(L->LongitudeBits) || (L->LongitudeBits&INT64_MAX)>UINT64_C(0x4066800000000000) ||
      ((L->Flags&2U) && !GnFinite(L->AltitudeBits)) ||
      ((L->Flags&4U) && !GnNonnegative(L->SpeedBits)) ||
      ((L->Flags&8U) && (!GnNonnegative(L->BearingBits) || (L->BearingBits&INT64_MAX)>=UINT64_C(0x4076800000000000))) ||
      ((R->Flags&MTK_RX_MSL) && !GnFinite(R->MslAltitudeBits)) ||
      ((R->Flags&MTK_RX_HDOP) && !GnNonnegative(R->HdopBits)) ||
      ((R->Flags&MTK_RX_HORIZONTAL_95) && !GnNonnegative(R->HorizontalAccuracy95Bits)) ||
      (!(R->Flags&MTK_RX_SATELLITES) && R->SatelliteCount)) return STATUS_DATA_ERROR;
  for (I=0;I<R->SatelliteCount;I++) {
    const MTK_GNSS_SATELLITE *S=&R->Satellites[I];
    if (S->Svid<=0 || S->Svid>32767 || S->Constellation<1 || S->Constellation>7 || (S->Flags&~15U) ||
        !GnFloat(S->Cn0Bits) || (S->Cn0Bits>>31 && (S->Cn0Bits<<1)) ||
        !GnFloat(S->ElevationBits) || (S->ElevationBits&0x7fffffffU)>0x42b40000U ||
        !GnFloat(S->AzimuthBits) || (S->AzimuthBits>>31 && (S->AzimuthBits<<1)) || (S->AzimuthBits&0x7fffffffU)>0x43b40000U) return STATUS_DATA_ERROR;
  }
  RtlZeroMemory(E,sizeof(*E)); E->Size=sizeof(*E); E->Version=GNSS_DRIVER_VERSION_6;
  E->EventType=GNSS_Event_FixAvailable_2; E->EventDataSize=sizeof(E->FixData2);
  F=&E->FixData2; F->Size=sizeof(*F); F->Version=GNSS_DRIVER_VERSION_6; F->FixSessionID=Id;
  F->FixTimeStamp.dwLowDateTime=(DWORD)Stamp; F->FixTimeStamp.dwHighDateTime=(DWORD)(Stamp>>32);
  F->IsFinalFix=(R->Flags&MTK_RX_FINAL)!=0; F->FixStatus=STATUS_SUCCESS; F->FixLevelOfDetails=GNSS_FIXDETAIL_BASIC;
  F->BasicData.Size=sizeof(F->BasicData); F->BasicData.Version=GNSS_DRIVER_VERSION_6;
  GnBits(&F->BasicData.Latitude,L->LatitudeBits); GnBits(&F->BasicData.Longitude,L->LongitudeBits);
  GnBits(&F->BasicData.Altitude,(R->Flags&MTK_RX_MSL)?R->MslAltitudeBits:MTK_RX_NAN);
  GnBits(&F->BasicData.AltitudeEllipsoid,(L->Flags&2U)?L->AltitudeBits:MTK_RX_NAN);
  GnBits(&F->BasicData.Speed,(L->Flags&4U)?L->SpeedBits:MTK_RX_NAN);
  GnBits(&F->BasicData.Heading,(L->Flags&8U)?L->BearingBits:MTK_RX_NAN);
  A=&F->AccuracyData; A->Size=sizeof(*A); A->Version=GNSS_DRIVER_VERSION_6;
  GnBits(&A->HorizontalAccuracy,MTK_RX_NAN); GnBits(&A->HorizontalErrorMajorAxis,MTK_RX_NAN);
  GnBits(&A->HorizontalErrorMinorAxis,MTK_RX_NAN); GnBits(&A->HorizontalErrorAngle,MTK_RX_NAN);
  GnBits(&A->PositionDilutionOfPrecision,MTK_RX_NAN); GnBits(&A->HorizontalDilutionOfPrecision,MTK_RX_NAN);
  GnBits(&A->VerticalDilutionOfPrecision,MTK_RX_NAN); GnBits(&A->GeometricDilutionOfPrecision,MTK_RX_NAN);
  GnBits(&A->TimeDilutionOfPrecision,MTK_RX_NAN);
  if ((Details&GNSS_FIXDETAIL_ACCURACY) && (R->Flags&(MTK_RX_HDOP|MTK_RX_HORIZONTAL_95))) {
    F->FixLevelOfDetails|=GNSS_FIXDETAIL_ACCURACY;
    if (R->Flags&MTK_RX_HDOP) GnBits(&A->HorizontalDilutionOfPrecision,R->HdopBits);
    if (R->Flags&MTK_RX_HORIZONTAL_95) GnBits(&A->HorizontalAccuracy,R->HorizontalAccuracy95Bits);
  }
  F->SatelliteData.Size=sizeof(F->SatelliteData); F->SatelliteData.Version=GNSS_DRIVER_VERSION_6;
  if ((Details&GNSS_FIXDETAIL_SATELLITE) && (R->Flags&MTK_RX_SATELLITES)) {
    F->FixLevelOfDetails|=GNSS_FIXDETAIL_SATELLITE;
    for (I=0;I<R->SatelliteCount;I++) {
      const MTK_GNSS_SATELLITE *S=&R->Satellites[I]; GNSS_SATELLITEINFO *O;
      Key=((ULONG)S->Constellation<<16)|(ULONG)S->Svid;
      for (J=0;J<F->SatelliteData.SatelliteCount;J++) if (F->SatelliteData.SatelliteArray[J].SatelliteId==Key) break;
      O=&F->SatelliteData.SatelliteArray[J];
      if (J==F->SatelliteData.SatelliteCount || GnFloatDouble(S->Cn0Bits)>O->SignalToNoiseRatio) {
        O->SatelliteId=Key; O->Elevation=GnFloatDouble(S->ElevationBits);
        O->Azimuth=GnFloatDouble(S->AzimuthBits); O->SignalToNoiseRatio=GnFloatDouble(S->Cn0Bits);
      }
      O->UsedInPositiong|=(S->Flags&4U)!=0;
      if (J==F->SatelliteData.SatelliteCount) F->SatelliteData.SatelliteCount++;
    }
  }
  return STATUS_SUCCESS;
}
static void GnClear(GN_DEVICE *D) {
  D->HaveEvent=D->FinalSeen=FALSE; D->Sequence=D->EventClock=0;
  RtlSecureZeroMemory(&D->Event,sizeof(D->Event)); RtlSecureZeroMemory(&D->Input,sizeof(D->Input));
}
static void GnQueueDrain(GN_DEVICE *D,NTSTATUS Status) {
  D->DrainStatus=Status;
  D->DrainPending=TRUE;
}
static void GnDrainPending(GN_DEVICE *D) {
  for (;;) {
    WDFREQUEST Requests[GN_MAX_PENDING_FIX_REQUESTS];
    ULONG Count=0,I; NTSTATUS Status;
    WdfWaitLockAcquire(D->Gate,NULL);
    if (!D->DrainPending) { WdfWaitLockRelease(D->Gate); return; }
    Status=D->DrainStatus;
    while (Count<RTL_NUMBER_OF(Requests) &&
           NT_SUCCESS(WdfIoQueueRetrieveNextRequest(D->Waiters,&Requests[Count]))) Count++;
    if (Count<RTL_NUMBER_OF(Requests)) D->DrainPending=FALSE;
    WdfWaitLockRelease(D->Gate);
    for (I=0;I<Count;I++) WdfRequestComplete(Requests[I],Status);
    if (Count<RTL_NUMBER_OF(Requests)) return;
  }
}
static NTSTATUS GnLower(GN_DEVICE *D,ULONG Code,PVOID Input,ULONG InBytes,PVOID Output,ULONG OutBytes) {
  WDF_MEMORY_DESCRIPTOR In,Out; WDF_REQUEST_SEND_OPTIONS Options; ULONG_PTR Bytes=0; NTSTATUS S;
  if (!D->Receiver) return STATUS_DEVICE_NOT_READY;
  if (InBytes) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&In,Input,InBytes);
  if (OutBytes) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Out,Output,OutBytes);
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_MS(500));
  S=WdfIoTargetSendIoctlSynchronously(D->Receiver,NULL,Code,InBytes?&In:NULL,OutBytes?&Out:NULL,&Options,&Bytes);
  if (NT_SUCCESS(S) && Bytes!=OutBytes) return STATUS_DATA_ERROR;
  return S;
}
static NTSTATUS GnLocation(GN_DEVICE *D,BOOLEAN Enable) {
  struct { ULONG Size,Version,Type,Reserved,Bytes; UCHAR Unused[512]; BOOL Value; } C;
  RtlZeroMemory(&C,sizeof(C)); C.Size=sizeof(C); C.Version=GNSS_DRIVER_VERSION_6;
  C.Type=GNSS_SetLocationServiceEnabled; C.Bytes=sizeof(C.Value); C.Value=Enable;
  return GnLower(D,MTK_RX_COMMAND,&C,sizeof(C),NULL,0);
}
static void GnQuarantine(GN_DEVICE *D,NTSTATUS S) {
  D->Faulted=TRUE; D->Active=FALSE; WdfTimerStop(D->Timer,FALSE); GnClear(D); GnQueueDrain(D,S);
  /* An uncertain START may have no lease. Disable is owner-wide at the lower
   * child, so it can still stop RF. Never turn that failure into ready/success. */
  if (D->Ready) (void)GnLocation(D,FALSE);
}
static NTSTATUS GnStop(GN_DEVICE *D,NTSTATUS Pending) {
  NTSTATUS S=STATUS_SUCCESS;
  WdfTimerStop(D->Timer,FALSE);
  if (D->Active || D->Lease) S=GnLower(D,MTK_RX_STOP,&D->Lease,sizeof(D->Lease),NULL,0);
  D->Active=FALSE; GnClear(D); GnQueueDrain(D,Pending);
  if (NT_SUCCESS(S)) D->Lease=0;
  else GnQuarantine(D,S);
  return S;
}
static BOOLEAN GnCachedFresh(const GN_DEVICE *D,ULONGLONG Clock) {
  ULONGLONG Stamp=((ULONGLONG)D->Event.FixData2.FixTimeStamp.dwHighDateTime<<32)|
    D->Event.FixData2.FixTimeStamp.dwLowDateTime;
  return Clock>=D->EventClock && Clock-D->EventClock<=MTK_RX_MAX_AGE_MS && GnFresh(Stamp,GnNow());
}
static void GnDiscardEvent(GN_DEVICE *D) {
  D->HaveEvent=FALSE; D->EventClock=0; RtlSecureZeroMemory(&D->Event,sizeof(D->Event));
}
static NTSTATUS GnDeliver(GN_DEVICE *D,WDFREQUEST R,ULONG_PTR *Bytes) {
  PVOID Out; NTSTATUS S;
  if (!D->HaveEvent) return STATUS_NO_MORE_ENTRIES;
  if (!GnCachedFresh(D,GetTickCount64())) {
    GnDiscardEvent(D);
    return D->FinalSeen?STATUS_IO_TIMEOUT:STATUS_NO_MORE_ENTRIES;
  }
  S=WdfRequestRetrieveOutputBuffer(R,sizeof(D->Event),&Out,NULL);
  if (!NT_SUCCESS(S)) return S;
  RtlCopyMemory(Out,&D->Event,sizeof(D->Event)); *Bytes=sizeof(D->Event);
  GnDiscardEvent(D);
  return STATUS_SUCCESS;
}
VOID GnPoll(WDFTIMER Timer) {
  GN_DEVICE *D=GnContext(WdfTimerGetParentObject(Timer)); NTSTATUS S;
  WDFREQUEST CompleteRequest=NULL; ULONG_PTR CompleteBytes=0; NTSTATUS CompleteStatus=STATUS_SUCCESS;
  ULONG_PTR Bytes; ULONGLONG Clock;
  WdfWaitLockAcquire(D->Gate,NULL);
  if (!D->Ready || !D->Enabled || !D->Active || D->Faulted || D->FinalSeen) goto Done;
  Clock=GetTickCount64();
  if (Clock<D->LastClock) { GnQuarantine(D,STATUS_DATA_ERROR); goto Done; }
  D->LastClock=Clock;
  // The adapter may register GET after the fix arrives. A later poll must
  // not replace a fresh, undelivered result for this session.
  if (D->HaveEvent) {
    if (GnCachedFresh(D,Clock)) goto Again;
    GnDiscardEvent(D);
  }
  RtlSecureZeroMemory(&D->Input,sizeof(D->Input));
  S=GnLower(D,MTK_RX_POLL,&D->Lease,sizeof(D->Lease),&D->Input,sizeof(D->Input));
  if (S==STATUS_NO_MORE_ENTRIES) goto Again;
  if (!NT_SUCCESS(S)) { GnQuarantine(D,S); goto Done; }
  S=GnMap(&D->Input,D->Info.Generation,D->Lease,D->Sequence,D->Session.FixSessionID,D->Session.FixLevelOfDetails,GnNow(),&D->Event);
  if (S==STATUS_NO_MORE_ENTRIES) goto Again;
  if (!NT_SUCCESS(S)) { GnQuarantine(D,S); goto Done; }
  D->Sequence=D->Input.Sequence; D->EventClock=Clock; D->HaveEvent=TRUE; D->FinalSeen=D->Event.FixData2.IsFinalFix!=0;
  RtlSecureZeroMemory(&D->Input,sizeof(D->Input));
  if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(D->Waiters,&CompleteRequest))) {
    Bytes=0; CompleteStatus=GnDeliver(D,CompleteRequest,&Bytes); CompleteBytes=Bytes;
  }
  // One fix satisfies one queued GET. After the final fix no future poll
  // can satisfy the other requests, so complete them after dropping Gate.
  if (D->FinalSeen) GnQueueDrain(D,STATUS_NO_MORE_ENTRIES);
Again:
  RtlSecureZeroMemory(&D->Input,sizeof(D->Input));
  if (D->Active && !D->FinalSeen && !D->Faulted) WdfTimerStart(D->Timer,WDF_REL_TIMEOUT_IN_MS(200));
Done:
  WdfWaitLockRelease(D->Gate);
  if (CompleteRequest) WdfRequestCompleteWithInformation(CompleteRequest,CompleteStatus,CompleteBytes);
  GnDrainPending(D);
}

static BOOLEAN GnSessionValid(const GNSS_FIXSESSION_PARAM *P,SIZE_T Length) {
  return Length==sizeof(*P) && P->Size==sizeof(*P) && P->Version==GNSS_DRIVER_VERSION_6 &&
    P->SessionType==GNSS_FixSession_SingleShot && !(P->FixLevelOfDetails&~7U) &&
    P->HorizontalConfidence<=100U && P->SingleShotParam.Size==sizeof(P->SingleShotParam) &&
    P->SingleShotParam.Version==GNSS_DRIVER_VERSION_6;
}
static NTSTATUS GnCommand(GN_DEVICE *D,GNSS_DRIVERCOMMAND_PARAM *P,SIZE_T Length) {
  ULONG Prefix=FIELD_OFFSET(GNSS_DRIVERCOMMAND_PARAM,CommandData); BOOL Enable; NTSTATUS S;
  if (Length<Prefix || Length>Prefix+256U+sizeof(*P)-Prefix || P->Size!=Length || P->Version!=GNSS_DRIVER_VERSION_6 ||
      P->Reserved || P->CommandDataSize>Length-Prefix || Length-Prefix-P->CommandDataSize>sizeof(*P)-Prefix) return STATUS_INVALID_PARAMETER;
  if (P->CommandType==GNSS_SetLocationServiceEnabled) {
    if (P->CommandDataSize!=sizeof(Enable)) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&Enable,P->CommandData,sizeof(Enable));
    if (Enable!=FALSE && Enable!=TRUE) return STATUS_INVALID_PARAMETER;
    if (Enable && D->Faulted) return STATUS_DEVICE_NOT_READY;
    if (!Enable) {
      D->Enabled=FALSE; S=GnStop(D,STATUS_CANCELLED);
      if (!NT_SUCCESS(S)) return S;
    }
    S=GnLower(D,MTK_RX_COMMAND,P,(ULONG)Length,NULL,0);
    if (NT_SUCCESS(S)) D->Enabled=(BOOLEAN)Enable;
    else GnQuarantine(D,S);
    return S;
  }
  if (P->CommandType!=GNSS_ResetEngine && P->CommandType!=GNSS_ClearAgnssData) return STATUS_NOT_SUPPORTED;
  if (D->Faulted && P->CommandType!=GNSS_ResetEngine) return STATUS_DEVICE_NOT_READY;
  S=GnStop(D,STATUS_CANCELLED);
  if (!NT_SUCCESS(S) && P->CommandType!=GNSS_ResetEngine) return S;
  S=GnLower(D,MTK_RX_COMMAND,P,(ULONG)Length,NULL,0);
  if (NT_SUCCESS(S)) S=GnLocation(D,D->Enabled);
  if (NT_SUCCESS(S)) { D->Lease=0; D->Faulted=FALSE; GnClear(D); }
  else GnQuarantine(D,S);
  return S;
}
VOID GnIoctl(WDFQUEUE Queue,WDFREQUEST R,size_t OutputLength,size_t InputLength,ULONG Code) {
  GN_DEVICE *D=GnContext(WdfIoQueueGetDevice(Queue)); NTSTATUS S=STATUS_INVALID_DEVICE_REQUEST;
  PVOID Input=NULL,Output=NULL; ULONG Id,Queued=0; ULONG_PTR Bytes=0; ULONGLONG Lease=0;
  GNSS_DEVICE_CAPABILITY *Cap; GNSS_PLATFORM_CAPABILITY *Platform; MTK_RX_MODIFY_PARAM Modify;
  UNREFERENCED_PARAMETER(OutputLength);
  WdfWaitLockAcquire(D->Gate,NULL);
  if (!D->Ready || !D->Owner || WdfRequestGetFileObject(R)!=D->Owner) { S=STATUS_DEVICE_NOT_READY; goto Done; }
  if (InputLength) { S=WdfRequestRetrieveInputBuffer(R,InputLength,&Input,NULL); if (!NT_SUCCESS(S)) goto Done; }
  switch (Code) {
    case IOCTL_GNSS_SEND_PLATFORM_CAPABILITY:
      if (InputLength!=sizeof(*Platform)) { S=STATUS_INVALID_PARAMETER; break; }
      Platform=Input;
      if (Platform->Size!=sizeof(*Platform) || Platform->Version<GNSS_DRIVER_VERSION_6 ||
          (Platform->SupportAgnssInjection!=FALSE && Platform->SupportAgnssInjection!=TRUE)) { S=STATUS_NOT_SUPPORTED; break; }
      D->PlatformKnown=TRUE; S=STATUS_SUCCESS; break;
    case IOCTL_GNSS_GET_DEVICE_CAPABILITY:
      if (InputLength) { S=STATUS_INVALID_PARAMETER; break; }
      S=WdfRequestRetrieveOutputBuffer(R,sizeof(*Cap),&Output,NULL); if (!NT_SUCCESS(S)) break;
      Cap=Output; RtlZeroMemory(Cap,sizeof(*Cap)); Cap->Size=sizeof(*Cap); Cap->Version=GNSS_DRIVER_VERSION_6;
      Bytes=sizeof(*Cap); break;
    case IOCTL_GNSS_GET_CHIPSETINFO:
      if (InputLength) { S=STATUS_INVALID_PARAMETER; break; }
      S=WdfRequestRetrieveOutputBuffer(R,sizeof(D->Info.Chipset),&Output,NULL); if (!NT_SUCCESS(S)) break;
      {
        GNSS_CHIPSETINFO *C=Output; ULONG I;
        RtlZeroMemory(C,sizeof(*C)); C->Size=sizeof(*C); C->Version=GNSS_DRIVER_VERSION_6;
        for (I=0;I<24 && D->Info.Chipset.ManufacturerID[I];I++) C->ManufacturerID[I]=D->Info.Chipset.ManufacturerID[I];
        for (I=0;I<24 && D->Info.Chipset.HardwareID[I];I++) C->HardwareID[I]=D->Info.Chipset.HardwareID[I];
        for (I=0;I<19 && D->Info.Chipset.FirmwareVersion[I];I++) C->FirmwareVersion[I]=D->Info.Chipset.FirmwareVersion[I];
      }
      Bytes=sizeof(D->Info.Chipset); break;
    case IOCTL_GNSS_SEND_DRIVERCOMMAND:
      if (!D->PlatformKnown || !Input) { S=STATUS_INVALID_DEVICE_STATE; break; }
      S=GnCommand(D,Input,InputLength); break;
    case IOCTL_GNSS_START_FIXSESSION:
    case IOCTL_GNSS_MODIFY_FIXSESSION:
      if (!D->PlatformKnown || !D->Enabled || D->Faulted) { S=STATUS_DEVICE_NOT_READY; break; }
      if (!Input || !GnSessionValid(Input,InputLength)) { S=STATUS_INVALID_PARAMETER; break; }
      if ((Code==IOCTL_GNSS_START_FIXSESSION && D->Active) ||
          (Code==IOCTL_GNSS_MODIFY_FIXSESSION && (!D->Active || D->Session.FixSessionID!=((GNSS_FIXSESSION_PARAM *)Input)->FixSessionID))) { S=STATUS_DEVICE_BUSY; break; }
      if (Code==IOCTL_GNSS_START_FIXSESSION) S=GnLower(D,MTK_RX_START,Input,(ULONG)InputLength,&Lease,sizeof(Lease));
      else {
        RtlZeroMemory(&Modify,sizeof(Modify)); Modify.Lease=D->Lease;
        RtlCopyMemory(&Modify.Session,Input,sizeof(Modify.Session));
        S=GnLower(D,MTK_RX_MODIFY,&Modify,sizeof(Modify),&Lease,sizeof(Lease));
      }
      if (!NT_SUCCESS(S)) { GnQuarantine(D,S); break; }
      if (!Lease || Lease<=D->LastLease) { S=STATUS_DATA_ERROR; GnQuarantine(D,S); break; }
      D->Lease=D->LastLease=Lease; D->Active=TRUE; GnClear(D);
      RtlCopyMemory(&D->Session,Input,sizeof(D->Session)); D->LastClock=GetTickCount64();
      WdfTimerStart(D->Timer,WDF_REL_TIMEOUT_IN_MS(1)); break;
    case IOCTL_GNSS_STOP_FIXSESSION:
      if (InputLength!=sizeof(GNSS_STOPFIXSESSION_PARAM) || ((GNSS_STOPFIXSESSION_PARAM *)Input)->Size!=InputLength ||
          ((GNSS_STOPFIXSESSION_PARAM *)Input)->Version!=GNSS_DRIVER_VERSION_6) { S=STATUS_INVALID_PARAMETER; break; }
      Id=((GNSS_STOPFIXSESSION_PARAM *)Input)->FixSessionID;
      if (!D->Active || Id!=D->Session.FixSessionID) { S=STATUS_INVALID_PARAMETER; break; }
      S=GnStop(D,STATUS_CANCELLED); break;
    case IOCTL_GNSS_GET_FIXDATA:
      if (InputLength!=sizeof(Id)) { S=STATUS_INVALID_PARAMETER; break; }
      RtlCopyMemory(&Id,Input,sizeof(Id));
      if (!D->Active || !D->Enabled || D->Faulted || Id!=D->Session.FixSessionID) { S=STATUS_INVALID_DEVICE_STATE; break; }
      S=WdfRequestRetrieveOutputBuffer(R,sizeof(D->Event),&Output,NULL); if (!NT_SUCCESS(S)) break;
      (void)WdfIoQueueGetState(D->Waiters,&Queued,NULL);
      if (Queued>=GN_MAX_PENDING_FIX_REQUESTS) { S=STATUS_DEVICE_BUSY; break; }
      S=GnDeliver(D,R,&Bytes);
      if (S!=STATUS_NO_MORE_ENTRIES) break;
      if (D->FinalSeen) { S=STATUS_NO_MORE_ENTRIES; break; }
      S=WdfRequestForwardToIoQueue(R,D->Waiters);
      if (NT_SUCCESS(S)) { WdfWaitLockRelease(D->Gate); GnDrainPending(D); return; }
      break;
    default:S=STATUS_NOT_SUPPORTED; break;
  }
Done:
  WdfWaitLockRelease(D->Gate); GnDrainPending(D); WdfRequestCompleteWithInformation(R,S,Bytes);
}
NTSTATUS GnD0Entry(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Previous) {
  GN_DEVICE *D=GnContext(Device); NTSTATUS S;
  UNREFERENCED_PARAMETER(Previous); WdfWaitLockAcquire(D->Gate,NULL);
  D->Ready=FALSE; D->Active=FALSE; D->Lease=0; GnClear(D); RtlZeroMemory(&D->Info,sizeof(D->Info));
  S=GnLower(D,MTK_RX_QUERY,NULL,0,&D->Info,sizeof(D->Info));
  if (!NT_SUCCESS(S)) goto Done;
  if (D->Info.Size!=sizeof(D->Info) || D->Info.Version!=MTK_RX_VERSION || D->Info.Format!=1 ||
      !D->Info.Generation || (D->Info.Commands&MTK_RX_MANDATORY_COMMANDS)!=MTK_RX_MANDATORY_COMMANDS ||
      D->Info.Chipset.Size!=sizeof(D->Info.Chipset) || D->Info.Chipset.Version!=GNSS_DRIVER_VERSION_6 ||
      D->Info.Chipset.ManufacturerID[24] || D->Info.Chipset.HardwareID[24] || D->Info.Chipset.FirmwareVersion[19]) { S=STATUS_DEVICE_CONFIGURATION_ERROR; goto Done; }
  S=GnLocation(D,FALSE);
  if (NT_SUCCESS(S) && D->Enabled) S=GnLocation(D,TRUE);
  if (NT_SUCCESS(S)) {
    if (D->Generation!=D->Info.Generation) D->LastLease=0;
    D->Generation=D->Info.Generation;
    D->Ready=TRUE; D->Faulted=FALSE;
  }
Done:
  WdfWaitLockRelease(D->Gate); return S;
}
NTSTATUS GnD0Exit(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Target) {
  GN_DEVICE *D=GnContext(Device); NTSTATUS S=STATUS_SUCCESS,T; BOOLEAN WasReady;
  UNREFERENCED_PARAMETER(Target);
  WdfWaitLockAcquire(D->Gate,NULL);
  WasReady=D->Ready;
  D->Ready=FALSE; D->Active=FALSE;
  WdfWaitLockRelease(D->Gate);
  // Close callback admission first, then wait with no lock the timer callback needs.
  WdfTimerStop(D->Timer,TRUE);
  WdfWaitLockAcquire(D->Gate,NULL);
  if (WasReady) {
    S=GnStop(D,STATUS_POWER_STATE_INVALID); T=GnLocation(D,FALSE);
    if (!NT_SUCCESS(T)) S=T;
  }
  D->Ready=FALSE; D->Active=FALSE; GnClear(D); GnQueueDrain(D,STATUS_POWER_STATE_INVALID);
  if (!NT_SUCCESS(S)) D->Faulted=TRUE;
  WdfWaitLockRelease(D->Gate); GnDrainPending(D); return S;
}
VOID GnRemoved(WDFDEVICE Device) {
  GN_DEVICE *D=GnContext(Device);
  WdfWaitLockAcquire(D->Gate,NULL);
  D->Ready=FALSE; D->Active=FALSE; D->Faulted=TRUE; GnClear(D);
  GnQueueDrain(D,STATUS_DEVICE_REMOVED);
  WdfWaitLockRelease(D->Gate);
  WdfTimerStop(D->Timer,TRUE);
  GnDrainPending(D);
}
VOID GnFileCreate(WDFDEVICE Device,WDFREQUEST R,WDFFILEOBJECT File) {
  GN_DEVICE *D=GnContext(Device); NTSTATUS S=STATUS_DEVICE_NOT_READY;
  WdfWaitLockAcquire(D->Gate,NULL);
  if (D->Ready && !D->Owner && !D->Faulted && !D->DrainPending) { D->Owner=File; D->PlatformKnown=FALSE; S=STATUS_SUCCESS; }
  WdfWaitLockRelease(D->Gate); WdfRequestComplete(R,S);
}
VOID GnFileCleanup(WDFFILEOBJECT File) {
  GN_DEVICE *D=GnContext(WdfFileObjectGetDevice(File)); NTSTATUS S;
  WdfWaitLockAcquire(D->Gate,NULL);
  if (D->Owner==File) {
    D->Enabled=FALSE;
    if (D->Ready) { (void)GnStop(D,STATUS_CANCELLED); S=GnLocation(D,FALSE); if (!NT_SUCCESS(S)) D->Faulted=TRUE; }
    GnClear(D); GnQueueDrain(D,STATUS_CANCELLED); D->Owner=NULL; D->PlatformKnown=FALSE;
  }
  WdfWaitLockRelease(D->Gate);
  WdfTimerStop(D->Timer,TRUE);
  GnDrainPending(D);
}
NTSTATUS GnDeviceAdd(WDFDRIVER Driver,PWDFDEVICE_INIT Init) {
  WDFDEVICE Device; GN_DEVICE *D; NTSTATUS S; WDF_OBJECT_ATTRIBUTES A;
  WDF_PNPPOWER_EVENT_CALLBACKS P; WDF_FILEOBJECT_CONFIG F; WDF_IO_QUEUE_CONFIG Q; WDF_TIMER_CONFIG T;
  UNREFERENCED_PARAMETER(Driver);
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&P); P.EvtDeviceD0Entry=GnD0Entry; P.EvtDeviceD0Exit=GnD0Exit; P.EvtDeviceSurpriseRemoval=GnRemoved;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init,&P);
  WDF_FILEOBJECT_CONFIG_INIT(&F,GnFileCreate,NULL,GnFileCleanup); WdfDeviceInitSetFileObjectConfig(Init,&F,WDF_NO_OBJECT_ATTRIBUTES);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,GN_DEVICE); A.ExecutionLevel=WdfExecutionLevelPassive; A.SynchronizationScope=WdfSynchronizationScopeNone;
  S=WdfDeviceCreate(&Init,&A,&Device); if (!NT_SUCCESS(S)) return S;
  D=GnContext(Device); D->Device=Device; D->Receiver=WdfDeviceGetIoTarget(Device);
  WDF_OBJECT_ATTRIBUTES_INIT(&A); A.ParentObject=Device;
  S=WdfWaitLockCreate(&A,&D->Gate); if (!NT_SUCCESS(S)) return S;
  WDF_IO_QUEUE_CONFIG_INIT(&Q,WdfIoQueueDispatchManual); Q.PowerManaged=WdfFalse;
  S=WdfIoQueueCreate(Device,&Q,WDF_NO_OBJECT_ATTRIBUTES,&D->Waiters); if (!NT_SUCCESS(S)) return S;
  WDF_TIMER_CONFIG_INIT(&T,GnPoll); T.AutomaticSerialization=FALSE;
  WDF_OBJECT_ATTRIBUTES_INIT(&A); A.ParentObject=Device; A.ExecutionLevel=WdfExecutionLevelPassive;
  S=WdfTimerCreate(&T,&A,&D->Timer); if (!NT_SUCCESS(S)) return S;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Q,WdfIoQueueDispatchSequential); Q.PowerManaged=WdfFalse; Q.EvtIoDeviceControl=GnIoctl;
  S=WdfIoQueueCreate(Device,&Q,WDF_NO_OBJECT_ATTRIBUTES,NULL); if (!NT_SUCCESS(S)) return S;
  return WdfDeviceCreateDeviceInterface(Device,&GUID_DEVINTERFACE_GNSS,NULL);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT Driver,PUNICODE_STRING Path) {
  WDF_DRIVER_CONFIG Config; WDF_DRIVER_CONFIG_INIT(&Config,GnDeviceAdd);
  return WdfDriverCreate(Driver,Path,WDF_NO_OBJECT_ATTRIBUTES,&Config,WDF_NO_HANDLE);
}

void __cdecl __security_init_cookie(void);
BOOL WINAPI GnDllMain(HINSTANCE Instance,DWORD Reason,LPVOID Reserved) {
  UNREFERENCED_PARAMETER(Instance); UNREFERENCED_PARAMETER(Reserved);
  if (Reason==DLL_PROCESS_ATTACH) __security_init_cookie();
  return TRUE;
}
