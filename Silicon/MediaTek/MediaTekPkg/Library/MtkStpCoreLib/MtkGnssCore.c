// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkGnssCore.h>
#include <stdint.h>

static void Wipe(MTK_GNSS_DECODER *D) {
  unsigned I;
  for (I=0;I<MTK_GNSS_FRAME_MAX;I++) D->Buffer[I]=0;
  D->State=D->Used=D->Expected=0;
}
MTK_GNSS_STATUS MtkGnssDecoderInit(MTK_GNSS_DECODER *D) {
  if (!D) return MtkGnssInvalid;
  Wipe(D); D->Initialized=1; D->Faulted=0; return MtkGnssOk;
}
MTK_GNSS_STATUS MtkGnssReceive(MTK_GNSS_DECODER *D,const unsigned char *Data,
  unsigned Length,unsigned *Consumed,MTK_GNSS_FRAME *Frame) {
  unsigned I=0,B,Sum,J;
  if (!Consumed) return MtkGnssInvalid;
  *Consumed=0;
  if (!D || !Frame || (!Data && Length) || D->Initialized!=1) return MtkGnssInvalid;
  if (D->Faulted) return MtkGnssFaulted;
  if (D->State>5 || D->Used>MTK_GNSS_FRAME_MAX || D->Expected>MTK_GNSS_FRAME_MAX ||
      (D->Expected && (D->Expected<10 || D->Used>D->Expected))) goto Bad;
  if (D->State==5) Wipe(D);
  while (I<Length) {
    B=Data[I++];
    switch (D->State) {
    case 0:
      if (B==0xaa) D->State=1;
      break;
    case 1:
      if (B==0xf0) {
        D->Buffer[0]=0xaa; D->Buffer[1]=0xf0; D->Used=2; D->State=2;
      } else if (B!=0xaa) D->State=0;
      break;
    case 2:
      if (B==0xaa) { D->State=4; break; }
      if (B==0xde) { D->State=3; break; }
      goto Append;
    case 3:
      if (B!=0xdf && B!=0xe0) goto Bad;
      B=B==0xdf?0xaaU:0xdeU; D->State=2;
Append:
      if (D->Used>=MTK_GNSS_FRAME_MAX-2U ||
          (D->Expected && D->Used>=D->Expected-2U)) goto Bad;
      D->Buffer[D->Used++]=(unsigned char)B;
      if (D->Used==4) {
        J=D->Buffer[2]|((D->Buffer[3]&15U)<<8);
        if (J<4 || J>MTK_GNSS_FRAME_MAX-6U) goto Bad;
        D->Expected=J+6U;
      }
      break;
    case 4:
      if (B!=0x0f || !D->Expected || D->Used!=D->Expected-2U) goto Bad;
      Sum=0;
      for (J=2;J<D->Used-2U;J++) Sum+=D->Buffer[J];
      if ((Sum&0xffffU)!=(D->Buffer[J]|((unsigned)D->Buffer[J+1U]<<8))) goto Bad;
      D->Buffer[D->Used++]=0xaa; D->Buffer[D->Used++]=0x0f; D->State=5;
      Frame->Type=D->Buffer[4]|((unsigned)D->Buffer[5]<<8);
      Frame->Flags=D->Buffer[3]>>4; Frame->Length=D->Used-10U;
      Frame->Data=D->Buffer+6; Frame->Raw=D->Buffer; Frame->RawLength=D->Used;
      *Consumed=I; return MtkGnssFrameReady;
    default:goto Bad;
    }
  }
  *Consumed=I; return MtkGnssNeedMore;
Bad:
  Wipe(D); D->Faulted=1; *Consumed=I; return MtkGnssMalformed;
}
static unsigned EscapeSize(unsigned B) { return B==0xaa || B==0xde?2U:1U; }
static unsigned Escape(unsigned char *Out,unsigned B) {
  if (B==0xaa || B==0xde) { Out[0]=0xde; Out[1]=B==0xaa?0xdf:0xe0; return 2; }
  Out[0]=(unsigned char)B; return 1;
}
MTK_GNSS_STATUS MtkGnssEncode(unsigned Type,const unsigned char *Data,
  unsigned Length,unsigned char *Out,unsigned Capacity,unsigned *Written) {
  unsigned I,N=4,Sum,J=Length+4U,At=2;
  unsigned char H[6]; uintptr_t A=(uintptr_t)Data,B=(uintptr_t)Out;
  if (!Written) return MtkGnssInvalid;
  *Written=0;
  if (Type>0xffffU || Length>MTK_GNSS_PAYLOAD_MAX || (!Data && Length) || !Out)
    return MtkGnssInvalid;
  H[0]=(unsigned char)J; H[1]=(unsigned char)(J>>8);
  H[2]=(unsigned char)Type; H[3]=(unsigned char)(Type>>8);
  Sum=H[0]+H[1]+H[2]+H[3];
  for (I=0;I<Length;I++) Sum+=Data[I];
  H[4]=(unsigned char)Sum; H[5]=(unsigned char)(Sum>>8);
  for (I=0;I<6;I++) N+=EscapeSize(H[I]);
  for (I=0;I<Length;I++) N+=EscapeSize(Data[I]);
  if (Length && (A<B?(B-A<Length):(A-B<N))) return MtkGnssInvalid;
  if (Capacity<N) return MtkGnssCapacity;
  Out[0]=0xaa; Out[1]=0xf0;
  for (I=0;I<4;I++) At+=Escape(Out+At,H[I]);
  for (I=0;I<Length;I++) At+=Escape(Out+At,Data[I]);
  At+=Escape(Out+At,H[4]); At+=Escape(Out+At,H[5]);
  Out[At++]=0xaa; Out[At++]=0x0f; *Written=At; return MtkGnssOk;
}

