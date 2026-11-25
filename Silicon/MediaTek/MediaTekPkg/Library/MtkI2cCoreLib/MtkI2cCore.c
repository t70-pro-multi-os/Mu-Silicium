// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkI2cCore.h>
/* Shipped mt6983_compat selects 32-bit controller MMIO; FIFO is byte-wide. */
#define R(c,o) ((c)->Io.Read((c)->Io.Context,0,(o),4))
#define W(c,o,v) ((c)->Io.Write((c)->Io.Context,0,(o),4,(v)))
#define DR(c,o) ((c)->Io.Read((c)->Io.Context,1,(o),4))
#define DW(c,o,v) ((c)->Io.Write((c)->Io.Context,1,(o),4,(v)))
#define IRQ_ALL 0x21fU
#define ACKERR 2U
#define NACK 4U
#define ARBLOST 8U
#define COMPLETE 1U
#define RESTART 16U
#define IRQ_RESULT (COMPLETE|ACKERR|NACK|ARBLOST)
#define BASE_CONTROL 0x28U
static MI_U32 Ceil(MI_U64 N, MI_U32 D) { return (MI_U32)((N+D-1)/D); }
MI_STATUS MiCalculateTiming(MI_U32 SourceHz, MI_U32 BusHz, MI_TIMING *T) {
  MI_U32 Div, Sample, Step, Best=0;
  MI_TIMING Candidate={0};
  if (!T || SourceHz<1000000U || SourceHz>1000000000U || BusHz<10000U || BusHz>400000U)
    return MiUnsupported;
  /* Search representable periods. Encoded high/low count zero means one cycle.
   * Deliberately conservative MTK AC limits include controller internal delays.
   * Do not round a requested frequency upwards. */
  for (Div=1;Div<=256;Div++) {
    MI_U32 ClockNs=(MI_U32)(1000000000ULL*Div/SourceHz);
    MI_U32 Start=Ceil(BusHz<=100000U?5200U:750U,ClockNs);
    if (Start>255U) continue;
    for (Sample=1;Sample<=8;Sample++) {
      MI_U32 Ns=(MI_U32)(1000000000ULL*Div*Sample/SourceHz);
      MI_U32 LowMin=Ceil(BusHz<=100000U?5200U:1450U,Ns);
      MI_U32 Hold=(BusHz<=100000U?2950U:750U)/Ns;
      MI_U32 Setup=Ceil(BusHz<=100000U?750U:250U,Ns);
      for (Step=2;Step<=64;Step++) {
        MI_U32 Low=LowMin>Step-1?LowMin:Step-1;
        MI_U32 High, SdaMin, SdaMax, Hz;
        MI_U64 Den=(MI_U64)Div*2*Sample*Step;
        if ((MI_U64)SourceHz>(MI_U64)BusHz*Den || Low>=64 || Low>=2*(Step-1)) continue;
        High=2*(Step-1)-Low;
        if (High>=64) continue;
        SdaMin=Setup<Low?0:Setup; SdaMax=Hold>Low?0:Hold;
        if (SdaMin>SdaMax || (SdaMin+SdaMax)/2>63) continue;
        Hz=(MI_U32)(SourceHz/Den);
        if (Hz<=Best) continue;
        Best=Hz;
        Candidate.Timing=(MI_U16)(((Sample-1)<<8)|High);
        Candidate.LowTiming=(MI_U16)(((Sample-1)<<6)|Low);
        Candidate.Ext=(MI_U16)((Start<<8)|1);
        Candidate.Sda=(MI_U16)(0x1000|((SdaMin+SdaMax)/2));
        Candidate.ClockDiv=(MI_U16)(((Div-1)<<8)|(Div-1));
        Candidate.ActualHz=Hz;
      }
    }
  }
  if (!Best) return MiUnsupported;
  *T=Candidate; return MiOk;
}
static int ValidIo(const MI_IO *I) {
  return I && I->Read && I->Write && I->NowUs && I->DelayUs && I->Fence;
}
static int Cancelled(MI_CONTROLLER *C) {
  return C->Io.Cancelled && C->Io.Cancelled(C->Io.Context);
}
static MI_STATUS StopWithBudget(MI_CONTROLLER *C, MI_U32 TimeoutUs) {
  MI_U64 Start; unsigned Poll;
  if (!C || !ValidIo(&C->Io)) return MiInvalid;
  C->Ready=0; C->DmaSafe=0; C->SequenceHeld=0;
  W(C,0x08,0); W(C,0x24,0);
  DW(C,0x04,0); DW(C,0x0c,6); W(C,0x50,0x21);
  C->Io.Fence(C->Io.Context); C->Io.DelayUs(C->Io.Context,10);
  DW(C,0x0c,0); W(C,0x50,0);
  Start=C->Io.NowUs(C->Io.Context);
  /* Iteration cap also terminates if a broken timer stops advancing. */
  for (Poll=0;Poll<=TimeoutUs/10;Poll++) {
    if (!(DR(C,0x08)&1)) { C->DmaSafe=1; W(C,0x0c,IRQ_ALL); W(C,0x38,1); return MiOk; }
    if (Poll==TimeoutUs/10 || C->Io.NowUs(C->Io.Context)-Start>=TimeoutUs) break;
    C->Io.DelayUs(C->Io.Context,10);
  }
  return MiDmaStuck;
}
MI_STATUS MiStop(MI_CONTROLLER *C) {
  return StopWithBudget(C,10000);
}
static void ProgramTiming(MI_CONTROLLER *C) {
  W(C,0x34,3); W(C,0x20,C->Timing.Timing); W(C,0x2c,C->Timing.LowTiming);
  W(C,0x28,C->Timing.Ext); W(C,0x3c,C->Timing.Sda); W(C,0x48,C->Timing.ClockDiv);
  W(C,0x90,0); /* Shipped MT6983 init clears SCL mismatch compensation. */
  W(C,0x30,0); W(C,0x10,BASE_CONTROL); W(C,0x1c,2); W(C,0x08,0);
  C->Ready=1;
}
MI_STATUS MiInitialize(MI_CONTROLLER *C,const MI_IO *Io,MI_U32 SourceHz,MI_U32 BusHz) {
  MI_STATUS S; MI_TIMING T;
  if (!C || !ValidIo(Io)) return MiInvalid;
  S=MiCalculateTiming(SourceHz,BusHz,&T); if (S!=MiOk) return S;
  C->Io=*Io; C->Timing=T; C->SourceHz=SourceHz; C->RequestedHz=BusHz;
  S=MiStop(C); if (S==MiOk) ProgramTiming(C); return S;
}
MI_STATUS MiSetFrequency(MI_CONTROLLER *C,MI_U32 BusHz,MI_U32 *ActualHz) {
  MI_TIMING T; MI_STATUS S;
  if (!C || !C->Ready || C->SequenceHeld || !ActualHz) return MiInvalid;
  S=MiCalculateTiming(C->SourceHz,BusHz,&T); if (S!=MiOk) return S;
  C->Timing=T; C->RequestedHz=BusHz; ProgramTiming(C); *ActualHz=T.ActualHz; return MiOk;
}
static int DmaRange(MI_U64 A,MI_U32 N) {
  return !N || (A && A<(1ULL<<36) && N<=(1ULL<<36)-A);
}
static MI_STATUS Transaction(MI_CONTROLLER *C,unsigned Address,const MI_U8 *Tx,MI_U32 TxLen,
                     MI_U64 TxDma,MI_U8 *Rx,MI_U32 RxLen,MI_U64 RxDma,
                     unsigned SequenceCount,unsigned Index,MI_U64 BudgetUs,MI_U32 RecoveryUs) {
  MI_U32 Control=BASE_CONTROL, Flags, I, Total, PollLimit; MI_U64 Begin, Limit, Elapsed; int Dma;
  MI_STATUS Result=MiTimeout, StopStatus;
  MI_U32 Expected=SequenceCount && Index+1<SequenceCount?RESTART:COMPLETE;
  MI_U32 Mask=IRQ_RESULT|(SequenceCount?RESTART:0);
  if (!C || !C->Ready || !C->DmaSafe || Address<8 || Address>0x77 ||
      (!TxLen&&!RxLen) || TxLen>65535U || RxLen>65535U ||
      (TxLen&&!Tx) || (RxLen&&!Rx) || !C->Timing.ActualHz) return MiInvalid;
  Dma=TxLen>16 || RxLen>16;
  if (Dma && (!DmaRange(TxDma,TxLen)||!DmaRange(RxDma,RxLen))) return MiInvalid;
  if (C->SequenceHeld && (!SequenceCount || !Index)) return MiInvalid;
  if (Cancelled(C)) return MiCancelled;
  /* Never reset somebody else's live DMA transfer. */
  if ((DR(C,0x08)&1) || ((!SequenceCount || !Index) && (R(C,0x24)&1))) return MiInvalid;
  /* MT6983 APDMA synchronization for each read in a multi-message request:
   * reset only the DMA handshake, preserving the repeated-START bus state.
   * The channel reset below is 0x20, not the controller's full 0x21 reset. */
  if (SequenceCount>1 && RxLen && !TxLen) {
    /* Original ARM64 transfer uses handshake(4)|warm(1), not hard reset(2). */
    DW(C,0x0c,5); C->Io.Fence(C->Io.Context);
    Begin=C->Io.NowUs(C->Io.Context);
    for (I=0;I<11;I++) {
      if (Cancelled(C)) { Result=MiCancelled; goto Cleanup; }
      if (!(DR(C,0x0c)&1)) break;
      if (C->Io.NowUs(C->Io.Context)-Begin>=100) break;
      C->Io.DelayUs(C->Io.Context,10);
    }
    if (DR(C,0x0c)&1) { Result=MiTimeout; goto Cleanup; }
    DW(C,0x0c,0); W(C,0x50,0x20); W(C,0x50,0);
  }
  if (Expected==RESTART) Control|=2;
  if (TxLen&&RxLen) Control|=0x12; /* direction change + repeated START */
  if (Dma) Control|=0x304;
  W(C,0x08,0); W(C,0x0c,IRQ_ALL); W(C,0x38,1); W(C,0x10,Control);
  W(C,0x94,(Address<<1)|(TxLen?0:1));
  W(C,0x14,TxLen?TxLen:RxLen); W(C,0x44,TxLen&&RxLen?RxLen:0);
  W(C,0x18,SequenceCount?SequenceCount:(TxLen&&RxLen?2:1));
  if (Dma) {
    C->DmaSafe=0;
    DW(C,0x00,0); DW(C,0x04,0); DW(C,0x18,0x14|(TxLen?0:1)|(TxLen&&RxLen?0x200:0));
    DW(C,0x54,(MI_U32)(TxDma>>32)); DW(C,0x1c,(MI_U32)TxDma); DW(C,0x24,TxLen);
    DW(C,0x58,(MI_U32)(RxDma>>32)); DW(C,0x20,(MI_U32)RxDma); DW(C,0x28,RxLen);
    C->Io.Fence(C->Io.Context); DW(C,0x08,1);
  } else {
    for (I=0;I<TxLen;I++) C->Io.Write(C->Io.Context,0,0,1,Tx[I]);
  }
  /* Arm only after stale status, FIFO and DMA setup are complete. */
  if (C->Io.WaitForEvent) W(C,0x08,Mask);
  C->Io.Fence(C->Io.Context);
  W(C,0x24,SequenceCount?(Expected==RESTART?0xc001:0x4001):(TxLen&&RxLen?1:0x4001));
  Total=TxLen+RxLen+4;
  Limit=((MI_U64)Total*9000000ULL+C->Timing.ActualHz-1)/C->Timing.ActualHz+50000;
  Begin=C->Io.NowUs(C->Io.Context);
  /* A 65535+65535-byte 10kHz transaction can take two minutes. Cap total
   * occupancy at the caller's remaining budget (at most 10 seconds).
   * Sequence preflight also rejects excessive total wire time before START. */
  if (Limit>BudgetUs) Limit=BudgetUs;
  /* A failed clock or repeated spurious wakeups must not extend the bound. */
  PollLimit=C->Io.WaitForEvent?(MI_U32)(Limit/5000+2):(MI_U32)((Limit+9)/10+1);
  for (I=0;I<PollLimit;I++) {
    Flags=R(C,0x0c);
    if (Cancelled(C)) { Result=MiCancelled; break; }
    if (Flags&ARBLOST) { Result=MiArbitration; break; }
    if (Flags&(ACKERR|NACK)) { Result=MiNack; break; }
    if (Flags&Expected) { Result=MiOk; break; }
    if (Flags&(COMPLETE|RESTART)) { Result=MiBusError; break; }
    Elapsed=C->Io.NowUs(C->Io.Context)-Begin;
    if (Elapsed>=Limit) break;
    if (C->Io.WaitForEvent) {
      unsigned Us=(unsigned)(Limit-Elapsed>5000?5000:Limit-Elapsed);
      C->Io.WaitForEvent(C->Io.Context,Us);
    } else C->Io.DelayUs(C->Io.Context,10);
  }
  W(C,0x08,0);
  /* A restart pauses the bus; resetting the controller here would insert STOP.
   * On every successful DMA stage, wait for the final memory access before
   * reporting its bytes or reusing the next coherent buffer. */
  if (Result==MiOk && Dma) {
    MI_U64 Drain=C->Io.NowUs(C->Io.Context);
    for (I=0;I<RecoveryUs/10 && (DR(C,0x08)&1);I++) {
      if (Cancelled(C)) { Result=MiCancelled; break; }
      if (C->Io.NowUs(C->Io.Context)-Drain>=RecoveryUs) break;
      C->Io.DelayUs(C->Io.Context,10);
    }
    if (Result==MiOk && (DR(C,0x08)&1)) Result=MiDmaStuck;
    if (Result==MiOk) C->DmaSafe=1;
  }
  if (Result==MiOk && !Dma) {
    for (I=0;I<RxLen;I++) Rx[I]=(MI_U8)C->Io.Read(C->Io.Context,0,0,1);
  }
  C->Io.Fence(C->Io.Context);
  if (Result==MiOk && SequenceCount && Expected==RESTART) {
    W(C,0x0c,IRQ_ALL); C->SequenceHeld=1; return MiOk;
  }
  /* The shipped driver leaves a successful transfer configured. Confirm
   * final START release before returning, without issuing another START,
   * clearing the FIFO, or resetting the controller. The bound also covers a
   * stuck START or clock. A repeated-START stage returns above intentionally. */
  if (Result==MiOk) {
    MI_U64 Drain=C->Io.NowUs(C->Io.Context);
    for (I=0;I<=RecoveryUs/10;I++) {
      if (Cancelled(C)) { Result=MiCancelled; break; }
      if (!(R(C,0x24)&1U)) {
        W(C,0x0c,IRQ_ALL); C->SequenceHeld=0; return MiOk;
      }
      if (I==RecoveryUs/10 || C->Io.NowUs(C->Io.Context)-Drain>=RecoveryUs) {
        Result=MiTimeout; break;
      }
      C->Io.DelayUs(C->Io.Context,10);
    }
  }
  /* Reset/verify APDMA on failure or cancellation. A completed DMA stage
   * was drained above; ExitBootServices and explicit stop still reset it.
   * Errors never report partial completion or replay a transaction. */
Cleanup:
  StopStatus=StopWithBudget(C,RecoveryUs);
  if (StopStatus!=MiOk) return StopStatus;
  ProgramTiming(C);
  return Result;
}


