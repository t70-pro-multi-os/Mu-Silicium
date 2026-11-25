// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/HimaxCore.h>
#include <stddef.h>
HM_STATUS HmDecode(const unsigned char *D,unsigned Length,unsigned Slots,
                   unsigned XMax,unsigned YMax,HM_FRAME *Frame) {
  HM_FRAME F={0}; unsigned I,Sum=0,Nonzero=0,Info,CountIndex,Declared,Mode;
  if (!D || !Frame || !Slots || Slots>HM_MAX_CONTACTS || !XMax || XMax>65534 ||
      !YMax || YMax>65534) return HmInvalid;
  /* Stock allocates coordinates, a four-byte-aligned pressure array, then
   * count/state/checksum. Stylus adds 12 bytes and has a separate format. */
  Info=Slots*4+((Slots+3)&~3U)+4;
  if (Length!=Info) return HmUnsupported;
  for (I=0;I<Length;I++) { Sum+=D[I]; Nonzero|=D[I]; }
  if (!Nonzero) return HmEmpty;
  if (Sum&255) return HmChecksum;
  CountIndex=Info-4; Mode=D[CountIndex]>>4;
  if (Mode!=0 && Mode!=15) return HmUnsupported;
  Declared=D[CountIndex]==255?0:(D[CountIndex]&15);
  if (Declared>Slots) return HmInvalid;
  F.DeclaredCount=(unsigned char)Declared;
  F.State[0]=D[Info-3]; F.State[1]=D[Info-2];
  for (I=0;I<Slots;I++) {
    HM_CONTACT *C=&F.Contacts[I];
    C->X=(unsigned short)((D[I*4]<<8)|D[I*4+1]);
    C->Y=(unsigned short)((D[I*4+2]<<8)|D[I*4+3]);
    C->Pressure=D[Slots*4+I];
    C->Active=(unsigned char)(Declared && C->X<=XMax && C->Y<=YMax);
    F.Count+=C->Active;
  }
  /* Reject contradictory frames instead of manufacturing extra contacts.
   * Stock counts in-range slots too but does not reject this inconsistency. */
  if (F.Count!=Declared) return HmInvalid;
  *Frame=F; return HmOk;
}
static unsigned HmLoadLe32(const unsigned char *P) {
  return (unsigned)P[0] | ((unsigned)P[1]<<8) |
         ((unsigned)P[2]<<16) | ((unsigned)P[3]<<24);
}

static int HmFirmwareTagKnown(unsigned Tag) {
  return Tag==HM_FW_TAG_A00 || Tag==HM_FW_TAG_A01 ||
         Tag==HM_FW_TAG_10000000 || Tag==HM_FW_TAG_10000100 ||
         Tag==HM_FW_TAG_10000600;
}

HM_STATUS HmCrc32c(const unsigned char *Data,unsigned Length,unsigned *Crc32c) {
  unsigned Offset,Crc=0xffffffffU,Word,Bit;
  if (!Data || !Crc32c || Length<4 || (Length&3U)) return HmInvalid;
  for (Offset=0;Offset<Length;Offset+=4) {
    Word=HmLoadLe32(Data+Offset);
    Crc^=Word;
    for (Bit=0;Bit<32;Bit++)
      Crc=(Crc>>1)^((Crc&1U)?0x82f63b78U:0U);
  }
  *Crc32c=Crc;
  return HmOk;
}

HM_STATUS HmInspectFirmware(const unsigned char *Image,unsigned Length,
                            HM_FIRMWARE_INFO *Info) {
  HM_FIRMWARE_INFO Parsed={0};
  unsigned I,J,Sum,Tag,Value,Crc;
  HM_STATUS Status;
  if (!Image || !Info || Length<0x80U || (Length&15U)) return HmInvalid;
  for (I=0;I<8;I++) if (Image[I]!=0) return HmInvalid;
  if (Image[14]!=0x56) return HmUnsupported;
  Status=HmCrc32c(Image,Length,&Crc);
  if (Status!=HmOk) return Status;
  Parsed.Length=Length;
  Parsed.Crc32c=Crc;
  for (I=0;I<Length;I+=16) {
    Sum=0;
    for (J=0;J<16;J++) Sum+=Image[I+J];
    Tag=HmLoadLe32(Image+I);
    if (!HmFirmwareTagKnown(Tag) || (Sum&255U)!=0) continue;
    if (Parsed.DescriptorCount==HM_FW_MAX_DESCRIPTORS) return HmInvalid;
    Value=HmLoadLe32(Image+I+4);
    for (J=0;J<Parsed.DescriptorCount;J++)
      if (Parsed.Descriptors[J].Tag==Tag) return HmInvalid;
    Parsed.Descriptors[Parsed.DescriptorCount].Tag=Tag;
    Parsed.Descriptors[Parsed.DescriptorCount].Value=Value;
    Parsed.Descriptors[Parsed.DescriptorCount].Offset=I;
    Parsed.DescriptorCount++;
  }
  if (!Parsed.DescriptorCount) return HmInvalid;
  *Info=Parsed;
  return HmOk;
}

