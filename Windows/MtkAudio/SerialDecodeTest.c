// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSerialDecode.h"
#include "AwRegisters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
int main(int Argc,char **Argv){
 uint8_t B[3857];T70_AW_SERIAL_FIELDS S,Old;
 C(Argc==2);FILE *F=fopen(Argv[1],"rb");C(F);size_t N=fread(B,1,sizeof B,F);C(!ferror(F));fclose(F);C(N==3856);
 for(unsigned Ch=0;Ch<2;Ch++)for(unsigned P=0;P<=10;P+=10){
  uint16_t V[4]={0};unsigned Found=0;
  for(unsigned I=0;I<57;I++){T70_AW_REGISTER R;C(T70AwAcfRegister(B,N,Ch,P,I,&R));
   unsigned K=R.Address==6?0:R.Address==7?1:R.Address==8?2:R.Address==0x13?3:4;
   if(K<4){C(!(Found&(1u<<K)));Found|=1u<<K;V[K]=R.Value;}
  }
  C(Found==15);C(T70AwDecodeSerialFields(V[0],V[1],V[2],V[3],&S));
  C(S.InputChannel==Ch+1 && S.ModeCode==0 && S.DataBits==32 && S.BckCode==2 &&
    S.RateCode==8 && S.SlotModeCode==0 && S.TxSlot==0 && S.RxRightSlot==1 &&
    S.RxLeftSlot==0 && S.TxChannel==Ch && S.Reg13==0x2102);
 }
 for(unsigned V=0;V<65536;V++){
  memset(&S,0xa5,sizeof S);Old=S;
  unsigned Ch=(V/1024)%4,Mode=(V/256)%4,Bck=(V/16)%4,Rate=V%16;
  int Valid=Ch!=0 && Mode!=3 && Bck!=3 && Rate<10;
  C(T70AwDecodeSerialFields(V,0x10,0x76,0x2102,&S)==Valid);
  if(!Valid)C(!memcmp(&S,&Old,sizeof S));
  else C(S.Reg06==V && S.InputChannel==Ch && S.ModeCode==Mode &&
    S.DataBits==((unsigned[]){16,20,24,32})[(V/64)%4] && S.BckCode==Bck && S.RateCode==Rate);
  memset(&S,0xa5,sizeof S);Old=S;
  Valid=(V/4096)%8<6 && (V/256)%16<8 && (V/16)%16<8 && V%16<8;
  C(T70AwDecodeSerialFields(0x4e8,V,0x76,0x2102,&S)==Valid);
  if(!Valid)C(!memcmp(&S,&Old,sizeof S));
  else C(S.Reg07==V && S.SlotModeCode==(V/4096)%8 && S.TxSlot==(V/256)%16 &&
    S.RxRightSlot==(V/16)%16 && S.RxLeftSlot==V%16);
  C(T70AwDecodeSerialFields(0x4e8,0x10,V,(uint16_t)~V,&S));
  C(S.Reg08==V && S.Reg13==(uint16_t)~V && S.TxChannel==V%2);
 }
 C(!T70AwDecodeSerialFields(0x4e8,0x10,0x76,0x2102,NULL));
 printf("SerialDecode: %u checks; configuration only\n",Checks);return 0;
}
