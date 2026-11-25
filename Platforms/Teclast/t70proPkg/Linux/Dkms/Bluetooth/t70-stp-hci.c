// SPDX-License-Identifier: GPL-2.0-only
/* BlueZ HCI consumer; transport, power and reset remain with the provider. */
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "t70-h4-rx.h"
#include "t70-stp-provider.h"

#define T70_TX_DEPTH 64
#define T70_TX_RETRY_MS 20
#define T70_TX_RETRY_WINDOW_MS 1000

struct t70_hci {
	struct mutex lock;
	struct t70_stp_bt_link link;
	struct t70_h4_rx rx;
	struct hci_dev *hdev;
	struct delayed_work tx_work;
	struct sk_buff_head tx;
	bool opened;
	bool tx_retrying;
	unsigned long tx_retry_deadline;
};

static int t70_deliver(void *ctx, unsigned char type, const unsigned char *data, size_t len)
{
	struct t70_hci *bt = ctx;
	struct sk_buff *skb = bt_skb_alloc(len, GFP_ATOMIC);

	/* Only an H4 framing error invalidates the rest of a transaction. */
	if (!skb) {
		bt->hdev->stat.err_rx++;
		return 0;
	}
	skb_put_data(skb, data, len);
	hci_skb_pkt_type(skb) = type;
	/* The HCI core consumes skb even when it rejects this complete frame. */
	if (hci_recv_frame(bt->hdev, skb))
		bt->hdev->stat.err_rx++;
	return 0;
}

static int t70_receive(void *ctx, const u8 *data, size_t len, size_t *used)
{
	struct t70_hci *bt = ctx;

	return t70_h4_feed(&bt->rx, data, len, used, t70_deliver, bt);
}

static void t70_tx_work(struct work_struct *work)
{
	struct t70_hci *bt = container_of(to_delayed_work(work), struct t70_hci, tx_work);
	struct sk_buff *skb;
	int ret;

	mutex_lock(&bt->lock);
	while (bt->opened && (skb = skb_peek(&bt->tx))) {
		if (bt->tx_retrying && time_after_eq(jiffies, bt->tx_retry_deadline))
			ret = -ETIMEDOUT;
		else
			ret = bt->link.send(bt->link.priv, hci_skb_pkt_type(skb), skb);
		if (ret == -EAGAIN || ret == -ENOSPC) {
			if (!bt->tx_retrying) {
				bt->tx_retrying = true;
				bt->tx_retry_deadline = jiffies +
					msecs_to_jiffies(T70_TX_RETRY_WINDOW_MS);
			}
			queue_delayed_work(system_dfl_wq, &bt->tx_work,
					   msecs_to_jiffies(T70_TX_RETRY_MS));
			break;
		}
		/* The next head owns a distinct retry window. */
		bt->tx_retrying = false;
		if (ret)
			bt->hdev->stat.err_tx++;
		else
			bt->hdev->stat.byte_tx += skb->len + 1;
		kfree_skb(skb_dequeue(&bt->tx));
	}
	mutex_unlock(&bt->lock);
}

static int t70_open(struct hci_dev *hdev)
{
	struct t70_hci *bt = hci_get_drvdata(hdev);
	int ret = 0;

	mutex_lock(&bt->lock);
	if (!bt->opened) {
		t70_h4_reset(&bt->rx);
		ret = bt->link.open(bt->link.priv, t70_receive, bt);
		bt->opened = !ret;
	}
	mutex_unlock(&bt->lock);
	return ret;
}

static int t70_close(struct hci_dev *hdev)
{
	struct t70_hci *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	if (bt->opened) {
		bt->opened = false;
		bt->link.close(bt->link.priv);
		t70_h4_reset(&bt->rx);
	}
	skb_queue_purge(&bt->tx);
	bt->tx_retrying = false;
	mutex_unlock(&bt->lock);
	cancel_delayed_work_sync(&bt->tx_work);
	return 0;
}

