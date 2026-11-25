// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef HIMAX_INPUT_REPORT_H
#define HIMAX_INPUT_REPORT_H
#include <Library/HimaxCore.h>
#pragma pack(push,1)
typedef struct {
  unsigned char ContactId, Flags;
  unsigned short X, Y;
  unsigned char Pressure;
} HM_HID_CONTACT;
typedef struct {
  unsigned char ReportId, ContactCount;
  HM_HID_CONTACT Contacts[HM_MAX_CONTACTS];
} HM_HID_REPORT;
#pragma pack(pop)
typedef struct {
  unsigned short ActiveMask;
  HM_HID_CONTACT Last[HM_MAX_CONTACTS];
} HM_HID_STATE;
typedef char HM_HID_REPORT_SIZE_MUST_BE_72[(sizeof(HM_HID_REPORT)==72)?1:-1];
int HmBuildHidReport(const HM_FRAME *Frame,const HM_HID_STATE *Previous,
                     HM_HID_REPORT *Report,HM_HID_STATE *Next);
int HmBuildMaxContactsFeature(unsigned char *Buffer,unsigned Length);
#endif