static void OffloadClear(MTK_GNSS_OFFLOAD *S) {
  unsigned I;
  volatile unsigned char *P=S->Buffer;
  for (I=0;I<S->Capacity;I++) P[I]=0;
  S->Used=S->Total=0; S->Next=1;
}
static unsigned OffloadValid(const MTK_GNSS_OFFLOAD *S) {
  return S && S->Buffer && S->Capacity &&
    S->Capacity<=MTK_GNSS_OFFLOAD_MESSAGE_MAX && S->Epoch;
}
MTK_GNSS_STATUS MtkGnssOffloadInit(MTK_GNSS_OFFLOAD *S,
  unsigned char *Buffer,unsigned Capacity) {
  if (!S || !Buffer || !Capacity || Capacity>MTK_GNSS_OFFLOAD_MESSAGE_MAX)
    return MtkGnssInvalid;
  S->Buffer=Buffer; S->Capacity=Capacity; S->Epoch=1; S->Faulted=0;
  OffloadClear(S); return MtkGnssOk;
}
MTK_GNSS_STATUS MtkGnssOffloadReset(MTK_GNSS_OFFLOAD *S,unsigned *Epoch) {
  if (!Epoch || !OffloadValid(S)) return MtkGnssInvalid;
  OffloadClear(S);
  if (S->Epoch==0xffffffffU) { S->Faulted=1; return MtkGnssFaulted; }
  S->Epoch++; S->Faulted=0; *Epoch=S->Epoch; return MtkGnssOk;
}
MTK_GNSS_STATUS MtkGnssOffloadReceive(MTK_GNSS_OFFLOAD *S,unsigned Epoch,
  const unsigned char *Data,unsigned Length,MTK_GNSS_MESSAGE *Message) {
  unsigned Total,Index,Body,I;
  uintptr_t A=(uintptr_t)Data,B;
  MTK_GNSS_STATUS Error=MtkGnssMalformed;
  if (!OffloadValid(S)) return MtkGnssInvalid;
  if (Epoch!=S->Epoch) return MtkGnssStale;
  if (S->Faulted) return MtkGnssFaulted;
  if (!Data || !Message) return MtkGnssInvalid;
  B=(uintptr_t)S->Buffer;
  if (A<B?(B-A<Length):(A-B<S->Capacity)) return MtkGnssInvalid;
  if (S->Used>S->Capacity || S->Total>255 || !S->Next ||
      S->Next>S->Total+1U || (!S->Total && S->Used)) goto Bad;
  if (S->Total && S->Next==S->Total+1U) OffloadClear(S);
  if (Length<4 || Length>MTK_GNSS_OFFLOAD_PART_MAX+4U ||
      (Data[0]|((unsigned)Data[1]<<8))!=Length) goto Bad;
  Total=Data[2]; Index=Data[3]; Body=Length-4U;
  if (!Total || !Index || Index>Total || Index!=S->Next ||
      (S->Total && Total!=S->Total)) goto Bad;
  if (Body>S->Capacity-S->Used) { Error=MtkGnssCapacity; goto Bad; }
  S->Total=Total;
  for (I=0;I<Body;I++) S->Buffer[S->Used+I]=Data[4U+I];
  S->Used+=Body; S->Next++;
  if (Index!=Total) return MtkGnssNeedMore;
  Message->Data=S->Buffer; Message->Length=S->Used; return MtkGnssFrameReady;
Bad:
  OffloadClear(S); S->Faulted=1; return Error;
}

_Static_assert(sizeof(MTK_GNSS_LOCATION)==112,"GNSS location layout");
_Static_assert(sizeof(MTK_GNSS_SATELLITE)==40,"GNSS satellite layout");
/* All field conversion uses integer arithmetic, including IEEE rounding.
 * This also keeps the shared decoder usable in OS kernel/firmware contexts. */
