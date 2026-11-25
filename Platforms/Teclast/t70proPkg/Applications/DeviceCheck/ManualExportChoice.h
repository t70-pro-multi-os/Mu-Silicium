#ifndef T70_MANUAL_EXPORT_CHOICE_H
#define T70_MANUAL_EXPORT_CHOICE_H
typedef enum { T70ChoiceIdle, T70ChoiceSelect, T70ChoiceConfirm,
               T70ChoiceApproved, T70ChoiceCancelled } T70_CHOICE_STATE;
typedef struct { T70_CHOICE_STATE State; unsigned Count, Index; } T70_EXPORT_CHOICE;
void T70ChoiceStart(T70_EXPORT_CHOICE *, unsigned Count);
void T70ChoiceKey(T70_EXPORT_CHOICE *, unsigned Key);
#endif
