/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_STP_PROVIDER_H
#define T70_STP_PROVIDER_H
#include <linux/types.h>

struct device;
struct sk_buff;

typedef int (*t70_stp_rx_fn)(void *ctx, const u8 *data, size_t len, size_t *consumed);

/*
 * Represents the exclusive Bluetooth STP channel-0 stream. Attaching
 * reserves a link but does not power on the controller. The provider owns
 * WMT, transitions/reset and transport buffers. receive() calls must be
 * serialized. A failed open or close must synchronously drain all callbacks
 * before returning. A framing error means the remainder of that input
 * transaction is not a trustworthy frame boundary.
 */
struct t70_stp_bt_link {
	void *priv;
	int (*open)(void *priv, t70_stp_rx_fn receive, void *ctx);
	void (*close)(void *priv);
	void (*flush)(void *priv);
	/* Zero consumes one complete H4 frame; a negative errno consumes none. */
	int (*send)(void *priv, u8 type, const struct sk_buff *body);
	void (*release)(void *priv);
};

int t70_mt6789_stp_attach_v1(struct device *consumer, struct t70_stp_bt_link *link);
#endif