static uint64_t Le(const unsigned char *P,unsigned N) {
  uint64_t V=0; unsigned I;
  for (I=0;I<N;I++) V|=(uint64_t)P[I]<<(8U*I);
  return V;
}
static unsigned Finite(uint64_t V) { return ((V>>52)&2047U)!=2047U; }
static unsigned Nonnegative(uint64_t V) { return !(V>>63) || !(V<<1); }
static unsigned Bounded(uint64_t V,uint64_t Maximum) {
  return (V&UINT64_C(0x7fffffffffffffff))<=Maximum;
}
static uint64_t Widen(uint32_t V) {
  uint64_t Sign=(uint64_t)(V>>31)<<63;
  unsigned E=(V>>23)&255U,F=V&0x7fffffU;
  if (!E) {
    if (!F) return Sign;
    E=897U;
    while (!(F&0x800000U)) { F<<=1; E--; }
    return Sign|((uint64_t)E<<52)|((uint64_t)(F&0x7fffffU)<<29);
  }
  return Sign|((uint64_t)(E==255U?2047U:E+896U)<<52)|((uint64_t)F<<29);
}
static uint64_t RoundShift(uint64_t V,unsigned Shift) {
  uint64_t Q=V>>Shift,R=V&((UINT64_C(1)<<Shift)-1U),Half=UINT64_C(1)<<(Shift-1U);
  return Q+(R>Half || (R==Half && (Q&1U)));
}
static uint64_t UnsignedDouble(uint64_t V) {
  uint64_t Q; unsigned E=0; uint64_t T=V;
  if (!V) return 0;
  while (T>>1) { T>>=1; E++; }
  Q=E>52U?RoundShift(V,E-52U):V<<(52U-E);
  if (Q==(UINT64_C(1)<<53)) { Q>>=1; E++; }
  return ((uint64_t)(E+1023U)<<52)|(Q&UINT64_C(0xfffffffffffff));
}
static uint32_t Narrow(uint64_t V) {
  unsigned E=(unsigned)(V>>52)&2047U,Shift;
  uint64_t Q; uint32_t Sign=(uint32_t)(V>>32)&0x80000000U;
  if (E<873U) return Sign; /* Below half the smallest binary32 subnormal. */
  Shift=E<897U?926U-E:29U;
  Q=RoundShift((V&UINT64_C(0xfffffffffffff))|(UINT64_C(1)<<52),Shift);
  if (E<897U) return Sign|(uint32_t)Q;
  if (Q==(UINT64_C(1)<<24)) { Q>>=1; E++; }
  return Sign|((E-896U)<<23)|((uint32_t)Q&0x7fffffU);
}
MTK_GNSS_STATUS MtkGnssDecodeLocation(const unsigned char *P,unsigned Length,
  MTK_GNSS_LOCATION *Location) {
  MTK_GNSS_LOCATION L={0};
  uint64_t *Values[]={&L.SpeedBits,&L.BearingBits,&L.HorizontalAccuracyBits,
    &L.VerticalAccuracyBits,&L.SpeedAccuracyBits,&L.BearingAccuracyBits};
  const unsigned Offsets[]={40,44,64,68,72,76};
  unsigned I;
  if (!P || !Location) return MtkGnssInvalid;
  if (Length!=MTK_GNSS_LOCATION_RECORD_BYTES) return MtkGnssMalformed;
  L.Flags=(uint32_t)Le(P+8,2); L.ElapsedFlags=(uint32_t)Le(P+80,2);
  if (L.Flags&~255U || L.ElapsedFlags&~3U || L.ElapsedFlags==2U) return MtkGnssMalformed;
  if (L.Flags&1U) {
    L.LatitudeBits=Le(P+16,8); L.LongitudeBits=Le(P+24,8);
    if (!Bounded(L.LatitudeBits,UINT64_C(0x4056800000000000)) ||
        !Bounded(L.LongitudeBits,UINT64_C(0x4066800000000000))) return MtkGnssMalformed;
  }
  if (L.Flags&2U) { L.AltitudeBits=Le(P+32,8); if (!Finite(L.AltitudeBits)) return MtkGnssMalformed; }
  for (I=0;I<6;I++) if (L.Flags&(4U<<I)) {
    *Values[I]=Widen((uint32_t)Le(P+Offsets[I],4));
    if (!Finite(*Values[I]) || !Nonnegative(*Values[I])) return MtkGnssMalformed;
    if (I==1 && (*Values[I]&UINT64_C(0x7fffffffffffffff))>=UINT64_C(0x4076800000000000)) return MtkGnssMalformed;
  }
  /* Signed UTC is copied by bytes to avoid unsigned-to-signed implementation
   * choices; no temporal inference is made from the numeric value. */
  for (I=0;I<8;I++) ((unsigned char *)&L.UtcMilliseconds)[I]=P[56U+I];
  if (L.ElapsedFlags&1U) {
    L.ElapsedTimestampNs=Le(P+88,8);
    if (L.ElapsedTimestampNs>INT64_MAX) return MtkGnssMalformed;
  }
  if (L.ElapsedFlags&2U) L.ElapsedUncertaintyBits=UnsignedDouble(Le(P+96,8));
  *Location=L; return MtkGnssOk;
}
static unsigned ReadSatellite(const unsigned char *P,MTK_GNSS_SATELLITE *S) {
  uint64_t Baseband,Carrier; unsigned E;
  S->Svid=(int32_t)Le(P+8,2); S->Constellation=P[10];
  S->Cn0Bits=(uint32_t)Le(P+12,4); S->ElevationBits=(uint32_t)Le(P+16,4);
  S->AzimuthBits=(uint32_t)Le(P+20,4); S->Flags=P[24]; S->Reserved=0;
  Baseband=Le(P+40,8); Carrier=Widen((uint32_t)Le(P+32,4));
  if (!S->Svid || S->Svid>32767 || S->Constellation>7 || S->Flags&~15U ||
      !Finite(Widen(S->Cn0Bits)) || !Nonnegative(Widen(S->Cn0Bits)) ||
      !Bounded(Widen(S->ElevationBits),UINT64_C(0x4056800000000000)) ||
      !Bounded(Widen(S->AzimuthBits),UINT64_C(0x4076800000000000)) ||
      !Nonnegative(Widen(S->AzimuthBits)) || !Nonnegative(Baseband) ||
      !Bounded(Baseband,UINT64_C(0x47efffffe0000000))) return 0;
  S->BasebandCn0Bits=Narrow(Baseband);
  S->CarrierFrequencyHz=0;
  if (S->Flags&8U) {
    if (!Finite(Carrier) || !Nonnegative(Carrier) || (Carrier&UINT64_C(0x7fffffffffffffff))>=UINT64_C(0x43e0000000000000)) return 0;
    E=(unsigned)(Carrier>>52)&2047U;
    if (E>=1023U) {
      uint64_t V=(Carrier&UINT64_C(0xfffffffffffff))|(UINT64_C(1)<<52);
      S->CarrierFrequencyHz=E>1075U?V<<(E-1075U):V>>(1075U-E);
    }
  }
  return 1;
}
MTK_GNSS_STATUS MtkGnssDecodeSvStatus(const unsigned char *P,unsigned Length,
  MTK_GNSS_SATELLITE *Satellites,unsigned Capacity,unsigned *Count) {
  unsigned N,I; MTK_GNSS_SATELLITE S;
  if (!P || !Count) return MtkGnssInvalid;
  if (Length<16U) return MtkGnssMalformed;
  N=(unsigned)Le(P+8,4);
  if (N>0x7fffffffU || N>(Length-16U)/MTK_GNSS_SV_RECORD_BYTES) return MtkGnssMalformed;
  if (N>Capacity) return MtkGnssCapacity;
  if (N && !Satellites) return MtkGnssInvalid;
  for (I=0;I<N;I++) if (!ReadSatellite(P+16U+I*MTK_GNSS_SV_RECORD_BYTES,&S)) return MtkGnssMalformed;
  for (I=0;I<N;I++) { ReadSatellite(P+16U+I*MTK_GNSS_SV_RECORD_BYTES,&S); Satellites[I]=S; }
  *Count=N; return MtkGnssOk;
}
MTK_GNSS_STATUS MtkGnssMcuPosition(const MTK_GNSS_MESSAGE *Message,
  MTK_GNSS_MESSAGE *Position,unsigned *Opaque) {
  const unsigned char *P;
  if (!Message || !Message->Data || !Position || !Opaque) return MtkGnssInvalid;
  P=Message->Data;
  if (Message->Length<6U) return MtkGnssMalformed;
  if (Le(P,2)!=2U) return MtkGnssUnrelated;
  if (Le(P+2,2)!=MTK_GNSS_MCU_POSITION_BYTES ||
      Message->Length!=6U+MTK_GNSS_MCU_POSITION_BYTES) return MtkGnssMalformed;
  *Opaque=(unsigned)Le(P+4,2); Position->Data=P+6;
  Position->Length=MTK_GNSS_MCU_POSITION_BYTES; return MtkGnssOk;
}

