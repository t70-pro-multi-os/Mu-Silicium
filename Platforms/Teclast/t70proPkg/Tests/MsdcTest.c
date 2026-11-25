// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Library/MsdcImplLib.h>
MSDC_PLATFORM_INFO gPlatformInfo={.HostMask=2,.ReadOnly=TRUE,.MaxClockHz=25000000};
static BOOLEAN Present = TRUE;
static EFI_STATUS DetectStatus;
static unsigned DetectCalls, RemoveOnSample, FailOnSample, GpioInits, PowerOns;
EFI_STATUS GetSourceClockRate(UINT32 i,UINTN *hz) { *hz=200000000; return EFI_SUCCESS; }
static BOOLEAN SourceClockOn, TrackClockGating;
EFI_STATUS SourceClockControl(UINT32 i,BOOLEAN on) { SourceClockOn=on; return EFI_SUCCESS; }
EFI_STATUS ClockControl(UINT32 i,BOOLEAN on) { return EFI_SUCCESS; }
EFI_STATUS PowerControl(UINT32 i,BOOLEAN on) { CHECK(on); PowerOns++; return EFI_SUCCESS; }
EFI_STATUS InitGpio(UINT32 i) { GpioInits++; return EFI_SUCCESS; }
EFI_STATUS GetCardPresent(UINT32 i,BOOLEAN *p) {
  DetectCalls++;
  *p=FALSE;
  if (DetectStatus != EFI_SUCCESS) return DetectStatus;
  if (FailOnSample && DetectCalls>=FailOnSample) return EFI_DEVICE_ERROR;
  *p=Present && !(RemoveOnSample && DetectCalls>=RemoveOnSample);
  return EFI_SUCCESS;
}
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c"
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcUtils.c"
static UINT32 Regs[256], LastCommand, TxWord, TxCompletionStatus;
static unsigned Writes, Commands, AutoClear, AutoCommand, AutoIdentify, FifoBytes, FifoReads;
// Separate deterministic TX FIFO model; RX and legacy direct-word tests unchanged.
#ifndef MSDC_TEST_TX_CAPACITY
#define MSDC_TEST_TX_CAPACITY 1024
#endif
static UINT8 TxPayload[MSDC_TEST_TX_CAPACITY];
static unsigned TxExpected, TxQueued, TxPending, TxDrained, TxPolls, TxBusyPolls;
static BOOLEAN TxStreaming, TxStarted, TxNoComplete, TxEarlyComplete;
// Optional byte source for integration fixtures; default fixed-pattern tests unchanged.
static CONST UINT8 *RxSource;
static UINTN RxSourceSize,RxSourcePosition;
static UINT8 RxByte(void) {
  CHECK(RxSource && RxSourcePosition<RxSourceSize);
  return RxSource[RxSourcePosition++];
}
static UINT32 StreamRemaining, StreamError;
static unsigned StreamErrorAfterReads, FifoDelayPolls;
static BOOLEAN StreamStall, StreamStarted, StreamNoComplete, StreamEmptyComplete;
static unsigned BusyReads, BusyClearAfter;
static UINT32 BusyClearMask;
static UINT32 CommandHistory[256], CommandArguments[256];
static unsigned CommandHistoryCount;
static UINT32 FailIdentifyCommand=MAX_UINT32, FailIdentifyInterrupt;
static UINT32 IfCondReply=0x1aa, IfCondInterrupt=MSDC_INT_CMDRDY;
static UINT32 SelectReply;
static UINT32 StatusReply=0x900, StatusCalls, StatusBusyReplies, StatusPendingReply;
static BOOLEAN SelectBusyForever;
static UINT32 RcaReply=0x10000, AppCmdReply=BIT5, LastOpCondArg;
static UINT8 TestCsdStructure=1;
static UINT32 OpCondCount, OpCondBusyResponses;
static BOOLEAN FlipCcs;
static BOOLEAN TestCsdV1Maximum, TestCsdV1Oversize, TestCsdV2Maximum;
static EFI_DEVICE_PATH_PROTOCOL *ChildPath;
static EFI_HANDLE ChildHandle=(EFI_HANDLE)(UINTN)0x77;
static EFI_BLOCK_IO_MEDIA ChildMedia;
static EFI_BLOCK_IO_PROTOCOL ChildBlockIo;
static BOOLEAN ChildPathInstalled, ChildParentOpen, ChildBlockIoInstalled;
static BOOLEAN ChildConnectSucceeds;
static EFI_STATUS OpenInfoStatus, ChildDevicePathStatus, ChildBlockIoStatus;
UINT32 EFIAPI MmioRead32(UINTN a) {
  CHECK(a>=0x100000 && a<0x100400);
  UINT32 off=a-0x100000;
  if (TxStreaming && TxStarted && off==MSDC_INT && !(Regs[off/4]&MSDC_INT_CMDRDY)) {
    TxPolls++;
    unsigned drained=MIN(TxPending,32U);
    TxPending-=drained;TxDrained+=drained;
    CHECK(TxDrained<=TxQueued && TxQueued<=TxExpected);
    if ((TxDrained==TxExpected && !TxNoComplete) ||
        (TxEarlyComplete && TxDrained>=MSDC_FIFO_SIZE))
      Regs[off/4]|=MSDC_INT_XFER_COMPL;
    if (TxNoComplete && TxDrained==TxExpected) TimeNs+=1000000ULL;
  }
  if (TxStreaming && off==MSDC_FIFOCS) {
    if(TxPending)TxBusyPolls++;
    return TxPending<<16;
  }
  if (off==MSDC_INT && StreamStarted && StreamEmptyComplete && !(Regs[off/4]&MSDC_INT_CMDRDY))
    return Regs[off/4] | MSDC_INT_XFER_COMPL | StreamError;
  if (off==MSDC_INT && StreamStarted && StreamRemaining && !(Regs[off/4]&MSDC_INT_CMDRDY)) {
    if (StreamError && FifoReads>=StreamErrorAfterReads) { Regs[off/4] |= StreamError; return Regs[off/4]; }
    if (StreamStall) { TimeNs+=1000000ULL; }
    else if (!FifoBytes) {
      FifoBytes=MIN(StreamRemaining,MSDC_FIFO_SIZE);
      StreamRemaining-=FifoBytes;
      /* Model 2.5 MB/s card delivery (within the 25 MHz, 1-bit bus), independently of CPU polling delays. */
      TimeNs+=(UINT64)FifoBytes*1000000000ULL/2500000ULL;
      if (!StreamRemaining && !StreamNoComplete) Regs[off/4]|=MSDC_INT_XFER_COMPL;
    }
  }
  if (off==MSDC_INT && StreamStarted && StreamNoComplete && !StreamRemaining && !FifoBytes)
    TimeNs+=1000000ULL;
  if (off==SDC_STS) {
    BusyReads++;
    if (BusyClearAfter && BusyReads>=BusyClearAfter) Regs[off/4]&=~BusyClearMask;
  }
  if (off==MSDC_RXDATA) { FifoReads++; CHECK(FifoBytes>=4); FifoBytes-=4;
    if(RxSource){UINT32 V=0;for(unsigned N=0;N<4;N++)V|=(UINT32)RxByte()<<(N*8);return V;}
    return 0x44332211; }
  if (off==MSDC_FIFOCS && AutoCommand) {
    if (StreamStarted && FifoDelayPolls && --FifoDelayPolls==0) {
      FifoBytes=StreamRemaining;
      StreamRemaining=0;
    }
    return FifoBytes;
  }
  return Regs[off/4];
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  CHECK(a>=0x100000 && a<0x100400); Writes++;
  UINT32 off=a-0x100000;
  if (off==MSDC_CFG && TrackClockGating && (v&MSDC_CFG_CCKPD))
    CHECK(SourceClockOn && (Regs[off/4]&MSDC_CFG_CCKSB));
  if (off==MSDC_INT) { Regs[off/4]&=~v; return v; }
  Regs[off/4]=v;
  if (AutoClear && off==MSDC_CFG) Regs[off/4]=(v & ~MSDC_CFG_RST) | MSDC_CFG_CCKSB;
  if (AutoClear && off==MSDC_FIFOCS) Regs[off/4]=v & ~MSDC_FIFOCS_CLR;
  if (off==SDC_CMD) {
    Commands++; LastCommand=v;
    if (CommandHistoryCount<sizeof(CommandHistory)/sizeof(CommandHistory[0])) {
      CommandArguments[CommandHistoryCount]=Regs[SDC_ARG/4];
      CommandHistory[CommandHistoryCount++]=v&63;
    }
    if (AutoCommand) { Regs[MSDC_INT/4]=MSDC_INT_CMDRDY | MSDC_INT_XFER_COMPL; FifoBytes=64; }
    if (StreamRemaining) { StreamStarted=TRUE; Regs[MSDC_INT/4]=MSDC_INT_CMDRDY; FifoBytes=0; }
    if (TxStreaming) {
      CHECK((v&63)==24 || (v&63)==25);
      CHECK(TxQueued==0 && TxPending==0 && TxDrained==0);
      TxStarted=TRUE;Regs[MSDC_INT/4]=MSDC_INT_CMDRDY;FifoBytes=0;
    }
    if (AutoIdentify) {
      UINT32 cmd=v&63;
      Regs[SDC_RESP0/4]=cmd==8?IfCondReply:
        cmd==41?(BIT31|((TestCsdStructure==1)?BIT30:0)|0xff8000):
        cmd==3?RcaReply:cmd==55?AppCmdReply:cmd==7?SelectReply:cmd==13?StatusReply:0;
      if (cmd==13 && ++StatusCalls<=StatusBusyReplies) Regs[SDC_RESP0/4]=StatusPendingReply;
      if (cmd==7 && SelectBusyForever) Regs[MSDC_PS/4]&=~BIT16;
      if (cmd==8) Regs[MSDC_INT/4]=IfCondInterrupt;
      if (cmd==41) {
        LastOpCondArg=Regs[SDC_ARG/4];
        OpCondCount++;
        if(OpCondCount<=OpCondBusyResponses) Regs[SDC_RESP0/4]&=~BIT31;
        if(FlipCcs) Regs[SDC_RESP0/4]^=BIT30;
      }
      if (cmd==9) {
        SD_CSD Csd;
        UINT8 Raw[16];
        ZeroMem (&Csd, sizeof (Csd));
        if (TestCsdStructure==0) {
          Csd.CsdStructure=0;
          if (TestCsdV1Maximum || TestCsdV1Oversize) {
            Csd.CSizeLow=3;
            Csd.CSizeHigh=1023;
            Csd.CSizeMul=7;
            Csd.ReadBlLen=TestCsdV1Oversize?12:11;
          } else {
            Csd.CSizeLow=1;
            Csd.CSizeHigh=1;
            Csd.CSizeMul=3;
            Csd.ReadBlLen=9;
          }
        } else if (TestCsdStructure==1) {
          SD_CSD2 *Csd2=(SD_CSD2 *)(VOID *)&Csd;
          Csd2->CsdStructure=1;
          Csd2->CSizeLow=TestCsdV2Maximum?0xffff:0;
          Csd2->CSizeHigh=TestCsdV2Maximum?0x3f:0;
        } else {
          Csd.CsdStructure=TestCsdStructure;
        }
        ZeroMem (Raw, sizeof (Raw));
        Raw[0]=0xa5; // CRC/low response byte omitted by the R2 register layout.
        CopyMem (Raw+1, ((UINT8 *)&Csd)+1, 15);
        CopyMem (&Regs[SDC_RESP0/4], Raw, sizeof (Raw));
      }
    }
  }
  if (off==SDC_CMD && AutoIdentify && ((v&63)==FailIdentifyCommand))
    Regs[MSDC_INT/4]=FailIdentifyInterrupt;
  if (off==MSDC_TXDATA) {
    TxWord=v;
    if(TxStreaming) {
      CHECK(TxStarted && TxQueued+4<=TxExpected && TxPending+4<=MSDC_FIFO_SIZE);
      for(unsigned n=0;n<4;n++)TxPayload[TxQueued++]=(UINT8)(v>>(n*8));
      TxPending+=4;
    }
    if (TxCompletionStatus) Regs[MSDC_INT/4]=TxCompletionStatus;
  }
  return v;
}
UINT8 EFIAPI MmioRead8(UINTN a) { CHECK(a==0x100000+MSDC_RXDATA && FifoBytes>0); FifoBytes--; return RxSource?RxByte():0x11; }
UINT8 EFIAPI MmioWrite8(UINTN a,UINT8 v) { CHECK(a==0x100000+MSDC_TXDATA); return v; }
static void Reset(MSDC_PRIVATE_DATA *p) {
  memset(Regs,0,sizeof(Regs)); TimeNs=0; Writes=Commands=AutoClear=AutoCommand=FifoBytes=0;
  TxCompletionStatus=0;RxSource=NULL;RxSourceSize=RxSourcePosition=0;
  TxStreaming=TxStarted=TxNoComplete=TxEarlyComplete=FALSE;
  TxExpected=TxQueued=TxPending=TxDrained=TxPolls=TxBusyPolls=0;
  memset(TxPayload,0,sizeof(TxPayload));
  StreamErrorAfterReads=0;
  SourceClockOn=TRUE; TrackClockGating=FALSE; gPlatformInfo.AsyncFifo=FALSE;
  StreamRemaining=StreamError=FifoDelayPolls=0; StreamStall=StreamStarted=StreamNoComplete=StreamEmptyComplete=FALSE;
  BusyReads=BusyClearAfter=BusyClearMask=0;
  FailIdentifyCommand=MAX_UINT32; FailIdentifyInterrupt=0;
  CommandHistoryCount=0; memset(CommandHistory,0,sizeof(CommandHistory));
  *p=gMSDCPrivateDataTemplate; p->Index=1; p->MsdcMmioReg=0x100000; p->TimeoutNs=1000000;
  p->SdInfo.CardType=SdCard; p->MediaReady=TRUE;
  Present=TRUE; DetectStatus=EFI_SUCCESS;
  DetectCalls=RemoveOnSample=FailOnSample=AutoIdentify=GpioInits=PowerOns=FifoReads=0;
}
static void PioWriteAutoCommandTests(void) {
  MSDC_PRIVATE_DATA p;
  UINT32 buffer=0x12345678;
  const UINT32 errors[]={MSDC_INT_ACMDTMO,MSDC_INT_ACMDCRCERR,
                         MSDC_INT_DATTMO,MSDC_INT_DATCRCERR};
  for (unsigned i=0;i<ARRAY_SIZE(errors);i++) for (unsigned complete=0;complete<2;complete++) {
    Reset(&p);
    TxCompletionStatus=errors[i] | (complete?MSDC_INT_XFER_COMPL:0);
    CHECK(MsdcPioWrite(&p,&buffer,sizeof(buffer))==
          ((i==0 || i==2)?EFI_TIMEOUT:EFI_CRC_ERROR));
    CHECK(TxWord==buffer && !(Regs[MSDC_INT/4]&errors[i]));
    CHECK(TimeNs<1000000 && PowerOns==0 && GpioInits==0);
  }
  Reset(&p); TxCompletionStatus=MSDC_INT_XFER_COMPL;
  CHECK(MsdcPioWrite(&p,&buffer,sizeof(buffer))==EFI_SUCCESS);
  CHECK(!(Regs[MSDC_INT/4]&MSDC_INT_XFER_COMPL));
  // T70 remains read-only: this shared-core fix must not expose writes.
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandIndex=25,.CommandType=SdMmcCommandTypeAdtc,
                                .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,
                                             .OutDataBuffer=&buffer,.OutTransferLength=sizeof(buffer)};
  Reset(&p);
  CHECK(gPlatformInfo.ReadOnly);
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_WRITE_PROTECTED);
  CHECK(Writes==0 && PowerOns==0 && GpioInits==0);
}

