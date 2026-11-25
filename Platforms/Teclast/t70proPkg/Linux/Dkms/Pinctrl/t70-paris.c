// SPDX-License-Identifier: GPL-2.0
/* MT6789 pinctrl descriptor for the generic MediaTek Paris core. */
#include <linux/build_bug.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "pinctrl-paris.h"
#include "t70-paris-pad-data.h"

static const char * const mt6789_bases[] = {
	"gpio", "iocfg_rb", "iocfg_bm", "iocfg_br",
	"iocfg_lm", "iocfg_bl", "iocfg_rt",
};

/* Stock AP EINT has 160 identity-mapped lines, 5 banks, debounce on 0..31.
 * The stock timing table matches the core's debounce_time_mt6878.
 */
static const struct mtk_eint_hw mt6789_eint = {
	.port_mask = 7,
	.ports = 5,
	.ap_num = 160,
	.db_cnt = 32,
	.db_time = debounce_time_mt6878,
};

static_assert(ARRAY_SIZE(mtk_pins_mt6789) == 222);
static_assert(ARRAY_SIZE(mt6789_pull_type) == ARRAY_SIZE(mtk_pins_mt6789));

static const struct mtk_pin_soc mt6789_pinctrl_data = {
	.reg_cal = mt6789_reg_cals,
	.pins = mtk_pins_mt6789,
	.npins = ARRAY_SIZE(mtk_pins_mt6789),
	.ngrps = ARRAY_SIZE(mtk_pins_mt6789),
	.nfuncs = 8,
	.eint_hw = &mt6789_eint,
	.gpio_m = 0,
	.base_names = mt6789_bases,
	.nbase_names = ARRAY_SIZE(mt6789_bases),
	.pull_type = mt6789_pull_type,
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.drive_set = mtk_pinconf_drive_set_raw,
	.drive_get = mtk_pinconf_drive_get_raw,
};

static const struct of_device_id mt6789_pinctrl_match[] = {
	{ .compatible = "teclast,t70pro-mt6789-pinctrl", .data = &mt6789_pinctrl_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6789_pinctrl_match);

static struct platform_driver mt6789_pinctrl_driver = {
	.probe = mtk_paris_pinctrl_probe,
	.driver = {
		.name = "t70-mt6789-pinctrl",
		.of_match_table = mt6789_pinctrl_match,
		.pm = pm_sleep_ptr(&mtk_paris_pinctrl_pm_ops),
		.suppress_bind_attrs = true,
	},
};

/* Paris EINT registers a chained parent IRQ with no removal hook. */
static int __init mt6789_pinctrl_init(void)
{
	return platform_driver_register(&mt6789_pinctrl_driver);
}
module_init(mt6789_pinctrl_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Teclast T70 Pro MT6789 pinctrl for generic MediaTek Paris");
