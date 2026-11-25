#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the AFE I2S constituent group for an object-only Linux 7.0 ABI check."""
import argparse
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess

HERE = Path(__file__).resolve().parent
DEFAULT_REFERENCE = HERE.parents[5].parent / "kernel-reference"
REVISION = "a71515dbaf0d539ffb36416f14186dad17eef743"
DAIS = ("pcm", "src", "hw-gain", "hostless", "adda")
AFE = "mindone/modules/snd_soc_mt6789_afe"
COMMON = "mindone/modules/include/sound/soc/mediatek/common"
SOURCES = {f"mt6789/{name}": f"{AFE}/{name}" for name in (
    "mt6789-dai-i2s.c", "mt6789-afe-common.h", "mt6789-afe-clk.h",
    "mt6789-afe-gpio.h", "mt6789-afe-gpio.c", "mt6789-afe-control.c",
    "mt6789-interconnection.h", "mt6789-reg.h",
    "mt-plat/aee.h")}
for name in ("mtk-base-afe.h", "mtk-sram-manager.h"):
    SOURCES[f"common/{name}"] = f"{COMMON}/{name}"


def show(repo, path, depth=0):
    if depth > 4 or not path.startswith("mindone/modules/") or ".." in PurePosixPath(path).parts:
        raise ValueError("vendor source symlink escapes the staged source set")
    entry = subprocess.check_output(
        ["git", "-C", str(repo), "ls-tree", REVISION, path], text=True).split()
    if not entry:
        raise ValueError(f"missing vendor source file: {path}")
    data = subprocess.check_output(["git", "-C", str(repo), "show", f"{REVISION}:{path}"])
    if entry[0] == "120000":
        target = posixpath.normpath(posixpath.join(posixpath.dirname(path), data.decode("ascii")))
        return show(repo, target, depth + 1)
    if entry[0] != "100644":
        raise ValueError(f"unexpected vendor source mode: {path}")
    return data


def port_trigger(text):
    begin = text.index("int mt6789_fe_trigger(")
    end = text.index("\nstatic int mt6789_memif_fs(", begin)
    t = text[begin:end]
    old = "\tint ret = 0;"
    if t.count(old) != 1:
        raise ValueError("unexpected MT6789 trigger locals")
    t = t.replace(old, "\tbool memif_started = false;\n\tint rollback;\n" + old)
    a = t.index("\tcase SNDRV_PCM_TRIGGER_START:")
    b = t.index("\tcase SNDRV_PCM_TRIGGER_STOP:", a)
    start = t[a:b]
    rate = start.index("\t\t/* set irq fs */")
    rate_end = start.index("\n\n", rate)
    rate_check = start[rate:rate_end].replace("ret = -EINVAL;\n\t\t\tbreak;", "return fs;")
    rate_check = rate_check.replace("\t\t/* set irq fs */", "\t\t/* Validate the rate before starting the memif. */")
    start = start[:rate] + start[rate_end + 2:]
    start = start.replace("\tcase SNDRV_PCM_TRIGGER_START:\n",
                          "\tcase SNDRV_PCM_TRIGGER_START:\n\t\tif (!rate)\n"
                          "\t\t\treturn -EINVAL;\n" + rate_check + "\n\n", 1)
    old = "\t\t\t\tbreak;\n\t\t\t}\n\t\t}"
    if start.count(old) != 1:
        raise ValueError("unexpected MT6789 memif start block")
    start = start.replace(old, old[:-1] + "\tmemif_started = true;\n\t\t}")
    for field in ("cnt", "fs"):
        old = f"\t\tmtk_regmap_update_bits(afe->regmap, irq_data->irq_{field}_reg,"
        if start.count(old) != 1:
            raise ValueError(f"unexpected MT6789 IRQ {field} setup")
        pos = start.index(old)
        stop = start.index(";", pos) + 1
        start = (start[:pos] + start[pos:stop].replace("mtk_regmap", "ret = mtk_regmap", 1)
                 + "\n\t\tif (ret)\n\t\t\tgoto start_failed;" + start[stop:])
    old = "\t\tif (!runtime->no_period_wakeup)\n\t\t\tmtk_irq_set_enable(afe, irq_data, id);"
    if start.count(old) != 1:
        raise ValueError("unexpected MT6789 IRQ start call")
    start = start.replace(old, """		if (!runtime->no_period_wakeup) {
			ret = mtk_irq_set_enable(afe, irq_data, id);
			if (ret)
				goto start_failed;
		}""")
    finish = "\n\treturn ret;\n}"
    tail = t[b:]
    if tail.count(finish) != 1:
        raise ValueError("unexpected MT6789 trigger return")
    tail = tail.replace(finish, finish[:-2] + """
start_failed:
	if (memif_started) {
		rollback = mtk_memif_set_disable(afe, id);
		if (rollback)
			dev_err(afe->dev, "%s: memif %d rollback failed: %d\\n",
				__func__, id, rollback);
	}
	return ret;
}""")
    return text[:begin] + t[:a] + start + tail + text[end:]


