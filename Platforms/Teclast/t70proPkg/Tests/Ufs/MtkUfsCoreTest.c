/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdint.h>
#include <Library/MtkUfsCore.h>

static void
TestDescriptorBounds(void)
{
  uint8_t Descriptor[] = { 4, 0x02, 0xAA, 0x55 };
  MTK_UFS_DESCRIPTOR_VIEW View;

  assert(MtkUfsParseDescriptorHeader(Descriptor, sizeof(Descriptor), &View) == MtkUfsCoreSuccess);
  assert(View.DescriptorLength == 4U);
  assert(View.DescriptorType == 0x02U);
  assert(MtkUfsParseDescriptorHeader(Descriptor, 1U, &View) == MtkUfsCoreBufferTooShort);
  Descriptor[0] = 1U;
  assert(MtkUfsParseDescriptorHeader(Descriptor, sizeof(Descriptor), &View) == MtkUfsCoreMalformed);
  Descriptor[0] = 5U;
  assert(MtkUfsParseDescriptorHeader(Descriptor, sizeof(Descriptor), &View) == MtkUfsCoreMalformed);
}

static void
TestResponseUpiuBoundsAndStatus(void)
{
  uint8_t Response[40] = { 0 };
  MTK_UFS_RESPONSE_VIEW View;

  Response[0] = MTK_UFS_UPIU_RESPONSE_TYPE;
  Response[1] = 0x20U;
  Response[2] = 2U;
  Response[3] = 7U;
  Response[4] = 0x21U;
  Response[6] = 0U;
  Response[7] = 2U;
  Response[10] = 0U;
  Response[11] = 6U;
  Response[15] = 3U;
  Response[33] = 4U;
  Response[34] = 0x70U;
  Response[35] = 0x00U;
  Response[36] = 0x05U;

  assert(MtkUfsParseResponseUpiu(Response, sizeof(Response), &View) == MtkUfsCoreSuccess);
  assert(View.TransactionType == MTK_UFS_UPIU_RESPONSE_TYPE);
  assert(View.Lun == 2U);
  assert(View.TaskTag == 7U);
  assert(View.CommandSet == 1U);
  assert(View.InitiatorId == 2U);
  assert(View.ScsiStatus == 2U);
  assert(View.ResidualTransferCount == 3U);
  assert(View.SenseDataLength == 4U);
  assert(View.SenseData == &Response[34]);

  assert(MtkUfsParseResponseUpiu(Response, 15U, &View) == MtkUfsCoreBufferTooShort);
  Response[10] = 0U;
  Response[11] = 1U;
  assert(MtkUfsParseResponseUpiu(Response, sizeof(Response), &View) == MtkUfsCoreMalformed);
  Response[10] = 0U;
  Response[11] = 6U;
  Response[33] = 5U;
  assert(MtkUfsParseResponseUpiu(Response, sizeof(Response), &View) == MtkUfsCoreMalformed);
  Response[0] = 0x01U;
  assert(MtkUfsParseResponseUpiu(Response, sizeof(Response), &View) == MtkUfsCoreMalformed);
}

static void
TestHandoffAssessment(void)
{
  assert(MtkUfsAssessHandoff(0U, 0x09U, 0U, 0U) == MtkUfsHandoffControllerDisabled);
  assert(MtkUfsAssessHandoff(1U, 0x01U, 0U, 0U) == MtkUfsHandoffUicNotReady);
  assert(MtkUfsAssessHandoff(1U, 0x08U, 0U, 0U) == MtkUfsHandoffDeviceAbsent);
  assert(MtkUfsAssessHandoff(1U, 0x09U, 1U, 0U) == MtkUfsHandoffRequestsOutstanding);
  assert(MtkUfsAssessHandoff(1U, 0x09U, 0U, 0U) == MtkUfsHandoffReadyForNop);
}

