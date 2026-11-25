// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeAddress.h"
T70_AFE_ADDRESS_RESULT T70AfeDl1SetAddress(const T70_AFE_PREPARED *Prepared,
                                         const T70_AFE_ADDRESS_IO *Io) {
  T70_AFE_ADDRESS_RESULT R={T70AfeAddressInvalid,0,0,0,0};
  if(!Io || !Io->Write32 || !T70AfeDl1PreparedValid(Prepared))return R;
  const uint32_t Offsets[]={0x54,0x64,0x50,0x60};
  const uint32_t Values[]={Prepared->Geometry.BaseLow,Prepared->Geometry.EndLow,
                           Prepared->Geometry.BaseHigh,Prepared->Geometry.EndHigh};
  for(unsigned I=0;I<4;I++) {
    R.WritesAttempted++;
    R.TransportStatus=Io->Write32(Io->Context,Offsets[I],Values[I]);
    if(R.TransportStatus){R.Code=T70AfeAddressWriteFailed;R.FailedOffset=Offsets[I];return R;}
    R.WritesCompleted++;
  }
  R.Code=T70AfeAddressConfigured;return R;
}
