#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the pinned T70 WLAN chip data for a generic ARM64 object-only build."""
import argparse
import io
from pathlib import Path, PurePosixPath
import subprocess
import tarfile

REV = "a71515dbaf0d539ffb36416f14186dad17eef743"
BASE = "mindone/modules/wlan_drv_gen4m_6789"
CHIP = "chips/soc2_1x1/soc2_1x1.c"
FW = "chips/common/fw_dl.c"
PDMA = tuple(f"os/linux/hif/common/{unit}_pdma.c" for unit in ("hal", "kal", "dbg"))
DISPLAY_HEADER = "mindone/modules/hynitron_touch/mtk_disp_notify.h"
KBUILD = """obj-m := chips/soc2_1x1/soc2_1x1.o
ccflags-y := -DLINUX -DSOC2_1X1 -D_HIF_AXI -DCFG_SUPPORT_CONNAC2X=0 \\
 -DCFG_MTK_ANDROID_WMT=0 -DCFG_MTK_ANDROID_EMI=0 -I$(src)/include -I$(src)/include/nic \\
 -I$(src)/include/mgmt -I$(src)/include/chips -I$(src)/os \\
 -I$(src)/os/linux/include -I$(src)/os/linux/hif/common/include \\
 -I$(src)/os/linux/hif/axi/include -I$(src)/pinned
"""


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"unexpected pinned source near {old.strip()}")
    return text.replace(old, new)


def require_wlan_qualified_firmware(text):
    a = text.index("\nvoid soc2_1x1ConstructFirmwarePrio(")
    b = text.index("\nvoid soc2_1x1ConstructPatchName(", a)
    body = text[a:b]
    if body.count("kalGetFwFlavor(&aucFlavor[0]);") != 1 or body.count("kalSnprintf(") != 4:
        raise ValueError("unexpected WLAN firmware candidates")
    start = body.index("\n{")
    return text[:a] + body[:start] + """
{
	uint8_t flavor = 0;
	int ret;

	/* Stock T70 ships only the two A-die-qualified executable names.
	 * Do not guess the die or fall back to an unqualified image.
	 */
	if (!pucNameIdx || !apucName || *pucNameIdx >= ucMaxNameIdx ||
	    !apucName[*pucNameIdx] || kalGetFwFlavor(&flavor) <= 0 ||
	    (flavor != 'd' && flavor != 'e'))
		return;
	ret = snprintf((char *)apucName[*pucNameIdx], CFG_FW_NAME_MAX_LEN,
		       "%s_%u%c_1.bin", apucSoc2_1x1FwName[0], CFG_WIFI_IP_SET, flavor);
	if (ret >= 0 && ret < CFG_FW_NAME_MAX_LEN)
		++*pucNameIdx;
}
""" + text[b:]


