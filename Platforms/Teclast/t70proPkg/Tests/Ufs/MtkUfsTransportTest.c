/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MtkUfsTransport.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static unsigned Checks;
#define CHECK(X) do { Checks++; if(!(X)) { fprintf(stderr,"line %u: %s\n",__LINE__,#X); abort(); } } while(0)
static uint32_t Reg[0x2300/4], Writes, Fences, Stalls;
static int Stuck;
static _Alignas(4096) uint8_t Memory[MTK_UFS_DMA_BYTES+16];
static uint32_t Read(void *C,uint32_t O) { CHECK(C==Reg && O<sizeof(Reg) && !(O&3)); return Reg[O/4]; }
static void Write(void *C,uint32_t O,uint32_t V) {
  CHECK(C==Reg && O<sizeof(Reg) && !(O&3)); Writes++;
  if(O==0x20) Reg[O/4]&=~V;
  else if(O==0x34 && !V && !Stuck) { Reg[O/4]=0; Reg[0x58/4]=Reg[0x78/4]=0; }
  else if(O==0x34 && !V && Stuck) { }
  else Reg[O/4]=V;
}
static void EnableInterrupts(void) { Write(Reg,0x24,MTK_UFS_INTERRUPTS); }
static void Fence(void *C) { CHECK(C==Reg); Fences++; }
static void Stall(void *C,uint32_t U) { CHECK(C==Reg && U==10); Stalls++; }
static const MTK_UFS_TRANSPORT_IO Io={Reg,Read,Write,Fence,Stall};
static void Reset(MTK_UFS_TRANSPORT *T) {
  memset(T,0,sizeof(*T)); memset(Reg,0,sizeof(Reg)); memset(Memory,0xa5,sizeof(Memory));
  Reg[0]=0x0100001f; Reg[8/4]=0x210; Reg[0x34/4]=1; Reg[0x30/4]=0xf;
  Reg[0x2240/4]=0x10450000;
  Writes=Fences=Stalls=0; Stuck=0;
}
static uint32_t L32(const uint8_t *P) { return (uint32_t)P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24); }
static void Complete(MTK_UFS_TRANSPORT *T, int Nop) {
  uint8_t *P=Memory+MTK_UFS_RESPONSE_OFFSET;
  memset(P,0,64); P[0]=Nop?0x20:0x21; P[2]=Nop?0:T->ActiveLun; P[3]=T->ActiveTag;
  Memory[8]=0; Reg[0x58/4]=0; Reg[0x20/4]|=1;
}
static void CompleteQuery(MTK_UFS_TRANSPORT *T,uint8_t Value) {
  uint8_t *P=Memory+MTK_UFS_RESPONSE_OFFSET;
  memset(P,0,64); P[0]=0x36; P[3]=T->ActiveTag;
  P[5]=T->QueryOpcode==5U?1U:0x81U; P[12]=T->QueryOpcode; P[13]=4; P[23]=Value;
  Memory[8]=0; Reg[0x58/4]=0; Reg[0x20/4]|=1;
}
static void TestPrivatePowerTransport(void) {
  MTK_UFS_TRANSPORT T; MTK_UFS_IO_RESULT Out,Before;
  unsigned I,Mode; uint32_t Saved;
  const uint8_t BadResponse[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
                               16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
  const uint8_t BadDescriptor[]={0,8,9,10,11,16,20,24,26,28,30};
  CHECK(MtkUfsTransportReadAutoBkops(NULL)==MtkUfsIoNotReady);
  CHECK(MtkUfsTransportDisableAutoBkops(NULL)==MtkUfsIoNotReady);
  CHECK(MtkUfsTransportPowerActive(NULL)==MtkUfsIoNotReady);
  for(Mode=0;Mode<2;Mode++) {
    Reset(&T);
    CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    CHECK((Mode?MtkUfsTransportDisableAutoBkops(&T):MtkUfsTransportReadAutoBkops(&T))==MtkUfsIoSuccess);
    CHECK(T.Active && !T.Nop && T.QueryOpcode==(Mode?7U:5U) && !T.ExpectedBytes && !T.ActiveLun);
    CHECK(Memory[MTK_UFS_COMMAND_OFFSET]==0x16 && !Memory[MTK_UFS_COMMAND_OFFSET+1] &&
          !Memory[MTK_UFS_COMMAND_OFFSET+2] && Memory[MTK_UFS_COMMAND_OFFSET+5]==(Mode?0x81U:1U) &&
          Memory[MTK_UFS_COMMAND_OFFSET+12]==(Mode?7U:5U) &&
          Memory[MTK_UFS_COMMAND_OFFSET+13]==4 &&
          L32(Memory)==0x11000000 && L32(Memory+24)==0x00080010 &&
          L32(Memory+28)==0x00180000);
    for(I=0;I<32;I++) if(I!=0 && I!=3 && I!=5 && I!=12 && I!=13)
      CHECK(!Memory[MTK_UFS_COMMAND_OFFSET+I]);
    memset(&Out,0xc3,sizeof(Out)); Before=Out; Saved=Writes;
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoPending && !memcmp(&Out,&Before,sizeof(Out)) &&
          Writes==Saved && T.Active);
    CHECK(MtkUfsTransportReadAutoBkops(&T)==MtkUfsIoBusy &&
          MtkUfsTransportDisableAutoBkops(&T)==MtkUfsIoBusy &&
          MtkUfsTransportPowerActive(&T)==MtkUfsIoBusy && Writes==Saved);
    CompleteQuery(&T,Mode?0:1);
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && !T.Active && !T.Faulted &&
          Out.QueryFlagValid==1 && Out.QueryFlagValue==(Mode?0U:1U) &&
          !Out.Transferred && !Out.ScsiStatus && !Out.SenseLength);
    /* A separate READ proves the flag value after the clear acknowledgment. */
    CHECK(MtkUfsTransportReadAutoBkops(&T)==MtkUfsIoSuccess);
    CompleteQuery(&T,0);
    Memory[MTK_UFS_RESPONSE_OFFSET+9]=1; /* permitted device information */
    memset(Memory+MTK_UFS_RESPONSE_OFFSET+32,0xa5,32); /* no data segment */
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && Out.QueryFlagValid && !Out.QueryFlagValue);
    CHECK(MtkUfsTransportNop(&T)==MtkUfsIoSuccess && !T.QueryOpcode);
    Complete(&T,1);
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess &&
          !Out.QueryFlagValid && !Out.QueryFlagValue);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  /* Every wire identity, function, reserved field and flag value is checked.
   * A malformed/unknown response keeps the active slot and caller output. */
  for(Mode=0;Mode<2;Mode++) for(I=0;I<sizeof(BadResponse);I++) {
    uint8_t *P=Memory+MTK_UFS_RESPONSE_OFFSET;
    Reset(&T);
    CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    CHECK((Mode?MtkUfsTransportDisableAutoBkops(&T):MtkUfsTransportReadAutoBkops(&T))==MtkUfsIoSuccess);
    CompleteQuery(&T,0);
    if(BadResponse[I]==9) P[9]=2;
    else if(BadResponse[I]==23) P[23]=Mode?1U:2U;
    else P[BadResponse[I]]++;
    memset(&Out,0xcc,sizeof(Out)); Before=Out; Saved=Writes;
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoError && T.Faulted && T.Active &&
          !memcmp(&Out,&Before,sizeof(Out)) && Writes==Saved);
    CHECK(MtkUfsTransportReadAutoBkops(&T)==MtkUfsIoNotReady &&
          MtkUfsTransportDisableAutoBkops(&T)==MtkUfsIoNotReady && Writes==Saved);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  for(I=0;I<sizeof(BadDescriptor);I++) {
    Reset(&T);
    CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    CHECK(MtkUfsTransportReadAutoBkops(&T)==MtkUfsIoSuccess); CompleteQuery(&T,0);
    Memory[BadDescriptor[I]]^=1U;
    memset(&Out,0xcc,sizeof(Out)); Before=Out; Saved=Writes;
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoError && T.Faulted && T.Active &&
          !memcmp(&Out,&Before,sizeof(Out)) && Writes==Saved);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  /* Fresh ownership gates are checked both before query submission and
   * completion; an old Attached bit cannot authorize foreign DMA or HCI. */
  {
    const uint32_t Roots[]={0x30,0x34,0x60,0x80,0x78,0x24,0x50,0x54,0x70,0x74};
    for(Mode=0;Mode<2;Mode++) for(I=0;I<sizeof(Roots)/sizeof(Roots[0]);I++) {
      uint32_t Old;
      Reset(&T);
      CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
      if(Mode) {
        CHECK(MtkUfsTransportReadAutoBkops(&T)==MtkUfsIoSuccess);
        CompleteQuery(&T,0);
      }
      Old=Reg[Roots[I]/4]; Reg[Roots[I]/4]=UINT32_MAX;
      memset(&Out,0xcc,sizeof(Out)); Before=Out; Saved=Writes;
      if(Mode) CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoError && T.Active &&
                     !memcmp(&Out,&Before,sizeof(Out)));
      else CHECK(MtkUfsTransportReadAutoBkops(&T)==MtkUfsIoError && !T.Active);
      CHECK(T.Faulted && Writes==Saved);
      Reg[Roots[I]/4]=Old;
      CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
    }
  }
  /* Resume sends only the fixed ACTIVE condition to the device WLUN. */
  {
    const uint8_t Active[6]={0x1b,0,0,0,0x10,0};
    Reset(&T);
    CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    Saved=Writes;
    CHECK(!MtkUfsTransportReadCdb(Active,sizeof(Active),0) &&
          !MtkUfsTransportWriteCdb(Active,sizeof(Active),0));
    CHECK(MtkUfsTransportScsi(&T,Active,sizeof(Active),0)==MtkUfsIoInvalid &&
          MtkUfsTransportWriteScsi(&T,Active,sizeof(Active),0,NULL)==MtkUfsIoInvalid && Writes==Saved);
    CHECK(MtkUfsTransportPowerActive(&T)==MtkUfsIoSuccess && T.Active &&
          T.ActiveLun==MTK_UFS_DEVICE_WLUN && !T.QueryOpcode &&
          Memory[MTK_UFS_COMMAND_OFFSET+2]==MTK_UFS_DEVICE_WLUN &&
          !memcmp(Memory+MTK_UFS_COMMAND_OFFSET+16,Active,sizeof(Active)));
    Complete(&T,0);
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && !Out.QueryFlagValid &&
          !Out.ScsiStatus && !Out.Transferred);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
}
static MTK_UFS_IO_STATUS SubmitOwnershipCase(MTK_UFS_TRANSPORT *T,unsigned Kind) {
  const uint8_t ReadCdb[10]={0x28,0,0,0,0,1,0,0,1,0};
  const uint8_t FlushCdb[10]={0x35};
  switch(Kind) {
  case 0: return MtkUfsTransportNop(T);
  case 1: return MtkUfsTransportScsi(T,ReadCdb,sizeof(ReadCdb),MTK_UFS_BLOCK_BYTES);
  case 2: return MtkUfsTransportWriteScsi(T,FlushCdb,sizeof(FlushCdb),0,NULL);
  case 3: return MtkUfsTransportPowerActive(T);
  default: return MtkUfsTransportReadAutoBkops(T);
  }
}
static void TestAllCommandOwnership(void) {
  const uint32_t Roots[]={0x30,0x34,0x60,0x80,0x78,0x24,0x50,0x54,0x70,0x74};
  static uint8_t BeforeMemory[sizeof(Memory)];
  MTK_UFS_TRANSPORT T;
  MTK_UFS_IO_RESULT Out,BeforeOut;
  for(unsigned Kind=0;Kind<5;Kind++) for(unsigned Phase=0;Phase<3;Phase++)
    for(unsigned I=0;I<sizeof(Roots)/sizeof(Roots[0]);I++) {
      Reset(&T);
      CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
      if(Phase) {
        CHECK(SubmitOwnershipCase(&T,Kind)==MtkUfsIoSuccess);
        if(Phase==2) {
          if(Kind==4) CompleteQuery(&T,0); else Complete(&T,Kind==0);
        }
      }
      /* Valid-but-foreign register values, not only inaccessible all-ones. */
      if(Roots[I]==0x30) Reg[Roots[I]/4]=0;
      else Reg[Roots[I]/4]^=0x1000;
      memcpy(BeforeMemory,Memory,sizeof(Memory));
      memset(&Out,0xcc,sizeof(Out)); BeforeOut=Out;
      uint32_t BeforeWrites=Writes;
      if(!Phase) CHECK(SubmitOwnershipCase(&T,Kind)==MtkUfsIoError && !T.Active);
      else CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoError && T.Active &&
                 !memcmp(&Out,&BeforeOut,sizeof(Out)));
      CHECK(T.Faulted && Writes==BeforeWrites && !memcmp(Memory,BeforeMemory,sizeof(Memory)));
    }
}
int main(void) {
  MTK_UFS_TRANSPORT T; MTK_UFS_IO_RESULT Out,Before;
  uint8_t Cdb[16]={0x28,0,0,0,0,1,0,0,1,0};
  const uint32_t Gates[]={0x34,0x30,0x58,0x78,0x60,0x80,0x24,0x50,0x54,0x70,0x74,0x18};
  const uint32_t UnknownIe[]={2U,UINT32_MAX};
  unsigned I,J; uint32_t Saved;
  Reset(&T);
  CHECK(MtkUfsTransportAttach(0,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoInvalid);
  CHECK(MtkUfsTransportAttach(&T,0,Memory,sizeof(Memory),0x80000000)==MtkUfsIoInvalid);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,MTK_UFS_DMA_BYTES-1,0x80000000)==MtkUfsIoInvalid);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000001)==MtkUfsIoInvalid);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory+1,sizeof(Memory)-1,0x80000000)==MtkUfsIoInvalid);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x1000000000ULL)==MtkUfsIoInvalid);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x1000000000ULL-MTK_UFS_DMA_BYTES+1024)==MtkUfsIoInvalid);
  CHECK(!Writes);
  for(I=0;I<sizeof(Gates)/sizeof(Gates[0]);I++) {
    Reset(&T); Reg[Gates[I]/4]=(Gates[I]==0x34||Gates[I]==0x30)?0:1;
    CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoNotReady);
    CHECK(!Writes && !T.Attached && Memory[0]==0xa5);
  }
  Reset(&T); Reg[0]=0;
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x100000000ULL)==MtkUfsIoInvalid);
  Reset(&T); Reg[2]=0x400;
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoInvalid && !Writes);
  Reset(&T); memset(&T,0xa5,sizeof(T)); T.Attached=0; T.Active=0; T.Faulted=0;
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x180000000ULL)==MtkUfsIoSuccess);
  CHECK(!T.UicPending && !T.Hibernate8CommandDone && !T.StopInProgress && !T.StopPhase);
  CHECK(Reg[0x50/4]==0x80000000 && Reg[0x54/4]==1 && Reg[0x60/4]==1 && !Reg[0x80/4]);
  CHECK(Reg[0x70/4]==0x80000400 && Reg[0x74/4]==1 && Fences);
  CHECK(Memory[MTK_UFS_DMA_BYTES]==0xa5);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoBusy);
  CHECK(MtkUfsTransportNop(&T)==MtkUfsIoSuccess);
  CHECK(L32(Memory)==0x11000000 && L32(Memory+16)==0x80000800 && L32(Memory+20)==1);
  CHECK(L32(Memory+24)==0x00080010 && L32(Memory+28)==0x00180000);
  CHECK(Memory[MTK_UFS_COMMAND_OFFSET]==0 && Memory[MTK_UFS_COMMAND_OFFSET+3]==1);
  memset(&Out,0xa6,sizeof(Out)); Before=Out;
  CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoPending && !memcmp(&Out,&Before,sizeof(Out)));
  CHECK(MtkUfsTransportNop(&T)==MtkUfsIoBusy);
  Complete(&T,1); CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && !T.Active && !Out.Transferred);
  CHECK(MtkUfsTransportScsi(&T,Cdb,10,4096)==MtkUfsIoSuccess);
  CHECK(L32(Memory)==0x15000000 && L32(Memory+28)==0x00180001);
  CHECK(L32(Memory+MTK_UFS_PRD_OFFSET)==0x80001000 && L32(Memory+MTK_UFS_PRD_OFFSET+4)==1);
  CHECK(L32(Memory+MTK_UFS_PRD_OFFSET+8)==0 && L32(Memory+MTK_UFS_PRD_OFFSET+12)==4095);
  CHECK(Memory[MTK_UFS_COMMAND_OFFSET+1]==0x40 && Memory[MTK_UFS_COMMAND_OFFSET+2]==2);
  CHECK(Memory[MTK_UFS_COMMAND_OFFSET+14]==0x10 && !memcmp(Memory+MTK_UFS_COMMAND_OFFSET+16,Cdb,10));
  Complete(&T,0); CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && Out.Transferred==4096);
  CHECK(MtkUfsTransportScsi(&T,Cdb,10,4096)==MtkUfsIoSuccess); Complete(&T,0);
  Memory[MTK_UFS_RESPONSE_OFFSET+1]=0x20; Memory[MTK_UFS_RESPONSE_OFFSET+14]=8;
  CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && Out.Transferred==2048);
  CHECK(MtkUfsTransportScsi(&T,Cdb,10,4096)==MtkUfsIoSuccess); Complete(&T,0);
  Memory[MTK_UFS_RESPONSE_OFFSET+7]=2; Memory[MTK_UFS_RESPONSE_OFFSET+11]=20;
  Memory[MTK_UFS_RESPONSE_OFFSET+33]=18; memset(Memory+MTK_UFS_RESPONSE_OFFSET+34,0x70,18);
  CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && Out.ScsiStatus==2 && Out.SenseLength==18 && Out.Sense[17]==0x70);
  {
    const uint8_t SenseCdb[6]={0x03,0,0,0,18,0};
    memset(Memory+MTK_UFS_DATA_OFFSET,0xa5,24);
    CHECK(MtkUfsTransportScsi(&T,SenseCdb,sizeof(SenseCdb),18)==MtkUfsIoSuccess);
    CHECK(L32(Memory+MTK_UFS_PRD_OFFSET+12)==19 && Memory[MTK_UFS_COMMAND_OFFSET+15]==18);
    CHECK(!Memory[MTK_UFS_DATA_OFFSET+18] && !Memory[MTK_UFS_DATA_OFFSET+19]);
    CHECK(Memory[MTK_UFS_DATA_OFFSET+20]==0xa5);
    Complete(&T,0);
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && Out.Transferred==18 && !T.Faulted);
  }
  CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess && !T.Attached && T.Faulted && !Reg[0x50/4]);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoBusy);
  Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
  CHECK(MtkUfsTransportNop(&T)==MtkUfsIoSuccess);
  CHECK(MtkUfsTransportDetach(&T)==MtkUfsIoBusy && T.Attached);
  Complete(&T,1); CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess);
  CHECK(MtkUfsTransportDetach(&T)==MtkUfsIoSuccess && !T.Attached && !T.Faulted && Reg[0x34/4]==1);
  CHECK(!Reg[0x50/4] && !Reg[0x54/4] && !Reg[0x70/4] && !Reg[0x74/4]);
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
  CHECK(MtkUfsTransportDetach(&T)==MtkUfsIoSuccess);
  Reset(&T); Reg[0x30/4]=UINT32_MAX;
  CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoNotReady && !Writes);
  /* Malformed DMA replies never release the slot, modify the result or permit reuse. */
  for(I=0;I<14;I++) {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    CHECK(MtkUfsTransportScsi(&T,Cdb,10,4096)==MtkUfsIoSuccess); Complete(&T,0);
    switch(I) {
    case 0: Memory[8]=1; break;
    case 1: Memory[MTK_UFS_RESPONSE_OFFSET]=0x61; break;
    case 2: Memory[MTK_UFS_RESPONSE_OFFSET+2]=0; break;
    case 3: Memory[MTK_UFS_RESPONSE_OFFSET+3]++; break;
    case 4: Memory[MTK_UFS_RESPONSE_OFFSET+4]=1; break;
    case 5: Memory[MTK_UFS_RESPONSE_OFFSET+6]=1; break;
    case 6: Memory[MTK_UFS_RESPONSE_OFFSET+8]=1; break;
    case 7: Memory[MTK_UFS_RESPONSE_OFFSET+1]=0x40; break;
    case 8: Memory[MTK_UFS_RESPONSE_OFFSET+15]=1; break;
    case 9: Memory[MTK_UFS_RESPONSE_OFFSET+1]=0x20; break;
    case 10: Memory[MTK_UFS_RESPONSE_OFFSET+1]=0x20; Memory[MTK_UFS_RESPONSE_OFFSET+14]=0x20; break;
    case 11: Memory[MTK_UFS_RESPONSE_OFFSET+11]=33; break;
    case 12: Reg[0x20/4]=1<<16; break;
    case 13: Reg[0x58/4]=2; break;
    }
    memset(&Out,0xcc,sizeof(Out)); Before=Out;
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoError && T.Faulted && T.Active);
    CHECK(!memcmp(&Out,&Before,sizeof(Out)));
    CHECK(MtkUfsTransportScsi(&T,Cdb,10,4096)==MtkUfsIoNotReady);
    if(I&1) { Stuck=1; CHECK(MtkUfsTransportStop(&T)==MtkUfsIoUnsafeDma && T.Attached && Stalls==100); }
    Stuck=0; CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess && !T.Attached);
  }
  for(I=0;I<256;I++) for(J=0;J<17;J++) {
    uint8_t C[16]={0}; C[0]=(uint8_t)I;
    if(I==0 && J==6) CHECK(MtkUfsTransportReadCdb(C,J,0));
    else if((I==3||I==0x12||I==0x1a) && J==6) CHECK(MtkUfsTransportReadCdb(C,J,0));
    else if((I==0x5a&&J==10)||(I==0x28&&J==10)||(I==0xa8&&J==12)||(I==0x88&&J==16)) CHECK(MtkUfsTransportReadCdb(C,J,0));
    else CHECK(!MtkUfsTransportReadCdb(C,J,0));
  }
  CHECK(MtkUfsTransportReadCdb(Cdb,10,4096));
  Cdb[1]=8; CHECK(MtkUfsTransportReadCdb(Cdb,10,4096)); Cdb[1]=0x20; CHECK(!MtkUfsTransportReadCdb(Cdb,10,4096)); Cdb[1]=0;
  Cdb[8]=33; CHECK(!MtkUfsTransportReadCdb(Cdb,10,33*4096)); Cdb[8]=1;
  CHECK(!MtkUfsTransportReadCdb(Cdb,10,4095));
  { uint8_t C[16]={0x88}; memset(C+2,0xff,8); C[13]=1; CHECK(!MtkUfsTransportReadCdb(C,16,4096)); }
  { uint8_t C[16]={0x9e,0x10}; C[13]=32; CHECK(MtkUfsTransportReadCdb(C,16,32)); C[1]=0x11; CHECK(!MtkUfsTransportReadCdb(C,16,32)); }
  /* Policy rejection is checked before any controller write, even when idle. */
  Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
  Saved=Writes; Cdb[0]=0x2a;
  CHECK(MtkUfsTransportScsi(&T,Cdb,10,4096)==MtkUfsIoInvalid && Writes==Saved && !T.Active);
  CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  /* Explicit write entry builds a data-out UPIU/PRD and snapshots the caller. */
  {
    static uint8_t Payload[MTK_UFS_DATA_BYTES];
    uint8_t Write[16]={0x2a,0x18,0,0,0,1,0,0,32,0};
    uint8_t Sync[16]={0x35};
    for(I=0;I<sizeof(Payload);I++) Payload[I]=(uint8_t)(I+11);
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    Saved=Writes;
    CHECK(MtkUfsTransportWriteScsi(&T,Write,10,sizeof(Payload),NULL)==MtkUfsIoInvalid && Writes==Saved);
    CHECK(MtkUfsTransportScsi(&T,Write,10,sizeof(Payload))==MtkUfsIoInvalid && Writes==Saved);
    CHECK(MtkUfsTransportWriteScsi(&T,Write,10,sizeof(Payload),Payload)==MtkUfsIoSuccess);
    CHECK(L32(Memory)==0x13000000 && Memory[MTK_UFS_COMMAND_OFFSET+1]==0x20);
    CHECK(L32(Memory+MTK_UFS_PRD_OFFSET+12)==sizeof(Payload)-1 && !memcmp(Memory+MTK_UFS_DATA_OFFSET,Payload,sizeof(Payload)));
    CHECK(Memory[MTK_UFS_DMA_BYTES]==0xa5);
    Payload[0]=0; CHECK(Memory[MTK_UFS_DATA_OFFSET]==11);
    Complete(&T,0); CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && Out.Transferred==sizeof(Payload));
    CHECK(MtkUfsTransportWriteCdb(Sync,10,0) && !MtkUfsTransportReadCdb(Sync,10,0));
    CHECK(MtkUfsTransportWriteScsi(&T,Sync,10,0,NULL)==MtkUfsIoSuccess);
    CHECK(L32(Memory)==0x11000000 && Memory[MTK_UFS_COMMAND_OFFSET+1]==0 && L32(Memory+28)==0x180000);
    Complete(&T,0); CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && !Out.Transferred);
    Sync[1]=2; CHECK(!MtkUfsTransportWriteCdb(Sync,10,0)); Sync[1]=0; Sync[0]=0x91;
    CHECK(MtkUfsTransportWriteCdb(Sync,16,0));
    Sync[0]=0x42; CHECK(!MtkUfsTransportWriteCdb(Sync,10,0));
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  /* Shutdown-only device WLUN powerdown remains outside the public LU2 allowlist. */
  {
    uint8_t PowerDown[6]={0x1b,0,0,0,0x30,0};
    uint8_t Wrong[6]={0x1b,0,0,0,0x20,0};
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    Saved=Writes;
    CHECK(!MtkUfsTransportReadCdb(PowerDown,sizeof(PowerDown),0));
    CHECK(!MtkUfsTransportWriteCdb(PowerDown,sizeof(PowerDown),0));
    CHECK(MtkUfsTransportScsi(&T,PowerDown,sizeof(PowerDown),0)==MtkUfsIoInvalid && Writes==Saved);
    CHECK(MtkUfsTransportWriteScsi(&T,PowerDown,sizeof(PowerDown),0,NULL)==MtkUfsIoInvalid && Writes==Saved);
    CHECK(MtkUfsTransportPowerDown(&T)==MtkUfsIoSuccess && T.Active &&
          T.ActiveLun==MTK_UFS_DEVICE_WLUN && Memory[MTK_UFS_COMMAND_OFFSET+2]==MTK_UFS_DEVICE_WLUN &&
          Memory[MTK_UFS_COMMAND_OFFSET+16]==0x1b && Memory[MTK_UFS_COMMAND_OFFSET+20]==0x30 &&
          L32(Memory+28)==0x00180000);
    CHECK(MtkUfsTransportPowerDown(&T)==MtkUfsIoBusy);
    Complete(&T,0); CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoSuccess && !Out.Transferred);
    CHECK(MtkUfsTransportScsi(&T,Wrong,sizeof(Wrong),0)==MtkUfsIoInvalid);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  /* Powerdown responses must echo the device WLUN and active tag. */
  for(I=0;I<2;I++) {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    CHECK(MtkUfsTransportPowerDown(&T)==MtkUfsIoSuccess); Complete(&T,0);
    if(I==0) Memory[MTK_UFS_RESPONSE_OFFSET+2]=MTK_UFS_USER_LUN;
    else Memory[MTK_UFS_RESPONSE_OFFSET+3]++;
    memset(&Out,0xCC,sizeof(Out)); Before=Out;
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoError && T.Faulted && T.Active &&
          !memcmp(&Out,&Before,sizeof(Out)));
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess && !T.Attached);
  }
  /* Hibern8 enter is a separate UIC operation and requires a confirmed link probe. */
  {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    EnableInterrupts(); /* match the Windows UfsInitialize IE state */
    Reg[0x20/4]|=1U<<10; /* stale UIC completion is cleared before command issue */
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoSuccess && T.UicPending &&
          Reg[0x90/4]==0x17 && !Reg[0x94/4] && !Reg[0x98/4] && !Reg[0x9c/4] &&
          !(Reg[0x20/4]&(1U<<10)));
    memset(&Out,0xa5,sizeof(Out)); Before=Out;
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoBusy && !memcmp(&Out,&Before,sizeof(Out)));
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoPending && T.UicPending);
    Reg[0x98/4]=1; Reg[0x20/4]|=1U<<10;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoError && T.Faulted && T.UicPending);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    EnableInterrupts();
    Reg[0x58/4]=1; Saved=Writes;
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoError && T.Faulted && Writes==Saved && !Reg[0x90/4]);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    EnableInterrupts();
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoSuccess);
    Reg[0x20/4]|=1U<<10; Reg[0x34/4]=0; Saved=Writes;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoError && T.Faulted && Writes==Saved && !Reg[0x22c0/4]);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    EnableInterrupts();
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoSuccess);
    Reg[0x20/4]|=1U<<10;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoPending && !T.UicPending && T.Hibernate8CommandDone);
    CHECK(Reg[0x22c0/4]==0x00820820 && Reg[0x22d0/4]==0 && Reg[0x22d4/4]==0x55555555 &&
          Reg[0x22d8/4]==0xaaaaaaaa && Reg[0x22dc/4]==UINT32_MAX);
    CHECK(MtkUfsTransportPoll(&T,&Out)==MtkUfsIoBusy);
    Reg[0x22c8/4]=0x30000000;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoSuccess && !T.Hibernate8CommandDone);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  {
    Reset(&T); Reg[0x2240/4]=0x10350000;
    CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    EnableInterrupts();
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoSuccess);
    Reg[0x20/4]|=1U<<10; Reg[0x22c8/4]=0x30000000;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoSuccess && Reg[0x22c0/4]==0x20);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  /* The DPC may enter Hibern8 with IE masked; unrelated IE values are rejected. */
  {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    CHECK(Reg[0x24/4]==0 && MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoSuccess &&
          T.UicPending && Reg[0x90/4]==0x17 && Reg[0x24/4]==0);
    Reg[0x20/4]|=1U<<10;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoPending && T.Hibernate8CommandDone &&
          Reg[0x24/4]==0);
    Reg[0x22c8/4]=0x30000000;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoSuccess && Reg[0x24/4]==0);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  for(I=0;I<sizeof(UnknownIe)/sizeof(UnknownIe[0]);I++) {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    Reg[0x24/4]=UnknownIe[I]; Saved=Writes;
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoError && T.Faulted &&
          Writes==Saved && !Reg[0x90/4]);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  for(I=0;I<sizeof(UnknownIe)/sizeof(UnknownIe[0]);I++) {
    Reset(&T); CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoSuccess);
    Reg[0x20/4]|=1U<<10; Reg[0x94/4]=0; Reg[0x24/4]=UnknownIe[I]; Saved=Writes;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoError && T.Faulted &&
          Writes==Saved+1 && !Reg[0x22c0/4]);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  for(I=0;I<2;I++) {
    Reset(&T); Reg[0x2240/4]=I?UINT32_MAX:0;
    CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
    EnableInterrupts();
    CHECK(MtkUfsTransportBeginHibernate8(&T)==MtkUfsIoSuccess);
    Reg[0x20/4]|=1U<<10;
    CHECK(MtkUfsTransportPollHibernate8(&T)==MtkUfsIoError && T.Faulted);
    CHECK(MtkUfsTransportStop(&T)==MtkUfsIoSuccess);
  }
  /* Stop phase 2 must re-prove every DMA-stopping root before accepting zero bases. */
  {
    const uint32_t Roots[]={0x34,0x24,0x60,0x80,0x58,0x78};
    for(I=0;I<sizeof(Roots)/sizeof(Roots[0]);I++) {
      Reset(&T);
      CHECK(MtkUfsTransportAttach(&T,&Io,Memory,sizeof(Memory),0x80000000)==MtkUfsIoSuccess);
      CHECK(MtkUfsTransportBeginStop(&T)==MtkUfsIoPending && T.StopInProgress && T.StopPhase==2 &&
            !Reg[0x34/4] && !Reg[0x24/4] && !Reg[0x60/4] && !Reg[0x80/4] &&
            !Reg[0x58/4] && !Reg[0x78/4] && !Reg[0x50/4] && !Reg[0x54/4] &&
            !Reg[0x70/4] && !Reg[0x74/4]);
      Saved=Writes; Reg[Roots[I]/4]=1;
      CHECK(MtkUfsTransportPollStop(&T)==MtkUfsIoUnsafeDma && Writes==Saved &&
            T.Attached && T.StopInProgress && T.StopPhase==2);
      Reg[Roots[I]/4]=0;
      CHECK(MtkUfsTransportPollStop(&T)==MtkUfsIoSuccess && !T.Attached &&
            !T.StopInProgress && !T.StopPhase);
    }
  }
  TestAllCommandOwnership();
  TestPrivatePowerTransport();
  printf("MtkUfsTransportTest: %u checks passed (ASan/UBSan, fake MMIO and DMA)\n",Checks);
  return 0;
}