HM_STATUS HmValidateT70Firmware(const unsigned char *Image,unsigned Length,
                                HM_FIRMWARE_INFO *Info) {
  static const HM_FW_DESCRIPTOR Expected[]={
    {HM_FW_TAG_A00,0x00024400U,0x70U},
    {HM_FW_TAG_10000000,0x00025402U,0x100U},
    {HM_FW_TAG_10000100,0x00025405U,0x110U},
    {HM_FW_TAG_10000600,0x00025500U,0x140U}
  };
  HM_FIRMWARE_INFO Parsed;
  unsigned I;
  HM_STATUS Status;
  if (!Info) return HmInvalid;
  Status=HmInspectFirmware(Image,Length,&Parsed);
  if (Status!=HmOk) return Status;
  if (Length!=HM_T70_FW_BYTES) return HmUnsupported;
  if (Parsed.Crc32c!=0) return HmChecksum;
  if (Parsed.DescriptorCount!=sizeof(Expected)/sizeof(Expected[0]))
    return HmUnsupported;
  for (I=0;I<Parsed.DescriptorCount;I++)
    if (Parsed.Descriptors[I].Tag!=Expected[I].Tag ||
        Parsed.Descriptors[I].Value!=Expected[I].Value ||
        Parsed.Descriptors[I].Offset!=Expected[I].Offset)
      return HmUnsupported;
  *Info=Parsed;
  return HmOk;
}

void HmPointerInit(HM_POINTER *P) {
  if (P) { P->X=P->Y=0; P->Pressure=P->Active=0; P->Primary=255; }
}
int HmPointerRelease(HM_POINTER *P) {
  int Changed;
  if (!P) return 0;
  Changed=P->Active!=0; P->Active=P->Pressure=0; P->Primary=255; return Changed;
}
int HmPointerUpdate(HM_POINTER *P,const HM_FRAME *F) {
  const HM_CONTACT *C; unsigned I; int Changed;
  if (!P || !F) return 0;
  if (P->Primary<HM_MAX_CONTACTS) {
    if (!F->Contacts[P->Primary].Active) return HmPointerRelease(P);
  } else {
    for (I=0;I<HM_MAX_CONTACTS && !F->Contacts[I].Active;I++);
    if (I==HM_MAX_CONTACTS) return HmPointerRelease(P);
    P->Primary=(unsigned char)I;
  }
  C=&F->Contacts[P->Primary];
  Changed=!P->Active || P->X!=C->X || P->Y!=C->Y || P->Pressure!=C->Pressure;
  P->X=C->X; P->Y=C->Y; P->Pressure=C->Pressure; P->Active=1; return Changed;
}


static HM_STATUS HmValidateIo(const HM_IO *Io) {
  return (Io && Io->Transfer && Io->Delay) ? HmOk : HmInvalid;
}

static HM_STATUS HmTransferRetry(const HM_IO *Io,
  const unsigned char *WriteData, unsigned WriteLength,
  unsigned char *ReadData, unsigned ReadLength) {
  unsigned Attempt;
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || !WriteData || !WriteLength) return HmInvalid;
  for (Attempt=0; Attempt<3; ++Attempt) {
    Status=Io->Transfer(Io->Context,WriteData,WriteLength,ReadData,ReadLength);
    if (Status==HmOk) return HmOk;
    /* The stock driver waits after each failed transfer, including the third. */
    Io->Delay(Io->Context,20000,20001);
  }
  return HmIoError;
}