def validate_wlan_firmware_trailer(text):
    a = text.index("\nuint32_t wlanGetConnacTailerInfo(")
    b = text.index("\n/*----------------------------------------------------------------------------*/", a)
    body = text[a:b]
    # Retain the vendor release reader, but only call it after validating
    # the entire release-info region and its destination string capacity.
    helper = """
static bool t70_wlan_release_info_valid(const uint8_t *buf, size_t bytes,
				      size_t manifest_capacity)
{
	struct HEADER_RELEASE_INFO header;
	size_t left, record;

	if (bytes < RELEASE_INFO_SEPARATOR_LEN + sizeof(header))
		return false;
	buf += RELEASE_INFO_SEPARATOR_LEN;
	bytes -= RELEASE_INFO_SEPARATOR_LEN;
	kalMemCopy(&header, buf, sizeof(header));
	left = header.u2Len;
	buf += sizeof(header);
	if (left > bytes - sizeof(header))
		return false;
	while (left) {
		if (left < sizeof(header))
			return false;
		kalMemCopy(&header, buf, sizeof(header));
		record = sizeof(header) + (size_t)header.u2Len + header.ucPaddingLen;
		if (record > left ||
		    ((header.ucTag == 1 || header.ucTag == 2) &&
		     header.u2Len >= manifest_capacity))
			return false;
		buf += record;
		left -= record;
	}
	return true;
}
"""
    start = body.index("\n{")
    if "pucImgPtr += prRegTailer->u4Len;" not in body or "fwDlGetReleaseInfoSection" not in body:
        raise ValueError("unexpected Connac firmware trailer")
    replacement = body[:start] + """
{
	const uint8_t *image = prFwBuffer;
	struct TAILER_COMMON_FORMAT_T common;
	struct TAILER_REGION_FORMAT_T region;
	size_t common_at, regions_at, regions_size, payload = 0;
	uint32_t i;

	if (!prVerInfo || !image || u4FwSize < sizeof(common))
		return WLAN_STATUS_FAILURE;
	common_at = u4FwSize - sizeof(common);
	kalMemCopy(&common, image + common_at, sizeof(common));
	if (!common.ucRegionNum || common.ucRegionNum > MAX_FWDL_SECTION_NUM)
		return WLAN_STATUS_FAILURE;
	regions_size = (size_t)common.ucRegionNum * sizeof(region);
	if (regions_size > common_at)
		return WLAN_STATUS_FAILURE;
	regions_at = common_at - regions_size;
	for (i = 0; i < common.ucRegionNum; i++) {
		kalMemCopy(&region, image + regions_at + i * sizeof(region), sizeof(region));
		if (region.u4Len > regions_at - payload)
			return WLAN_STATUS_FAILURE;
		payload += region.u4Len;
	}
	if (common.ucFormatFlag && payload < regions_at &&
	    !t70_wlan_release_info_valid(image + payload, regions_at - payload,
					 sizeof(prVerInfo->aucReleaseManifest)))
		return WLAN_STATUS_FAILURE;

	kalMemCopy(&prVerInfo->rCommonTailer, &common, sizeof(common));
	kalMemCopy(prVerInfo->rRegionTailers, image + regions_at, regions_size);
	kalMemZero(prVerInfo->aucReleaseManifest, sizeof(prVerInfo->aucReleaseManifest));
	if (common.ucFormatFlag && payload < regions_at)
		fwDlGetReleaseInfoSection(prVerInfo, (uint8_t *)image + payload);
	return WLAN_STATUS_SUCCESS;
}
"""
    return text[:a] + helper + replacement + text[b:]


def bound_wlan_diagnostics(chip, loader):
    anchor = "\t.getFwDlInfo = asicGetFwDlInfo,"
    a = chip.index("\nstruct FWDL_OPS_T")
    chip = replace_once(chip, anchor, "\t.getFwDlInfo = t70_wlan_chip_info,")
    chip = chip[:a] + """
static uint32_t t70_wlan_chip_info(struct ADAPTER *adapter, char *buf, int size)
{
	const struct WIFI_VER_INFO *ver;
	const struct TAILER_COMMON_FORMAT_T *info;
	size_t used;

	if (!adapter || !buf || size <= 0)
		return 0;
	ver = &adapter->rVerInfo;
	info = &ver->rCommonTailer;
	used = scnprintf(buf, size, "Tailer Ver[%u:%u] %.10s (%.15s) info %u:E%u\\n",
			 info->ucFormatVer, info->ucFormatFlag, info->aucRamVersion,
			 info->aucRamBuiltDate, info->ucChipInfo, info->ucEcoCode + 1);
	if (info->ucFormatFlag)
		used += scnprintf(buf + used, size - used, "Release manifest: %.255s\\n",
				  ver->aucReleaseManifest);
	return used;
}
""" + chip[a:]
    if loader is None:
        return chip, None
    a = loader.index("\nuint32_t fwDlGetFwdlInfo(")
    b = loader.index("\nvoid fwDlGetReleaseInfoSection(", a)
    body = loader[a:b]
    if body.count("u4Offset += snprintf(") != 4 or body.count("kalSnprintf(") != 4:
        raise ValueError("unexpected vendor WLAN diagnostic formatter")
    body = body.replace("u4Offset += snprintf(", "u4Offset += scnprintf(")
    body = replace_once(body, "struct WIFI_VER_INFO *prVerInfo = &prAdapter->rVerInfo;",
        "struct WIFI_VER_INFO *prVerInfo;")
    start = body.index("\tuint8_t aucBuf[32], aucDate[32];")
    end = body.index("\n\tu4Offset += scnprintf(", start)
    body = body[:start] + """
	if (!prAdapter || !pcBuf || i4TotalLen <= 0)
		return 0;
	prVerInfo = &prAdapter->rVerInfo;
	prFwDlOps = prAdapter->chip_info ? prAdapter->chip_info->fw_dl_ops : NULL;
""" + body[end:]
    body = replace_once(body, "%s-%u.%u.%u[DEC] (%s)", "%.4s-%u.%u.%u[DEC] (%.16s)")
    body = replace_once(body, "\t\t\taucBuf,", "\t\t\tprVerInfo->aucFwBranchInfo,")
    body = replace_once(body, "prVerInfo->ucFwBuildNumber, aucDate);",
                        "prVerInfo->ucFwBuildNumber, prVerInfo->aucFwDateCode);")
    a_cb = body.index("\tif (prFwDlOps->getFwDlInfo)")
    b_cb = body.index("\n\n\tif (!prVerInfo->fgPatchIsDlByDrv)", a_cb)
    body = body[:a_cb] + """	if (prFwDlOps && prFwDlOps->getFwDlInfo && u4Offset + 1 < (uint32_t)i4TotalLen) {
		uint32_t remaining = i4TotalLen - u4Offset - 1;
		uint32_t count = prFwDlOps->getFwDlInfo(prAdapter, pcBuf + u4Offset, remaining + 1);

		u4Offset += count < remaining ? count : remaining;
		pcBuf[u4Offset] = '\\0';
	}""" + body[b_cb:]
    start = body.index("\n\tkalSnprintf(aucBuf")
    end = body.index("\n\tu4Offset += scnprintf(", start)
    body = body[:start] + "\n" + body[end:]
    body = replace_once(body, "Patch platform %s version 0x%04X %s", "Patch platform %.4s version 0x%04X %.16s")
    body = replace_once(body, "aucBuf, prVerInfo->rPatchHeader.u4PatchVersion,\n\t\t\t     aucDate);",
        "prVerInfo->rPatchHeader.aucPlatform, prVerInfo->rPatchHeader.u4PatchVersion,\n"
        "\t\t\t     prVerInfo->rPatchHeader.aucBuildDate);")
    return chip, loader[:a] + body + loader[b:]