static void StreamingWriteCommandTests(void) {
  MSDC_PRIVATE_DATA p;
  UINT32 words[256];UINT8 *bytes=(UINT8 *)words;
  for(unsigned n=0;n<sizeof(words);n++)bytes[n]=(UINT8)(n*37U+11U);
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandType=SdMmcCommandTypeAdtc,
                                .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,
    .OutDataBuffer=words,.Timeout=1000};
  // Each size: complete only after FIFO drain, missing completion, premature completion.
  for(unsigned blocks=1;blocks<=2;blocks++)for(unsigned mode=0;mode<3;mode++) {
    Reset(&p);AutoClear=AutoCommand=1;TxStreaming=TRUE;
    TxExpected=512U*blocks;TxNoComplete=(mode==1);TxEarlyComplete=(mode==2);
    cmd.CommandIndex=blocks==1?24:25;packet.OutTransferLength=TxExpected;
    EFI_STATUS expected=mode==0?EFI_SUCCESS:mode==1?EFI_TIMEOUT:EFI_ABORTED;
    CHECK(MsdcSendCmd(&p,&packet)==expected);
    CHECK(Commands==1 && (LastCommand&63)==cmd.CommandIndex);
    CHECK((LastCommand&SDC_CMD_RW)!=0);
    CHECK((LastCommand&(SDC_CMD_SINGLE_BLK|SDC_CMD_MULTIPLE_BLK))==
          (blocks==1?SDC_CMD_SINGLE_BLK:SDC_CMD_MULTIPLE_BLK));
    CHECK(((LastCommand&SDC_CMD_AUTO12)!=0)==(blocks==2));
    CHECK(((LastCommand>>SDC_CMD_BLK_SIZE_SHIFT)&0xfff)==512);
    CHECK(Regs[SDC_BLK_NUM/4]==blocks && TxBusyPolls>0);
    CHECK(TxQueued==(mode==2?MSDC_FIFO_SIZE:TxExpected));
    CHECK(TxDrained==TxQueued && TxPending==0);
    CHECK(!memcmp(TxPayload,bytes,TxQueued));
    CHECK(TxPolls>=TxDrained/32);
    CHECK(gPlatformInfo.ReadOnly && PowerOns==0 && GpioInits==0);
  }
}

static void FifoAlignmentTailTests(void) {
  MSDC_PRIVATE_DATA p;
  UINT32 storage[40];
  UINT8 *bytes=(UINT8 *)storage;
  for(unsigned offset=0;offset<4;offset++) for(unsigned length=0;length<=129;length++) {
    Reset(&p); AutoCommand=1; FifoBytes=length;
    memset(storage,0xa5,sizeof(storage));
    UINT8 *buffer=bytes+4+offset;
    MsdcFifoRead(&p,buffer,length);
    CHECK(FifoBytes==0);
    unsigned lead=MIN(length,(4-offset)%4);
    unsigned words=(length-lead)/4;
    CHECK(FifoReads==words);
    for(unsigned i=0;i<sizeof(storage);i++) {
      if(i<4+offset || i>=4+offset+length) CHECK(bytes[i]==0xa5);
      else {
        unsigned pos=i-4-offset;
        UINT8 expected=(pos>=lead && pos<lead+words*4)?
                       (UINT8)(0x11*(1+(pos-lead)%4)):0x11;
        CHECK(bytes[i]==expected);
      }
    }
  }
  // PIO drains a non-word tail across FIFO-size boundaries without overwriting guards.
  for(unsigned offset=0;offset<4;offset++) for(unsigned tail=1;tail<=3;tail++) {
    Reset(&p); AutoCommand=1; StreamStarted=TRUE; StreamRemaining=MSDC_FIFO_SIZE+tail;
    memset(storage,0xa5,sizeof(storage));
    CHECK(MsdcPioRead(&p,bytes+4+offset,MSDC_FIFO_SIZE+tail)==EFI_SUCCESS);
    CHECK(FifoBytes==0 && StreamRemaining==0);
    for(unsigned i=0;i<4+offset;i++) CHECK(bytes[i]==0xa5);
    for(unsigned i=4+offset+MSDC_FIFO_SIZE+tail;i<sizeof(storage);i++) CHECK(bytes[i]==0xa5);
  }
}

static void ShortFifoCompletionTests(void) {
  const UINT32 Supplied[]={1,3,4,128,508,512};
  MSDC_PRIVATE_DATA p;
  UINT8 Buffer[520];
  EFI_SD_MMC_COMMAND_BLOCK Cmd={.CommandIndex=17,.CommandType=SdMmcCommandTypeAdtc,
                               .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK Response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet={.SdMmcCmdBlk=&Cmd,.SdMmcStatusBlk=&Response,
    .InDataBuffer=Buffer+4,.InTransferLength=512,.Timeout=1000};
  for(UINTN Case=0;Case<ARRAY_SIZE(Supplied)+4;Case++) {
    Reset(&p); AutoClear=AutoCommand=1;
    memset(Buffer,0xa5,sizeof(Buffer));
    UINTN Count=(Case<ARRAY_SIZE(Supplied))?Supplied[Case]:0;
    StreamRemaining=Count?Count:512;
    if(Case==6) StreamEmptyComplete=TRUE;
    if(Case==7) StreamStall=TRUE;
    if(Case>=8) { StreamEmptyComplete=TRUE;
      StreamError=(Case==8)?MSDC_INT_DATCRCERR:MSDC_INT_DATTMO; }
    EFI_STATUS Expected=(Count==512)?EFI_SUCCESS:EFI_ABORTED;
    if(Case==7 || Case==9) Expected=EFI_TIMEOUT;
    if(Case==8) Expected=EFI_CRC_ERROR;
    CHECK(MsdcPassThru(&p.PassThru,0,&Packet,NULL)==Expected);
    CHECK(Packet.TransactionStatus==Expected && !p.CommandActive && p.MediaReady);
    CHECK(Commands==1);
    for(UINTN i=0;i<4;i++) CHECK(Buffer[i]==0xa5);
    for(UINTN i=4+Count;i<sizeof(Buffer);i++) CHECK(Buffer[i]==0xa5);
    if(Count) CHECK(Buffer[4]==0x11 && Buffer[3+Count]!=0xa5);
    // A subsequent clean read is a separate command, not an implicit retry.
    StreamRemaining=512; StreamError=0; StreamStarted=StreamStall=StreamEmptyComplete=FALSE;
    FifoBytes=0;
    CHECK(MsdcPassThru(&p.PassThru,0,&Packet,NULL)==EFI_SUCCESS);
    CHECK(Commands==2 && Packet.TransactionStatus==EFI_SUCCESS && !p.CommandActive);
    CHECK(Buffer[512]==0x11 && Buffer[513]==0x22 && Buffer[514]==0x33 && Buffer[515]==0x44);
    for(UINTN i=516;i<sizeof(Buffer);i++) CHECK(Buffer[i]==0xa5);
  }
  Reset(&p); AutoClear=AutoCommand=1; gPlatformInfo.AsyncFifo=TRUE;
  StreamRemaining=64; StreamEmptyComplete=TRUE; FifoDelayPolls=3;
  Packet.InTransferLength=64;
  memset(Buffer,0xa5,sizeof(Buffer));
  CHECK(MsdcPassThru(&p.PassThru,0,&Packet,NULL)==EFI_SUCCESS &&
        FifoReads==16 && !FifoBytes && !StreamRemaining && !FifoDelayPolls);
  CHECK(Buffer[3]==0xa5 && Buffer[4]==0x11 && Buffer[67]==0x44 && Buffer[68]==0xa5);
}

static void PartialReadErrorTests(void) {
  MSDC_PRIVATE_DATA p;
  UINT8 buffer[1024];
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandIndex=18,.CommandType=SdMmcCommandTypeAdtc,
                                .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,
    .InDataBuffer=buffer,.InTransferLength=sizeof(buffer),.Timeout=1000};
  const UINT32 errors[]={MSDC_INT_DATCRCERR,MSDC_INT_DATTMO};
  for(unsigned i=0;i<ARRAY_SIZE(errors);i++) {
    Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=sizeof(buffer);
    StreamError=errors[i]; StreamErrorAfterReads=MSDC_FIFO_SIZE/4;
    memset(buffer,0xa5,sizeof(buffer));
    EFI_STATUS expected=(i==0)?EFI_CRC_ERROR:EFI_TIMEOUT;
    CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==expected);
    CHECK(packet.TransactionStatus==expected && !p.CommandActive && p.MediaReady);
    CHECK(Commands==1 && FifoReads==MSDC_FIFO_SIZE/4);
    CHECK(buffer[0]==0x11 && buffer[1]==0x22 && buffer[2]==0x33 && buffer[3]==0x44);
    for(unsigned n=MSDC_FIFO_SIZE;n<sizeof(buffer);n++) CHECK(buffer[n]==0xa5);
    CHECK((Regs[MSDC_INT/4]&errors[i])==0 && !p.ChildWithdrawalPending);
  }
}