HM_STATUS HmBusRead(const HM_IO *Io,unsigned Command,
                    unsigned char *Data,unsigned Length) {
  unsigned char Type,Temp[HM_BUS_READ_MAX_BYTES];
  unsigned I;
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || Command>255 || !Data || !Length ||
      Length>HM_BUS_READ_MAX_BYTES) return HmInvalid;
  Type=(unsigned char)Command;
  Status=HmTransferRetry(Io,&Type,1,Temp,Length);
  if (Status!=HmOk) return Status;
  for (I=0;I<Length;I++) Data[I]=Temp[I];
  return HmOk;
}

HM_STATUS HmBusWrite(const HM_IO *Io,unsigned Command,unsigned Address,
                     const unsigned char *Data,unsigned Length) {
  unsigned char Packet[HM_BUS_WRITE_MAX_BODY+1];
  unsigned PayloadLength,I;
  if (HmValidateIo(Io)!=HmOk || Command>255 || !Length ||
      Length>HM_BUS_WRITE_MAX_BODY) return HmInvalid;
  Packet[0]=(unsigned char)Command;
  if (Address==HM_BUS_WRITE_SENTINEL) {
    if (!Data) return HmInvalid;
    PayloadLength=Length;
    for (I=0;I<PayloadLength;I++) Packet[I+1]=Data[I];
  } else {
    if (Length<4) return HmInvalid;
    Packet[1]=(unsigned char)Address;
    Packet[2]=(unsigned char)(Address>>8);
    Packet[3]=(unsigned char)(Address>>16);
    Packet[4]=(unsigned char)(Address>>24);
    PayloadLength=Length-4;
    if (PayloadLength && !Data) return HmInvalid;
    for (I=0;I<PayloadLength;I++) Packet[I+5]=Data[I];
  }
  return HmTransferRetry(Io,Packet,Length+1,NULL,0);
}

static HM_STATUS HmBurstEnable(const HM_IO *Io,unsigned Enable) {
  const unsigned char BurstStart=0x31;
  unsigned char Control=(unsigned char)(0x10|(Enable!=0));
  HM_STATUS Status;
  Status=HmBusWrite(Io,0x13,HM_BUS_WRITE_SENTINEL,&BurstStart,1);
  if (Status!=HmOk) return Status;
  return HmBusWrite(Io,0x0d,HM_BUS_WRITE_SENTINEL,&Control,1);
}

HM_STATUS HmRegisterRead(const HM_IO *Io,unsigned Address,
                         unsigned char *Data,unsigned Length) {
  unsigned char Zero=0,Temp[HM_BUS_READ_MAX_BYTES];
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || !Data || !Length ||
      Length>HM_BUS_READ_MAX_BYTES || Address==HM_BUS_WRITE_SENTINEL)
    return HmInvalid;
  Status=HmBurstEnable(Io,(Address!=0x8000002cU && Length>4));
  if (Status!=HmOk) return Status;
  Status=HmBusWrite(Io,0,Address,NULL,4);
  if (Status!=HmOk) return Status;
  Status=HmBusWrite(Io,0x0c,HM_BUS_WRITE_SENTINEL,&Zero,1);
  if (Status!=HmOk) return Status;
  Status=HmBusRead(Io,0x08,Temp,Length);
  if (Status!=HmOk) return Status;
  for (unsigned I=0;I<Length;I++) Data[I]=Temp[I];
  return HmOk;
}

HM_STATUS HmRegisterWrite(const HM_IO *Io,unsigned Address,
                          const unsigned char *Data,unsigned Length) {
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || !Data || !Length || Length>HM_BUS_WRITE_MAX_BODY-4 ||
      Address==HM_BUS_WRITE_SENTINEL) return HmInvalid;
  Status=HmBurstEnable(Io,(Address!=0x8000002cU && Length>4));
  if (Status!=HmOk) return Status;
  return HmBusWrite(Io,0,Address,Data,Length+4);
}

/* T70 stock hx_ap_notify_fw_sus (0x11e48), g_hx83132_setup+0x18.
 * Deliberately stricter than its void/best-effort return: all bytes must match.
 * This command is not a power, firmware-running or ownership attestation. */
