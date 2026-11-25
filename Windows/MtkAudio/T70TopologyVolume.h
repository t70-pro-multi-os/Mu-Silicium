// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_TOPOLOGY_VOLUME_H
#define T70_TOPOLOGY_VOLUME_H
#include "T70TopologyMiniport.h"
extern "C" {
#include "AwKsVolume.h"
}
/* Optional, explicit binding, copied at creation. Parent must retain the actual
 * resource owner, immutable resident ACF and fixed active profile until final
 * miniport Release. Map is Windows channel -> verified board channel permutation.
 * Actual target identity, remote-removal exclusion and external serialization
 * against all other register/profile users remain provider responsibilities.
 * The miniport gate serializes its own property calls and Close only.
 * This exposes finite attenuation, not mute or amplifier/power initialization. */
struct T70_TOPOLOGY_VOLUME_BINDING {
 T70_AW_SPB_RESOURCES *Owner;
 const void *Acf;size_t Bytes;uint16_t Profile;uint8_t Map[2];
};
#endif