static void RemovalDuringReadTests(void) {
  MSDC_PRIVATE_DATA p;
  UINT8 buffer[1024];
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandIndex=18,.CommandType=SdMmcCommandTypeAdtc,
                                .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,
    .InDataBuffer=buffer,.InTransferLength=sizeof(buffer),.Timeout=1000};
  const unsigned samples[]={4,5,4+sizeof(buffer)/MSDC_FIFO_SIZE};
  const unsigned copied[]={0,MSDC_FIFO_SIZE,sizeof(buffer)};
  for(unsigned i=0;i<ARRAY_SIZE(samples);i++) {
    Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=sizeof(buffer);
    memset(buffer,0xa5,sizeof(buffer)); RemoveOnSample=samples[i];
    CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_NO_MEDIA);
    CHECK(packet.TransactionStatus==EFI_NO_MEDIA && !p.CommandActive && !p.MediaReady);
    CHECK(p.ChildWithdrawalPending && p.SdInfo.LastBlock==0 && p.SdInfo.CardType==UnknownCard);
    CHECK(Commands==1 && FifoReads==copied[i]/4);
    for(unsigned n=copied[i];n<sizeof(buffer);n++) CHECK(buffer[n]==0xa5);
    // A new presence sample alone must not permit stale identity to read again.
    RemoveOnSample=0; Present=TRUE;
    unsigned reads=FifoReads;
    CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_NOT_READY);
    CHECK(Commands==1 && FifoReads==reads && !p.CommandActive);
  }
}

static void ReadRetryStateTests(void) {
  MSDC_PRIVATE_DATA p;
  UINT8 buffer[1024];
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandIndex=18,.CommandType=SdMmcCommandTypeAdtc,
                                .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,
    .InDataBuffer=buffer,.InTransferLength=sizeof(buffer),.Timeout=1000};
  const UINT32 errors[]={MSDC_INT_ACMDTMO,MSDC_INT_ACMDCRCERR,MSDC_INT_DATTMO,0};
  for(unsigned i=0;i<ARRAY_SIZE(errors);i++) {
    Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=sizeof(buffer);
    StreamError=errors[i]; StreamStall=(errors[i]==0);
    EFI_STATUS expected=(errors[i]==MSDC_INT_ACMDCRCERR)?EFI_CRC_ERROR:EFI_TIMEOUT;
    CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==expected);
    CHECK(packet.TransactionStatus==expected && !p.CommandActive && p.MediaReady);
    CHECK(Commands==1 && (LastCommand&SDC_CMD_AUTO12)!=0);
    CHECK((Regs[MSDC_INT/4]&errors[i])==0);
    CHECK(GpioInits==0 && PowerOns==0);

    // A host retaining BUSY cannot be recovered by merely submitting again.
    StreamError=0; StreamStall=FALSE; StreamRemaining=sizeof(buffer);
    Regs[SDC_STS/4]=SDC_STS_BUSY;
    CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_TIMEOUT);
    CHECK(Commands==1 && !p.CommandActive);

    // If hardware releases BUSY, the next transaction retires status/FIFO.
    Regs[SDC_STS/4]=0; Regs[MSDC_INT/4]=MSDC_INT_ACMDTMO;
    StreamRemaining=sizeof(buffer); StreamStarted=FALSE; FifoBytes=0;
    CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_SUCCESS);
    CHECK(packet.TransactionStatus==EFI_SUCCESS && Commands==2 && !p.CommandActive);
    CHECK(FifoBytes==0 && StreamRemaining==0 && GpioInits==0 && PowerOns==0);
  }
}

static void CommandReadinessTests(void) {
  MSDC_PRIVATE_DATA p;
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandIndex=13,.CommandType=SdMmcCommandTypeAc,
                              .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  UINT8 buffer[64];
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,
                                             .InDataBuffer=buffer};
  // Command busy always blocks a command, even with no data transfer.
  Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=SDC_STS_CMDBUSY;
  CHECK(MsdcSendCmd(&p,&packet)==EFI_TIMEOUT && Commands==0 && TimeNs<=1010000);
  Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=SDC_STS_BUSY;
  CHECK(MsdcSendCmd(&p,&packet)==EFI_SUCCESS && Commands==1);
  Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=SDC_STS_BUSY|SDC_STS_CMDBUSY;
  BusyClearAfter=3; BusyClearMask=SDC_STS_CMDBUSY;
  CHECK(MsdcSendCmd(&p,&packet)==EFI_SUCCESS && Commands==1 && BusyReads==3);
  CHECK((Regs[SDC_STS/4]&SDC_STS_BUSY)!=0);
  // Data commands and R1b must also wait for the data path.
  cmd.CommandIndex=17; cmd.CommandType=SdMmcCommandTypeAdtc; packet.InTransferLength=sizeof(buffer);
  for (UINT32 busy=1;busy<=3;busy++) {
    Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=busy;
    CHECK(MsdcSendCmd(&p,&packet)==EFI_TIMEOUT && Commands==0 && TimeNs<=1010000);
  }
  Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=SDC_STS_BUSY;
  BusyClearAfter=3; BusyClearMask=SDC_STS_BUSY;
  CHECK(MsdcSendCmd(&p,&packet)==EFI_SUCCESS && Commands==1 && BusyReads==3);
  cmd.CommandIndex=7; cmd.CommandType=SdMmcCommandTypeAc; cmd.ResponseType=SdMmcResponseTypeR1b;
  packet.InTransferLength=0;
  Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=SDC_STS_BUSY;
  CHECK(MsdcSendCmd(&p,&packet)==EFI_TIMEOUT && Commands==0);
  Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=SDC_STS_BUSY;
  Regs[MSDC_PS/4]=BIT16; BusyClearAfter=3; BusyClearMask=SDC_STS_BUSY;
  CHECK(MsdcSendCmd(&p,&packet)==EFI_SUCCESS && Commands==1 && BusyReads==3);
  // Card removal still stops the wait before any command is issued.
  cmd.CommandIndex=13; cmd.ResponseType=SdMmcResponseTypeR1;
  Reset(&p); AutoClear=AutoCommand=1; Regs[SDC_STS/4]=SDC_STS_CMDBUSY; RemoveOnSample=3;
  CHECK(MsdcSendCmd(&p,&packet)==EFI_NO_MEDIA && Commands==0);
}
static EFI_TPL CurrentTpl = TPL_APPLICATION;
static unsigned TplCalls;
static EFI_TPL EFIAPI Raise(EFI_TPL next) { TplCalls++; EFI_TPL old=CurrentTpl; CHECK(next>=old); CurrentTpl=next; return old; }
static VOID EFIAPI Restore(EFI_TPL old) { TplCalls++; CHECK(old<=CurrentTpl); CurrentTpl=old; }
static void HotplugTests(void);
static void SdIdentificationTests(void);
static void CardReplacementIdentityTests(void);
static void DiagnosticsTests(void);
static MSDC_DIAGNOSTICS_RECORD ReadDiagnostics(void);
static void TransferTimeoutTests(void) {
  MSDC_PRIVATE_DATA p;
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandIndex=18,.CommandType=SdMmcCommandTypeAdtc,
                              .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  UINT32 bytes=65535U*512U;
  UINT8 *buffer=malloc(bytes);
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,
    .InDataBuffer=buffer,.InTransferLength=bytes};
  CHECK(buffer!=NULL);
  /* Same maximum chunk and microsecond budget as SdRwMultiBlocks. */
  packet.Timeout=(bytes/(2*1024*1024)+1)*1000000ULL;
  Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=bytes;
  EFI_STATUS status=MsdcPassThru(&p.PassThru,0,&packet,NULL);
  fprintf(stderr,"MSDC slow read: status=%llu elapsed_ns=%llu budget_us=%llu remaining=%u\n",
    (unsigned long long)status,(unsigned long long)TimeNs,
    (unsigned long long)packet.Timeout,StreamRemaining);
  CHECK(status==EFI_SUCCESS && !StreamRemaining && !FifoBytes);
  CHECK(packet.TransactionStatus==status && !p.CommandActive && CurrentTpl==TPL_APPLICATION);
  CHECK(TimeNs>5000000000ULL && TimeNs<packet.Timeout*1000ULL);
  CHECK(buffer[0]==0x11 && buffer[bytes-1]==0x44);
  Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=bytes; StreamStall=TRUE;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_TIMEOUT);
  CHECK(packet.TransactionStatus==EFI_TIMEOUT && !p.CommandActive && CurrentTpl==TPL_APPLICATION);
  CHECK(TimeNs>=packet.Timeout*1000ULL && TimeNs<packet.Timeout*1000ULL+2000000ULL);
  Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=bytes; StreamStall=TRUE;
  packet.Timeout=0;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_TIMEOUT);
  CHECK(packet.TransactionStatus==EFI_TIMEOUT && !p.CommandActive && CurrentTpl==TPL_APPLICATION);
  CHECK(TimeNs>=MSDC_DEFAULT_TIMEOUT_NS && TimeNs<MSDC_DEFAULT_TIMEOUT_NS+2000000ULL);
  Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=512; StreamNoComplete=TRUE;
  packet.InTransferLength=512; packet.Timeout=5000;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_TIMEOUT);
  CHECK(!StreamRemaining && !FifoBytes && !p.CommandActive &&
        packet.TransactionStatus==EFI_TIMEOUT && CurrentTpl==TPL_APPLICATION);
  CHECK(TimeNs>=5000000ULL && TimeNs<7000000ULL);
  Reset(&p); AutoClear=AutoCommand=1; StreamRemaining=512;
  packet.InTransferLength=512; packet.Timeout=MAX_UINT64;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_SUCCESS);
  CHECK(p.TimeoutNs==(MAX_UINT64/1000ULL)*1000ULL);
  free(buffer);
}
int main(void) {
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore;
  PioWriteAutoCommandTests();
  StreamingWriteCommandTests();
  FifoAlignmentTailTests();
  PartialReadErrorTests();
  ShortFifoCompletionTests();
  RemovalDuringReadTests();
  ReadRetryStateTests();
  CommandReadinessTests();
  TransferTimeoutTests();
  MSDC_PRIVATE_DATA p;
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandType=SdMmcCommandTypeAc,.ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  UINT8 buffer[1024]; UINT32 count;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.SdMmcCmdBlk=&cmd,.SdMmcStatusBlk=&response,.InDataBuffer=buffer,.OutDataBuffer=buffer,.Timeout=1000};
  Reset(&p);
  for (unsigned n=0;n<64;n++) {
    cmd.CommandIndex=n;
    if (n==6 || n==0 || n==1 || n==2 || n==3 || n==7 || n==8 || n==9 || n==10 || n==12 || n==13 || n==16 || n==17 || n==18 || n==41 || n==51 || n==55 || n==58) continue;
    CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_WRITE_PROTECTED && Writes==0);
  }
  cmd.CommandIndex=17; packet.OutTransferLength=512;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_WRITE_PROTECTED && Writes==0);
  packet.OutTransferLength=0;
  CHECK(MsdcPassThru(&p.PassThru,1,&packet,NULL)==EFI_INVALID_PARAMETER && Writes==0);
  AutoClear=AutoCommand=1;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,(EFI_EVENT)1)==EFI_SUCCESS && Commands==1);
  CHECK(packet.TransactionStatus==EFI_SUCCESS && !p.CommandActive);
  Reset(&p);
  packet.InTransferLength=512; packet.InDataBuffer=NULL;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_INVALID_PARAMETER && Writes==0);
  packet.InDataBuffer=buffer; packet.InTransferLength=0;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_TIMEOUT && TimeNs<=1100000);
  Reset(&p); CHECK(MsdcReset(&p)==EFI_TIMEOUT && TimeNs==100000000);
  Reset(&p); CHECK(MsdcClearFifo(&p)==EFI_TIMEOUT && TimeNs==100000000);
  Reset(&p); CHECK(MsdcSetMclk(&p,0)==EFI_INVALID_PARAMETER && Writes==0);
  CHECK(MsdcSetMclk(&p,1000)==EFI_UNSUPPORTED && Writes==0);
  Regs[MSDC_CFG/4]=MSDC_CFG_CCKPD;
  p.HostData.Sclk=25000000;
  TrackClockGating=TRUE;
  CHECK(MsdcSetMclk(&p,400000)==EFI_TIMEOUT && TimeNs==200000000 &&
        !p.HostData.Sclk && !(Regs[MSDC_CFG/4]&MSDC_CFG_CCKPD));
  Reset(&p); AutoClear=1; TrackClockGating=TRUE; Regs[MSDC_CFG/4]=MSDC_CFG_CCKPD;
  CHECK(MsdcSetMclk(&p,400000)==EFI_SUCCESS && p.HostData.Sclk==400000 &&
        (Regs[MSDC_CFG/4]&MSDC_CFG_CCKPD));
  Reset(&p); Regs[MSDC_INT/4]=MSDC_INT_CMDRDY|MSDC_INT_CMDCRCERR;
  CHECK(MsdcPollInterrupts(&p,MSDC_INT_CMDSTS,MSDC_INT_CMDRDY)==EFI_CRC_ERROR);
  // Auto-command ready is not an error, nor main-command completion.
  Reset(&p); Regs[MSDC_INT/4]=MSDC_INT_CMDRDY|MSDC_INT_ACMDRDY|MSDC_INT_XFER_COMPL;
  CHECK(MsdcPollInterrupts(&p,MSDC_INT_CMDSTS,MSDC_INT_CMDRDY)==EFI_SUCCESS);
  CHECK(Regs[MSDC_INT/4]==(MSDC_INT_ACMDRDY|MSDC_INT_XFER_COMPL));
  Reset(&p); Regs[MSDC_INT/4]=MSDC_INT_ACMDRDY;
  CHECK(MsdcPollInterrupts(&p,MSDC_INT_CMDSTS,MSDC_INT_CMDRDY)==EFI_TIMEOUT);
  CHECK(Regs[MSDC_INT/4]==MSDC_INT_ACMDRDY && Writes==0);
  const UINT32 CommandErrors[]={MSDC_INT_CMDCRCERR,MSDC_INT_CMDTMO,MSDC_INT_ACMDCRCERR,MSDC_INT_ACMDTMO};
  const EFI_STATUS CommandStatuses[]={EFI_CRC_ERROR,EFI_TIMEOUT,EFI_CRC_ERROR,EFI_TIMEOUT};
  for(unsigned error=0;error<4;error++) {
    Reset(&p); Regs[MSDC_INT/4]=MSDC_INT_CMDRDY|MSDC_INT_ACMDRDY|CommandErrors[error]|MSDC_INT_XFER_COMPL;
    CHECK(MsdcPollInterrupts(&p,MSDC_INT_CMDSTS,MSDC_INT_CMDRDY)==CommandStatuses[error]);
    CHECK(Regs[MSDC_INT/4]==MSDC_INT_XFER_COMPL);
  }
  Reset(&p); Regs[MSDC_FIFOCS/4]=0x007f0000; MsdcFifoTxBytes(&p,&count); CHECK(count==127);
  Reset(&p); CHECK(MsdcPioRead(&p,buffer,64)==EFI_TIMEOUT && TimeNs<=1000000);
  Reset(&p); Regs[MSDC_FIFOCS/4]=0x00810000;
  CHECK(MsdcPioWrite(&p,buffer,64)==EFI_DEVICE_ERROR);
  Reset(&p); *(UINT32 *)buffer=0x78563412;
  MsdcFifoWrite(&p,buffer,4); CHECK(TxWord==0x78563412);
  Reset(&p); AutoClear=AutoCommand=1; cmd.CommandIndex=6; cmd.CommandType=SdMmcCommandTypeAdtc; packet.InTransferLength=64;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_SUCCESS);
  CHECK(((LastCommand>>SDC_CMD_BLK_SIZE_SHIFT)&0xfff)==64 && Regs[SDC_BLK_NUM/4]==1 && FifoBytes==0);
  CHECK(buffer[0]==0x11 && buffer[1]==0x22 && buffer[2]==0x33 && buffer[3]==0x44);
  Reset(&p); p.Index=0; CHECK(!MsdcReadOnlyCommand(&p,&packet));
  Reset(&p); packet.InTransferLength=513; CHECK(MsdcSendCmd(&p,&packet)==EFI_BAD_BUFFER_SIZE && Commands==0);
  Reset(&p); AutoClear=AutoCommand=1; Regs[MSDC_PS/4]=BIT16;
  Regs[SDC_RESP0/4]=BIT8|(4U<<9); // CMD13 ready transfer state.
  CHECK(CardSetBusMode(&p,1)==EFI_SUCCESS && Commands==2 && (LastCommand&63)==13);
  Reset(&p); AutoClear=AutoCommand=1; packet.InTransferLength=0; cmd.CommandIndex=7;
  cmd.CommandType=SdMmcCommandTypeAc; cmd.ResponseType=SdMmcResponseTypeR1b;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_TIMEOUT && TimeNs<=1010000);
  Reset(&p); AutoClear=AutoCommand=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_SUCCESS);
  HotplugTests();
  SdIdentificationTests();
  CardReplacementIdentityTests();
  DiagnosticsTests();
  printf("MSDC CORE: %u checks passed (CSD geometry, child connection/withdrawal, retry, media safety, EBS)\n",Checks);
}

