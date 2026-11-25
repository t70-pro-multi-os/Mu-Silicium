/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MtkUfsTransport.h>
#include <Library/MtkUfsCore.h>

#define UFS_HC_IS_UCCS             (1U << 10)
#define UFS_HC_HCS_UCRDY           (1U << 3)
#define UFS_HC_UIC_HIBERN8_ENTER   0x17U
#define UFS_HC_UIC_CMD_OFFSET      0x90U
#define UFS_HC_UIC_ARG2_OFFSET     0x98U
#define UFS_T70_IP_VERSION_OFFSET  0x2240U
#define UFS_T70_DEBUG_SEL_OFFSET   0x22C0U
#define UFS_T70_LINK_PROBE_OFFSET  0x22C8U
#define UFS_T70_DEBUG_B0_OFFSET    0x22D0U

static uint32_t R(MTK_UFS_TRANSPORT *T, uint32_t O) { return T->Io.Read32(T->Io.Context,O); }
static void W(MTK_UFS_TRANSPORT *T, uint32_t O, uint32_t V) { T->Io.Write32(T->Io.Context,O,V); }
static void Fence(MTK_UFS_TRANSPORT *T) { T->Io.Fence(T->Io.Context); }
static void Zero(volatile uint8_t *P, size_t N) { while(N--) *P++=0; }
static void Le32(volatile uint8_t *P, uint32_t V) {
  P[0]=(uint8_t)V; P[1]=(uint8_t)(V>>8); P[2]=(uint8_t)(V>>16); P[3]=(uint8_t)(V>>24);
}
static void Be32(volatile uint8_t *P, uint32_t V) {
  P[0]=(uint8_t)(V>>24); P[1]=(uint8_t)(V>>16); P[2]=(uint8_t)(V>>8); P[3]=(uint8_t)V;
}
static uint32_t ReadLe32(const volatile uint8_t *P) {
  return (uint32_t)P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);
}
static uint32_t B32(const uint8_t *P) {
  return ((uint32_t)P[0]<<24)|((uint32_t)P[1]<<16)|((uint32_t)P[2]<<8)|P[3];
}
static uint64_t B64(const uint8_t *P) { return ((uint64_t)B32(P)<<32)|B32(P+4); }

int MtkUfsTransportReadCdb(const uint8_t *C, size_t N, uint32_t Bytes) {
  uint64_t Lba=0, Blocks=0, MaxBlocks=MTK_UFS_USER_BYTES/MTK_UFS_BLOCK_BYTES;
  if (!C || N<6 || N>16 || Bytes>MTK_UFS_DATA_BYTES) return 0;
  /* Restrict control bits (including LINK/NACA), reserved flags and allocations,
   * so a data-in flag cannot hide a device-changing/vendor operation. */
  switch(C[0]) {
  case 0x00: return N==6 && !C[1] && !C[2] && !C[3] && !C[4] && !C[5] && !Bytes;
  case 0x03: return N==6 && !(C[1]&~1U) && !C[2] && !C[3] && !C[5] && C[4]==Bytes;
  case 0x12: return N==6 && !(C[1]&~1U) && ((C[1]&1U)||!C[2]) && !C[5] &&
                    (((uint32_t)C[3]<<8)|C[4])==Bytes;
  case 0x1a: return N==6 && !(C[1]&~8U) && !(C[2]&0xc0U) && !C[3] && !C[5] && C[4]==Bytes;
  case 0x5a: return N==10 && !(C[1]&~0x18U) && !(C[2]&0xc0U) && !C[3] && !C[4] && !C[5] &&
                    !C[6] && !C[9] && (((uint32_t)C[7]<<8)|C[8])==Bytes;
  case 0x25: return N==10 && !C[1] && !B32(C+2) && !C[6] && !C[7] && !C[8] && !C[9] && Bytes==8;
  case 0x9e: return N==16 && C[1]==0x10 && !B64(C+2) && !C[14] && !C[15] &&
                    B32(C+10)==Bytes && Bytes>=32;
  /* READ(6/10/12/16); reject protection/FUA/group/control extensions. */
  case 0x08:
    if(N!=6 || C[1]&0xe0U || C[5]) return 0;
    Lba=((uint32_t)(C[1]&0x1fU)<<16)|((uint32_t)C[2]<<8)|C[3]; Blocks=C[4]?C[4]:256; break;
  case 0x28:
    if(N!=10 || (C[1]&~0x18U) || C[6] || C[9]) return 0;
    Lba=B32(C+2); Blocks=((uint32_t)C[7]<<8)|C[8]; break;
  case 0xa8:
    if(N!=12 || (C[1]&~0x18U) || C[10] || C[11]) return 0;
    Lba=B32(C+2); Blocks=B32(C+6); break;
  case 0x88:
    if(N!=16 || (C[1]&~0x18U) || C[14] || C[15]) return 0;
    Lba=B64(C+2); Blocks=B32(C+10); break;
  default: return 0;
  }
  return Lba<MaxBlocks && Blocks<=MaxBlocks-Lba && Blocks<=MTK_UFS_DATA_BYTES/MTK_UFS_BLOCK_BYTES &&
         Blocks*MTK_UFS_BLOCK_BYTES==Bytes;
}

