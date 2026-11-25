// SPDX-License-Identifier: GPL-2.0
/* Software-only shared-DRB completion for the MT6789 network transport. */
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include "ccci_hif_dpmaif_v1.h"
#include "t70-ccci-net-tx-notify.h"
#include "dpmaif-v1-ring-geometry.h"
#include "dpmaif-v1-tx-reclaim.h"

/*
 * CCCI's SMEM_USER_MD_DATA is exposed as a write-combined __iomem area.
 * The owner supplies the bounded map and serializes this call with submit/close.
 * Keep retired private until unlock, then free its skbs and notify netdevs.
 * This function accesses only shared DRB memory; no registers or callbacks.
 */
int t70_dpmaif_v1_tx_reclaim(struct drb_queue_info __iomem *ring,
			    const struct dpmaif_drb_pd __iomem *desc,
			    struct dpmaif_drb_skb *slots, unsigned int capacity,
			    atomic_t *budget, struct sk_buff_head *retired)
{
	unsigned int rd, wr, rel, nr, i, index;
	u32 first, type;
	int free_slots;

	if (!ring || !desc || !slots || !budget || !retired ||
	    capacity < 2 || capacity > DPMA_UL_Q0_SIZE ||
	    readl(&ring->sz) != capacity)
		return -EINVAL;

	rd = readl(&ring->rd);
	wr = readl(&ring->wr);
	rel = readl(&ring->rel);
	if (rd >= capacity || wr >= capacity || rel >= capacity)
		return -EIO;

	nr = ringbuf_releasable(capacity, rel, rd);
	free_slots = atomic_read(budget);
	if (nr > ringbuf_releasable(capacity, rel, wr) ||
	    free_slots < 0 || free_slots > capacity - nr)
		return -EIO;


	/* Decode shared words once. Retirement ownership is host metadata,
	 * protected by tx_lock together with the producer.
	 */
	index = rel;
	for (i = 0; i < nr; i++) {
		struct dpmaif_drb_skb *slot = &slots[index];
		bool msg, last;

		first = readl((const void __iomem *)&desc[index]);
		type = first & GENMASK(1, 0);
		if (type != DES_DTYP_PD && type != DES_DTYP_MSG)
			return -EIO;
		msg = type == DES_DTYP_MSG;
		last = !msg && !(first & BIT(2));
		if (slot->is_msg != msg || slot->is_last_one != last ||
		    (last && (!slot->skb || slot->skb->next)))
			return -EIO;
		index = ringbuf_get_next_idx(capacity, index, 1);
	}
	index = rel;
	for (i = 0; i < nr; i++) {
		struct dpmaif_drb_skb *slot = &slots[index];

		/* The queue sets next, so even duplicate host aliases retire once.
		 * Clear borrowed message/fragment aliases before the skb is freed.
		 */
		if (slot->is_last_one && !slot->skb->next)
			__skb_queue_tail(retired, slot->skb);
		slot->skb = NULL;
		index = ringbuf_get_next_idx(capacity, index, 1);
	}
	if (nr) {
		writel(rd, &ring->rel);
		/* Order the shared-map write before advertising space to the AP. */
		readl(&ring->rel);
		atomic_add(nr, budget);
	}
	return nr;
}

/*
 * Called by the shared-ring completion worker, not an ISR. The owner must
 * quiesce that worker before CCCI teardown or ring unmap. The same tx_lock
 * protects submit and close's que_started transition. CCCI callbacks run
 * outside it because they acquire the network port's flag_lock.
 */
int t70_dpmaif_v1_tx_complete(struct dpmaif_tx_queue *txq,
			      struct drb_queue_info __iomem *ring,
			      const struct dpmaif_drb_pd __iomem *desc,
			      struct dpmaif_drb_skb *slots, unsigned int capacity)
{
	struct sk_buff_head retired;
	struct sk_buff *skb;
	unsigned long flags;
	bool may_wake;
	int nr;

	if (!txq || txq->index > 1)
		return -EINVAL;

	__skb_queue_head_init(&retired);
	spin_lock_irqsave(&txq->tx_lock, flags);
	if (!READ_ONCE(txq->que_started)) {
		spin_unlock_irqrestore(&txq->tx_lock, flags);
		return -ENETDOWN;
	}
	nr = t70_dpmaif_v1_tx_reclaim(ring, desc, slots, capacity,
				    &txq->tx_budget, &retired);
	may_wake = nr > 0 && atomic_read(&txq->tx_budget) > capacity / 8;
	spin_unlock_irqrestore(&txq->tx_lock, flags);

	while ((skb = __skb_dequeue(&retired)))
		ccci_free_skb(skb);

	/* The original shared-ring policy wakes above 1/8 capacity. */
	if (may_wake && READ_ONCE(txq->que_started) &&
	    (ccci_md_get_cap_by_id(MD_SYS1) & MODEM_CAP_TXBUSY_STOP))
		t70_ccci_net_tx_notify(txq->index, TX_IRQ);

	return nr;
}
