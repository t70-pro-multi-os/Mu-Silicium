// SPDX-License-Identifier: GPL-2.0-only
/* Playback-only machine link for the T70 Pro AW88261 pair. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

struct t70_speaker {
	struct snd_soc_card card;
	struct snd_soc_dai_link links[2];
	struct snd_soc_dai_link_component cpus[2];
	struct snd_soc_dai_link_component platforms[2];
	struct snd_soc_dai_link_component amps[2];
};

SND_SOC_DAILINK_DEF(t70_speaker_dummy, COMP_DUMMY());

static void t70_put_node(void *data)
{
	of_node_put(data);
}

static struct device_node *t70_phandle(struct device *dev, const char *name, int index)
{
	struct device_node *node = of_parse_phandle(dev->of_node, name, index);
	int ret;

	if (!node)
		return ERR_PTR(-EINVAL);
	ret = devm_add_action_or_reset(dev, t70_put_node, node);
	if (ret)
		return ERR_PTR(ret);
	return node;
}

static int t70_speaker_fixup(struct snd_soc_pcm_runtime *rtd,
			     struct snd_pcm_hw_params *params)
{
	struct snd_mask *format = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	snd_mask_none(format);
	params_set_format(params, SNDRV_PCM_FORMAT_S32_LE);
	return 0;
}

static int t70_speaker_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *afe, *amp[2], *bus[2];
	struct t70_speaker *spk;
	unsigned int addr, channel;
	int n;

	if (!dev->of_node || !of_device_is_available(dev->of_node))
		return -ENODEV;
	if (of_property_count_u32_elems(dev->of_node, "teclast,afe") != 1 ||
	    of_property_count_u32_elems(dev->of_node, "teclast,amps") != 2)
		return -EINVAL;

	afe = t70_phandle(dev, "teclast,afe", 0);
	if (IS_ERR(afe))
		return PTR_ERR(afe);
	if (!of_device_is_compatible(afe, "teclast,t70pro-mt6789-afe"))
		return -EINVAL;
	if (!of_device_is_available(afe))
		return dev_err_probe(dev, -EPROBE_DEFER, "T70 AFE is disabled\n");

	for (n = 0; n < ARRAY_SIZE(amp); n++) {
		amp[n] = t70_phandle(dev, "teclast,amps", n);
		if (IS_ERR(amp[n]))
			return PTR_ERR(amp[n]);
		if (!of_device_is_compatible(amp[n], "awinic,aw88261") ||
		    of_property_read_u32(amp[n], "reg", &addr) ||
		    of_property_read_u32(amp[n], "awinic,audio-channel", &channel) ||
		    addr != 0x34 + n || channel != n)
			return -EINVAL;
		if (!of_device_is_available(amp[n]))
			return dev_err_probe(dev, -EPROBE_DEFER, "T70 AW88261 is disabled\n");
	}
	bus[0] = of_get_parent(amp[0]);
	bus[1] = of_get_parent(amp[1]);
	n = !bus[0] || bus[0] != bus[1] ||
	    !of_device_is_compatible(bus[0], "teclast,t70pro-mt6983-i2c");
	if (!n && !of_device_is_available(bus[0]))
		n = -EPROBE_DEFER;
	of_node_put(bus[1]);
	of_node_put(bus[0]);
	if (n)
		return n < 0 ? n : -EINVAL;

	spk = devm_kzalloc(dev, sizeof(*spk), GFP_KERNEL);
	if (!spk)
		return -ENOMEM;
	for (n = 0; n < ARRAY_SIZE(spk->cpus); n++) {
		spk->cpus[n].of_node = afe;
		spk->platforms[n].of_node = afe;
		spk->amps[n].of_node = amp[n];
		spk->amps[n].dai_name = "aw88261-aif";
	}
	spk->cpus[0].dai_name = "DL1";
	spk->cpus[1].dai_name = "I2S3";
	spk->links[0] = (struct snd_soc_dai_link) {
		.name = "T70 Speaker Playback",
		.stream_name = "T70 Speaker Playback",
		.cpus = &spk->cpus[0], .num_cpus = 1,
		.platforms = &spk->platforms[0], .num_platforms = 1,
		.codecs = t70_speaker_dummy, .num_codecs = ARRAY_SIZE(t70_speaker_dummy),
		.dynamic = 1, .playback_only = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE, SND_SOC_DPCM_TRIGGER_PRE },
	};
	spk->links[1] = (struct snd_soc_dai_link) {
		.name = "T70 I2S3 AW88261",
		.cpus = &spk->cpus[1], .num_cpus = 1,
		.platforms = &spk->platforms[1], .num_platforms = 1,
		.codecs = spk->amps, .num_codecs = ARRAY_SIZE(spk->amps),
		.no_pcm = 1, .playback_only = 1, .ignore_pmdown_time = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC | SND_SOC_DAIFMT_GATED,
		.be_hw_params_fixup = t70_speaker_fixup,
	};
	spk->card = (struct snd_soc_card) {
		.name = "Teclast T70 Pro Speakers",
		.driver_name = "t70pro-aw88261",
		.dev = dev, .owner = THIS_MODULE,
		.dai_link = spk->links, .num_links = ARRAY_SIZE(spk->links),
	};
	return devm_snd_soc_register_card(dev, &spk->card);
}

static const struct of_device_id t70_speaker_of_match[] = {
	{ .compatible = "teclast,t70pro-aw88261-speaker" },
	{ }
};
MODULE_DEVICE_TABLE(of, t70_speaker_of_match);

static struct platform_driver t70_speaker_driver = {
	.probe = t70_speaker_probe,
	.driver = {
		.name = "t70-mt6789-aw88261",
		.of_match_table = t70_speaker_of_match,
	},
};
module_platform_driver(t70_speaker_driver);

MODULE_DESCRIPTION("Teclast T70 Pro MT6789 AW88261 playback-only machine");
MODULE_LICENSE("GPL");