def port_fe_irq(data):
    old = b"\tregmap_update_bits(afe->regmap, irq_data->irq_en_reg,"
    if data.count(old) != 2:
        raise ValueError("unexpected protected IRQ register writes")
    # Unlock even when the register write fails, and propagate that result.
    return data.replace(old, b"\tret = " + old[1:])


def drop_pcm_raw_debug(text):
    # Do not ship the stock raw register sysfs/debugfs command surfaces.
    start = text.index("static ssize_t mt6789_debug_read_reg(", text.index("static int mt6789_afe_component_probe("))
    end = text.index("static int mt6789_dai_memif_register(", start)
    block = text[start:end]
    if ('MTK_AFE_DBG_CMD("write_reg", mtk_afe_debug_write_reg)' not in block or
            "mt6789_debugfs_ops" not in block):
        raise ValueError("unexpected vendor PCM raw debugfs helpers")
    text = text[:start] + text[end:]
    text, count = re.subn(r"(?ms)^#if IS_ENABLED\(CONFIG_DEBUG_FS\)\n\t/\* debugfs \*/\n.*?^#endif\n",
                          "", text)
    if count != 1:
        raise ValueError("unexpected vendor PCM debugfs registration")
    text, count = re.subn(r"(?m)^static ssize_t mt6789_debug_read_reg\([^;\n]+\);\n", "", text)
    if count != 1:
        raise ValueError("unexpected vendor PCM debug prototype")
    for old in ('#include "../common/mtk-afe-debug.h"\n',):
        if text.count(old) != 1:
            raise ValueError("unexpected vendor PCM debug include")
        text = text.replace(old, "")
    return text


def port_pcm(data):
    text = data.decode("utf-8")
    old = "snd_soc_kcontrol_component(kcontrol)"
    if text.count(old) != 29:
        raise ValueError("unexpected legacy PCM control lookups")
    text = text.replace(old, "snd_kcontrol_chip(kcontrol)")
    old = ".remove_new = mt6789_afe_pcm_dev_remove,"
    if text.count(old) != 1:
        raise ValueError("unexpected PCM remove callback")
    text = text.replace(old, ".remove = mt6789_afe_pcm_dev_remove,")
    copy = re.search(r"(?m)^static[ \t]+\w+[ \t]+copy_from_buffer_request\(", text)
    if not copy:
        raise ValueError("unexpected vendor PCM sysfs helper")
    start = copy.start()
    probe = text.index("static int mt6789_afe_component_probe(", start)
    end = text.index("\n}", probe) + 2
    previous = text[probe:end]
    if ("snd_card_add_dev_attr" not in previous or
            "afe_sysfs_debug_write" not in text[start:probe]):
        raise ValueError("unexpected PCM raw sysfs boundary")
    # Keep legitimate component controls, do not expose vendor raw sysfs.
    clean = """static int mt6789_afe_component_probe(struct snd_soc_component *component)
{
\tmtk_afe_add_sub_dai_control(component);
\tmt6789_add_misc_control(component);
\treturn 0;
}"""
    return port_trigger(drop_pcm_raw_debug(text[:start] + clean + text[end:])).encode("utf-8")


