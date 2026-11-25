// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkThermalCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static unsigned Checks;
#define CHECK(x) do { ++Checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while(0)
static unsigned Be32(const unsigned char *P) {
  return (unsigned)P[0]<<24 | (unsigned)P[1]<<16 | (unsigned)P[2]<<8 | P[3];
}
static int Signed32(const unsigned char *P) {
  unsigned V=Be32(P);
  return V<=INT_MAX ? (int)V : -1-(int)(~V);
}
int main(int argc, char **argv) {
  FILE *F; unsigned char *B; long Size; unsigned P, Strings, Limit;
  const unsigned char *Tables[2]={0}; unsigned Ntc=0;
  const unsigned Count=166;
  MTK_THERMAL_SAMPLE S; unsigned D=0; int Prev=INT_MAX;
  CHECK(argc==2); F=fopen(argv[1],"rb"); CHECK(F!=0);
  CHECK(fseek(F,0,SEEK_END)==0); Size=ftell(F); CHECK(Size>=40);
  rewind(F); B=malloc((size_t)Size); CHECK(B!=0);
  CHECK(fread(B,1,(size_t)Size,F)==(size_t)Size); fclose(F);
  CHECK(Be32(B)==0xd00dfeed); P=Be32(B+8); Strings=Be32(B+12);
  Limit=P+Be32(B+36); CHECK(Limit<=(unsigned long)Size);
  CHECK(Strings+Be32(B+32)<=(unsigned long)Size);
  while(P+4<=Limit) {
    unsigned Token=Be32(B+P); P+=4;
    if(Token==1) {
      unsigned End=P; while(End<Limit && B[End]) ++End;
      CHECK(End<Limit);
      Ntc=!strcmp((char*)B+P,"thermal-ntc1") ? 1 : !strcmp((char*)B+P,"thermal-ntc2") ? 2 : 0;
      P=(End+4)&~3U;
    } else if(Token==2) { Ntc=0;
    } else if(Token==3) {
      unsigned Len, Name; CHECK(P+8<=Limit);
      Len=Be32(B+P); Name=Be32(B+P+4); P+=8;
      CHECK(Len<=Limit-P); CHECK(Name<Be32(B+32));
      CHECK(memchr(B+Strings+Name,0,Be32(B+32)-Name)!=0);
      if(Ntc && !strcmp((char*)B+Strings+Name,"temperature-lookup-table")) {
        CHECK(Len==Count*8 && Tables[Ntc-1]==0);
        Tables[Ntc-1]=B+P;
      }
      P=(P+Len+3)&~3U;
    } else if(Token==9) break;
    else CHECK(Token==4);
  }
  CHECK(Tables[0]!=0 && Tables[1]!=0);
  CHECK(memcmp(Tables[0],Tables[1],Count*8)==0);
  const unsigned char *Table=Tables[0];
  for(unsigned I=0; I<Count; ++I) {
    CHECK(Signed32(Table+8*I)==-40000+(int)I*1000);
    CHECK(MtkThermalApNtcConvert(Signed32(Table+8*I+4),&S));
    CHECK(S.MilliCelsius==Signed32(Table+8*I)); CHECK(S.Clamped==0);
  }
  /* Independent quotient/remainder expansion from stock ARM64 0x2bc..2d0,
   * rather than copying production's multiply-before-divide expression. */
  for(int Adc=-100; Adc<=2000; ++Adc) {
    unsigned I=0; int Expected;
    while(I<Count && Signed32(Table+8*I+4)>Adc) ++I;
    if(I==0) Expected=Signed32(Table);
    else if(I==Count) Expected=Signed32(Table+8*(Count-1));
    else {
      int T0=Signed32(Table+8*(I-1)), T1=Signed32(Table+8*I);
      int A0=Signed32(Table+8*(I-1)+4), A1=Signed32(Table+8*I+4);
      int Q=(T1-T0)/(A1-A0), R=(T1-T0)%(A1-A0);
      Expected=T0+Q*(Adc-A0)+R*(Adc-A0)/(A1-A0);
    }
    CHECK(MtkThermalApNtcConvert(Adc,&S)); CHECK(S.MilliCelsius==Expected);
    CHECK(S.MilliCelsius<=Prev); Prev=S.MilliCelsius;
    CHECK(S.Clamped==(Adc<44 ? -1 : Adc>1760 ? 1 : 0));
  }
  CHECK(MtkThermalApNtcConvert(INT_MIN,&S)); CHECK(S.MilliCelsius==125000 && S.Clamped==-1);
  CHECK(MtkThermalApNtcConvert(INT_MAX,&S)); CHECK(S.MilliCelsius==-40000 && S.Clamped==1);
  CHECK(!MtkThermalApNtcConvert(100,0));
  for(int T=-40000; T<=125000; ++T) {
    CHECK(MtkThermalToAcpi(T,&D));
    CHECK((long)D*100-(T+273150)>=-49 && (long)D*100-(T+273150)<=50);
  }
  CHECK(MtkThermalToAcpi(-40000,&D) && D==2332);
  CHECK(MtkThermalToAcpi(0,&D) && D==2732);
  CHECK(MtkThermalToAcpi(25000,&D) && D==2982);
  D=123; CHECK(!MtkThermalToAcpi(INT_MIN,&D) && D==123);
  CHECK(!MtkThermalToAcpi(INT_MAX,&D) && D==123);
  CHECK(!MtkThermalToAcpi(0,0));
  unsigned Words[MTK_LVTS_EFUSE_WORDS] = {0};
  MTK_LVTS_CALIBRATION Calibration;
  Words[0] = 0x12abcd;
  Words[7] = 0xabcd0000;
  Words[11] = 0xab000000;
  CHECK(MtkThermalLvtsCalibration(Words, &Calibration));
  CHECK(Calibration.Golden == 60);
  for (unsigned Index = 0; Index < MTK_LVTS_SENSOR_COUNT; ++Index)
    CHECK(Calibration.CountR[Index] == 0x88b8);
  Words[11] |= 0x100;
  CHECK(MtkThermalLvtsCalibration(Words, &Calibration) && Calibration.Golden == 0);
  CHECK(Calibration.CountR[0] == 0);
  Words[11] = 0;
  for (unsigned Index = 0; Index < MTK_LVTS_SENSOR_COUNT; ++Index)
    Words[1 + Index / 2] |= (0x100 + Index) << (16 * (Index % 2));
  CHECK(MtkThermalLvtsCalibration(Words, &Calibration) && Calibration.Golden == 0);
  Words[0] |= 64U << 24;
  CHECK(MtkThermalLvtsCalibration(Words, &Calibration) && Calibration.Golden == 64);
  for (unsigned Index = 0; Index < MTK_LVTS_SENSOR_COUNT; ++Index)
    CHECK(Calibration.CountR[Index] == 0x100 + Index);
  for (unsigned Index = 0; Index < MTK_LVTS_EFUSE_WORDS; ++Index) {
    unsigned Saved = Words[Index];
    Words[Index] = UINT_MAX;
    CHECK(!MtkThermalLvtsCalibration(Words, &Calibration));
    CHECK(Calibration.Golden == 64 && Calibration.CountR[12] == 0x10c);
    Words[Index] = Saved;
  }
  CHECK(!MtkThermalLvtsCalibration(NULL, &Calibration));
  CHECK(!MtkThermalLvtsCalibration(Words, NULL));
  free(B); printf("MtkThermalCoreTest: %u checks PASS\n",Checks); return 0;
}
