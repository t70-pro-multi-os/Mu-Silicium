/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_H4_RX_H
#define T70_H4_RX_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stddef.h>
#endif

enum { T70_H4_ACL = 2, T70_H4_SCO = 3, T70_H4_EVENT = 4 };
enum { T70_H4_MAX_ACL = 1024, T70_H4_FRAME_CAP = T70_H4_MAX_ACL + 4 };

struct t70_h4_rx {
	unsigned char frame[T70_H4_FRAME_CAP];
	size_t count, header, total;
	unsigned char type;
};

typedef int (*t70_h4_deliver)(void *, unsigned char, const unsigned char *, size_t);

static inline void t70_h4_reset(struct t70_h4_rx *rx)
{
	rx->count = 0;
	rx->header = 0;
	rx->total = 0;
	rx->type = 0;
}

/* Serialized RX only. Callback sees an HCI body without its leading H4 type. */
static inline int t70_h4_feed(struct t70_h4_rx *rx, const unsigned char *buf,
			      size_t len, size_t *consumed, t70_h4_deliver receive, void *ctx)
{
	*consumed = 0;
	while (*consumed < len) {
		unsigned char byte = buf[(*consumed)++];
		size_t payload, length;
		unsigned char type;
		int ret;

		if (!rx->header) {
			switch (byte) {
			case T70_H4_ACL:
				rx->header = 4;
				break;
			case T70_H4_SCO:
				rx->header = 3;
				break;
			case T70_H4_EVENT:
				rx->header = 2;
				break;
			default:
				t70_h4_reset(rx);
				return -EPROTO;
			}
			rx->type = byte;
			continue;
		}

		rx->frame[rx->count++] = byte;
		if (rx->count == rx->header) {
			switch (rx->type) {
			case T70_H4_ACL:
				payload = rx->frame[2] | ((size_t)rx->frame[3] << 8);
				if (payload > T70_H4_MAX_ACL) {
					t70_h4_reset(rx);
					return -EMSGSIZE;
				}
				break;
			case T70_H4_SCO:
				payload = rx->frame[2];
				break;
			default:
				payload = rx->frame[1];
				break;
			}
			rx->total = rx->header + payload;
		}
		if (!rx->total || rx->count != rx->total)
			continue;
		type = rx->type;
		length = rx->count;
		t70_h4_reset(rx);
		ret = receive(ctx, type, rx->frame, length);
		if (ret)
			return ret;
	}
	return 0;
}

#endif
