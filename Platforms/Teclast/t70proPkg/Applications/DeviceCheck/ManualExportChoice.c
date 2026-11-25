#include "ManualExportChoice.h"
void T70ChoiceStart(T70_EXPORT_CHOICE *C,unsigned Count) {
 if (!C) return;
 C->Count=Count; C->Index=0;
 C->State=(Count>0&&Count<=9)?T70ChoiceSelect:T70ChoiceCancelled;
}
void T70ChoiceKey(T70_EXPORT_CHOICE *C,unsigned Key) {
 if (!C) return;
 if (C->State==T70ChoiceSelect) {
  if(Key>='1'&&Key<='9'&&Key-'1'<C->Count) {
   C->Index=Key-'1'; C->State=T70ChoiceConfirm;
  } else C->State=T70ChoiceCancelled;
 } else if(C->State==T70ChoiceConfirm) {
  C->State=(Key=='y'||Key=='Y')?T70ChoiceApproved:T70ChoiceCancelled;
 } else {
  /* Approval is one-shot; repeated keys cannot reuse an old authorization. */
  C->State=T70ChoiceCancelled;
 }
}