MI_STATUS MiTransfer(MI_CONTROLLER *C,unsigned Address,const MI_U8 *Tx,MI_U32 TxLen,
                     MI_U64 TxDma,MI_U8 *Rx,MI_U32 RxLen,MI_U64 RxDma) {
  return Transaction(C,Address,Tx,TxLen,TxDma,Rx,RxLen,RxDma,0,0,10000000,10000);
}
MI_STATUS MiTransferTimed(MI_CONTROLLER *C,unsigned Address,const MI_U8 *Tx,MI_U32 TxLen,
                          MI_U64 TxDma,MI_U8 *Rx,MI_U32 RxLen,MI_U64 RxDma,
                          MI_U32 TimeoutUs) {
  if (!TimeoutUs || TimeoutUs>10000) return MiInvalid;
  return Transaction(C,Address,Tx,TxLen,TxDma,Rx,RxLen,RxDma,0,0,TimeoutUs,1000);
}
MI_STATUS MiSequence(MI_CONTROLLER *C,unsigned Address,const MI_MESSAGE *M,
                     unsigned Count,unsigned *Completed) {
  unsigned I; MI_STATUS S; MI_U64 Estimate=0,Begin,Elapsed;
  if (!Completed) return MiInvalid;
  *Completed=0;
  if (!C || !C->Ready || !C->DmaSafe || !ValidIo(&C->Io) || !M ||
      !Count || Count>MI_MAX_MESSAGES || Address<8 || Address>0x77 ||
      !C->Timing.ActualHz || C->SequenceHeld) return MiInvalid;
  /* Refuse unbounded bus occupancy before any target write. */
  for (I=0;I<Count;I++) {
    if (!M[I].Buffer || !M[I].Length || M[I].Length>65535 || M[I].Read>1 ||
        M[I].DelayUs>1000000 || (M[I].Length>16 && !DmaRange(M[I].Dma,M[I].Length))) return MiInvalid;
    Estimate+=M[I].DelayUs+((MI_U64)(M[I].Length+2)*9000000+C->Timing.ActualHz-1)/C->Timing.ActualHz+50000;
  }
  if (Estimate>10000000) return MiUnsupported;
  Begin=C->Io.NowUs(C->Io.Context);
  for (I=0;I<Count;I++) {
    MI_U32 Remaining=M[I].DelayUs;
    while (Remaining) {
      unsigned Us=Remaining>10?10:Remaining;
      if (Cancelled(C)) {
        if (I) { S=MiStop(C); if (S==MiOk) ProgramTiming(C); else return S; }
        return MiCancelled;
      }
      C->Io.DelayUs(C->Io.Context,Us); Remaining-=Us;
    }
    Elapsed=C->Io.NowUs(C->Io.Context)-Begin;
    if (Elapsed>=10000000) {
      S=MiStop(C); if (S==MiOk) ProgramTiming(C);
      return S==MiOk?MiTimeout:S;
    }
    S=Transaction(C,Address,M[I].Read?0:M[I].Buffer,M[I].Read?0:M[I].Length,M[I].Dma,
                  M[I].Read?M[I].Buffer:0,M[I].Read?M[I].Length:0,M[I].Dma,Count,I,
                  10000000-Elapsed,10000);
    if (S!=MiOk) {
      /* Cancellation or lost ownership before START still has to release a
       * bus held by earlier messages. Never replay a completed write. */
      if (C->SequenceHeld) {
        MI_STATUS Stop=MiStop(C); if (Stop==MiOk) ProgramTiming(C); else return Stop;
      }
      return S;
    }
    *Completed=I+1;
  }
  return MiOk;
}