/* Generate correctly rounded binary64 n/d with overflow-safe long division. */
static uint64_t RationalDouble(uint64_t N,uint64_t D,unsigned Negative) {
  unsigned NB=0,DB=0,I; int E; uint64_t T,R,Q=1;
  uint64_t Sign=(uint64_t)Negative<<63;
  if (!N) return Sign;
  for (T=N;T;T>>=1) NB++;
  for (T=D;T;T>>=1) DB++;
  E=(int)NB-(int)DB;
  if (E>=0) {
    if (N<(D<<(unsigned)E)) E--;
  } else if ((N<<(unsigned)-E)<D) E--;
  if (E>=0) { D<<=(unsigned)E; R=N-D; }
  else R=(N<<(unsigned)-E)-D;
  for (I=0;I<52;I++) {
    Q<<=1;
    if (R>=D-R) { R-=D-R; Q|=1; } else R<<=1;
  }
  if (R>D-R || (R==D-R && (Q&1U))) Q++;
  if (Q==(UINT64_C(1)<<53)) { Q>>=1; E++; }
  return Sign|((uint64_t)(E+1023)<<52)|(Q&UINT64_C(0xfffffffffffff));
}
typedef struct { const unsigned char *P; unsigned N; } NMEA_FIELD;
static int Hex(unsigned C) {
  if (C>='0' && C<='9') return (int)(C-'0');
  if (C>='A' && C<='F') return (int)(C-'A'+10U);
  if (C>='a' && C<='f') return (int)(C-'a'+10U);
  return -1;
}
static unsigned Digits(NMEA_FIELD F,unsigned *Value) {
  unsigned I,V=0;
  if (!F.N) return 0;
  for (I=0;I<F.N;I++) {
    unsigned C=F.P[I];
    if (C<'0' || C>'9' || V>(0xffffffffU-(C-'0'))/10U) return 0;
    V=V*10U+C-'0';
  }
  *Value=V; return 1;
}
static unsigned Decimal(NMEA_FIELD F,unsigned Signed,int64_t *Value) {
  unsigned I=0,Frac=0,Dot=0,Negative=0,Whole=0; uint64_t V=0;
  if (!F.N) return 0;
  if (Signed && (F.P[0]=='-' || F.P[0]=='+')) { Negative=F.P[0]=='-'; I++; }
  for (;I<F.N;I++) {
    unsigned C=F.P[I];
    if (C=='.' && !Dot && Whole) { Dot=1; continue; }
    if (C<'0' || C>'9' || (Dot && Frac==9) || V>((uint64_t)INT64_MAX-(C-'0'))/10U) return 0;
    V=V*10U+C-'0'; if (Dot) Frac++; else Whole++;
  }
  if (!Whole || (Dot && !Frac)) return 0;
  while (Frac<9) { if (V>(uint64_t)INT64_MAX/10U) return 0; V*=10U; Frac++; }
  *Value=Negative?-(int64_t)V:(int64_t)V; return 1;
}
/* NMEA coordinates and UTC have fixed-width integer prefixes. A decimal
 * point inside that prefix is never allowed, even at exactly that width. */
