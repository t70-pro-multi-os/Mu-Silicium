// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairSerial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
static uint32_t U32(const uint8_t *P){return P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);}
static void Put32(uint8_t *P,uint32_t V){for(unsigned I=0;I<4;I++)P[I]=(uint8_t)(V>>(8*I));}
static void Repair(uint8_t *B,unsigned D){uint32_t O=U32(B+D+36),N=U32(B+D+32),Sum=0;uint8_t Crc=0;
 for(unsigned I=4;I<N;I++)Sum+=B[O+I];Put32(B+O,Sum);
 for(unsigned I=0;I<N;I++){Crc^=B[O+I];for(unsigned J=0;J<8;J++)Crc=(uint8_t)((Crc>>1)^((Crc&1)?0x8c:0));}B[D+40]=Crc;
}
static void Fail(const void *B,size_t N,unsigned Profile){T70_AW_PAIR_SERIAL S,Old;memset(&S,0xa5,sizeof S);Old=S;
 C(!T70AwPairSerialObserve(B,N,0x11e01000,0x34,0x35,Profile,&S));C(!memcmp(&S,&Old,sizeof S));}
int main(int Argc,char **Argv){uint8_t B[3857],Copy[3857];T70_AW_PAIR_SERIAL S;
 C(Argc==2);FILE *F=fopen(Argv[1],"rb");C(F);size_t N=fread(B,1,sizeof B,F);C(!ferror(F));fclose(F);C(N==3856);
 for(unsigned P=0;P<=10;P+=10){C(T70AwPairSerialObserve(B,N,0x11e01000,0x34,0x35,P,&S));C(S.Profile==P);
  for(unsigned Ch=0;Ch<2;Ch++)C(S.Channel[Ch].InputChannel==Ch+1&&S.Channel[Ch].TxChannel==Ch&&S.Channel[Ch].DataBits==32&&S.Channel[Ch].ModeCode==0&&S.Channel[Ch].Reg13==0x2102);
  for(size_t I=0;I<N;I++)Fail(B,I,P);
 }
 unsigned Selected=0;
 for(unsigned D=80;D<80+12*64;D+=64){if(B[D+20]>1||U32(B+D+28)!=4)continue;Selected++;
  unsigned P=B[D+26],O=U32(B+D+36);
  for(unsigned I=0;I<57;I++){unsigned R=O+64+I*4,A=B[R];if(A!=6&&A!=7&&A!=8&&A!=0x13)continue;
   memcpy(Copy,B,N);Copy[R]=0x7f;Repair(Copy,D);Fail(Copy,N,P); /* missing */
   memcpy(Copy,B,N);unsigned Other=O+64+((I+1)%57)*4;Copy[Other]=A;Copy[Other+1]=0;Repair(Copy,D);Fail(Copy,N,P); /* duplicate */
   if(A==6||A==7){memcpy(Copy,B,N);Copy[R+2]=0xff;Copy[R+3]=0xff;Repair(Copy,D);Fail(Copy,N,P);}
  }
  memcpy(Copy,B,N);Copy[O+32]=3;Repair(Copy,D);Fail(Copy,N,P);
 }
 C(Selected==4);Fail(NULL,N,0);Fail(B,N,1);
 C(!T70AwPairSerialObserve(B,N,0,0x34,0x35,0,&S));C(!T70AwPairSerialObserve(B,N,0x11e01000,0x35,0x34,0,&S));
 C(!T70AwPairSerialObserve(B,N,0x11e01000,0x34,0x35,0,NULL));
 memcpy(Copy,B,N);C(!T70AwPairSerialObserve(Copy,N,0x11e01000,0x34,0x35,0,(void *)Copy));C(!memcmp(Copy,B,N));
 C(!T70AwPairSerialObserve(B,SIZE_MAX,0x11e01000,0x34,0x35,0,&S));
 C(T70AwPairSerialObserve(Copy,N,0x11e01000,0x34,0x35,0,&S));T70_AW_PAIR_SERIAL Old=S;memset(Copy,0,sizeof Copy);C(!memcmp(&S,&Old,sizeof S));
 printf("PairSerial: %u checks; copied configuration only\n",Checks);return 0;
}
