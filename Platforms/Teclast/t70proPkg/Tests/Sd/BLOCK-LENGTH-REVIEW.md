# SD default block length and CMD16

2026-11-18. Neither the inspected MsdcDxe initialization nor SdDxe discovery
issues CMD16. This alone is not a demonstrated missing initialization step.

The SD Association Physical Layer Simplified Specification v6.00, CMD16 entry
in the block-oriented command table (printed page 87), states that the default
SDSC block length is 512 bytes. For SDHC/SDXC memory reads/writes, block length
is always 512 bytes regardless of CMD16. CSD READ_BL_LEN used in SDSC capacity
calculation is not proof that the active transfer length has changed to 1024 or
2048 bytes. Source:
https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver6.00.pdf

The T70 sequence starts from CMD0, then identification/selection and CMD13.
Neither inspected layer changes the block length. SdDxe publishes BlockSize 512;
its existing CSD tests cover READ_BL_LEN 9/10/11 and block-address/byte-address
read tests separately. MsdcSendCmd programs 512-byte blocks for normal reads.
No CMD16 transport or R1 error path exists to inject on this initialization path,
and no new command was added on the unsupported premise that it is mandatory.

This conclusion is limited to the inspected reset/initialization path and
standards-conforming cards. An unrelated PassThru consumer could explicitly
change SDSC block length; coexistence with such a consumer is not established
by this review. No physical card or inherited non-default card state was tested.
