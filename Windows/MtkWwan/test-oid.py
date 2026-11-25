#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Compile actual NDIS OID callbacks with narrow host shims.
import os
from pathlib import Path
import subprocess
import tempfile
here=Path(__file__).resolve().parent
source=(here/"Driver.c").read_text()
start=source.index("static NDIS_STATUS\nWwCopyQuery")
end=source.index("\nstatic NDIS_STATUS\nWwOidRequest",start)
callbacks=source[start:end]
harness=(here/"NdisOidCallbackHarness.c").read_text()
prefix,suffix=harness.split("/* PRODUCTION_CALLBACKS */",1)
with tempfile.TemporaryDirectory(prefix="t70-wwan-oid-") as temp:
    c=Path(temp)/"oid-test.c"
    exe=Path(temp)/"oid-test"
    c.write_text(prefix+callbacks+suffix)
    cc=os.environ.get("CC") or "gcc"
    subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
        "-fno-sanitize-recover=all","-I",str(here),str(c),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