def stage(reference, dest, with_loader=False, with_pdma=False):
    with_loader = with_loader or with_pdma
    sources = {CHIP, FW} if with_loader else {CHIP}
    if with_pdma:
        sources.update(PDMA)
    git = ["git", "-C", str(reference)]
    data = subprocess.check_output(git + ["archive", REV, BASE])
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for item in archive:
            if not item.isfile():
                continue
            rel = PurePosixPath(item.name).relative_to(BASE)
            if ".." in rel.parts:
                continue
            if str(rel) in sources or (rel.suffix == ".h" and rel.parts[0] in ("include", "os")):
                files[str(rel)] = archive.extractfile(item).read()
    if len(files) < 200:
        raise ValueError("incomplete pinned WLAN headers")
    config = files["include/config.h"].decode()
    for option in ("CFG_SUPPORT_QA_TOOL", "CFG_SUPPORT_TX_BF"):
        config = replace_once(config, f"#define {option}\t1\n", f"#define {option}\t0\n")
    files["include/config.h"] = config.encode()

    # The event layout is declared even without QA. Keep its original size.
    name = "include/nic_cmd_event.h"
    event = files[name].decode()
    const = "#define MAX_UEVENT_LEN\t300\n"
    event = replace_once(event, const, "")
    event = replace_once(event, "\nstruct EVENT_REPORT_U_EVENT", "\n" + const + "\nstruct EVENT_REPORT_U_EVENT")
    files[name] = event.encode()

    # A private WEXT calibration command invokes vendor WMT reset and waits
    # unboundedly. Do not compile or replace that debug hook in this probe.
    chip = files[CHIP].decode()
    a = chip.index("\nvoid soc2_1x1wlanCalDebugCmd(")
    b = chip.index("\nstruct BUS_INFO soc2_1x1_bus_info", a)
    if "mtk_wcn_wmt_do_reset_only(WMTDRV_TYPE_WIFI);" not in chip[a:b]:
        raise ValueError("unexpected WLAN debug hook")
    chip = replace_once(chip[:a] + chip[b:], "\n\t.calDebugCmd = soc2_1x1wlanCalDebugCmd,", "")
    chip = require_wlan_qualified_firmware(chip)
    if with_loader:
        # The generic staging target has no audited EMI provider. A missing
        # implementation cannot report that a requested segment was written.
        fw = files[FW].decode()
        a = fw.index("\nuint32_t wlanDownloadEMISection(")
        b = fw.index("\n#if CFG_SUPPORT_COMPRESSION_FW_OPTION", a)
        fw = fw[:a] + replace_once(
            fw[a:b], "#endif /* CFG_MTK_ANDROID_EMI */\n\treturn WLAN_STATUS_SUCCESS;",
            "\treturn WLAN_STATUS_SUCCESS;\n#else\n\treturn WLAN_STATUS_NOT_SUPPORTED;\n"
            "#endif /* CFG_MTK_ANDROID_EMI */") + fw[b:]
        fw = replace_once(fw,
                         "/* escape from loop if any pending error occurs */\n"
                         "\t\t\tif (u4Status == WLAN_STATUS_FAILURE)",
                         "/* Do not let another section overwrite an error status. */\n"
                         "\t\t\tif (u4Status != WLAN_STATUS_SUCCESS)")
        fw = validate_wlan_firmware_trailer(fw)
        chip, fw = bound_wlan_diagnostics(chip, fw)
        files[FW] = fw.encode()
    else:
        chip, _ = bound_wlan_diagnostics(chip, None)
    files[CHIP] = chip.encode()
    if with_pdma:
        name = PDMA[0]
        hal = files[name].decode()
        old = "del_timer_sync(&prHifInfo->rSerTimer);"
        if hal.count(old) != 2:
            raise ValueError("unexpected WLAN recovery timer cancellation sites")
        # Final uninit must block rearming. The live recovery path still
        # needs to rearm its timer later, so it only deletes synchronously.
        a = hal.index("\nvoid halHifSwInfoUnInit(")
        b = hal.index("\n}", a) + 2
        uninit = replace_once(hal[a:b], old, "timer_shutdown_sync(&prHifInfo->rSerTimer);")
        hal = hal[:a] + uninit + hal[b:]
        hal = replace_once(hal, old, "timer_delete_sync(&prHifInfo->rSerTimer);")
        hal = replace_once(hal, "from_timer(prHif, timer, rSerTimer)",
                           "timer_container_of(prHif, timer, rSerTimer)")
        files[name] = hal.encode()
        name = PDMA[1]
        kal = files[name].decode()
        bad = 'Cannot remap address[%pa].\\n",\n\t\t       __func__, addr);'
        if kal.count(bad) != 2:
            raise ValueError("unexpected PHY-address logging sites")
        kal = kal.replace(bad, bad.replace("__func__, addr", "__func__, &addr"))
        kal = replace_once(kal, 'Read CONSYS 0x%08x=0x%08x.\\n", addr, *val);',
                                'Read CONSYS %pa=0x%08x.\\n", &addr, *val);')
        kal = replace_once(kal, 'Write CONSYS 0x%08x=0x%08x.\\n", addr, val);',
                                'Write CONSYS %pa=0x%08x.\\n", &addr, val);')
        files[name] = kal.encode()
    files["pinned/mtk_disp_notify.h"] = subprocess.check_output(git + ["show", f"{REV}:{DISPLAY_HEADER}"])
    kbuild = KBUILD
    if with_loader:
        kbuild = kbuild.replace("obj-m := chips/soc2_1x1/soc2_1x1.o",
                                "obj-m := t70_wlan_chip_fw.o\n"
                                "t70_wlan_chip_fw-y := chips/soc2_1x1/soc2_1x1.o chips/common/fw_dl.o")
    if with_pdma:
        kbuild += "t70_wlan_chip_fw-y += " + " ".join(f"{name[:-2]}.o" for name in PDMA) + "\n"
    files["Kbuild"] = kbuild.encode()

    dest.mkdir()
    for name, payload in files.items():
        target = dest / name
        target.parent.mkdir(exist_ok=True, parents=True)
        target.write_bytes(payload)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference", type=Path)
    p.add_argument("destination", type=Path)
    p.add_argument("--with-loader", action="store_true")
    p.add_argument("--with-pdma", action="store_true")
    a = p.parse_args()
    try:
        stage(a.reference, a.destination, a.with_loader, a.with_pdma)
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        p.exit(2, f"WLAN chip staging refused: {error}\n")
