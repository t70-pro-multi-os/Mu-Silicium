#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage source for the T70 opt-in MT6983 I2C provider, not its hardware."""
import argparse
import gzip
import hashlib
import io
from pathlib import Path
import re
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
DEFAULT_REFERENCE = HERE.parents[4].parent / "kernel-reference"
REVISION = "a71515dbaf0d539ffb36416f14186dad17eef743"
SOURCE = "mindone/modules/i2c_mt65xx/i2c_mt65xx.c"
DIGEST = "b6c4ffc8f8cf413db5a0ab26cf32ae24fa043bdb29c6a0c9743d9e0bbc1efac2"


def once(text, pattern, replacement):
    result, count = re.subn(pattern, replacement, text, flags=re.M)
    if count != 1:
        raise ValueError(f"expected one I2C source replacement, got {count}: {pattern}")
    return result


def check_i2c_transfer_outcome(text):
    a = text.index("\nstatic int mtk_i2c_do_transfer(")
    b = text.index("\nstatic int mtk_i2c_transfer(", a)
    body = text[a:b]
    body = once(body, r"^\tint ret;$", "\tint ret, result;")
    mark = "\t/* Clear interrupt mask */"
    if body.count(mark) != 1:
        raise ValueError("unexpected I2C completion")
    old = "\tif (isDMA == true) {"
    c = body.index(old, body.index(mark))
    done = body[c:]
    for expr in ("i2c_put_dma_safe_msg_buf(dma_rd_buf, msgs, true);",
                 "i2c_put_dma_safe_msg_buf(dma_rd_buf, (msgs + 1), true);"):
        if done.count(expr) != 1:
            raise ValueError("unexpected I2C read bounce ownership")
        done = done.replace(expr, expr.replace(", true);", ", result == 0);"))
    done = once(done, r"^\tif \(i2c->irq_stat & \(I2C_HS_NACKERR \| I2C_ACKERR\)\) \{$",
                "\tif (result == -ENXIO) {")
    done = once(done,
                r"^\tif \(\(i2c->op != I2C_MASTER_WR\) &&$",
                "\tif (result)\n\t\treturn result;\n\tif ((i2c->op != I2C_MASTER_WR) &&")
    body = body[:c] + ("\tresult = t70_i2c_transfer_result(ret, READ_ONCE(i2c->irq_stat), restart_flag);\n\n"
                      + done)
    helper = """
static int t70_i2c_transfer_result(unsigned long waited, u16 irq_stat, u16 restart_flag)
{
	if (!waited)
		return -ETIMEDOUT;
	if (irq_stat & I2C_ARB_LOST)
		return -EAGAIN;
	if (irq_stat & (I2C_HS_NACKERR | I2C_ACKERR))
		return -ENXIO;
	return (irq_stat & (I2C_TRANSAC_COMP | restart_flag)) ? 0 : -EIO;
}
"""
    text = text[:a] + helper + body + text[b:]
    return once(text, r"^\tif \(i2c->ignore_restart_irq && \(i2c->irq_stat & restart_flag\)\) \{$",
        "\tif (i2c->irq_stat & I2C_ARB_LOST) {\n\t\tcomplete(&i2c->msg_complete);\n"
        "\t} else if (i2c->ignore_restart_irq && (i2c->irq_stat & restart_flag)) {")



def preserve_i2c_message_boundaries(text):
    # MT6983 already supports per-message repeated starts. The vendor bulk
    # path narrows the sum into i2c_msg.len and discards the original flags.
    # Leave WRRD (a distinct write/read operation) and normal DMA unchanged.
    text = once(text, r"^\tI2C_MASTER_CONTINUOUS_WR,\n", "")
    text = once(text, r"^\t\t\tif \(i2c->op == I2C_MASTER_CONTINUOUS_WR\)\n"
                      r"\t\t\t\tkfree\(msgs->buf\);\n", "")
    text = once(text, r"^\t} else if \(i2c->op == I2C_MASTER_CONTINUOUS_WR\) \{\n"
                      r"\t\tmtk_i2c_writew\(i2c, msgs->len / num, OFFSET_TRANSFER_LEN\);\n"
                      r"\t\tmtk_i2c_writew\(i2c, num, OFFSET_TRANSAC_LEN\);\n", "")
    text = once(text, r"^\t\t} else if \(i2c->op == I2C_MASTER_CONTINUOUS_WR\) \{\n"
                      r"(?s:.*?)^\t\t\twritel\(msgs->len, i2c->pdmabase \+ OFFSET_TX_LEN\);\n", "")
    text = once(text, r"^\t\t} else if \(i2c->op == I2C_MASTER_CONTINUOUS_WR\) \{\n"
                      r"\t\t\tdma_unmap_single\(i2c->dev, wpaddr,\n"
                      r"\t+ +msgs->len, DMA_TO_DEVICE\);\n", "")
    text = once(text, r"^\tif \(i2c->op == I2C_MASTER_CONTINUOUS_WR\) \{\n"
                      r"\t\tkfree\(msgs->buf\);\n\t}\n\n", "")
    text = once(text, r"^\tif \(\(i2c->op != I2C_MASTER_WR\) &&\n"
                      r"\t\t\t\(i2c->op != I2C_MASTER_CONTINUOUS_WR\) && \(isDMA == false\)\) \{$",
                      "\tif (i2c->op != I2C_MASTER_WR && !isDMA) {")
    text = once(text, r"^\tint i, j;\n\tu8 \*dma_multi_wr_buf;\n"
                      r"\tstruct i2c_msg multi_msg\[1\];\n", "")
    text = once(text, r"^\tif \(num > 1\) \{\n\t\tfor \(i = 0; i < num - 1; i\+\+\) \{\n"
                      r"(?s:.*?)^\t}\n\n(?=\tif \(i2c->auto_restart && num >= 2)", "")
    if "I2C_MASTER_CONTINUOUS_WR" in text or "dma_multi_wr_buf" in text:
        raise ValueError("incomplete removal of I2C bulk write")
    return text



