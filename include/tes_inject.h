#pragma once
/* tes_inject.h — Pass-through TCS and stereo TES SPIR-V builders */
#include <stdint.h>
#include <stdbool.h>

/* Build a pass-through TCS (3 control points, tessLevel = 1).
 * vs_spv / vs_wc: cached VS SPIR-V used to scan the VS output block layout
 * so that the TCS gl_in struct exactly matches.  May be NULL/0 (falls back
 * to a 1-member block with only gl_Position).
 * Caller must free(*out). */
bool build_tcs_spv(const uint32_t *vs_spv, size_t vs_wc,
                   uint32_t **out, size_t *out_c);

/* Build a base TES (triangles/equal/ccw, barycentric interpolation, no
 * stereo).  Run through spirv_patch_stereo_vertex to add gl_ViewIndex
 * offset.  Caller must free(*out). */
bool build_base_tes_spv(uint32_t **out, size_t *out_c);