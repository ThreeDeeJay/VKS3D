#pragma once
/* tes_inject.h — Pass-through TCS and stereo TES SPIR-V builders */
#include <stdint.h>
#include <stdbool.h>

/* Build a pass-through TCS (3 control points, tessLevel = 1).
 * Caller must free(*out). */
bool build_tcs_spv(uint32_t **out, size_t *out_c);

/* Build a base TES (triangles/equal/ccw, barycentric interpolation, no
 * stereo).  Run through spirv_patch_stereo_vertex to add gl_ViewIndex
 * offset.  Caller must free(*out). */
bool build_base_tes_spv(uint32_t **out, size_t *out_c);