// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Library/HimaxCore.h>
static void Checksum(unsigned char *D,unsigned N) { unsigned S=0,I; D[N-1]=0; for(I=0;I<N;I++)S+=D[I]; D[N-1]=(unsigned char)-S; }

typedef struct {
  unsigned Calls, DelayCalls, FailFirst, FailAlways, ReadLength;
  unsigned ApAckMisses, ApAckReads;
  unsigned BadReadbacks, Readbacks, FailAt, FailFrom;
  unsigned FlashMode, FlashProgramReady, FlashWipBusyReads, FlashCrcBusyReads;
  unsigned FlashControl, FlashProgramControls, FlashWipCommands;
  unsigned FlashCrcResult, FlashCrcStatusReads, FlashCrcOutputReads;
  unsigned FlashWipReads, FlashPage16Writes, FlashPage240Writes;
  unsigned FlashPageOffsetWrites, FlashPageOffsetErrors, FlashPageMismatch, FlashResetWrites;
  unsigned FlashFailAddress, FlashFailAddressAlways, FlashFailPayloadLength, FlashCurrentOffset;
  unsigned FlashCrcStart, FlashCrcWords, FlashCrcStartWrites, FlashCrcLengthWrites, FlashCrcWordTailErrors;
  const unsigned char *FlashImage;
  unsigned SenseOffMode, SenseOffBadStart, SenseOffStopTimeout;
  unsigned SenseOffBroadcastBusy, SenseOffBroadcastPhase, SenseOffFinalState, SenseOffBusyLimit;
  unsigned SenseOff5cReads, SenseOffA8Reads;
  unsigned SenseOffBroadcastReads, SenseOffCommandWrites;
  unsigned SenseOffResetWrites, SenseOffStopWrites, SenseOffControlWrites;
  unsigned SenseOff84Writes;
  unsigned SenseOffConfigWrites, SenseOff3100Writes, SenseOff3200Writes;
  unsigned FailReadAddress, SelectedAddress;
  unsigned RetainedFirmwareState;
  unsigned DelayMinimum[128], DelayMaximum[128];
  unsigned WriteLength[128], ReadSize[128];
  unsigned char Writes[128][HM_BUS_WRITE_MAX_BODY+1];
  unsigned char ReadBytes[HM_BUS_READ_MAX_BYTES];
} HM_MOCK_IO;
static HM_STATUS MockTransfer(void *Context,const unsigned char *WriteData,
                              unsigned WriteLength,unsigned char *ReadData,
                              unsigned ReadLength) {
  HM_MOCK_IO *M=(HM_MOCK_IO *)Context; unsigned Index=M->Calls++,Address=0,PayloadLength=0;
  CHECK(WriteLength<=HM_BUS_WRITE_MAX_BODY+1&&ReadLength<=HM_BUS_READ_MAX_BYTES);
  if (Index<128) {
    M->WriteLength[Index]=WriteLength; M->ReadSize[Index]=ReadLength;
    memcpy(M->Writes[Index],WriteData,WriteLength);
  }
  if (WriteLength>=5 && WriteData[0]==0) {
    Address=(unsigned)WriteData[1]|((unsigned)WriteData[2]<<8)|
            ((unsigned)WriteData[3]<<16)|((unsigned)WriteData[4]<<24);
    M->SelectedAddress=Address;
    PayloadLength=WriteLength-5;
  }
  if (M->SenseOffMode && WriteLength>=5 && WriteData[0]==0) {
    if (WriteLength>=9) {
      if (Address==0x90000018U && WriteData[5]==0x55) M->SenseOffResetWrites++;
      if (Address==0x9000005cU && WriteData[5]==0x5a) M->SenseOffStopWrites++;
      if (Address==0x8007008cU) { M->SenseOffControlWrites++; M->SenseOffBroadcastPhase=WriteData[5]?1:2; }
      if (Address==0x80070084U) M->SenseOff84Writes++;
      if (Address==0x80070004U) M->SenseOffConfigWrites++;
      if (Address==0xc00f3100U) M->SenseOff3100Writes++;
      if (Address==0xc00f3200U) M->SenseOff3200Writes++;
    }
  }
  if (M->SenseOffMode && WriteLength>=1 && WriteData[0]==0x31)
    M->SenseOffCommandWrites++;
  if (M->FlashMode && M->FlashFailAddressAlways &&
      Address==M->FlashFailAddress && PayloadLength &&
      (!M->FlashFailPayloadLength || PayloadLength==M->FlashFailPayloadLength))
    return HmIoError;
  if (M->FailAlways || Index<M->FailFirst || (M->FailAt && Index+1==M->FailAt) ||
      (M->FailFrom && Index+1>=M->FailFrom)) return HmIoError;

  if (M->FlashMode && WriteData[0]==0 && PayloadLength) {
    const unsigned char *Payload=WriteData+5;
    unsigned Value=(PayloadLength>=4)?
      ((unsigned)Payload[0]|((unsigned)Payload[1]<<8)|
       ((unsigned)Payload[2]<<16)|((unsigned)Payload[3]<<24)):0;
    if (Address==0x90000018U && PayloadLength>=4 && Payload[0]==0x55) M->FlashResetWrites++;
    if (M->FlashMode==2) {
      if (Address==0x80000020U && PayloadLength==4) {
        M->FlashControl=Value;
        if (Value==0x610ff000U) M->FlashProgramControls++;
      } else if (Address==0x80000024U && PayloadLength==4 &&
                 M->FlashControl==0x42000003U) {
        CHECK(Value==5U); M->FlashWipCommands++;
      } else if (Address==0x80000028U && PayloadLength==4) {
        CHECK(M->FlashControl==0x610ff000U);
        if (Value!=M->FlashPageOffsetWrites*HM_T70_FW_PAGE_BYTES)
          M->FlashPageOffsetErrors++;
        M->FlashCurrentOffset=Value; M->FlashPageOffsetWrites++;
      } else if (Address==0x8000002cU && PayloadLength==16) {
        M->FlashPage16Writes++;
        if (!M->FlashImage || M->FlashCurrentOffset+16U>HM_T70_FW_BYTES ||
            memcmp(Payload,M->FlashImage+M->FlashCurrentOffset,16U)) M->FlashPageMismatch++;
      } else if (Address==0x8000002cU && PayloadLength==240) {
        M->FlashPage240Writes++;
        if (!M->FlashImage || M->FlashCurrentOffset+256U>HM_T70_FW_BYTES ||
            memcmp(Payload,M->FlashImage+M->FlashCurrentOffset+16U,240U)) M->FlashPageMismatch++;
      }
    } else if (M->FlashMode==3) {
      if (Address==0x80050020U && PayloadLength==4) {
        M->FlashCrcStart=Value; M->FlashCrcStartWrites++;
      } else if (Address==0x80050028U && PayloadLength==4) {
        M->FlashCrcWords=(unsigned)Payload[0]|((unsigned)Payload[1]<<8);
        if (Payload[2]!=0x99 || Payload[3]!=0) M->FlashCrcWordTailErrors++;
        M->FlashCrcLengthWrites++;
      }
    }
  }

  if (ReadLength) {
    if (M->FlashMode && WriteData[0]==0x08) {
      if (M->FailReadAddress && M->SelectedAddress==M->FailReadAddress)
        return HmIoError;
      memset(ReadData,0,ReadLength);
      if (M->SelectedAddress==0x900000d0U) {
        memcpy(ReadData,M->ReadBytes,ReadLength);
      } else if (M->FlashMode==2 && M->SelectedAddress==0x8000002cU) {
        if (M->FlashControl==0x42000003U) {
          M->FlashWipReads++;
          ReadData[0]=2U;
          if (M->FlashWipBusyReads) { M->FlashWipBusyReads--; ReadData[0]|=1U; }
        } else {
          CHECK(M->FlashControl==0x42000000U);
          ReadData[0]=M->FlashProgramReady?2:0;
        }
      } else if (M->FlashMode==3 && M->SelectedAddress==0x80050000U) {
        M->FlashCrcStatusReads++;
        if (M->FlashCrcBusyReads) { M->FlashCrcBusyReads--; ReadData[0]=1; }
      } else if (M->FlashMode==3 && M->SelectedAddress==0x80050018U) {
        M->FlashCrcOutputReads++;
        ReadData[0]=(unsigned char)M->FlashCrcResult;
        if (ReadLength>1) ReadData[1]=(unsigned char)(M->FlashCrcResult>>8);
        if (ReadLength>2) ReadData[2]=(unsigned char)(M->FlashCrcResult>>16);
        if (ReadLength>3) ReadData[3]=(unsigned char)(M->FlashCrcResult>>24);
      } else {
        memcpy(ReadData,M->ReadBytes,ReadLength);
      }
    } else if (M->SenseOffMode && WriteData[0]==0x08) {
      unsigned ReadIndex;
      if (M->FailReadAddress && M->SelectedAddress==M->FailReadAddress)
        return HmIoError;
      memset(ReadData,0,ReadLength);
      if (M->SelectedAddress==0x900000a8U) {
        ReadIndex=M->SenseOffA8Reads++;
        if (ReadIndex==0) ReadData[0]=0x05;
        else if (M->SenseOffBadStart) ReadData[0]=0x01;
        else if (ReadIndex==1) ReadData[0]=0x0c;
        else if (ReadIndex==2) ReadData[0]=0x05;
        else ReadData[0]=(unsigned char)(M->SenseOffFinalState?M->SenseOffFinalState:0x05);
      } else if (M->SelectedAddress==0x9000005cU) {
        ReadIndex=M->SenseOff5cReads++;
        if (M->SenseOffBadStart) ReadData[0]=0x01;
        else if (ReadIndex==0) ReadData[0]=0x05;
        else if (M->SenseOffStopTimeout) ReadData[0]=0x01;
        else if (ReadIndex==1) ReadData[0]=0x0c;
        else ReadData[0]=0x6a;
      } else if (M->SelectedAddress==0x80070034U) {
        M->SenseOffBroadcastReads++;
        ReadData[0]=(unsigned char)(M->SenseOffBroadcastPhase==M->SenseOffBroadcastBusy &&
          (!M->SenseOffBusyLimit || M->SenseOffBroadcastReads<=M->SenseOffBusyLimit));
      }
    } else {
      memcpy(ReadData,M->ReadBytes,ReadLength);
      if (WriteLength==1 && WriteData[0]==0x08 && M->SelectedAddress==0x900000a8U)
        ReadData[0]=(unsigned char)M->RetainedFirmwareState;
      if (WriteLength==1 && WriteData[0]==0x08 && M->SelectedAddress==0x100001d0U) {
        if (M->ApAckReads++<M->ApAckMisses) ReadData[0]^=1;
      }
      if (WriteLength==1 && WriteData[0]==0x13) ReadData[0]=M->Readbacks<M->BadReadbacks?0:0x31;
      if (WriteLength==1 && WriteData[0]==0x0d) { ReadData[0]=M->Readbacks<M->BadReadbacks?0:0x10; M->Readbacks++; }
    }
  }
  return HmOk;
}
static void MockDelay(void *Context,unsigned MinimumUs,unsigned MaximumUs) {
  HM_MOCK_IO *M=(HM_MOCK_IO *)Context; unsigned Index=M->DelayCalls++;
  if (Index<128) { M->DelayMinimum[Index]=MinimumUs; M->DelayMaximum[Index]=MaximumUs; }
}
static void ResetMock(HM_MOCK_IO *M,HM_IO *Io) {
  memset(M,0,sizeof(*M));
  Io->Context=M; Io->Transfer=MockTransfer; Io->Delay=MockDelay;
  M->ReadBytes[0]=0x55; M->ReadBytes[1]=0x2c; M->ReadBytes[2]=0x13; M->ReadBytes[3]=0x83;
  for (unsigned I=4;I<HM_BUS_READ_MAX_BYTES;I++) M->ReadBytes[I]=(unsigned char)I;
}
static int HasWrite(HM_MOCK_IO *M,unsigned Index,const unsigned char *Bytes,unsigned Length) {
  return Index<128&&Index<M->Calls&&M->WriteLength[Index]==Length&&!memcmp(M->Writes[Index],Bytes,Length);
}
static void PutLe32(unsigned char *P,unsigned V) {
  P[0]=(unsigned char)V; P[1]=(unsigned char)(V>>8);
  P[2]=(unsigned char)(V>>16); P[3]=(unsigned char)(V>>24);
}
static unsigned char SyntheticT70[HM_T70_FW_BYTES];
static void BuildSyntheticT70Firmware(void) {
  unsigned Crc;
  memset(SyntheticT70,0,sizeof(SyntheticT70));
  SyntheticT70[14]=0x56;
  PutLe32(SyntheticT70+0x70,HM_FW_TAG_A00);
  PutLe32(SyntheticT70+0x74,0x00024400U); Checksum(SyntheticT70+0x70,16);
  PutLe32(SyntheticT70+0x100,HM_FW_TAG_10000000);
  PutLe32(SyntheticT70+0x104,0x00025402U); Checksum(SyntheticT70+0x100,16);
  PutLe32(SyntheticT70+0x110,HM_FW_TAG_10000100);
  PutLe32(SyntheticT70+0x114,0x00025405U); Checksum(SyntheticT70+0x110,16);
  PutLe32(SyntheticT70+0x140,HM_FW_TAG_10000600);
  PutLe32(SyntheticT70+0x144,0x00025500U); Checksum(SyntheticT70+0x140,16);
  CHECK(HmCrc32c(SyntheticT70,HM_T70_FW_BYTES-4U,&Crc)==HmOk);
  PutLe32(SyntheticT70+HM_T70_FW_BYTES-4U,Crc);
  CHECK(HmCrc32c(SyntheticT70,HM_T70_FW_BYTES,&Crc)==HmOk&&Crc==0);
}
static HM_FIRMWARE_UPDATE_PROFILE FirmwareProfile(unsigned Facts) {
  HM_FIRMWARE_UPDATE_PROFILE P={HM_FW_UPDATE_PROFILE_VERSION,
    sizeof(HM_FIRMWARE_UPDATE_PROFILE),Facts,HM_FW_FACTS_KNOWN};
  return P;
}
static void BuildT70FirmwareSummary(HM_FIRMWARE_INFO *Info) {
  memset(Info,0,sizeof(*Info));
  Info->Length=HM_T70_FW_BYTES; Info->Crc32c=0; Info->DescriptorCount=4;
  Info->Descriptors[0]=(HM_FW_DESCRIPTOR){HM_FW_TAG_A00,0x00024400U,0x70U};
  Info->Descriptors[1]=(HM_FW_DESCRIPTOR){HM_FW_TAG_10000000,0x00025402U,0x100U};
  Info->Descriptors[2]=(HM_FW_DESCRIPTOR){HM_FW_TAG_10000100,0x00025405U,0x110U};
  Info->Descriptors[3]=(HM_FW_DESCRIPTOR){HM_FW_TAG_10000600,0x00025500U,0x140U};
}
static void TestApSuspendNotify(void) {
  HM_MOCK_IO M; HM_IO Io; unsigned Index;
  const unsigned char Suspend[4]={0x5a,0xa5,0x5a,0xa5};
  ResetMock(&M,&Io); CHECK(HmNotifyApSuspend(&Io,2)==HmInvalid && !M.Calls && !M.DelayCalls);
  CHECK(HmNotifyApSuspend(NULL,0)==HmInvalid);
  ResetMock(&M,&Io); memcpy(M.ReadBytes,Suspend,4);
  CHECK(HmNotifyApSuspend(&Io,1)==HmOk && M.Calls==8 && M.DelayCalls==1);
  CHECK(M.WriteLength[2]==9 && M.Writes[2][0]==0 &&
    M.Writes[2][1]==0xd0 && M.Writes[2][2]==1 && M.Writes[2][3]==0 && M.Writes[2][4]==0x10);
  CHECK(!memcmp(M.Writes[2]+5,Suspend,4));
  CHECK(M.DelayMinimum[0]==1000 && M.DelayMaximum[0]==1001);
  ResetMock(&M,&Io); memset(M.ReadBytes,0,4);
  CHECK(HmNotifyApSuspend(&Io,0)==HmOk && M.Calls==8);
  CHECK(!memcmp(M.Writes[2]+5,M.ReadBytes,4));
  for(Index=1;Index<=10;Index++) {
    ResetMock(&M,&Io); memcpy(M.ReadBytes,Suspend,4); M.ApAckMisses=Index;
    CHECK(HmNotifyApSuspend(&Io,1)==HmOk && M.ApAckReads==Index+1 &&
          M.Calls==8*(Index+1) && M.DelayCalls==Index+1);
  }
  for(Index=0;Index<4;Index++) {
    ResetMock(&M,&Io); memcpy(M.ReadBytes,Suspend,4); M.ReadBytes[Index]^=1;
    CHECK(HmNotifyApSuspend(&Io,1)==HmTimeout && M.Calls==88 && M.DelayCalls==11);
  }
  for(Index=1;Index<=8;Index++) {
    ResetMock(&M,&Io); memcpy(M.ReadBytes,Suspend,4); M.FailFrom=Index;
    CHECK(HmNotifyApSuspend(&Io,1)==HmIoError);
  }
}
static void TestSenseOff(void) {
  HM_MOCK_IO M; HM_IO Io; HM_FIRMWARE_INFO Firmware;
  HM_SENSE_OFF_PROFILE Profile={HM_SENSE_OFF_PROFILE_VERSION,
    sizeof(HM_SENSE_OFF_PROFILE),0,0};
  BuildT70FirmwareSummary(&Firmware);
  ResetMock(&M,&Io); M.SenseOffMode=1;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmUnsupported&&!M.Calls);
  Profile.KnownFlagsMask=HM_SENSE_OFF_RUNTIME_BIT4;
  Profile.FeatureFlags=HM_SENSE_OFF_RUNTIME_BIT4;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmUnsupported&&!M.Calls);
  Profile.FeatureFlags=0;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmOk);
  CHECK(M.SenseOffCommandWrites==1&&M.SenseOffResetWrites==0&&M.SenseOffStopWrites==51);
  CHECK(M.SenseOffA8Reads==4&&M.SenseOff5cReads==52);
  CHECK(M.SenseOffBroadcastReads==2&&M.SenseOffControlWrites==2&&M.SenseOffConfigWrites==2);
  CHECK(M.SenseOff84Writes==1&&M.SenseOff3100Writes==1&&M.SenseOff3200Writes==1);
  CHECK(M.DelayCalls==54);
  { unsigned NormalCalls=M.Calls;
    for (unsigned Fail=1;Fail<=NormalCalls;Fail++) {
      ResetMock(&M,&Io);M.SenseOffMode=1;M.FailFrom=Fail;
      CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmIoError);
      CHECK(M.Calls<=Fail+5);
    }
  }
  /* The stock helper's 0x6a readback is not the completion condition. */
  ResetMock(&M,&Io); M.SenseOffMode=1; M.SenseOffStopTimeout=1;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmOk&&M.SenseOffStopWrites==51);
  CHECK(M.SenseOffBroadcastReads==2&&M.SenseOffConfigWrites==2);
  ResetMock(&M,&Io); M.SenseOffMode=1; M.SenseOffFinalState=0x0c;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmOk&&M.SenseOffA8Reads==4);
  for (unsigned Phase=1;Phase<=2;Phase++) {
    ResetMock(&M,&Io); M.SenseOffMode=1; M.SenseOffBroadcastBusy=Phase;
    CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmTimeout);
    CHECK(M.SenseOffControlWrites==2&&M.SenseOffBroadcastReads==100+Phase);
    CHECK(M.SenseOff84Writes==0&&M.SenseOffConfigWrites==0);
  }
  ResetMock(&M,&Io); M.SenseOffMode=1; M.SenseOffBroadcastBusy=1; M.SenseOffBusyLimit=100;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmOk && M.SenseOffBroadcastReads==102);
  ResetMock(&M,&Io); M.SenseOffMode=1; M.FailReadAddress=0x80070034U;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmIoError);
  CHECK(M.SenseOffControlWrites==2&&M.SenseOff84Writes==0);
  ResetMock(&M,&Io); M.SenseOffMode=1; M.FailReadAddress=0x900000a8U;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmIoError);
  CHECK(M.SenseOffCommandWrites==0&&M.SenseOffResetWrites==0);
  ResetMock(&M,&Io); M.SenseOffMode=1; M.SenseOffBadStart=1;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmTimeout);
  CHECK(M.SenseOffCommandWrites==6&&M.SenseOffResetWrites==6);
  CHECK(M.SenseOffControlWrites==0);
  Profile.Version++;
  ResetMock(&M,&Io); M.SenseOffMode=1;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmInvalid&&!M.Calls);
  Profile.Version--;
  Profile.Size--;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmInvalid&&!M.Calls);
  Profile.Size++;
  Firmware.Descriptors[0].Value++;
  CHECK(HmSenseOff(&Io,&Firmware,&Profile)==HmUnsupported&&!M.Calls);
}