static unsigned FixedDecimal(NMEA_FIELD F,unsigned Width,int64_t *Value) {
  unsigned Whole;
  NMEA_FIELD Prefix={F.P,Width};
  return F.N>=Width && (F.N==Width || F.P[Width]=='.') &&
    Digits(Prefix,&Whole) && Decimal(F,0,Value);
}
static uint64_t SignedRational(int64_t V) {
  return RationalDouble(V<0?0U-(uint64_t)V:(uint64_t)V,UINT64_C(1000000000),V<0);
}
static unsigned Coordinate(NMEA_FIELD F,NMEA_FIELD H,unsigned Longitude,uint64_t *Bits,
  int64_t *Nanominutes,unsigned *Step) {
  int64_t V; uint64_t Degree,Minute; unsigned Whole=Longitude?5U:4U,Negative;
  if (H.N!=1 || !FixedDecimal(F,Whole,&V)) return 0;
  if (Longitude) { if (H.P[0]!='E' && H.P[0]!='W') return 0; Negative=H.P[0]=='W'; }
  else { if (H.P[0]!='N' && H.P[0]!='S') return 0; Negative=H.P[0]=='S'; }
  Degree=(uint64_t)V/UINT64_C(100000000000); Minute=(uint64_t)V%UINT64_C(100000000000);
  if (Minute>=UINT64_C(60000000000) || Degree>(Longitude?180U:90U) ||
      (Degree==(Longitude?180U:90U) && Minute)) return 0;
  *Nanominutes=(int64_t)(Degree*UINT64_C(60000000000)+Minute);
  *Bits=RationalDouble((uint64_t)*Nanominutes,UINT64_C(60000000000),Negative);
  if (Negative) *Nanominutes=-*Nanominutes;
  *Step=1;
  { unsigned Digits=F.N>Whole?F.N-Whole-1U:0;
    while (Digits++<9U) *Step*=10U;
  }
  return 1;
}
static unsigned TimeOfDay(NMEA_FIELD F,MTK_GNSS_NMEA *R) {
  int64_t V; unsigned HMS,Hour,Minute,Second;
  if (!FixedDecimal(F,6,&V)) return 0;
  HMS=(unsigned)((uint64_t)V/UINT64_C(1000000000));
  if ((uint64_t)V/UINT64_C(1000000000)>235960U) return 0;
  Hour=HMS/10000U; Minute=HMS/100U%100U; Second=HMS%100U;
  if (Hour>23 || Minute>59 || Second>60 || (Second==60 && (Hour!=23 || Minute!=59))) return 0;
  R->TimeOfDayNanoseconds=(Hour*3600U+Minute*60U+Second)*UINT64_C(1000000000)+(uint64_t)V%UINT64_C(1000000000);
  R->TimeOfDayMilliseconds=(unsigned)(R->TimeOfDayNanoseconds/1000000U);
  R->AuxFlags|=MTK_GNSS_NMEA_TIME_OF_DAY;
  if (Second==60) R->AuxFlags|=MTK_GNSS_NMEA_LEAP_SECOND;
  return 1;
}
static unsigned RmcDate(NMEA_FIELD F,unsigned Century,MTK_GNSS_NMEA *R) {
  static const unsigned Days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  unsigned V,Y,M,D,Leap,I; int64_t Total;
  if (F.N!=6 || !Digits(F,&V)) return 0;
  Y=Century+V%100U; M=V/100U%100U; D=V/10000U;
  Leap=Y%4U==0 && (Y%100U!=0 || Y%400U==0);
  if (!M || M>12 || !D || D>Days[M-1U]+(M==2U && Leap)) return 0;
  R->Year=Y; R->Month=M; R->Day=D;
  Total=365*((int64_t)Y-1970)+(Y-1U)/4U-1969/4-(Y-1U)/100U+1969/100+(Y-1U)/400U-1969/400;
  for (I=1;I<M;I++) Total+=Days[I-1U]+(I==2U && Leap);
  R->Location.UtcMilliseconds=(Total+D-1)*INT64_C(86400000)+R->TimeOfDayMilliseconds;
  R->AuxFlags|=MTK_GNSS_NMEA_UTC; return 1;
}
static void NmeaCopy(void *Out,const void *Input,unsigned Length) {
  unsigned I;
  for (I=0;I<Length;I++) ((volatile unsigned char *)Out)[I]=((const unsigned char *)Input)[I];
}
static void NmeaZero(void *Out,unsigned Length) {
  unsigned I; for (I=0;I<Length;I++) ((volatile unsigned char *)Out)[I]=0;
}
MTK_GNSS_STATUS MtkGnssNmeaParse(const unsigned char *P,unsigned Length,
  unsigned Century,MTK_GNSS_NMEA *Report) {
  NMEA_FIELD F[24]; MTK_GNSS_NMEA R={0};
  unsigned I,At=1,Count=0,Xor=0,End,Active; int A,B; int64_t V,Msl=0,Geoid=0;
  if (!P || !Report) return MtkGnssInvalid;
  if (Length<11 || Length>MTK_GNSS_NMEA_MAX || P[0]!='$' || P[Length-2]!='\r' || P[Length-1]!='\n') return MtkGnssMalformed;
  End=Length-5U;
  if (P[End]!='*' || (A=Hex(P[End+1U]))<0 || (B=Hex(P[End+2U]))<0) return MtkGnssMalformed;
  for (I=1;I<End;I++) { if (P[I]<32 || P[I]>126 || P[I]=='*') return MtkGnssMalformed; Xor^=P[I]; }
  if (Xor!=(unsigned)(A*16+B)) return MtkGnssMalformed;
  if (End<7 || P[1]<'A' || P[1]>'Z' || P[2]<'A' || P[2]>'Z' || P[6]!=',') return MtkGnssUnrelated;
  if (P[3]=='R' && P[4]=='M' && P[5]=='C') R.Kind=1;
  else if (P[3]=='G' && P[4]=='G' && P[5]=='A') R.Kind=2;
  else return MtkGnssUnrelated;
  for (I=1;I<=End;I++) if (I==End || P[I]==',') {
    if (Count==24) return MtkGnssMalformed;
    F[Count].P=P+At; F[Count++].N=I-At; At=I+1U;
  }
  if (R.Kind==1) {
    if (Count<10 || F[2].N!=1 || (F[2].P[0]!='A' && F[2].P[0]!='V')) return MtkGnssMalformed;
    Active=F[2].P[0]=='A';
    if (Count>12 && F[12].N) {
      if (F[12].N!=1 || F[12].P[0]<'A' || F[12].P[0]>'Z') return MtkGnssMalformed;
      R.Mode=F[12].P[0]; if (R.Mode=='N') Active=0;
    }
    if (F[1].N || F[9].N || Active) {
      if (Century<1900 || Century>9900 || Century%100U) return MtkGnssInvalid;
      if (!TimeOfDay(F[1],&R) || !RmcDate(F[9],Century,&R)) return MtkGnssMalformed;
    }
    if (Active) {
      if (!Coordinate(F[3],F[4],0,&R.Location.LatitudeBits,&R.LatitudeNanominutes,&R.LatitudeStepNanominutes) || !Coordinate(F[5],F[6],1,&R.Location.LongitudeBits,&R.LongitudeNanominutes,&R.LongitudeStepNanominutes)) return MtkGnssMalformed;
      R.Location.Flags=1;
      if (F[7].N) {
        if (!Decimal(F[7],0,&V) || (uint64_t)V>UINT64_MAX/463U) return MtkGnssMalformed;
        R.Location.SpeedBits=RationalDouble((uint64_t)V*463U,UINT64_C(900000000000),0); R.Location.Flags|=4;
      }
      if (F[8].N) {
        if (!Decimal(F[8],0,&V) || V>=INT64_C(360000000000)) return MtkGnssMalformed;
        R.Location.BearingBits=SignedRational(V); R.Location.Flags|=8;
      }
    }
  } else {
    if (Count<13 || !Digits(F[6],&R.Quality) || R.Quality>8) return MtkGnssMalformed;
    if ((F[1].N || R.Quality) && !TimeOfDay(F[1],&R)) return MtkGnssMalformed;
    if (F[7].N && !Digits(F[7],&R.SatellitesUsed)) return MtkGnssMalformed;
    if (R.Quality) {
      if (!Coordinate(F[2],F[3],0,&R.Location.LatitudeBits,&R.LatitudeNanominutes,&R.LatitudeStepNanominutes) || !Coordinate(F[4],F[5],1,&R.Location.LongitudeBits,&R.LongitudeNanominutes,&R.LongitudeStepNanominutes)) return MtkGnssMalformed;
      R.Location.Flags=1;
      if (F[8].N) {
        if (!Decimal(F[8],0,&V)) return MtkGnssMalformed;
        R.HdopBits=SignedRational(V); R.AuxFlags|=MTK_GNSS_NMEA_HDOP;
      }
      if (F[9].N) {
        if (F[10].N!=1 || F[10].P[0]!='M' || !Decimal(F[9],1,&Msl)) return MtkGnssMalformed;
        R.MslAltitudeBits=SignedRational(Msl); R.AuxFlags|=MTK_GNSS_NMEA_MSL;
      }
      if (F[11].N) {
        if (F[12].N!=1 || F[12].P[0]!='M' || !Decimal(F[11],1,&Geoid)) return MtkGnssMalformed;
        R.GeoidSeparationBits=SignedRational(Geoid); R.AuxFlags|=MTK_GNSS_NMEA_GEOID;
      }
      if ((R.AuxFlags&6U)==6U) {
        if ((Geoid>0 && Msl>INT64_MAX-Geoid) || (Geoid<0 && Msl<INT64_MIN-Geoid)) return MtkGnssMalformed;
        R.Location.AltitudeBits=SignedRational(Msl+Geoid); R.Location.Flags|=2;
      }
    }
  }
  /* Volatile stores keep freestanding builds independent of compiler memcpy. */
  NmeaCopy(Report,&R,sizeof(R));
  return MtkGnssOk;
}

