/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_GPIO_HARDWARE_H
#define MTK_GPIO_HARDWARE_H
#include <stdint.h>
#define MG_PINS 188U
#define MG_TOTAL_PINS 189U
#define MG_IRQ_PINS 126U
#define MG_PMIC_IRQ_PIN 188U
#define MG_PMIC_IRQ_BANK 5U
#define MG_PMIC_IRQ_NUMBER 28U
#define MG_PMIC_EINT_BANK 4U
#define MG_PMIC_EINT_BIT (1U<<9)
#define MG_BANKS 6U
#define MG_MMIO_LENGTH 0x1000U
#define MG_SPACES 8U
#define MG_CONN_PIN_FIELDS 8U
#include "PinMap.h"
/* Same values as the documented GpioClx pull configuration constants. */
typedef enum { MgPullDefault, MgPullUp, MgPullDown, MgPullNone } MG_PULL;
/* GPIO/EINT MMIO and bank serialization belong to the caller. */
typedef enum { MgOk, MgInvalid, MgNotReady, MgBusy, MgUnsupported, MgIoError } MG_STATUS;
typedef struct {
  void *Context;
  MG_STATUS (*Read)(void *,unsigned,unsigned,uint32_t *);
  MG_STATUS (*Write)(void *,unsigned,unsigned,uint32_t);
} MG_IO;
typedef struct {
  uint8_t Mode,Direction,Output,Kind,Saved,IrqSaved,Sensitivity,Polarity,Pad,IoAlias,Pull;
  uint8_t SharedIndex,SharedTarget,ContextValid,ContextMode,ContextDirection;
  uint8_t ContextOutput,ContextPad,ContextSchmitt,ContextDrive,ContextRsel,ContextEh;
  uint8_t ContextSensitivity,ContextPolarity,ContextUnmasked;
} MG_PIN;
typedef struct {
  MG_FIELD Field;
  uint8_t Baseline,Target,Users,Valid;
} MG_SHARED_FIELD;
/* Logical pin 188 is an EINT-only route for the PMIC parent. It has no GPIO
 * pad, direction, output latch, or entry in Pin[]. */
typedef struct {
  uint8_t Saved,Sensitivity,Polarity,Masked;
  uint8_t ContextValid,ContextSensitivity,ContextPolarity,ContextUnmasked;
} MG_VIRTUAL_IRQ;
typedef struct {
  MG_IO Io;
  uint32_t Signature,Owned[MG_BANKS];
  MG_PIN Pin[MG_PINS];
  MG_SHARED_FIELD Shared[MG_PINS];
  MG_VIRTUAL_IRQ PmicInterrupt;
  uint32_t ConnPinBaseline[MG_CONN_PIN_FIELDS];
  unsigned SharedCount;
  int Active,Suspended,ConnPinsOwned;
} MG_CORE;
MG_STATUS MgInitialize(MG_CORE *,const MG_IO *);
MG_STATUS MgStart(MG_CORE *);
MG_STATUS MgStop(MG_CORE *);
MG_STATUS MgSuspend(MG_CORE *);
/* Caller supplies OS-armed wake pins and guarantees powered MMIO for both
 * transitions. NULL masks all owned IRQs. Unowned EINT lines are untouched. */
MG_STATUS MgSuspendWake(MG_CORE *,const uint32_t [MG_BANKS]);
MG_STATUS MgResume(MG_CORE *);
/* Restores only EINT masks after the retained domains and pads match. */
MG_STATUS MgResumeRetained(MG_CORE *);
/* MT6789 board LNA pins 25/26 only. The caller owns GPS/RF power and serializes
 * this core. Acquire is read-only; release with MgDisconnect(...,0). Stop also
 * restores the inherited pin state. Quiesce/release before GPIO suspend. */
typedef enum { MgGpsLnaOff, MgGpsLnaDsp, MgGpsLnaOn } MG_GPS_LNA_STATE;
MG_STATUS MgGpsLnaAcquire(MG_CORE *,unsigned);
MG_STATUS MgGpsLnaSet(MG_CORE *,unsigned,MG_GPS_LNA_STATE);
/* Board CONN GPIO68 and GPIO177–185. Serialize with ordinary pin changes. */
int MgConnPinsOwns(const MG_CORE *,unsigned);
MG_STATUS MgConnPinsAcquire(MG_CORE *);
MG_STATUS MgConnPinsRead(MG_CORE *,unsigned,unsigned,uint32_t *);
MG_STATUS MgConnPinsWrite(MG_CORE *,unsigned,unsigned,uint32_t);
MG_STATUS MgConnPinsRelease(MG_CORE *);
MG_STATUS MgConnect(MG_CORE *,unsigned,int);
MG_STATUS MgConnectEx(MG_CORE *,unsigned,int,MG_PULL);
MG_STATUS MgDisconnect(MG_CORE *,unsigned,int);
MG_STATUS MgDisconnectInputOutput(MG_CORE *,unsigned,int);
MG_STATUS MgDisableInterrupt(MG_CORE *,unsigned);
MG_STATUS MgEnable(MG_CORE *,unsigned,int,int);
MG_STATUS MgEnableEx(MG_CORE *,unsigned,int,int,MG_PULL);
MG_STATUS MgConfigureInterrupt(MG_CORE *,unsigned,int,int);
MG_STATUS MgMask(MG_CORE *,unsigned,uint32_t);
MG_STATUS MgAck(MG_CORE *,unsigned,uint32_t);
MG_STATUS MgUnmask(MG_CORE *,unsigned);
MG_STATUS MgQuery(MG_CORE *,unsigned,uint32_t,uint32_t *,uint32_t *);
MG_STATUS MgRead(MG_CORE *,unsigned,int,uint32_t *);
MG_STATUS MgWrite(MG_CORE *,unsigned,uint32_t,uint32_t);
uint32_t MgValidMask(unsigned);
#endif