HM_STATUS HmNotifyApSuspend(const HM_IO *Io,unsigned Suspended) {
  unsigned char Value[4],Observed[4];
  unsigned Attempt,Index;
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || Suspended>1U) return HmInvalid;
  Value[0]=Value[2]=Suspended?0x5a:0;
  Value[1]=Value[3]=Suspended?0xa5:0;
  for (Attempt=0;Attempt<11;Attempt++) {
    Status=HmRegisterWrite(Io,0x100001d0U,Value,sizeof(Value));
    if (Status!=HmOk) return Status;
    Io->Delay(Io->Context,1000,1001);
    Status=HmRegisterRead(Io,0x100001d0U,Observed,sizeof(Observed));
    if (Status!=HmOk) return Status;
    for (Index=0;Index<4 && Observed[Index]==Value[Index];Index++) {}
    if (Index==4) return HmOk;
  }
  return HmTimeout;
}

int HmChipIdMatches(const unsigned char Id[4]) {
  return Id[3]==0x83 && Id[2]==0x13 && Id[1]>=0x2a && Id[1]<=0x2c;
}

HM_STATUS HmReadChipId(const HM_IO *Io) {
  unsigned char Id[4];
  unsigned Attempt;
  HM_STATUS Status=HmIoError;
  for (Attempt=0;Attempt<5;Attempt++) {
    Status=HmRegisterRead(Io,HM_CHIP_ID_REGISTER,Id,sizeof(Id));
    if (Status==HmOk && HmChipIdMatches(Id)) return HmOk;
  }
  return Status==HmOk ? HmInvalid : Status;
}

HM_STATUS HmInterfaceOn(const HM_IO *Io) {
  unsigned char Discard[4],Burst,Control;
  unsigned Attempt;
  HM_STATUS Status=HmBusRead(Io,0x08,Discard,sizeof(Discard));
  if (Status!=HmOk) return Status;
  for (Attempt=0;Attempt<10;Attempt++) {
    Status=HmBurstEnable(Io,0);
    if (Status!=HmOk) return Status;
    Status=HmBusRead(Io,0x13,&Burst,1);
    if (Status!=HmOk) return Status;
    Status=HmBusRead(Io,0x0d,&Control,1);
    if (Status!=HmOk) return Status;
    if (Burst==0x31 && Control==0x10) return HmOk;
    Io->Delay(Io->Context,1000,1100);
  }
  /* Preserve the warning for strict callers; stock sense-on continues below. */
  return HmTimeout;
}

HM_STATUS HmSystemReset(const HM_IO *Io) {
  const unsigned char Reset[4]={0x55,0,0,0};
  return HmRegisterWrite(Io,0x90000018U,Reset,sizeof(Reset));
}

#define HM_FW_WIP_POLLS 101U

static HM_STATUS HmReadRegister32(const HM_IO *Io,unsigned Address,
                                  unsigned char Data[4]);

static void HmClearFirmwareBytes(unsigned char *Data,unsigned Length) {
  volatile unsigned char *P=(volatile unsigned char *)Data;
  while (Length--) *P++=0;
}

static HM_STATUS HmValidateFirmwareUpdateProfile(
  const HM_FIRMWARE_UPDATE_PROFILE *Profile,unsigned RequiredFacts) {
  if (!Profile || Profile->Version!=HM_FW_UPDATE_PROFILE_VERSION ||
      Profile->Size!=sizeof(*Profile)) return HmInvalid;
  if ((Profile->KnownFactsMask&~HM_FW_FACTS_KNOWN) ||
      (Profile->Facts&~HM_FW_FACTS_KNOWN) ||
      (Profile->Facts&~Profile->KnownFactsMask)) return HmInvalid;
  if ((Profile->KnownFactsMask&RequiredFacts)!=RequiredFacts ||
      (Profile->Facts&RequiredFacts)!=RequiredFacts) return HmUnsupported;
  return HmOk;
}

static HM_STATUS HmWriteFirmwareWord(const HM_IO *Io,unsigned Address,
                                     unsigned Value) {
  unsigned char Data[4];
  HM_STATUS Status;
  Data[0]=(unsigned char)Value;
  Data[1]=(unsigned char)(Value>>8);
  Data[2]=(unsigned char)(Value>>16);
  Data[3]=(unsigned char)(Value>>24);
  Status=HmRegisterWrite(Io,Address,Data,sizeof(Data));
  HmClearFirmwareBytes(Data,sizeof(Data));
  return Status;
}

/* Stock wait_wip uses 101 reads. Its argument is the delay in milliseconds
 * between busy reads (1 for a page, 100 for an erase block). */