def port_platform(files):
    # This vendor code and its users must agree on the private AFE layout.
    # Do not bind to identically named generic exports with another layout.
    source = "common/mtk-afe-platform-driver.c"
    data = files[source].decode("utf-8")
    if data.count("&component->dapm") != 2 or data.count("component->dapm.card") != 1:
        raise ValueError("unexpected legacy DAPM member accesses")
    data = data.replace("&component->dapm", "snd_soc_component_to_dapm(component)")
    data = data.replace("component->dapm.card", "component->card")
    data, count = re.subn(r"(} else \{\n[ \t]+)(sp_copy\()", r"\1return \2", data)
    if count != 1:
        raise ValueError("unexpected platform copy fallback")
    old = "\tpcm_ptr_bytes = hw_ptr_64 - hw_base_64;"
    if data.count(old) != 1:
        raise ValueError("unexpected platform pointer offset calculation")
    # A successful register read need not yield a valid ring offset.
    # Reject before narrowing to the legacy int or aligning to 16 bytes.
    data = data.replace(old, """	if (hw_ptr_64 < hw_base_64 ||
	    hw_ptr_64 - hw_base_64 >= memif->dma_bytes)
		return SNDRV_PCM_POS_XRUN;
	pcm_ptr_bytes = hw_ptr_64 - hw_base_64;""")
    files[source] = data.encode("utf-8")
    namespace_common(files, source, 10)


def namespace_common(files, source, expected):
    export = re.compile(rb"(?m)^EXPORT_SYMBOL(?:_GPL)?\((\w+)\);\n")
    names = export.findall(files[source])
    if len(names) != expected or len(set(names)) != expected:
        raise ValueError(f"unexpected vendor exports: {source}")
    files[source] = export.sub(b"", files[source])
    pattern = re.compile(rb"\b(" + b"|".join(names) + rb")\b")
    for name in files:
        if name.endswith((".h", ".c")):
            files[name] = pattern.sub(lambda match: b"t70_" + match[0], files[name])


def port_sram(data):
    for name in (b"prefer_mode", b"mode_size", b"block_size"):
        old = b'"' + name + b'"'
        if data.count(old) != 1:
            raise ValueError(f"unexpected vendor SRAM property: {old!r}")
        data = data.replace(old, b'"' + name.replace(b"_", b"-") + b'"')
    for old, new in (
        (b"if (!regaddr_p)", b"if (!regaddr_p || !size64 || size64 > U32_MAX)"),
        (b"\tsram->block_num = (sram->size / sram->block_size);",
         b"\tif (!sram->block_size || sram->prefer_mode >= MTK_AUDIO_SRAM_MODE_NUM ||\n"
         b"\t    sram->mode_size[0] < sram->block_size || sram->mode_size[0] > sram->size ||\n"
         b"\t    sram->mode_size[1] < sram->block_size || sram->mode_size[1] > sram->size)\n"
         b"\t\tgoto of_error;\n"
         b"\tsram->block_num = sram->size / sram->block_size;"),
        (b"\tfor (i = 0; i < sram->block_num ; i++) {\n\t\tsram->blocks[i].valid",
         b"\tif (!sram->blocks)\n\t\treturn -ENOMEM;\n"
         b"\tfor (i = 0; i < sram->block_num ; i++) {\n\t\tsram->blocks[i].valid"),
    ):
        if data.count(old) != 1:
            raise ValueError("unexpected vendor SRAM geometry or allocation")
        data = data.replace(old, new)
    return data


def port_ack(data):
    original = b"""	unsigned int size_per_frame = runtime->frame_bits / 8;
	snd_pcm_uframes_t avail = snd_pcm_playback_avail(runtime);"""
    checked = b"""	unsigned int size_per_frame;
	snd_pcm_uframes_t avail;
	unsigned long ring_bytes;

	if (!runtime || !runtime->dma_area || !runtime->buffer_size ||
	    !runtime->control || !runtime->status)
		return -EINVAL;
	size_per_frame = frames_to_bytes(runtime, 1);
	if (!size_per_frame || runtime->buffer_size > UINT_MAX / size_per_frame)
		return -EINVAL;
	ring_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	if (ring_bytes > runtime->dma_bytes)
		return -EINVAL;
	avail = snd_pcm_playback_avail(runtime);
	if (avail > runtime->buffer_size)
		return 0; /* XRUN: there is no trustworthy free playback span. */"""
    if data.count(original) != 1 or data.count(b"runtime->dma_bytes") != 2:
        raise ValueError("unexpected vendor PCM acknowledge bounds")
    # Use the active ring, not the larger preallocated ALSA backing store.
    data = data.replace(b"runtime->dma_bytes", b"ring_bytes")
    data = data.replace(original, checked)
    return data.replace(b"if (write_idx + copy_size < ring_bytes)",
                        b"if (copy_size <= ring_bytes - write_idx)")