UINTN EFIAPI DevicePathNodeLength(CONST VOID *node) { const UINT8 *b=node; return b[2] | ((UINTN)b[3]<<8); }


typedef struct {
  UINT32 Type;
  EFI_TPL Tpl;
  EFI_EVENT_NOTIFY Notify;
  VOID *Context;
  BOOLEAN Closed;
} MOCK_EVENT;
static MOCK_EVENT Events[4];
static unsigned EventCount, EventAttempts, CloseCount, FailEvent, Reinstalls;
static EFI_STATUS TimerStatus, InstallStatus, ReinstallStatus;
static MSDC_PRIVATE_DATA *Installed;
static MSDC_DIAGNOSTICS_PROTOCOL *InstalledDiagnostics;
static EFI_STATUS DiagnosticInstallStatus;
static VOID *InstalledPath;
EFI_GUID gEfiEventExitBootServicesGuid={11}, gEfiDevicePathProtocolGuid={12},
         gEfiSdMmcPassThruProtocolGuid={13}, gEfiBlockIoProtocolGuid={14},
         gMediaTekMsdcDiagnosticsProtocolGuid={15};

UINTN EFIAPI GetDevicePathSize(CONST EFI_DEVICE_PATH_PROTOCOL *Path) {
  UINTN Size=0;
  if (Path==NULL) return 0;
  for (UINTN n=0;n<256;n++) {
    const UINT8 *b=(const UINT8 *)Path;
    UINTN NodeSize=b[2] | ((UINTN)b[3]<<8);
    if (NodeSize<sizeof(EFI_DEVICE_PATH_PROTOCOL)) return 0;
    Size+=NodeSize;
    if (b[0]==END_DEVICE_PATH_TYPE && b[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE)
      return Size;
    Path=(CONST EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)Path+NodeSize);
  }
  return 0;
}

BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *Node) {
  const UINT8 *b=Node;
  return b!=NULL && b[0]==END_DEVICE_PATH_TYPE &&
         b[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE;
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode(
  CONST EFI_DEVICE_PATH_PROTOCOL *Path,
  CONST EFI_DEVICE_PATH_PROTOCOL *Node
  ) {
  UINTN PathSize=GetDevicePathSize(Path);
  const UINT8 *b=(const UINT8 *)Node;
  UINTN NodeSize;
  UINTN NewSize;
  UINT8 *NewPath;
  EFI_DEVICE_PATH_PROTOCOL *End;
  if (PathSize<sizeof(EFI_DEVICE_PATH_PROTOCOL) || Node==NULL) return NULL;
  NodeSize=b[2] | ((UINTN)b[3]<<8);
  if (NodeSize<sizeof(EFI_DEVICE_PATH_PROTOCOL)) return NULL;
  NewSize=PathSize-sizeof(EFI_DEVICE_PATH_PROTOCOL)+NodeSize+
          sizeof(EFI_DEVICE_PATH_PROTOCOL);
  NewPath=malloc(NewSize);
  if (NewPath==NULL) return NULL;
  memcpy(NewPath,Path,PathSize-sizeof(EFI_DEVICE_PATH_PROTOCOL));
  memcpy(NewPath+PathSize-sizeof(EFI_DEVICE_PATH_PROTOCOL),Node,NodeSize);
  End=(EFI_DEVICE_PATH_PROTOCOL *)(NewPath+NewSize-sizeof(EFI_DEVICE_PATH_PROTOCOL));
  End->Type=END_DEVICE_PATH_TYPE;
  End->SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;
  End->Length[0]=sizeof(EFI_DEVICE_PATH_PROTOCOL);
  End->Length[1]=0;
  return (EFI_DEVICE_PATH_PROTOCOL *)NewPath;
}

static SD_DEVICE_PATH *GetChildSdPath(VOID) {
  UINTN ParentSize;
  if (ChildPath==NULL || InstalledPath==NULL) return NULL;
  ParentSize=GetDevicePathSize((EFI_DEVICE_PATH_PROTOCOL *)InstalledPath);
  if (ParentSize<sizeof(EFI_DEVICE_PATH_PROTOCOL)) return NULL;
  return (SD_DEVICE_PATH *)((UINT8 *)ChildPath+ParentSize-sizeof(EFI_DEVICE_PATH_PROTOCOL));
}

static VOID DestroyChild(VOID) {
  if (ChildPath!=NULL) {
    free(ChildPath);
    ChildPath=NULL;
  }
  ChildPathInstalled=ChildParentOpen=ChildBlockIoInstalled=FALSE;
}

static VOID CreateChild(VOID) {
  DestroyChild();
  CHECK(Installed!=NULL && InstalledPath!=NULL);
  ChildPath=AppendDevicePathNode(
    (EFI_DEVICE_PATH_PROTOCOL *)InstalledPath,
    (EFI_DEVICE_PATH_PROTOCOL *)&gSDDevicePathTemplate);
  CHECK(ChildPath!=NULL);
  ChildPathInstalled=TRUE;
  ChildParentOpen=TRUE;
  ChildBlockIoInstalled=TRUE;
  ZeroMem(&ChildMedia,sizeof(ChildMedia));
  ChildMedia.MediaId=1;
  ChildMedia.MediaPresent=TRUE;
  ChildMedia.BlockSize=BLOCK_SIZE;
  ChildMedia.LastBlock=Installed->SdInfo.LastBlock;
  ChildBlockIo.Media=&ChildMedia;
}

static EFI_STATUS EFIAPI MockHandleProtocol(
  EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface
  ) {
  if (Handle==Installed && Guid==&gEfiDevicePathProtocolGuid) {
    *Interface=InstalledPath;
    return InstalledPath==NULL?EFI_NOT_FOUND:EFI_SUCCESS;
  }
  if (Handle==ChildHandle && Guid==&gEfiDevicePathProtocolGuid) {
    if (EFI_ERROR(ChildDevicePathStatus)) return ChildDevicePathStatus;
    if (!ChildPathInstalled) return EFI_NOT_FOUND;
    *Interface=ChildPath;
    return EFI_SUCCESS;
  }
  if (Handle==ChildHandle && Guid==&gEfiBlockIoProtocolGuid) {
    if (EFI_ERROR(ChildBlockIoStatus)) return ChildBlockIoStatus;
    if (!ChildBlockIoInstalled) return EFI_NOT_FOUND;
    *Interface=&ChildBlockIo;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockOpenProtocolInformation(
  EFI_HANDLE Handle, EFI_GUID *Guid,
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer, UINTN *EntryCount
  ) {
  CHECK(Handle==Installed && Guid==&gEfiSdMmcPassThruProtocolGuid);
  *EntryBuffer=NULL;
  *EntryCount=0;
  // Failed enumeration does not transfer ownership of its output buffer.
  if (EFI_ERROR(OpenInfoStatus)) { *EntryBuffer=(VOID *)(UINTN)0xdead; return OpenInfoStatus; }
  if (ChildPathInstalled && ChildParentOpen) {
    *EntryBuffer=malloc(sizeof(**EntryBuffer));
    if (*EntryBuffer==NULL) return EFI_OUT_OF_RESOURCES;
    (*EntryBuffer)[0]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
      .AgentHandle=(EFI_HANDLE)(UINTN)0x88,
      .ControllerHandle=ChildHandle,
      .Attributes=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER,
      .OpenCount=1
    };
    *EntryCount=1;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockLocateDevicePath(
  EFI_GUID *Guid, EFI_DEVICE_PATH_PROTOCOL **Path, EFI_HANDLE *Handle
  ) {
  UINTN Size;
  CHECK(Guid==&gEfiDevicePathProtocolGuid && Path!=NULL && *Path!=NULL);
  *Handle=NULL;
  if (!ChildPathInstalled) return EFI_NOT_FOUND;
  Size=GetDevicePathSize(*Path);
  if ((Size!=GetDevicePathSize(ChildPath)) || CompareMem(*Path,ChildPath,Size)!=0)
    return EFI_NOT_FOUND;
  *Handle=ChildHandle;
  *Path=(EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)ChildPath+Size-sizeof(EFI_DEVICE_PATH_PROTOCOL));
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InstallDiagnosticProtocol(
  EFI_HANDLE *handle, EFI_GUID *guid, EFI_INTERFACE_TYPE type, VOID *interface
  ) {
  (VOID)handle;
  CHECK(guid==&gMediaTekMsdcDiagnosticsProtocolGuid);
  CHECK(type==EFI_NATIVE_INTERFACE);
  if (EFI_ERROR(DiagnosticInstallStatus)) return DiagnosticInstallStatus;
  CHECK(interface==&mMsdcDiagnosticsProtocol);
  InstalledDiagnostics=(MSDC_DIAGNOSTICS_PROTOCOL *)interface;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI EventCreate(UINT32 type,EFI_TPL tpl,EFI_EVENT_NOTIFY cb,
                                   VOID *ctx,EFI_EVENT *out) {
  CHECK(cb==(type==(EVT_TIMER|EVT_NOTIFY_SIGNAL)?MsdcPollMedia:MsdcExitBootServices));
  if (++EventAttempts==FailEvent) { *out=(EFI_EVENT)(UINTN)0xdead; return EFI_OUT_OF_RESOURCES; }
  CHECK(EventCount<4);
  MOCK_EVENT *e=&Events[EventCount++];
  *e=(MOCK_EVENT){type,tpl,cb,ctx,FALSE}; *out=e; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventEx(UINT32 type,EFI_TPL tpl,EFI_EVENT_NOTIFY cb,
                                CONST VOID *ctx,CONST EFI_GUID *group,EFI_EVENT *out) {
  CHECK(group==&gEfiEventExitBootServicesGuid && tpl==TPL_NOTIFY);
  return EventCreate(type,tpl,cb,(VOID *)ctx,out);
}
static EFI_STATUS EFIAPI Timer(EFI_EVENT event,EFI_TIMER_DELAY type,UINT64 period) {
  MOCK_EVENT *e=event;
  CHECK(e->Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && e->Tpl==TPL_CALLBACK);
  CHECK(type==TimerPeriodic && period==200000);
  return TimerStatus;
}
static EFI_STATUS EFIAPI EventClose(EFI_EVENT event) {
  CHECK(event!=NULL && event!=(EFI_EVENT)(UINTN)0xdead);
  MOCK_EVENT *e=event; CHECK(!e->Closed); e->Closed=TRUE; CloseCount++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Publish(EFI_HANDLE *handle,...) {
  if (InstallStatus!=EFI_SUCCESS) return InstallStatus;
  va_list args; va_start(args,handle);
  CHECK(va_arg(args,EFI_GUID *)==&gEfiDevicePathProtocolGuid);
  InstalledPath=va_arg(args,VOID *);
  CHECK(va_arg(args,EFI_GUID *)==&gEfiSdMmcPassThruProtocolGuid);
  EFI_SD_MMC_PASS_THRU_PROTOCOL *pass=va_arg(args,VOID *);
  CHECK(va_arg(args,VOID *)==NULL); va_end(args);
  Installed=MSDC_PRIVATE_FROM_THIS(pass); *handle=Installed; Installs++;
  if (Installed->MediaReady && ChildConnectSucceeds) CreateChild();
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Republish(EFI_HANDLE handle,EFI_GUID *guid,VOID *old,VOID *next) {
  CHECK(handle==Installed && guid==&gEfiSdMmcPassThruProtocolGuid &&
        old==&Installed->PassThru && next==old && Installed->PollActive);
  Reinstalls++;
  EFI_DEVICE_PATH_PROTOCOL *node=(VOID *)1;
  EFI_STATUS st=MsdcBuildDevicePath(&Installed->PassThru,0,&node);
  if (Installed->MediaReady) { CHECK(st==EFI_SUCCESS && node!=NULL); FreePool(node); }
  else CHECK(st==EFI_NOT_FOUND && node==NULL);
  if (ReinstallStatus==EFI_SUCCESS) {
    DestroyChild();
    if (Installed->MediaReady && ChildConnectSucceeds) CreateChild();
  }
  return ReinstallStatus;
}
static void Poll(MSDC_PRIVATE_DATA *p) {
  EFI_TPL old=CurrentTpl; CurrentTpl=TPL_CALLBACK; MsdcPollMedia(NULL,p); CurrentTpl=old;
}
static BOOLEAN HasCsdBeforeSelect(VOID) {
  INTN RcaIndex=-1, CsdIndex=-1, SelectIndex=-1;
  for (UINTN i=0;i<CommandHistoryCount;i++) {
    if ((RcaIndex<0) && (CommandHistory[i]==3)) RcaIndex=(INTN)i;
    if ((RcaIndex>=0) && (CsdIndex<0) && (CommandHistory[i]==9)) CsdIndex=(INTN)i;
    if ((CsdIndex>=0) && (CommandHistory[i]==7)) { SelectIndex=(INTN)i; break; }
  }
  return (RcaIndex>=0) && (CsdIndex>RcaIndex) && (SelectIndex>CsdIndex);
}
static void Setup(void) {
  MSDC_PRIVATE_DATA scratch;
  DestroyChild();
  Reset(&scratch); memset(Events,0,sizeof(Events));
  EventCount=EventAttempts=CloseCount=FailEvent=Reinstalls=Installs=0;
  TimerStatus=InstallStatus=ReinstallStatus=LocateStatus=EFI_SUCCESS;
  Installed=NULL; InstalledPath=NULL; InstalledDiagnostics=NULL;
  DiagnosticInstallStatus=EFI_SUCCESS; CurrentTpl=TPL_APPLICATION; TplCalls=0;
  ZeroMem(&mMsdcDiagnostics,sizeof(mMsdcDiagnostics));
  mMsdcDiagnostics.Revision=MSDC_DIAGNOSTICS_REVISION;
  mMsdcDiagnostics.Size=sizeof(mMsdcDiagnostics);
  mMsdcDiagnostics.HostIndex=1;
  mMsdcDiagnostics.CardDetectStatus=EFI_NOT_READY;
  mMsdcDiagnostics.LastDriverStatus=EFI_NOT_READY;
  mMsdcDiagnostics.LastInitStatus=EFI_NOT_READY;
  mMsdcDiagnostics.DevicePathStatus=EFI_NOT_READY;
  ChildConnectSucceeds=TRUE;
  TestCsdStructure=1; OpCondCount=OpCondBusyResponses=0; FlipCcs=FALSE;
  IfCondReply=0x1aa; IfCondInterrupt=MSDC_INT_CMDRDY;
  SelectReply=0; SelectBusyForever=FALSE;
  StatusReply=0x900; StatusCalls=StatusBusyReplies=StatusPendingReply=0;
  RcaReply=0x10000; AppCmdReply=BIT5; LastOpCondArg=0;
  TestCsdV1Maximum=TestCsdV1Oversize=TestCsdV2Maximum=FALSE;
  OpenInfoStatus=ChildDevicePathStatus=ChildBlockIoStatus=EFI_SUCCESS;
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore;
  Services.CreateEvent=EventCreate; Services.CreateEventEx=EventEx;
  Services.CloseEvent=EventClose; Services.SetTimer=Timer;
  Services.InstallProtocolInterface=InstallDiagnosticProtocol;
  Services.InstallMultipleProtocolInterfaces=Publish;
  Services.ReinstallProtocolInterface=Republish;
  Services.HandleProtocol=MockHandleProtocol;
  Services.OpenProtocolInformation=MockOpenProtocolInformation;
  Services.LocateDevicePath=MockLocateDevicePath;
}
static void Dispose(void) {
  if (Installed!=NULL) {
    EventClose(Installed->ConnectEvent); EventClose(Installed->ExitEvent);
    FreePool(InstalledPath); FreePool(Installed); Installed=NULL;
    InstalledPath=NULL;
  }
  DestroyChild();
}
static void HotplugTests(void) {
  EFI_SD_MMC_COMMAND_BLOCK cmd={.CommandIndex=13,.CommandType=SdMmcCommandTypeAc,
                               .ResponseType=SdMmcResponseTypeR1};
  EFI_SD_MMC_STATUS_BLOCK response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET packet={.Timeout=1000,.SdMmcCmdBlk=&cmd,
                                            .SdMmcStatusBlk=&response};
  SD_DEVICE_PATH *ChildSdPath;
  Setup(); Present=FALSE;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installs==1 && EventCount==2);
  CHECK(Installed!=NULL && !Installed->MediaReady && !Installed->MediaAnnounced);
  CHECK(Writes==0 && Commands==0 && GpioInits==0 && PowerOns==0);
  for(unsigned n=0;n<8;n++) Poll(Installed);
  CHECK(Commands==0 && Writes==0 && Reinstalls==0);
  UINT8 slot=0xff;
  CHECK(MsdcGetNextSlot(&Installed->PassThru,&slot)==EFI_SUCCESS && slot==0);
  CHECK(MsdcGetNextSlot(&Installed->PassThru,&slot)==EFI_NOT_FOUND);
  slot=2; CHECK(MsdcGetNextSlot(&Installed->PassThru,&slot)==EFI_INVALID_PARAMETER);
  EFI_DEVICE_PATH_PROTOCOL *node=(VOID *)1;
  CHECK(MsdcBuildDevicePath(&Installed->PassThru,0,&node)==EFI_NOT_FOUND && node==NULL);
  CHECK(MsdcPassThru(&Installed->PassThru,0,&packet,NULL)==EFI_NO_MEDIA &&
        packet.TransactionStatus==EFI_NO_MEDIA && Writes==0);
  Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  Poll(Installed); Poll(Installed);
  CHECK(Commands==0 && Installed->StableSamples==2);
  Present=FALSE; Poll(Installed); CHECK(Installed->StableSamples==0);
  Present=TRUE; Poll(Installed); Poll(Installed); CHECK(Commands==0);
  Poll(Installed);
  CHECK(Installed->MediaReady && Installed->MediaAnnounced && Reinstalls==1 &&
        !Installed->ChildConnectionPending && !Installed->ChildWithdrawalPending &&
        ChildPathInstalled && ChildParentOpen &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock && HasCsdBeforeSelect() &&
        GpioInits==1 && PowerOns==1 && Commands>0);
  CHECK(MsdcPassThru(&Installed->PassThru,0,&packet,NULL)==EFI_SUCCESS);
  CHECK(MsdcBuildDevicePath(&Installed->PassThru,0,&node)==EFI_SUCCESS);
  ((SD_DEVICE_PATH *)node)->SlotNumber=1;
  CHECK(MsdcGetSlotNumber(&Installed->PassThru,node,&slot)==EFI_UNSUPPORTED);
  ((SD_DEVICE_PATH *)node)->SlotNumber=0;
  CHECK(MsdcGetSlotNumber(&Installed->PassThru,node,&slot)==EFI_SUCCESS && slot==0);
  FreePool(node);
  unsigned before=Writes, beforeCmd=Commands;
  Present=FALSE;
  CHECK(MsdcPassThru(&Installed->PassThru,0,&packet,NULL)==EFI_NO_MEDIA &&
        packet.TransactionStatus==EFI_NO_MEDIA && Writes==before &&
        !Installed->MediaReady && Installed->MediaAnnounced);
  CHECK(MsdcResetDevice(&Installed->PassThru,0)==EFI_NO_MEDIA);
  CHECK(MsdcResetDevice(NULL,0)==EFI_INVALID_PARAMETER);
  CHECK(MsdcResetDevice(&Installed->PassThru,1)==EFI_INVALID_PARAMETER);
  ReinstallStatus=EFI_ACCESS_DENIED; Poll(Installed);
  CHECK(Installed->MediaAnnounced && Installed->ChildWithdrawalPending &&
        !Installed->MediaReady && Commands==beforeCmd);
  Present=TRUE;
  for(unsigned n=0;n<5;n++) Poll(Installed);
  CHECK(!Installed->MediaReady && Commands==beforeCmd);
  ReinstallStatus=EFI_SUCCESS;
  for(unsigned n=0;n<100 && Installed->ChildWithdrawalPending;n++) Poll(Installed);
  CHECK(!Installed->MediaAnnounced && !Installed->MediaReady && Commands==beforeCmd);
  Poll(Installed); Poll(Installed); CHECK(Commands==beforeCmd);
  Poll(Installed); CHECK(Installed->MediaReady && !Installed->ChildConnectionPending &&
                          Commands>beforeCmd && ChildPathInstalled &&
                          ChildMedia.LastBlock==Installed->SdInfo.LastBlock);
  // A failed detect must not be interpreted as an absent->present transition.
  DetectStatus=EFI_DEVICE_ERROR; before=Writes;
  CHECK(MsdcPassThru(&Installed->PassThru,0,&packet,NULL)==EFI_DEVICE_ERROR &&
        !Installed->MediaReady && Writes==before);
  Poll(Installed); CHECK(!Installed->MediaAnnounced);
  DetectStatus=EFI_SUCCESS;
  Poll(Installed); Poll(Installed); Poll(Installed);
  CHECK(Installed->MediaReady && !Installed->ChildConnectionPending);
  // Reentrancy and the EBS callback touch no pins, MMIO or boot services.
  before=Writes; beforeCmd=DetectCalls; Installed->CommandActive=TRUE;
  Poll(Installed);
  CHECK(MsdcPassThru(&Installed->PassThru,0,&packet,NULL)==EFI_NOT_READY);
  CHECK(Writes==before && DetectCalls==beforeCmd);
  Installed->CommandActive=FALSE; Installed->PollActive=TRUE;
  Poll(Installed); CHECK(Writes==before && DetectCalls==beforeCmd);
  Installed->PollActive=FALSE;
  unsigned beforeTpl=TplCalls;
  MsdcExitBootServices(NULL,Installed); MsdcExitBootServices(NULL,Installed);
  Poll(Installed);
  CHECK(MsdcPassThru(&Installed->PassThru,0,&packet,NULL)==EFI_UNSUPPORTED &&
        Writes==before && DetectCalls==beforeCmd);
  CHECK(MsdcBuildDevicePath(&Installed->PassThru,0,&node)==EFI_NOT_FOUND &&
        node==NULL && TplCalls==beforeTpl);
  Dispose();

  // A new card whose insertion notification fails stays blocked until a
  // successful withdrawal and fresh identification.
  Setup(); Present=FALSE; CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS);
  Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  ReinstallStatus=EFI_ACCESS_DENIED;
  Poll(Installed); Poll(Installed); Poll(Installed);
  CHECK(!Installed->MediaReady && Installed->MediaAnnounced);
  beforeCmd=Commands; ReinstallStatus=EFI_SUCCESS;
  Poll(Installed); CHECK(!Installed->MediaAnnounced && Commands==beforeCmd);
  Poll(Installed); Poll(Installed); Poll(Installed);
  CHECK(Installed->MediaReady && !Installed->ChildConnectionPending &&
        Commands>beforeCmd);
  Dispose();

  // The DXE core can return success from Reinstall even when the SD child
  // driver's Start failed. Keep the card identified and retry the binding
  // without sending another card-identification sequence.
  Setup(); Present=FALSE; CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS);
  ChildConnectSucceeds=FALSE;
  Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  Poll(Installed); Poll(Installed); Poll(Installed);
  CHECK(Reinstalls==1 && Installed->MediaReady && Installed->MediaAnnounced &&
        Installed->ChildConnectionPending && !Installed->ChildWithdrawalPending &&
        !ChildPathInstalled);
  beforeCmd=Commands;
  for(unsigned n=0;n<12;n++) Poll(Installed);
  CHECK(Reinstalls>1 && Installed->ChildConnectionPending &&
        Commands==beforeCmd && !ChildPathInstalled);
  ChildConnectSucceeds=TRUE;
  for(unsigned n=0;n<100 && Installed->ChildConnectionPending;n++) Poll(Installed);
  CHECK(!Installed->ChildConnectionPending && ChildPathInstalled && ChildParentOpen &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock && Commands==beforeCmd);
  Dispose();

  // A same-slot path with stale capacity is not a connected child. It must be
  // torn down and re-created with the new CSD-derived LastBlock.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && ChildPathInstalled);
  ChildConnectSucceeds=FALSE;
  ChildMedia.LastBlock++;
  beforeCmd=Commands;
  Poll(Installed);
  CHECK(Installed->MediaReady && Installed->ChildConnectionPending &&
        !ChildPathInstalled && Commands==beforeCmd);
  ChildConnectSucceeds=TRUE;
  for(unsigned n=0;n<100 && Installed->ChildConnectionPending;n++) Poll(Installed);
  CHECK(!Installed->ChildConnectionPending && ChildPathInstalled &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock && Commands==beforeCmd);
  Dispose();

  // A matching path without BY_CHILD_CONTROLLER is an orphan, not proof that
  // SdDxe owns a usable child relationship.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && ChildPathInstalled && ChildParentOpen);
  ChildConnectSucceeds=FALSE;
  ChildParentOpen=FALSE;
  beforeCmd=Commands;
  Poll(Installed);
  CHECK(Installed->MediaReady && Installed->ChildConnectionPending &&
        !ChildPathInstalled && Commands==beforeCmd);
  ChildConnectSucceeds=TRUE;
  for(unsigned n=0;n<100 && Installed->ChildConnectionPending;n++) Poll(Installed);
  CHECK(!Installed->ChildConnectionPending && ChildPathInstalled && ChildParentOpen &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock && Commands==beforeCmd);
  Dispose();

  // An OpenProtocolInformation failure is unknown state and must not be
  // treated as a missing child or trigger unverified media publication.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && ChildPathInstalled);
  OpenInfoStatus=EFI_OUT_OF_RESOURCES;
  beforeCmd=Commands;
  Poll(Installed);
  CHECK(Installed->MediaReady && Installed->ChildConnectionPending && Reinstalls==0 &&
        Commands==beforeCmd && ChildPathInstalled);
  OpenInfoStatus=EFI_SUCCESS;
  for(unsigned n=0;n<100 && Installed->ChildConnectionPending;n++) Poll(Installed);
  CHECK(!Installed->ChildConnectionPending && Reinstalls==0 && Commands==beforeCmd);
  Dispose();

  // CSD v1 uses its byte-size and multiplier fields; its LastBlock must match
  // the same capacity reported by the child driver.
  Setup(); TestCsdStructure=0; Present=TRUE;
  AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed->MediaReady &&
        Installed->SdInfo.LastBlock==191 && ChildPathInstalled &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock && HasCsdBeforeSelect());
  Dispose();

  // The largest byte-addressed CSD v1 capacity fits exactly in 32-bit byte
  // addressing; one larger READ_BL_LEN must be rejected instead of wrapping.
  Setup(); TestCsdStructure=0; TestCsdV1Maximum=TRUE; Present=TRUE;
  AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed->MediaReady &&
        Installed->SdInfo.LastBlock==8388607 &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock);
  Dispose();
  Setup(); TestCsdStructure=1; TestCsdV2Maximum=TRUE; Present=TRUE;
  AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed->MediaReady &&
        Installed->SdInfo.LastBlock==0xffffffffULL &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock);
  Dispose();
  Setup(); TestCsdStructure=0; TestCsdV1Oversize=TRUE; Present=TRUE;
  AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady &&
        Installed->SdInfo.LastBlock==0 && !ChildPathInstalled);
  Dispose();

  // Reserved CSD structures must not publish a child with an invented size.
  Setup(); TestCsdStructure=2; Present=TRUE;
  AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady &&
        !Installed->MediaAnnounced && !Installed->ChildConnectionPending &&
        Installed->SdInfo.LastBlock==0 && !ChildPathInstalled);
  Dispose();

  // A tombstoned BlockIo child is not usable merely because its path and
  // parent/child open relation remain installed.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && ChildPathInstalled && ChildParentOpen);
  ChildConnectSucceeds=FALSE;
  ChildMedia.MediaPresent=FALSE;
  beforeCmd=Commands;
  Poll(Installed);
  CHECK(Installed->MediaReady && Installed->ChildConnectionPending &&
        !ChildPathInstalled && Commands==beforeCmd);
  ChildConnectSucceeds=TRUE;
  for(unsigned n=0;n<100 && Installed->ChildConnectionPending;n++) Poll(Installed);
  CHECK(!Installed->ChildConnectionPending && ChildPathInstalled &&
        ChildMedia.MediaPresent && ChildMedia.LastBlock==Installed->SdInfo.LastBlock &&
        Commands==beforeCmd);
  Dispose();

  // A child controller relation with the wrong SD slot path is not this
  // controller's child and cannot satisfy the connection check.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && ChildPathInstalled && ChildParentOpen);
  ChildConnectSucceeds=FALSE;
  ChildSdPath=GetChildSdPath();
  CHECK(ChildSdPath!=NULL && ChildSdPath->SlotNumber==0);
  ChildSdPath->SlotNumber=1;
  beforeCmd=Commands;
  Poll(Installed);
  CHECK(Installed->MediaReady && Installed->ChildConnectionPending &&
        !ChildPathInstalled && Commands==beforeCmd);
  ChildConnectSucceeds=TRUE;
  for(unsigned n=0;n<100 && Installed->ChildConnectionPending;n++) Poll(Installed);
  ChildSdPath=GetChildSdPath();
  CHECK(!Installed->ChildConnectionPending && ChildPathInstalled &&
        ChildSdPath!=NULL && ChildSdPath->SlotNumber==0 &&
        ChildMedia.LastBlock==Installed->SdInfo.LastBlock && Commands==beforeCmd);
  Dispose();

  // Removal/error while waiting for a command or data cannot return success
  // or drain a FIFO belonging to a later card.
  MSDC_PRIVATE_DATA p;
  Reset(&p); AutoClear=AutoCommand=1; RemoveOnSample=3;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_NO_MEDIA && Commands==1 &&
        !p.MediaReady && packet.TransactionStatus==EFI_NO_MEDIA);
  Reset(&p); AutoClear=AutoCommand=1; FailOnSample=3;
  CHECK(MsdcPassThru(&p.PassThru,0,&packet,NULL)==EFI_DEVICE_ERROR && Commands==1 &&
        !p.MediaReady);
  UINT8 data[64];
  Reset(&p); FifoBytes=64; AutoCommand=1; RemoveOnSample=1;
  CHECK(MsdcPioRead(&p,data,sizeof(data))==EFI_NO_MEDIA && FifoReads==0);
  Reset(&p); FifoBytes=64; AutoCommand=1; FailOnSample=1;
  CHECK(MsdcPioRead(&p,data,sizeof(data))==EFI_DEVICE_ERROR && FifoReads==0);

  // Event and protocol setup failures must close every allocated event.
  for(unsigned n=1;n<=4;n++) {
    Setup(); Present=FALSE;
    if(n<=2) FailEvent=n;
    else if(n==3) TimerStatus=EFI_DEVICE_ERROR;
    else InstallStatus=EFI_ACCESS_DENIED;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed==NULL &&
          InstalledDiagnostics!=NULL);
    CHECK(CloseCount==EventCount && Installs==0 && Writes==0);
    MSDC_DIAGNOSTICS_RECORD snapshot=ReadDiagnostics();
    CHECK(EFI_ERROR(snapshot.LastDriverStatus));
  }
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed->MediaReady &&
        Installed->MediaAnnounced && Reinstalls==0);
  Dispose();
}

