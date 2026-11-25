// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeIrqFields.h"
T70_AFE_IRQ_FIELDS_RESULT T70AfeDl1UpdateIrqFields(const T70_AFE_PREPARED *P,
 const T70_AFE_IRQ_DESCRIPTOR *D,const T70_AFE_FORMAT_IO *Io){
 T70_AFE_IRQ_FIELDS_RESULT R={T70AfeIrqFieldsInvalid,0,2,0,0,0};uint32_t Count;
 static const uint32_t Offsets[]={0x300,0x3ac,0x3b0,0x3e4,0x3e8,0x3bc,0x304,0x3dc,0x308,0x600,0x604,0x3ec,0x40c,0x608,0x60c,0x610,0x614,0x618,0x61c,0x620,0x624,0x628,0x62c,0x630,0x634,0x638,0x63c};
 static const uint32_t RateOffsets[]={0x2e4,0x2e8,0x2f0,0x2f4};
 if(!D||D->Id>=27||!Io||!Io->Read32||!Io->Write32||
    D->CountOffset!=Offsets[D->Id]||D->CountShift!=0||D->CountMask!=0x3ffff||
    D->RateOffset!=RateOffsets[D->Id/8]||D->RateShift!=(D->Id%8)*4||D->RateMask!=15||
    !T70AfeDl1IrqCount(P,&Count))return R;
 const uint32_t Registers[]={D->CountOffset,D->RateOffset};
 const uint32_t Masks[]={D->CountMask,D->RateMask<<D->RateShift};
 const uint32_t Values[]={Count,P->Format.RateCode<<D->RateShift};
 for(unsigned I=0;I<2;I++){
  uint32_t Old;R.ReadsAttempted++;
  R.TransportStatus=Io->Read32(Io->Context,Registers[I],&Old);
  if(R.TransportStatus){R.Code=T70AfeIrqFieldsReadFailed;R.FailedField=I;return R;}
  R.WritesAttempted++;
  R.TransportStatus=Io->Write32(Io->Context,Registers[I],(Old&~Masks[I])|(Values[I]&Masks[I]));
  if(R.TransportStatus){R.Code=T70AfeIrqFieldsWriteFailed;R.FailedField=I;return R;}
  R.WritesCompleted++;
 }
 R.Code=T70AfeIrqFieldsCompleted;return R;
}