def port_tone(data):
    boundary = b"/* usb call control */"
    if data.count(boundary) != 1:
        raise ValueError("unexpected vendor tone source boundary")
    tone = data.split(boundary)[0]
    old = b"snd_soc_kcontrol_component(kcontrol)"
    if tone.count(old) != 8 or tone.count(b"ucontrol->value.integer.value[0]") != 7:
        raise ValueError("unexpected vendor tone control API")
    tone = tone.replace(old, b"snd_kcontrol_chip(kcontrol)")
    tone = tone.replace(b"ucontrol->value.integer.value[0]", b"ucontrol->value.enumerated.item[0]")
    return tone + b"""int mt6789_add_misc_control(struct snd_soc_component *component)
{
	return snd_soc_add_component_controls(component, mt6789_afe_sgen_controls,
				     ARRAY_SIZE(mt6789_afe_sgen_controls));
}
"""


def keep_alsa_buffers(pcm, fe):
    # Android HAL's private dma-buf fd API is not ALSA PCM mmap.
    header = b'#include "../common/mtk-mmap-ion.h"\n'
    if pcm.count(header) != 1 or fe.count(b'#include "mtk-mmap-ion.h"\n') != 1:
        raise ValueError("unexpected vendor mmap header use")
    pcm = pcm.replace(header, b"")
    fe = fe.replace(b'#include "mtk-mmap-ion.h"\n', b"")
    start = pcm.index(b"static int mt6789_mmap_dl_scene_get(")
    end = pcm.index(b"static const struct snd_kcontrol_new mt6789_pcm_kcontrols", start)
    if pcm[start:end].count(b"use_mmap_share_mem") != 8:
        raise ValueError("unexpected vendor mmap PCM handlers")
    pcm = pcm[:start] + pcm[end:]
    start = pcm.index(b'\tSOC_SINGLE_EXT("mmap_play_scenario"')
    end = pcm.index(b"\n};", start)
    if pcm[start:end].count(b"SOC_SINGLE_EXT") != 5:
        raise ValueError("unexpected vendor mmap ALSA controls")
    pcm = pcm[:start] + pcm[end + 1:]
    start = fe.index(b"\t// mmap don't alloc buffer")
    end = fe.index(b"\n#if IS_ENABLED(CONFIG_SND_SOC_MTK_SRAM)", start)
    if b"mtk_get_mmap_ul_buffer" not in fe[start:end]:
        raise ValueError("unexpected vendor mmap allocation branch")
    fe = fe[:start] + fe[end + 1:]
    old = b"\t// mmap do not free buffer\n\tif (memif->use_mmap_share_mem)\n\t\treturn 0;\n\n"
    if fe.count(old) != 1:
        raise ValueError("unexpected vendor mmap free branch")
    return pcm, fe.replace(old, b"")





def validate_afe_irq_status(data):
    begin = data.index(b"static irqreturn_t mt6789_afe_irq_handler(")
    end = data.index(b"\n}", begin)
    handler = data[begin:end]
    old = handler[handler.index(b"\t/* get irq that is sent to MCU */"):
                  handler.index(b"\tktime_get_ts64(")]
    if b"goto err_irq;" not in old or b"AFE_IRQ_MCU_EN" not in old:
        raise ValueError("unexpected MT6789 AFE IRQ status block")
    handler = handler.replace(old, b"""	/* Do not acknowledge a status mask from a failed register read. */
	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &mcu_en);
	if (ret)
		return IRQ_NONE;

	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	if (ret)
		return IRQ_NONE;

	status_mcu = status & mcu_en & AFE_IRQ_STATUS_BITS;
	if (!status_mcu)
		return IRQ_NONE;

""")
    handler = handler.replace(b"\nerr_irq:\n", b"\n")
    old = b"\t\tif (memif->irq_usage < 0)\n"
    if handler.count(old) != 1:
        raise ValueError("unexpected MT6789 AFE IRQ index check")
    handler = handler.replace(old, b"\t\tif (memif->irq_usage < 0 || memif->irq_usage >= afe->irqs_size)\n")
    return data[:begin] + handler + data[end:]