int MtkUfsTransportWriteCdb(const uint8_t *C, size_t N, uint32_t Bytes) {
  uint8_t Read[16];
  uint64_t Lba,Blocks,Max=MTK_UFS_USER_BYTES/MTK_UFS_BLOCK_BYTES;
  size_t I;
  if(!C || N<6 || N>16) return 0;
  for(I=0;I<N;I++) Read[I]=C[I];
  switch(C[0]) {
  case 0x0a: Read[0]=0x08; break;
  case 0x2a: Read[0]=0x28; break;
  case 0xaa: Read[0]=0xa8; break;
  case 0x8a: Read[0]=0x88; break;
  case 0x35:
    if(N!=10 || Bytes || C[1] || C[6] || C[9]) return 0;
    Lba=B32(C+2); Blocks=((uint32_t)C[7]<<8)|C[8];
    return Lba<Max && Blocks<=Max-Lba;
  case 0x91:
    if(N!=16 || Bytes || C[1] || C[14] || C[15]) return 0;
    Lba=B64(C+2); Blocks=B32(C+10);
    return Lba<Max && Blocks<=Max-Lba;
  default: return 0;
  }
  return MtkUfsTransportReadCdb(Read,N,Bytes);
}

MTK_UFS_IO_STATUS MtkUfsTransportAttach(MTK_UFS_TRANSPORT *T,
  const MTK_UFS_TRANSPORT_IO *Io, volatile void *Dma, size_t DmaBytes, uint64_t Physical) {
  MTK_UFS_TRANSPORT C;
  uint32_t Cap,Version,Hcs;
  uint64_t Limit;
  if(!T || !Io || !Io->Read32 || !Io->Write32 || !Io->Fence || !Io->StallUs || !Dma ||
     DmaBytes<MTK_UFS_DMA_BYTES || (Physical&1023U) || ((uintptr_t)Dma&7U)) return MtkUfsIoInvalid;
  /* Construct a local candidate; never overwrite an already-owned transport. */
  if(T->Attached || T->Active || T->Faulted) return MtkUfsIoBusy;
  C.Io=*Io; C.Dma=Dma; C.Physical=Physical;
  C.UicPending=0; C.Hibernate8CommandDone=0;
  Cap=R(&C,0); Version=R(&C,8);
  Limit=(Cap&(1U<<24))?0x1000000000ULL:0x100000000ULL;
  if(Cap==UINT32_MAX || (Version!=0x200U && Version!=0x210U) ||
     Physical>=Limit || MTK_UFS_DMA_BYTES>Limit-Physical) return MtkUfsIoInvalid;
  Hcs=R(&C,0x30);
  if(Hcs==UINT32_MAX || R(&C,0x34)!=1U || (Hcs&0xfU)!=0xfU || R(&C,0x58) || R(&C,0x78) ||
     R(&C,0x60) || R(&C,0x80) || R(&C,0x24) || R(&C,0x50) || R(&C,0x54) ||
     R(&C,0x70) || R(&C,0x74) || R(&C,0x18) || (R(&C,0x20)&MTK_UFS_ERROR_INTERRUPTS)) return MtkUfsIoNotReady;
  C.ExpectedBytes=0; C.NextTag=1; C.ActiveTag=0; C.ActiveLun=0;
  C.Attached=1; C.Active=0; C.Nop=0; C.QueryOpcode=0; C.Faulted=0; *T=C;
  T->StopInProgress=0; T->StopPhase=0;
  Zero(T->Dma,MTK_UFS_DMA_BYTES);
  W(T,0x50,(uint32_t)Physical); W(T,0x54,(uint32_t)(Physical>>32));
  W(T,0x70,(uint32_t)(Physical+0x400)); W(T,0x74,(uint32_t)((Physical+0x400)>>32));
  /* Disable interrupt aggregation: this driver owns one slot only. */
  W(T,0x4c,0); W(T,0x20,MTK_UFS_INTERRUPTS); Fence(T);
  W(T,0x60,1); Fence(T);
  if(R(T,0x60)!=1U) { T->Faulted=1; return MtkUfsIoError; }
  return MtkUfsIoSuccess;
}

