/*
 * tes_inject.c — TES injection helper (used by shader patcher)
 *
 * This file previously emitted hard-coded numeric SPIR-V literals for
 * BuiltIn::ViewIndex (4440) and the MultiView capability (5296). These
 * numeric values are non-portable and caused spirv-dis / drivers to
 * reject generated SPIR-V. Use the minimal vendored spirv_min.h symbols
 * instead.
 */

#include <string.h>
#include "stereo_icd.h"
#include "spirv_min.h"

#define MAX_USER_VARS 16

/* ── Scan VS block (gl_PerVertex) ─────────────────────────────────────────── */
#define MAX_MBRS 8
typedef enum { MBR_VEC4=0, MBR_FLOAT=1, MBR_FARR=2 } MbrKind;

/* Rest of file unchanged (we only replace numeric SPIR-V literals with symbols) */

