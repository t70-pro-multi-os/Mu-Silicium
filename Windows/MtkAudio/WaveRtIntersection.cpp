// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtMiniport.h"
#include "PcmIntersection.h"
NTSTATUS T70WaveRtMiniport::DataRangeIntersection(ULONG Pin,PKSDATARANGE Client,
 PKSDATARANGE Matching,ULONG Capacity,void *Output,PULONG Length){
 return T70PcmIntersection(Ranges,this,sizeof(*this),ResourceInitializationSucceeded(),
  Pin,Client,Matching,Capacity,Output,Length);
}
