// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef HIMAX_CORE_H
#define HIMAX_CORE_H
/* Normal HX83132 reports and guarded page-program/flash-CRC stages are
 * reconstructed from the shipped hxchipset_i2c.ko. Erase, flash unlock,
 * reset/restart orchestration, GPIO and power sequencing are not implemented. */
#define HM_MAX_CONTACTS 10
#define HM_EVENT_COMMAND 0x30
#define HM_T70_REPORT_BYTES 56
#define HM_BUS_WRITE_SENTINEL 0xffffdaf1U
#define HM_CHIP_ID_REGISTER 0x900000d0U
#define HM_BUS_WRITE_MAX_BODY 252
#define HM_BUS_READ_MAX_BYTES 256
#define HM_FW_TAG_A00 0x00000a00U
#define HM_FW_TAG_A01 0x00000a01U
#define HM_FW_TAG_10000000 0x10000000U
#define HM_FW_TAG_10000100 0x10000100U
#define HM_FW_TAG_10000600 0x10000600U
#define HM_FW_MAX_DESCRIPTORS 5
#define HM_T70_FW_BYTES 261120U
#define HM_T70_FW_PAGE_BYTES 256U
#define HM_FW_UPDATE_PROFILE_VERSION 1U
#define HM_FW_FACT_T70_BOARD_CONFIRMED 0x00000001U
#define HM_FW_FACT_POWER_STABLE 0x00000002U
#define HM_FW_FACT_TOUCH_STOPPED 0x00000004U
#define HM_FW_FACT_IRQ_QUIESCED 0x00000008U
#define HM_FW_FACT_EXCLUSIVE_BUS 0x00000010U
#define HM_FW_FACT_FLASH_UNPROTECTED 0x00000020U
#define HM_FW_FACT_TARGET_ERASED 0x00000040U
#define HM_FW_FACT_IMAGE_SHA256_VERIFIED 0x00000080U
#define HM_FW_FACT_PROGRAM_COMPLETED 0x00000100U
#define HM_FW_FACTS_KNOWN 0x000001ffU
#define HM_FW_FACTS_PROGRAM (HM_FW_FACT_T70_BOARD_CONFIRMED|HM_FW_FACT_POWER_STABLE|HM_FW_FACT_TOUCH_STOPPED|HM_FW_FACT_IRQ_QUIESCED|HM_FW_FACT_EXCLUSIVE_BUS|HM_FW_FACT_FLASH_UNPROTECTED|HM_FW_FACT_TARGET_ERASED|HM_FW_FACT_IMAGE_SHA256_VERIFIED)
#define HM_FW_FACTS_VERIFY (HM_FW_FACT_T70_BOARD_CONFIRMED|HM_FW_FACT_POWER_STABLE|HM_FW_FACT_TOUCH_STOPPED|HM_FW_FACT_IRQ_QUIESCED|HM_FW_FACT_EXCLUSIVE_BUS|HM_FW_FACT_FLASH_UNPROTECTED|HM_FW_FACT_IMAGE_SHA256_VERIFIED|HM_FW_FACT_PROGRAM_COMPLETED)
typedef enum { HmOk, HmInvalid, HmChecksum, HmEmpty, HmUnsupported, HmIoError, HmTimeout, HmStateError } HM_STATUS;
/* Transfer is one synchronous I2C request. When ReadLength is nonzero, the
 * backend must send WriteData then issue a repeated START for the read. */
typedef HM_STATUS (*HM_I2C_TRANSFER)(void *Context,
  const unsigned char *WriteData, unsigned WriteLength,
  unsigned char *ReadData, unsigned ReadLength);
