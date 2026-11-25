#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the external-only T70 MSDC object from the pinned kernel reference."""
import argparse
from pathlib import Path
import re
import subprocess

REV = "a71515dbaf0d539ffb36416f14186dad17eef743"
DRIVER = "drivers/mmc/host/mtk-sd.c"


def sd_only(text):
    def cut(first, following):
        nonlocal text
        if text.count(first) != 1 or text.count(following) != 1:
            raise ValueError("unexpected MMC CQE function layout")
        start = text.index(first)
        end = text.index(following, start)
        text = text[:start] + text[end:]

    cut("static irqreturn_t msdc_cmdq_irq(", "static irqreturn_t msdc_irq(")
    cut("static void msdc_hs400_enhanced_strobe(", "static const struct mmc_host_ops mt_msdc_ops")
    cut("static const struct cqhci_host_ops msdc_cmdq_ops", "static void msdc_of_property_parse(")
    for paragraph in (
        r"(?ms)^\t\tif \(\(mmc->caps2 & MMC_CAP2_CQE\) &&.*?^\t\t\}\n\n",
        r'(?m)^\tif \(of_property_read_bool\(pdev->dev.of_node,\n[ \t]+"supports-cqe"\)\)\n'
        r"\t\thost->cqhci = true;\n\telse\n\t\thost->cqhci = false;\n",
        r"(?m)^\tif \(host->cqhci\)\n\t\tmmc->caps2 \|= MMC_CAP2_CQE \| MMC_CAP2_CQE_DCMD;\n",
        r"(?ms)^\tif \(mmc->caps2 & MMC_CAP2_CQE\) \{\n\t\thost->cq_host = .*?^\t\}\n\n",
        r"(?ms)^\tint ret;\n\tu32 val;\n\n\tif \(mmc->caps2 & MMC_CAP2_CQE\) \{\n"
        r"\t\tret = cqhci_suspend\(mmc\);.*?^\t\}\n\n",
    ):
        text, count = re.subn(paragraph, "", text)
        if count != 1:
            raise ValueError(f"unexpected MMC CQE branch: {paragraph!r}")
    for line in ('#include "cqhci.h"\n', "\t.hs400_enhanced_strobe = msdc_hs400_enhanced_strobe,\n",
                 "\tstruct cqhci_host *cq_host;\n", "\tu32 cq_ssc1_time;\n"):
        if text.count(line) != 1:
            raise ValueError(f"unexpected CQE field: {line!r}")
        text = text.replace(line, "")
    text, count = re.subn(r"(?m)^\tbool cqhci;[^\n]*\n", "", text)
    if count != 1 or re.search(r"\b(cqhci_\w+|MMC_CAP2_CQE\w*)\b", text):
        raise ValueError("private CQHCI dependency still present")
    return text


def guard_dma_map(text):
    a = text.index("\nstatic void msdc_prepare_data(")
    b = text.index("\nstatic bool msdc_data_prepared(", a)
    body = text[a:b]
    if "dma_map_sg(host->dev, data->sg, data->sg_len," not in body:
        raise ValueError("unexpected stock DMA prepare function")
    return text[:a] + """
static bool t70_msdc_dma_map_valid(struct msdc_host *host, struct mmc_data *data)
{
	struct scatterlist *sg;
	unsigned int i, len, max_len = host->dev_comp->support_64g ?
				       BDMA_DESC_BUFLEN_EXT : BDMA_DESC_BUFLEN;
	u64 addr, limit = BIT_ULL(host->dev_comp->support_64g ? 36 : 32);
	u64 left = (u64)data->blocks * data->blksz;

	if (data->sg_count <= 0 || data->sg_count > data->sg_len ||
	    data->sg_count > MAX_BD_NUM || !left)
		return false;
	for_each_sg(data->sg, sg, data->sg_count, i) {
		addr = sg_dma_address(sg);
		len = sg_dma_len(sg);
		/* The descriptor stores exactly this width; never truncate a map. */
		if (!len || len > max_len || len > left ||
		    addr >= limit || len > limit - addr)
			return false;
		left -= len;
	}
	return left == 0;
}

static void msdc_prepare_data(struct msdc_host *host, struct mmc_data *data)
{
	if (data->host_cookie & MSDC_PREPARE_FLAG)
		return;
	data->sg_count = 0;
	if (!data->sg || !data->sg_len || data->sg_len > MAX_BD_NUM)
		return;
	data->sg_count = dma_map_sg(host->dev, data->sg, data->sg_len,
				    mmc_get_dma_dir(data));
	if (!data->sg_count)
		return;
	if (!t70_msdc_dma_map_valid(host, data)) {
		dma_unmap_sg(host->dev, data->sg, data->sg_len, mmc_get_dma_dir(data));
		data->sg_count = 0;
		return;
	}
	data->host_cookie |= MSDC_PREPARE_FLAG;
}
""" + text[b:]


