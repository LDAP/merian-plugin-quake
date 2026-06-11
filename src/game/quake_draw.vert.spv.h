#pragma once
// Precompiled from quake_draw.vert via merian's scripts/compile_shader.py.

#include "stdint.h"

#if __cplusplus
extern "C" {
#endif

const uint32_t* merian_quake_draw_vert_spv(void);

uint32_t merian_quake_draw_vert_spv_size(void);

#if __cplusplus
}
#endif
