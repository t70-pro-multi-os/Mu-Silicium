// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/MtkPmicWrapper.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned Checks;
#define CHECK(Expression) do { \
  Checks++; \
  if (!(Expression)) { \
    fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #Expression); \
    abort (); \
  } \
} while (0)

#define TEST_KPD_BASE  0x10010000U
#define TEST_KPD_SIZE  0x1000U
#define TEST_EVENT_MAX 32U

EFI_GUID gMediaTekPmicWrapperProtocolGuid;
EFI_BOOT_SERVICES *gBS;

static EFI_BOOT_SERVICES               Services;
static EFI_MEMORY_REGION_DESCRIPTOR    Region;
static MTK_PMIC_WRAPPER_PROTOCOL       PmicWrapper;
static EFI_STATUS                      LocateProtocolStatus;
static EFI_STATUS                      LocateRegionStatus;
static EFI_STATUS                      PmicReadStatus;
static UINT16                           PmicTopStatus;
static UINT16                           KpdRegisters[TEST_KPD_SIZE / sizeof (UINT16)];
static UINT16                           ReadOffsets[256];
static unsigned                         MmioReadCount;
static unsigned                         MmioWriteCount;
static unsigned                         PmicReadCount;
static unsigned                         PmicWriteCount;
static UINT16                           LastPmicAddress;
static EFI_KEY_DATA                     Events[TEST_EVENT_MAX];
static unsigned                         EventCount;
static BOOLEAN                          ReenterPmicRead;
static EFI_STATUS                       ReenterStatus;
static KEYPAD_DEVICE_PROTOCOL           Device = { KeypadDeviceReset, KeypadDeviceGetKeys };
static KEYPAD_RETURN_API                ReturnApi;

#include "../Library/KeypadDeviceLib/KeypadDeviceLib.c"

VOID *
EFIAPI
SetMem (
  VOID  *Buffer,
  UINTN  Length,
  UINT8  Value
  )
{
  return memset (Buffer, Value, Length);
}