static HM_STATUS HmWaitFirmwareWip(const HM_IO *Io,unsigned DelayMs) {
  unsigned char StatusBytes[4];
  unsigned Poll;
  HM_STATUS Status=HmWriteFirmwareWord(Io,0x80000010U,0x00020780U);
  if (Status!=HmOk) { HmClearFirmwareBytes(StatusBytes,sizeof(StatusBytes)); return Status; }
  for (Poll=0;Poll<HM_FW_WIP_POLLS;Poll++) {
    Status=HmWriteFirmwareWord(Io,0x80000020U,0x42000003U);
    if (Status!=HmOk) break;
    Status=HmWriteFirmwareWord(Io,0x80000024U,5U);
    if (Status!=HmOk) break;
    Status=HmReadRegister32(Io,0x8000002cU,StatusBytes);
    if (Status!=HmOk) break;
    if ((StatusBytes[0]&1U)==0) {
      HmClearFirmwareBytes(StatusBytes,sizeof(StatusBytes));
      return HmOk;
    }
    if (Poll+1U<HM_FW_WIP_POLLS)
      Io->Delay(Io->Context,DelayMs*1000U,DelayMs*1000U+1U);
  }
  if (Status==HmOk) Status=HmTimeout;
  HmClearFirmwareBytes(StatusBytes,sizeof(StatusBytes));
  return Status;
}

static void HmStoreFirmwareProgress(HM_FIRMWARE_PROGRESS *Output,
                                    HM_FIRMWARE_PROGRESS *Progress,
                                    HM_STATUS Status) {
  if (!Output || !Progress) return;
  Progress->LastStatus=Status;
  *Output=*Progress;
}

HM_STATUS HmProgramT70Firmware(const HM_IO *Io,const unsigned char *Image,
  unsigned Length,const HM_FIRMWARE_UPDATE_PROFILE *Profile,
  HM_FIRMWARE_PROGRESS *Progress) {
  HM_FIRMWARE_INFO Firmware;
  HM_FIRMWARE_PROGRESS P={0};
  unsigned Offset;
  unsigned char StatusBytes[4];
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || !Image || !Progress) return HmInvalid;
  Status=HmValidateFirmwareUpdateProfile(Profile,HM_FW_FACTS_PROGRAM);
  if (Status!=HmOk) return Status;
  Status=HmValidateT70Firmware(Image,Length,&Firmware);
  if (Status!=HmOk) return Status;
  P.Stage=HmFwStagePreflight;
  P.TotalBytes=Length;
  Status=HmReadChipId(Io);
  if (Status!=HmOk) goto Done;
  Status=HmInterfaceOn(Io);
  if (Status!=HmOk) goto Done;
  P.Stage=HmFwStageControllerReady;
  Status=HmWriteFirmwareWord(Io,0x80000010U,0x00020780U);
  if (Status!=HmOk) goto Done;

  for (Offset=0;Offset<Length;Offset+=HM_T70_FW_PAGE_BYTES) {
    P.CurrentOffset=Offset;
    P.Stage=HmFwStagePageSetup;
    P.FlashContentsMayBeChanged=1;
    Status=HmWriteFirmwareWord(Io,0x80000020U,0x47000000U);
    if (Status!=HmOk) goto Done;
    Status=HmWriteFirmwareWord(Io,0x80000024U,6U);
    if (Status!=HmOk) goto Done;
    Status=HmWriteFirmwareWord(Io,0x80000020U,0x42000000U);
    if (Status!=HmOk) goto Done;
    Status=HmWriteFirmwareWord(Io,0x80000024U,5U);
    if (Status!=HmOk) goto Done;
    Status=HmReadRegister32(Io,0x8000002cU,StatusBytes);
    if (Status!=HmOk) goto Done;
    if ((StatusBytes[0]&2U)==0) { Status=HmStateError; goto Done; }

    Status=HmWriteFirmwareWord(Io,0x80000020U,0x47000000U);
    if (Status!=HmOk) goto Done;
    Status=HmWriteFirmwareWord(Io,0x80000024U,6U);
    if (Status!=HmOk) goto Done;
    Status=HmWriteFirmwareWord(Io,0x80000020U,0x610ff000U);
    if (Status!=HmOk) goto Done;
    P.Stage=HmFwStagePageAddress;
    Status=HmWriteFirmwareWord(Io,0x80000028U,Offset);
    if (Status!=HmOk) goto Done;
    P.Stage=HmFwStagePagePayload;
    Status=HmRegisterWrite(Io,0x8000002cU,Image+Offset,16U);
    if (Status!=HmOk) goto Done;
    Status=HmWriteFirmwareWord(Io,0x80000024U,2U);
    if (Status!=HmOk) goto Done;
    Status=HmRegisterWrite(Io,0x8000002cU,Image+Offset+16U,240U);
    if (Status!=HmOk) goto Done;
    P.Stage=HmFwStagePageWait;
    Status=HmWaitFirmwareWip(Io,1U);
    if (Status!=HmOk) goto Done;
    P.PagesCompleted++;
    P.BytesCompleted=Offset+HM_T70_FW_PAGE_BYTES;
    P.CurrentOffset=P.BytesCompleted;
  }
  P.Stage=HmFwStageProgrammed;
