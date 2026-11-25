// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "RouteMapped.h"
T70_MAPPED_ROUTE_RESULT T70AfeObserveMappedRoute(T70_AFE_MAPPING_OWNER *Owner,
                                               T70_AFE_ROUTE_OBSERVATION *Out){
 T70_MAPPED_ROUTE_RESULT R={0};T70_AFE_ROUTE_OBSERVATION S={0};
 uintptr_t P=(uintptr_t)Out,O=(uintptr_t)Owner;
 if(!Owner || !Out || (P&3) || P>UINTPTR_MAX-sizeof(*Out) ||
    O>UINTPTR_MAX-sizeof(*Owner) ||
    (P<O+sizeof(*Owner) && O<P+sizeof(*Out)))return R;
 uintptr_t B=(uintptr_t)Owner->Mapping.VirtualBase;size_t N=Owner->Mapping.Length;
 if(N>UINTPTR_MAX-B || (P<B+N && B<P+sizeof(*Out)))return R;
 R.BeginAttempted=1;R.BeginStatus=T70AfeMappingBegin(Owner);
 if(!NT_SUCCESS(R.BeginStatus)){R.Code=T70MappedRouteBeginFailed;return R;}
 const T70_AFE_ROUTE_READER Io={&Owner->Mapping,T70AfeMappedRead32};
 R.ReadStarted=1;R.Route=T70AfeObserveDl1I2s3(&Io,&S);
 R.EndAttempted=1;R.EndStatus=T70AfeMappingEnd(Owner);
 if(!NT_SUCCESS(R.EndStatus)){R.Code=T70MappedRouteEndFailed;return R;}
 R.LeaseEnded=1;
 if(R.Route.Code!=T70AfeRouteObserved){R.Code=T70MappedRouteReadFailed;return R;}
 *Out=S;R.Code=T70MappedRouteObserved;return R;
}
