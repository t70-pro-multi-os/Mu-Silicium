// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <string.h>
#include "InputReport.h"
int main(void) {
  HM_FRAME F={0}; HM_HID_REPORT R,Before; HM_HID_STATE State={0},Next={0},BeforeState;
  unsigned char Feature[2]={0},ShortFeature[1]={0xa5};
  F.Count=2; F.Contacts[0]=(HM_CONTACT){1400,2240,255,1};
  F.Contacts[7]=(HM_CONTACT){1,2,80,1};
  assert(HmBuildHidReport(&F,&State,&R,&Next));
  assert(R.ReportId==1 && R.ContactCount==2 && Next.ActiveMask==0x81);
  assert(R.Contacts[0].ContactId==1 && R.Contacts[1].ContactId==8);
  assert(R.Contacts[0].Flags==3 && R.Contacts[0].X==1400 && R.Contacts[0].Y==2240 && R.Contacts[0].Pressure==255);
  assert(R.Contacts[1].Flags==3 && R.Contacts[1].X==1 && R.Contacts[1].Y==2 && R.Contacts[1].Pressure==80);
  assert(R.Contacts[2].Flags==0 && R.Contacts[9].Flags==0);
  State=Next;

  memset(&F,0,sizeof(F)); F.Count=1; F.Contacts[7]=(HM_CONTACT){2,3,20,1};
  assert(HmBuildHidReport(&F,&State,&R,&Next));
  assert(R.ReportId==1 && R.ContactCount==2 && Next.ActiveMask==0x80);
  assert(R.Contacts[0].ContactId==1 && R.Contacts[0].Flags==0);
  assert(R.Contacts[0].X==1400 && R.Contacts[0].Y==2240 && R.Contacts[0].Pressure==0);
  assert(R.Contacts[1].ContactId==8 && R.Contacts[1].Flags==3);
  State=Next;

  memset(&F,0,sizeof(F));
  assert(HmBuildHidReport(&F,&State,&R,&Next));
  assert(R.ContactCount==1 && Next.ActiveMask==0);
  assert(R.Contacts[0].ContactId==8 && R.Contacts[0].Flags==0);
  assert(R.Contacts[0].X==2 && R.Contacts[0].Y==3 && R.Contacts[0].Pressure==0);
  State=Next;
  assert(HmBuildHidReport(&F,&State,&R,&Next) && R.ContactCount==0);

  memset(&R,0xA5,sizeof(R)); Before=R; BeforeState=State; Next=State;
  F.Count=1; F.Contacts[0]=(HM_CONTACT){1401,0,1,1};
  assert(!HmBuildHidReport(&F,&State,&R,&Next));
  assert(memcmp(&R,&Before,sizeof(R))==0 && memcmp(&Next,&State,sizeof(State))==0);
  F.Contacts[0]=(HM_CONTACT){0,0,1,0};
  assert(!HmBuildHidReport(&F,&State,&R,&Next));
  assert(memcmp(&R,&Before,sizeof(R))==0 && memcmp(&Next,&State,sizeof(State))==0);
  assert(!HmBuildHidReport(NULL,&State,&R,&Next));
  assert(memcmp(&State,&BeforeState,sizeof(State))==0);
  assert(HmBuildMaxContactsFeature(Feature,sizeof(Feature)) && Feature[0]==2 && Feature[1]==10);
  assert(!HmBuildMaxContactsFeature(ShortFeature,sizeof(ShortFeature)) && ShortFeature[0]==0xa5);
  return 0;
}
