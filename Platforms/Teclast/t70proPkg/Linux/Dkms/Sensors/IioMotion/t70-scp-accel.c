// SPDX-License-Identifier: GPL-2.0-only
/* Standard IIO consumer for an independently owned T70 SCP accelerometer. */
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "t70-scp-motion.h"

struct t70_scan {
	s32 xyz[3];
	/* IIO copies the aligned gap when a timestamp is requested. */
	u32 zero_pad;
	s64 timestamp __aligned(8);
};
static_assert(offsetof(struct t70_scan, zero_pad) == 12);
static_assert(offsetof(struct t70_scan, timestamp) == 16);
static_assert(sizeof(struct t70_scan) == 24);

struct t70_accel {
	struct t70_motion_link link;
	bool started;
	bool attached;
};

#define T70_ACCEL_AXIS(axis, idx) { \
	.type = IIO_ACCEL, .modified = 1, .channel2 = IIO_MOD_##axis, \
	.scan_index = idx, .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.scan_type = { .sign = 's', .realbits = 32, .storagebits = 32, \
		       .endianness = IIO_CPU }, \
}

static const struct iio_chan_spec t70_channels[] = {
	T70_ACCEL_AXIS(X, 0),
	T70_ACCEL_AXIS(Y, 1),
	T70_ACCEL_AXIS(Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static const unsigned long t70_scan_masks[] = { BIT(0) | BIT(1) | BIT(2), 0 };

static int t70_sample(void *ctx, const s32 xyz[3], s64 boottime_ns)
{
	struct iio_dev *indio = ctx;
	struct t70_scan scan = { 0 };

	if (iio_device_get_clock(indio) != CLOCK_BOOTTIME)
		return -EOPNOTSUPP;
	if (!xyz || boottime_ns <= 0)
		return -EINVAL;
	memcpy(scan.xyz, xyz, sizeof(scan.xyz));
	return iio_push_to_buffers_with_ts(indio, &scan, sizeof(scan), boottime_ns);
}

static int t70_postenable(struct iio_dev *indio)
{
	struct t70_accel *accel = iio_priv(indio);
	int ret;

	if (iio_device_get_clock(indio) != CLOCK_BOOTTIME)
		return -EOPNOTSUPP;
	ret = accel->link.start(accel->link.priv, t70_sample, indio);
	accel->started = !ret;
	return ret;
}

static int t70_predisable(struct iio_dev *indio)
{
	struct t70_accel *accel = iio_priv(indio);

	if (accel->started) {
		accel->link.stop(accel->link.priv);
		accel->started = false;
	}
	return 0;
}

static int t70_read_raw(struct iio_dev *indio, const struct iio_chan_spec *chan,
			int *val, int *val2, long mask)
{
	struct t70_accel *accel = iio_priv(indio);

	if (mask != IIO_CHAN_INFO_SCALE || chan->type != IIO_ACCEL)
		return -EINVAL;
	*val = accel->link.scale_num;
	*val2 = accel->link.scale_den;
	return IIO_VAL_FRACTIONAL;
}

static const struct iio_buffer_setup_ops t70_buffer_ops = {
	.postenable = t70_postenable,
	.predisable = t70_predisable,
};
static const struct iio_info t70_info = { .read_raw = t70_read_raw };

static void t70_release(void *data)
{
	struct iio_dev *indio = data;
	struct t70_accel *accel = iio_priv(indio);

	t70_predisable(indio);
	if (accel->attached && accel->link.release)
		accel->link.release(accel->link.priv);
	symbol_put(t70_scp_motion_attach_v1);
}

static int t70_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	typeof(&t70_scp_motion_attach_v1) attach;
	struct iio_dev *indio;
	struct t70_accel *accel;
	int ret;

	if (!dev->of_node || !of_device_is_available(dev->of_node))
		return -ENODEV;
	indio = devm_iio_device_alloc(dev, sizeof(*accel));
	if (!indio)
		return -ENOMEM;
	accel = iio_priv(indio);
	attach = symbol_get(t70_scp_motion_attach_v1);
	if (!attach)
		return dev_err_probe(dev, -EPROBE_DEFER, "SCP accelerometer provider missing\n");
	ret = devm_add_action_or_reset(dev, t70_release, indio);
	if (ret)
		return ret;
	ret = attach(dev, &accel->link);
	if (ret)
		return dev_err_probe(dev, ret, "SCP accelerometer session unavailable\n");
	accel->attached = true;
	if (!accel->link.start || !accel->link.stop || !accel->link.release ||
	    accel->link.scale_num <= 0 || accel->link.scale_den <= 0)
		return dev_err_probe(dev, -EPROTO, "invalid motion provider contract\n");

	indio->name = "t70pro-scp-accelerometer";
	indio->info = &t70_info;
	indio->channels = t70_channels;
	indio->num_channels = ARRAY_SIZE(t70_channels);
	indio->available_scan_masks = t70_scan_masks;
	ret = iio_device_set_clock(indio, CLOCK_BOOTTIME);
	if (ret)
		return ret;
	ret = devm_iio_kfifo_buffer_setup(dev, indio, &t70_buffer_ops);
	if (ret)
		return ret;
	return devm_iio_device_register(dev, indio);
}

static const struct of_device_id t70_ids[] = {
	{ .compatible = "teclast,t70pro-scp-accelerometer" },
	{ }
};
MODULE_DEVICE_TABLE(of, t70_ids);
static struct platform_driver t70_driver = {
	.probe = t70_probe,
	.driver = { .name = "t70-scp-accel", .of_match_table = t70_ids },
};
module_platform_driver(t70_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T70 Pro SCP accelerometer IIO consumer");
