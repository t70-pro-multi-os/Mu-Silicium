// SPDX-License-Identifier: BSD-2-Clause-Patent
// Test-only ABI. Version-script localization isolates all EFI/mock symbols.
#ifndef MSDC_TEST_TX_CAPACITY
#define MSDC_TEST_TX_CAPACITY 65536
#endif
#define main UnusedMsdcMain
#include "../MsdcTest.c"
#undef main
#include <unistd.h>
#include <sys/stat.h>
static int ImageFd=-1;
static UINT8 Input[MSDC_TEST_TX_CAPACITY];
static UINT64 Counts[5]; // commands, RX bytes, TX bytes, CMD13, CMD25
static BOOLEAN Pending;
int T70MsdcImageInit(int Fd){
 struct stat S;if(fstat(Fd,&S)||!S_ISREG(S.st_mode)||S.st_size!=32*1024*1024||S.st_nlink!=1)return -1;
 ImageFd=Fd;Pending=FALSE;memset(Counts,0,sizeof Counts);
 Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;return 0;
}
EFI_STATUS T70MsdcImagePacket(EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *P){
 CHECK(ImageFd>=0&&P&&P->SdMmcCmdBlk&&P->SdMmcStatusBlk);
 UINT32 Cmd=P->SdMmcCmdBlk->CommandIndex;BOOLEAN Write=Cmd==24||Cmd==25,Read=Cmd==17||Cmd==18;
 CHECK(Write||Read||Cmd==13);CHECK(Cmd==13?Pending:!Pending);
 UINT32 N=Write?P->OutTransferLength:P->InTransferLength;
 UINT64 Offset=(UINT64)P->SdMmcCmdBlk->CommandArgument*512;
 if(Read||Write)CHECK(N&&N<=sizeof(Input)&&!(N%512)&&Offset<=32*1024*1024ULL-N);
 MSDC_PRIVATE_DATA Host;Reset(&Host);AutoClear=AutoCommand=AutoIdentify=1;
 Host.TimeoutNs=1000000000;StatusCalls=StatusBusyReplies=0;StatusReply=BIT8|(4U<<9);
 if(Write){TxStreaming=TRUE;TxExpected=N;}
 if(Read){CHECK(pread(ImageFd,Input,N,Offset)==N);RxSource=Input;RxSourceSize=N;StreamRemaining=N;}
 EFI_STATUS S=MsdcSendCmd(&Host,P);P->TransactionStatus=S;Counts[0]++;
 if(EFI_ERROR(S))return S;
 if(Write){
  CHECK(TxQueued==N&&TxDrained==N&&!TxPending);
  CHECK((LastCommand&SDC_CMD_RW)&&((!!(LastCommand&SDC_CMD_AUTO12))==(Cmd==25)));
  CHECK(pwrite(ImageFd,TxPayload,N,Offset)==N);Counts[2]+=N;Pending=TRUE;if(Cmd==25)Counts[4]++;
 }else if(Read){CHECK(RxSourcePosition==N&&!StreamRemaining&&!FifoBytes);Counts[1]+=N;}
 else {CHECK(P->SdMmcStatusBlk->Resp0==(BIT8|(4U<<9)));Pending=FALSE;Counts[3]++;}
 CHECK(gPlatformInfo.ReadOnly&&!GpioInits&&!PowerOns);return S;
}
void T70MsdcImageStats(UINT64 *Out){CHECK(!Pending);memcpy(Out,Counts,sizeof Counts);}