Done:
  HmClearFirmwareBytes(StatusBytes,sizeof(StatusBytes));
  HmStoreFirmwareProgress(Progress,&P,Status);
  return Status;
}

HM_STATUS HmVerifyT70FirmwareFlash(const HM_IO *Io,const unsigned char *Image,
  unsigned Length,const HM_FIRMWARE_UPDATE_PROFILE *Profile,
  HM_FIRMWARE_PROGRESS *Progress) {
  HM_FIRMWARE_INFO Firmware;
  HM_FIRMWARE_PROGRESS P={0};
  unsigned char Start[4]={0,0,0,0},WordCount[4]={0,0,0x99,0},StatusBytes[4],CrcBytes[4];
  unsigned ExpectedCrc,Words,Poll;
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || !Image || !Progress) return HmInvalid;
  Status=HmValidateFirmwareUpdateProfile(Profile,HM_FW_FACTS_VERIFY);
  if (Status!=HmOk) return Status;
  Status=HmValidateT70Firmware(Image,Length,&Firmware);
  if (Status!=HmOk) return Status;
  Status=HmCrc32c(Image,Length,&ExpectedCrc);
  if (Status!=HmOk) return Status;
  P.Stage=HmFwStagePreflight;
  P.TotalBytes=Length;
  Status=HmReadChipId(Io);
  if (Status!=HmOk) goto Done;
  Status=HmInterfaceOn(Io);
  if (Status!=HmOk) goto Done;
  P.Stage=HmFwStageControllerReady;
  P.Stage=HmFwStageCrcSetup;
  Status=HmRegisterWrite(Io,0x80050020U,Start,sizeof(Start));
  if (Status!=HmOk) goto Done;
  Words=(Length+3U)>>2;
  if (Words>0xffffU) { Status=HmUnsupported; goto Done; }
  WordCount[0]=(unsigned char)Words;
  WordCount[1]=(unsigned char)(Words>>8);
  Status=HmRegisterWrite(Io,0x80050028U,WordCount,sizeof(WordCount));
  if (Status!=HmOk) goto Done;

  P.Stage=HmFwStageCrcPoll;
  for (Poll=0;Poll<HM_FW_WIP_POLLS;Poll++) {
    Status=HmRegisterRead(Io,0x80050000U,StatusBytes,sizeof(StatusBytes));
    if (Status!=HmOk) goto Done;
    if ((StatusBytes[0]&1U)==0) break;
    Io->Delay(Io->Context,1000U,1100U);
  }
  if (Poll==HM_FW_WIP_POLLS) { Status=HmTimeout; goto Done; }
  P.Stage=HmFwStageCrcRead;
  Status=HmRegisterRead(Io,0x80050018U,CrcBytes,sizeof(CrcBytes));
  if (Status!=HmOk) goto Done;
  P.ObservedCrc32c=(unsigned)CrcBytes[0]|
    ((unsigned)CrcBytes[1]<<8)|((unsigned)CrcBytes[2]<<16)|
    ((unsigned)CrcBytes[3]<<24);
  if (P.ObservedCrc32c!=ExpectedCrc) { Status=HmChecksum; goto Done; }
  P.BytesCompleted=Length;
  P.Stage=HmFwStageVerified;
  Status=HmOk;
Done:
  HmClearFirmwareBytes(Start,sizeof(Start));
  HmClearFirmwareBytes(WordCount,sizeof(WordCount));
  HmClearFirmwareBytes(StatusBytes,sizeof(StatusBytes));
  HmClearFirmwareBytes(CrcBytes,sizeof(CrcBytes));
  HmStoreFirmwareProgress(Progress,&P,Status);
  return Status;
}