UINTN EFIAPI AsciiSPrint(CHAR8 *buf,UINTN n,CONST CHAR8 *fmt,...) {
  va_list ap; va_start(ap,fmt); int result=vsnprintf(buf,n,fmt,ap); va_end(ap);
  return result<0?0:(UINTN)result;
}


static BOOLEAN CommandWasSent(UINT32 Command) {
  for (UINTN i=0;i<CommandHistoryCount;i++) if (CommandHistory[i]==Command) return TRUE;
  return FALSE;
}

static void CardReplacementIdentityTests(void) {
  const UINT32 Failures[]={8,41,3,9,7,13};
  for (UINTN Old=0;Old<2;Old++) for (UINTN f=0;f<ARRAY_SIZE(Failures);f++) {
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16; TestCsdStructure=(UINT8)Old;
    RcaReply=0x12340000;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed->MediaReady && ChildPathInstalled);
    CHECK(Installed->SdInfo.LastBlock==(Old?1023U:191U));
    Present=FALSE; Poll(Installed);
    CHECK(!Installed->MediaReady && Installed->SdInfo.CardType==UnknownCard &&
          Installed->SdInfo.LastBlock==0);
    for(UINTN n=0;n<103 && ChildPathInstalled;n++) Poll(Installed);
    CHECK(!ChildPathInstalled && !Installed->ChildWithdrawalPending);

    // Reuse this host instance, replacing the card and then failing initialization.
    TestCsdStructure=(UINT8)!Old; RcaReply=0xabcd0000;
    IfCondInterrupt=(!Old)?MSDC_INT_CMDRDY:MSDC_INT_CMDTMO;
    FailIdentifyCommand=Failures[f]; FailIdentifyInterrupt=MSDC_INT_CMDCRCERR;
    Present=TRUE; CommandHistoryCount=0; OpCondCount=0;
    Poll(Installed); Poll(Installed); Poll(Installed);
    CHECK(!Installed->MediaReady && !Installed->Initializing && !ChildPathInstalled);
    CHECK(Installed->SdInfo.CardType==UnknownCard && Installed->SdInfo.LastBlock==0);
    CHECK(Installed->IdentifyRetryCountdown>0);

    FailIdentifyCommand=MAX_UINT32; CommandHistoryCount=0; OpCondCount=0;
    for(UINTN n=0;n<103 && !Installed->MediaReady;n++) Poll(Installed);
    CHECK(Installed->MediaReady && ChildPathInstalled && !Installed->Initializing);
    CHECK(Installed->SdInfo.LastBlock==(Old?191U:1023U));
    CHECK(ChildMedia.LastBlock==Installed->SdInfo.LastBlock);
    CHECK(Installed->IdentifyRetryCountdown==0 && Installed->IdentifyRetryDelay==0);
    CHECK(((LastOpCondArg&BIT30)!=0)==(!Old));
    UINTN Addressed=0;
    for(UINTN n=0;n<CommandHistoryCount;n++) {
      if(CommandHistory[n]==9 || CommandHistory[n]==7 || CommandHistory[n]==13) {
        CHECK(CommandArguments[n]==0xabcd0000); Addressed++;
      }
    }
    CHECK(Addressed>=3);
    Dispose();
  }
}