/* Every command shares this driver's already-owned single slot. Revalidate
 * the live host/DMA layout before submission and before trusting completion;
 * an old Attached flag cannot authorize a foreign descriptor list. */
static int HostOwned(MTK_UFS_TRANSPORT *T) {
  uint32_t Hcs=R(T,0x30),Ie=R(T,0x24);
  return Hcs!=UINT32_MAX && (Hcs&0xbU)==0xbU && R(T,0x34)==1U &&
    R(T,0x60)==1U && R(T,0x80)==0U && R(T,0x78)==0U &&
    (Ie==0U || Ie==MTK_UFS_INTERRUPTS) &&
    R(T,0x50)==(uint32_t)T->Physical && R(T,0x54)==(uint32_t)(T->Physical>>32) &&
    R(T,0x70)==(uint32_t)(T->Physical+0x400U) &&
    R(T,0x74)==(uint32_t)((T->Physical+0x400U)>>32);
}

static MTK_UFS_IO_STATUS Submit(MTK_UFS_TRANSPORT *T,const uint8_t *Cdb,size_t N,uint32_t Bytes,int Nop,int Write,const void *Data,uint8_t Lun,int DevicePower,uint8_t QueryOpcode) {
  volatile uint8_t *D,*C;
  uint32_t DmaBytes;
  uint64_t A;
  size_t I;
  if(!T || !T->Attached || T->Faulted) return MtkUfsIoNotReady;
  if(T->Active || T->UicPending || T->Hibernate8CommandDone) return MtkUfsIoBusy;
  if(QueryOpcode) {
    if((QueryOpcode!=5U && QueryOpcode!=7U) || Nop || Write || Cdb || N || Bytes || Lun || DevicePower)
      return MtkUfsIoInvalid;
  } else if(DevicePower) {
    if((DevicePower!=1 && DevicePower!=2) || Nop || Write || Lun!=MTK_UFS_DEVICE_WLUN || !Cdb || N!=6 || Bytes ||
       Cdb[0]!=0x1BU || Cdb[1] || Cdb[2] || Cdb[3] ||
       Cdb[4]!=(DevicePower==1?0x30U:0x10U) || Cdb[5])
      return MtkUfsIoInvalid;
  } else if(!Nop && ((!Write && !MtkUfsTransportReadCdb(Cdb,N,Bytes)) ||
                    (Write && (!MtkUfsTransportWriteCdb(Cdb,N,Bytes) || (Bytes && !Data))))) return MtkUfsIoInvalid;
  if(R(T,0x58) || R(T,0x78) || R(T,0x60)!=1U || R(T,0x34)!=1U ||
     (R(T,0x30)&0xbU)!=0xbU) { T->Faulted=1; return MtkUfsIoError; }
  if(!HostOwned(T)) { T->Faulted=1; return MtkUfsIoError; }
  D=T->Dma; C=D+MTK_UFS_COMMAND_OFFSET;
  DmaBytes=(Bytes+3U)&~3U;
  Zero(D,32); Zero(C,0x70); Zero(D+MTK_UFS_DATA_OFFSET,DmaBytes);
  if(Write && Bytes) {
    const uint8_t *Source=Data;
    for(I=0;I<Bytes;I++) D[MTK_UFS_DATA_OFFSET+I]=Source[I];
  }
  T->ActiveTag=T->NextTag++; T->ActiveLun=(Nop || QueryOpcode)?0:Lun;
  T->ExpectedBytes=Bytes; T->Nop=(uint8_t)Nop; T->QueryOpcode=QueryOpcode;
  C[0]=QueryOpcode?0x16U:Nop?0:1; C[1]=Bytes?(Write?0x20:0x40):0; C[2]=T->ActiveLun; C[3]=T->ActiveTag;
  if(QueryOpcode) {
    C[5]=QueryOpcode==5U?1U:0x81U; C[12]=QueryOpcode; C[13]=4U;
  } else if(!Nop) { Be32(C+12,Bytes); for(I=0;I<N;I++) C[16+I]=Cdb[I]; }
  Le32(D,0x11000000U|(Bytes?(Write?0x02000000U:0x04000000U):0)); Le32(D+8,0xf);
  A=T->Physical+MTK_UFS_COMMAND_OFFSET; Le32(D+16,(uint32_t)A); Le32(D+20,(uint32_t)(A>>32));
  Le32(D+24,(8U<<16)|16U); Le32(D+28,(24U<<16)|(Bytes?1U:0));
  if(Bytes) {
    A=T->Physical+MTK_UFS_DATA_OFFSET;
    Le32(D+MTK_UFS_PRD_OFFSET,(uint32_t)A); Le32(D+MTK_UFS_PRD_OFFSET+4,(uint32_t)(A>>32));
    Le32(D+MTK_UFS_PRD_OFFSET+12,DmaBytes-1U);
  }
  if(R(T,0x20)&MTK_UFS_ERROR_INTERRUPTS) { T->Faulted=1; return MtkUfsIoError; }
  W(T,0x20,1); Fence(T);
  T->Active=1;
  W(T,0x58,1); Fence(T);
  return MtkUfsIoSuccess;
}
MTK_UFS_IO_STATUS MtkUfsTransportNop(MTK_UFS_TRANSPORT *T) { return Submit(T,0,0,0,1,0,0,0,0,0); }
MTK_UFS_IO_STATUS MtkUfsTransportScsi(MTK_UFS_TRANSPORT *T,const uint8_t *C,size_t N,uint32_t B) {
  return Submit(T,C,N,B,0,0,0,MTK_UFS_USER_LUN,0,0);
}
MTK_UFS_IO_STATUS MtkUfsTransportWriteScsi(MTK_UFS_TRANSPORT *T,const uint8_t *C,size_t N,uint32_t B,const void *Data) {
  return Submit(T,C,N,B,0,1,Data,MTK_UFS_USER_LUN,0,0);
}
MTK_UFS_IO_STATUS MtkUfsTransportPoll(MTK_UFS_TRANSPORT *T, MTK_UFS_IO_RESULT *Result) {
  MTK_UFS_RESPONSE_VIEW View;
  MTK_UFS_IO_RESULT Out={0};
  uint8_t Response[64];
  uint32_t Is,Door;
  size_t I;
  if(!T || !Result || !T->Attached || T->Faulted) return MtkUfsIoNotReady;
  if(T->UicPending || T->Hibernate8CommandDone) return MtkUfsIoBusy;
  if(!HostOwned(T)) goto Fault;
  Is=R(T,0x20); Door=R(T,0x58);
  if(Is==UINT32_MAX || Door==UINT32_MAX || (Is&MTK_UFS_ERROR_INTERRUPTS) || (Door&~1U)) goto Fault;
  if(!T->Active) return MtkUfsIoNotReady;
  if(Door&1U) return MtkUfsIoPending;
  Fence(T);
  if(T->Dma[8]!=0) goto Fault;
  for(I=0;I<sizeof(Response);I++) Response[I]=T->Dma[MTK_UFS_RESPONSE_OFFSET+I];
  if(T->QueryOpcode) {
    uint8_t Function=T->QueryOpcode==5U?1U:0x81U;
    uint64_t Command=T->Physical+MTK_UFS_COMMAND_OFFSET;
    if(T->Nop || T->ExpectedBytes || T->ActiveLun ||
       ReadLe32(T->Dma)!=0x11000000U || ReadLe32(T->Dma+8) ||
       ReadLe32(T->Dma+16)!=(uint32_t)Command || ReadLe32(T->Dma+20)!=(uint32_t)(Command>>32) ||
       ReadLe32(T->Dma+24)!=0x00080010U || ReadLe32(T->Dma+28)!=0x00180000U ||
       Response[0]!=0x36U || Response[1] || Response[2] || Response[3]!=T->ActiveTag ||
       Response[4] || Response[5]!=Function || Response[6] || Response[7] || Response[8] ||
       (Response[9]&~1U) || Response[10] || Response[11] ||
       Response[12]!=T->QueryOpcode || Response[13]!=4U || Response[14] || Response[15] ||
       Response[16] || Response[17] || Response[18] || Response[19] ||
       Response[20] || Response[21] || Response[22] || Response[23]>1U ||
       (T->QueryOpcode==7U && Response[23])) goto Fault;
    for(I=24;I<32;I++) if(Response[I]) goto Fault;
    Out.QueryFlagValid=1; Out.QueryFlagValue=Response[23];
  } else if(T->Nop) {
    if(Response[0]!=0x20 || Response[1] || Response[2] || Response[3]!=T->ActiveTag ||
       Response[4] || Response[5] || Response[6] || Response[7] || Response[8] ||
       Response[9] || Response[10] || Response[11]) goto Fault;
  } else {
    if(Response[0]!=0x21 || Response[8] || Response[4] || Response[5] ||
       MtkUfsParseResponseUpiu(Response,sizeof(Response),&View)!=MtkUfsCoreSuccess ||
       View.Lun!=T->ActiveLun || View.TaskTag!=T->ActiveTag || View.Response ||
       (View.Flags&~0x20U) || View.ResidualTransferCount>T->ExpectedBytes ||
       (!(View.Flags&0x20U) && View.ResidualTransferCount) ||
       ((View.Flags&0x20U) && !View.ResidualTransferCount) ||
       View.SenseDataLength>sizeof(Out.Sense)) goto Fault;
    Out.ScsiStatus=View.ScsiStatus; Out.Transferred=T->ExpectedBytes-View.ResidualTransferCount;
    Out.SenseLength=(uint8_t)View.SenseDataLength;
    for(I=0;I<View.SenseDataLength;I++) Out.Sense[I]=View.SenseData[I];
  }
  W(T,0x20,Is&MTK_UFS_INTERRUPTS); Fence(T);
  T->Active=0; *Result=Out;
  return MtkUfsIoSuccess;
Fault:
  T->Faulted=1;
  return MtkUfsIoError;
}