def classify_sd_completion(text):
    a = text.index("\nstatic void msdc_data_xfer_done(")
    b = text.index("\nstatic void msdc_set_buswidth(", a)
    body = text[a:b]
    for old, new in (
        ("\tbool done;", "\tbool done, dma_timeout;"),
        ('\t\tif (ret)\n\t\t\tdev_dbg(host->dev, "DMA stop timed out\\n");',
         '\t\tdma_timeout = ret != 0;\n'
         '\t\tif (ret)\n\t\t\tdev_dbg(host->dev, "DMA stop timed out\\n");'),
        ('\t\tif (ret)\n\t\t\tdev_dbg(host->dev, "DMA inactive timed out\\n");',
         '\t\tdma_timeout |= ret != 0;\n'
         '\t\tif (ret)\n\t\t\tdev_dbg(host->dev, "DMA inactive timed out\\n");'),
        ("\t\tif ((events & MSDC_INT_XFER_COMPL) && (!stop || !stop->error)) {",
         "\t\tret = t70_msdc_completion_status(events, stop ? stop->error : 0, dma_timeout);\n"
         "\t\tif (!ret) {"),
        ("\t\t\tif (events & MSDC_INT_DATTMO)\n\t\t\t\tdata->error = -ETIMEDOUT;\n"
         "\t\t\telse if (events & MSDC_INT_DATCRCERR)\n\t\t\t\tdata->error = -EILSEQ;",
         "\t\t\tdata->error = ret;"),
    ):
        if body.count(old) != 1:
            raise ValueError("unexpected SD completion classification")
        body = body.replace(old, new)
    helper = """
static int t70_msdc_completion_status(u32 events, int stop_error, bool dma_timeout)
{
	if (dma_timeout || (events & MSDC_INT_DATTMO))
		return -ETIMEDOUT;
	if (events & (MSDC_INT_DMA_BDCSERR | MSDC_INT_DMA_GPDCSERR | MSDC_INT_DMA_PROTECT))
		return -EIO;
	if (events & MSDC_INT_DATCRCERR)
		return -EILSEQ;
	if (stop_error)
		return stop_error;
	return (events & MSDC_INT_XFER_COMPL) ? 0 : -EIO;
}
"""
    return text[:a] + helper + body + text[b:]