static void SdIdentificationTests(void) {
  // Selection alone does not mean CMD13 reports ready transfer state.
  const UINT32 badStatus[]={0,4U<<9,BIT8|(3U<<9),BIT8|(7U<<9),
                           0x900|BIT23,0x900|BIT22,0x900|BIT21,0x900|BIT20,0x900|BIT19,
                           0x900|BIT23|BIT22|BIT19};
  for(UINTN i=0;i<ARRAY_SIZE(badStatus);i++) {
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16; StatusReply=badStatus[i];
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    CHECK(!Installed->MediaReady && !ChildPathInstalled && Installed->SdInfo.LastBlock==0);
    CHECK(!Installed->Initializing && !Installed->CommandActive);
    CHECK(mMsdcDiagnostics.LastInitStatus==((i<4)?EFI_TIMEOUT:EFI_DEVICE_ERROR));
    CHECK(StatusCalls==((i<4)?100U:1U));
    Dispose();
  }
  const UINT32 busyStatusCounts[]={0,1,99,100};
  for(UINTN i=0;i<ARRAY_SIZE(busyStatusCounts);i++) {
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16; StatusBusyReplies=busyStatusCounts[i]; StatusPendingReply=7U<<9;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    CHECK(Installed->MediaReady==(busyStatusCounts[i]<100));
    CHECK(ChildPathInstalled==(busyStatusCounts[i]<100));
    CHECK(StatusCalls==((busyStatusCounts[i]<100)?busyStatusCounts[i]+1:100));
    Dispose();
  }
  // CMD7 transport success must not hide card-reported selection errors.
  const UINT32 selectReplies[]={BIT23,BIT22,BIT19,BIT23|BIT22|BIT19,0,BIT8|(4U<<9)};
  for(UINTN i=0;i<ARRAY_SIZE(selectReplies);i++) {
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16; SelectReply=selectReplies[i];
    BOOLEAN valid=(i>=4);
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    CHECK(Installed->MediaReady==valid && ChildPathInstalled==valid);
    CHECK(CommandWasSent(7) && CommandWasSent(13)==valid);
    CHECK(!Installed->Initializing && !Installed->CommandActive);
    if(!valid) CHECK(mMsdcDiagnostics.LastInitStatus==EFI_DEVICE_ERROR &&
                    Installed->SdInfo.LastBlock==0 && Installed->SdInfo.CardType==UnknownCard);
    Dispose();
  }
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
  Regs[MSDC_PS/4]=BIT16; SelectBusyForever=TRUE;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
  CHECK(!Installed->MediaReady && !ChildPathInstalled && !CommandWasSent(13));
  CHECK(mMsdcDiagnostics.LastInitStatus==EFI_TIMEOUT && Installed->SdInfo.LastBlock==0);
  CHECK(!Installed->Initializing && !Installed->CommandActive);
  Dispose();
  // R1 COM_CRC_ERROR must not authorize ACMD41 even with APP_CMD set.
  const UINT32 appReplies[]={0,BIT5,BIT22,BIT5|BIT22,BIT23,BIT5|BIT23,BIT5|BIT22|BIT23};
  const UINT32 transportFlags[]={MSDC_INT_CMDRDY,MSDC_INT_CMDCRCERR,
                                MSDC_INT_CMDTMO,MSDC_INT_CMDTMO|MSDC_INT_CMDCRCERR};
  for(UINTN r=0;r<ARRAY_SIZE(appReplies);r++) for(UINTN f=0;f<ARRAY_SIZE(transportFlags);f++) {
    MSDC_PRIVATE_DATA p;
    UINT32 ocr=0x12345678;
    Reset(&p); AutoClear=AutoCommand=AutoIdentify=1;
    AppCmdReply=appReplies[r]; FailIdentifyCommand=55; FailIdentifyInterrupt=transportFlags[f];
    EFI_STATUS expected=(f==2)?EFI_TIMEOUT:(f!=0)?EFI_CRC_ERROR:
                         (appReplies[r]==BIT5)?EFI_SUCCESS:EFI_DEVICE_ERROR;
    CHECK(SdCardSendOpCond(&p.PassThru,0,0xff8000,FALSE,FALSE,TRUE,&ocr)==expected);
    CHECK(Commands==((expected==EFI_SUCCESS)?2U:1U));
    if(expected!=EFI_SUCCESS) CHECK(ocr==0x12345678);
  }
  for(UINTN f=1;f<ARRAY_SIZE(transportFlags);f++) {
    MSDC_PRIVATE_DATA p;
    UINT32 ocr=0x12345678;
    Reset(&p); AutoClear=AutoCommand=AutoIdentify=1; AppCmdReply=BIT5;
    FailIdentifyCommand=41; FailIdentifyInterrupt=transportFlags[f];
    CHECK(SdCardSendOpCond(&p.PassThru,0,0xff8000,FALSE,FALSE,TRUE,&ocr)==
          ((f==2)?EFI_TIMEOUT:EFI_CRC_ERROR));
    CHECK(Commands==2 && ocr==0x12345678);
  }
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
  Regs[MSDC_PS/4]=BIT16; AppCmdReply=BIT5|BIT23;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady &&
        !ChildPathInstalled && !CommandWasSent(41));
  CHECK(mMsdcDiagnostics.LastInitStatus==EFI_DEVICE_ERROR);
  Dispose();
  // Mixed transport flags must not turn a CRC-corrupt CMD8 into legacy fallback.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
  Regs[MSDC_PS/4]=BIT16;
  IfCondInterrupt=MSDC_INT_CMDTMO|MSDC_INT_CMDCRCERR;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady &&
        !ChildPathInstalled && !CommandWasSent(41));
  CHECK(mMsdcDiagnostics.LastInitStatus==EFI_CRC_ERROR);
  Dispose();

  // Exhaust all R7 echo/voltage bits and the illegal-command flag.
  for(UINT32 illegal=0;illegal<2;illegal++) for(UINT32 low=0;low<4096;low++) {
    MSDC_PRIVATE_DATA p;
    Reset(&p); AutoClear=AutoCommand=AutoIdentify=1;
    IfCondReply=low|(illegal?BIT22:0); IfCondInterrupt=MSDC_INT_CMDRDY;
    CHECK(SdSendIfCond(&p.PassThru)==((low==0x1aa && !illegal)?EFI_SUCCESS:EFI_DEVICE_ERROR));
    CHECK(Commands==1 && CommandHistory[0]==8);
  }

  // Legacy CMD8 timeout keeps HCS/XPC clear and cannot admit CSD v2.
  for(UINTN structure=0;structure<2;structure++) for(UINTN flip=0;flip<2;flip++) {
    Setup(); TestCsdStructure=(UINT8)structure; FlipCcs=(BOOLEAN)flip;
    Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
    IfCondInterrupt=MSDC_INT_CMDTMO;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    CHECK((LastOpCondArg&(BIT30|BIT28))==0);
    CHECK(Installed->MediaReady==(structure==0));
    CHECK(ChildPathInstalled==(structure==0));
    if(structure!=0) CHECK(mMsdcDiagnostics.LastInitStatus==EFI_UNSUPPORTED && !CommandWasSent(7));
    Dispose();
  }
  // CCS from ready ACMD41 and CSD structure must agree on address units.
  for(UINTN structure=0;structure<2;structure++) {
    Setup(); TestCsdStructure=(UINT8)structure; FlipCcs=TRUE;
    Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    CHECK(!Installed->MediaReady && !ChildPathInstalled && Installed->SdInfo.LastBlock==0);
    CHECK(mMsdcDiagnostics.LastInitStatus==EFI_UNSUPPORTED && !CommandWasSent(7));
    Dispose();
  }
  const UINT32 busyCounts[]={0,1,100,101};
  for(UINTN i=0;i<ARRAY_SIZE(busyCounts);i++) {
    Setup(); OpCondBusyResponses=busyCounts[i];
    Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    if(busyCounts[i]<=100) {
      CHECK(Installed->MediaReady && ChildPathInstalled);
      CHECK(OpCondCount==((busyCounts[i]<2)?2:busyCounts[i]+1));
    } else {
      CHECK(!Installed->MediaReady && !ChildPathInstalled && !CommandWasSent(2));
      CHECK(OpCondCount==101 && mMsdcDiagnostics.LastInitStatus==EFI_TIMEOUT);
      CHECK(TimeNs>=1000000000ULL && TimeNs<2000000000ULL);
    }
    Dispose();
  }
  // Transport failure at each identification stage must not publish media.
  const UINT32 failCommands[]={8,55,41,2,3,9,7,13};
  const UINT32 successPrefix[]={0,8,55,41,55,41,2,3,9,7};
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
  Regs[MSDC_PS/4]=BIT16;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed->MediaReady && ChildPathInstalled);
  // T70 stays on the reset-default one-bit bus; no ACMD6 transition exists.
  CHECK(!CommandWasSent(6));
  CHECK(((Regs[SDC_CFG/4]>>SDC_CFG_BUS_WIDTH_SHIFT)&3U)==MSDC_BUS_WIDTH_1);
  CHECK(CommandHistoryCount>=ARRAY_SIZE(successPrefix));
  for(UINTN n=0;n<ARRAY_SIZE(successPrefix);n++) CHECK(CommandHistory[n]==successPrefix[n]);
  Dispose();
  for(UINTN i=0;i<ARRAY_SIZE(failCommands);i++) for(UINTN timeout=0;timeout<2;timeout++) {
    if(timeout && failCommands[i]==8) continue; // Existing legacy-card fallback test below.
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16;
    FailIdentifyCommand=failCommands[i];
    FailIdentifyInterrupt=timeout?MSDC_INT_CMDTMO:MSDC_INT_CMDCRCERR;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    CHECK(!Installed->MediaReady && !Installed->Initializing &&
          Installed->SdInfo.CardType==UnknownCard && Installed->SdInfo.LastBlock==0);
    CHECK(!ChildPathInstalled && !ChildBlockIoInstalled);
    CHECK(Installed->IdentifyRetryCountdown>0 && Installed->IdentifyRetryDelay>0);
    CHECK(mMsdcDiagnostics.LastInitStatus==(timeout?EFI_TIMEOUT:EFI_CRC_ERROR));
    CHECK(CommandHistoryCount>0 && CommandHistory[CommandHistoryCount-1]==failCommands[i]);
    FailIdentifyCommand=MAX_UINT32;
    for(UINTN n=0;n<103 && !Installed->MediaReady;n++) Poll(Installed);
    CHECK(Installed->MediaReady && ChildPathInstalled && ChildBlockIoInstalled);
    CHECK(!Installed->Initializing && Installed->IdentifyRetryCountdown==0);
    Dispose();
  }
  // Bad cards (or a tray-detect signal without usable media) must not cause
  // controller resets every 60 ms. Removal cancels the bounded backoff.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
  Regs[MSDC_PS/4]=BIT16; IfCondReply=0x1ab;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady);
  for (UINTN n=0;n<100;n++) Poll(Installed);
  CHECK(GpioInits<=5 && !Installed->MediaReady && !ChildPathInstalled);
  CHECK(Installed->IdentifyRetryCountdown>0);
  Present=FALSE; Poll(Installed);
  CHECK(Installed->IdentifyRetryCountdown==0 && Installed->IdentifyRetryDelay==0);
  Present=TRUE; IfCondReply=0x1aa;
  Poll(Installed); Poll(Installed); CHECK(!Installed->MediaReady);
  Poll(Installed); CHECK(Installed->MediaReady && ChildPathInstalled);
  Dispose();
  // A successful transport is not a valid CMD8 echo. Do not reach ACMD41
  // or publish a card after a voltage/pattern/error-status mismatch.
  const UINT32 BadIfCond[]={0,0x1ab,0x2aa,0x1aa|BIT22};
  for (UINTN i=0;i<sizeof(BadIfCond)/sizeof(BadIfCond[0]);i++) {
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16; IfCondReply=BadIfCond[i];
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady &&
          !ChildPathInstalled && CommandWasSent(8) && !CommandWasSent(41));
    IfCondReply=0x1aa;
    for (UINTN n=0;n<103 && !Installed->MediaReady;n++) Poll(Installed);
    CHECK(Installed->MediaReady && ChildPathInstalled);
    Dispose();
  }

  // A pre-2.0 SD card has no CMD8 reply. Only that timeout permits the
  // legacy path; HCS and XPC must remain clear on the subsequent ACMD41.
  Setup(); TestCsdStructure=0; Present=TRUE;
  AutoClear=AutoCommand=AutoIdentify=1; Regs[MSDC_PS/4]=BIT16;
  IfCondInterrupt=MSDC_INT_CMDTMO;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed->MediaReady &&
        Installed->SdInfo.LastBlock==191 && ChildPathInstalled &&
        CommandWasSent(41) && (LastOpCondArg&(BIT30|BIT28))==0);
  Dispose();

  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
  Regs[MSDC_PS/4]=BIT16; IfCondInterrupt=MSDC_INT_CMDCRCERR;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady &&
        !ChildPathInstalled && !CommandWasSent(41));
  Dispose();

  // CMD55 must acknowledge APP_CMD. An illegal-command response cannot
  // authorize the following command even when the transport succeeded.
  const UINT32 BadAppCmd[]={0,BIT5|BIT22};
  for (UINTN i=0;i<sizeof(BadAppCmd)/sizeof(BadAppCmd[0]);i++) {
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16; AppCmdReply=BadAppCmd[i];
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && !Installed->MediaReady &&
          !ChildPathInstalled && CommandWasSent(55) && !CommandWasSent(41));
    Dispose();
  }

  // All R6 error combinations must leave the caller's RCA untouched.
  const UINT32 rcas[]={0,1,0xffff};
  for(UINTN r=0;r<ARRAY_SIZE(rcas);r++) for(UINT32 errors=0;errors<8;errors++) {
    UINT32 reply=(rcas[r]<<16)|(errors<<13)|0x1fff;
    BOOLEAN valid=(rcas[r]!=0 && errors==0);
    for(UINTN f=0;f<ARRAY_SIZE(transportFlags);f++) {
      MSDC_PRIVATE_DATA p;
      UINT16 rca=0xa55a;
      Reset(&p); AutoClear=AutoCommand=AutoIdentify=1;
      RcaReply=reply; FailIdentifyCommand=3; FailIdentifyInterrupt=transportFlags[f];
      EFI_STATUS expected=(f==2)?EFI_TIMEOUT:(f!=0)?EFI_CRC_ERROR:
                           valid?EFI_SUCCESS:EFI_DEVICE_ERROR;
      CHECK(SdCardSetRca(&p.PassThru,&rca)==expected);
      CHECK(rca==((expected==EFI_SUCCESS)?rcas[r]:0xa55a));
      CHECK(Commands==1 && CommandHistory[0]==3);
    }
    Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
    Regs[MSDC_PS/4]=BIT16; RcaReply=reply;
    CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
    CHECK(Installed->MediaReady==valid && ChildPathInstalled==valid);
    CHECK(CommandWasSent(3) && CommandWasSent(9)==valid && CommandWasSent(7)==valid);
    if(!valid) {
      CHECK(Installed->SdInfo.LastBlock==0 && Installed->SdInfo.CardType==UnknownCard);
      CHECK(!Installed->Initializing && !Installed->CommandActive);
      CHECK(mMsdcDiagnostics.LastInitStatus==EFI_DEVICE_ERROR);
    }
    Dispose();
  }
}