MTK_UFS_IO_STATUS MtkUfsTransportReadAutoBkops(MTK_UFS_TRANSPORT *T) {
  return Submit(T,0,0,0,0,0,0,0,0,5U);
}
MTK_UFS_IO_STATUS MtkUfsTransportDisableAutoBkops(MTK_UFS_TRANSPORT *T) {
  return Submit(T,0,0,0,0,0,0,0,0,7U);
}
MTK_UFS_IO_STATUS MtkUfsTransportPowerActive(MTK_UFS_TRANSPORT *T) {
  static const uint8_t Cdb[6]={0x1B,0,0,0,0x10,0};
  return Submit(T,Cdb,sizeof(Cdb),0,0,0,0,MTK_UFS_DEVICE_WLUN,2,0);
}
MTK_UFS_IO_STATUS MtkUfsTransportPowerDown(MTK_UFS_TRANSPORT *T) {
  static const uint8_t Cdb[6]={0x1B,0,0,0,0x30,0};
  return Submit(T,Cdb,sizeof(Cdb),0,0,0,0,MTK_UFS_DEVICE_WLUN,1,0);
}

MTK_UFS_IO_STATUS MtkUfsTransportBeginHibernate8(MTK_UFS_TRANSPORT *T) {
  uint32_t Is,Hcs,Ie;
  if(!T || !T->Attached || T->Faulted) return MtkUfsIoNotReady;
  if(T->Active || T->UicPending || T->Hibernate8CommandDone) return MtkUfsIoBusy;
  Is=R(T,0x20); Hcs=R(T,0x30); Ie=R(T,0x24);
  if(Is==UINT32_MAX || Hcs==UINT32_MAX || Ie==UINT32_MAX || (Is&MTK_UFS_ERROR_INTERRUPTS)) goto Fault;
  /* The DPC owner may already have masked IE while handling the command reply. */
  if(Ie!=0U && Ie!=MTK_UFS_INTERRUPTS) goto Fault;
  if((Hcs&UFS_HC_HCS_UCRDY)==0) return MtkUfsIoNotReady;
  if(R(T,0x34)!=1U || R(T,0x60)!=1U || R(T,0x80)!=0U ||
     R(T,0x58)!=0U || R(T,0x78)!=0U || R(T,0x50)!=(uint32_t)T->Physical ||
     R(T,0x54)!=(uint32_t)(T->Physical>>32) || R(T,0x70)!=(uint32_t)(T->Physical+0x400U) ||
     R(T,0x74)!=(uint32_t)((T->Physical+0x400U)>>32)) goto Fault;
  if(Is&UFS_HC_IS_UCCS) {
    W(T,0x20,UFS_HC_IS_UCCS); Fence(T);
    if(R(T,0x20)&UFS_HC_IS_UCCS) goto Fault;
  }
  W(T,0x94,0); W(T,UFS_HC_UIC_ARG2_OFFSET,0); W(T,0x9C,0); Fence(T);
  W(T,UFS_HC_UIC_CMD_OFFSET,UFS_HC_UIC_HIBERN8_ENTER); Fence(T);
  T->UicPending=1; T->Hibernate8CommandDone=0;
  return MtkUfsIoSuccess;
Fault:
  T->Faulted=1;
  return MtkUfsIoError;
}