static void TestFirmware(void) {
  unsigned char Image[0x120]={0};
  HM_FIRMWARE_INFO Info,Before;
  unsigned CrcValue=0xa5a5a5a5U;
  Image[14]=0x56;
  PutLe32(Image+0x70,HM_FW_TAG_A00); PutLe32(Image+0x74,0x70);
  Checksum(Image+0x70,16);
  PutLe32(Image+0x100,HM_FW_TAG_10000000); PutLe32(Image+0x104,0x100);
  Checksum(Image+0x100,16);
  PutLe32(Image+0x110,HM_FW_TAG_10000100);
  /* Vendor parser skips nonzero descriptor records with a bad sum8. */
  memset(&Info,0xa5,sizeof(Info));
  CHECK(HmInspectFirmware(Image,sizeof(Image),&Info)==HmOk);
  CHECK(Info.Length==sizeof(Image)&&Info.DescriptorCount==2);
  CHECK(Info.Descriptors[0].Tag==HM_FW_TAG_A00&&Info.Descriptors[0].Value==0x70&&Info.Descriptors[0].Offset==0x70);
  CHECK(Info.Descriptors[1].Tag==HM_FW_TAG_10000000&&Info.Descriptors[1].Value==0x100&&Info.Descriptors[1].Offset==0x100);
  CHECK(HmCrc32c((const unsigned char *)"\x01\x02\x03\x04",4,&CrcValue)==HmOk&&CrcValue==0xd6cf730bU);
  CrcValue=0xa5a5a5a5U;
  CHECK(HmCrc32c(Image,3,&CrcValue)==HmInvalid&&CrcValue==0xa5a5a5a5U);
  Before=Info; Image[14]=0;
  CHECK(HmInspectFirmware(Image,sizeof(Image),&Info)==HmUnsupported);
  CHECK(!memcmp(&Info,&Before,sizeof(Info))); Image[14]=0x56;
  PutLe32(Image+0x110,HM_FW_TAG_A00); PutLe32(Image+0x114,0x71);
  Checksum(Image+0x110,16);
  CHECK(HmInspectFirmware(Image,sizeof(Image),&Info)==HmInvalid);
  CHECK(!memcmp(&Info,&Before,sizeof(Info)));
  CHECK(HmInspectFirmware(Image,sizeof(Image)-1,&Info)==HmInvalid);
  PutLe32(Image+0x110,HM_FW_TAG_10000100); PutLe32(Image+0x114,0x1234);
  Checksum(Image+0x110,16);
  CHECK(HmValidateT70Firmware(Image,sizeof(Image),&Info)==HmUnsupported);
  CHECK(!memcmp(&Info,&Before,sizeof(Info)));
  {
    HM_FIRMWARE_INFO T70Info,Unchanged;
    BuildSyntheticT70Firmware();
    memset(&T70Info,0xa5,sizeof(T70Info));
    CHECK(HmValidateT70Firmware(SyntheticT70,sizeof(SyntheticT70),&T70Info)==HmOk);
    CHECK(T70Info.Crc32c==0&&T70Info.DescriptorCount==4);
    Unchanged=T70Info; SyntheticT70[0x200]^=1;
    CHECK(HmValidateT70Firmware(SyntheticT70,sizeof(SyntheticT70),&T70Info)==HmChecksum);
    CHECK(!memcmp(&T70Info,&Unchanged,sizeof(T70Info)));
    SyntheticT70[0x200]^=1;
  }
}
static void TestT70FlashStages(void) {
  HM_MOCK_IO M; HM_IO Io; HM_FIRMWARE_UPDATE_PROFILE Profile;
  HM_FIRMWARE_PROGRESS Progress,Before;
  unsigned I;
  BuildSyntheticT70Firmware();

  Profile=FirmwareProfile(HM_FW_FACTS_PROGRAM);
  ResetMock(&M,&Io); M.FlashMode=2; M.FlashProgramReady=1; M.FlashImage=SyntheticT70;
  CHECK(HmProgramT70Firmware(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmOk);
  CHECK(Progress.Stage==HmFwStageProgrammed&&Progress.LastStatus==HmOk);
  CHECK(Progress.TotalBytes==HM_T70_FW_BYTES&&Progress.BytesCompleted==HM_T70_FW_BYTES);
  CHECK(Progress.PagesCompleted==HM_T70_FW_BYTES/HM_T70_FW_PAGE_BYTES);
  CHECK(M.FlashPage16Writes==Progress.PagesCompleted&&M.FlashPage240Writes==Progress.PagesCompleted);
  CHECK(M.FlashProgramControls==Progress.PagesCompleted&&M.FlashWipCommands==M.FlashWipReads);
  CHECK(M.FlashPageOffsetWrites==Progress.PagesCompleted&&!M.FlashPageOffsetErrors);
  CHECK(!M.FlashPageMismatch&&!M.FlashWipBusyReads&&M.FlashWipReads==Progress.PagesCompleted);
  CHECK(M.FlashResetWrites==0&&!M.DelayCalls);

  Profile=FirmwareProfile(HM_FW_FACTS_PROGRAM&~HM_FW_FACT_TARGET_ERASED);
  ResetMock(&M,&Io); M.FlashMode=2;
  memset(&Progress,0xa5,sizeof(Progress)); Before=Progress;
  CHECK(HmProgramT70Firmware(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmUnsupported);
  CHECK(!M.Calls&&!memcmp(&Progress,&Before,sizeof(Progress)));

  Profile=FirmwareProfile(HM_FW_FACTS_PROGRAM);
  ResetMock(&M,&Io); M.FlashMode=2; M.FlashProgramReady=0; M.FlashImage=SyntheticT70;
  CHECK(HmProgramT70Firmware(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmStateError);
  CHECK(!M.FlashPage16Writes&&!M.FlashPage240Writes&&Progress.PagesCompleted==0);
  CHECK(Progress.CurrentOffset==0&&Progress.FlashContentsMayBeChanged==1);
  CHECK(M.FlashResetWrites==0);

  ResetMock(&M,&Io); M.FlashMode=2; M.FlashProgramReady=1; M.FlashImage=SyntheticT70;
  M.FlashFailAddress=0x8000002cU; M.FlashFailAddressAlways=1;
  CHECK(HmProgramT70Firmware(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmIoError);
  CHECK(Progress.PagesCompleted==0&&Progress.BytesCompleted==0);
  CHECK(!M.FlashPage16Writes&&!M.FlashPage240Writes&&M.DelayCalls==3);
  CHECK(M.FlashResetWrites==0);

  ResetMock(&M,&Io); M.FlashMode=2; M.FlashProgramReady=1; M.FlashImage=SyntheticT70;
  M.FlashFailAddress=0x8000002cU; M.FlashFailAddressAlways=1; M.FlashFailPayloadLength=240;
  CHECK(HmProgramT70Firmware(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmIoError);
  CHECK(Progress.PagesCompleted==0&&Progress.BytesCompleted==0);
  CHECK(M.FlashPage16Writes==1&&!M.FlashPage240Writes&&M.FlashResetWrites==0);

  ResetMock(&M,&Io); M.FlashMode=2; M.FlashProgramReady=1; M.FlashImage=SyntheticT70;
  M.FlashWipBusyReads=101;
  CHECK(HmProgramT70Firmware(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmTimeout);
  CHECK(Progress.PagesCompleted==0&&Progress.BytesCompleted==0);
  CHECK(M.FlashPage16Writes==1&&M.FlashPage240Writes==1&&M.FlashWipReads==101);
  CHECK(M.DelayCalls==100&&M.FlashResetWrites==0);

  Profile=FirmwareProfile(HM_FW_FACTS_VERIFY);
  ResetMock(&M,&Io); M.FlashMode=3; M.FlashImage=SyntheticT70;
  CHECK(HmVerifyT70FirmwareFlash(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmOk);
  CHECK(Progress.Stage==HmFwStageVerified&&Progress.LastStatus==HmOk);
  CHECK(Progress.ObservedCrc32c==0&&Progress.FlashContentsMayBeChanged==0);
  CHECK(M.FlashCrcStartWrites==1&&M.FlashCrcStart==0);
  CHECK(M.FlashCrcLengthWrites==1&&M.FlashCrcWords==0xff00U&&!M.FlashCrcWordTailErrors);
  CHECK(M.FlashCrcStatusReads==1&&M.FlashCrcOutputReads==1&&!M.DelayCalls);
  CHECK(M.FlashResetWrites==0);

  ResetMock(&M,&Io); M.FlashMode=3; M.FlashImage=SyntheticT70; M.FlashCrcResult=0x12345678U;
  CHECK(HmVerifyT70FirmwareFlash(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmChecksum);
  CHECK(Progress.Stage==HmFwStageCrcRead&&Progress.ObservedCrc32c==0x12345678U);
  CHECK(M.FlashCrcOutputReads==1&&M.FlashResetWrites==0);

  ResetMock(&M,&Io); M.FlashMode=3; M.FlashImage=SyntheticT70; M.FlashCrcBusyReads=101;
  CHECK(HmVerifyT70FirmwareFlash(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmTimeout);
  CHECK(Progress.Stage==HmFwStageCrcPoll&&M.FlashCrcStatusReads==101);
  CHECK(!M.FlashCrcOutputReads&&M.DelayCalls==101&&M.FlashResetWrites==0);

  ResetMock(&M,&Io); M.FlashMode=3; M.FlashImage=SyntheticT70;
  M.FailReadAddress=0x80050000U;
  CHECK(HmVerifyT70FirmwareFlash(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmIoError);
  CHECK(Progress.Stage==HmFwStageCrcPoll&&!M.FlashCrcOutputReads&&M.FlashResetWrites==0);

  ResetMock(&M,&Io); M.FlashMode=3; M.FlashImage=SyntheticT70;
  M.FailReadAddress=0x80050018U;
  CHECK(HmVerifyT70FirmwareFlash(&Io,SyntheticT70,sizeof(SyntheticT70),
    &Profile,&Progress)==HmIoError);
  CHECK(Progress.Stage==HmFwStageCrcRead&&M.FlashCrcStatusReads==1);
  CHECK(!M.FlashCrcOutputReads&&M.FlashResetWrites==0);
}

static void TestTransport(void) {
  HM_MOCK_IO M; HM_IO Io; unsigned char Out[HM_BUS_READ_MAX_BYTES],Expected[9];
  ResetMock(&M,&Io); memset(Out,0xa5,4);
  CHECK(HmReadChipId(&Io)==HmOk&&M.Calls==5&&!M.DelayCalls);
  { const unsigned char A[]={0x13,0x31},B[]={0x0d,0x10},C[]={0x00,0xd0,0x00,0x00,0x90},D[]={0x0c,0x00};
    CHECK(HasWrite(&M,0,A,sizeof(A))); CHECK(HasWrite(&M,1,B,sizeof(B)));
    CHECK(HasWrite(&M,2,C,sizeof(C))); CHECK(HasWrite(&M,3,D,sizeof(D)));
    CHECK(M.WriteLength[4]==1&&M.Writes[4][0]==0x08&&M.ReadSize[4]==4);
  }
  /* SPI200 data FIFO keeps a fixed indirect address for multi-byte I/O;
   * the similarly spelled 0x0080002c is an ordinary incrementing address. */
  for (unsigned Direction=0;Direction<2;Direction++) {
    for (unsigned Fifo=0;Fifo<2;Fifo++) {
      unsigned Address=Fifo?0x8000002cU:0x0080002cU;
      ResetMock(&M,&Io);
      if (Direction) CHECK(HmRegisterWrite(&Io,Address,Out,16)==HmOk);
      else CHECK(HmRegisterRead(&Io,Address,Out,16)==HmOk);
      CHECK(M.WriteLength[1]==2&&M.Writes[1][0]==0x0d);
      CHECK(M.Writes[1][1]==(Fifo?0x10:0x11));
    }
  }
  ResetMock(&M,&Io); M.ReadBytes[1]=0x2d;
  CHECK(HmReadChipId(&Io)==HmInvalid&&M.Calls==25);
  ResetMock(&M,&Io); CHECK(HmSenseOn(&Io)==HmOk&&M.Calls==9);
  Expected[0]=0x00;Expected[1]=0x5c;Expected[2]=0;Expected[3]=0;Expected[4]=0x90;
  Expected[5]=Expected[6]=Expected[7]=Expected[8]=0;
  CHECK(HasWrite(&M,7,Expected,sizeof(Expected)));
  CHECK(M.WriteLength[8]==2&&M.Writes[8][0]==0x31&&M.Writes[8][1]==0);
  CHECK(M.DelayCalls==1&&M.DelayMinimum[0]==10000&&M.DelayMaximum[0]==10001);
  ResetMock(&M,&Io); CHECK(HmEnter(&Io,HmEntryInitialHandoff)==HmOk&&M.Calls==14);
  CHECK(M.DelayCalls==1&&M.Writes[13][0]==0x31&&M.Writes[13][1]==0);
  ResetMock(&M,&Io); M.RetainedFirmwareState=5;
  CHECK(HmEnter(&Io,HmEntryRetainedResume)==HmOk&&M.Calls==10&&!M.DelayCalls);
  for (unsigned I=0;I<M.Calls;I++) CHECK(M.Writes[I][0]!=0x31&&M.WriteLength[I]<=5);
  ResetMock(&M,&Io); M.RetainedFirmwareState=0x0c;
  CHECK(HmEnter(&Io,HmEntryRetainedResume)==HmStateError&&M.Calls==10&&!M.DelayCalls);
  ResetMock(&M,&Io); M.FailFrom=10;
  CHECK(HmEnter(&Io,HmEntryRetainedResume)==HmIoError&&M.Calls==12&&M.DelayCalls==3);
  ResetMock(&M,&Io); M.ReadBytes[1]=0x2d;
  CHECK(HmEnter(&Io,HmEntryInitialHandoff)==HmInvalid&&M.Calls==25&&!M.DelayCalls);
  for (unsigned I=0;I<M.Calls;I++) CHECK(M.Writes[I][0]!=0x31);
  ResetMock(&M,&Io); CHECK(HmEnter(&Io,(HM_ENTRY_REASON)2)==HmInvalid&&!M.Calls);
  ResetMock(&M,&Io); CHECK(HmInterfaceOn(&Io)==HmOk&&M.Calls==5&&M.Readbacks==1&&!M.DelayCalls);
  ResetMock(&M,&Io); M.BadReadbacks=9;
  CHECK(HmInterfaceOn(&Io)==HmOk&&M.Calls==41&&M.Readbacks==10&&M.DelayCalls==9);
  for (unsigned I=0;I<9;I++) CHECK(M.DelayMinimum[I]==1000&&M.DelayMaximum[I]==1100);
  ResetMock(&M,&Io); M.BadReadbacks=10;
  CHECK(HmInterfaceOn(&Io)==HmTimeout&&M.Calls==41&&M.Readbacks==10&&M.DelayCalls==10);
  ResetMock(&M,&Io); M.BadReadbacks=10;
  CHECK(HmSenseOn(&Io)==HmOk&&M.Calls==45&&M.Readbacks==10&&M.DelayCalls==11);
  CHECK(HasWrite(&M,43,Expected,sizeof(Expected)));
  CHECK(M.Writes[44][0]==0x31&&M.Writes[44][1]==0);
  CHECK(M.DelayMinimum[10]==10000&&M.DelayMaximum[10]==10001);
  for (unsigned Fail=1;Fail<=45;Fail++) {
    ResetMock(&M,&Io);M.BadReadbacks=10;M.FailFrom=Fail;
    CHECK(HmSenseOn(&Io)==HmIoError);
  }
  ResetMock(&M,&Io); M.FailAlways=1;
  CHECK(HmSenseOn(&Io)==HmIoError&&M.Calls==3&&M.DelayCalls==3);
  ResetMock(&M,&Io); CHECK(HmSystemReset(&Io)==HmOk&&M.Calls==3&&!M.DelayCalls);
  { const unsigned char Reset[]={0,0x18,0,0,0x90,0x55,0,0,0};
    CHECK(HasWrite(&M,2,Reset,sizeof(Reset)));
  }
  CHECK(HmSystemReset(NULL)==HmInvalid&&HmInterfaceOn(NULL)==HmInvalid);
  ResetMock(&M,&Io); M.FailFirst=2;
  CHECK(HmBusRead(&Io,HM_EVENT_COMMAND,Out,56)==HmOk&&M.Calls==3&&M.DelayCalls==2);
  CHECK(M.DelayMinimum[0]==20000&&M.DelayMaximum[0]==20001&&M.DelayMinimum[1]==20000);
  CHECK(Out[0]==0x55&&Out[1]==0x2c&&Out[55]==55);
  ResetMock(&M,&Io); M.FailAlways=1; memset(Out,0xa5,56);
  CHECK(HmBusRead(&Io,HM_EVENT_COMMAND,Out,56)==HmIoError&&M.Calls==3&&M.DelayCalls==3);
  for (unsigned I=0;I<56;I++) CHECK(Out[I]==0xa5);
  CHECK(HmRegisterRead(&Io,HM_CHIP_ID_REGISTER,Out,257)==HmInvalid);
  CHECK(HmBusRead(NULL,0x30,Out,56)==HmInvalid);
  ResetMock(&M,&Io); CHECK(HmBusWrite(&Io,0x00,0xffffdaf1U,NULL,1)==HmInvalid);
}

int main(void) {
  TestApSuspendNotify();
  unsigned char D[56]; HM_FRAME F,Before; HM_POINTER P; unsigned I;
  TestTransport();
  TestFirmware();
  TestSenseOff();
  TestT70FlashStages();
  memset(D,255,40); memset(D+40,0,16);
  D[0]=1; D[1]=2; D[2]=3; D[3]=4; D[40]=77; D[52]=1; Checksum(D,56);
  CHECK(HmDecode(D,56,10,1400,2240,&F)==HmOk&&F.Count==1&&F.DeclaredCount==1);
  CHECK(F.Contacts[0].Active&&F.Contacts[0].X==258&&F.Contacts[0].Y==772&&F.Contacts[0].Pressure==77);
  HmPointerInit(&P); CHECK(P.Primary==255&&!P.Active);
  CHECK(HmPointerUpdate(&P,&F)&&P.Active&&P.X==258&&P.Y==772&&P.Primary==0);
  CHECK(!HmPointerUpdate(&P,&F));
  D[4]=0;D[5]=12;D[6]=0;D[7]=34;D[41]=55;D[52]=2;Checksum(D,56);
  CHECK(HmDecode(D,56,10,1400,2240,&F)==HmOk&&F.Count==2);
  CHECK(!HmPointerUpdate(&P,&F)&&P.Primary==0);
  memset(D,255,4);D[52]=1;Checksum(D,56);
  CHECK(HmDecode(D,56,10,1400,2240,&F)==HmOk);
  CHECK(HmPointerUpdate(&P,&F)&&!P.Active&&P.Primary==255&&P.X==258);
  CHECK(HmPointerUpdate(&P,&F)&&P.Active&&P.Primary==1&&P.X==12);
  Before=F;D[1]^=1;CHECK(HmDecode(D,56,10,1400,2240,&F)==HmChecksum&&!memcmp(&Before,&F,sizeof(F)));D[1]^=1;
  CHECK(HmDecode(NULL,56,10,1400,2240,&F)==HmInvalid);
  CHECK(HmDecode(D,56,0,1400,2240,&F)==HmInvalid);
  CHECK(HmDecode(D,56,11,1400,2240,&F)==HmInvalid);
  CHECK(HmDecode(D,56,10,65535,2240,&F)==HmInvalid);
  CHECK(HmDecode(D,55,10,1400,2240,&F)==HmUnsupported);
  CHECK(HmDecode(D,56,10,1400,2240,NULL)==HmInvalid);
  for(I=0;I<56;I++) { unsigned char Old=D[I];D[I]^=128;CHECK(HmDecode(D,56,10,1400,2240,&F)==HmChecksum);D[I]=Old; }
  D[52]=0x21;Checksum(D,56);CHECK(HmDecode(D,56,10,1400,2240,&F)==HmUnsupported);
  D[52]=11;Checksum(D,56);CHECK(HmDecode(D,56,10,1400,2240,&F)==HmInvalid);
  D[52]=2;Checksum(D,56);CHECK(HmDecode(D,56,10,1400,2240,&F)==HmInvalid);
  D[52]=255;Checksum(D,56);CHECK(HmDecode(D,56,10,1400,2240,&F)==HmOk&&!F.Count);
  CHECK(HmPointerUpdate(&P,&F)&&!P.Active);CHECK(!HmPointerRelease(&P));
  memset(D,0,56);CHECK(HmDecode(D,56,10,1400,2240,&F)==HmEmpty);
  memset(D,255,40);memset(D+40,0,16);
  D[0]=1400>>8;D[1]=1400&255;D[2]=2240>>8;D[3]=2240&255;D[52]=1;Checksum(D,56);
  CHECK(HmDecode(D,56,10,1400,2240,&F)==HmOk&&F.Contacts[0].Active);
  D[1]++;Checksum(D,56);CHECK(HmDecode(D,56,10,1400,2240,&F)==HmInvalid);
  memset(D,0,56);memset(D,255,4);D[4]=0;D[5]=0;D[6]=0;D[7]=0;D[8]=1;Checksum(D,12);
  CHECK(HmDecode(D,12,1,1400,2240,&F)==HmInvalid); // malformed one-slot count is at byte8 (pressure), not a 10-slot frame
  printf("Himax core: %u checks passed (stock report fields, guarded indirect I2C commands, ID/sense-on sequence, retry and failure behavior)\n",Checks);
}
