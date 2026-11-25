// SPDX-License-Identifier: GPL-2.0-only
/* Teclast T70 Pro HX83132, retained-controller, finger input only. */
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#include <Library/HimaxCore.h>
#include "himax_release.h"

struct t70_hx {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties props;
	HM_IO io;
	struct mutex report_lock;
	struct delayed_work release_work;
	struct t70_hx_release_state release_state;
	unsigned long release_deadline;
	bool irq_enabled;
};

static HM_STATUS t70_hx_transfer(void *ctx, const unsigned char *tx,
				 unsigned int txlen, unsigned char *rx,
				 unsigned int rxlen)
{
	struct i2c_client *client = ctx;
	struct i2c_msg msg[2] = {
		{ .addr = client->addr, .len = txlen, .buf = (u8 *)tx },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = rxlen, .buf = rx },
	};
	int n = rxlen ? 2 : 1;

	return i2c_transfer(client->adapter, msg, n) == n ? HmOk : HmIoError;
}

static void t70_hx_delay(void *ctx, unsigned int min_us, unsigned int max_us)
{
	usleep_range(min_us, max_us);
}

static void t70_hx_report(struct t70_hx *hx, const HM_FRAME *frame)
{
	unsigned int i;

	for (i = 0; i < HM_MAX_CONTACTS; ++i) {
		const HM_CONTACT *point = frame ? &frame->Contacts[i] : NULL;

		input_mt_slot(hx->input, i);
		input_mt_report_slot_state(hx->input, MT_TOOL_FINGER,
					   point && point->Active);
		if (!point || !point->Active)
			continue;
		touchscreen_report_pos(hx->input, &hx->props, point->X,
				       point->Y, true);
		input_report_abs(hx->input, ABS_MT_PRESSURE, point->Pressure);
	}
	input_mt_sync_frame(hx->input);
	input_sync(hx->input);
}

static void t70_hx_release_work(struct work_struct *work)
{
	struct t70_hx *hx = container_of(to_delayed_work(work), struct t70_hx, release_work);

	unsigned long now;

	mutex_lock(&hx->report_lock);
	now = jiffies;
	if (hx->release_state.active && hx->release_state.bad_frames) {
		/* A previous callback may have raced with a newer frame. */
		if (time_before(now, hx->release_deadline))
			mod_delayed_work(system_wq, &hx->release_work,
					 hx->release_deadline - now);
		else if (t70_hx_timeout(&hx->release_state) == T70_HX_RELEASE)
			t70_hx_report(hx, NULL);
	}
	mutex_unlock(&hx->report_lock);
}

static void t70_hx_cleanup_work(void *data)
{
	struct t70_hx *hx = data;

	cancel_delayed_work_sync(&hx->release_work);
}

/* Caller holds report_lock so resume and the threaded IRQ cannot consume
 * the retained controller's event FIFO concurrently.
 */
static bool t70_hx_accept_report(struct t70_hx *hx)
{
	u8 report[HM_T70_REPORT_BYTES];
	HM_FRAME frame;

	if (HmBusRead(&hx->io, HM_EVENT_COMMAND, report, sizeof(report)) != HmOk ||
	    HmDecode(report, sizeof(report), HM_MAX_CONTACTS, 1400, 2240,
		     &frame) != HmOk)
		return false;

	t70_hx_valid(&hx->release_state, frame.Count);
	cancel_delayed_work(&hx->release_work);
	t70_hx_report(hx, &frame);
	return true;
}

static irqreturn_t t70_hx_interrupt(int irq, void *data)
{
	struct t70_hx *hx = data;

	mutex_lock(&hx->report_lock);
	if (!t70_hx_accept_report(hx)) {
		enum t70_hx_release_action action = t70_hx_bad(&hx->release_state);

		if (action == T70_HX_ARM_TIMEOUT) {
			unsigned long delay = msecs_to_jiffies(T70_HX_BAD_FRAME_TIMEOUT_MS);

			hx->release_deadline = jiffies + delay;
			mod_delayed_work(system_wq, &hx->release_work, delay);
		} else if (action == T70_HX_RELEASE) {
			cancel_delayed_work(&hx->release_work);
			t70_hx_report(hx, NULL);
		}
	}
	mutex_unlock(&hx->report_lock);
	return IRQ_HANDLED;
}