static MTK_UFS_IO_STATUS SelectT70LinkProbe(MTK_UFS_TRANSPORT *T) {
  uint32_t Raw,Version,Ie;
  if(R(T,0x34)!=1U || R(T,0x60)!=1U || R(T,0x80)!=0U ||
     R(T,0x58)!=0U || R(T,0x78)!=0U) return MtkUfsIoError;
  Ie=R(T,0x24);
  if(Ie==UINT32_MAX || (Ie!=0U && Ie!=MTK_UFS_INTERRUPTS)) return MtkUfsIoError;
  Raw=R(T,UFS_T70_IP_VERSION_OFFSET);
  if(Raw==0 || Raw==UINT32_MAX) return MtkUfsIoError;
  Version=(((Raw|0x01000000U)>>24)==1U)?((Raw&0x00FFFFFFU)|0x10000000U):Raw;
  if(((Version>>16)&0xFFU)>=0x36U) {
    W(T,UFS_T70_DEBUG_SEL_OFFSET,0x00820820U);
    W(T,UFS_T70_DEBUG_B0_OFFSET,0U);
    W(T,UFS_T70_DEBUG_B0_OFFSET+4U,0x55555555U);
    W(T,UFS_T70_DEBUG_B0_OFFSET+8U,0xAAAAAAAAU);
    W(T,UFS_T70_DEBUG_B0_OFFSET+12U,UINT32_MAX);
  } else {
    W(T,UFS_T70_DEBUG_SEL_OFFSET,0x20U);
  }
  Fence(T);
  Raw=R(T,UFS_T70_LINK_PROBE_OFFSET);
  if(Raw==UINT32_MAX) return MtkUfsIoError;
  return ((Raw>>28)==3U)?MtkUfsIoSuccess:MtkUfsIoPending;
}

