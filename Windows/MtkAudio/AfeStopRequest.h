// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_STOP_REQUEST_H
#define T70_AFE_STOP_REQUEST_H
#include "AfeDisableRequest.h"
typedef struct {T70_AFE_DISABLE_RESULT Disable;uint32_t Stage,Completed;int32_t Status;} T70_AFE_STOP_RESULT;
/* Normal DL1 period-wakeup STOP subset, exclusive caller-owned domain:
 * memif disable request -> enable observation -> IRQ0 OFF -> IRQ0 clear.
 * Same alias restrictions as DisableRequest. No idle proof, waiting, power,
 * reset, base clearing or buffer release. First error retains all references.
 * Execute under the actual IRQ serialization provider. */
T70_AFE_STOP_RESULT T70AfeDl1RequestStop(const T70_AFE_FORMAT_IO *);
#endif
