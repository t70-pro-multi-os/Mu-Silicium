// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_I2C_CONTROLLER_H
#define MTK_I2C_CONTROLLER_H
/* Published only after the board validates exclusive ownership, live clock
 * gates, live source-clock configuration, pinmux and the DMA/IOMMU route. No generic DTB
 * compatible string alone establishes these preconditions. Revision 1 is
 * specifically the MT6983 register-v2/APDMA-v1, 36-bit DMA variant. */
#define MTK_I2C_CONTROLLER_REVISION 2
#define MTK_I2C_CONTROLLER_READY 1
#define MTK_I2C_DMA_ROUTE_VALIDATED 2
#define MTK_I2C_REQUIRED_FLAGS 3
#define MTK_I2C_DRIVER_HANDOFF 0x1bU
typedef struct {
  UINT32 Revision;
  UINT32 Flags;
  EFI_PHYSICAL_ADDRESS ControllerBase;
  UINTN ControllerSize;
  EFI_PHYSICAL_ADDRESS DmaBase;
  UINTN DmaSize;
  UINT32 SourceClockHz;
  UINT64 IommuBase;
  UINT32 DmaId;
  // Revision 2 retains driver-binding/start results for platform diagnostics.
  UINT32 DriverStartPhase;
  EFI_STATUS DriverStartStatus;
} MTK_I2C_CONTROLLER;
extern EFI_GUID gMediaTekI2cControllerProtocolGuid;
#endif
