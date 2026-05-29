#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Build pass-through TCS (3 control points, tessLevel=1, full user-varying passthrough).
 * vs_spv/vs_wc: VS SPIR-V to scan for block layout and user-defined Output varyings. */
bool build_tcs_spv(const uint32_t *vs_spv, size_t vs_wc,
                   uint32_t **out, size_t *out_c);

/* Build base TES (triangles/equal/ccw, barycentric interpolation of Position and all
 * user-defined varyings from VS). Pass through spirv_patch_stereo_vertex afterwards.
 * vs_spv/vs_wc: VS SPIR-V to scan for user-defined Output varyings. */
bool build_base_tes_spv(const uint32_t *vs_spv, size_t vs_wc,
                        uint32_t **out, size_t *out_c);