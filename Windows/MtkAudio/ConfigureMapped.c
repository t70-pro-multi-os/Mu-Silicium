// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "ConfigureMapped.h"
T70_MAPPED_CONFIG_RESULT T70AfeConfigureMapped(T70_AFE_MAPPING_OWNER *Owner,
                                             const T70_AFE_PREPARED *Prepared){
 T70_MAPPED_CONFIG_RESULT R={0};
 if(!Owner || !T70AfeDl1PreparedValid(Prepared))return R;
 R.BeginAttempted=1;R.BeginStatus=T70AfeMappingBegin(Owner);
 if(!NT_SUCCESS(R.BeginStatus)){R.Code=T70MappedConfigBeginFailed;return R;}
 T70_AFE_FORMAT_IO Io={&Owner->Mapping,T70AfeMappedRead32,T70AfeMappedWrite32};
 R.ConfigureStarted=1;R.Configure=T70AfeDl1Configure(Prepared,&Io);
 R.EndAttempted=1;R.EndStatus=T70AfeMappingEnd(Owner);
 if(!NT_SUCCESS(R.EndStatus)){R.Code=T70MappedConfigEndFailed;return R;}
 R.LeaseEnded=1;
 R.Code=R.Configure.Code==T70AfeConfigCallbacksCompleted?
        T70MappedConfigCallbacksCompleted:T70MappedConfigConfigureFailed;
 return R;
}