def preserve_i2c_probe_supplier_errors(text):
    start = text.index("\nstatic int mtk_i2c_probe(")
    end = text.index("\nstatic void mtk_i2c_remove(", start)
    probe = text[start:end]
    probe = once(probe, r'^\ti2c->clk_arb = devm_clk_get\(&pdev->dev, "arb"\);\n'
                        r'\tif \(IS_ERR\(i2c->clk_arb\)\)\n\t\ti2c->clk_arb = NULL;',
                 lambda _: """	i2c->clk_arb = devm_clk_get_optional(&pdev->dev, "arb");
	if (IS_ERR(i2c->clk_arb))
		return dev_err_probe(&pdev->dev, PTR_ERR(i2c->clk_arb),
				     "cannot get arbitration clock\\n");""")
    mask = """	if (i2c->dev_comp->max_dma_support > 32) {
		ret = dma_set_mask(&pdev->dev,
				DMA_BIT_MASK(i2c->dev_comp->max_dma_support));
		if (ret) {
			dev_err(&pdev->dev, "dma_set_mask return error.\\n");
			return ret;
		}
	}
"""
    if probe.count(mask) != 2 or mask + "\n" + mask not in probe:
        raise ValueError("unexpected duplicated T70 I2C DMA mask setup")
    probe = probe.replace(mask + "\n" + mask, mask)
    return text[:start] + probe + text[end:]


def port(source):
    if hashlib.sha256(source).hexdigest() != DIGEST:
        raise ValueError("wrong vendor I2C source hash")
    text = source.decode("utf-8")
    # Linux 7.0 uses a void .remove. The vendor callback is already void.
    text = once(text, r"^(\s*)\.remove_new = mtk_i2c_remove,$",
                r"\1.remove = mtk_i2c_remove,")
    # Do not claim any device handled by the distribution's i2c-mt65xx.
    text = once(text, r"^(\s*)\.name = I2C_DRV_NAME,$",
                r'\1.name = "t70-mt6983-i2c",')
    text = once(text, r"(?ms)^static const struct of_device_id mtk_i2c_of_match\[\] = \{\n.*?^\};",
                '''static const struct of_device_id mtk_i2c_of_match[] = {
\t{ .compatible = "teclast,t70pro-mt6983-i2c", .data = &mt6983_compat },
\t{}
};''')
    # Remove unused SoC profiles instead of weakening compiler warnings.
    text = re.sub(r"(?ms)^static const struct mtk_i2c_compatible (?!mt6983_compat\b)"
                  r"\w+ = \{\n.*?^\};\n\n", "", text)
    # Refuse unusable timing instead of registering an adapter with no plan.
    text = once(text, r"mtk_i2c_set_speed\(struct mtk_i2c \*i2c, unsigned int parent_clk\)",
                "mtk_i2c_set_speed(struct mtk_i2c *i2c, unsigned long parent_clk)")
    text = once(text, r"^\ttarget_speed = i2c->speed_hz;\n\tparent_clk /= i2c->clk_src_div;$",
                """\tif (!parent_clk || parent_clk > ~0U ||
\t    !i2c->clk_src_div || !i2c->speed_hz)
\t\treturn -EINVAL;
\ttarget_speed = i2c->speed_hz;
\tparent_clk /= i2c->clk_src_div;""")
    text = once(text, r"^(\t\tclk_src = parent_clk / clk_div;)$",
                r"\1\n\t\tif (clk_src < 2)\n\t\t\treturn -EINVAL;")
    text = once(text, r"^\ti2c->ac_timing.inter_clk_div = \(\(clk_div - 1\) << 8\) \| \(clk_div - 1\);$",
                r"\tif (clk_div > max_clk_div)\n\t\treturn -EINVAL;\n\n\g<0>")
    text = preserve_i2c_message_boundaries(check_i2c_transfer_outcome(text))
    return preserve_i2c_probe_supplier_errors(text).encode("utf-8")


PACKAGE = "t70-mt6983-i2c-0.1.4"


def write_archive(destination, files):
    with destination.open("xb") as output, gzip.GzipFile(
            fileobj=output, filename="", mode="wb", mtime=0) as gz, tarfile.open(
                fileobj=gz, mode="w|") as archive:
        for name, data in sorted(files.items()):
            entry = tarfile.TarInfo(f"{PACKAGE}/{name}")
            entry.mode = 0o644
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path, help="new source directory or archive")
    parser.add_argument("--archive", action="store_true", help="write source-only tar.gz")
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    args = parser.parse_args()
    try:
        def reference(path):
            return subprocess.check_output(
                ["git", "-C", str(args.reference), "show", f"{REVISION}:{path}"])

        files = {"t70_mt6983_i2c.c": port(reference(SOURCE)),
                 "LICENSE.GPL-2.0": reference("LICENSES/preferred/GPL-2.0")}
        files.update({name: (HERE / "I2c" / name).read_bytes()
                      for name in ("Makefile", "dkms.conf")})
        if args.archive:
            write_archive(args.destination, files)
        else:
            args.destination.mkdir()
            for name, content in files.items():
                (args.destination / name).write_bytes(content)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"I2C source staging refused: {error}\n")


if __name__ == "__main__":
    main()