def manage_afe_irq_wake(data):
    start = b"static int mt6789_afe_pcm_dev_probe(struct platform_device *pdev)"
    old = b"""	ret = enable_irq_wake(irq_id);
	if (ret < 0)
		dev_err(dev, "enable_irq_wake %d err: %d\\n", irq_id, ret);
"""
    if data.count(start) != 1 or data.count(old) != 1:
        raise ValueError("unexpected MT6789 IRQ wake setup")
    helper = b"""#if !defined(CONFIG_FPGA_EARLY_PORTING) || defined(FORCE_FPGA_ENABLE_IRQ)
static void t70_afe_release_irq_wake(void *data)
{
	disable_irq_wake((unsigned long)data);
}

static int t70_afe_acquire_irq_wake(struct device *dev, unsigned int irq)
{
	int ret = enable_irq_wake(irq);

	if (ret) {
		/* Wake support is optional, as in the stock probe. */
		dev_err(dev, "enable_irq_wake %u err: %d\\n", irq, ret);
		return 0;
	}

	/* Runs before the earlier devm IRQ is freed, on either exit path. */
	return devm_add_action_or_reset(dev, t70_afe_release_irq_wake,
					(void *)(unsigned long)irq);
}
#endif

"""
    return data.replace(start, helper + start).replace(old, b"""	ret = t70_afe_acquire_irq_wake(dev, irq_id);
	if (ret)
		goto err_pm_disable;
""")


def balance_probe_regmap_pm(data):
    start = b"static int mt6789_afe_pcm_dev_probe(struct platform_device *pdev)"
    begin = data.index(start)
    end = data.index(b"\nstatic void mt6789_afe_pcm_dev_remove(", begin)
    probe = data[begin:end]
    old = b"""	if (IS_ERR(afe->base_addr))
		return PTR_ERR(afe->base_addr);

	/* enable clock for regcache get default value from hw */
	pm_runtime_get_sync(&pdev->dev);

	afe->regmap = devm_regmap_init_mmio(&pdev->dev, afe->base_addr,
					    &mt6789_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	pm_runtime_put_sync(&pdev->dev);
"""
    if probe.count(old) != 1:
        raise ValueError("unexpected MT6789 PCM regmap probe")
    probe = probe.replace(old, b"""	if (IS_ERR(afe->base_addr)) {
		ret = PTR_ERR(afe->base_addr);
		goto err_pm_disable;
	}

	ret = t70_afe_probe_regmap(afe);
	if (ret)
		goto err_pm_disable;
""")
    helper = b"""static int t70_afe_probe_regmap(struct mtk_base_afe *afe)
{
	struct device *dev = afe->dev;
	struct regmap *map;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	map = devm_regmap_init_mmio(dev, afe->base_addr, &mt6789_afe_regmap_config);
	if (IS_ERR(map)) {
		/* Leave afe->regmap NULL for the runtime suspend callback. */
		ret = PTR_ERR(map);
		pm_runtime_put_sync(dev);
		return ret;
	}

	afe->regmap = map;
	pm_runtime_put_sync(dev);
	return 0;
}

"""
    return data[:begin] + helper + probe + data[end:]


