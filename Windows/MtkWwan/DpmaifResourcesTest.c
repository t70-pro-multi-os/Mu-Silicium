/* SPDX-License-Identifier: GPL-2.0-only */
#include "DpmaifResources.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static T70_DPMAIF_RESOURCE Resources[6] = {
  {T70DpmaifResourceMemory, 0x10014000, 0x1000, 0, 0, 0},
  {T70DpmaifResourceMemory, 0x1022D000, 0x1000, 0, 0, 0},
  {T70DpmaifResourceMemory, 0x1022C000, 0x1000, 0, 0, 0},
  {T70DpmaifResourceMemory, 0x1022E000, 0x1000, 0, 0, 0},
  {T70DpmaifResourceInterrupt, 273, 0, 1, 1, 0},
  {T70DpmaifResourceMemory, 0x43F7F180, 0x40, 0, 0, 0}
};

static void TestExactResourceMap(void)
{
  T70_DPMAIF_RESOURCE_MAP Map;
  T70_DPMAIF_RESOURCE Before;

  memset(&Map, 0xa5, sizeof(Map));
  CHECK(T70DpmaifMapResources(Resources, 6, &Map) ==
        T70DpmaifResourceSuccess);
  CHECK(Map.AoUlBase == 0x10014000 && Map.PdUlBase == 0x1022D000 &&
        Map.PdMdMiscBase == 0x1022C000 && Map.PdSramBase == 0x1022E000 &&
        Map.CcciHandoffBase == 0x43F7F180 && Map.CcciHandoffLength == 0x40 &&
        Map.Interrupt == 273);

  Before = Resources[4];
  Resources[4].Shared = 1;
  T70_DPMAIF_RESOURCE_MAP Original = Map;
  CHECK(T70DpmaifMapResources(Resources, 6, &Map) ==
        T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map, &Original, sizeof(Map)) == 0);
  Resources[4] = Before;
}

static void TestRejectsAmbiguousSets(void)
{
  T70_DPMAIF_RESOURCE_MAP Map, Before;
  T70_DPMAIF_RESOURCE Changed[6];

  memset(&Map, 0x6d, sizeof(Map));
  Before = Map;
  CHECK(T70DpmaifMapResources(NULL, 5, &Map) ==
        T70DpmaifResourceInvalid);
  CHECK(memcmp(&Map, &Before, sizeof(Map)) == 0);
  CHECK(T70DpmaifMapResources(Resources, 5, &Map) ==
        T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map, &Before, sizeof(Map)) == 0);

  memcpy(Changed, Resources, sizeof(Changed));
  Changed[1] = Changed[0];
  CHECK(T70DpmaifMapResources(Changed, 6, &Map) ==
        T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map, &Before, sizeof(Map)) == 0);

  memcpy(Changed, Resources, sizeof(Changed));
  Changed[0].Length--;
  CHECK(T70DpmaifMapResources(Changed, 6, &Map) ==
        T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map, &Before, sizeof(Map)) == 0);

  memcpy(Changed, Resources, sizeof(Changed));
  Changed[4].LevelTriggered = 0;
  CHECK(T70DpmaifMapResources(Changed, 6, &Map) ==
        T70DpmaifResourceMismatch);
  memcpy(Changed, Resources, sizeof(Changed));
  Changed[4].ActiveHigh = 0;
  CHECK(T70DpmaifMapResources(Changed, 6, &Map) ==
        T70DpmaifResourceMismatch);
  memcpy(Changed, Resources, sizeof(Changed));
  Changed[4].Start = 274;
  CHECK(T70DpmaifMapResources(Changed, 6, &Map) ==
        T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map, &Before, sizeof(Map)) == 0);
  memcpy(Changed, Resources, sizeof(Changed));
  Changed[5].Length=0x20U;
  CHECK(T70DpmaifMapResources(Changed, 6, &Map)==T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map,&Before,sizeof(Map))==0);
  memcpy(Changed, Resources, sizeof(Changed));
  Changed[5].Start=0x43f7f140U;
  CHECK(T70DpmaifMapResources(Changed, 6, &Map)==T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map,&Before,sizeof(Map))==0);
  memcpy(Changed, Resources, sizeof(Changed));
  Changed[5].Shared=1U;
  CHECK(T70DpmaifMapResources(Changed, 6, &Map)==T70DpmaifResourceMismatch);
  CHECK(memcmp(&Map,&Before,sizeof(Map))==0);
}

int main(void)
{
  TestExactResourceMap();
  TestRejectsAmbiguousSets();
  if (Failure) {
    fprintf(stderr, "DpmaifResourcesTest failed at line %u after %u checks\n",
            Failure, Checks);
    return (int)Failure;
  }
  printf("T70 DPMAIF ACPI resource map: %u resource and failure checks passed\n",
         Checks);
  return 0;
}
