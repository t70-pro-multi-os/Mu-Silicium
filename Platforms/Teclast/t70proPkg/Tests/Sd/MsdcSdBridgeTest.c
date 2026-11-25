// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual SdDxe BlockIo -> command callback -> actual MsdcSendCmd -> fake FIFO.
// This intentionally tests below the unchanged product PassThru read-only gate.
#define main MsdcComponentMain
#include "../MsdcTest.c"
#undef main
#include <unistd.h>
#include <sys/stat.h>
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Sd/SdDxe/SdDxe.h"
VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
static MSDC_PRIVATE_DATA BridgeHost;
static unsigned BridgeWrites,BridgeStatuses,BridgeStalls,BridgeReads;
static UINT32 ReadLba,ReadSize;
static UINT8 ReadFixture[1024];
static int CardImageFd=-1;
static EFI_STATUS EFIAPI BridgeStall(UINTN Us){BridgeStalls++;MicroSecondDelay(Us);return 0;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return FALSE;}
static EFI_STATUS EFIAPI BridgePath(EFI_SD_MMC_PASS_THRU_PROTOCOL *This,UINT8 Slot,EFI_DEVICE_PATH_PROTOCOL **Path){
 (void)This;return MsdcBuildDevicePath(&BridgeHost.PassThru,Slot,Path);
}
static EFI_STATUS EFIAPI BridgeCommand(EFI_SD_MMC_PASS_THRU_PROTOCOL *This,UINT8 Slot,EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,EFI_EVENT Event){
 (void)This;CHECK(Slot==0&&!Event);
 UINT32 Cmd=Packet->SdMmcCmdBlk->CommandIndex;
 if(Cmd==17||Cmd==18){
  CHECK((CardImageFd>=0 || (!BridgeWrites&&!BridgeStatuses))&&!BridgeReads);
  CHECK(Cmd==(ReadSize==512?17U:18U)&&Packet->SdMmcCmdBlk->CommandArgument==ReadLba);
  CHECK(Packet->InTransferLength==ReadSize&&!Packet->OutTransferLength);
  BridgeReads++;StreamRemaining=ReadSize;TxStreaming=FALSE;
  if(CardImageFd>=0)CHECK(pread(CardImageFd,ReadFixture,ReadSize,(UINT64)ReadLba*512)==(ssize_t)ReadSize);
  RxSource=ReadFixture;RxSourceSize=ReadSize;RxSourcePosition=0;
 }else if(Cmd==25){
  CHECK(!BridgeWrites&&!BridgeStatuses&&Packet->OutTransferLength==1024&&Packet->SdMmcCmdBlk->CommandArgument==9);
  BridgeWrites++;TxStreaming=TRUE;TxExpected=Packet->OutTransferLength;
 }else{
  CHECK(Cmd==13&&BridgeWrites==1&&TxQueued==1024&&TxDrained==1024);
  CHECK(Packet->SdMmcCmdBlk->CommandArgument==(1U<<16));
  BridgeStatuses++;TxStreaming=FALSE;
 }
 BridgeHost.StartNs=TimeNs;BridgeHost.TimeoutNs=100000000;
 EFI_STATUS S=MsdcSendCmd(&BridgeHost,Packet);
 if(Cmd==25&&S==EFI_SUCCESS){
  CHECK((LastCommand&(SDC_CMD_RW|SDC_CMD_AUTO12|SDC_CMD_MULTIPLE_BLK))==
        (SDC_CMD_RW|SDC_CMD_AUTO12|SDC_CMD_MULTIPLE_BLK));
  CHECK(Regs[SDC_BLK_NUM/4]==2&&TxDrained==1024);
  // The synthetic card persists ONLY bytes actually accepted by the TX FIFO.
  if(CardImageFd>=0)CHECK(pwrite(CardImageFd,TxPayload,TxDrained,9*512)==(ssize_t)TxDrained);
 }
 if((Cmd==17||Cmd==18)&&S==EFI_SUCCESS){
  CHECK(!(LastCommand&SDC_CMD_RW));
  CHECK((LastCommand&(SDC_CMD_SINGLE_BLK|SDC_CMD_MULTIPLE_BLK))==
        (ReadSize==512?SDC_CMD_SINGLE_BLK:SDC_CMD_MULTIPLE_BLK));
  CHECK(((LastCommand&SDC_CMD_AUTO12)!=0)==(ReadSize==1024));
  CHECK(Regs[SDC_BLK_NUM/4]==ReadSize/512&&RxSourcePosition==ReadSize);
 }
 Packet->TransactionStatus=S;return S;
}
static void ImageRoundTrip(int Fd){
 struct stat St;CHECK(fstat(Fd,&St)==0&&S_ISREG(St.st_mode)&&St.st_nlink==1&&St.st_size==8192);
 CardImageFd=Fd;
 Reset(&BridgeHost);AutoClear=AutoCommand=AutoIdentify=1;
 StatusCalls=StatusBusyReplies=0;StatusReply=BIT8|(4U<<9);
 BridgeWrites=BridgeStatuses=BridgeStalls=BridgeReads=0;CurrentTpl=TPL_APPLICATION;
 EFI_SD_MMC_PASS_THRU_PROTOCOL Pass={0};Pass.PassThru=BridgeCommand;Pass.BuildDevicePath=BridgePath;
 SD_DRIVER_PRIVATE_DATA Private={0};Private.PassThru=&Pass;
 SD_DEVICE Device={0};Device.Signature=SD_DEVICE_SIGNATURE;Device.Private=&Private;
 Device.SectorAddressing=TRUE;Device.Rca=1;Device.BlockMedia.MediaId=7;
 Device.BlockMedia.MediaPresent=TRUE;Device.BlockMedia.BlockSize=512;Device.BlockMedia.LastBlock=15;
 Device.BlockIo.Media=&Device.BlockMedia;InitializeListHead(&Device.Queue);InitializeListHead(&Device.OrderedIo);
 UINT32 Payload[256],Readback[256];UINT8 *Bytes=(VOID *)Payload;
 for(unsigned N=0;N<sizeof(Payload);N++)Bytes[N]=(UINT8)(N*37+11);
 CHECK(SdWriteBlocks(&Device.BlockIo,7,9,sizeof(Payload),Payload)==EFI_SUCCESS);
 CHECK(BridgeWrites==1&&BridgeStatuses==1&&TxQueued==1024&&TxDrained==1024);
 CHECK(SdFlushBlocks(&Device.BlockIo)==EFI_SUCCESS);
 // Host fsync is image durability, not an assertion about physical SD storage.
 CHECK(fsync(Fd)==0);
 memset(Readback,0xa5,sizeof(Readback));ReadLba=9;ReadSize=sizeof(Readback);
 CHECK(SdReadBlocks(&Device.BlockIo,7,9,sizeof(Readback),Readback)==EFI_SUCCESS);
 CHECK(!memcmp(Readback,Payload,sizeof(Payload))&&RxSourcePosition==1024&&BridgeReads==1);
 CHECK(CommandHistoryCount==3&&CommandHistory[0]==25&&CommandHistory[1]==13&&CommandHistory[2]==18);
 CHECK(!Device.ActiveOperations&&!Device.WriteStatusPending&&gPlatformInfo.ReadOnly);
 CHECK(close(Fd)==0);CardImageFd=-1;
 printf("Private card image: CMD25 -> CMD13 -> FlushBlocks -> CMD18, 1024 FIFO bytes matched\n");
}
int main(int Argc,char **Argv){
 Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;Services.Stall=BridgeStall;
 for(unsigned Busy=0;Busy<3;Busy++){
  Reset(&BridgeHost);AutoClear=AutoCommand=AutoIdentify=1;
  StatusCalls=0;StatusReply=BIT8|(4U<<9);StatusPendingReply=7U<<9;StatusBusyReplies=Busy;
  BridgeWrites=BridgeStatuses=BridgeStalls=0;CurrentTpl=TPL_APPLICATION;
  EFI_SD_MMC_PASS_THRU_PROTOCOL Pass={0};Pass.PassThru=BridgeCommand;Pass.BuildDevicePath=BridgePath;
  SD_DRIVER_PRIVATE_DATA Private={0};Private.PassThru=&Pass;
  SD_DEVICE Device={0};Device.Signature=SD_DEVICE_SIGNATURE;Device.Private=&Private;
  Device.SectorAddressing=TRUE;Device.Rca=1;Device.BlockMedia.MediaId=7;
  Device.BlockMedia.MediaPresent=TRUE;Device.BlockMedia.BlockSize=512;Device.BlockMedia.LastBlock=1023;
  Device.BlockIo.Media=&Device.BlockMedia;InitializeListHead(&Device.Queue);InitializeListHead(&Device.OrderedIo);
  UINT32 Payload[256];UINT8 *Bytes=(VOID *)Payload;
  for(unsigned N=0;N<sizeof(Payload);N++)Bytes[N]=(UINT8)(N*37+11);
  CHECK(SdWriteBlocks(&Device.BlockIo,7,9,sizeof(Payload),Payload)==EFI_SUCCESS);
  CHECK(BridgeWrites==1&&BridgeStatuses==Busy+1&&BridgeStalls==Busy);
  CHECK(!memcmp(TxPayload,Bytes,sizeof(Payload))&&TxQueued==1024&&TxDrained==1024&&!TxPending);
  CHECK(CommandHistoryCount==Busy+2&&CommandHistory[0]==25);
  for(unsigned N=1;N<CommandHistoryCount;N++)CHECK(CommandHistory[N]==13);
  CHECK(!Device.WriteInProgress&&!Device.WriteStatusPending&&!Device.ActiveOperations);
  CHECK(SdFlushBlocks(&Device.BlockIo)==EFI_SUCCESS&&BridgeStatuses==Busy+1);
  CHECK(gPlatformInfo.ReadOnly&&CurrentTpl==TPL_APPLICATION&&GpioInits==0&&PowerOns==0);
 }
 unsigned ReadCases=0;
 const UINT32 Lbas[]={0,9,1022};
 for(unsigned Blocks=1;Blocks<=2;Blocks++)for(unsigned L=0;L<ARRAY_SIZE(Lbas);L++)for(unsigned Offset=0;Offset<4;Offset++){
  Reset(&BridgeHost);AutoClear=AutoCommand=AutoIdentify=1;
  BridgeWrites=BridgeStatuses=BridgeStalls=BridgeReads=0;CurrentTpl=TPL_APPLICATION;
  ReadLba=Lbas[L];ReadSize=Blocks*512;
  for(unsigned N=0;N<ReadSize;N++)ReadFixture[N]=(UINT8)((ReadLba+N/512)*53U+(N%512)*37U+N/7U+11U);
  EFI_SD_MMC_PASS_THRU_PROTOCOL Pass={0};Pass.PassThru=BridgeCommand;Pass.BuildDevicePath=BridgePath;
  SD_DRIVER_PRIVATE_DATA Private={0};Private.PassThru=&Pass;
  SD_DEVICE Device={0};Device.Signature=SD_DEVICE_SIGNATURE;Device.Private=&Private;
  Device.SectorAddressing=TRUE;Device.Rca=1;Device.BlockMedia.MediaId=7;
  Device.BlockMedia.MediaPresent=TRUE;Device.BlockMedia.BlockSize=512;Device.BlockMedia.LastBlock=1023;
  Device.BlockIo.Media=&Device.BlockMedia;InitializeListHead(&Device.Queue);InitializeListHead(&Device.OrderedIo);
  UINT32 Storage[260];UINT8 *All=(VOID *)Storage,*Buffer=All+4+Offset;memset(Storage,0xa5,sizeof(Storage));
  CHECK(SdReadBlocks(&Device.BlockIo,7,ReadLba,ReadSize,Buffer)==EFI_SUCCESS);
  CHECK(BridgeReads==1&&!BridgeWrites&&!BridgeStatuses&&!BridgeStalls);
  CHECK(!memcmp(Buffer,ReadFixture,ReadSize)&&RxSourcePosition==ReadSize&&!StreamRemaining&&!FifoBytes);
  for(unsigned N=0;N<sizeof(Storage);N++)if(N<4+Offset||N>=4+Offset+ReadSize)CHECK(All[N]==0xa5);
  CHECK(CommandHistoryCount==1&&CommandHistory[0]==(Blocks==1?17U:18U));
  CHECK(!Device.ActiveOperations&&!Device.WriteInProgress&&!Device.WriteStatusPending);
  CHECK(gPlatformInfo.ReadOnly&&CurrentTpl==TPL_APPLICATION&&!GpioInits&&!PowerOns);
  ReadCases++;
 }
 CHECK(ReadCases==24);
 CHECK(Argc==1||Argc==2);if(Argc==2)ImageRoundTrip(atoi(Argv[1]));
 printf("SD DXE to MSDC: %u checks passed; 3 write/status + 24 variable RX FIFO scenarios, no hardware\n",Checks);return 0;
}
