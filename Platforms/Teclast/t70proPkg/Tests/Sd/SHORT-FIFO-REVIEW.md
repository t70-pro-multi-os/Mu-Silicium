# Short FIFO completion regression

ShortFifoCompletionTests calls actual MsdcPassThru/MsdcPioRead for a 512-byte
CMD17 request. Mock delivery of 1, 3, 4, 128 or 508 bytes followed by transfer
complete returns EFI_ABORTED, never success. All 512 bytes, including the final
32-bit word, returns EFI_SUCCESS. An empty FIFO with transfer complete also
aborts; a permanently empty FIFO without completion reaches EFI_TIMEOUT.
CRC or data-timeout concurrent with completion takes precedence over success.

Ten cases check Packet.TransactionStatus, one command per request, cleared
CommandActive, preserved media readiness, copied-prefix bounds and untouched
suffix/canaries. Each then issues a separate successful command with the mock
configured to supply new data only after the next command starts. This does
not prove hardware recovery from a stuck controller or card; it verifies
software state is not left locked and no implicit follow-up command was sent.

The host suite passed 17,743,760 checks under ASan/UBSan. No production defect
was reproduced; no firmware build or device access was performed.
