/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_DSI_FIXTURE_H
#define T70_DSI_FIXTURE_H
static MTK_DSI_U8 GetParams[MTK_DSI_GET_PARAMS_SIZE];
static MTK_DSI_OP7_PROFILE
SupportedProfile (void)
{
  MTK_DSI_OP7_PROFILE Profile;

  memset (GetParams, 0, sizeof (GetParams));
#define SET_PARAM(Offset, Value) do { \
  GetParams[(Offset)] = (MTK_DSI_U8)((Value) & 0xFFU); \
  GetParams[(Offset) + 1U] = (MTK_DSI_U8)(((Value) >> 8) & 0xFFU); \
  GetParams[(Offset) + 2U] = (MTK_DSI_U8)(((Value) >> 16) & 0xFFU); \
  GetParams[(Offset) + 3U] = (MTK_DSI_U8)(((Value) >> 24) & 0xFFU); \
} while (0)

  SET_PARAM (0x000U, MTK_DSI_LK_PANEL_TYPE);
  SET_PARAM (0x018U, MTK_DSI_LK_WIDTH);
  SET_PARAM (0x01CU, MTK_DSI_LK_HEIGHT);
  SET_PARAM (0x170U, 1U);
  SET_PARAM (0x184U, 4U);
  SET_PARAM (0x194U, 2U);
  SET_PARAM (0x1A0U, 2U);
  SET_PARAM (0x1A4U, 0x1A40U);
  SET_PARAM (0x1A8U, 0x100U);
  SET_PARAM (0x1B0U, 8U);
  SET_PARAM (0x1B4U, 32U);
  SET_PARAM (0x1B8U, 230U);
  SET_PARAM (0x1C0U, MTK_DSI_LK_HEIGHT);
  SET_PARAM (0x1C4U, 8U);
  SET_PARAM (0x1C8U, 32U);
  SET_PARAM (0x1CCU, 40U);
  SET_PARAM (0x1D4U, MTK_DSI_LK_WIDTH);
  SET_PARAM (0x230U, MTK_DSI_LK_RATE_FIELD);
  SET_PARAM (0x240U, 1U);
  SET_PARAM (0x25CU, 1U);
  SET_PARAM (0x5E8U, 1U);
#undef SET_PARAM

  Profile.GetParams = GetParams;
  Profile.GetParamsSize = MTK_DSI_GET_PARAMS_SIZE;
  Profile.ModuleId = MTK_DSI_LK_MODULE_ID;
  Profile.Mode = 0U;
  Profile.ConfigWidth = MTK_DSI_LK_WIDTH;
  Profile.ConfigHeight = MTK_DSI_LK_HEIGHT;
  Profile.Initial.Reg08 = 0x10203040U;
  Profile.Initial.Reg14 = 0x12345678U | 0x00100000U;
  Profile.Initial.Reg18 = 0xA5A5A5FFU;
  Profile.Initial.Reg1C = 0x00031A40U;
  Profile.Initial.Reg64 = 0x55667788U;
  Profile.Initial.Reg200 = 33U;
  return Profile;
}

#endif
