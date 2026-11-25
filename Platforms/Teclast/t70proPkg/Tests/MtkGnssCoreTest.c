// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkGnssCore.h>
#include <Library/MtkStpCore.h>
#ifdef STP_TEST_NATIVE
#include <stdio.h>
#endif
unsigned MtkGnssAssertionCount;
#define CHECK(X) do { ++MtkGnssAssertionCount; if (!(X)) return __LINE__; } while (0)
static MTK_GNSS_DECODER D;
static MTK_GNSS_FRAME F;
static unsigned char Joined[2U*MTK_GNSS_WIRE_MAX];
static unsigned char Body[MTK_GNSS_PAYLOAD_MAX],Wire[MTK_GNSS_WIRE_MAX+16],Original[MTK_GNSS_WIRE_MAX+16];
static unsigned Equal(const unsigned char *A,const unsigned char *B,unsigned N) {
  unsigned I; for (I=0;I<N;I++) if (A[I]!=B[I]) return 0; return 1;
}
static int KnownTest(void) {
  const unsigned char Input[]={1,0xaa,0xde};
  const unsigned char Expected[]={0xaa,0xf0,7,0,0x34,0x12,1,0xde,0xdf,0xde,0xe0,0xd6,1,0xaa,0x0f};
  const unsigned char Raw[]={0xaa,0xf0,7,0,0x34,0x12,1,0xaa,0xde,0xd6,1,0xaa,0x0f};
  unsigned N,C,I;
  CHECK(MtkGnssEncode(0x1234,Input,3,Wire,sizeof(Wire),&N)==MtkGnssOk);
  CHECK(N==sizeof(Expected) && Equal(Wire,Expected,N));
  for (I=0;I<=N;I++) {
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk); F.Length=0xfeed;
    CHECK(MtkGnssReceive(&D,Wire,I,&C,&F)==(I==N?MtkGnssFrameReady:MtkGnssNeedMore) && C==I);
    if (I<N) {
      CHECK(F.Length==0xfeed);
      CHECK(MtkGnssReceive(&D,Wire+I,N-I,&C,&F)==MtkGnssFrameReady && C==N-I);
    }
    CHECK(F.Type==0x1234 && !F.Flags && F.Length==3 && F.RawLength==sizeof(Raw));
    CHECK(Equal(F.Data,Input,3) && Equal(F.Raw,Raw,sizeof(Raw)));
    CHECK(MtkGnssReceive(&D,0,0,&C,&F)==MtkGnssNeedMore && !C);
    for (C=0;C<sizeof(D.Buffer);C++) CHECK(!D.Buffer[C]);
  }
  CHECK(MtkGnssEncode(0,0,0,Wire,sizeof(Wire),&N)==MtkGnssOk && N==10);
  CHECK(MtkGnssReceive(&D,Wire,N,&C,&F)==MtkGnssFrameReady && !F.Length && !F.Type);
  return 0;
}
static int StreamingTest(void) {
  unsigned L,Pattern,I,N,C,At,Type,Flags,Sum,Written;
  MTK_GNSS_STATUS R;
  for (Pattern=0;Pattern<3;Pattern++) for (L=0;L<=MTK_GNSS_PAYLOAD_MAX;L++) {
    Type=Pattern==0?0xfe2fU:Pattern==1?0xaadeU:0xffffU;
    for (I=0;I<L;I++) Body[I]=(unsigned char)(Pattern==0?I:Pattern==1?(I&1?0xaa:0xde):(I*37U+L));
    CHECK(MtkGnssEncode(Type,Body,L,Wire,sizeof(Wire),&N)==MtkGnssOk);
    CHECK(N<=MTK_GNSS_WIRE_MAX && N>=L+10);
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk);
    for (I=0;I<N;I++) {
      R=MtkGnssReceive(&D,Wire+I,1,&C,&F);
      CHECK(C==1 && R==(I+1==N?MtkGnssFrameReady:MtkGnssNeedMore));
    }
    CHECK(F.Type==Type && !F.Flags && F.Length==L && F.RawLength==L+10);
    CHECK(Equal(F.Data,Body,L));
    /* Two concatenated frames must leave the second one's bytes untouched. */
    for (I=0;I<N;I++) Joined[I]=Joined[N+I]=Wire[I];
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk);
    CHECK(MtkGnssReceive(&D,Joined,2U*N,&C,&F)==MtkGnssFrameReady && C==N);
    CHECK(MtkGnssReceive(&D,Joined+C,2U*N-C,&C,&F)==MtkGnssFrameReady && C==N && Equal(F.Data,Body,L));
    for (I=0;I<N;I++) Original[I]=Wire[I];
    for (I=0;I<sizeof(Wire);I++) Wire[I]=0x5a;
    CHECK(MtkGnssEncode(Type,Body,L,Wire,N-1,&Written)==MtkGnssCapacity && !Written);
    for (I=0;I<sizeof(Wire);I++) CHECK(Wire[I]==0x5a);
  }
  /* Preserve the opaque high length nibble and include it in the checksum. */
  for (Flags=0;Flags<16;Flags++) {
    unsigned char Raw[]={0xaa,0xf0,4,0,0,0,0,0,0xaa,0x0f};
    Raw[3]=(unsigned char)(Flags<<4); Sum=4+Raw[3]; Raw[6]=(unsigned char)Sum; Raw[7]=(unsigned char)(Sum>>8);
    At=2; Wire[0]=0xaa; Wire[1]=0xf0;
    for (I=2;I<8;I++) {
      if (Raw[I]==0xaa || Raw[I]==0xde) { Wire[At++]=0xde; Wire[At++]=Raw[I]==0xaa?0xdf:0xe0; }
      else Wire[At++]=Raw[I];
    }
    Wire[At++]=0xaa; Wire[At++]=0x0f;
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk);
    CHECK(MtkGnssReceive(&D,Wire,At,&C,&F)==MtkGnssFrameReady && C==At && F.Flags==Flags);
  }
  return 0;
}
static int BadTest(void) {
  const unsigned char Good[]={0xaa,0xf0,4,0,0,0,4,0,0xaa,0x0f};
  const unsigned char TooLong[]={0xaa,0xf0,0xff,1};
  const unsigned char TooShort[]={0xaa,0xf0,3,0};
  const unsigned char BadEscape[]={0xaa,0xf0,4,0,0xde,1};
  const unsigned char EarlyEnd[]={0xaa,0xf0,4,0,0xaa,0x0f};
  const unsigned char Restart[]={0xaa,0xf0,4,0,0xaa,0xf0};
  const unsigned char Extra[]={0xaa,0xf0,4,0,0,0,4,0,7};
  const unsigned char *Bad[]={TooLong,TooShort,BadEscape,EarlyEnd,Restart,Extra};
  const unsigned Lengths[]={sizeof(TooLong),sizeof(TooShort),sizeof(BadEscape),sizeof(EarlyEnd),sizeof(Restart),sizeof(Extra)};
  unsigned I,J,C,N;
  for (I=0;I<sizeof(Bad)/sizeof(Bad[0])+sizeof(Good)-2U;I++) {
    const unsigned char *P; unsigned L;
    if (I<sizeof(Bad)/sizeof(Bad[0])) { P=Bad[I]; L=Lengths[I]; }
    else {
      for (J=0;J<sizeof(Good);J++) Wire[J]=Good[J];
      Wire[2+I-sizeof(Bad)/sizeof(Bad[0])]^=1; P=Wire; L=sizeof(Good);
    }
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk); F.Length=0xfeed;
    CHECK(MtkGnssReceive(&D,P,L,&C,&F)==MtkGnssMalformed && C>0 && C<=L && F.Length==0xfeed);
    CHECK(D.Faulted && !D.Used);
    for (J=0;J<sizeof(D.Buffer);J++) CHECK(!D.Buffer[J]);
    CHECK(MtkGnssReceive(&D,Good,sizeof(Good),&C,&F)==MtkGnssFaulted && !C);
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk);
    CHECK(MtkGnssReceive(&D,Good,sizeof(Good),&C,&F)==MtkGnssFrameReady);
  }
  CHECK(MtkGnssDecoderInit(0)==MtkGnssInvalid);
  CHECK(MtkGnssReceive(0,Good,sizeof(Good),&C,&F)==MtkGnssInvalid && !C);
  CHECK(MtkGnssReceive(&D,0,1,&C,&F)==MtkGnssInvalid && !C);
  CHECK(MtkGnssReceive(&D,Good,1,0,&F)==MtkGnssInvalid);
  CHECK(MtkGnssReceive(&D,Good,1,&C,0)==MtkGnssInvalid && !C);
  CHECK(MtkGnssEncode(0x10000,Body,0,Wire,sizeof(Wire),&N)==MtkGnssInvalid && !N);
  CHECK(MtkGnssEncode(0,Body,MTK_GNSS_PAYLOAD_MAX+1,Wire,sizeof(Wire),&N)==MtkGnssInvalid && !N);
  CHECK(MtkGnssEncode(0,0,1,Wire,sizeof(Wire),&N)==MtkGnssInvalid && !N);
  CHECK(MtkGnssEncode(0,Body,1,Wire,sizeof(Wire),0)==MtkGnssInvalid);
  CHECK(MtkGnssEncode(0,Wire,1,Wire,sizeof(Wire),&N)==MtkGnssInvalid && !N);
  CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk);
  /* Ignored leading noise and a split sync are not a received frame. */
  for (I=0;I<256;I++) if (I!=0xaa) {
    Wire[0]=(unsigned char)I; CHECK(MtkGnssReceive(&D,Wire,1,&C,&F)==MtkGnssNeedMore && C==1);
  }
  CHECK(MtkGnssReceive(&D,Good,1,&C,&F)==MtkGnssNeedMore);
  CHECK(MtkGnssReceive(&D,Good,sizeof(Good),&C,&F)==MtkGnssFrameReady);
  return 0;
}
static int StpIntegrationTest(void) {
  static MTK_STP_DECODER Stp;
  static MTK_STP_QUEUE Queue;
  static unsigned char Outer[MTK_STP_FRAME_MAX],Read[64];
  MTK_STP_FRAME Frame;
  const unsigned Modes[]={MTK_STP_UART_MANDATORY,MTK_STP_BTIF_MANDATORY,MTK_STP_SDIO};
  const unsigned Chunks[]={1,7,51,MTK_STP_TX_MAX};
  unsigned M,K,N,I,At,Part,Encoded,Used,ReadBytes,Cursor,Done,Epoch,Old;
  MTK_GNSS_STATUS R;
  for (I=0;I<sizeof(Body);I++) Body[I]=(unsigned char)(I*37U);
  CHECK(MtkGnssEncode(0xfe2f,Body,sizeof(Body),Wire,sizeof(Wire),&N)==MtkGnssOk);
  for (M=0;M<3;M++) for (K=0;K<4;K++) {
    CHECK(MtkStpDecoderInit(&Stp,Modes[M])==MtkStpOk);
    CHECK(MtkStpQueueInit(&Queue,MTK_STP_GPS_CHANNEL)==MtkStpOk);
    Epoch=Queue.Epoch;
    for (At=0;At<N;At+=Part) {
      Part=N-At; if (Part>Chunks[K]) Part=Chunks[K];
      CHECK(MtkStpEncode(Modes[M],MTK_STP_GPS_CHANNEL,Wire+At,Part,Outer,sizeof(Outer),&Encoded)==MtkStpOk);
      if (Modes[M]==MTK_STP_SDIO) while (Encoded&3U) Outer[Encoded++]=0;
      CHECK(MtkStpReceive(&Stp,Outer,Encoded,&Used,&Frame)==MtkStpFrameReady && Used==Encoded);
      CHECK(MtkStpQueuePush(&Queue,Epoch,&Frame)==MtkStpOk);
    }
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk); Done=0;
    while (Queue.Count) {
      CHECK(MtkStpQueueRead(&Queue,Epoch,Read,sizeof(Read),&ReadBytes)==MtkStpOk && ReadBytes);
      Cursor=0;
      while (Cursor<ReadBytes) {
        R=MtkGnssReceive(&D,Read+Cursor,ReadBytes-Cursor,&Used,&F); Cursor+=Used;
        CHECK(Used && (R==MtkGnssNeedMore || R==MtkGnssFrameReady));
        if (R==MtkGnssFrameReady) {
          Done++;
          CHECK(F.Type==0xfe2f && F.Length==sizeof(Body) && Equal(F.Data,Body,sizeof(Body)));
        }
      }
    }
    CHECK(Done==1);
    /* A late STP callback is rejected before it can reach a restarted decoder. */
    Old=Epoch; CHECK(MtkStpQueueReset(&Queue,&Epoch)==MtkStpOk && Epoch!=Old);
    CHECK(MtkGnssDecoderInit(&D)==MtkGnssOk);
    CHECK(MtkStpQueuePush(&Queue,Old,&Frame)==MtkStpStale && !Queue.Count && !D.Used);
  }
  return 0;
}
static MTK_GNSS_OFFLOAD Offload;
static MTK_GNSS_MESSAGE Message;
static unsigned char OffloadStorage[MTK_GNSS_OFFLOAD_MESSAGE_MAX+2U];
static unsigned char Fragment[MTK_GNSS_OFFLOAD_PART_MAX+5U];
static unsigned AllZero(const unsigned char *P,unsigned N) {
  unsigned I; for (I=0;I<N;I++) if (P[I]) return 0; return 1;
}
static unsigned MakePart(unsigned Total,unsigned Index,unsigned Length,unsigned Seed) {
  unsigned I,N=Length+4U;
  Fragment[0]=(unsigned char)N; Fragment[1]=(unsigned char)(N>>8);
  Fragment[2]=(unsigned char)Total; Fragment[3]=(unsigned char)Index;
  for (I=0;I<Length;I++) Fragment[4U+I]=(unsigned char)(Seed+I*37U);
  return N;
}
static int OffloadTest(void) {
  unsigned N,L,I,J,T,At,Part,Epoch,Old;
  MTK_GNSS_STATUS R;
  OffloadStorage[0]=0x5a; OffloadStorage[sizeof(OffloadStorage)-1U]=0xa5;
  CHECK(MtkGnssOffloadInit(&Offload,OffloadStorage+1,4096)==MtkGnssOk);
  /* Every single-fragment size, including empty and escaped-marker bytes. */
  for (L=0;L<=MTK_GNSS_OFFLOAD_PART_MAX;L++) {
    N=MakePart(1,1,L,L);
    CHECK(MtkGnssOffloadReceive(&Offload,Offload.Epoch,Fragment,N,&Message)==MtkGnssFrameReady);
    CHECK(Message.Length==L && Message.Data==OffloadStorage+1 && Equal(Message.Data,Fragment+4,L));
    CHECK(AllZero(Offload.Buffer+L,Offload.Capacity-L));
  }
  CHECK(MtkGnssOffloadReset(&Offload,&Epoch)==MtkGnssOk);
  /* All U8 total values, variable-sized fragments: no guessed fixed stride. */
  for (T=1;T<=255;T++) {
    At=0;
    for (I=1;I<=T;I++) {
      L=(T+I)%17U; N=MakePart(T,I,L,I); Message.Length=0xfeed;
      R=MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message);
      CHECK(R==(I==T?MtkGnssFrameReady:MtkGnssNeedMore));
      for (J=0;J<L;J++) CHECK(Offload.Buffer[At+J]==(unsigned char)(I+J*37U));
      At+=L;
      CHECK(Offload.Used==At);
      if (I<T) CHECK(Message.Length==0xfeed);
    }
    CHECK(Message.Length==At && Message.Data==Offload.Buffer);
  }
  /* A fresh synchronous owner, with no old callbacks, uses the larger buffer.
   * Largest representable message: no 16-bit wrap or off-by-one. */
  CHECK(MtkGnssOffloadInit(&Offload,OffloadStorage+1,MTK_GNSS_OFFLOAD_MESSAGE_MAX)==MtkGnssOk);
  Epoch=Offload.Epoch;
  At=0; T=(MTK_GNSS_OFFLOAD_MESSAGE_MAX+MTK_GNSS_OFFLOAD_PART_MAX-1U)/MTK_GNSS_OFFLOAD_PART_MAX;
  for (I=1;I<=T;I++) {
    Part=MTK_GNSS_OFFLOAD_MESSAGE_MAX-At;
    if (Part>MTK_GNSS_OFFLOAD_PART_MAX) Part=MTK_GNSS_OFFLOAD_PART_MAX;
    N=MakePart(T,I,Part,At); R=MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message);
    CHECK(R==(I==T?MtkGnssFrameReady:MtkGnssNeedMore)); At+=Part;
  }
  CHECK(Message.Length==MTK_GNSS_OFFLOAD_MESSAGE_MAX);
  for (At=0,I=1;I<=T;I++) {
    Part=MTK_GNSS_OFFLOAD_MESSAGE_MAX-At;
    if (Part>MTK_GNSS_OFFLOAD_PART_MAX) Part=MTK_GNSS_OFFLOAD_PART_MAX;
    for (J=0;J<Part;J++) CHECK(Message.Data[At+J]==(unsigned char)(At+J*37U));
    At+=Part;
  }
  CHECK(OffloadStorage[0]==0x5a && OffloadStorage[sizeof(OffloadStorage)-1U]==0xa5);
  Old=Epoch; CHECK(MtkGnssOffloadReset(&Offload,&Epoch)==MtkGnssOk && Old!=Epoch);
  CHECK(AllZero(Offload.Buffer,Offload.Capacity));
  N=MakePart(2,1,3,7); CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssNeedMore);
  /* A stale, even NULL completion cannot read data, wipe or fault new input. */
  CHECK(MtkGnssOffloadReceive(&Offload,Old,0,0,0)==MtkGnssStale && Offload.Used==3 && !Offload.Faulted);
  N=MakePart(2,2,1,9); CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssFrameReady && Message.Length==4);
  CHECK(Message.Data[0]==7 && Message.Data[3]==9);
  return 0;
}
static int OffloadBadTest(void) {
  unsigned I,N,Epoch;
  const unsigned BadLengths[]={0,1,2,3,4,5,6,1537};
  CHECK(MtkGnssOffloadInit(0,OffloadStorage,8)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadInit(&Offload,0,8)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadInit(&Offload,OffloadStorage,0)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadInit(&Offload,OffloadStorage,65536)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadReset(0,&Epoch)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadInit(&Offload,OffloadStorage+1,32)==MtkGnssOk); Epoch=Offload.Epoch;
  CHECK(MtkGnssOffloadReset(&Offload,0)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadReceive(0,Epoch,Fragment,4,&Message)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadReceive(&Offload,Epoch,0,4,&Message)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,4,0)==MtkGnssInvalid);
  CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Offload.Buffer,4,&Message)==MtkGnssInvalid && !Offload.Faulted);
  for (I=0;I<sizeof(BadLengths)/sizeof(BadLengths[0])+10U;I++) {
    CHECK(MtkGnssOffloadReset(&Offload,&Epoch)==MtkGnssOk);
    N=MakePart(2,1,3,7); CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssNeedMore);
    N=MakePart(2,2,3,9); Message.Length=0xfeed;
    if (I<sizeof(BadLengths)/sizeof(BadLengths[0])) N=BadLengths[I];
    else switch (I-sizeof(BadLengths)/sizeof(BadLengths[0])) {
      case 0:Fragment[2]=0; break;
      case 1:Fragment[3]=0; break;
      case 2:Fragment[3]=3; break;
      case 3:Fragment[2]=3; break;
      case 4:Fragment[3]=1; break;
      case 5:Fragment[0]=6; break;
      case 6:Fragment[1]=1; break;
      case 7:Offload.Next=4; break;
      case 8:Offload.Total=256; break;
      default:Offload.Used=33; break;
    }
    CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssMalformed);
    CHECK(Offload.Faulted && !Offload.Used && Message.Length==0xfeed && AllZero(Offload.Buffer,Offload.Capacity));
    CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,7,&Message)==MtkGnssFaulted);
  }
  CHECK(MtkGnssOffloadReset(&Offload,&Epoch)==MtkGnssOk);
  N=MakePart(3,1,16,3); CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssNeedMore);
  N=MakePart(3,2,16,5); CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssNeedMore);
  N=MakePart(3,3,1,7); CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssCapacity);
  CHECK(Offload.Faulted && !Offload.Used && AllZero(Offload.Buffer,32));
  CHECK(MtkGnssOffloadReset(&Offload,&Epoch)==MtkGnssOk);
  N=MakePart(2,2,1,7); CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&Message)==MtkGnssMalformed);
  CHECK(MtkGnssOffloadReset(&Offload,&Epoch)==MtkGnssOk);
  Offload.Epoch=0xffffffffU; Epoch=77;
  CHECK(MtkGnssOffloadReset(&Offload,&Epoch)==MtkGnssFaulted && Epoch==77 && Offload.Epoch==0xffffffffU && Offload.Faulted);
  CHECK(MtkGnssOffloadReceive(&Offload,0xffffffffU,Fragment,N,&Message)==MtkGnssFaulted);
  CHECK(AllZero(Offload.Buffer,Offload.Capacity));
  return 0;
}
static unsigned char LocationRecord[MTK_GNSS_LOCATION_RECORD_BYTES+1U];
static unsigned char SvRecord[16U+3U*MTK_GNSS_SV_RECORD_BYTES+1U];
static unsigned char McuRecord[6U+MTK_GNSS_MCU_POSITION_BYTES+1U];
static void Put(unsigned char *P,uint64_t V,unsigned N) {
  unsigned I; for (I=0;I<N;I++) P[I]=(unsigned char)(V>>(8U*I));
}
static void LocationInput(unsigned char *P,unsigned Flags) {
  unsigned I;
  for (I=0;I<104;I++) P[I]=0xa5;
  Put(P+8,Flags,2); Put(P+16,UINT64_C(0xc046a00000000000),8); /* -45.25 */
  Put(P+24,UINT64_C(0x4060e00000000000),8); /* 135 */
  Put(P+32,UINT64_C(0xc008000000000000),8); /* -3 metres ellipsoid */
  Put(P+40,0x3fc00000U,4); Put(P+44,0x43b38000U,4); /* 1.5 m/s, 359 deg */
  Put(P+56,UINT64_C(1700000000123),8);
  Put(P+64,1,4); Put(P+68,0x00800000U,4);
  Put(P+72,0x007fffffU,4); Put(P+76,0x80000000U,4);
  Put(P+80,3,2); Put(P+88,UINT64_C(1234567890123),8);
  Put(P+96,UINT64_C(0x0020000000000001),8); /* tie: 2^53+1 */
}
static int LocationTest(void) {
  MTK_GNSS_LOCATION L;
  unsigned char *P=LocationRecord+1; unsigned I,J,Flags;
  const unsigned Offset[]={16,24,32,40,44,64,68,72,76};
  const unsigned Bit[]={1,1,2,4,8,16,32,64,128};
  for (Flags=0;Flags<256;Flags++) {
    LocationInput(P,Flags);
    CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssOk && L.Flags==Flags && !L.Reserved && !L.Reserved2);
    CHECK(L.LatitudeBits==((Flags&1)?UINT64_C(0xc046a00000000000):0));
    CHECK(L.LongitudeBits==((Flags&1)?UINT64_C(0x4060e00000000000):0));
    CHECK(L.AltitudeBits==((Flags&2)?UINT64_C(0xc008000000000000):0));
    CHECK(L.SpeedBits==((Flags&4)?UINT64_C(0x3ff8000000000000):0));
    CHECK(L.BearingBits==((Flags&8)?UINT64_C(0x4076700000000000):0));
    CHECK(L.HorizontalAccuracyBits==((Flags&16)?UINT64_C(0x36a0000000000000):0));
    CHECK(L.VerticalAccuracyBits==((Flags&32)?UINT64_C(0x3810000000000000):0));
    CHECK(L.SpeedAccuracyBits==((Flags&64)?UINT64_C(0x380fffffc0000000):0));
    CHECK(L.BearingAccuracyBits==((Flags&128)?UINT64_C(0x8000000000000000):0));
    CHECK(L.UtcMilliseconds==INT64_C(1700000000123) && L.ElapsedFlags==3);
    CHECK(L.ElapsedTimestampNs==UINT64_C(1234567890123) && L.ElapsedUncertaintyBits==UINT64_C(0x4340000000000000));
  }
  /* Absent values may contain junk; present nonfinite/range errors are atomic. */
  for (I=0;I<9;I++) for (J=0;J<3;J++) {
    unsigned N=I<3?8U:4U;
    uint64_t Bad=I<3?(J==0?UINT64_C(0x7ff0000000000000):J==1?UINT64_C(0x7ff8000000000001):UINT64_C(0xfff0000000000000)):
      (J==0?0x7f800000U:J==1?0x7fc00001U:0xff800000U);
    LocationInput(P,255); Put(P+Offset[I],Bad,N); L.Flags=0xfeed;
    CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed && L.Flags==0xfeed);
    Put(P+8,255U&~Bit[I],2); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssOk);
  }
  for (I=0;I<104;I++) CHECK(MtkGnssDecodeLocation(P,I,&L)==MtkGnssMalformed);
  CHECK(MtkGnssDecodeLocation(P,105,&L)==MtkGnssMalformed);
  CHECK(MtkGnssDecodeLocation(0,104,&L)==MtkGnssInvalid);
  CHECK(MtkGnssDecodeLocation(P,104,0)==MtkGnssInvalid);
  LocationInput(P,256); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed);
  LocationInput(P,255); Put(P+80,2,2); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed);
  Put(P+80,4,2); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed);
  Put(P+80,1,2); Put(P+88,UINT64_C(0x8000000000000000),8);
  CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed);
  Put(P+80,0,2); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssOk && !L.ElapsedTimestampNs && !L.ElapsedUncertaintyBits);
  LocationInput(P,255); Put(P+96,UINT64_MAX,8);
  CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssOk && L.ElapsedUncertaintyBits==UINT64_C(0x43f0000000000000));
  Put(P+16,UINT64_C(0x4056800000000001),8); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed);
  Put(P+16,UINT64_C(0xc056800000000000),8); Put(P+24,UINT64_C(0xc066800000000000),8);
  CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssOk);
  Put(P+44,0x43b40000U,4); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed);
  Put(P+44,0x80000000U,4); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssOk);
  Put(P+40,0xbf800000U,4); CHECK(MtkGnssDecodeLocation(P,104,&L)==MtkGnssMalformed);
  return 0;
}
static void SatelliteInput(unsigned char *P,unsigned Svid) {
  unsigned I; for (I=0;I<48;I++) P[I]=0xa5;
  Put(P+8,Svid,2); P[10]=1; Put(P+12,0x42140000U,4); /* 37 */
  Put(P+16,0xc1f00000U,4); Put(P+20,0x43b40000U,4); /* -30, 360 */
  P[24]=15; Put(P+32,0x4ebbcf26U,4); /* synthetic 1575457536 Hz */
  Put(P+40,UINT64_C(0x4042800010000000),8); /* 37+half binary32 ULP: even */
}
static int SatelliteTest(void) {
  unsigned char *P=SvRecord+1; MTK_GNSS_SATELLITE S[3]; unsigned N=0xfeed,I,J;
  for (I=0;I<16;I++) P[I]=0xa5;
  for (I=0;I<3;I++) SatelliteInput(P+16+48U*I,7);
  Put(P+8,3,4);
  CHECK(MtkGnssDecodeSvStatus(P,160,S,3,&N)==MtkGnssOk && N==3);
  for (I=0;I<3;I++) {
    CHECK(S[I].Svid==7 && S[I].Constellation==1 && S[I].Flags==15 && !S[I].Reserved);
    CHECK(S[I].Cn0Bits==0x42140000U && S[I].BasebandCn0Bits==0x42140000U);
    CHECK(S[I].ElevationBits==0xc1f00000U && S[I].AzimuthBits==0x43b40000U);
    CHECK(S[I].CarrierFrequencyHz==UINT64_C(1575457536));
  }
  N=0xfeed; S[0].Svid=0xfeed;
  CHECK(MtkGnssDecodeSvStatus(P,160,S,2,&N)==MtkGnssCapacity && N==0xfeed && S[0].Svid==0xfeed);
  for (I=0;I<160;I++) CHECK(MtkGnssDecodeSvStatus(P,I,S,3,&N)==MtkGnssMalformed && N==0xfeed && S[0].Svid==0xfeed);
  Put(P+8,0xffffffffU,4); CHECK(MtkGnssDecodeSvStatus(P,160,S,3,&N)==MtkGnssMalformed);
  Put(P+8,3,4);
  for (I=0;I<9;I++) {
    unsigned char *Q=P+16+2U*48;
    SatelliteInput(Q,7);
    switch (I) {
      case 0:Put(Q+8,0,2); break;
      case 1:Q[10]=8; break;
      case 2:Put(Q+12,0x7f800000U,4); break;
      case 3:Put(Q+16,0x42b40001U,4); break;
      case 4:Put(Q+20,0xbf800000U,4); break;
      case 5:Q[24]=16; break;
      case 6:Put(Q+32,0x5f000000U,4); break;
      case 7:Put(Q+40,UINT64_C(0x7ff8000000000001),8); break;
      default:Put(Q+40,UINT64_C(0x47f0000000000000),8); break;
    }
    CHECK(MtkGnssDecodeSvStatus(P,160,S,3,&N)==MtkGnssMalformed && N==0xfeed && S[0].Svid==0xfeed);
  }
  SatelliteInput(P+112,7); Put(P+8,0,4);
  CHECK(MtkGnssDecodeSvStatus(P,160,0,0,&N)==MtkGnssOk && !N);
  Put(P+8,1,4); CHECK(MtkGnssDecodeSvStatus(P,160,0,1,&N)==MtkGnssInvalid);
  CHECK(MtkGnssDecodeSvStatus(0,160,S,3,&N)==MtkGnssInvalid);
  CHECK(MtkGnssDecodeSvStatus(P,160,S,3,0)==MtkGnssInvalid);
  /* Normal/subnormal/round-to-even conversion at the stock double->float site. */
  {
    const uint64_t B[]={0,UINT64_C(0x8000000000000000),UINT64_C(0x3690000000000000),
      UINT64_C(0x3690000000000001),UINT64_C(0x36a0000000000000),UINT64_C(0x380fffffe0000000),
      UINT64_C(0x4042800010000000),UINT64_C(0x4042800030000000),UINT64_C(0x47efffffe0000000)};
    const uint32_t F[]={0,0x80000000U,0,1,1,0x00800000U,0x42140000U,0x42140002U,0x7f7fffffU};
    for (J=0;J<sizeof(B)/sizeof(B[0]);J++) {
      Put(P+16+40,B[J],8); CHECK(MtkGnssDecodeSvStatus(P,64,S,1,&N)==MtkGnssOk && S[0].BasebandCn0Bits==F[J]);
    }
  }
  return 0;
}
static int McuPositionTest(void) {
  unsigned char *P=McuRecord+1; MTK_GNSS_MESSAGE In={P,3174},Out;
  unsigned I,Opaque=0xfeed,N,Epoch;
  Put(P,2,2); Put(P+2,3168,2); Put(P+4,0xdeaa,2);
  for (I=0;I<3168;I++) P[6+I]=(unsigned char)(I*37U);
  CHECK(MtkGnssMcuPosition(&In,&Out,&Opaque)==MtkGnssOk && Out.Length==3168 && Out.Data==P+6 && Opaque==0xdeaa);
  for (I=0;I<3174;I++) {
    In.Length=I; Out.Length=0xfeed; Opaque=0xfeed;
    CHECK(MtkGnssMcuPosition(&In,&Out,&Opaque)==MtkGnssMalformed && Out.Length==0xfeed && Opaque==0xfeed);
  }
  In.Length=3175; CHECK(MtkGnssMcuPosition(&In,&Out,&Opaque)==MtkGnssMalformed);
  In.Length=3174; Put(P+2,3167,2); CHECK(MtkGnssMcuPosition(&In,&Out,&Opaque)==MtkGnssMalformed);
  Put(P,4,2); CHECK(MtkGnssMcuPosition(&In,&Out,&Opaque)==MtkGnssUnrelated);
  Put(P,2,2); Put(P+2,3168,2);
  CHECK(MtkGnssOffloadInit(&Offload,OffloadStorage+1,4000)==MtkGnssOk); Epoch=Offload.Epoch;
  for (I=0;I<3;I++) {
    unsigned Start=I*1532U,Part=3174U-Start,J;
    if (Part>1532U) Part=1532U;
    N=MakePart(3,I+1U,Part,0); for (J=0;J<Part;J++) Fragment[4+J]=P[Start+J];
    CHECK(MtkGnssOffloadReceive(&Offload,Epoch,Fragment,N,&In)==(I==2?MtkGnssFrameReady:MtkGnssNeedMore));
  }
  CHECK(MtkGnssMcuPosition(&In,&Out,&Opaque)==MtkGnssOk && Opaque==0xdeaa && Equal(Out.Data,P+6,3168));
  CHECK(MtkGnssMcuPosition(0,&Out,&Opaque)==MtkGnssInvalid);
  CHECK(MtkGnssMcuPosition(&In,0,&Opaque)==MtkGnssInvalid);
  CHECK(MtkGnssMcuPosition(&In,&Out,0)==MtkGnssInvalid);
  In.Data=0; CHECK(MtkGnssMcuPosition(&In,&Out,&Opaque)==MtkGnssInvalid);
  return 0;
}
static unsigned NmeaLine(const char *Text) {
  static const char H[]="0123456789ABCDEF";
  unsigned I=0,C=0;
  Original[0]='$';
  while (Text[I]) { C^=(unsigned char)Text[I]; Original[I+1U]=(unsigned char)Text[I]; I++; }
  Original[++I]='*'; Original[++I]=(unsigned char)H[C>>4]; Original[++I]=(unsigned char)H[C&15U];
  Original[++I]='\r'; Original[++I]='\n'; return I+1U;
}
static int NmeaTest(void) {
  MTK_GNSS_NMEA R; unsigned N,I;
  N=NmeaLine("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W");
  CHECK(MtkGnssNmeaParse(Original,N,1900,&R)==MtkGnssOk && R.Kind==1 && R.AuxFlags==33);
  CHECK(R.Year==1994 && R.Month==3 && R.Day==23 && R.TimeOfDayMilliseconds==45319000);
  CHECK(R.Location.Flags==13 && R.Location.UtcMilliseconds==INT64_C(764426119000));
  CHECK(R.Location.LatitudeBits==UINT64_C(0x40480f03afb7e910) && R.Location.LongitudeBits==UINT64_C(0x4027088888888889));
  CHECK(R.Location.SpeedBits==UINT64_C(0x40270c0f7949801d) && R.Location.BearingBits==UINT64_C(0x405519999999999a));
  CHECK(!R.Location.AltitudeBits && !R.Location.HorizontalAccuracyBits && !R.Location.ElapsedFlags);
  R.Kind=0xfeed;
  for (I=0;I<N;I++) CHECK(MtkGnssNmeaParse(Original,I,1900,&R)==MtkGnssMalformed && R.Kind==0xfeed);
  for (I=1;I<N-2;I++) {
    Original[I]^=1; CHECK(MtkGnssNmeaParse(Original,N,1900,&R)==MtkGnssMalformed && R.Kind==0xfeed); Original[I]^=1;
  }
  CHECK(MtkGnssNmeaParse(Original,N,1901,&R)==MtkGnssInvalid && R.Kind==0xfeed);
  CHECK(MtkGnssNmeaParse(0,N,1900,&R)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaParse(Original,N,1900,0)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaParse(Original,257,1900,&R)==MtkGnssMalformed);
  N=NmeaLine("GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
  CHECK(MtkGnssNmeaParse(Original,N,0,&R)==MtkGnssOk && R.Kind==2 && R.AuxFlags==46);
  CHECK(R.Location.Flags==3 && !R.Location.UtcMilliseconds && !R.Year && R.Quality==1 && R.SatellitesUsed==8);
  CHECK(R.Location.AltitudeBits==UINT64_C(0x4082826666666666));
  CHECK(R.HdopBits==UINT64_C(0x3feccccccccccccd) && !R.Location.HorizontalAccuracyBits);
  CHECK(R.MslAltitudeBits==UINT64_C(0x40810b3333333333) && R.GeoidSeparationBits==UINT64_C(0x4047733333333333));
  N=NmeaLine("GNGGA,123519,4807,N,01131,E,1,08,0.9,545.4,M,46.9,M,,");
  CHECK(MtkGnssNmeaParse(Original,N,0,&R)==MtkGnssOk &&
      R.TimeOfDayMilliseconds==45319000 && R.LatitudeNanominutes==2887000000000LL &&
      R.LongitudeNanominutes==691000000000LL);
  N=NmeaLine("GPGGA,123519,4807.038,S,01131.000,W,1,08,,545.4,M,,M,,");
  CHECK(MtkGnssNmeaParse(Original,N,0,&R)==MtkGnssOk && R.Location.Flags==1 && !R.Location.AltitudeBits);
  CHECK(R.AuxFlags==34 && R.Location.LatitudeBits==UINT64_C(0xc0480f03afb7e910));
  N=NmeaLine("GNRMC,000000.000000001,A,9000.000000000,S,18000.000000000,W,,,290200,,,A");
  CHECK(MtkGnssNmeaParse(Original,N,2000,&R)==MtkGnssOk && R.Location.Flags==1 && R.Year==2000 && R.Day==29);
  CHECK(R.Location.LatitudeBits==UINT64_C(0xc056800000000000) && R.Location.LongitudeBits==UINT64_C(0xc066800000000000));
  CHECK(MtkGnssNmeaParse(Original,N,1900,&R)==MtkGnssMalformed);
  N=NmeaLine("GPRMC,235960.125,A,0000.000,N,00000.000,E,0,0,311216,,,A");
  CHECK(MtkGnssNmeaParse(Original,N,2000,&R)==MtkGnssOk && R.AuxFlags==49 && R.TimeOfDayMilliseconds==86400125);
  CHECK(R.Location.UtcMilliseconds==INT64_C(1483228800125));
  N=NmeaLine("GPRMC,123519,V,,,,,,,230394,,,");
  CHECK(MtkGnssNmeaParse(Original,N,1900,&R)==MtkGnssOk && !R.Location.Flags && !R.Location.LatitudeBits && R.AuxFlags==33);
  N=NmeaLine("GPRMC,,V,,,,,,,,,,");
  CHECK(MtkGnssNmeaParse(Original,N,0,&R)==MtkGnssOk && !R.Location.Flags && !R.AuxFlags);
  N=NmeaLine("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,,,N");
  CHECK(MtkGnssNmeaParse(Original,N,1900,&R)==MtkGnssOk && !R.Location.Flags && R.Mode=='N');
  N=NmeaLine("GPGGA,,,,,,0,00,,,,,,,");
  CHECK(MtkGnssNmeaParse(Original,N,0,&R)==MtkGnssOk && !R.Location.Flags && !R.AuxFlags && !R.SatellitesUsed);
  {
    const char *Bad[]={
      "GPRMC,12.345,A,4807.038,N,01131.000,E,1,1,230394,,,A",
      "GPRMC,123519,A,1.23,N,01131.000,E,1,1,230394,,,A",
      "GPRMC,123519,A,4807.038,N,12.34,E,1,1,230394,,,A",
      "GPGGA,12.345,4807,N,01131,E,1,08,0.9,545.4,M,46.9,M,,",
      "GPGGA,123519,1.23,N,12.34,E,1,08,0.9,545.4,M,46.9,M,,",
      "GPRMC,123519,A,4860.000,N,01131.000,E,1,1,230394,,,A",
      "GPRMC,123519,A,4807.038,N,18000.001,E,1,1,230394,,,A",
      "GPRMC,123519,A,4807.038,E,01131.000,E,1,1,230394,,,A",
      "GPRMC,123519,A,4807.038,N,01131.000,E,-1,1,230394,,,A",
      "GPRMC,123519,A,4807.038,N,01131.000,E,1,360,230394,,,A",
      "GPRMC,123519,A,4807.038,N,01131.000,E,1,1,310299,,,A",
      "GPRMC,235961,A,4807.038,N,01131.000,E,1,1,230394,,,A",
      "GPRMC,123560,A,4807.038,N,01131.000,E,1,1,230394,,,A",
      "GPRMC,123519,A,4807.038,N,01131.000,E,1,1,230394,,,NN",
      "GPRMC,123519,A,4807.038,N,01131.000,E,1,1,,,,A",
      "GPRMC,123519,A,4807.0380000001,N,01131.000,E,1,1,230394,,,A",
      "GPGGA,123519,4807.038,N,01131.000,E,9,08,0.9,545.4,M,46.9,M,,",
      "GPGGA,123519,4807.038,N,01131.000,E,1,4294967296,0.9,545.4,M,46.9,M,,",
      "GPGGA,123519,4807.038,N,01131.000,E,1,08,-0.9,545.4,M,46.9,M,,",
      "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,F,46.9,M,,",
      "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,9223372036,M,9223372036,M,,",
      "GPRMC,123519,A,4807.038,N,01131.000,E,9999999999,1,230394,,,A",
      "GPRMC,123519,A,4807.038,N,01131.000,E,1e2,1,230394,,,A"};
    for (I=0;I<sizeof(Bad)/sizeof(Bad[0]);I++) {
      N=NmeaLine(Bad[I]); R.Kind=0xfeed;
      CHECK(MtkGnssNmeaParse(Original,N,2000,&R)==MtkGnssMalformed && R.Kind==0xfeed);
    }
  }
  N=NmeaLine("GNGSV,1,1,0"); R.Kind=0xfeed;
  CHECK(MtkGnssNmeaParse(Original,N,2000,&R)==MtkGnssUnrelated && R.Kind==0xfeed);
  return 0;
}
static MTK_GNSS_NMEA_STREAM Ns,NsBefore;
static MTK_GNSS_NMEA Nr;
static void SaveStreamState(void) {
  unsigned I;
  for (I=0;I<sizeof(Ns);I++) ((volatile unsigned char *)&NsBefore)[I]=((const unsigned char *)&Ns)[I];
}
static unsigned char Nrmc[MTK_GNSS_NMEA_MAX],Ngga[MTK_GNSS_NMEA_MAX],Nother[MTK_GNSS_NMEA_MAX];
static const char *RmcText="GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W";
static const char *GgaText="GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
static unsigned SaveNmea(const char *Text,unsigned char *Out) {
  unsigned N=NmeaLine(Text),I; for (I=0;I<N;I++) Out[I]=Original[I]; return N;
}
static MTK_GNSS_STATUS StreamLine(const char *Text,uint64_t Now) {
  unsigned N=NmeaLine(Text),C; MTK_GNSS_STATUS R;
  R=MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,Now,Original,N,&C,&Nr);
  return C==N?R:MtkGnssInvalid;
}
static unsigned StreamSensitiveZero(void) {
  return !Ns.Used && !Ns.HaveRmc && !Ns.HaveGga && !Ns.HaveChronology &&
    AllZero(Ns.Line,sizeof(Ns.Line)) && AllZero((const unsigned char *)&Ns.Rmc,sizeof(Ns.Rmc)) && AllZero((const unsigned char *)&Ns.Gga,sizeof(Ns.Gga));
}
static int NmeaStreamKnownTest(void) {
  unsigned A=SaveNmea(RmcText,Nrmc),B=SaveNmea(GgaText,Ngga),O=SaveNmea("GNGSV,1,1,00",Nother);
  unsigned Order,Split,C,FirstN,SecondN,I,J,Step; MTK_GNSS_STATUS R;
  const unsigned char *First,*Second;
  const unsigned Steps[]={1,7,256};
  CHECK(sizeof(Nr)==208 && sizeof(Ns)==760);
  for (Order=0;Order<2;Order++) {
    First=Order?Ngga:Nrmc; FirstN=Order?B:A; Second=Order?Nrmc:Ngga; SecondN=Order?A:B;
    for (Split=0;Split<=FirstN;Split++) {
      CHECK(MtkGnssNmeaStreamInit(&Ns,1900,1000)==MtkGnssOk); Nr.Kind=0xfeed;
      R=MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,10,First,Split,&C,&Nr);
      CHECK(C==Split && R==(Split==FirstN?MtkGnssFrameReady:MtkGnssNeedMore));
      if (Split<FirstN) {
        CHECK(Nr.Kind==0xfeed);
        CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,20,First+Split,FirstN-Split,&C,&Nr)==MtkGnssFrameReady && C==FirstN-Split);
      }
      CHECK(Nr.Kind==(Order?2U:1U) && Nr.TimeOfDayNanoseconds==UINT64_C(45319000000000));
      CHECK(Nr.LatitudeNanominutes==INT64_C(2887038000000) && Nr.LatitudeStepNanominutes==1000000U);
      CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,30,Second,SecondN,&C,&Nr)==MtkGnssFrameReady && C==SecondN);
      CHECK(Nr.Kind==3 && Nr.Location.Flags==15 && Nr.AuxFlags==47);
      CHECK(Nr.Location.UtcMilliseconds==INT64_C(764426119000) && Nr.Location.AltitudeBits==UINT64_C(0x4082826666666666));
      CHECK(Nr.Quality==1 && Nr.SatellitesUsed==8 && !Ns.HaveRmc && !Ns.HaveGga);
      CHECK(AllZero((const unsigned char *)&Ns.Rmc,sizeof(Ns.Rmc)) && AllZero((const unsigned char *)&Ns.Gga,sizeof(Ns.Gga)));
    }
    for (J=0;J<sizeof(Steps)/sizeof(Steps[0]);J++) {
      CHECK(MtkGnssNmeaStreamInit(&Ns,1900,1000)==MtkGnssOk);
      CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,10,First,FirstN,&C,&Nr)==MtkGnssFrameReady);
      for (I=0;I<SecondN;I+=Step) {
        Step=SecondN-I; if (Step>Steps[J]) Step=Steps[J]; Nr.Kind=0xfeed;
        R=MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,20,Second+I,Step,&C,&Nr);
        CHECK(C==Step && R==(I+Step==SecondN?MtkGnssFrameReady:MtkGnssNeedMore));
        CHECK(Nr.Kind==(I+Step==SecondN?3U:0xfeedU));
      }
    }
  }
  CHECK(MtkGnssNmeaStreamInit(&Ns,1900,1000)==MtkGnssOk);
  for (I=0;I<A;I++) Joined[I]=Nrmc[I];
  for (I=0;I<B;I++) Joined[A+I]=Ngga[I];
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,10,Joined,A+B,&C,&Nr)==MtkGnssFrameReady && C==A && Nr.Kind==1);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,20,Joined+C,B,&C,&Nr)==MtkGnssFrameReady && C==B && Nr.Kind==3);
  CHECK(MtkGnssNmeaStreamInit(&Ns,1900,1000)==MtkGnssOk);
  CHECK(StreamLine(RmcText,10)==MtkGnssFrameReady); Nr.Kind=0xfeed;
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,15,Nother,O,&C,&Nr)==MtkGnssUnrelated && C==O && Nr.Kind==0xfeed && Ns.HaveRmc);
  CHECK(StreamLine(GgaText,20)==MtkGnssFrameReady && Nr.Kind==3);
  return 0;
}
static int NmeaStreamTimeTest(void) {
  unsigned C,Epoch,Old;
  CHECK(MtkGnssNmeaStreamInit(&Ns,1900,1000)==MtkGnssOk);
  CHECK(StreamLine(RmcText,1)==MtkGnssFrameReady);
  CHECK(StreamLine(GgaText,1001)==MtkGnssFrameReady && Nr.Kind==3);
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine(RmcText,1)==MtkGnssFrameReady); Nr.Kind=0xfeed;
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1002,0,0,&C,&Nr)==MtkGnssNeedMore && !C && !Ns.HaveRmc && Nr.Kind==0xfeed);
  CHECK(AllZero((const unsigned char *)&Ns.Rmc,sizeof(Ns.Rmc)));
  CHECK(StreamLine(GgaText,1003)==MtkGnssFrameReady && Nr.Kind==2 && !(Nr.AuxFlags&MTK_GNSS_NMEA_UTC));
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine("GPRMC,123519.000000001,A,4807.038,N,01131.000,E,1,1,230394,,,A",1)==MtkGnssFrameReady);
  CHECK(StreamLine("GPGGA,123519.000999999,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,",2)==MtkGnssFrameReady && Nr.Kind==2);
  CHECK(StreamLine("GPRMC,123519.000999999,A,4807.038,N,01131.000,E,1,1,230394,,,A",3)==MtkGnssFrameReady && Nr.Kind==3);
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine(RmcText,1)==MtkGnssFrameReady);
  CHECK(StreamLine("GPGGA,123519,4807.0384,N,01131.0000,E,1,08,0.9,545.4,M,46.9,M,,",2)==MtkGnssFrameReady && Nr.Kind==3);
  CHECK(Nr.Location.LatitudeBits==UINT64_C(0x40480f03afb7e910));
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine(RmcText,1)==MtkGnssFrameReady);
  CHECK(StreamLine("GPGGA,123519,4807.0386,N,01131.0000,E,1,08,0.9,545.4,M,46.9,M,,",2)==MtkGnssFrameReady && Nr.Kind==2);
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine(RmcText,1)==MtkGnssFrameReady);
  CHECK(StreamLine("GPGGA,,,,,,0,00,,,,,,,",2)==MtkGnssFrameReady && !Nr.Location.Flags && !Ns.HaveRmc && !Ns.HaveGga);
  CHECK(StreamLine(GgaText,3)==MtkGnssFrameReady && Nr.Kind==2);
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine(RmcText,10)==MtkGnssFrameReady); Nr.Kind=0xfeed;
  CHECK(StreamLine("GPRMC,123519,A,4807.038,N,01131.000,E,1,1,220394,,,A",11)==MtkGnssMalformed && Nr.Kind==0xfeed && Ns.Faulted && StreamSensitiveZero());
  CHECK(MtkGnssNmeaStreamReset(&Ns,2000,&Epoch)==MtkGnssOk);
  CHECK(StreamLine("GPRMC,235960.500,A,4807.038,N,01131.000,E,1,1,311216,,,A",1)==MtkGnssFrameReady && (Nr.AuxFlags&16U));
  CHECK(StreamLine("GPRMC,000000.100,A,4807.038,N,01131.000,E,1,1,010117,,,A",2)==MtkGnssFrameReady && !Ns.Faulted && Nr.Year==2017);
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine(RmcText,10)==MtkGnssFrameReady); Nr.Kind=0xfeed;
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,9,0,0,&C,&Nr)==MtkGnssMalformed && !C && Nr.Kind==0xfeed && Ns.Faulted && StreamSensitiveZero());
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1,Nrmc,10,&C,&Nr)==MtkGnssNeedMore && C==10);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1002,0,0,&C,&Nr)==MtkGnssMalformed && Ns.Faulted && StreamSensitiveZero());
  Old=Ns.Epoch; CHECK(MtkGnssNmeaStreamReset(&Ns,2000,&Epoch)==MtkGnssOk && Epoch!=Old);
  SaveStreamState(); C=0xfeed;
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Old,0,0,0xffffffffU,&C,0)==MtkGnssStale && !C);
  CHECK(Equal((const unsigned char *)&Ns,(const unsigned char *)&NsBefore,sizeof(Ns)));
  CHECK(StreamLine(RmcText,UINT64_MAX)==MtkGnssFrameReady);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,0,0,0,&C,&Nr)==MtkGnssMalformed && StreamSensitiveZero());
  return 0;
}
static int NmeaStreamBadTest(void) {
  static char Text[MTK_GNSS_NMEA_MAX+1U];
  unsigned C,N,I,Epoch; const char Prefix[]="GPXXX,";
  CHECK(MtkGnssNmeaStreamInit(0,1900,1)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamInit(&Ns,0,1)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamInit(&Ns,1901,1)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamInit(&Ns,1900,0)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamInit(&Ns,1900,60001)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamInit(&Ns,1900,1000)==MtkGnssOk);
  SaveStreamState(); C=0xfeed;
  CHECK(MtkGnssNmeaStreamReset(&Ns,1901,&Epoch)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Ns.Epoch)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1,Nrmc,10,&Ns.Used,&Nr)==MtkGnssInvalid);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1,(const unsigned char *)&C,4,&C,&Nr)==MtkGnssInvalid && C==0xfeed);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1,Ns.Line,10,&C,&Nr)==MtkGnssInvalid && C==0xfeed);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1,Nrmc,10,&C,&Ns.Rmc)==MtkGnssInvalid && C==0xfeed);
  CHECK(Equal((const unsigned char *)&Ns,(const unsigned char *)&NsBefore,sizeof(Ns)));
  for (I=0;I<sizeof(Prefix)-1U;I++) Text[I]=Prefix[I];
  for (;I<250;I++) Text[I]='X';
  Text[250]=0;
  N=NmeaLine(Text); CHECK(N==MTK_GNSS_NMEA_MAX);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,1,Original,N,&C,&Nr)==MtkGnssUnrelated && C==N && !Ns.Used);
  CHECK(StreamLine(RmcText,2)==MtkGnssFrameReady);
  Text[250]='X'; Text[251]=0; N=NmeaLine(Text);
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,3,Original,N,&C,&Nr)==MtkGnssMalformed && C==256 && Ns.Faulted && StreamSensitiveZero());
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,3,0,0,&C,&Nr)==MtkGnssFaulted && !C);
  for (I=0;I<7;I++) {
    CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
    CHECK(StreamLine(RmcText,1)==MtkGnssFrameReady); N=SaveNmea(GgaText,Ngga); Nr.Kind=0xfeed;
    switch (I) {
      case 0:Ngga[20]^=1; break;
      case 1:Ngga[20]=0; break;
      case 2:Ngga[20]='$'; break;
      case 3:Ngga[N-1U]='X'; break;
      case 4:Ngga[20]='\n'; break;
      case 5:Ns.Used=MTK_GNSS_NMEA_MAX; break;
      default:Ns.HaveGga=2; break;
    }
    CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,2,Ngga,N,&C,&Nr)==MtkGnssMalformed && Ns.Faulted && StreamSensitiveZero() && Nr.Kind==0xfeed);
  }
  CHECK(MtkGnssNmeaStreamReset(&Ns,1900,&Epoch)==MtkGnssOk);
  CHECK(StreamLine(RmcText,1)==MtkGnssFrameReady); Ns.Epoch=0xffffffffU; Epoch=0xfeed;
  CHECK(MtkGnssNmeaStreamReset(&Ns,2000,&Epoch)==MtkGnssFaulted && Epoch==0xfeed && Ns.Faulted && StreamSensitiveZero());
  CHECK(MtkGnssNmeaStreamReceive(&Ns,Ns.Epoch,2,0,0,&C,&Nr)==MtkGnssFaulted);
  return 0;
}
int TestMain(void) {
  int R=KnownTest(); if (!R) R=StreamingTest(); if (!R) R=BadTest(); if (!R) R=StpIntegrationTest(); if (!R) R=OffloadTest(); if (!R) R=OffloadBadTest(); if (!R) R=LocationTest(); if (!R) R=SatelliteTest(); if (!R) R=McuPositionTest(); if (!R) R=NmeaTest(); if (!R) R=NmeaStreamKnownTest(); if (!R) R=NmeaStreamTimeTest(); if (!R) R=NmeaStreamBadTest(); return R;
}
#ifdef STP_TEST_NATIVE
int main(void) {
  int R=TestMain();
  if (R) { printf("GNSS failure at line %d\n",R); return 1; }
  printf("GNSS: %u checks PASS (no device access)\n",MtkGnssAssertionCount); return 0;
}
#endif
