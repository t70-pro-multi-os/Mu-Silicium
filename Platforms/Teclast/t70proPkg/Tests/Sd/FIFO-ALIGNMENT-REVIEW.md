# SD FIFO alignment and tail bytes

2026-11-18. Msdc advertises IoAlign=4. SdDxe propagates that requirement into
BlockIo media and rejects nonzero-size reads whose buffer is not four-byte
aligned before calling PassThru. This is the external caller contract, even
though the low-level FIFO helper can tolerate unaligned pointers.

MsdcFifoRead uses byte accesses until the destination becomes four-byte aligned,
then aligned 32-bit stores, then byte accesses for the last one to three bytes.
Host tests exercise destination offsets 0..3 and helper lengths 0..129, checking
all produced bytes, exact FIFO consumption and surrounding canaries. Actual
MsdcPioRead tests also drain a FIFO-size chunk plus one/two/three tail bytes at
each alignment. SdDxe tests reject offsets 1..3 with no PassThru call.

MsdcPassThru itself currently lacks an explicit IoAlign rejection; this review
does not turn its tolerance into an advertised arbitrary-alignment guarantee.
No unsafe store or buffer overrun was reproduced, so production code was not
changed. These are host-memory/MMIO mocks, not electrical proof of byte-wide
FIFO transactions or permission to issue arbitrary short CMD17 card requests.

Host results: MsdcTest 17,729,521 and SdDxeMediaTest 28,698 ASan/UBSan checks.
No device access or full firmware build occurred.
