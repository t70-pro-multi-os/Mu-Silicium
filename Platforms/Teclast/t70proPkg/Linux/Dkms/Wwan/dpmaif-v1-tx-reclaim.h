/* SPDX-License-Identifier: GPL-2.0 */
#ifndef T70_DPMAIF_V1_TX_RECLAIM_H
#define T70_DPMAIF_V1_TX_RECLAIM_H

#include <linux/atomic.h>
#include <linux/compiler_types.h>

struct drb_queue_info;
struct dpmaif_drb_pd;
struct dpmaif_drb_skb;
struct dpmaif_tx_queue;
struct sk_buff_head;

int t70_dpmaif_v1_tx_reclaim(struct drb_queue_info __iomem *ring,
			    const struct dpmaif_drb_pd __iomem *desc,
			    struct dpmaif_drb_skb *slots, unsigned int capacity,
			    atomic_t *budget, struct sk_buff_head *retired);
int t70_dpmaif_v1_tx_complete(struct dpmaif_tx_queue *txq,
			      struct drb_queue_info __iomem *ring,
			      const struct dpmaif_drb_pd __iomem *desc,
			      struct dpmaif_drb_skb *slots, unsigned int capacity);
#endif
