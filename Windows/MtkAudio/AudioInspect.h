// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_INSPECT_H
#define T70_AUDIO_INSPECT_H
#include "AfePrepare.h"
#include "AwPairInspect.h"
typedef struct {
  T70_AFE_PREPARED Dl1Values; /* validated input COPY, NOT an AFE observation */
  T70_AW_PAIR_INSPECTION Amplifiers; /* sequential reads + borrowed ACF views */
} T70_AUDIO_INSPECTION;
typedef enum {T70AudioInspectInvalid,T70AudioInspectAmplifiersFailed,
              T70AudioInspectCollected} T70_AUDIO_INSPECT_CODE;
typedef struct {
  T70_AUDIO_INSPECT_CODE Code;
  uint32_t AmplifiersStarted;
  T70_AW_INSPECT_RESULT Amplifiers; /* valid only when Started is nonzero */
} T70_AUDIO_INSPECT_RESULT;
/* Read-only consumer boundary: reject an invalid DL1 snapshot before ANY amp
 * read, then collect the existing pair inspection. Publish both only on success.
 * Deliberately does NOT call T70AfeDl1Configure: that function writes hardware.
 * No AFE read, write, setup or route/format compatibility verdict. Collected is
 * NOT configured/ready and cannot authorize start; these are independent facts.
 * Caller owns immutable snapshot/endpoints and verified serialized read targets.
 * Output must be disjoint from inputs/contexts. ACF must remain alive/immutable
 * for borrowed view lifetime. Failure preserves output, although earlier amp
 * reads may have occurred. No allocation, retry or internal retained pointers. */
T70_AUDIO_INSPECT_RESULT T70AudioInspect(const T70_AFE_PREPARED *Dl1,
 const void *Acf,size_t Bytes,const T70_AW_PROBE_ENDPOINT *First,
 const T70_AW_PROBE_ENDPOINT *Second,uint16_t Profile,T70_AUDIO_INSPECTION *Out);
#endif