def guard_sd_watchdog(text):
    field = "\tstruct delayed_work req_timeout;"
    if text.count(field) != 1:
        raise ValueError("unexpected SD watchdog state")
    text = text.replace(field, field + "\n\tunsigned long req_deadline;")

    def arm_before(text, name, slot, value):
        a = text.index("\nstatic void " + name + "(")
        b = text.index("\n}\n", a)
        body = text[a:b]
        old_arm = "\tmod_delayed_work(system_wq, &host->req_timeout, DAT_TIMEOUT);\n"
        assign = f"\tWARN_ON(host->{slot});\n\thost->{slot} = {value};"
        if body.count(old_arm) != 1 or body.count(assign) != 1:
            raise ValueError(f"unexpected SD watchdog arming in {name}")
        body = body.replace(old_arm, "").replace(assign,
            "\tspin_lock_irqsave(&host->lock, flags);\n" + assign
            + "\n\thost->req_deadline = jiffies + DAT_TIMEOUT;\n"
            + old_arm + "\tspin_unlock_irqrestore(&host->lock, flags);")
        if slot == "data":
            body = body.replace("\tbool read;", "\tunsigned long flags;\n\tbool read;", 1)
        return text[:a] + body + text[b:]

    text = arm_before(text, "msdc_start_command", "cmd", "cmd")
    text = arm_before(text, "msdc_start_data", "data", "data")
    a = text.index("\nstatic void msdc_request_timeout(")
    b = text.index("\nstatic void __msdc_enable_sdio_irq(", a)
    if "/* simulate HW timeout status */" not in text[a:b]:
        raise ValueError("unexpected SD watchdog handler")
    return text[:a] + """
static void msdc_request_timeout(struct work_struct *work)
{
	struct msdc_host *host = container_of(work, struct msdc_host,
					     req_timeout.work);
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	struct mmc_data *data;
	unsigned long flags;

	/* This port owns a dedicated IRQ. An already-running IRQ may finish
	 * the request or arm the following stage before disable_irq returns.
	 */
	disable_irq(host->irq);
	spin_lock_irqsave(&host->lock, flags);
	mrq = host->mrq;
	cmd = host->cmd;
	data = host->data;
	if (time_before(jiffies, host->req_deadline))
		mrq = NULL;
	spin_unlock_irqrestore(&host->lock, flags);
	if (mrq && cmd)
		msdc_cmd_done(host, MSDC_INT_CMDTMO, mrq, cmd);
	else if (mrq && data)
		msdc_data_xfer_done(host, MSDC_INT_DATTMO, mrq, data);
	enable_irq(host->irq);
}
""" + text[b:]


