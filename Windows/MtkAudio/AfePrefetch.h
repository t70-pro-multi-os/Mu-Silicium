// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_PREFETCH_H
#define T70_AFE_PREFETCH_H
#include "AfePrepare.h"
#include "AfeFormatUpdate.h"
/* Normal stock DL1 prepare subset: pbuf then minlen. No CUR write/rewind,
 * power, start, DMA-idle test or allocation release. Caller owns disabled
 * exclusive hardware. First callback error stops; partial writes remain. */
int32_t T70AfeDl1PreparePrefetch(const T70_AFE_PREPARED *,const T70_AFE_FORMAT_IO *);
#endif
