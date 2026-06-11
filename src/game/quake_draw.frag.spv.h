#pragma once
// Precompiled from quake_draw.frag via merian's scripts/compile_shader.py.

#include "stdint.h"

#if __cplusplus
extern "C" {
#endif

const uint32_t* merian_quake_draw_frag_spv(void);

uint32_t merian_quake_draw_frag_spv_size(void);

#if __cplusplus
}
#endif