static void
TestReadOnlyCdbAllowlist(void)
{
  uint8_t Read10[10] = { 0x28U };
  uint8_t Read16[16] = { 0x88U };
  uint8_t Capacity16[16] = { 0x9EU, 0x10U };
  uint8_t Write10[10] = { 0x2AU };
  uint8_t Unmap[10] = { 0x42U };
  uint8_t FormatUnit[6] = { 0x04U };
  uint8_t SecurityOut[12] = { 0xB5U };

  assert(MtkUfsScsiCdbIsReadOnly(Read10, sizeof(Read10)));
  assert(MtkUfsScsiCdbIsReadOnly(Read16, sizeof(Read16)));
  assert(MtkUfsScsiCdbIsReadOnly(Capacity16, sizeof(Capacity16)));
  assert(!MtkUfsScsiCdbIsReadOnly(Write10, sizeof(Write10)));
  assert(!MtkUfsScsiCdbIsReadOnly(Unmap, sizeof(Unmap)));
  assert(!MtkUfsScsiCdbIsReadOnly(FormatUnit, sizeof(FormatUnit)));
  assert(!MtkUfsScsiCdbIsReadOnly(SecurityOut, sizeof(SecurityOut)));
  assert(!MtkUfsScsiCdbIsReadOnly(Read16, 15U));
  assert(!MtkUfsScsiCdbIsReadOnly(NULL, sizeof(Read16)));
}

static void
TestCapacityDecoding(void)
{
  uint8_t Capacity10[8] = {
    0x00, 0x00, 0x01, 0x23,
    0x00, 0x00, 0x10, 0x00
  };
  uint8_t Capacity16[32] = {
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00
  };
  uint64_t LastBlock;
  uint32_t BlockSize;
  int Requires16;

  assert(MtkUfsDecodeReadCapacity10(Capacity10, sizeof(Capacity10), &LastBlock, &BlockSize, &Requires16) == MtkUfsCoreSuccess);
  assert(LastBlock == 0x123U);
  assert(BlockSize == 4096U);
  assert(!Requires16);

  Capacity10[0] = 0xFFU;
  Capacity10[1] = 0xFFU;
  Capacity10[2] = 0xFFU;
  Capacity10[3] = 0xFFU;
  assert(MtkUfsDecodeReadCapacity10(Capacity10, sizeof(Capacity10), &LastBlock, &BlockSize, &Requires16) == MtkUfsCoreSuccess);
  assert(Requires16);

  assert(MtkUfsDecodeReadCapacity16(Capacity16, sizeof(Capacity16), &LastBlock, &BlockSize) == MtkUfsCoreSuccess);
  assert(LastBlock == 0x100000000ULL);
  assert(BlockSize == 4096U);
  assert(MtkUfsDecodeReadCapacity16(Capacity16, 11U, &LastBlock, &BlockSize) == MtkUfsCoreBufferTooShort);

  Capacity16[10] = 0U;
  assert(MtkUfsDecodeReadCapacity16(Capacity16, sizeof(Capacity16), &LastBlock, &BlockSize) == MtkUfsCoreMalformed);
}


static void
TestCapacityMatchesEvidence(void)
{
  const uint64_t MainLuBlocks = 31240192ULL;
  const uint64_t MainLuBytes = 127959826432ULL;

  assert(MtkUfsCapacityMatches(MainLuBlocks - 1U, 4096U, MainLuBytes));
  assert(!MtkUfsCapacityMatches(MainLuBlocks - 1U, 512U, MainLuBytes));
  assert(!MtkUfsCapacityMatches(MainLuBlocks, 4096U, MainLuBytes));
  assert(!MtkUfsCapacityMatches(UINT64_MAX, 4096U, MainLuBytes));
  assert(!MtkUfsCapacityMatches(MainLuBlocks - 1U, 4096U, 0U));
}

int
main(void)
{
  TestDescriptorBounds();
  TestResponseUpiuBoundsAndStatus();
  TestHandoffAssessment();
  TestReadOnlyCdbAllowlist();
  TestCapacityDecoding();
  TestCapacityMatchesEvidence();
  return 0;
}
