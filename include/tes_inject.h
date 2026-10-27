#pragma once
#include <stdint.h>
#include <stdbool.h>
bool build_tcs_spv(const uint32_t *vs_spv, size_t vs_wc,
uint32_t **out, size_t *out_c);
bool build_base_tes_spv(const uint32_t *vs_spv, size_t vs_wc,
uint32_t **out, size_t *out_c);
bool build_passthrough_gs_spv(const uint32_t *vs_spv, size_t vs_wc,
uint32_t **out, size_t *out_c);