typedef void (*HM_DELAY_RANGE)(void *Context, unsigned MinimumUs, unsigned MaximumUs);
typedef struct {
  void *Context;
  HM_I2C_TRANSFER Transfer;
  HM_DELAY_RANGE Delay;
} HM_IO;
typedef struct {
  unsigned short X, Y;
  unsigned char Pressure, Active;
} HM_CONTACT;
typedef struct { unsigned Tag, Value, Offset; } HM_FW_DESCRIPTOR;
typedef struct {
  unsigned Length, Crc32c, DescriptorCount;
  HM_FW_DESCRIPTOR Descriptors[HM_FW_MAX_DESCRIPTORS];
} HM_FIRMWARE_INFO;
/* Fact bits are explicit caller attestations. The portable core cannot prove
 * board identity, power stability, IRQ state, bus exclusivity, flash unlock,
 * erase completion, or manifest SHA256. */
typedef struct {
  unsigned Version, Size, Facts, KnownFactsMask;
} HM_FIRMWARE_UPDATE_PROFILE;
typedef enum {
  HmFwStagePreflight, HmFwStageControllerReady, HmFwStagePageSetup,
  HmFwStagePageAddress, HmFwStagePagePayload,
  HmFwStagePageWait, HmFwStageProgrammed, HmFwStageCrcSetup,
  HmFwStageCrcPoll, HmFwStageCrcRead, HmFwStageVerified
} HM_FIRMWARE_STAGE;
typedef struct {
  unsigned Stage, TotalBytes, BytesCompleted, PagesCompleted;
  unsigned CurrentOffset, ObservedCrc32c, FlashContentsMayBeChanged;
  HM_STATUS LastStatus;
} HM_FIRMWARE_PROGRESS;
#define HM_SENSE_OFF_PROFILE_VERSION 1U
#define HM_SENSE_OFF_RUNTIME_BIT4 0x10U
typedef struct {
  unsigned Version, Size;
  unsigned FeatureFlags, KnownFlagsMask;
} HM_SENSE_OFF_PROFILE;
typedef struct {
  HM_CONTACT Contacts[HM_MAX_CONTACTS];
  unsigned char Count, DeclaredCount, State[2];
} HM_FRAME;
typedef struct {
  unsigned short X, Y;
  unsigned char Pressure, Active, Primary;
} HM_POINTER;
/* On error Frame is unchanged. Dimensions are inclusive coordinate maxima,
 * matching the stock parser, not the physical panel's last pixel number. */
HM_STATUS HmDecode(const unsigned char *Data, unsigned Length, unsigned Slots,
                   unsigned XMax, unsigned YMax, HM_FRAME *Frame);
/* Wire-compatible HX83132 indirect-register transport reconstructed from the
 * stock ARM64 driver. HmBusWrite Length excludes the leading command byte;
 * ordinary addresses include the four address bytes, sentinel writes are raw. */
HM_STATUS HmBusRead(const HM_IO *Io, unsigned Command,
                    unsigned char *Data, unsigned Length);
HM_STATUS HmBusWrite(const HM_IO *Io, unsigned Command, unsigned Address,
                     const unsigned char *Data, unsigned Length);
HM_STATUS HmRegisterRead(const HM_IO *Io, unsigned Address,
                         unsigned char *Data, unsigned Length);
HM_STATUS HmRegisterWrite(const HM_IO *Io, unsigned Address,
                          const unsigned char *Data, unsigned Length);
/* Exact AP CRC helper from the stock driver: reflected CRC32C, init ffffffff,
 * no final xor; byte length must be a multiple of four. */
HM_STATUS HmCrc32c(const unsigned char *Data, unsigned Length, unsigned *Crc32c);
/* Parse the stock HX83132 16-byte metadata records. Invalid-checksum records
 * are ignored as in the vendor parser; duplicate valid tags are rejected. */
HM_STATUS HmInspectFirmware(const unsigned char *Image, unsigned Length,
                            HM_FIRMWARE_INFO *Info);
/* Check the T70 firmware format/variant: 261120 bytes, zero AP CRC32C
 * residue, and four descriptor tag/value pairs. This is not a cryptographic
 * identity check; the firmware manifest's SHA256 check identifies the exact
 * backed-up blob. */
HM_STATUS HmValidateT70Firmware(const unsigned char *Image, unsigned Length,
                                HM_FIRMWARE_INFO *Info);