static MSDC_DIAGNOSTICS_RECORD ReadDiagnostics(void) {
  MSDC_DIAGNOSTICS_RECORD snapshot;
  UINT32 required=0;
  CHECK(InstalledDiagnostics!=NULL);
  ZeroMem(&snapshot,sizeof(snapshot));
  CHECK(InstalledDiagnostics->GetSnapshot(InstalledDiagnostics,sizeof(snapshot),
                                           &required,&snapshot)==EFI_SUCCESS);
  CHECK(required==sizeof(snapshot));
  return snapshot;
}

static void DiagnosticsTests(void) {
  MSDC_DIAGNOSTICS_RECORD snapshot;
  EFI_DEVICE_PATH_PROTOCOL *path;
  UINT32 required=0;
  unsigned detectBefore;

  // Absent media is distinguished from a GPIO API failure before
  // BuildDevicePath collapses it to EFI_NOT_FOUND. Snapshot is cache-only.
  Setup(); Present=FALSE;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL && InstalledDiagnostics!=NULL);
  path=(VOID *)1;
  CHECK(MsdcBuildDevicePath(&Installed->PassThru,0,&path)==EFI_NOT_FOUND && path==NULL);
  detectBefore=DetectCalls;
  snapshot=ReadDiagnostics();
  CHECK(snapshot.DetectSampleValid && snapshot.CardDetectStatus==EFI_SUCCESS &&
        !snapshot.CardPresent && snapshot.DevicePathStatus==EFI_NO_MEDIA);
  CHECK(DetectCalls==detectBefore && Commands==0 && Writes==0);

  // The snapshot contract works at application and notify TPL without
  // lowering TPL or triggering another card sample.
  CurrentTpl=TPL_NOTIFY;
  detectBefore=DetectCalls;
  snapshot=ReadDiagnostics();
  CHECK(CurrentTpl==TPL_NOTIFY && DetectCalls==detectBefore);
  CurrentTpl=TPL_APPLICATION;
  CHECK(InstalledDiagnostics->GetSnapshot(InstalledDiagnostics,0,&required,&snapshot)==
        EFI_BUFFER_TOO_SMALL && required==sizeof(snapshot));
  CHECK(InstalledDiagnostics->GetSnapshot(InstalledDiagnostics,sizeof(snapshot),
                                           NULL,&snapshot)==EFI_INVALID_PARAMETER);
  CHECK(InstalledDiagnostics->GetSnapshot(InstalledDiagnostics,sizeof(snapshot),
                                           &required,NULL)==EFI_INVALID_PARAMETER);
  Dispose();

  // A failed SD identification remains visible after event setup and host
  // publication succeed; driver status must not overwrite media status.
  Setup(); Present=TRUE; AutoClear=AutoCommand=AutoIdentify=1;
  Regs[MSDC_PS/4]=BIT16; IfCondInterrupt=MSDC_INT_CMDCRCERR;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && Installed!=NULL);
  snapshot=ReadDiagnostics();
  CHECK(snapshot.LastInitStage==MsdcDiagStageInterfaceCondition &&
        snapshot.LastInitStatus==EFI_CRC_ERROR);
  CHECK(snapshot.LastDriverStage==MsdcDiagStageControllerPublish &&
        snapshot.LastDriverStatus==EFI_SUCCESS && !snapshot.MediaReady);
  Dispose();

  // Early post-install setup failure returns success so the image and its
  // diagnostic interface remain loaded and callable.
  Setup(); gPlatformInfo.HostMask=0;
  CHECK(InitMsdc(NULL,NULL)==EFI_SUCCESS && InstalledDiagnostics!=NULL && Installed==NULL);
  snapshot=ReadDiagnostics();
  CHECK(snapshot.LastDriverStage==MsdcDiagStageDriverConfig &&
        snapshot.LastDriverStatus==EFI_INVALID_PARAMETER);
  gPlatformInfo.HostMask=2;

  // If the diagnostic interface itself cannot be installed, propagate that
  // error and do not pretend it can be queried.
  Setup(); DiagnosticInstallStatus=EFI_DEVICE_ERROR;
  CHECK(InitMsdc(NULL,NULL)==EFI_DEVICE_ERROR && InstalledDiagnostics==NULL);
}
