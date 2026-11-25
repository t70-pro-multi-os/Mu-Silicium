// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwProbe.h"
#include "AwBoard.h"
#include "AwIdentity.h"
typedef struct {const T70_AW_STATUS_IO *Io;int32_t Status;} ID_CONTEXT;
static int ReadId(void *Context,uint16_t Register,uint16_t *Value) {
  ID_CONTEXT *C=Context;
  C->Status=C->Io->Read(C->Io->Context,Register,Value);
  return C->Status==0;
}
T70_AW_PROBE_RESULT T70Aw88261Probe(uint64_t Base,uint16_t Address,uint16_t Channel,
                                   const T70_AW_STATUS_IO *Io,
                                   T70_AW_PROBE_SNAPSHOT *Snapshot) {
  T70_AW_PROBE_RESULT R={T70AwProbeInvalid,0};
  T70_AW_PROBE_SNAPSHOT S={0};
  if(!Snapshot || !Io || !Io->Read || !T70AwBoardTupleValid(Base,Address,Channel))return R;
  ID_CONTEXT C={Io,0};T70_AW_REGISTER_IO Identity={&C,ReadId,NULL};
  if(!T70Aw88261Identify(&Identity,&S.ChipId)) {
    R.TransportStatus=C.Status;
    R.Code=C.Status?T70AwProbeIdReadFailed:T70AwProbeWrongId;return R;
  }
  T70_AW_STATUS_RESULT Status=T70Aw88261ReadClockStatus(Io,&S.Clock);
  if(Status.Code!=T70AwStatusRead) {
    R.Code=T70AwProbeStatusReadFailed;R.TransportStatus=Status.TransportStatus;return R;
  }
  *Snapshot=S;R.Code=T70AwProbeRead;return R;
}