/* The guarded page-program stage is reconstructed from the stock HX83132
 * ELF. Program writes 256-byte pages as 16+240-byte chunks and does not erase or
 * unlock flash. It requires caller attestations that the target was erased and
 * flash is already unprotected. The caller separately verifies image SHA256;
 * this core checks format, descriptors and AP CRC32C. Failures stop immediately
 * and report only confirmed completed pages. The stage does not reset or start
 * sensing and is not called by D0/resume paths. */
HM_STATUS HmProgramT70Firmware(const HM_IO *Io, const unsigned char *Image,
  unsigned Length, const HM_FIRMWARE_UPDATE_PROFILE *Profile,
  HM_FIRMWARE_PROGRESS *Progress);
/* Uses the stock CRC registers, rejects a busy timeout (stock helper returns
 * ambiguous zero here), compares device CRC with HmCrc32c(Image), and never
 * resets. Caller must attest that this exact image was just programmed. */
HM_STATUS HmVerifyT70FirmwareFlash(const HM_IO *Io,
  const unsigned char *Image, unsigned Length,
  const HM_FIRMWARE_UPDATE_PROFILE *Profile, HM_FIRMWARE_PROGRESS *Progress);
/* Notify already-running T70 firmware of AP state (0=awake, 1=suspended).
 * Requires caller-owned powered/reset-released pins and exclusive bus access.
 * Bounded write/readback handshake; success does NOT establish HMRD readiness.
 * A failed write may have reached firmware: failure leaves AP state uncertain;
 * no rollback or automatic opposite-state command is attempted.
 * No reset, rail control or firmware recovery. Not called by input drivers
 * until a complete runtime board provider owns the surrounding transition. */
HM_STATUS HmNotifyApSuspend(const HM_IO *Io, unsigned Suspended);
/* Chip ID bytes must be [3]=0x83, [2]=0x13, [1]=0x2a..0x2c. */
int HmChipIdMatches(const unsigned char Id[4]);
HM_STATUS HmReadChipId(const HM_IO *Io);
/* Wake the bus and confirm both interface registers, at most ten attempts. */
HM_STATUS HmInterfaceOn(const HM_IO *Io);
/* Exact software-reset command; caller must own powered pins/bus and arrange
 * firmware recovery. Not called by the inherited-firmware input drivers. */
HM_STATUS HmSystemReset(const HM_IO *Io);
/* Stock argument-zero recovery sequence for a validated T70 firmware summary
 * and confirmed hx_s_ts[0x78].bit4 clear. Includes bounded host-stop requests,
 * the broadcast handshake and final sense-off registers. Bus errors and an
 * uncompleted handshake fail. The bit4-set slave scan remains unsupported.
 * Caller must own powered pins/bus and arrange any required firmware recovery;
 * this is not called by normal input start or retained resume. */
HM_STATUS HmSenseOff(const HM_IO *Io, const HM_FIRMWARE_INFO *Firmware,
                     const HM_SENSE_OFF_PROFILE *Profile);
/* Stock no-reset sense-on, including bounded interface readback retries.
 * Exhausted readback mismatches alone do not suppress the sense-on commands.
 * Successful command completion alone does not prove running firmware. */
HM_STATUS HmSenseOn(const HM_IO *Io);
typedef enum { HmEntryInitialHandoff, HmEntryRetainedResume } HM_ENTRY_REASON;
/* Enter after the board has freshly attested stock firmware and exclusive bus
 * ownership. Cold start checks the ID then runs the proven no-reset sense-on
 * path. Retained resume checks only the ID; it does NOT reproduce the full stock
 * resume_proc, which resends runtime settings and notifies firmware of AP state.
 * The caller's runtime provider must own that transition. This cannot recover
 * a power-lost controller. */
HM_STATUS HmEnter(const HM_IO *Io, HM_ENTRY_REASON Reason);
/* Stable primary contact for UEFI's single absolute pointer. A primary lift
 * produces a release before another active slot can become the new primary. */
void HmPointerInit(HM_POINTER *Pointer);
int HmPointerUpdate(HM_POINTER *Pointer, const HM_FRAME *Frame);
int HmPointerRelease(HM_POINTER *Pointer);
#endif