static int t70_flush(struct hci_dev *hdev)
{
	struct t70_hci *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	if (bt->opened)
		bt->link.flush(bt->link.priv);
	skb_queue_purge(&bt->tx);
	bt->tx_retrying = false;
	mutex_unlock(&bt->lock);
	return 0;
}

static int t70_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct t70_hci *bt = hci_get_drvdata(hdev);
	u8 type = hci_skb_pkt_type(skb);
	int ret = 0;

	if (type != HCI_COMMAND_PKT && type != HCI_ACLDATA_PKT && type != HCI_SCODATA_PKT)
		return -EOPNOTSUPP;

	mutex_lock(&bt->lock);
	if (!bt->opened)
		ret = -ENETDOWN;
	else if (skb_queue_len(&bt->tx) >= T70_TX_DEPTH)
		ret = -ENOBUFS;
	else {
		skb_queue_tail(&bt->tx, skb);
		queue_delayed_work(system_dfl_wq, &bt->tx_work, 0);
	}
	mutex_unlock(&bt->lock);
	return ret;
}

static int t70_hci_probe(struct platform_device *pdev)
{
	typeof(&t70_mt6789_stp_attach_v1) attach;
	struct hci_dev *hdev;
	struct t70_hci *bt;
	int ret;

	if (!pdev->dev.of_node ||
	    !of_device_is_compatible(pdev->dev.of_node, "teclast,t70pro-mt6789-stp-hci") ||
	    !of_device_is_available(pdev->dev.of_node))
		return -ENODEV;
	attach = symbol_get(t70_mt6789_stp_attach_v1);
	if (!attach)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER, "T70 STP provider unavailable\n");
	hdev = hci_alloc_dev_priv(sizeof(*bt));
	if (!hdev) {
		ret = -ENOMEM;
		goto put;
	}
	bt = hci_get_priv(hdev);
	bt->hdev = hdev;
	mutex_init(&bt->lock);
	INIT_DELAYED_WORK(&bt->tx_work, t70_tx_work);
	skb_queue_head_init(&bt->tx);
	ret = attach(&pdev->dev, &bt->link);
	if (ret)
		goto free;
	if (!bt->link.open || !bt->link.close || !bt->link.flush ||
	    !bt->link.send || !bt->link.release) {
		ret = -EPROTO;
		goto release;
	}

	hdev->bus = HCI_VIRTUAL; /* software HCI over proprietary STP, not UART/SDIO */
	hdev->open = t70_open;
	hdev->close = t70_close;
	hdev->flush = t70_flush;
	hdev->send = t70_send;
	hci_set_drvdata(hdev, bt);
	SET_HCIDEV_DEV(hdev, &pdev->dev);
	ret = hci_register_dev(hdev);
	if (ret < 0)
		goto release;
	platform_set_drvdata(pdev, hdev);
	return 0;

release:
	if (bt->link.release)
		bt->link.release(bt->link.priv);
free:
	hci_free_dev(hdev);
put:
	symbol_put(t70_mt6789_stp_attach_v1);
	return ret;
}

static void t70_hci_remove(struct platform_device *pdev)
{
	struct hci_dev *hdev = platform_get_drvdata(pdev);
	struct t70_hci *bt = hci_get_priv(hdev);

	hci_unregister_dev(hdev);
	t70_close(hdev);
	bt->link.release(bt->link.priv);
	hci_free_dev(hdev);
	symbol_put(t70_mt6789_stp_attach_v1);
}

static const struct of_device_id t70_stp_match[] = {
	{ .compatible = "teclast,t70pro-mt6789-stp-hci" },
	{ }
};
MODULE_DEVICE_TABLE(of, t70_stp_match);

static struct platform_driver t70_stp_driver = {
	.probe = t70_hci_probe,
	.remove = t70_hci_remove,
	.driver = {
		.name = "t70-stp-hci",
		.of_match_table = t70_stp_match,
	},
};
module_platform_driver(t70_stp_driver);
MODULE_DESCRIPTION("T70 Pro Bluetooth HCI consumer for an owned STP link");
MODULE_LICENSE("GPL");