HM_STATUS HmSenseOn(const HM_IO *Io) {
  const unsigned char Clear[4]={0,0,0,0};
  const unsigned char Start=0;
  HM_STATUS Status=HmInterfaceOn(Io);
  /* Stock continues after ten mismatched readbacks, but not a failed transfer. */
  if (Status!=HmOk && Status!=HmTimeout) return Status;
  Status=HmRegisterWrite(Io,0x9000005cU,Clear,sizeof(Clear));
  if (Status!=HmOk) return Status;
  Io->Delay(Io->Context,10000,10001);
  return HmBusWrite(Io,0x31,HM_BUS_WRITE_SENTINEL,&Start,1);
}


#define HM_SENSE_OFF_PHASE_ATTEMPTS 6U
#define HM_SENSE_OFF_STATUS_POLLS 51U

static int HmFirmwareSummaryIsT70(const HM_FIRMWARE_INFO *Info) {
  static const HM_FW_DESCRIPTOR Expected[]={
    {HM_FW_TAG_A00,0x00024400U,0x70U},
    {HM_FW_TAG_10000000,0x00025402U,0x100U},
    {HM_FW_TAG_10000100,0x00025405U,0x110U},
    {HM_FW_TAG_10000600,0x00025500U,0x140U}
  };
  unsigned I;
  if (!Info || Info->Length!=HM_T70_FW_BYTES || Info->Crc32c!=0 ||
      Info->DescriptorCount!=sizeof(Expected)/sizeof(Expected[0])) return 0;
  for (I=0;I<Info->DescriptorCount;I++)
    if (Info->Descriptors[I].Tag!=Expected[I].Tag ||
        Info->Descriptors[I].Value!=Expected[I].Value ||
        Info->Descriptors[I].Offset!=Expected[I].Offset) return 0;
  return 1;
}

static HM_STATUS HmReadRegister32(const HM_IO *Io,unsigned Address,
                                  unsigned char Data[4]) {
  return HmRegisterRead(Io,Address,Data,4);
}

/* The stock helper always sends all 51 requests and its return is ignored.
 * Completion is checked separately by the broadcast handshake. */
static HM_STATUS HmRequestFirmwareStop(const HM_IO *Io) {
  unsigned char Data[4],Stop[4]={0x5a,0,0,0};
  unsigned Attempt;
  HM_STATUS Status=HmReadRegister32(Io,0x900000a8U,Data);
  if (Status!=HmOk) return Status;
  if (Data[0]!=0x05) return HmOk;
  for (Attempt=0;Attempt<HM_SENSE_OFF_STATUS_POLLS;Attempt++) {
    Status=HmReadRegister32(Io,0x9000005cU,Data);
    if (Status!=HmOk) return Status;
    Status=HmRegisterWrite(Io,0x9000005cU,Stop,sizeof(Stop));
    if (Status!=HmOk) return Status;
    Io->Delay(Io->Context,20000,21000);
  }
  return HmOk;
}

static HM_STATUS HmWaitBroadcastLine(const HM_IO *Io) {
  unsigned char Data[4];
  unsigned Phase,BusyPolls=0;
  HM_STATUS Status=HmBurstEnable(Io,1),ClearStatus;
  if (Status!=HmOk) return Status;
  Status=HmWriteFirmwareWord(Io,0x8007008cU,1);
  if (Status!=HmOk) {
    HmWriteFirmwareWord(Io,0x8007008cU,0);
    return Status;
  }
  for (Phase=0;Phase<2;Phase++) {
    if (Phase) {
      ClearStatus=HmWriteFirmwareWord(Io,0x8007008cU,0);
      if (ClearStatus!=HmOk) return ClearStatus;
    }
    for (;;) {
      Io->Delay(Io->Context,10000,11000);
      Status=HmReadRegister32(Io,0x80070034U,Data);
      if (Status!=HmOk) {
        if (!Phase) HmWriteFirmwareWord(Io,0x8007008cU,0);
        return Status;
      }
      if (!Data[0]) break;
      if (BusyPolls==100U) {
        if (!Phase) HmWriteFirmwareWord(Io,0x8007008cU,0);
        return HmTimeout;
      }
      BusyPolls++;
    }
  }
  return HmOk;
}