MTK_UFS_IO_STATUS MtkUfsTransportPollHibernate8(MTK_UFS_TRANSPORT *T) {
  uint32_t Is,Arg2;
  if(!T || !T->Attached || T->Faulted) return MtkUfsIoNotReady;
  if(!T->UicPending && !T->Hibernate8CommandDone) return MtkUfsIoNotReady;
  if(R(T,0x34)!=1U || R(T,0x60)!=1U || R(T,0x80)!=0U || R(T,0x58)!=0U || R(T,0x78)!=0U)
    goto Fault;
  if(T->UicPending) {
    Is=R(T,0x20);
    if(Is==UINT32_MAX || (Is&MTK_UFS_ERROR_INTERRUPTS)) goto Fault;
    if((Is&UFS_HC_IS_UCCS)==0) return MtkUfsIoPending;
    Arg2=R(T,UFS_HC_UIC_ARG2_OFFSET);
    if(Arg2==UINT32_MAX || (Arg2&0xFFU)!=0) goto Fault;
    W(T,0x20,UFS_HC_IS_UCCS); Fence(T);
    if(R(T,0x20)&UFS_HC_IS_UCCS) goto Fault;
    T->UicPending=0; T->Hibernate8CommandDone=1;
  }
  switch(SelectT70LinkProbe(T)) {
    case MtkUfsIoSuccess:
      T->Hibernate8CommandDone=0;
      return MtkUfsIoSuccess;
    case MtkUfsIoPending:
      return MtkUfsIoPending;
    default:
      break;
  }
Fault:
  T->Faulted=1;
  return MtkUfsIoError;
}

