// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbProfileSerial.h"
#include "AwBoard.h"
static int Validate(const void *Acf,size_t Bytes,uint64_t Base,uint16_t Address,
 uint16_t Channel,uint16_t Profile,T70_AW_PAIR_SERIAL *S){
 if(!Acf||Bytes>UINTPTR_MAX-(uintptr_t)Acf||
    !T70AwBoardTupleValid(Base,Address,Channel))return 0;
 if(!T70AwPairSerialObserve(Acf,Bytes,Base,0x34,0x35,Profile,S))return 0;
 T70_AW_PROFILE_VIEW V;if(!T70AwProfileView(Acf,Bytes,Base,Address,Channel,Profile,&V))return 0;
 unsigned Seen=0;
 for(uint32_t I=0;I<V.RecordCount;I++){
  const uint8_t *P=V.Section.Data+64+(size_t)I*4;
  if(!P[1]&&P[0]==6){if(Seen)return 0;Seen=1;}
  if(!P[1]&&P[0]==7){if(Seen!=1)return 0;Seen=2;}
 }
 return Seen==2;
}
int T70AwSpbValidateProfileSerial(const void *Acf,size_t Bytes,uint64_t Base,
 uint16_t Address,uint16_t Channel,uint16_t Profile){
 T70_AW_PAIR_SERIAL S;return Validate(Acf,Bytes,Base,Address,Channel,Profile,&S);
}
T70_AW_SERIAL_APPLY_RESULT T70AwSpbApplyProfileSerial(const void *Acf,size_t Bytes,
 uint64_t Base,uint16_t Address,uint16_t Channel,uint16_t Profile,WDFIOTARGET Target){
 T70_AW_SERIAL_APPLY_RESULT R={T70AwSerialInvalid,0,0,0,0};T70_AW_PAIR_SERIAL S;
 if(!Acf||!Target||KeGetCurrentIrql()!=PASSIVE_LEVEL||
    Bytes>UINTPTR_MAX-(uintptr_t)Acf||!T70AwBoardTupleValid(Base,Address,Channel))return R;
 R.Code=T70AwSerialBadProfile;
 if(!Validate(Acf,Bytes,Base,Address,Channel,Profile,&S))return R;
 USHORT Id;R.TransportStatus=T70AwSpbReadRegister(Target,0,&Id);
 if(R.TransportStatus){R.Code=T70AwSerialIdFailed;return R;}
 if(Id!=0x2113){R.Code=T70AwSerialWrongId;return R;}
 const uint16_t Values[]={S.Channel[Channel].Reg06,S.Channel[Channel].Reg07};
 const uint16_t Masks[]={0x0fff,0x7fff};
 for(unsigned I=0;I<2;I++){
  USHORT Old;R.FailedRegister=(uint16_t)(6+I);
  R.TransportStatus=T70AwSpbReadRegister(Target,R.FailedRegister,&Old);
  if(R.TransportStatus){R.Code=T70AwSerialReadFailed;return R;}
  USHORT New=(USHORT)((Old&~Masks[I])|(Values[I]&Masks[I]));
  R.WritesAttempted++;
  R.TransportStatus=T70AwSpbWriteRegister(Target,R.FailedRegister,New);
  if(R.TransportStatus){R.Code=T70AwSerialWriteFailed;return R;}
  R.WritesCompleted++;
 }
 R.FailedRegister=0;R.Code=T70AwSerialFieldsWritten;return R;
}