def require_audio_owner(data):
    start = b"static int mt6789_afe_pcm_dev_probe(struct platform_device *pdev)"
    if data.count(start) != 1:
        raise ValueError("unexpected MT6789 PCM probe")
    data = data.replace(start, b"""/* The generic board module cannot infer exclusive AFE ownership. */
enum t70_audio_owner_op { T70_AUDIO_CLAIM, T70_AUDIO_ATTACH, T70_AUDIO_RELEASE };
int t70_mt6789_audio_owner_v1(struct device *dev, struct mtk_base_afe *afe,
                            enum t70_audio_owner_op op);

struct t70_audio_owner {
        typeof(&t70_mt6789_audio_owner_v1) call;
        struct device *dev;
        struct mtk_base_afe *afe;
};

static void t70_audio_owner_release(void *data)
{
        struct t70_audio_owner *owner = data;

        owner->call(owner->dev, owner->afe, T70_AUDIO_RELEASE);
        symbol_put(t70_mt6789_audio_owner_v1);
}

""" + start)
    a = data.index(start)
    b = data.index(b"static void mt6789_afe_pcm_dev_remove(", a)
    probe = data[a:b]
    alloc = b"\tret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(34));"
    old = b"\tplatform_set_drvdata(pdev, afe);"
    attach = b"\taudio_set_dsp_afe(afe);"
    for marker in (alloc, old, attach, b"\tint ret, i;"):
        if probe.count(marker) != 1:
            raise ValueError("unexpected MT6789 PCM owner hookup")
    probe = probe.replace(b"\tint ret, i;", b"\tstruct t70_audio_owner *owner;\n\tint ret, i;")
    pos1 = probe.index(alloc)
    pos2 = probe.index(b"\n\tafe = devm_kzalloc", pos1)
    mask = probe[pos1:pos2]
    probe = probe[:pos1] + probe[pos2:]
    probe = probe.replace(old, b"""	owner = devm_kzalloc(&pdev->dev, sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return -ENOMEM;
	owner->dev = &pdev->dev;
	owner->afe = afe;
	owner->call = symbol_get(t70_mt6789_audio_owner_v1);
	if (!owner->call)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "T70 audio resource owner not available\\n");
	ret = owner->call(owner->dev, owner->afe, T70_AUDIO_CLAIM);
	if (ret) {
		symbol_put(t70_mt6789_audio_owner_v1);
		return dev_err_probe(&pdev->dev, ret, "T70 audio claim failed\\n");
	}
	ret = devm_add_action_or_reset(&pdev->dev, t70_audio_owner_release, owner);
	if (ret)
		return ret;

""" + mask + b"\n" + old)
    probe = probe.replace(attach, b"""	ret = owner->call(owner->dev, owner->afe, T70_AUDIO_ATTACH);
	if (ret)
		goto err_pm_disable;""")
    stock = b'.compatible = "mediatek,mt6789-sound"'
    if data.count(stock) != 1:
        raise ValueError("unexpected MT6789 PCM device match")
    return (data[:a] + probe + data[b:]).replace(
        stock, b'.compatible = "teclast,t70pro-mt6789-afe"')