MTK_UFS_IO_STATUS MtkUfsTransportBeginStop(MTK_UFS_TRANSPORT *T) {
  if(!T || !T->Attached) return MtkUfsIoNotReady;
  T->Faulted=1;
  if(!T->StopInProgress) {
    T->StopInProgress=1; T->StopPhase=1;
    W(T,0x24,0); W(T,0x60,0); W(T,0x80,0); W(T,0x34,0); Fence(T);
  }
  return MtkUfsTransportPollStop(T);
}

MTK_UFS_IO_STATUS MtkUfsTransportPollStop(MTK_UFS_TRANSPORT *T) {
  uint32_t Ie,Run,Door,Hce;
  if(!T || !T->Attached || !T->StopInProgress) return MtkUfsIoNotReady;
  Ie=R(T,0x24); Run=R(T,0x60)|R(T,0x80); Door=R(T,0x58)|R(T,0x78); Hce=R(T,0x34);
  if(Ie==UINT32_MAX || Run==UINT32_MAX || Door==UINT32_MAX || Hce==UINT32_MAX)
    return MtkUfsIoUnsafeDma;
  if(T->StopPhase==1) {
    if(Hce || Ie || Run || Door) {
      W(T,0x24,0); W(T,0x60,0); W(T,0x80,0); W(T,0x34,0); Fence(T);
      return MtkUfsIoPending;
    }
    W(T,0x50,0); W(T,0x54,0); W(T,0x70,0); W(T,0x74,0); Fence(T);
    T->StopPhase=2;
    return MtkUfsIoPending;
  }
  if(T->StopPhase==2) {
    /* Re-prove all DMA-stopping roots before touching or accepting bases. */
    if(Hce || Ie || Run || Door) return MtkUfsIoUnsafeDma;
    if(R(T,0x50)==UINT32_MAX || R(T,0x54)==UINT32_MAX ||
       R(T,0x70)==UINT32_MAX || R(T,0x74)==UINT32_MAX) return MtkUfsIoUnsafeDma;
    if(R(T,0x50) || R(T,0x54) || R(T,0x70) || R(T,0x74)) {
      W(T,0x50,0); W(T,0x54,0); W(T,0x70,0); W(T,0x74,0); Fence(T);
      return MtkUfsIoPending;
    }
    T->UicPending=0; T->Hibernate8CommandDone=0; T->StopInProgress=0; T->StopPhase=0;
    T->Attached=0; T->Active=0; return MtkUfsIoSuccess;
  }
  return MtkUfsIoUnsafeDma;
}

MTK_UFS_IO_STATUS MtkUfsTransportStop(MTK_UFS_TRANSPORT *T) {
  unsigned I;
  MTK_UFS_IO_STATUS Status;
  Status=MtkUfsTransportBeginStop(T);
  if(Status!=MtkUfsIoPending) return Status;
  for(I=0;I<=100;I++) {
    Status=MtkUfsTransportPollStop(T);
    if(Status!=MtkUfsIoPending) return Status;
    if(I<100) T->Io.StallUs(T->Io.Context,10);
  }
  return MtkUfsIoUnsafeDma;
}

MTK_UFS_IO_STATUS MtkUfsTransportDetach(MTK_UFS_TRANSPORT *T) {
  if(!T || !T->Attached || T->Faulted) return MtkUfsIoNotReady;
  if(T->Active || T->UicPending || T->Hibernate8CommandDone || R(T,0x58) || R(T,0x78)) return MtkUfsIoBusy;
  W(T,0x24,0); W(T,0x60,0); W(T,0x80,0); Fence(T);
  if(R(T,0x60) || R(T,0x80) || R(T,0x58) || R(T,0x78) || R(T,0x24) ||
     R(T,0x34)!=1 || (R(T,0x30)&0xfU)!=0xfU || R(T,0x30)==UINT32_MAX ||
     (R(T,0x20)&MTK_UFS_ERROR_INTERRUPTS)) { T->Faulted=1; return MtkUfsIoError; }
  W(T,0x50,0); W(T,0x54,0); W(T,0x70,0); W(T,0x74,0); W(T,0x20,MTK_UFS_INTERRUPTS); Fence(T);
  if(R(T,0x50) || R(T,0x54) || R(T,0x70) || R(T,0x74)) { T->Faulted=1; return MtkUfsIoError; }
  T->Attached=0;
  return MtkUfsIoSuccess;
}
