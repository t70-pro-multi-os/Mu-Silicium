/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_SCP_MOTION_H
#define T70_SCP_MOTION_H
#include <linux/types.h>

struct device;
typedef int (*t70_motion_sample_fn)(void *ctx, const s32 xyz[3], s64 boottime_ns);

/*
 * A real, exclusive SCP accelerometer session. Scale is metres/s^2 per
 * signed count. Timestamp is validated host CLOCK_BOOTTIME at acquisition,
 * not delivery time. Calls to sample are serialized, including during
 * start; failed start and stop must drain callbacks before returning.
 * No caller can safely keep a sample callback past release.
 */
struct t70_motion_link {
	void *priv;
	int scale_num;
	int scale_den;
	int (*start)(void *priv, t70_motion_sample_fn sample, void *ctx);
	void (*stop)(void *priv);
	void (*release)(void *priv);
};

int t70_scp_motion_attach_v1(struct device *dev, struct t70_motion_link *link);
#endif