def stage(repo, destination, exact_clock_check=False, pcm_check=False, dai_check=False, platform_check=False, fe_check=False, sram_check=False, ack_check=False, tone_check=False, alsa_buffer_check=False, notifier_check=False, module_check=False):
    notifier_check = notifier_check or module_check
    alsa_buffer_check = alsa_buffer_check or notifier_check
    tone_check = tone_check or alsa_buffer_check
    ack_check = ack_check or tone_check
    sram_check = sram_check or ack_check
    fe_check = fe_check or sram_check
    platform_check = platform_check or fe_check
    dai_check = dai_check or platform_check
    pcm_check = pcm_check or dai_check
    exact_clock_check = exact_clock_check or pcm_check
    sources = dict(SOURCES)
    if exact_clock_check:
        for name in ("mt6789-afe-clk.c", "mtk_sip_svc_ext.h"):
            sources[f"mt6789/{name}"] = f"{AFE}/{name}"
    if pcm_check:
        sources["mt6789/mt6789-afe-pcm.c"] = f"{AFE}/mt6789-afe-pcm.c"
        for name in ("mtk-afe-platform-driver.h",
                     "mtk-afe-fe-dai.h", "mtk-sp-pcm-ops.h", "mtk-mmap-ion.h"):
            sources[f"common/{name}"] = f"{COMMON}/{name}"
        dsp_headers = "sound/soc/mediatek/audio_dsp"
        for name in ("mtk-dsp-common.h", "mtk-dsp-common_define.h"):
            sources[f"include/{dsp_headers}/{name}"] = f"mindone/modules/include/{dsp_headers}/{name}"
        ipi_headers = "mindone/modules/include/drivers/misc/mediatek/audio_ipi/include"
        sources["include/audio_playback_msg_id.h"] = f"{ipi_headers}/audio_playback_msg_id.h"
    if dai_check:
        for dai in DAIS:
            name = f"mt6789-dai-{dai}.c"
            sources[f"mt6789/{name}"] = f"{AFE}/{name}"
    if platform_check:
        name = "mtk-afe-platform-driver.c"
        sources[f"common/{name}"] = f"mindone/modules/snd_soc_mtk_common/{name}"
    if fe_check:
        sources["common/mtk-afe-fe-dai.c"] = (
            "mindone/modules/snd_soc_mtk_common/mtk-afe-fe-dai.c")
        sources["common/mtk-afe-external.h"] = f"{COMMON}/mtk-afe-external.h"
    if sram_check:
        sources["mt6789/mtk-sram-manager.c"] = (
            "mindone/modules/snd_soc_mtk_common/mtk-sram-manager.c")
    if ack_check:
        sources["mt6789/mtk-sp-pcm-ops.c"] = (
            "mindone/modules/snd_soc_mtk_common/mtk-sp-pcm-ops.c")
    if tone_check:
        sources["mt6789/mt6789-misc-control.c"] = f"{AFE}/mt6789-misc-control.c"
    if notifier_check:
        sources["mt6789/mtk-afe-external.c"] = (
            "mindone/modules/mtk_afe_external/mtk_afe_external.c")
    files = {name: show(repo, src) for name, src in sources.items()}
    if pcm_check:
        path = "mt6789/mt6789-afe-pcm.c"
        files[path] = port_pcm(files[path])
    name = "mt6789/mt6789-dai-i2s.c"
    old = b"snd_soc_kcontrol_component(kcontrol)"
    if files[name].count(old) != 2:
        raise ValueError("unexpected number of legacy ASoC lookups")
    # The legacy helper only returned snd_kcontrol_chip() for controls
    # registered by a component (as these are).
    files[name] = files[name].replace(old, b"snd_kcontrol_chip(kcontrol)")
    if dai_check:
        name = "mt6789/mt6789-dai-adda.c"
        if files[name].count(old) != 4:
            raise ValueError("unexpected legacy ADDA component lookups")
        files[name] = files[name].replace(old, b"snd_kcontrol_chip(kcontrol)")
    name = "mt6789/mt6789-afe-common.h"
    mark = b"int mt6789_dai_i2s_register(struct mtk_base_afe *afe);"
    if files[name].count(mark) != 1:
        raise ValueError("unexpected vendor I2S declarations")
    files[name] = files[name].replace(mark, mark +
        b"\nint mt6789_dai_i2s_get_share(struct mtk_base_afe *afe);"
        b"\nint mt6789_dai_i2s_set_priv(struct mtk_base_afe *afe);")
    name = "mt6789/mt-plat/aee.h"
    old = b"\nvoid monitor_hang_regist_ldt(void (*fn)(void))\n{"
    if files[name].count(old) != 1:
        raise ValueError("unexpected vendor AEE disabled branch")
    # The original no-feature branch is already a no-op. Give it local
    # linkage so it cannot emit one global definition per translation unit.
    files[name] = files[name].replace(old, b"\nstatic inline " + old[1:])
    if platform_check:
        port_platform(files)
    if fe_check:
        name = "common/mtk-afe-fe-dai.c"
        files[name] = port_fe_irq(files[name])
        namespace_common(files, name, 25)
    if sram_check:
        name = "mt6789/mtk-sram-manager.c"
        files[name] = port_sram(files[name])
        namespace_common(files, name, 4)
    if ack_check:
        name = "mt6789/mtk-sp-pcm-ops.c"
        files[name] = port_ack(files[name])
        namespace_common(files, name, 1)
    if tone_check:
        name = "mt6789/mt6789-misc-control.c"
        files[name] = port_tone(files[name])
        name = "mt6789/mt6789-afe-pcm.c"
        old = b"\tmt6789_add_misc_control(component);\n\treturn 0;"
        if files[name].count(old) != 1:
            raise ValueError("unexpected vendor tone registration call")
        files[name] = files[name].replace(old, b"\treturn mt6789_add_misc_control(component);")
    if alsa_buffer_check:
        pcm, fe = "mt6789/mt6789-afe-pcm.c", "common/mtk-afe-fe-dai.c"
        files[pcm], files[fe] = keep_alsa_buffers(files[pcm], files[fe])
        del files["common/mtk-mmap-ion.h"]
    if notifier_check:
        # Keep the real atomic chain, not a fabricated success/fallback.
        namespace_common(files, "mt6789/mtk-afe-external.c", 9)
    if module_check:
        name = "mt6789/mt6789-afe-pcm.c"
        files[name] = validate_afe_irq_status(require_audio_owner(balance_probe_regmap_pm(
            manage_afe_irq_wake(files[name]))))
    files["mt6789/Makefile"] = (
        b"obj-m := t70_afe_i2s_slice.o\n"
        b"t70_afe_i2s_slice-y := mt6789-dai-i2s.o mt6789-afe-control.o mt6789-afe-gpio.o"
        + (b" mt6789-afe-clk.o" if exact_clock_check else b"")
        + (b" mt6789-afe-pcm.o" if pcm_check else b"")
        + ("".join(f" mt6789-dai-{dai}.o" for dai in DAIS).encode() if dai_check else b"")
        + b"\nccflags-y += -I$(src)\n"
        + (b"ccflags-y += -I$(src)/../include\n" if pcm_check else b""))
    if platform_check:
        files["mt6789/mtk-afe-platform-driver.c"] = files.pop("common/mtk-afe-platform-driver.c")
        files["mt6789/Makefile"] = files["mt6789/Makefile"].replace(
            b"\nccflags-y", b" mtk-afe-platform-driver.o\nccflags-y", 1)
        files["mt6789/Makefile"] += b"ccflags-y += -I$(src)/../common\n"
    if fe_check:
        files["mt6789/mtk-afe-fe-dai.c"] = files.pop("common/mtk-afe-fe-dai.c")
        files["mt6789/Makefile"] = files["mt6789/Makefile"].replace(
            b"\nccflags-y", b" mtk-afe-fe-dai.o\nccflags-y", 1)
    if sram_check:
        files["mt6789/Makefile"] = files["mt6789/Makefile"].replace(
            b"\nccflags-y", b" mtk-sram-manager.o\nccflags-y", 1)
    if ack_check:
        files["mt6789/Makefile"] = files["mt6789/Makefile"].replace(
            b"\nccflags-y", b" mtk-sp-pcm-ops.o\nccflags-y", 1)
    if tone_check:
        files["mt6789/Makefile"] = files["mt6789/Makefile"].replace(
            b"\nccflags-y", b" mt6789-misc-control.o\nccflags-y", 1)
    if notifier_check:
        files["mt6789/Makefile"] = files["mt6789/Makefile"].replace(
            b"\nccflags-y", b" mtk-afe-external.o\nccflags-y", 1)
    if module_check:
        files["Makefile"] = b"""KERNELDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all clean
all:
	$(MAKE) -C "$(KERNELDIR)" M="$(CURDIR)/mt6789" modules
clean:
	$(MAKE) -C "$(KERNELDIR)" M="$(CURDIR)/mt6789" clean
"""
        files["dkms.conf"] = rb"""PACKAGE_NAME="t70-mt6789-afe"
PACKAGE_VERSION="0.1.3"
BUILT_MODULE_NAME[0]="t70_afe_i2s_slice"
BUILT_MODULE_LOCATION[0]="mt6789"
DEST_MODULE_LOCATION[0]="/updates/dkms"
BUILD_EXCLUSIVE_ARCH="aarch64"
BUILD_EXCLUSIVE_KERNEL="^7[.]0[.]"
MAKE[0]="make KERNELDIR='\$kernel_source_dir'"
AUTOINSTALL="no"
"""
    destination.mkdir()
    for name, data in files.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    parser.add_argument("--exact-clock-check", action="store_true")
    parser.add_argument("--pcm-check", action="store_true")
    parser.add_argument("--dai-check", action="store_true")
    parser.add_argument("--platform-check", action="store_true")
    parser.add_argument("--fe-check", action="store_true")
    parser.add_argument("--sram-check", action="store_true")
    parser.add_argument("--ack-check", action="store_true")
    parser.add_argument("--tone-check", action="store_true")
    parser.add_argument("--alsa-buffer-check", action="store_true")
    parser.add_argument("--notifier-check", action="store_true")
    parser.add_argument("--module-check", action="store_true")
    args = parser.parse_args()
    try:
        stage(args.reference, args.destination, args.exact_clock_check, args.pcm_check,
              args.dai_check, args.platform_check, args.fe_check, args.sram_check, args.ack_check, args.tone_check,
              args.alsa_buffer_check, args.notifier_check, args.module_check)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"I2S source staging refused: {error}\n")
