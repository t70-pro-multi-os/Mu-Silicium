# SD CID response contract

SdGetCid issues synchronous CMD10/R2 with RCA in argument bits 31:16. It clears
the entire SD_CID output before submission, then on success copies 15 bytes from
the fixed response registers into CID bytes 1..15. The omitted low byte stays
zero. R2 is a fixed register response, not a data-buffer transfer: there is no
returned CID byte-count field to validate as a short packet. Transport/CRC
errors must be reported by PassThru; a provider returning success with arbitrary
register contents cannot be diagnosed as a truncated response by this interface.

TestCidResponseContract covers 256 byte patterns against success, CRC, timeout,
device error and no media. Even failed responses containing register data leave
CID completely zero, not stale. Guards verify the exact output extent, and RCA,
R2 response type and zero data-transfer lengths are checked. Successive fetches
replace all payload bytes. After child invalidation, DiskInfo Inquiry refuses
to expose retained identity bytes and leaves the caller buffer unchanged.

The actual DiscoverUserArea call path returns immediately when SdGetCid fails,
before model-name construction and reselect. Existing child-tombstone tests and
these direct helper tests do not establish physical same-presence card-swap
detection or a hardware CID CRC check. No production fault was reproduced.
Focused SdDxe host suite: 74,118 checks, ASan/UBSan. No firmware build or device
access. Concurrent USB/DSDT changes were not touched.
