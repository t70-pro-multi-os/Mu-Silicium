/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_FRAMEBUFFER_PRELOAD_H
#define T70_FRAMEBUFFER_PRELOAD_H

#include <stdint.h>

struct nir_shader;

struct nir_shader *t70_shader_build_framebuffer_preload_nir(uint32_t format,
                                                            uint32_t array_layers);

#endif
