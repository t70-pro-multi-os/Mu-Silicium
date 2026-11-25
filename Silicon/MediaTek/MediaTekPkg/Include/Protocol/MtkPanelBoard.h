/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef _MTK_PANEL_BOARD_H_
#define _MTK_PANEL_BOARD_H_

#define MTK_PANEL_BOARD_PROTOCOL_REVISION 2U

typedef struct _MTK_PANEL_BOARD_PROTOCOL MTK_PANEL_BOARD_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *MTK_PANEL_BOARD_RUN_SEQUENCE) (
  IN MTK_PANEL_BOARD_PROTOCOL *This
  );

struct _MTK_PANEL_BOARD_PROTOCOL {
  UINT32 Revision;
  MTK_PANEL_BOARD_RUN_SEQUENCE RunInit;
  MTK_PANEL_BOARD_RUN_SEQUENCE RunSuspend;

  // Shipping Android panel GPIO lifecycle. These are explicit transactions;
  // publication performs no GPIO work. Each transaction owns all nine panel
  // pins, including shared touch-reset GPIO152, so touch must be quiesced.
  // Prepare establishes the rails/reset levels. The display caller must have
  // configured and started its stream before Enable, and stopped it before
  // Unprepare. None of these methods proves DSI clocks, PHY, or visible pixels.
  // Calls after ExitBootServices, at TPL above CALLBACK, or from inside another
  // panel call fail. On failure, retry the same operation to finish its retained
  // pin lease; do not switch sequences or start another GPIO owner.
  MTK_PANEL_BOARD_RUN_SEQUENCE RunKernelPrepare;
  MTK_PANEL_BOARD_RUN_SEQUENCE RunKernelEnable;
  MTK_PANEL_BOARD_RUN_SEQUENCE RunKernelUnprepare;
};

extern EFI_GUID gMtkPanelBoardProtocolGuid;

#endif /* _MTK_PANEL_BOARD_H_ */
