// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeConfigure.h"
T70_AFE_CONFIG_RESULT T70AfeDl1Configure(const T70_AFE_PREPARED *Prepared,
                                        const T70_AFE_FORMAT_IO *Io) {
  T70_AFE_CONFIG_RESULT R={0};
  if(!Io || !Io->Read32 || !Io->Write32 || !T70AfeDl1PreparedValid(Prepared))return R;
  const T70_AFE_ADDRESS_IO AddressIo={Io->Context,Io->Write32};
  R.AddressStarted=1;
  R.Address=T70AfeDl1SetAddress(Prepared,&AddressIo);
  if(R.Address.Code!=T70AfeAddressConfigured){R.Code=T70AfeConfigAddressFailed;return R;}
  R.FormatStarted=1;
  R.Format=T70AfeDl1UpdateFormat(Prepared->Rate,Prepared->SampleFormat,Prepared->Channels,Io);
  R.Code=R.Format.Code==T70AfeFieldsUpdated?T70AfeConfigCallbacksCompleted:T70AfeConfigFormatFailed;
  return R;
}
