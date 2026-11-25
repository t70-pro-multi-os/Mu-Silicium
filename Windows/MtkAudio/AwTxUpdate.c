// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwTxUpdate.h"
T70_AW_TX_UPDATE_RESULT T70Aw88261UpdateTx(const T70_AW_TX_IO *Io,unsigned Enabled,
                                         uint16_t *AppliedRegister) {
  T70_AW_TX_UPDATE_RESULT Result={T70AwTxInvalid,0};uint16_t Existing,Updated;
  if(!Io || !Io->Read || !Io->Write || !AppliedRegister || Enabled>1)return Result;
  Result.TransportStatus=Io->Read(Io->Context,T70_AW88261_TX_REGISTER,&Existing);
  if(Result.TransportStatus!=0){Result.Code=T70AwTxReadFailed;return Result;}
  T70Aw88261ComposeTxField(Existing,Enabled,&Updated);
  Result.TransportStatus=Io->Write(Io->Context,T70_AW88261_TX_REGISTER,Updated);
  if(Result.TransportStatus!=0){Result.Code=T70AwTxWriteFailed;return Result;}
  *AppliedRegister=Updated;Result.Code=T70AwTxUpdated;return Result;
}