static unsigned NmeaCentury(unsigned Century) {
  return Century>=1900U && Century<=9900U && Century%100U==0;
}
static unsigned NmeaStreamValid(const MTK_GNSS_NMEA_STREAM *S) {
  return S && S->Initialized==1 && S->Epoch && NmeaCentury(S->Century) &&
    S->PairWindowMs && S->PairWindowMs<=60000U;
}
static unsigned NmeaOverlap(const void *P,unsigned N,const void *Q,unsigned M) {
  uintptr_t A=(uintptr_t)P,B=(uintptr_t)Q;
  if (!A || !B || !N || !M) return 0;
  return A<B?B-A<N:A-B<M;
}
static void NmeaForgetPairs(MTK_GNSS_NMEA_STREAM *S) {
  NmeaZero(&S->Rmc,sizeof(S->Rmc)); NmeaZero(&S->Gga,sizeof(S->Gga));
  S->HaveRmc=S->HaveGga=0; S->RmcAtMs=S->GgaAtMs=0;
}
static void NmeaStreamClear(MTK_GNSS_NMEA_STREAM *S) {
  NmeaForgetPairs(S); NmeaZero(S->Line,sizeof(S->Line));
  S->Used=S->ClockValid=S->HaveChronology=0;
  S->LastSeenMs=S->LineStartMs=S->LastRmcTimeNs=0; S->LastRmcDay=0;
}
MTK_GNSS_STATUS MtkGnssNmeaStreamInit(MTK_GNSS_NMEA_STREAM *S,
  unsigned Century,unsigned PairWindowMs) {
  if (!S || !NmeaCentury(Century) || !PairWindowMs || PairWindowMs>60000U) return MtkGnssInvalid;
  NmeaZero(S,sizeof(*S)); S->Century=Century; S->PairWindowMs=PairWindowMs;
  S->Epoch=S->Initialized=1; return MtkGnssOk;
}
MTK_GNSS_STATUS MtkGnssNmeaStreamReset(MTK_GNSS_NMEA_STREAM *S,
  unsigned Century,unsigned *Epoch) {
  if (!NmeaStreamValid(S) || !Epoch || !NmeaCentury(Century) ||
      NmeaOverlap(S,sizeof(*S),Epoch,sizeof(*Epoch))) return MtkGnssInvalid;
  NmeaStreamClear(S);
  if (S->Epoch==0xffffffffU) { S->Faulted=1; return MtkGnssFaulted; }
  S->Century=Century; S->Epoch++; S->Faulted=0; *Epoch=S->Epoch; return MtkGnssOk;
}
static unsigned NmeaCoordinateMatch(int64_t A,unsigned StepA,int64_t B,unsigned StepB) {
  uint64_t X=A<0?0U-(uint64_t)A:(uint64_t)A,Y=B<0?0U-(uint64_t)B:(uint64_t)B,Diff;
  if (!StepA || !StepB || StepA>1000000000U || StepB>1000000000U) return 0;
  if ((A<0)!=(B<0)) { if (X>UINT64_MAX-Y) return 0; Diff=X+Y; }
  else Diff=X>Y?X-Y:Y-X;
  return Diff<=((uint64_t)StepA+StepB)/2U;
}
static unsigned NmeaPairMatches(const MTK_GNSS_NMEA_STREAM *S) {
  const MTK_GNSS_NMEA *A=&S->Rmc,*B=&S->Gga;
  return S->HaveRmc && S->HaveGga && A->TimeOfDayNanoseconds==B->TimeOfDayNanoseconds &&
    NmeaCoordinateMatch(A->LatitudeNanominutes,A->LatitudeStepNanominutes,B->LatitudeNanominutes,B->LatitudeStepNanominutes) &&
    NmeaCoordinateMatch(A->LongitudeNanominutes,A->LongitudeStepNanominutes,B->LongitudeNanominutes,B->LongitudeStepNanominutes);
}
MTK_GNSS_STATUS MtkGnssNmeaStreamReceive(MTK_GNSS_NMEA_STREAM *S,
  unsigned Epoch,uint64_t NowMs,const unsigned char *Data,unsigned Length,
  unsigned *Consumed,MTK_GNSS_NMEA *Report) {
  unsigned I=0,C; uint64_t At; int64_t Day; MTK_GNSS_STATUS Status; MTK_GNSS_NMEA R;
  if (!Consumed || NmeaOverlap(S,sizeof(*S),Consumed,sizeof(*Consumed)) ||
      NmeaOverlap(Data,Length,Consumed,sizeof(*Consumed)) ||
      NmeaOverlap(Report,sizeof(*Report),Consumed,sizeof(*Consumed)) ||
      NmeaOverlap(S,sizeof(*S),Data,Length) || NmeaOverlap(S,sizeof(*S),Report,sizeof(*Report)) ||
      NmeaOverlap(Data,Length,Report,sizeof(*Report))) return MtkGnssInvalid;
  *Consumed=0;
  if (!NmeaStreamValid(S)) return MtkGnssInvalid;
  if (Epoch!=S->Epoch) return MtkGnssStale;
  if (S->Faulted) return MtkGnssFaulted;
  if (!Report || (!Data && Length)) return MtkGnssInvalid;
  if (S->Used>=MTK_GNSS_NMEA_MAX || S->ClockValid>1 || S->HaveRmc>1 || S->HaveGga>1 || S->HaveChronology>1 ||
      (!S->ClockValid && (S->Used || S->HaveRmc || S->HaveGga || S->HaveChronology)) ||
      (S->ClockValid && NowMs<S->LastSeenMs) || (S->Used && NowMs<S->LineStartMs) ||
      (S->HaveRmc && NowMs<S->RmcAtMs) || (S->HaveGga && NowMs<S->GgaAtMs)) goto Bad;
  S->ClockValid=1; S->LastSeenMs=NowMs;
  if (S->Used && NowMs-S->LineStartMs>S->PairWindowMs) goto Bad;
  if (S->HaveRmc && NowMs-S->RmcAtMs>S->PairWindowMs) {
    NmeaZero(&S->Rmc,sizeof(S->Rmc)); S->HaveRmc=0; S->RmcAtMs=0;
  }
  if (S->HaveGga && NowMs-S->GgaAtMs>S->PairWindowMs) {
    NmeaZero(&S->Gga,sizeof(S->Gga)); S->HaveGga=0; S->GgaAtMs=0;
  }
  while (I<Length && I<MTK_GNSS_NMEA_MAX) {
    C=Data[I++];
    if (!S->Used) {
      if (C=='\r' || C=='\n') continue;
      if (C!='$') goto Bad;
      S->LineStartMs=NowMs;
    } else if (S->Line[S->Used-1U]=='\r') {
      if (C!='\n') goto Bad;
    } else if (C=='\n' || C=='$' || (C!='\r' && (C<32 || C>126))) goto Bad;
    S->Line[S->Used++]=(unsigned char)C;
    if (C!='\n') { if (S->Used==MTK_GNSS_NMEA_MAX) goto Bad; continue; }
    At=S->LineStartMs; Status=MtkGnssNmeaParse(S->Line,S->Used,S->Century,&R);
    NmeaZero(S->Line,sizeof(S->Line)); S->Used=0; S->LineStartMs=0;
    if (Status==MtkGnssUnrelated) { *Consumed=I; return Status; }
    if (Status!=MtkGnssOk) goto Bad;
    if (!(R.Location.Flags&1U)) NmeaForgetPairs(S);
    else if (R.Kind==1) {
      Day=(R.Location.UtcMilliseconds-(int64_t)R.TimeOfDayMilliseconds)/INT64_C(86400000);
      if (S->HaveChronology && (Day<S->LastRmcDay || (Day==S->LastRmcDay && R.TimeOfDayNanoseconds<S->LastRmcTimeNs))) goto Bad;
      S->HaveChronology=1; S->LastRmcDay=Day; S->LastRmcTimeNs=R.TimeOfDayNanoseconds;
      NmeaCopy(&S->Rmc,&R,sizeof(R)); S->RmcAtMs=At; S->HaveRmc=1;
    } else { NmeaCopy(&S->Gga,&R,sizeof(R)); S->GgaAtMs=At; S->HaveGga=1; }
    if (NmeaPairMatches(S)) {
      NmeaCopy(&R,&S->Rmc,sizeof(R)); R.Kind=3;
      R.Location.Flags|=S->Gga.Location.Flags&2U; R.Location.AltitudeBits=S->Gga.Location.AltitudeBits;
      R.AuxFlags|=S->Gga.AuxFlags&(MTK_GNSS_NMEA_MSL|MTK_GNSS_NMEA_GEOID|MTK_GNSS_NMEA_HDOP);
      R.Quality=S->Gga.Quality; R.SatellitesUsed=S->Gga.SatellitesUsed;
      R.HdopBits=S->Gga.HdopBits; R.MslAltitudeBits=S->Gga.MslAltitudeBits; R.GeoidSeparationBits=S->Gga.GeoidSeparationBits;
      NmeaForgetPairs(S);
    }
    NmeaCopy(Report,&R,sizeof(R)); *Consumed=I; return MtkGnssFrameReady;
  }
  *Consumed=I; return MtkGnssNeedMore;
Bad:
  NmeaStreamClear(S); S->Faulted=1; *Consumed=I; return MtkGnssMalformed;
}