HM_STATUS HmSenseOff(const HM_IO *Io,const HM_FIRMWARE_INFO *Firmware,
                     const HM_SENSE_OFF_PROFILE *Profile) {
  const unsigned char SenseOffCommand[2]={0x27,0x95};
  unsigned char Command[4]={0xa5,0,0,0},StatusBytes[4],Data[4];
  unsigned Attempt;
  HM_STATUS Status;
  if (HmValidateIo(Io)!=HmOk || !Firmware || !Profile) return HmInvalid;
  if (Profile->Version!=HM_SENSE_OFF_PROFILE_VERSION ||
      Profile->Size!=sizeof(*Profile)) return HmInvalid;
  if ((Profile->KnownFlagsMask&HM_SENSE_OFF_RUNTIME_BIT4)==0 ||
      (Profile->FeatureFlags&HM_SENSE_OFF_RUNTIME_BIT4)!=0 ||
      !HmFirmwareSummaryIsT70(Firmware)) return HmUnsupported;

  /* _hx83132_init passes argument 0; the bit4-clear path has no slave scan. */
  Status=HmRegisterWrite(Io,0x9000005cU,Command,sizeof(Command));
  if (Status!=HmOk) return Status;
  Status=HmReadRegister32(Io,0x900000a8U,Data);
  if (Status!=HmOk) return Status;
  if (Data[0]==0x05) {
    Io->Delay(Io->Context,10000,10001);
    Status=HmReadRegister32(Io,0x9000005cU,StatusBytes);
    if (Status!=HmOk) return Status;
  }

  for (Attempt=0;Attempt<HM_SENSE_OFF_PHASE_ATTEMPTS;Attempt++) {
    Status=HmBusWrite(Io,0x31,HM_BUS_WRITE_SENTINEL,
                      SenseOffCommand,sizeof(SenseOffCommand));
    if (Status!=HmOk) return Status;
    Status=HmReadRegister32(Io,0x900000a8U,StatusBytes);
    if (Status!=HmOk) return Status;
    if (StatusBytes[0]==0x0c) break;
    Io->Delay(Io->Context,10000,10001);
    Status=HmSystemReset(Io);
    if (Status!=HmOk) return Status;
  }
  if (Attempt==HM_SENSE_OFF_PHASE_ATTEMPTS) return HmTimeout;

  Status=HmRequestFirmwareStop(Io);
  if (Status!=HmOk) return Status;
  Status=HmWaitBroadcastLine(Io);
  if (Status!=HmOk) return Status;
  Status=HmWriteFirmwareWord(Io,0x80070084U,0x7e);
  if (Status!=HmOk) return Status;
  Status=HmReadRegister32(Io,0x80070004U,Data);
  if (Status!=HmOk) return Status;
  Data[0]|=0x20;
  Data[1]|=1;
  Status=HmRegisterWrite(Io,0x80070004U,Data,sizeof(Data));
  if (Status!=HmOk) return Status;
  Status=HmWriteFirmwareWord(Io,0xc00f3100U,0x27);
  if (Status!=HmOk) return Status;
  Status=HmWriteFirmwareWord(Io,0xc00f3200U,0x95);
  if (Status!=HmOk) return Status;
  Status=HmReadRegister32(Io,0x80070004U,Data);
  if (Status!=HmOk) return Status;
  Data[1]&=0x7e;
  Status=HmRegisterWrite(Io,0x80070004U,Data,sizeof(Data));
  if (Status!=HmOk) return Status;
  Status=HmReadRegister32(Io,0x900000a8U,Data);
  if (Status!=HmOk) return Status;
  if (Data[0]!=0x05) return HmWriteFirmwareWord(Io,0x9000005cU,0);
  return HmOk;
}

HM_STATUS HmEnter(const HM_IO *Io,HM_ENTRY_REASON Reason) {
  unsigned char FirmwareState[4];
  HM_STATUS Status;
  if (Reason!=HmEntryInitialHandoff && Reason!=HmEntryRetainedResume) return HmInvalid;
  Status=HmReadChipId(Io);
  if (Status!=HmOk) return Status;
  if (Reason==HmEntryInitialHandoff) return HmSenseOn(Io);
  Status=HmReadRegister32(Io,0x900000a8U,FirmwareState);
  if (Status!=HmOk) return Status;
  return FirmwareState[0]==0x05?HmOk:HmStateError;
}