def stage(repo, dest):
    def show(path):
        return subprocess.check_output(["git", "-C", repo, "show", f"{REV}:{path}"])

    text = show(DRIVER).decode()
    begin = text.index("static const struct mtk_mmc_compatible mt2701_compat")
    end = text.index("MODULE_DEVICE_TABLE(of, msdc_of_ids);", begin)
    common = text[begin:end]
    m = re.search(r"(?ms)^static const struct mtk_mmc_compatible mt6779_compat = \{.*?^\};", common)
    if not m or '"mediatek,mt6779-mmc", .data = &mt6779_compat' not in common:
        raise ValueError("unexpected baseline MMC profile")
    text = text[:begin] + m[0].replace("mt6779_compat", "t70_ext_sd_compat") + """

static const struct of_device_id msdc_of_ids[] = {
	{ .compatible = "teclast,t70pro-mt6789-ext-sd",
	  .data = &t70_ext_sd_compat },
	{ }
};
""" + text[end:]
    begin = text.index("static int msdc_drv_probe(")
    end = text.index("static void msdc_drv_remove(", begin)
    probe = text[begin:end]
    mark = "\t/* Allocate MMC host for this device */"
    if probe.count(mark) != 1:
        raise ValueError("unexpected MMC probe")
    probe = probe.replace(mark, """	/* Only the removable board slot, never the internal MMC/CQE path. */
	if (!of_device_is_compatible(pdev->dev.of_node, "teclast,t70pro-mt6789-ext-sd") ||
	    !of_property_read_bool(pdev->dev.of_node, "no-mmc") ||
	    of_property_read_bool(pdev->dev.of_node, "non-removable") ||
	    of_property_read_bool(pdev->dev.of_node, "supports-cqe"))
		return -EINVAL;

""" + mark)
    before = """	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		host->top_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(host->top_base))
			host->top_base = NULL;
	}"""
    after = """	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res || res->start != 0x11ef0000 || resource_size(res) != 0x1000)
		return -EINVAL;
	host->top_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(host->top_base))
		return PTR_ERR(host->top_base);"""
    if probe.count(before) != 1:
        raise ValueError("unexpected MMC top resource path")
    probe = probe.replace(before, after)
    primary = """	host->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->base))
		return PTR_ERR(host->base);"""
    if probe.count(primary) != 1:
        raise ValueError("unexpected MMC primary resource path")
    probe = probe.replace(primary, """	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x11240000 || resource_size(res) != 0x1000)
		return -EINVAL;
""" + primary)
    supply = """	ret = mmc_regulator_get_supply(mmc);
	if (ret)
		return ret;"""
    if probe.count(supply) != 1:
        raise ValueError("unexpected MMC supply path")
    probe = probe.replace(supply + "\n\n", "")
    check = "\tres = platform_get_resource(pdev, IORESOURCE_MEM, 0);"
    if probe.count(check) != 1:
        raise ValueError("unexpected MMC primary mapping position")
    probe = probe.replace(check, supply + """
	if (IS_ERR_OR_NULL(mmc->supply.vmmc) || IS_ERR_OR_NULL(mmc->supply.vqmmc))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "T70 external SD requires VMMC and VQMMC\\n");

""" + check)
    parse = """	ret = mmc_of_parse(mmc);
	if (ret)
		return ret;"""
    if probe.count(parse) != 1:
        raise ValueError("unexpected MMC OF parsing path")
    probe = probe.replace(parse, parse + """
	if (!(mmc->caps & MMC_CAP_4_BIT_DATA) || (mmc->caps & MMC_CAP_8_BIT_DATA))
		return dev_err_probe(&pdev->dev, -EINVAL, "T70 requires a four-bit SD bus\\n");
	if (!mmc_host_can_gpio_cd(mmc))
		return dev_err_probe(&pdev->dev, -ENODEV, "T70 SD card-detect GPIO missing\\n");
""".rstrip())
    text = text[:begin] + probe + text[end:]
    for old, new in ((".remove_new = msdc_drv_remove,", ".remove = msdc_drv_remove,"),
                     ("mmc_can_gpio_cd(mmc)", "mmc_host_can_gpio_cd(mmc)"),
                     ('.name = "mtk-msdc",', '.name = "t70-mt6789-ext-sd",')):
        if text.count(old) != 1:
            raise ValueError(f"unexpected kernel driver field: {old}")
        text = text.replace(old, new)
    text = guard_sd_watchdog(classify_sd_completion(guard_dma_map(sd_only(text))))
    add_host_fail = "\nend:\n\tpm_runtime_disable(host->dev);\nrelease:"
    if text.count(add_host_fail) != 1:
        raise ValueError("unexpected MMC host registration failure path")
    text = text.replace(add_host_fail,
        "\nend:\n\tdevm_free_irq(&pdev->dev, host->irq, host);"
        "\n\tpm_runtime_disable(host->dev);\nrelease:")
    old = "\tplatform_set_drvdata(pdev, NULL);\n\tmmc_remove_host(mmc);\n\tmsdc_deinit_hw(host);"
    if text.count(old) != 1:
        raise ValueError("unexpected external SD removal path")
    text = text.replace(old,
        "\tmmc_remove_host(mmc);\n"
        "\t/* Drain the watchdog while its dedicated IRQ is still registered. */\n"
        "\tcancel_delayed_work_sync(&host->req_timeout);\n"
        "\tdevm_free_irq(&pdev->dev, host->irq, host);\n"
        "\tplatform_set_drvdata(pdev, NULL);\n\tmsdc_deinit_hw(host);")
    dest.mkdir()
    (dest / "t70_mt6789_ext_sd.c").write_text(text)
    (dest / "Kbuild").write_text("obj-m := t70_mt6789_ext_sd.o\n")
    (dest / "Makefile").write_text("""KERNELDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all clean
all:
\t$(MAKE) -C "$(KERNELDIR)" M="$(CURDIR)" modules
clean:
\t$(MAKE) -C "$(KERNELDIR)" M="$(CURDIR)" clean
""")
    (dest / "dkms.conf").write_text(r"""PACKAGE_NAME="t70-mt6789-ext-sd"
PACKAGE_VERSION="0.1.7"
BUILT_MODULE_NAME[0]="t70_mt6789_ext_sd"
DEST_MODULE_LOCATION[0]="/updates/dkms"
BUILD_EXCLUSIVE_ARCH="^(aarch64|arm64)$"
BUILD_EXCLUSIVE_KERNEL="^7[.]0[.]"
MAKE[0]="make KERNELDIR='\$kernel_source_dir'"
AUTOINSTALL="no"
""")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("reference", type=Path)
    ap.add_argument("destination", type=Path)
    x = ap.parse_args()
    try:
        stage(x.reference, x.destination)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        ap.exit(2, f"MMC staging refused: {error}\n")
