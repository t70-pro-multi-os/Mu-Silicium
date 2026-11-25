// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/HimaxCore.h>
#include "InputReport.h"

int HmBuildHidReport(const HM_FRAME *Frame,const HM_HID_STATE *Previous,
                     HM_HID_REPORT *Report,HM_HID_STATE *Next) {
  HM_HID_REPORT R={0}; HM_HID_STATE N; unsigned I,Active=0,Reported=0;
  if (!Frame || !Previous || !Report || !Next || Frame->Count>HM_MAX_CONTACTS ||
      (Previous->ActiveMask&~((1u<<HM_MAX_CONTACTS)-1u))) return 0;
  N=*Previous; N.ActiveMask=0; R.ReportId=1;
  for (I=0;I<HM_MAX_CONTACTS;I++) {
    const HM_CONTACT *C=&Frame->Contacts[I];
    const HM_HID_CONTACT *Old=&Previous->Last[I];
    int WasActive=(Previous->ActiveMask&(1u<<I))!=0;
    if (WasActive && (Old->ContactId!=I+1 || Old->Flags!=3 ||
                      Old->X>1400 || Old->Y>2240)) return 0;
    if (C->Active) {
      HM_HID_CONTACT *H;
      if (C->X>1400 || C->Y>2240 || Reported>=HM_MAX_CONTACTS) return 0;
      H=&R.Contacts[Reported++]; H->ContactId=(unsigned char)(I+1); H->Flags=3;
      H->X=C->X; H->Y=C->Y; H->Pressure=C->Pressure;
      N.Last[I]=*H; N.ActiveMask|=(unsigned short)(1u<<I); Active++;
    } else if (WasActive) {
      HM_HID_CONTACT *H=&R.Contacts[Reported++];
      H->ContactId=(unsigned char)(I+1); H->Flags=0;
      H->X=Old->X; H->Y=Old->Y; H->Pressure=0;
      N.Last[I]=*H;
    }
  }
  if (Active!=Frame->Count) return 0;
  R.ContactCount=(unsigned char)Reported;
  *Report=R; *Next=N; return 1;
}

int HmBuildMaxContactsFeature(unsigned char *Buffer,unsigned Length) {
  if (!Buffer || Length<2) return 0;
  Buffer[0]=2; Buffer[1]=HM_MAX_CONTACTS; return 1;
}
