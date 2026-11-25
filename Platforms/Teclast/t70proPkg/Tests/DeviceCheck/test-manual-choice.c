#include "ManualExportChoice.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 T70_EXPORT_CHOICE C;
 T70ChoiceStart(&C,2); T70ChoiceKey(&C,'Y'); assert(C.State==T70ChoiceCancelled);
 for(unsigned N=1;N<=9;N++)for(unsigned I=0;I<N;I++){
  T70ChoiceStart(&C,N);T70ChoiceKey(&C,'1'+I);
  assert(C.State==T70ChoiceConfirm&&C.Index==I);
  T70ChoiceKey(&C,'Y');assert(C.State==T70ChoiceApproved);
  T70ChoiceKey(&C,'Y');assert(C.State==T70ChoiceCancelled);
 }
 T70ChoiceStart(&C,1);T70ChoiceKey(&C,'2');assert(C.State==T70ChoiceCancelled);
 T70ChoiceStart(&C,1);T70ChoiceKey(&C,'1');T70ChoiceKey(&C,27);
 assert(C.State==T70ChoiceCancelled);
 T70ChoiceStart(&C,0);assert(C.State==T70ChoiceCancelled);
 T70ChoiceStart(&C,10);assert(C.State==T70ChoiceCancelled);
 puts("PASS: explicit selection, confirmation, cancellation, one-shot authorization");
 return 0;
}
