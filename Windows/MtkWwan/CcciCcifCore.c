/* SPDX-License-Identifier: GPL-2.0-only */
/* AP CCIF channel send and interrupt acknowledgement rules from T70 kernel. */
#include "CcciCcifCore.h"
#include <string.h>

enum {
  RegBusy = 0x04U,
  RegTchNum = 0x0cU,
  RegRchNum = 0x10U,
  RegAck = 0x14U,
  RingExceptionBase = 15U,
  DataAckMask = 0x0000ffffU,
  ExceptionAckMask = 0xffffU << RingExceptionBase,
  ValidChannelMask = (1U << T70_CCCI_CCIF_CHANNEL_COUNT) - 1U
};

T70_CCCI_CCIF_STATUS
T70CcciCcifBuildSendPlan (
  uint32_t Channel,
  uint32_t BusyRegister,
  T70_CCCI_CCIF_WRITE *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  )
{
  T70_CCCI_CCIF_WRITE Plan[T70_CCCI_CCIF_MAX_SEND_OPS];
  uint32_t ChannelBit;

  if (Channel >= T70_CCCI_CCIF_CHANNEL_COUNT ||
      Operations == NULL || OperationCount == NULL) {
    return T70CcciCcifInvalid;
  }
  ChannelBit = 1U << Channel;
  if ((BusyRegister & ChannelBit) != 0U) {
    return T70CcciCcifBusy;
  }
  if (OperationCapacity < T70_CCCI_CCIF_MAX_SEND_OPS) {
    return T70CcciCcifNoSpace;
  }
  Plan[0].Offset = RegBusy;
  Plan[0].Value = ChannelBit;
  Plan[1].Offset = RegTchNum;
  Plan[1].Value = Channel;
  memcpy(Operations, Plan, sizeof(Plan));
  *OperationCount = T70_CCCI_CCIF_MAX_SEND_OPS;
  return T70CcciCcifSuccess;
}

T70_CCCI_CCIF_STATUS
T70CcciCcifDecodeInterrupt (
  uint32_t ReceivedChannels,
  T70_CCCI_CCIF_INTERRUPT_KIND Kind,
  T70_CCCI_CCIF_RX *Result
  )
{
  T70_CCCI_CCIF_RX Decoded;
  uint32_t AckMask;

  if (Result == NULL ||
      (Kind != T70CcciCcifDataInterrupt &&
       Kind != T70CcciCcifExceptionInterrupt)) {
    return T70CcciCcifInvalid;
  }
  Decoded.ReceivedChannels = ReceivedChannels & ValidChannelMask;
  if (Decoded.ReceivedChannels == 0U) {
    return T70CcciCcifEmpty;
  }
  AckMask = (Kind == T70CcciCcifDataInterrupt) ?
      DataAckMask : ExceptionAckMask;
  Decoded.AckMask = Decoded.ReceivedChannels & AckMask;
  *Result = Decoded;
  return T70CcciCcifSuccess;
}

T70_CCCI_CCIF_STATUS
T70CcciCcifSend (
  const T70_CCCI_CCIF_IO *Io,
  uint32_t Channel
  )
{
  T70_CCCI_CCIF_WRITE Operations[T70_CCCI_CCIF_MAX_SEND_OPS];
  uint32_t Busy;
  size_t Count=0U,Index;
  T70_CCCI_CCIF_STATUS Status;

  if (Io==NULL || Io->Read32==NULL || Io->Write32==NULL) {
    return T70CcciCcifInvalid;
  }
  if (Io->Read32(Io->Context,RegBusy,&Busy)!=0) {
    return T70CcciCcifIoError;
  }
  Status=T70CcciCcifBuildSendPlan(Channel,Busy,Operations,
      T70_CCCI_CCIF_MAX_SEND_OPS,&Count);
  if (Status!=T70CcciCcifSuccess) {
    return Status;
  }
  for (Index=0U;Index<Count;Index++) {
    if (Io->Write32(Io->Context,Operations[Index].Offset,
        Operations[Index].Value)!=0) {
      return T70CcciCcifIoError;
    }
  }
  return T70CcciCcifSuccess;
}

T70_CCCI_CCIF_STATUS
T70CcciCcifReadAndAcknowledge (
  const T70_CCCI_CCIF_IO *Io,
  T70_CCCI_CCIF_INTERRUPT_KIND Kind,
  T70_CCCI_CCIF_RX *Result
  )
{
  T70_CCCI_CCIF_RX Decoded;
  uint32_t Channels;
  T70_CCCI_CCIF_STATUS Status;

  if (Io==NULL || Io->Read32==NULL || Io->Write32==NULL || Result==NULL ||
      (Kind!=T70CcciCcifDataInterrupt && Kind!=T70CcciCcifExceptionInterrupt)) {
    return T70CcciCcifInvalid;
  }
  if (Io->Read32(Io->Context,RegRchNum,&Channels)!=0) {
    return T70CcciCcifIoError;
  }
  Status=T70CcciCcifDecodeInterrupt(Channels,Kind,&Decoded);
  if (Status!=T70CcciCcifSuccess) {
    return Status;
  }
  if (Io->Write32(Io->Context,RegAck,Decoded.AckMask)!=0) {
    return T70CcciCcifIoError;
  }
  *Result=Decoded;
  return T70CcciCcifSuccess;
}