static void t70_hx_sync_and_enable_irq(struct t70_hx *hx)
{
	/* A falling edge may already have occurred while masked. A bad
	 * snapshot is not an IRQ report: don't advance the release timer.
	 */
	mutex_lock(&hx->report_lock);
	t70_hx_accept_report(hx);
	mutex_unlock(&hx->report_lock);
	enable_irq(hx->client->irq);
	hx->irq_enabled = true;
}

static int t70_hx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct t70_hx *hx;
	int ret;

	if (client->addr != 0x48 || (client->flags & I2C_CLIENT_TEN) ||
	    client->irq <= 0 || !i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -ENODEV, "missing T70 I2C/IRQ resources\n");

	hx = devm_kzalloc(dev, sizeof(*hx), GFP_KERNEL);
	if (!hx)
		return -ENOMEM;
	hx->client = client;
	hx->io = (HM_IO) { .Context = client, .Transfer = t70_hx_transfer,
			   .Delay = t70_hx_delay };
	/* Opt-in firmware binding only. Do not request power/reset or load firmware. */
	if (HmReadChipId(&hx->io) != HmOk)
		return dev_err_probe(dev, -ENODEV, "retained HX83132 ID not verified\n");

	hx->input = devm_input_allocate_device(dev);
	if (!hx->input)
		return -ENOMEM;
	hx->input->name = "Teclast T70 Pro HX83132 Touchscreen";
	hx->input->id.bustype = BUS_I2C;
	input_set_abs_params(hx->input, ABS_MT_POSITION_X, 0, 1400, 0, 0);
	input_set_abs_params(hx->input, ABS_MT_POSITION_Y, 0, 2240, 0, 0);
	input_set_abs_params(hx->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	touchscreen_parse_properties(hx->input, true, &hx->props);
	ret = input_mt_init_slots(hx->input, HM_MAX_CONTACTS, INPUT_MT_DIRECT);
	if (ret)
		return ret;
	ret = input_register_device(hx->input);
	if (ret)
		return ret;

	mutex_init(&hx->report_lock);
	INIT_DELAYED_WORK(&hx->release_work, t70_hx_release_work);
	/* devres frees the IRQ before draining work, then unregisters input. */
	ret = devm_add_action_or_reset(dev, t70_hx_cleanup_work, hx);
	if (ret)
		return ret;
	i2c_set_clientdata(client, hx);
	ret = devm_request_threaded_irq(dev, client->irq, NULL, t70_hx_interrupt,
				       IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(dev), hx);
	if (ret)
		return dev_err_probe(dev, ret, "cannot acquire touch IRQ\n");
	t70_hx_sync_and_enable_irq(hx);
	return 0;
}

static int t70_hx_suspend(struct device *dev)
{
	struct t70_hx *hx = dev_get_drvdata(dev);

	if (hx->irq_enabled) {
		disable_irq(hx->client->irq);
		hx->irq_enabled = false;
	}
	cancel_delayed_work_sync(&hx->release_work);
	t70_hx_valid(&hx->release_state, 0);
	t70_hx_report(hx, NULL);
	return 0;
}

static int t70_hx_resume(struct device *dev)
{
	struct t70_hx *hx = dev_get_drvdata(dev);

	if (HmReadChipId(&hx->io) != HmOk)
		return dev_err_probe(dev, -ENODEV, "touch state lost across suspend\n");
	if (!hx->irq_enabled)
		t70_hx_sync_and_enable_irq(hx);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(t70_hx_pm, t70_hx_suspend, t70_hx_resume);
static const struct of_device_id t70_hx_of_match[] = {
	{ .compatible = "teclast,t70pro-hx83132-retained" },
	{ }
};
MODULE_DEVICE_TABLE(of, t70_hx_of_match);

static struct i2c_driver t70_hx_driver = {
	.probe = t70_hx_probe,
	.driver = {
		.name = "t70_hx83132",
		.of_match_table = t70_hx_of_match,
		.pm = pm_sleep_ptr(&t70_hx_pm),
	},
};
module_i2c_driver(t70_hx_driver);
MODULE_DESCRIPTION("Teclast T70 Pro retained HX83132 touchscreen");
MODULE_LICENSE("GPL");
