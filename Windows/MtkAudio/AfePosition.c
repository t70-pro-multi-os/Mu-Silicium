// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfePosition.h"
static int Position(const T70_AFE_PREPARED *Prepared,uint64_t Current,
                    int Normalize,T70_AFE_POSITION *Output) {
  T70_AFE_PREPARED P;T70_AFE_POSITION R;uint64_t Offset;
  if(!Output || !T70AfeDl1PreparedValid(Prepared))return 0;
  P=*Prepared;
  if(Current>T70_AFE_DMA_MAX || Current<P.DmaBase || Current>P.Geometry.EndInclusive)return 0;
  Offset=Current-P.DmaBase;
  if(Normalize)Offset-=Offset%16;
  else if(Offset%16)return 0;
  if(Offset%P.Format.FrameBytes)return 0;
  R.ByteOffset=(uint32_t)Offset;R.FrameOffset=(uint32_t)(Offset/P.Format.FrameBytes);
  *Output=R;return 1;
}

int T70AfeDl1Position(const T70_AFE_PREPARED *Prepared,uint64_t Current,
                      T70_AFE_POSITION *Output) {
  return Position(Prepared,Current,0,Output);
}
int T70AfeDl1NormalizedPosition(const T70_AFE_PREPARED *Prepared,uint64_t Current,
                                T70_AFE_POSITION *Output) {
  return Position(Prepared,Current,1,Output);
}