EFI_STATUS
EFIAPI
LocateMemoryRegionByName (
  CHAR8                         *Name,
  EFI_MEMORY_REGION_DESCRIPTOR *MemoryRegionDescriptor
  )
{
  CHECK (Name != NULL);
  CHECK (strcmp ((const char *)Name, "Keypad Controller") == 0);
  if (EFI_ERROR (LocateRegionStatus)) {
    return LocateRegionStatus;
  }

  *MemoryRegionDescriptor = Region;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockLocateProtocol (
  EFI_GUID *Protocol,
  VOID     *Registration,
  VOID    **Interface
  )
{
  (VOID)Registration;
  CHECK (Protocol == &gMediaTekPmicWrapperProtocolGuid);
  CHECK (Interface != NULL);
  if (EFI_ERROR (LocateProtocolStatus)) {
    return LocateProtocolStatus;
  }

  *Interface = &PmicWrapper;
  return EFI_SUCCESS;
}

UINT16
EFIAPI
MmioRead16 (
  UINTN Address
  )
{
  UINTN Offset;

  CHECK (Address >= TEST_KPD_BASE);
  CHECK (Address < TEST_KPD_BASE + TEST_KPD_SIZE);
  CHECK ((Address & 1U) == 0);
  Offset = Address - TEST_KPD_BASE;
  CHECK (Offset < TEST_KPD_SIZE);
  CHECK (MmioReadCount < (sizeof (ReadOffsets) / sizeof (ReadOffsets[0])));
  ReadOffsets[MmioReadCount++] = (UINT16)Offset;
  return KpdRegisters[Offset / sizeof (UINT16)];
}

UINT16
EFIAPI
MmioWrite16 (
  UINTN  Address,
  UINT16 Value
  )
{
  (VOID)Address;
  (VOID)Value;
  MmioWriteCount++;
  CHECK (FALSE);
  return 0;
}

static
EFI_STATUS
EFIAPI
MockPmicRead (
  UINT16  Address,
  UINT16 *Value
  )
{
  CHECK (Value != NULL);
  PmicReadCount++;
  LastPmicAddress = Address;
  if (ReenterPmicRead) {
    ReenterPmicRead = FALSE;
    ReenterStatus = KeypadDeviceGetKeys (&Device, &ReturnApi, 0);
  }

  if (EFI_ERROR (PmicReadStatus)) {
    return PmicReadStatus;
  }

  *Value = PmicTopStatus;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockPmicWrite (
  UINT16 Address,
  UINT16 Value
  )
{
  (VOID)Address;
  (VOID)Value;
  PmicWriteCount++;
  return EFI_UNSUPPORTED;
}

static
VOID
EFIAPI
PushKey (
  KEYPAD_RETURN_API *This,
  EFI_KEY_DATA      *KeyData
  )
{
  CHECK (This == &ReturnApi);
  CHECK (KeyData != NULL);
  CHECK (EventCount < TEST_EVENT_MAX);
  Events[EventCount++] = *KeyData;
}

static
VOID
ResetFixture (
  VOID
  )
{
  SetMem (&Region, sizeof (Region), 0);
  Region.Address          = TEST_KPD_BASE;
  Region.Length           = TEST_KPD_SIZE;
  Region.HobOption        = AddDev;
  Region.ResourceType     = MMAP_IO;
  Region.ResourceAttribute = UNCACHEABLE;
  Region.MemoryType       = MmIO;
  Region.ArmAttributes    = NS_DEVICE;

  SetMem (KpdRegisters, sizeof (KpdRegisters), 0xff);
  KpdRegisters[0x18 / sizeof (UINT16)] = 1024;
  // T70 uses the DT default use-extend-type=0; this register is not readiness.
  KpdRegisters[0x20 / sizeof (UINT16)] = 0;
  KpdRegisters[0x24 / sizeof (UINT16)] = BIT0;
  PmicTopStatus       = 0xffff;
  PmicReadStatus      = EFI_SUCCESS;
  LocateProtocolStatus = EFI_SUCCESS;
  LocateRegionStatus  = EFI_SUCCESS;
  PmicWrapper.Read    = MockPmicRead;
  PmicWrapper.Write   = MockPmicWrite;
  Services.LocateProtocol = MockLocateProtocol;
  gBS = &Services;
  ReturnApi.PushEfikeyBufTail = PushKey;

  MmioReadCount   = 0;
  MmioWriteCount  = 0;
  PmicReadCount   = 0;
  PmicWriteCount  = 0;
  LastPmicAddress = 0;
  EventCount      = 0;
  ReenterPmicRead = FALSE;
  ReenterStatus   = EFI_SUCCESS;
}

static
VOID
ClearTrace (
  VOID
  )
{
  MmioReadCount  = 0;
  PmicReadCount  = 0;
  PmicWriteCount = 0;
  EventCount     = 0;
}

static
EFI_STATUS
Sample (
  UINT64 Delta
  )
{
  return KeypadDeviceGetKeys (&Device, &ReturnApi, Delta);
}

static
VOID
CheckReadOffsets (
  CONST UINT16 *Offsets,
  unsigned      Count
  )
{
  unsigned Index;

  CHECK (MmioReadCount == Count);
  for (Index = 0; Index < Count; Index++) {
    CHECK (ReadOffsets[Index] == Offsets[Index]);
  }
}

static
VOID
TestConstructorValidation (
  VOID
  )
{
  ResetFixture ();
  LocateProtocolStatus = EFI_NOT_FOUND;
  CHECK (KeypadDeviceConstructor () == RETURN_NOT_FOUND);
  CHECK (MmioReadCount == 0 && PmicReadCount == 0);
  CHECK (Sample (0) == EFI_NOT_READY);

  ResetFixture ();
  LocateRegionStatus = EFI_NOT_FOUND;
  CHECK (KeypadDeviceConstructor () == RETURN_NOT_FOUND);
  CHECK (MmioReadCount == 0 && PmicReadCount == 0);

  ResetFixture ();
  Region.Address++;
  CHECK (KeypadDeviceConstructor () == RETURN_UNSUPPORTED);
  CHECK (MmioReadCount == 0 && PmicReadCount == 0);

  ResetFixture ();
  Region.ArmAttributes = 0;
  CHECK (KeypadDeviceConstructor () == RETURN_UNSUPPORTED);
  CHECK (MmioReadCount == 0 && PmicReadCount == 0);

  ResetFixture ();
  PmicWrapper.Read = NULL;
  CHECK (KeypadDeviceConstructor () == RETURN_NOT_FOUND);
  CHECK (MmioReadCount == 0 && PmicReadCount == 0);

  ResetFixture ();
  CHECK (KeypadDeviceConstructor () == RETURN_SUCCESS);
  CHECK (KeypadDeviceReset (&Device) == EFI_SUCCESS);
}

static
VOID
TestReadinessAndAtomicReadOrder (
  VOID
  )
{
  CONST UINT16 EnableOnly[] = { 0x24 };
  CONST UINT16 DebounceOnly[] = { 0x24, 0x18 };
  CONST UINT16 FullSample[] = { 0x24, 0x18, 0x04 };

  ResetFixture ();
  CHECK (KeypadDeviceConstructor () == RETURN_SUCCESS);

  KpdRegisters[0x24 / sizeof (UINT16)] = 0;
  CHECK (Sample (0) == EFI_NOT_READY);
  CheckReadOffsets (EnableOnly, 1);
  CHECK (PmicReadCount == 0 && EventCount == 0);

  ClearTrace ();
  KpdRegisters[0x24 / sizeof (UINT16)] = 1;
  KpdRegisters[0x18 / sizeof (UINT16)] = 1023;
  CHECK (Sample (0) == EFI_NOT_READY);
  CheckReadOffsets (DebounceOnly, 2);
  CHECK (PmicReadCount == 0 && EventCount == 0);

  ClearTrace ();
  KpdRegisters[0x18 / sizeof (UINT16)] = 0x8400 | 1024;
  KpdRegisters[0x20 / sizeof (UINT16)] = 0;
  CHECK (Sample (0) == EFI_SUCCESS);
  CheckReadOffsets (FullSample, 3);
  CHECK (PmicReadCount == 1 && LastPmicAddress == 0x28);
  CHECK (PmicWriteCount == 0 && MmioWriteCount == 0 && EventCount == 0);
}

static
VOID
TestVolumePressReleaseAndMultipleKeys (
  VOID
  )
{
  ResetFixture ();
  CHECK (KeypadDeviceConstructor () == RETURN_SUCCESS);

  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~BIT0;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  CHECK (EventCount == 0);
  KpdRegisters[0x04 / sizeof (UINT16)] = 0xffff;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  CHECK (EventCount == 1 && Events[0].Key.ScanCode == SCAN_UP);
  CHECK (Events[0].Key.UnicodeChar == 0);
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);

  CHECK (KeypadDeviceReset (&Device) == EFI_SUCCESS);
  ClearTrace ();
  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~(BIT0 | BIT1);
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  KpdRegisters[0x04 / sizeof (UINT16)] = 0xffff;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  CHECK (EventCount == 2);
  CHECK (Events[0].Key.ScanCode == SCAN_UP);
  CHECK (Events[1].Key.ScanCode == SCAN_DOWN);
  CHECK (PmicWriteCount == 0 && MmioWriteCount == 0);
}

static
VOID
TestPowerShortLongAndVolumeRepeat (
  VOID
  )
{
  unsigned Index;

  ResetFixture ();
  CHECK (KeypadDeviceConstructor () == RETURN_SUCCESS);

  PmicTopStatus = (UINT16)~BIT1;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  PmicTopStatus = 0xffff;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  CHECK (EventCount == 1 && Events[0].Key.UnicodeChar == CHAR_CARRIAGE_RETURN);

  CHECK (KeypadDeviceReset (&Device) == EFI_SUCCESS);
  ClearTrace ();
  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~BIT0;
  PmicTopStatus = (UINT16)~BIT1;
  CHECK (Sample (0) == EFI_SUCCESS);
  for (Index = 0; Index < 5; Index++) {
    CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  }
  CHECK (EventCount == 1 && Events[0].Key.ScanCode == SCAN_LEFT);

  KpdRegisters[0x04 / sizeof (UINT16)] = 0xffff;
  PmicTopStatus = 0xffff;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  CHECK (EventCount == 1);

  CHECK (KeypadDeviceReset (&Device) == EFI_SUCCESS);
  ClearTrace ();
  PmicTopStatus = (UINT16)~BIT1;
  CHECK (Sample (0) == EFI_SUCCESS);
  for (Index = 0; Index < 5; Index++) {
    CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  }
  CHECK (EventCount == 1 && Events[0].Key.ScanCode == SCAN_ESC);

  CHECK (KeypadDeviceReset (&Device) == EFI_SUCCESS);
  ClearTrace ();
  PmicTopStatus = 0xffff;
  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~BIT0;
  CHECK (Sample (0) == EFI_SUCCESS);
  for (Index = 0; Index < 4; Index++) {
    CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  }
  CHECK (EventCount == 0);

  // A new power press on the volume repeat threshold gives the combo priority.
  PmicTopStatus = (UINT16)~BIT1;
  CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  CHECK (EventCount == 0);
  for (Index = 0; Index < 5; Index++) {
    CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  }
  CHECK (EventCount == 1 && Events[0].Key.ScanCode == SCAN_LEFT);

  CHECK (KeypadDeviceReset (&Device) == EFI_SUCCESS);
  ClearTrace ();
  PmicTopStatus = 0xffff;
  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~BIT1;
  CHECK (Sample (0) == EFI_SUCCESS);
  for (Index = 0; Index < 5; Index++) {
    CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  }
  CHECK (EventCount == 1 && Events[0].Key.ScanCode == SCAN_DOWN);
  CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  CHECK (EventCount == 2 && Events[1].Key.ScanCode == SCAN_DOWN);
}

static
VOID
TestPmicFailurePreservesVolumeInput (
  VOID
  )
{
  unsigned Index;

  ResetFixture ();
  CHECK (KeypadDeviceConstructor () == RETURN_SUCCESS);
  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~BIT0;
  PmicTopStatus = (UINT16)~BIT1;
  PmicReadStatus = EFI_DEVICE_ERROR;
  CHECK (Sample (MS2NS (10)) == EFI_DEVICE_ERROR);
  KpdRegisters[0x04 / sizeof (UINT16)] = 0xffff;
  CHECK (Sample (MS2NS (10)) == EFI_DEVICE_ERROR);
  CHECK (EventCount == 1 && Events[0].Key.ScanCode == SCAN_UP);
  CHECK (Sample (MS2NS (10)) == EFI_DEVICE_ERROR);
  CHECK (EventCount == 1);
  ClearTrace ();
  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~BIT0;
  PmicReadStatus = EFI_SUCCESS;
  CHECK (Sample (0) == EFI_SUCCESS);
  CHECK (EventCount == 0);
  for (Index = 0; Index < 5; Index++) {
    CHECK (Sample (MS2NS (100)) == EFI_SUCCESS);
  }
  CHECK (EventCount == 1 && Events[0].Key.ScanCode == SCAN_LEFT);

  KpdRegisters[0x04 / sizeof (UINT16)] = 0xffff;
  PmicTopStatus = 0xffff;
  PmicReadStatus = EFI_TIMEOUT;
  CHECK (Sample (MS2NS (10)) == EFI_TIMEOUT);
  CHECK (EventCount == 1);
  PmicReadStatus = EFI_SUCCESS;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  CHECK (EventCount == 1);

  CHECK (PmicWriteCount == 0 && MmioWriteCount == 0);
}

static
VOID
TestReadinessFailureDoesNotConsumeRelease (
  VOID
  )
{
  ResetFixture ();
  CHECK (KeypadDeviceConstructor () == RETURN_SUCCESS);
  KpdRegisters[0x04 / sizeof (UINT16)] = (UINT16)~BIT0;
  PmicTopStatus = (UINT16)~BIT1;
  CHECK (Sample (0) == EFI_SUCCESS);

  KpdRegisters[0x04 / sizeof (UINT16)] = 0xffff;
  PmicTopStatus = 0xffff;
  KpdRegisters[0x24 / sizeof (UINT16)] = 0;
  ClearTrace ();
  CHECK (Sample (MS2NS (10)) == EFI_NOT_READY);
  CHECK (EventCount == 0 && PmicReadCount == 0);

  KpdRegisters[0x24 / sizeof (UINT16)] = BIT0;
  CHECK (Sample (MS2NS (10)) == EFI_SUCCESS);
  CHECK (EventCount == 2);
  CHECK (Events[0].Key.ScanCode == SCAN_UP);
  CHECK (Events[1].Key.UnicodeChar == CHAR_CARRIAGE_RETURN);
}

static
VOID
TestReentrantSampleIsRejected (
  VOID
  )
{
  ResetFixture ();
  CHECK (KeypadDeviceConstructor () == RETURN_SUCCESS);
  ReenterPmicRead = TRUE;
  CHECK (Sample (0) == EFI_SUCCESS);
  CHECK (ReenterStatus == EFI_NOT_READY);
  CHECK (MmioReadCount == 3 && PmicReadCount == 1);
}

int
main (
  VOID
  )
{
  ReturnApi.PushEfikeyBufTail = PushKey;
  TestConstructorValidation ();
  TestReadinessAndAtomicReadOrder ();
  TestVolumePressReleaseAndMultipleKeys ();
  TestPowerShortLongAndVolumeRepeat ();
  TestPmicFailurePreservesVolumeInput ();
  TestReadinessFailureDoesNotConsumeRelease ();
  TestReentrantSampleIsRejected ();
  CHECK (PmicWriteCount == 0 && MmioWriteCount == 0);
  printf ("KeypadDeviceTest: %u checks passed\n", Checks);
  return 0;
}
